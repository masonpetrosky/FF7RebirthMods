// Checks how DLSS super resolution handles a render size larger than the size its feature was created for, the way
// FINAL FANTASY VII REBIRTH creates the feature: for the optimal render size of the quality mode it picks from the
// render scale (>= 0.66 Quality, >= 0.58 Balanced, >= 0.5 Performance), while its render scale entries are 1/1.7
// (58.8%) and 100%. A synthetic still scene (checkerboard, thin lines, rings) is rendered with jitter at the input
// size, DLSS runs for two jitter cycles, and the output is compared with the scene supersampled at the output size.
// A feature that crops or mis-scales the larger input shows up as error growing toward the right and bottom edges.
//
// usage: dlss_subrect_check.exe <folder with nvngx_dlss.dll> [frames]
//
// Not part of the build: it needs NVIDIA's DLSS SDK (github.com/NVIDIA/DLSS), which this repository does not include.
// In a Visual Studio x64 prompt:
//   cl /O2 /EHsc /std:c++20 /MT /I<sdk>\include dlss_subrect_check.cpp /link <sdk lib folder>\nvsdk_ngx_s.lib
//      d3d12.lib dxgi.lib advapi32.lib version.lib user32.lib
// Results (RTX 5070 Ti, DLSS 310.9.1) are in NpcMotion/docs/recon.md, "Dynamic resolution limits".
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr char kProjectId[] = "5b9d6c1e-3f2a-4e8b-a7d4-0c6e2f19b384";
constexpr UINT kOutW = 3840, kOutH = 2160;

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
ID3D12CommandAllocator* g_alloc = nullptr;
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_event = nullptr;
UINT64 g_fence_value = 0;

void Check(HRESULT hr, const char* what) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "%s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
    std::exit(1);
  }
}

void Submit() {
  Check(g_list->Close(), "Close");
  ID3D12CommandList* lists[] = {g_list};
  g_queue->ExecuteCommandLists(1, lists);
  Check(g_queue->Signal(g_fence, ++g_fence_value), "Signal");
  Check(g_fence->SetEventOnCompletion(g_fence_value, g_event), "SetEventOnCompletion");
  WaitForSingleObject(g_event, INFINITE);
  Check(g_alloc->Reset(), "Reset allocator");
  Check(g_list->Reset(g_alloc, nullptr), "Reset list");
}

ID3D12Resource* Texture(UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = w;
  desc.Height = h;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  ID3D12Resource* res = nullptr;
  Check(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res)),
        "CreateCommittedResource");
  return res;
}

ID3D12Resource* Buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = type;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* res = nullptr;
  Check(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res)),
        "buffer");
  return res;
}

void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  g_list->ResourceBarrier(1, &b);
}

uint16_t ToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000;
  const int32_t exp = static_cast<int32_t>((x >> 23) & 0xff) - 127 + 15;
  const uint32_t mant = x & 0x7fffff;
  if (exp <= 0) return static_cast<uint16_t>(sign);
  if (exp >= 31) return static_cast<uint16_t>(sign | 0x7c00);
  uint32_t h = sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
  const uint32_t rem = mant & 0x1fff;
  if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) ++h;
  return static_cast<uint16_t>(h);
}

float FromHalf(uint16_t h) {
  const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
  if (exp == 0) {
    const float f = std::ldexp(static_cast<float>(mant), -24);
    return sign ? -f : f;
  }
  uint32_t x = exp == 31 ? (sign | 0x7f800000u | (mant << 13)) : (sign | ((exp - 15 + 127) << 23) | (mant << 13));
  float f;
  std::memcpy(&f, &x, 4);
  return f;
}

// The still scene in image coordinates (0..1): a 48 x 27 checkerboard (square cells at 16:9), thin dark diagonal
// lines and bright rings around the centre.
float Scene(double u, double v) {
  const int cx = static_cast<int>(std::floor(u * 48)), cy = static_cast<int>(std::floor(v * 27));
  float value = ((cx + cy) & 1) ? 0.80f : 0.20f;
  const double line = u * 96 + v * 54;
  if (line - std::floor(line) < 0.04) value = 0.02f;
  const double r = std::hypot(u - 0.5, (v - 0.5) * 9.0 / 16.0);
  const double ring = r * 40;
  if (ring - std::floor(ring) < 0.06) value = 0.98f;
  return value;
}

template <typename F>
void ParallelRows(UINT rows, F&& f) {
  const unsigned n = std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < n; ++t)
    threads.emplace_back([&, t] {
      for (UINT y = t; y < rows; y += n) f(y);
    });
  for (auto& th : threads) th.join();
}

float Halton(int index, int base) {
  float f = 1, r = 0;
  for (int i = index; i > 0; i /= base) {
    f /= static_cast<float>(base);
    r += f * static_cast<float>(i % base);
  }
  return r;
}

struct Upload {
  ID3D12Resource* buffer = nullptr;
  uint8_t* mapped = nullptr;
};

// Fills `tex` (w x h, `bytes_per_pixel`) from rows written by `fill(y, row_pointer)`, through the upload buffer.
template <typename F>
void FillTexture(Upload& up, ID3D12Resource* tex, UINT w, UINT h, D3D12_RESOURCE_STATES state, F&& fill) {
  const D3D12_RESOURCE_DESC desc = tex->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
  UINT64 total = 0;
  g_device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
  ParallelRows(h, [&](UINT y) { fill(y, up.mapped + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch); });
  Barrier(tex, state, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = tex;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = up.buffer;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = fp;
  g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, state);
  (void)w;
}

struct Optimal {
  unsigned w = 0, h = 0, max_w = 0, max_h = 0, min_w = 0, min_h = 0;
  float sharpness = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
};

Optimal GetOptimal(NVSDK_NGX_Parameter* params, unsigned out_w, unsigned out_h, NVSDK_NGX_PerfQuality_Value q) {
  Optimal o;
  o.result = NGX_DLSS_GET_OPTIMAL_SETTINGS(params, out_w, out_h, q, &o.w, &o.h, &o.max_w, &o.max_h, &o.min_w, &o.min_h,
                                           &o.sharpness);
  return o;
}

struct Case {
  const char* name;
  NVSDK_NGX_PerfQuality_Value quality;
  UINT create_w, create_h;  // the size the feature is created for
  UINT render_w, render_h;  // the frames' render size (= the input textures' size, as in the game)
};

struct Result {
  NVSDK_NGX_Result create = NVSDK_NGX_Result_Fail, evaluate = NVSDK_NGX_Result_Success;
  double mae = 0, right = 0, bottom = 0, worst_block = 0;
};

std::vector<float> g_truth;  // the scene supersampled 4 x 4 at the output size

Result Run(NVSDK_NGX_Parameter* params, Upload& up, ID3D12Resource* output, ID3D12Resource* readback,
           const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& out_fp, const Case& c, int preset, int frames, float jitter_sign) {
  Result res;
  for (const char* hint :
       {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
    NVSDK_NGX_Parameter_SetUI(params, hint, static_cast<unsigned>(preset));
  NVSDK_NGX_DLSS_Create_Params create = {};
  create.Feature.InWidth = c.create_w;
  create.Feature.InHeight = c.create_h;
  create.Feature.InTargetWidth = kOutW;
  create.Feature.InTargetHeight = kOutH;
  create.Feature.InPerfQualityValue = c.quality;
  // The game's flags (0x4B): IsHDR | MVLowRes | DepthInverted | AutoExposure.
  create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
  create.InEnableOutputSubrects = true;
  NVSDK_NGX_Handle* feature = nullptr;
  res.create = NGX_D3D12_CREATE_DLSS_EXT(g_list, 1, 1, &feature, params, &create);
  Submit();
  if (NVSDK_NGX_FAILED(res.create)) return res;
  // A failed evaluation leaves the output alone: start from black so it cannot pass for the last run's image.
  FillTexture(up, output, kOutW, kOutH, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              [&](UINT, uint8_t* row) { std::memset(row, 0, 8 * kOutW); });
  Submit();

  const auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  ID3D12Resource* color = Texture(c.render_w, c.render_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, srv);
  ID3D12Resource* depth = Texture(c.render_w, c.render_h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, srv);
  ID3D12Resource* motion = Texture(c.render_w, c.render_h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, srv);
  FillTexture(up, depth, c.render_w, c.render_h, srv, [&](UINT, uint8_t* row) {
    const float d = 0.5f;
    for (UINT x = 0; x < c.render_w; ++x) std::memcpy(row + 4 * x, &d, 4);
  });
  Submit();
  FillTexture(up, motion, c.render_w, c.render_h, srv, [&](UINT, uint8_t* row) { std::memset(row, 0, 4 * c.render_w); });
  Submit();

  for (int i = 0; i < frames; ++i) {
    const float jx = Halton(i % 32 + 1, 2) - 0.5f, jy = Halton(i % 32 + 1, 3) - 0.5f;
    FillTexture(up, color, c.render_w, c.render_h, srv, [&](UINT y, uint8_t* row) {
      auto* px = reinterpret_cast<uint16_t*>(row);
      for (UINT x = 0; x < c.render_w; ++x) {
        const double u = (x + 0.5 + jitter_sign * jx) / c.render_w, v = (y + 0.5 + jitter_sign * jy) / c.render_h;
        const uint16_t s = ToHalf(Scene(u, v));
        px[4 * x + 0] = s;
        px[4 * x + 1] = s;
        px[4 * x + 2] = s;
        px[4 * x + 3] = 0x3c00;
      }
    });
    NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
    eval.Feature.pInColor = color;
    eval.Feature.pInOutput = output;
    eval.pInDepth = depth;
    eval.pInMotionVectors = motion;
    eval.InJitterOffsetX = jx;
    eval.InJitterOffsetY = jy;
    eval.InRenderSubrectDimensions = {c.render_w, c.render_h};
    eval.InReset = i == 0 ? 1 : 0;
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;
    eval.InFrameTimeDeltaInMsec = 16.0f;
    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(g_list, feature, params, &eval);
    if (NVSDK_NGX_FAILED(r) && !NVSDK_NGX_FAILED(res.evaluate)) res.evaluate = r;
    Submit();
  }

  Barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = output;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = out_fp;
  g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Barrier(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Submit();
  uint8_t* mapped = nullptr;
  Check(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map readback");
  const UINT bw = 64, bh = 64, bx_n = kOutW / bw, by_n = kOutH / bh;
  std::vector<double> rows(kOutH), right_rows(kOutH), blocks(static_cast<size_t>(bx_n) * by_n, 0.0);
  std::vector<double> block_rows(static_cast<size_t>(kOutH) * bx_n, 0.0);
  ParallelRows(kOutH, [&](UINT y) {
    const auto* px = reinterpret_cast<const uint16_t*>(mapped + out_fp.Offset + static_cast<size_t>(y) * out_fp.Footprint.RowPitch);
    double sum = 0, right = 0;
    for (UINT x = 0; x < kOutW; ++x) {
      const double e = std::fabs(FromHalf(px[4 * x]) - g_truth[static_cast<size_t>(y) * kOutW + x]);
      sum += e;
      if (x >= kOutW * 92 / 100) right += e;
      if (x / bw < bx_n) block_rows[static_cast<size_t>(y) * bx_n + x / bw] += e;
    }
    rows[y] = sum;
    right_rows[y] = right;
  });
  readback->Unmap(0, nullptr);
  double total = 0, right = 0, bottom = 0;
  for (UINT y = 0; y < kOutH; ++y) {
    total += rows[y];
    right += right_rows[y];
    if (y >= kOutH * 92 / 100) bottom += rows[y];
  }
  for (UINT y = 0; y < by_n * bh; ++y)
    for (UINT bx = 0; bx < bx_n; ++bx) blocks[(y / bh) * bx_n + bx] += block_rows[static_cast<size_t>(y) * bx_n + bx];
  res.mae = total / (static_cast<double>(kOutW) * kOutH);
  res.right = right / (static_cast<double>(kOutW - kOutW * 92 / 100) * kOutH);
  res.bottom = bottom / (static_cast<double>(kOutW) * (kOutH - kOutH * 92 / 100));
  for (double b : blocks) res.worst_block = std::max(res.worst_block, b / (bw * bh));
  NVSDK_NGX_D3D12_ReleaseFeature(feature);
  color->Release();
  depth->Release();
  motion->Release();
  return res;
}

const char* QualityName(NVSDK_NGX_PerfQuality_Value q) {
  switch (q) {
    case NVSDK_NGX_PerfQuality_Value_MaxPerf: return "Performance";
    case NVSDK_NGX_PerfQuality_Value_Balanced: return "Balanced";
    case NVSDK_NGX_PerfQuality_Value_MaxQuality: return "Quality";
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return "UltraPerf";
    case NVSDK_NGX_PerfQuality_Value_UltraQuality: return "UltraQuality";
    case NVSDK_NGX_PerfQuality_Value_DLAA: return "DLAA";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: dlss_subrect_check.exe <folder with nvngx_dlss.dll> [frames]\n");
    return 2;
  }
  const std::string dir8 = argv[1];
  const std::wstring dll_dir(dir8.begin(), dir8.end());
  const int frames = argc > 2 ? std::atoi(argv[2]) : 64;

  IDXGIFactory6* factory = nullptr;
  Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  IDXGIAdapter1* adapter = nullptr;
  Check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
        "EnumAdapterByGpuPreference");
  DXGI_ADAPTER_DESC1 ad = {};
  adapter->GetDesc1(&ad);
  std::wprintf(L"GPU: %ls\n", ad.Description);
  Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device)), "D3D12CreateDevice");
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  Check(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)), "CreateCommandQueue");
  Check(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc)), "allocator");
  Check(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_list)),
        "CreateCommandList");
  Check(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)), "CreateFence");
  g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  const wchar_t* paths[] = {dll_dir.c_str()};
  NVSDK_NGX_FeatureCommonInfo info = {};
  info.PathListInfo.Path = paths;
  info.PathListInfo.Length = 1;
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", temp,
                                                           g_device, &info))) {
    std::fprintf(stderr, "NGX init failed\n");
    return 1;
  }
  NVSDK_NGX_Parameter* caps = nullptr;
  NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
  int available = 0;
  NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
  if (!available) {
    std::fprintf(stderr, "DLSS not available\n");
    return 1;
  }
  NVSDK_NGX_Parameter* params = nullptr;
  NVSDK_NGX_D3D12_AllocateParameters(&params);

  // What NGX reports per quality mode: the optimal render size the game creates the feature for, and the range.
  const NVSDK_NGX_PerfQuality_Value modes[] = {
      NVSDK_NGX_PerfQuality_Value_UltraPerformance, NVSDK_NGX_PerfQuality_Value_MaxPerf,
      NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxQuality,
      NVSDK_NGX_PerfQuality_Value_UltraQuality, NVSDK_NGX_PerfQuality_Value_DLAA};
  const unsigned outputs[][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080}};
  for (const auto& o : outputs) {
    std::printf("optimal settings at %ux%u\n", o[0], o[1]);
    for (NVSDK_NGX_PerfQuality_Value q : modes) {
      const Optimal opt = GetOptimal(caps, o[0], o[1], q);
      std::printf("  %-12s result 0x%08x optimal %4ux%-4u (%.4f) dynamic %4ux%-4u .. %4ux%-4u\n", QualityName(q),
                  static_cast<unsigned>(opt.result), opt.w, opt.h, o[0] ? static_cast<double>(opt.w) / o[0] : 0.0,
                  opt.min_w, opt.min_h, opt.max_w, opt.max_h);
    }
  }

  const Optimal bal = GetOptimal(caps, kOutW, kOutH, NVSDK_NGX_PerfQuality_Value_Balanced);
  const Optimal qual = GetOptimal(caps, kOutW, kOutH, NVSDK_NGX_PerfQuality_Value_MaxQuality);
  if (bal.w == 0 || qual.w == 0) {
    std::fprintf(stderr, "no optimal settings\n");
    return 1;
  }
  // Unreal's view rect at a fraction f: ceil(output * f) (FSceneRenderer::PrepareViewRectsForRendering).
  const float game58 = 58.8235283f / 100.0f;
  const UINT g58_w = static_cast<UINT>(std::ceil(kOutW * game58)), g58_h = static_cast<UINT>(std::ceil(kOutH * game58));
  std::printf("game 58 entry: %ux%u, NGX Balanced optimal %ux%u, NGX Quality optimal %ux%u\n", g58_w, g58_h, bal.w,
              bal.h, qual.w, qual.h);

  // Ground truth.
  g_truth.resize(static_cast<size_t>(kOutW) * kOutH);
  ParallelRows(kOutH, [&](UINT y) {
    for (UINT x = 0; x < kOutW; ++x) {
      double s = 0;
      for (int sy = 0; sy < 4; ++sy)
        for (int sx = 0; sx < 4; ++sx) s += Scene((x + (sx + 0.5) / 4) / kOutW, (y + (sy + 0.5) / 4) / kOutH);
      g_truth[static_cast<size_t>(y) * kOutW + x] = static_cast<float>(s / 16);
    }
  });

  Upload up;
  {
    // Large enough for the biggest input (RGBA16F at the output size) with row pitch alignment.
    up.buffer = Buffer(static_cast<UINT64>(kOutW) * 8 * kOutH + 2 * 1024 * 1024, D3D12_HEAP_TYPE_UPLOAD,
                       D3D12_RESOURCE_STATE_GENERIC_READ);
    Check(up.buffer->Map(0, nullptr, reinterpret_cast<void**>(&up.mapped)), "Map upload");
  }
  ID3D12Resource* output = Texture(kOutW, kOutH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const D3D12_RESOURCE_DESC out_desc = output->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT out_fp = {};
  UINT64 out_total = 0;
  g_device->GetCopyableFootprints(&out_desc, 0, 1, 0, &out_fp, nullptr, nullptr, &out_total);
  ID3D12Resource* readback = Buffer(out_total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

  // Jitter sign: the convention with the lower error on a correctly created feature.
  const Case ref = {"ref", NVSDK_NGX_PerfQuality_Value_Balanced, g58_w, g58_h, g58_w, g58_h};
  const Result plus = Run(params, up, output, readback, out_fp, ref, 11, frames, +1.0f);
  const Result minus = Run(params, up, output, readback, out_fp, ref, 11, frames, -1.0f);
  const float sign = plus.mae <= minus.mae ? 1.0f : -1.0f;
  std::printf("jitter sign check (Balanced, K, created for %ux%u): +1 error %.5f, -1 error %.5f -> %+.0f\n", g58_w,
              g58_h, plus.mae, minus.mae, sign);

  const Case cases[] = {
      {"58.8% on a 58.8% feature", NVSDK_NGX_PerfQuality_Value_Balanced, g58_w, g58_h, g58_w, g58_h},
      {"58.8% on Balanced optimal", NVSDK_NGX_PerfQuality_Value_Balanced, bal.w, bal.h, g58_w, g58_h},
      {"Balanced optimal (NVIDIA)", NVSDK_NGX_PerfQuality_Value_Balanced, bal.w, bal.h, bal.w, bal.h},
      {"100% on a 100% Quality feature", NVSDK_NGX_PerfQuality_Value_MaxQuality, kOutW, kOutH, kOutW, kOutH},
      {"100% on Quality optimal (game)", NVSDK_NGX_PerfQuality_Value_MaxQuality, qual.w, qual.h, kOutW, kOutH},
      {"Quality optimal (game 66%)", NVSDK_NGX_PerfQuality_Value_MaxQuality, qual.w, qual.h, qual.w, qual.h},
  };
  const int presets[] = {11, 12, 13};
  const char* preset_names[] = {"", "", "", "", "", "E", "", "", "", "", "J", "K", "L", "M"};
  std::printf("\n%-32s %-6s %-11s %-11s %-11s %-11s %8s %8s %8s %8s\n", "case", "preset", "created", "render", "create",
              "evaluate", "error", "right8%", "bottom8%", "worst64");
  for (int preset : presets) {
    for (const Case& c : cases) {
      const Result r = Run(params, up, output, readback, out_fp, c, preset, frames, sign);
      std::printf("%-32s %-6s %4ux%-6u %4ux%-6u 0x%08x  0x%08x  %8.5f %8.5f %8.5f %8.5f\n", c.name,
                  preset_names[preset], c.create_w, c.create_h, c.render_w, c.render_h,
                  static_cast<unsigned>(r.create), static_cast<unsigned>(r.evaluate), r.mae, r.right, r.bottom,
                  r.worst_block);
      std::fflush(stdout);
    }
  }
  NVSDK_NGX_D3D12_DestroyParameters(params);
  NVSDK_NGX_D3D12_DestroyParameters(caps);
  NVSDK_NGX_D3D12_Shutdown1(g_device);
  return 0;
}
