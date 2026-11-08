// Offline check of Ray Reconstruction's path (src/ray_reconstruction.cpp) on this PC, before it runs in the game.
// - A D3D12 device on the high-performance adapter with the debug layer: validation messages are printed, and an
//   error fails the test.
// - NGX initialised the way the game initialises it (NVIDIA's static library). First with only the game's DLSS folder
//   on its search path, as Unreal's DLSS plugin passes it: reports whether Ray Reconstruction is found that way. Then
//   with the game's Streamline folder too, which holds the game's nvngx_dlssd.dll, for the rest of the test.
// - The game's super resolution feature created with the game's flags in one parameter block, which then carries each
//   frame's inputs as the game's does: colour and output R11G11B10_FLOAT, motion vectors R16G16_FLOAT, depth
//   R32G8X24_TYPELESS (the game's formats), 1920x1080 -> 3840x2160, and a G-buffer of known values: a dielectric
//   left half, a tilted metal right half, unlit rows at the top.
// - Frames 1-4 show one guide each over the output (the development view); the output is read back and compared with
//   the guides computed here from the same quantised G-buffer (frame 1 with the G-buffer still a render target, so the
//   pass's own barriers are exercised). A frame without a base pass must fall back to the game's call. Then frames of
//   plain Ray Reconstruction must give finite output close to the flat input colour.
// usage: rr_test.exe <the game's Win64 folder> <scratch folder for NGX's logs>
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "ray_reconstruction.hpp"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr UINT kW = 1920, kH = 1080, kOutW = 3840, kOutH = 2160;
constexpr UINT kUnlitRows = 64;
constexpr float kProjection[2] = {1.0f, 1.7778f};  // ViewToClip[0][0], [1][1]
int g_failures = 0;

void Check(bool ok, const std::string& what) {
  printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
  if (!ok) ++g_failures;
}

void OnRrLog(int level, const char* text) {
  printf("  [rr %s] %s\n", level == 0 ? "info" : level == 1 ? "warning" : "error", text);
}

void NVSDK_CONV OnNgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  std::string line = message;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (line.find("rror") != std::string::npos || line.find("arning") != std::string::npos)
    printf("  [ngx] %s\n", line.c_str());
}

struct Gpu {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12InfoQueue> info;
  UINT64 value = 0;
  HANDLE event = nullptr;
  int errors = 0;

  void Submit() {
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), ++value);
    fence->SetEventOnCompletion(value, event);
    WaitForSingleObject(event, INFINITE);
    allocator->Reset();
    list->Reset(allocator.Get(), nullptr);
    Messages();
  }

  // Prints the debug layer's warnings and errors since the last call.
  void Messages() {
    if (info == nullptr) return;
    const UINT64 n = info->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
      SIZE_T size = 0;
      info->GetMessage(i, nullptr, &size);
      std::vector<char> buffer(size);
      auto* m = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
      if (FAILED(info->GetMessage(i, m, &size))) continue;
      if (m->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
      const bool error = m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
      printf("  [d3d12 %s] %s\n", error ? "error" : "warning", m->pDescription);
      if (error) ++errors;
    }
    info->ClearStoredMessages();
  }
};

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  list->ResourceBarrier(1, &b);
}

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                               D3D12_RESOURCE_STATES state, const wchar_t* name,
                               const D3D12_CLEAR_VALUE* clear = nullptr) {
  const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = w;
  desc.Height = h;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> r;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&r)))) {
    printf("could not create %ls\n", name);
    exit(2);
  }
  r->SetName(name);
  return r;
}

ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type) {
  const D3D12_HEAP_PROPERTIES heap = {type};
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> r;
  const D3D12_RESOURCE_STATES state =
      type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
  device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
  return r;
}

// Fills a 32-bit-per-texel texture (created in COPY_DEST) and leaves it in `after`.
void Upload(Gpu& g, ID3D12Resource* texture, const std::vector<uint32_t>& texels, D3D12_RESOURCE_STATES after) {
  const D3D12_RESOURCE_DESC desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows;
  UINT64 row_size, total;
  g.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &total);
  ComPtr<ID3D12Resource> upload = Buffer(g.device.Get(), total, D3D12_HEAP_TYPE_UPLOAD);
  uint8_t* mapped = nullptr;
  upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  for (UINT y = 0; y < rows; ++y)
    memcpy(mapped + fp.Offset + y * fp.Footprint.RowPitch, texels.data() + size_t(y) * desc.Width, desc.Width * 4);
  upload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION dst = {texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION src = {upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  src.PlacedFootprint = fp;
  g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Transition(g.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_DEST, after);
  g.Submit();  // keeps the upload buffer alive until the copy ran
}

// Reads a 32-bit-per-texel texture back (its state before and after: `state`).
std::vector<uint32_t> ReadBack(Gpu& g, ID3D12Resource* texture, D3D12_RESOURCE_STATES state) {
  const D3D12_RESOURCE_DESC desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows;
  UINT64 row_size, total;
  g.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &total);
  ComPtr<ID3D12Resource> readback = Buffer(g.device.Get(), total, D3D12_HEAP_TYPE_READBACK);
  Transition(g.list.Get(), texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src = {texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  src.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION dst = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = fp;
  g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Transition(g.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
  g.Submit();
  std::vector<uint32_t> texels(size_t(desc.Width) * desc.Height);
  uint8_t* mapped = nullptr;
  readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  for (UINT y = 0; y < rows; ++y)
    memcpy(texels.data() + size_t(y) * desc.Width, mapped + fp.Offset + y * fp.Footprint.RowPitch, desc.Width * 4);
  readback->Unmap(0, nullptr);
  return texels;
}

// R11G11B10_FLOAT.
float SmallFloat(uint32_t bits, int mantissa_bits) {
  const uint32_t exponent = bits >> mantissa_bits, mantissa = bits & ((1u << mantissa_bits) - 1);
  const float m = float(mantissa) / float(1u << mantissa_bits);
  if (exponent == 0) return std::ldexp(m, -14);
  if (exponent == 31) return mantissa != 0 ? NAN : INFINITY;
  return std::ldexp(1.0f + m, int(exponent) - 15);
}
void Unpack11_11_10(uint32_t v, float out[3]) {
  out[0] = SmallFloat(v & 0x7FF, 6);
  out[1] = SmallFloat((v >> 11) & 0x7FF, 6);
  out[2] = SmallFloat((v >> 22) & 0x3FF, 5);
}
uint32_t Pack11_11_10Half() { return (14u << 6) | ((14u << 6) << 11) | ((14u << 5) << 22); }  // 0.5, 0.5, 0.5

float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float LinearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
uint32_t Byte(float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
uint32_t Ten(float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 1023.0f)); }

// The test G-buffer: unlit rows at the top, a dielectric left half facing the camera, a tilted metal right half.
struct Material {
  bool lit;
  float normal[3];
  uint32_t specular2;  // GBufferA's 2-bit alpha
  float metallic, roughness, base[3];
};
Material MaterialAt(UINT x, UINT y) {
  if (y < kUnlitRows) return {false, {0, 0, 1}, 0, 0, 0, {0, 0, 0}};
  if (x < kW / 2) return {true, {0.0f, 0.0f, -1.0f}, 1, 0.0f, 0.5f, {0.5f, 0.25f, 0.125f}};
  return {true, {0.3f, 0.0f, -0.9539f}, 0, 1.0f, 0.2f, {0.9f, 0.6f, 0.3f}};
}
uint32_t GBufferA(const Material& m) {
  return Ten(m.normal[0] * 0.5f + 0.5f) | Ten(m.normal[1] * 0.5f + 0.5f) << 10 | Ten(m.normal[2] * 0.5f + 0.5f) << 20 |
         m.specular2 << 30;
}
uint32_t GBufferB(const Material& m) {  // B8G8R8A8_UNORM: memory B, G, R, A = roughness, -, metallic, shading model
  return Byte(m.roughness) | 0u << 8 | Byte(m.metallic) << 16 | (m.lit ? 1u : 0u) << 24;
}
uint32_t GBufferC(const Material& m) {  // B8G8R8A8_UNORM_SRGB
  return Byte(LinearToSrgb(m.base[2])) | Byte(LinearToSrgb(m.base[1])) << 8 | Byte(LinearToSrgb(m.base[0])) << 16 |
         255u << 24;
}

// rr_guides.hlsl's EnvBRDFApprox2, in C++ (HLSL's float2x2(a, b, c, d) has rows (a, b) and (c, d); mul(M, v) = M v).
void EnvBRDFApprox2(const float f0[3], float alpha, float NoV, float out[3]) {
  NoV = std::fabs(NoV);
  const float X[4] = {1.0f, NoV, NoV * NoV, NoV * NoV * NoV};
  const float Y[4] = {1.0f, alpha, alpha * alpha, alpha * alpha * alpha};
  const float m1[2] = {0.99044f * X[0] + -1.28514f * X[1], 1.29678f * X[0] + -0.755907f * X[1]};
  const float m2[3] = {1.0f * X[0] + 2.92338f * X[1] + 59.4188f * X[3],
                       20.3225f * X[0] + -27.0302f * X[1] + 222.592f * X[3],
                       121.563f * X[0] + 626.13f * X[1] + 316.627f * X[3]};
  const float m3[2] = {0.0365463f * X[0] + 3.32707f * X[1], 9.0632f * X[0] + -9.04756f * X[1]};
  const float m4[3] = {1.0f * X[0] + 3.59685f * X[2] + -1.36772f * X[3],
                       9.04401f * X[0] + -16.3174f * X[2] + 9.22949f * X[3],
                       5.56589f * X[0] + 19.7886f * X[2] + -20.2123f * X[3]};
  float bias = (m1[0] * Y[0] + m1[1] * Y[1]) / (m2[0] * Y[0] + m2[1] * Y[1] + m2[2] * Y[3]);
  const float scale = (m3[0] * Y[0] + m3[1] * Y[1]) / (m4[0] * Y[0] + m4[1] * Y[1] + m4[2] * Y[3]);
  bias *= std::clamp(f0[1] * 50.0f, 0.0f, 1.0f);
  for (int i = 0; i < 3; ++i) out[i] = f0[i] * std::max(0.0f, scale) + std::max(0.0f, bias);
}

// What a guide view shows at render pixel (x, y), from the quantised G-buffer (as the shader decodes it).
void Expected(int guide, UINT x, UINT y, float out[3]) {
  const Material m = MaterialAt(x, y);
  if (!m.lit) {
    const float unlit[5][3] = {{0, 0, 0}, {0.5f, 0.5f, 0.5f}, {0, 0, 0}, {0.5f, 0.5f, 0.5f}, {0, 0, 0}};
    for (int i = 0; i < 3; ++i) out[i] = unlit[guide][i];
    return;
  }
  const uint32_t a = GBufferA(m), b = GBufferB(m), c = GBufferC(m);
  float n[3] = {float(a & 1023) / 1023.0f * 2 - 1, float((a >> 10) & 1023) / 1023.0f * 2 - 1,
                float((a >> 20) & 1023) / 1023.0f * 2 - 1};
  const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  for (float& v : n) v /= len;
  const float s = float(a >> 30) / 3.0f;
  const float metallic = float((b >> 16) & 255) / 255.0f, roughness = float(b & 255) / 255.0f;
  const float base[3] = {SrgbToLinear(float((c >> 16) & 255) / 255.0f), SrgbToLinear(float((c >> 8) & 255) / 255.0f),
                         SrgbToLinear(float(c & 255) / 255.0f)};
  const float dielectric = std::clamp(s < 2.0f / 3.0f ? s * 0.06f : s * 0.36f - 0.20f, 0.0f, 0.16f);
  float f0[3];
  for (int i = 0; i < 3; ++i) f0[i] = dielectric + metallic * (base[i] - dielectric);
  const float ndc[2] = {(x + 0.5f) / kW * 2 - 1, 1 - (y + 0.5f) / kH * 2};
  float ray[3] = {ndc[0] / kProjection[0], ndc[1] / kProjection[1], 1.0f};
  const float rl = std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + 1.0f);
  for (float& v : ray) v /= rl;
  const float NoV = -(n[0] * ray[0] + n[1] * ray[1] + n[2] * ray[2]);  // the identity world-to-view of this test
  switch (guide) {
    case 1:
      for (int i = 0; i < 3; ++i) out[i] = base[i] * (1 - metallic);
      break;
    case 2:
      EnvBRDFApprox2(f0, roughness * roughness, NoV, out);
      break;
    case 3:
      for (int i = 0; i < 3; ++i) out[i] = n[i] * 0.5f + 0.5f;
      break;
    default:
      for (int i = 0; i < 3; ++i) out[i] = roughness;
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    printf("usage: rr_test.exe <the game's Win64 folder> <scratch folder>\n");
    return 2;
  }
  const fs::path game = fs::weakly_canonical(fs::path(argv[1]) / ".." / ".." / "..");
  const fs::path scratch = argv[2];
  fs::create_directories(scratch);
  const fs::path dlss_dir = game / "Engine" / "Plugins" / "DLSSSubset" / "Binaries" / "ThirdParty" / "Win64";
  const fs::path streamline_dir = game / "Engine" / "Plugins" / "StreamlineSubset" / "Binaries" / "ThirdParty" / "Win64";
  Check(fs::exists(dlss_dir / "nvngx_dlss.dll"), "the game's DLSS folder has nvngx_dlss.dll");
  Check(fs::exists(streamline_dir / "nvngx_dlssd.dll"), "the game's Streamline folder has nvngx_dlssd.dll");

  ComPtr<ID3D12Debug> debug;
  const bool debug_layer = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
  if (debug_layer) debug->EnableDebugLayer();
  printf("D3D12 debug layer: %s\n", debug_layer ? "on" : "not available");
  ComPtr<IDXGIFactory6> factory;
  ComPtr<IDXGIAdapter1> adapter;
  CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  DXGI_ADAPTER_DESC1 ad = {};
  adapter->GetDesc1(&ad);
  printf("adapter: %ls\n", ad.Description);
  Gpu g;
  if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.device)))) {
    printf("no D3D12 device\n");
    return 2;
  }
  g.device.As(&g.info);
  const D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
  g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
  g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator));
  g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator.Get(), nullptr, IID_PPV_ARGS(&g.list));
  g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
  g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  // NGX as the game initialises it.
  const std::wstring dlss_path = dlss_dir.wstring(), streamline_path = streamline_dir.wstring();
  const wchar_t* paths[] = {dlss_path.c_str(), streamline_path.c_str()};
  auto init = [&](unsigned count) {
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = count;
    info.LoggingInfo.LoggingCallback = OnNgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    return NVSDK_NGX_D3D12_Init_with_ProjectID("7d3c1e5a-2b4f-4c8e-9a61-f0e2d4b8c917", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                               "1.0", scratch.c_str(), g.device.Get(), &info);
  };
  auto rr_available = [] {
    NVSDK_NGX_Parameter* caps = nullptr;
    int available = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)) && caps != nullptr) {
      NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
      NVSDK_NGX_D3D12_DestroyParameters(caps);
    }
    return available != 0;
  };
  NVSDK_NGX_Result result = init(1);
  if (NVSDK_NGX_SUCCEED(result)) {
    printf("Ray Reconstruction with only the game's DLSS folder on NGX's search path: %s\n",
           rr_available() ? "AVAILABLE" : "not available (the game would need nvngx_dlssd.dll there)");
    NVSDK_NGX_D3D12_Shutdown1(g.device.Get());
  }
  // --dlss-folder-only: the rest with only the game's DLSS folder too, as the game's own NGX has it, so Ray
  // Reconstruction comes from wherever NGX finds it then (NGX's log names the DLL and the preset it picks).
  const bool dlss_folder_only = argc > 3 && std::wstring(argv[3]) == L"--dlss-folder-only";
  result = init(dlss_folder_only ? 1 : 2);
  Check(NVSDK_NGX_SUCCEED(result), dlss_folder_only ? "NGX initialised with only the game's DLSS folder"
                                                    : "NGX initialised with the game's DLSS and Streamline folders");
  Check(rr_available(), "Ray Reconstruction available");
  if (g_failures != 0) return 1;

  // The game's DLSS inputs.
  ComPtr<ID3D12Resource> color = Texture(g.device.Get(), kW, kH, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                                         D3D12_RESOURCE_STATE_COPY_DEST, L"test colour");
  ComPtr<ID3D12Resource> output =
      Texture(g.device.Get(), kOutW, kOutH, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"test output");
  constexpr D3D12_RESOURCE_STATES kRead =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  ComPtr<ID3D12Resource> motion = Texture(g.device.Get(), kW, kH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                                          kRead, L"test motion vectors");  // committed memory starts zeroed
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  clear.DepthStencil = {0.5f, 0};
  ComPtr<ID3D12Resource> depth =
      Texture(g.device.Get(), kW, kH, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
              D3D12_RESOURCE_STATE_DEPTH_WRITE, L"test depth", &clear);
  ComPtr<ID3D12DescriptorHeap> dsv_heap;
  const D3D12_DESCRIPTOR_HEAP_DESC dhd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  g.device->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&dsv_heap));
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
  dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  g.device->CreateDepthStencilView(depth.Get(), &dsv, dsv_heap->GetCPUDescriptorHandleForHeapStart());
  g.list->ClearDepthStencilView(dsv_heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0,
                                nullptr);
  Transition(g.list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, kRead);
  g.Submit();
  Upload(g, color.Get(), std::vector<uint32_t>(size_t(kW) * kH, Pack11_11_10Half()), kRead);

  // The G-buffer, left as render targets like after the base pass.
  const DXGI_FORMAT formats[3] = {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
                                  DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
  ComPtr<ID3D12Resource> gbuffer[3];
  std::vector<uint32_t> texels[3];
  for (auto& t : texels) t.resize(size_t(kW) * kH);
  for (UINT y = 0; y < kH; ++y)
    for (UINT x = 0; x < kW; ++x) {
      const Material m = MaterialAt(x, y);
      texels[0][size_t(y) * kW + x] = GBufferA(m);
      texels[1][size_t(y) * kW + x] = GBufferB(m);
      texels[2][size_t(y) * kW + x] = GBufferC(m);
    }
  static const wchar_t* const kNames[3] = {L"test GBufferA", L"test GBufferB", L"test GBufferC"};
  for (int i = 0; i < 3; ++i) {
    gbuffer[i] = Texture(g.device.Get(), kW, kH, formats[i], D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                         D3D12_RESOURCE_STATE_COPY_DEST, kNames[i]);
    Upload(g, gbuffer[i].Get(), texels[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
  }

  // The game's super resolution feature, in the parameter block that then carries every evaluation's inputs.
  NVSDK_NGX_Parameter* params = nullptr;
  NVSDK_NGX_D3D12_AllocateParameters(&params);
  NVSDK_NGX_DLSS_Create_Params create = {};
  create.Feature.InWidth = kW;
  create.Feature.InHeight = kH;
  create.Feature.InTargetWidth = kOutW;
  create.Feature.InTargetHeight = kOutH;
  create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxPerf;
  create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
  NVSDK_NGX_Handle* super_resolution = nullptr;
  result = NGX_D3D12_CREATE_DLSS_EXT(g.list.Get(), 1, 1, &super_resolution, params, &create);
  g.Submit();
  Check(NVSDK_NGX_SUCCEED(result), "the game's super resolution feature created");
  npcmotion::rr::OnGameCreate();
  NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_Color, color.Get());
  NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_Output, output.Get());
  NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_Depth, depth.Get());
  NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_MotionVectors, motion.Get());
  NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_MV_Scale_X, float(kW));
  NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_MV_Scale_Y, float(kH));
  NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_Reset, 0);
  NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, kW);
  NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, kH);
  NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
  NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
  NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 16.7f);

  npcmotion::rr::Matrices matrices = {};
  for (int i = 0; i < 4; ++i) matrices.translated_world_to_view[i * 5] = 1.0f;
  matrices.view_to_clip[0] = kProjection[0];
  matrices.view_to_clip[5] = kProjection[1];
  matrices.view_to_clip[11] = 1.0f;   // reverse-Z perspective, row vectors
  matrices.view_to_clip[14] = 10.0f;  // the near plane
  npcmotion::rr::SetLogger(OnRrLog);
  npcmotion::rr::SetWanted(true);

  // One frame: the base pass's binding (and, unless `stay_target`, its barrier to shader resources), the jitter, the
  // evaluation the DLSS hook would hand over.
  bool readable = false;  // the G-buffer's state: shader resource or render target
  auto frame = [&](int n, bool base_pass, bool stay_target, int guide) {
    if (base_pass) {
      if (readable)
        for (auto& r : gbuffer) Transition(g.list.Get(), r.Get(), kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
      readable = false;
      npcmotion::rr::OnGBuffer(gbuffer[0].Get(), gbuffer[1].Get(), gbuffer[2].Get(), formats[0], formats[1],
                               formats[2]);
      if (!stay_target) {
        for (auto& r : gbuffer) {
          Transition(g.list.Get(), r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
          npcmotion::rr::OnBarrier(r.Get(), kRead);
        }
        readable = true;
      }
    }
    NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_Jitter_Offset_X, 0.25f * float((n % 4) - 1.5f));
    NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.2f * float((n % 5) - 2));
    npcmotion::rr::SetGuideView(guide);
    const bool ran = npcmotion::rr::Evaluate(g.list.Get(), params, 1.0f, &matrices);
    g.Submit();
    return ran;
  };

  static const char* const kGuides[5] = {"", "diffuse albedo", "specular albedo", "normals", "roughness"};
  const UINT probes[3][2] = {{200, 500}, {1500, 500}, {200, 20}};  // dielectric, metal, unlit (render pixels)
  static const char* const kProbeNames[3] = {"dielectric", "metal", "unlit"};
  for (int guide = 1; guide <= 4; ++guide) {
    const bool ran = frame(guide, true, guide == 1, guide);
    Check(ran, std::string("frame ") + std::to_string(guide) + ": Ray Reconstruction ran (" + kGuides[guide] + " shown" +
                   (guide == 1 ? ", G-buffer still a render target)" : ")"));
    const std::vector<uint32_t> out = ReadBack(g, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (int p = 0; p < 3; ++p) {
      float got[3], want[3];
      Unpack11_11_10(out[size_t(2 * probes[p][1] + 1) * kOutW + 2 * probes[p][0] + 1], got);
      Expected(guide, probes[p][0], probes[p][1], want);
      bool match = true;
      for (int i = 0; i < 3; ++i) match = match && std::fabs(got[i] - want[i]) <= 0.01f + 0.04f * std::fabs(want[i]);
      char line[200];
      snprintf(line, sizeof(line), "  %s %s: got (%.4f, %.4f, %.4f), expected (%.4f, %.4f, %.4f)", kGuides[guide],
               kProbeNames[p], got[0], got[1], got[2], want[0], want[1], want[2]);
      Check(match, line);
    }
  }
  Check(!frame(5, false, false, 0), "a frame without a base pass is left to the game's call");
  for (int n = 6; n <= 12; ++n) frame(n, true, false, 0);
  const std::vector<uint32_t> out = ReadBack(g, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  double sum = 0, lo = 1e9, hi = -1e9;
  size_t bad = 0;
  for (size_t i = 0; i < out.size(); i += 97) {
    float c[3];
    Unpack11_11_10(out[i], c);
    for (float v : c) {
      if (!std::isfinite(v)) ++bad;
      sum += v;
      lo = std::min(lo, double(v));
      hi = std::max(hi, double(v));
    }
  }
  const double mean = sum / (3.0 * double((out.size() + 96) / 97));
  char line[160];
  snprintf(line, sizeof(line), "plain Ray Reconstruction of a flat 0.5 input: mean %.4f, min %.4f, max %.4f, %zu not finite",
           mean, lo, hi, bad);
  Check(bad == 0 && mean > 0.4 && mean < 0.6 && lo > 0.2 && hi < 0.9, line);
  printf("Ray Reconstruction status: %s\n", npcmotion::rr::Status().c_str());

  npcmotion::rr::Shutdown();
  NVSDK_NGX_D3D12_ReleaseFeature(super_resolution);
  NVSDK_NGX_D3D12_DestroyParameters(params);
  NVSDK_NGX_D3D12_Shutdown1(g.device.Get());
  g.Messages();
  if (debug_layer) Check(g.errors == 0, std::to_string(g.errors) + " D3D12 validation errors");
  printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
