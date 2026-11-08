// SPDX-License-Identifier: MIT

#include "ray_reconstruction.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_dlssd_d3d.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "kRrGuides.h"
#include "kRrShowGuide.h"

namespace npcmotion::rr {
namespace {

using Microsoft::WRL::ComPtr;

// --- The driver's NGX exports ----------------------------------------------------------------------------------------
// The game's NGX static library calls these through pointers it resolved from _nvngx.dll (the DLSS hooks swap two of
// them); calling the exports directly works in the game's NGX context, so no second NVSDK_NGX_D3D12_Init is needed.
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*,
                                               NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                 const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using DestroyFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
struct Exports {
  CreateFn create = nullptr;
  EvaluateFn evaluate = nullptr;
  ParametersFn allocate = nullptr;
  ParametersFn capabilities = nullptr;
  DestroyFn destroy = nullptr;
  ReleaseFn release = nullptr;
};
Exports g_ngx;

}  // namespace
}  // namespace npcmotion::rr

// The SDK's functions that its helpers (nvsdk_ngx_helpers_dlssd_d3d.h) call. NVIDIA implements them in its static
// library, which keeps its own NGX state and would need its own NVSDK_NGX_D3D12_Init: here the parameter functions
// call the parameter block's own methods (as NVIDIA's do) and the feature functions go to the driver's exports.
// The offline test (tests/rr_test.cpp) links NVIDIA's static library instead, the way the game does.
#if !defined(NPCMOTION_RR_WITH_NGX_LIBRARY)
extern "C" {
void NVSDK_CONV NVSDK_NGX_Parameter_SetF(NVSDK_NGX_Parameter* p, const char* name, float value) { p->Set(name, value); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter* p, const char* name, unsigned int value) {
  p->Set(name, value);
}
void NVSDK_CONV NVSDK_NGX_Parameter_SetI(NVSDK_NGX_Parameter* p, const char* name, int value) { p->Set(name, value); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d12Resource(NVSDK_NGX_Parameter* p, const char* name, ID3D12Resource* value) {
  p->Set(name, value);
}
void NVSDK_CONV NVSDK_NGX_Parameter_SetVoidPointer(NVSDK_NGX_Parameter* p, const char* name, void* value) {
  p->Set(name, value);
}
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_Parameter_GetI(NVSDK_NGX_Parameter* p, const char* name, int* value) {
  return p->Get(name, value);
}
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
                                                          NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle) {
  const auto create = npcmotion::rr::g_ngx.create;
  return create != nullptr ? create(list, feature, params, handle) : NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* list,
                                                            const NVSDK_NGX_Handle* handle,
                                                            const NVSDK_NGX_Parameter* params,
                                                            PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto evaluate = npcmotion::rr::g_ngx.evaluate;
  return evaluate != nullptr ? evaluate(list, handle, params, callback) : NVSDK_NGX_Result_Fail;
}
// The helpers' C variant (they pass no progress callback); the driver exports only the one above.
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList* list,
                                                              const NVSDK_NGX_Handle* handle,
                                                              const NVSDK_NGX_Parameter* params,
                                                              PFN_NVSDK_NGX_ProgressCallback_C) {
  const auto evaluate = npcmotion::rr::g_ngx.evaluate;
  return evaluate != nullptr ? evaluate(list, handle, params, nullptr) : NVSDK_NGX_Result_Fail;
}
}
#endif

namespace npcmotion::rr {
namespace {

void (*g_log)(int level, const char* text) = nullptr;

void Log(int level, const std::string& text) {
  if (g_log != nullptr) g_log(level, text.c_str());
}

std::string Hex(unsigned value) {
  char text[16];
  snprintf(text, sizeof(text), "0x%08X", value);
  return text;
}

// The development trace (Trace): lines left, and the time it was armed.
std::atomic<int> g_trace_lines{0};
std::atomic<int64_t> g_trace_start{0};

void TraceLine(const std::string& what) {
  if (g_trace_lines.fetch_sub(1) <= 0) return;
  LARGE_INTEGER now, frequency;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&frequency);
  char line[320];
  snprintf(line, sizeof(line), "Ray Reconstruction trace +%.3f ms [thread %lu] %s",
           double(now.QuadPart - g_trace_start.load()) * 1000.0 / double(frequency.QuadPart), GetCurrentThreadId(),
           what.c_str());
  if (g_log != nullptr) g_log(0, line);
}

constexpr D3D12_RESOURCE_STATES kReadable =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

std::atomic<bool> g_wanted{false};
std::atomic<int> g_guide_view{0};
std::atomic<bool> g_game_recreated{false};

// The base pass's G-buffer targets A, B and C, held (AddRef) so a pointer never outlives its texture; `g_generation`
// counts the base passes seen, so an evaluation only uses a G-buffer bound since the previous one.
std::mutex g_gbuffer_mutex;
ID3D12Resource* g_gbuffer[3] = {};
DXGI_FORMAT g_gbuffer_format[3] = {};
std::atomic<ID3D12Resource*> g_tracked[3] = {};  // the same pointers, for OnBarrier without the lock
std::atomic<uint32_t> g_state[3] = {};           // their latest states in recording order (for the trace only)
std::atomic<uint64_t> g_generation{0};

// Everything below is touched under g_mutex: the game evaluates DLSS from one thread, the overlay reads the status.
std::mutex g_mutex;
bool g_initialized = false;
bool g_unavailable = false;
NVSDK_NGX_Parameter* g_params = nullptr;
NVSDK_NGX_Handle* g_feature = nullptr;
struct FeatureKey {
  unsigned width = 0, height = 0, out_width = 0, out_height = 0;
  int quality = -1, flags = 0, output_subrects = 0;
  bool operator==(const FeatureKey&) const = default;
};
FeatureKey g_key;
FeatureKey g_failed_key;  // a creation that failed: not tried again until the game creates its DLSS anew
bool g_failed = false;
bool g_reset_next = true;
uint64_t g_evaluations = 0;  // calls handed here
uint64_t g_ran = 0;          // Ray Reconstruction evaluations
uint64_t g_fallbacks = 0;
uint64_t g_used_generation = 0;
std::string g_status = "off";
std::string g_last_fallback;

// Released a few evaluations later, when the GPU is done with the frames that used them.
constexpr uint64_t kReleaseDelay = 8;
struct RetiredResource {
  ComPtr<ID3D12Resource> resource;
  uint64_t at;
};
struct RetiredFeature {
  NVSDK_NGX_Handle* handle;
  uint64_t at;
};
std::vector<RetiredResource> g_retired_resources;
std::vector<RetiredFeature> g_retired_features;

// The guides pass: plain D3D12 objects, made on the game's device.
struct GuideConstants {
  float world_to_view[3][4];  // TranslatedWorldToView's rows
  float projection_scale[2];  // ViewToClip[0][0], [1][1]
  uint32_t size[2];           // the render size
  uint32_t output_size[2];    // ShowGuide: the output size
  uint32_t guide;             // ShowGuide: which guide
  uint32_t unused;
};
static_assert(sizeof(GuideConstants) == 20 * 4);
constexpr uint32_t kSetSize = 10;  // t0-t5, u0-u3 (rr_guides.hlsl)
constexpr uint32_t kSets = 8;      // a ring, so a set is not rewritten while the GPU may still read it
struct Pass {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> guides, show;
  ComPtr<ID3D12DescriptorHeap> heap;
  UINT increment = 0;
  uint32_t next_set = 0;
  ComPtr<ID3D12Resource> textures[3];  // diffuse albedo, specular albedo, normals and roughness
  uint32_t width = 0, height = 0;
  bool failed = false;
};
Pass g_pass;

// A frame left to the game's DLSS. Logged when the reason changes, the first 10 times (the overlay keeps the count and
// the last reason), so reasons that alternate cannot flood the log.
bool Fallback(const std::string& why) {
  static int logged = 0;
  ++g_fallbacks;
  g_reset_next = true;
  if (why != g_last_fallback) {
    g_last_fallback = why;
    if (logged++ < 10)
      Log(1, "Ray Reconstruction: " + why + "; this frame uses the game's DLSS (fallback " +
                 std::to_string(g_fallbacks) + ")");
  }
  return false;
}

void ReleaseRetired(bool all) {
  for (size_t i = 0; i < g_retired_resources.size();)
    if (all || g_retired_resources[i].at <= g_evaluations)
      g_retired_resources.erase(g_retired_resources.begin() + static_cast<ptrdiff_t>(i));
    else
      ++i;
  for (size_t i = 0; i < g_retired_features.size();)
    if (all || g_retired_features[i].at <= g_evaluations) {
      if (g_ngx.release != nullptr) g_ngx.release(g_retired_features[i].handle);
      g_retired_features.erase(g_retired_features.begin() + static_cast<ptrdiff_t>(i));
    } else {
      ++i;
    }
}

bool InitializeNgx() {
  g_initialized = true;
  HMODULE ngx = GetModuleHandleW(L"_nvngx.dll");
  if (ngx == nullptr) {
    g_status = "not available: NGX is not loaded";
    return false;
  }
  g_ngx.create = reinterpret_cast<CreateFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
  g_ngx.evaluate = reinterpret_cast<EvaluateFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
  g_ngx.allocate = reinterpret_cast<ParametersFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
  g_ngx.capabilities = reinterpret_cast<ParametersFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
  g_ngx.destroy = reinterpret_cast<DestroyFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_DestroyParameters"));
  g_ngx.release = reinterpret_cast<ReleaseFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature"));
  if (g_ngx.create == nullptr || g_ngx.evaluate == nullptr || g_ngx.allocate == nullptr ||
      g_ngx.capabilities == nullptr || g_ngx.destroy == nullptr || g_ngx.release == nullptr) {
    g_status = "not available: NGX exports missing";
    return false;
  }
  NVSDK_NGX_Parameter* caps = nullptr;
  NVSDK_NGX_Result result = g_ngx.capabilities(&caps);
  if (NVSDK_NGX_FAILED(result) || caps == nullptr) {
    g_status = "not available: NGX capability query failed " + Hex(result);
    return false;
  }
  int available = 0, needs_driver = 0, init_result = 0;
  NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
  NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needs_driver);
  NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &init_result);
  g_ngx.destroy(caps);
  Log(0, "Ray Reconstruction: NGX reports available=" + std::to_string(available) + ", needs a newer driver=" +
             std::to_string(needs_driver) + ", feature init result " + Hex(static_cast<unsigned>(init_result)));
  if (needs_driver != 0) {
    g_status = "not available: needs a newer NVIDIA driver";
    return false;
  }
  if (available == 0) {
    g_status = "not available: the game's NGX does not offer it (nvngx_dlssd.dll not found on its search path?)";
    return false;
  }
  result = g_ngx.allocate(&g_params);
  if (NVSDK_NGX_FAILED(result) || g_params == nullptr) {
    g_params = nullptr;
    g_status = "not available: NGX parameter allocation failed " + Hex(result);
    return false;
  }
  return true;
}

// The game's values in its parameter block (the creation's sizes and flags stay in it next to each evaluation's).
unsigned GetU(const NVSDK_NGX_Parameter* p, const char* name, unsigned fallback = 0) {
  unsigned v = 0;
  return NVSDK_NGX_SUCCEED(p->Get(name, &v)) ? v : fallback;
}
int GetI(const NVSDK_NGX_Parameter* p, const char* name, int fallback = 0) {
  int v = 0;
  return NVSDK_NGX_SUCCEED(p->Get(name, &v)) ? v : fallback;
}
float GetF(const NVSDK_NGX_Parameter* p, const char* name, float fallback) {
  float v = 0;
  return NVSDK_NGX_SUCCEED(p->Get(name, &v)) ? v : fallback;
}
ID3D12Resource* GetR(const NVSDK_NGX_Parameter* p, const char* name) {
  ID3D12Resource* r = nullptr;
  return NVSDK_NGX_SUCCEED(p->Get(name, &r)) ? r : nullptr;
}

bool EnsureFeature(ID3D12GraphicsCommandList* list, const FeatureKey& key) {
  const bool recreated = g_game_recreated.exchange(false);
  if (g_feature != nullptr && key == g_key && !recreated) return true;
  if (g_failed && key == g_failed_key && !recreated) return false;
  if (g_feature != nullptr) {
    g_retired_features.push_back({g_feature, g_evaluations + kReleaseDelay});
    g_feature = nullptr;
  }
  for (const char* hint : {NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
                           NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
                           NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
                           NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
                           NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
                           NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality})
    NVSDK_NGX_Parameter_SetUI(g_params, hint, NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default);
  NVSDK_NGX_DLSSD_Create_Params create = {};
  create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
  create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;  // the roughness in the normals' .w
  create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
  create.InWidth = key.width;
  create.InHeight = key.height;
  create.InTargetWidth = key.out_width;
  create.InTargetHeight = key.out_height;
  create.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(key.quality);
  // The game's flags (IsHDR, MVLowRes, DepthInverted, AutoExposure): Ray Reconstruction has no auto-exposure and
  // takes the pre-exposure instead.
  create.InFeatureCreateFlags = key.flags & ~static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
  create.InEnableOutputSubrects = key.output_subrects != 0;
  const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSSD_EXT(list, 1, 1, &g_feature, g_params, &create);
  const bool made = NVSDK_NGX_SUCCEED(result) && g_feature != nullptr;
  const std::string outcome = made ? "created" : "creation failed " + Hex(result);
  char line[256];
  snprintf(line, sizeof(line), "Ray Reconstruction: feature for %ux%u -> %ux%u, quality mode %d, flags 0x%X%s: %s",
           key.width, key.height, key.out_width, key.out_height, key.quality, create.InFeatureCreateFlags,
           recreated ? " (the game recreated its DLSS)" : "", outcome.c_str());
  Log(made ? 0 : 2, line);
  if (!made) {
    g_feature = nullptr;
    g_key = {};
    // The same values would fail again: wait for the game's next creation before trying.
    g_failed = true;
    g_failed_key = key;
    return false;
  }
  g_failed = false;
  g_key = key;
  g_reset_next = true;
  return true;
}

bool CreatePass(ID3D12Device* device, std::string* why) {
  g_pass.device = device;
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 6;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 4;
  ranges[1].OffsetInDescriptorsFromTableStart = 6;
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = sizeof(GuideConstants) / 4;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 2;
  params[1].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC root = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, error;
  if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
      FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&g_pass.root)))) {
    *why = "the guides pass's root signature could not be made";
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = g_pass.root.Get();
  pso.CS = {kRrGuides, sizeof(kRrGuides)};
  if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pass.guides)))) {
    *why = "the guides pass's pipeline could not be made";
    return false;
  }
  pso.CS = {kRrShowGuide, sizeof(kRrShowGuide)};
  if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pass.show)))) {
    *why = "the guide view's pipeline could not be made";
    return false;
  }
  const D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSetSize * kSets,
                                           D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_pass.heap)))) {
    *why = "the guides pass's descriptor heap could not be made";
    return false;
  }
  g_pass.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return true;
}

bool EnsureTextures(uint32_t width, uint32_t height, std::string* why) {
  if (g_pass.textures[0] != nullptr && g_pass.width == width && g_pass.height == height) return true;
  for (ComPtr<ID3D12Resource>& texture : g_pass.textures)
    if (texture != nullptr) g_retired_resources.push_back({std::move(texture), g_evaluations + kReleaseDelay});
  const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  static const wchar_t* const kNames[3] = {L"NpcMotion RR diffuse albedo", L"NpcMotion RR specular albedo",
                                           L"NpcMotion RR normals and roughness"};
  for (int i = 0; i < 3; ++i) {
    if (FAILED(g_pass.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kReadable, nullptr,
                                                      IID_PPV_ARGS(&g_pass.textures[i])))) {
      for (ComPtr<ID3D12Resource>& texture : g_pass.textures) texture.Reset();
      *why = "the guide textures could not be made";
      return false;
    }
    g_pass.textures[i]->SetName(kNames[i]);
  }
  g_pass.width = width;
  g_pass.height = height;
  return true;
}

DXGI_FORMAT TypedFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return format;
  }
}


// Writes this evaluation's descriptor set and records the guides pass; `set` is kept for the guide view.
bool RunGuides(ID3D12GraphicsCommandList* list, ID3D12Resource* output, uint32_t color_width, uint32_t color_height,
               uint32_t width, uint32_t height, const Matrices& m, uint32_t* set, std::string* why) {
  if (g_pass.failed) {
    *why = "the guides pass could not be made";
    return false;
  }
  if (g_pass.device == nullptr) {
    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || !CreatePass(device.Get(), why)) {
      g_pass.failed = true;
      Log(2, "Ray Reconstruction: " + *why);
      return false;
    }
  }
  // Held while this pass records (OnGBuffer may swap the targets from another thread); the game keeps its own
  // references for the GPU's use.
  ComPtr<ID3D12Resource> gbuffer[3];
  DXGI_FORMAT formats[3];
  uint64_t generation;
  {
    std::lock_guard lock(g_gbuffer_mutex);
    for (int i = 0; i < 3; ++i) {
      gbuffer[i] = g_gbuffer[i];
      formats[i] = g_gbuffer_format[i];
    }
    generation = g_generation.load();
  }
  if (gbuffer[0] == nullptr || gbuffer[1] == nullptr || gbuffer[2] == nullptr) {
    *why = "no G-buffer seen";
    return false;
  }
  if (generation == g_used_generation) {
    *why = "no base pass since the last evaluation";
    return false;
  }
  for (const ComPtr<ID3D12Resource>& g : gbuffer) {
    const D3D12_RESOURCE_DESC d = g->GetDesc();
    if (d.Width < width || d.Height < height) {
      *why = "the G-buffer is smaller than the render size";
      return false;
    }
  }
  if (!EnsureTextures(color_width, color_height, why)) return false;
  g_used_generation = generation;

  *set = g_pass.next_set++ % kSets;
  const D3D12_CPU_DESCRIPTOR_HANDLE cpu_base = {g_pass.heap->GetCPUDescriptorHandleForHeapStart().ptr +
                                                SIZE_T(*set) * kSetSize * g_pass.increment};
  auto at = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu_base.ptr + SIZE_T(i) * g_pass.increment}; };
  auto srv = [&](ID3D12Resource* r, DXGI_FORMAT format, uint32_t i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = format;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    g_pass.device->CreateShaderResourceView(r, &d, at(i));
  };
  auto uav = [&](ID3D12Resource* r, DXGI_FORMAT format, uint32_t i) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = format;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_pass.device->CreateUnorderedAccessView(r, nullptr, &d, at(i));
  };
  for (int i = 0; i < 3; ++i) srv(gbuffer[i].Get(), TypedFormat(formats[i]), i);
  for (int i = 0; i < 3; ++i) srv(g_pass.textures[i].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 3 + i);
  for (int i = 0; i < 3; ++i) uav(g_pass.textures[i].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 6 + i);
  const DXGI_FORMAT output_format = output != nullptr ? TypedFormat(output->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
  if (output != nullptr && output_format != DXGI_FORMAT_UNKNOWN)
    uav(output, output_format, 9);
  else
    uav(nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT, 9);  // a null view: only the guide view writes it

  GuideConstants c = {};
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) c.world_to_view[r][k] = m.translated_world_to_view[r * 4 + k];
  c.projection_scale[0] = m.view_to_clip[0];
  c.projection_scale[1] = m.view_to_clip[5];
  c.size[0] = width;
  c.size[1] = height;

  // No barriers on the G-buffer: when the GPU reaches the game's DLSS call the targets are shader resources (0xC0),
  // as the game only ever switches them between that and render target (0x4), and its lighting reads them before
  // DLSS. The states the barrier events report cannot tell that: the game records the base pass on worker threads and
  // resolves some transitions when it submits, so in recording order its 0x4 -> 0xC0 sometimes comes after the DLSS
  // call (docs/ray-reconstruction.md, the barrier trace).
  if (g_ran == 0) {
    char line[160];
    snprintf(line, sizeof(line), "Ray Reconstruction: G-buffer formats %d/%d/%d", formats[0], formats[1], formats[2]);
    Log(0, line);
  }
  D3D12_RESOURCE_BARRIER to_uav[3] = {}, to_read[3] = {};
  for (int i = 0; i < 3; ++i) {
    to_uav[i].Type = to_read[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_uav[i].Transition.pResource = to_read[i].Transition.pResource = g_pass.textures[i].Get();
    to_uav[i].Transition.Subresource = to_read[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_uav[i].Transition.StateBefore = to_read[i].Transition.StateAfter = kReadable;
    to_uav[i].Transition.StateAfter = to_read[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  list->ResourceBarrier(3, to_uav);
  ID3D12DescriptorHeap* heaps[] = {g_pass.heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(g_pass.root.Get());
  list->SetPipelineState(g_pass.guides.Get());
  list->SetComputeRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
  list->SetComputeRootDescriptorTable(1, {g_pass.heap->GetGPUDescriptorHandleForHeapStart().ptr +
                                          UINT64(*set) * kSetSize * g_pass.increment});
  list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  list->ResourceBarrier(3, to_read);
  return true;
}

// Development: one guide over DLSS's output (after Ray Reconstruction wrote it).
void ShowGuide(ID3D12GraphicsCommandList* list, ID3D12Resource* output, uint32_t set, uint32_t width, uint32_t height,
               int guide) {
  if (output == nullptr || TypedFormat(output->GetDesc().Format) == DXGI_FORMAT_UNKNOWN) return;
  const D3D12_RESOURCE_DESC d = output->GetDesc();
  GuideConstants c = {};
  c.size[0] = width;
  c.size[1] = height;
  c.output_size[0] = static_cast<uint32_t>(d.Width);
  c.output_size[1] = d.Height;
  c.guide = static_cast<uint32_t>(guide);
  D3D12_RESOURCE_BARRIER uav = {};
  uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav.UAV.pResource = output;
  list->ResourceBarrier(1, &uav);
  ID3D12DescriptorHeap* heaps[] = {g_pass.heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(g_pass.root.Get());
  list->SetPipelineState(g_pass.show.Get());
  list->SetComputeRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
  list->SetComputeRootDescriptorTable(1, {g_pass.heap->GetGPUDescriptorHandleForHeapStart().ptr +
                                          UINT64(set) * kSetSize * g_pass.increment});
  list->Dispatch((c.output_size[0] + 7) / 8, (c.output_size[1] + 7) / 8, 1);
  list->ResourceBarrier(1, &uav);
}

}  // namespace

void SetLogger(void (*log)(int level, const char* text)) { g_log = log; }

void SetWanted(bool on) {
  const bool was = g_wanted.exchange(on);
  if (was == on) return;
  std::lock_guard lock(g_mutex);
  g_reset_next = true;
  if (!g_unavailable) g_status = on ? "waiting for the game's DLSS" : "off";
  Log(0, on ? "Ray Reconstruction on." : "Ray Reconstruction off: the game's DLSS super resolution.");
  if (on) Trace(120);
}

void Trace(int lines) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  g_trace_start = now.QuadPart;
  g_trace_lines = lines;
}

void TraceMarker(const char* what) {
  if (g_trace_lines.load(std::memory_order_relaxed) > 0) TraceLine(what);
}

bool wanted() { return g_wanted; }

void SetGuideView(int guide) { g_guide_view = guide < 0 || guide > 4 ? 0 : guide; }

int guide_view() { return g_guide_view; }

void OnGBuffer(ID3D12Resource* a, ID3D12Resource* b, ID3D12Resource* c, uint32_t format_a, uint32_t format_b,
               uint32_t format_c) {
  ID3D12Resource* const targets[3] = {a, b, c};
  const uint32_t formats[3] = {format_a, format_b, format_c};
  std::lock_guard lock(g_gbuffer_mutex);
  for (int i = 0; i < 3; ++i) {
    if (g_gbuffer[i] != targets[i]) {
      if (targets[i] != nullptr) targets[i]->AddRef();
      if (g_gbuffer[i] != nullptr) g_gbuffer[i]->Release();
      g_gbuffer[i] = targets[i];
      g_tracked[i] = targets[i];
    }
    g_gbuffer_format[i] = static_cast<DXGI_FORMAT>(formats[i]);
    g_state[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  ++g_generation;
  if (g_trace_lines.load(std::memory_order_relaxed) > 0) {
    char what[96];
    snprintf(what, sizeof(what), "the base pass binds the G-buffer (A %p)", static_cast<void*>(a));
    TraceLine(what);
  }
}

void OnBarrier(const void* resource, uint32_t old_state, uint32_t new_state) {
  if (resource == nullptr) return;
  for (int i = 0; i < 3; ++i) {
    if (g_tracked[i].load(std::memory_order_relaxed) != resource) continue;
    if (old_state != new_state) g_state[i] = new_state;
    if (g_trace_lines.load(std::memory_order_relaxed) > 0) {
      char what[96];
      snprintf(what, sizeof(what), "barrier on GBuffer%c: 0x%X -> 0x%X", "ABC"[i], old_state, new_state);
      TraceLine(what);
    }
  }
}

bool Evaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* game, float pre_exposure,
              const Matrices* matrices) {
  if (!g_wanted) return false;
  std::lock_guard lock(g_mutex);
  ++g_evaluations;
  if (g_trace_lines.load(std::memory_order_relaxed) > 0) {
    char what[96];
    snprintf(what, sizeof(what), "DLSS evaluation; tracked states 0x%X/0x%X/0x%X", g_state[0].load(),
             g_state[1].load(), g_state[2].load());
    TraceLine(what);
  }
  ReleaseRetired(false);
  if (g_unavailable) return false;
  if (!g_initialized && !InitializeNgx()) {
    g_unavailable = true;
    Log(2, "Ray Reconstruction " + g_status + "; the game's DLSS keeps running.");
    return false;
  }
  if (matrices == nullptr) return Fallback("no View matrices read yet");

  ID3D12Resource* const color = GetR(game, NVSDK_NGX_Parameter_Color);
  ID3D12Resource* const output = GetR(game, NVSDK_NGX_Parameter_Output);
  ID3D12Resource* const depth = GetR(game, NVSDK_NGX_Parameter_Depth);
  ID3D12Resource* const motion = GetR(game, NVSDK_NGX_Parameter_MotionVectors);
  if (color == nullptr || output == nullptr || depth == nullptr || motion == nullptr)
    return Fallback("the game's evaluation lacks an input");
  const unsigned width = GetU(game, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width);
  const unsigned height = GetU(game, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height);
  if (width == 0 || height == 0) return Fallback("no render size in the game's evaluation");
  // The guides are written from the top-left corner; a view elsewhere in the textures would need offsets.
  if (GetU(game, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X) != 0 ||
      GetU(game, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y) != 0)
    return Fallback("the game's colour is not at the textures' top-left corner");

  FeatureKey key;
  key.width = GetU(game, NVSDK_NGX_Parameter_Width);
  key.height = GetU(game, NVSDK_NGX_Parameter_Height);
  key.out_width = GetU(game, NVSDK_NGX_Parameter_OutWidth);
  key.out_height = GetU(game, NVSDK_NGX_Parameter_OutHeight);
  key.quality = GetI(game, NVSDK_NGX_Parameter_PerfQualityValue, -1);
  key.flags = GetI(game, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags);
  key.output_subrects = GetI(game, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects);
  if (key.width == 0 || key.height == 0 || key.out_width == 0 || key.out_height == 0 || key.quality < 0)
    return Fallback("the game's creation values are not readable");
  if (!EnsureFeature(list, key)) return Fallback("no Ray Reconstruction feature");

  const D3D12_RESOURCE_DESC color_desc = color->GetDesc();
  uint32_t set = 0;
  std::string why;
  if (!RunGuides(list, output, static_cast<uint32_t>(color_desc.Width), color_desc.Height, width, height, *matrices,
                 &set, &why))
    return Fallback(why);

  float world_to_view[16], view_to_clip[16];
  memcpy(world_to_view, matrices->translated_world_to_view, sizeof(world_to_view));
  memcpy(view_to_clip, matrices->view_to_clip, sizeof(view_to_clip));
  NVSDK_NGX_D3D12_DLSSD_Eval_Params eval = {};
  eval.pInDiffuseAlbedo = g_pass.textures[0].Get();
  eval.pInSpecularAlbedo = g_pass.textures[1].Get();
  eval.pInNormals = g_pass.textures[2].Get();  // the roughness in .w (NVSDK_NGX_DLSS_Roughness_Mode_Packed)
  eval.pInColor = color;
  eval.pInOutput = output;
  eval.pInDepth = depth;
  eval.pInMotionVectors = motion;
  eval.InJitterOffsetX = GetF(game, NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
  eval.InJitterOffsetY = GetF(game, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
  eval.InRenderSubrectDimensions = {width, height};
  eval.InReset = (GetI(game, NVSDK_NGX_Parameter_Reset) != 0 || g_reset_next) ? 1 : 0;
  eval.InMVScaleX = GetF(game, NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
  eval.InMVScaleY = GetF(game, NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
  eval.InDepthSubrectBase = {GetU(game, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X),
                             GetU(game, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y)};
  eval.InMVSubrectBase = {GetU(game, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X),
                          GetU(game, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y)};
  eval.InOutputSubrectBase = {GetU(game, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X),
                              GetU(game, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y)};
  eval.pInWorldToViewMatrix = world_to_view;
  eval.pInViewToClipMatrix = view_to_clip;
  eval.InPreExposure = pre_exposure > 0.0f ? pre_exposure : 1.0f;
  eval.InExposureScale = GetF(game, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
  eval.InFrameTimeDeltaInMsec = GetF(game, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 0.0f);
  const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSSD_EXT(list, g_feature, g_params, &eval);
  if (NVSDK_NGX_FAILED(result)) return Fallback("Ray Reconstruction's evaluation failed " + Hex(result));

  const int guide = g_guide_view;
  if (guide != 0) ShowGuide(list, output, set, width, height, guide);
  if (g_ran++ == 0) {
    char line[200];
    snprintf(line, sizeof(line), "Ray Reconstruction: first evaluation, render %ux%u in %llux%u, pre-exposure %g", width,
             height, static_cast<unsigned long long>(color_desc.Width), color_desc.Height, eval.InPreExposure);
    Log(0, line);
  }
  g_reset_next = false;
  char status[160];
  snprintf(status, sizeof(status), "running: %ux%u -> %ux%u, %llu frames, %llu left to the game's DLSS", width, height,
           key.out_width, key.out_height, static_cast<unsigned long long>(g_ran),
           static_cast<unsigned long long>(g_fallbacks));
  g_status = status;
  if (!g_last_fallback.empty()) g_status += " (last: " + g_last_fallback + ")";
  return true;
}

void OnGameCreate() { g_game_recreated = true; }

std::string Status() {
  std::lock_guard lock(g_mutex);
  return g_status;
}

void Shutdown() {
  std::lock_guard lock(g_mutex);
  ReleaseRetired(true);
  if (g_feature != nullptr && g_ngx.release != nullptr) g_ngx.release(g_feature);
  g_feature = nullptr;
  if (g_params != nullptr && g_ngx.destroy != nullptr) g_ngx.destroy(g_params);
  g_params = nullptr;
  g_pass = Pass();
  std::lock_guard gbuffer_lock(g_gbuffer_mutex);
  for (int i = 0; i < 3; ++i) {
    if (g_gbuffer[i] != nullptr) g_gbuffer[i]->Release();
    g_gbuffer[i] = nullptr;
    g_tracked[i] = nullptr;
  }
}

}  // namespace npcmotion::rr
