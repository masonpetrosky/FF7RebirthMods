// SPDX-License-Identifier: MIT

#include "dlss_probe.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

struct ID3D11Resource;

namespace npcmotion::dlss_probe {
namespace {

// NGX's parameter block, as declared in the DLSS SDK (nvsdk_ngx_params.h); the method order defines the vtable.
struct NgxParameter {
  virtual void Set(const char* name, unsigned long long value) = 0;
  virtual void Set(const char* name, float value) = 0;
  virtual void Set(const char* name, double value) = 0;
  virtual void Set(const char* name, unsigned int value) = 0;
  virtual void Set(const char* name, int value) = 0;
  virtual void Set(const char* name, ID3D11Resource* value) = 0;
  virtual void Set(const char* name, ID3D12Resource* value) = 0;
  virtual void Set(const char* name, void* value) = 0;
  virtual int Get(const char* name, unsigned long long* value) const = 0;
  virtual int Get(const char* name, float* value) const = 0;
  virtual int Get(const char* name, double* value) const = 0;
  virtual int Get(const char* name, unsigned int* value) const = 0;
  virtual int Get(const char* name, int* value) const = 0;
  virtual int Get(const char* name, ID3D11Resource** value) const = 0;
  virtual int Get(const char* name, ID3D12Resource** value) const = 0;
  virtual int Get(const char* name, void** value) const = 0;
  virtual void Reset() = 0;
};
constexpr int kNgxSuccess = 1;

using CreateFeatureFn = int (*)(ID3D12GraphicsCommandList*, int feature, NgxParameter*, void** handle);
using EvaluateFeatureFn = int (*)(ID3D12GraphicsCommandList*, const void* handle, const NgxParameter*, void* callback);

std::atomic<bool> g_enabled{false};
std::filesystem::path g_log_path;
std::mutex g_log_mutex;
FILE* g_log = nullptr;
std::atomic<bool> g_installed{false};
std::string g_status = "waiting for NGX";
CreateFeatureFn g_create = nullptr;
EvaluateFeatureFn g_evaluate = nullptr;
std::atomic<uint32_t> g_armed{0};
std::atomic<uint64_t> g_frame{0};
std::atomic<uint64_t> g_evaluations{0};

// The projection jitter of the current frame (written at the first skinned draw, read at the evaluation).
std::mutex g_jitter_mutex;
uint64_t g_jitter_frame = 0;
float g_jitter[4] = {};

// Jitter positions of recent evaluations (rounded to 1/1000 px), to estimate the sequence length.
std::mutex g_history_mutex;
std::vector<std::pair<int, int>> g_history;

void Write(const char* text) {
  std::lock_guard lock(g_log_mutex);
  if (g_log == nullptr && _wfopen_s(&g_log, g_log_path.c_str(), L"a") != 0) g_log = nullptr;
  if (g_log == nullptr) return;
  fputs(text, g_log);
  fputc('\n', g_log);
  fflush(g_log);
}

float GetF(const NgxParameter* p, const char* name, float fallback = NAN) {
  float v = 0;
  return p->Get(name, &v) == kNgxSuccess ? v : fallback;
}

long long GetU(const NgxParameter* p, const char* name) {
  unsigned int v = 0;
  return p->Get(name, &v) == kNgxSuccess ? static_cast<long long>(v) : -1;
}

long long GetI(const NgxParameter* p, const char* name) {
  int v = 0;
  return p->Get(name, &v) == kNgxSuccess ? v : -1;
}

std::string Resource(const NgxParameter* p, const char* name) {
  ID3D12Resource* r = nullptr;
  if (p->Get(name, &r) != kNgxSuccess || r == nullptr) return std::string(name) + "=none";
  const D3D12_RESOURCE_DESC d = r->GetDesc();
  char buf[160];
  snprintf(buf, sizeof(buf), "%s=%llux%u fmt%d", name, static_cast<unsigned long long>(d.Width), d.Height,
           static_cast<int>(d.Format));
  return buf;
}

std::string CreateFlags(long long flags) {
  if (flags < 0) return "?";
  static const char* kNames[] = {"IsHDR", "MVLowRes", "MVJittered", "DepthInverted", "Reserved0", "DoSharpening",
                                 "AutoExposure", "AlphaUpscaling"};
  std::string out;
  for (int i = 0; i < 8; ++i)
    if (flags & (1ll << i)) out += std::string(out.empty() ? "" : "|") + kNames[i];
  return out.empty() ? "none" : out;
}

std::string Presets(const NgxParameter* p) {
  char buf[200];
  snprintf(buf, sizeof(buf), "preset hints dlaa=%lld q=%lld b=%lld p=%lld up=%lld uq=%lld",
           GetU(p, "DLSS.Hint.Render.Preset.DLAA"), GetU(p, "DLSS.Hint.Render.Preset.Quality"),
           GetU(p, "DLSS.Hint.Render.Preset.Balanced"), GetU(p, "DLSS.Hint.Render.Preset.Performance"),
           GetU(p, "DLSS.Hint.Render.Preset.UltraPerformance"), GetU(p, "DLSS.Hint.Render.Preset.UltraQuality"));
  return buf;
}

int CreateFeatureHook(ID3D12GraphicsCommandList* list, int feature, NgxParameter* p, void** handle) {
  const int result = g_create(list, feature, p, handle);
  char line[1024];
  snprintf(line, sizeof(line),
           "create feature=%d result=0x%X handle=%p frame=%llu render=%lldx%lld out=%lldx%lld quality=%lld "
           "flags=%s (%lld) enable_output_subrects=%lld %s",
           feature, result, handle ? *handle : nullptr, static_cast<unsigned long long>(g_frame.load()),
           GetU(p, "Width"), GetU(p, "Height"), GetU(p, "OutWidth"), GetU(p, "OutHeight"), GetI(p, "PerfQualityValue"),
           CreateFlags(GetI(p, "DLSS.Feature.Create.Flags")).c_str(), GetI(p, "DLSS.Feature.Create.Flags"),
           GetI(p, "DLSS.Enable.Output.Subrects"), Presets(p).c_str());
  Write(line);
  return result;
}

int EvaluateFeatureHook(ID3D12GraphicsCommandList* list, const void* handle, const NgxParameter* p, void* callback) {
  const uint64_t n = ++g_evaluations;
  const float jx = GetF(p, "Jitter.Offset.X"), jy = GetF(p, "Jitter.Offset.Y");
  size_t distinct = 0, window = 0;
  {
    std::lock_guard lock(g_history_mutex);
    g_history.emplace_back(static_cast<int>(std::lround(jx * 1000)), static_cast<int>(std::lround(jy * 1000)));
    if (g_history.size() > 256) g_history.erase(g_history.begin());
    distinct = std::set<std::pair<int, int>>(g_history.begin(), g_history.end()).size();
    window = g_history.size();
  }
  uint32_t armed = g_armed.load();
  while (armed > 0 && !g_armed.compare_exchange_weak(armed, armed - 1)) {
  }
  if (armed > 0) {
    const uint64_t frame = g_frame.load();
    float proj[4] = {NAN, NAN, NAN, NAN};
    bool proj_now = false;
    {
      std::lock_guard lock(g_jitter_mutex);
      if (g_jitter_frame != 0) {
        for (int i = 0; i < 4; ++i) proj[i] = g_jitter[i];
        proj_now = g_jitter_frame == frame;
      }
    }
    const long long rw = GetU(p, "DLSS.Render.Subrect.Dimensions.Width");
    const long long rh = GetU(p, "DLSS.Render.Subrect.Dimensions.Height");
    // Clip-space jitter to render pixels (x right, y down), for comparison with DLSS's.
    const double w = rw > 0 ? static_cast<double>(rw) : 1.0, h = rh > 0 ? static_cast<double>(rh) : 1.0;
    char line[2048];
    snprintf(line, sizeof(line),
             "eval #%llu frame=%llu handle=%p jitter=(%.4f,%.4f) proj_jitter_px=(%.4f,%.4f) prev=(%.4f,%.4f) "
             "proj_from_this_frame=%d mv_scale=(%.4f,%.4f) reset=%lld render_subrect=%lldx%lld size=%lldx%lld "
             "out=%lldx%lld sharpness=%.3f pre_exposure=%.4f exposure_scale=%.4f dt_ms=%.3f flags=%s %s %s %s %s %s "
             "%s distinct_jitter=%zu/%zu",
             static_cast<unsigned long long>(n), static_cast<unsigned long long>(frame), handle, jx, jy,
             proj[0] * w / 2, proj[1] * h / 2, proj[2] * w / 2, proj[3] * h / 2, proj_now ? 1 : 0,
             GetF(p, "MV.Scale.X"), GetF(p, "MV.Scale.Y"), GetI(p, "Reset"), rw, rh, GetU(p, "Width"),
             GetU(p, "Height"), GetU(p, "OutWidth"), GetU(p, "OutHeight"), GetF(p, "Sharpness"),
             GetF(p, "DLSS.Pre.Exposure"), GetF(p, "DLSS.Exposure.Scale"), GetF(p, "FrameTimeDeltaInMsec"),
             CreateFlags(GetI(p, "DLSS.Feature.Create.Flags")).c_str(), Resource(p, "Color").c_str(),
             Resource(p, "Output").c_str(), Resource(p, "MotionVectors").c_str(), Resource(p, "Depth").c_str(),
             Resource(p, "ExposureTexture").c_str(), Resource(p, "DLSS.Input.Bias.Current.Color.Mask").c_str(),
             distinct, window);
    Write(line);
  }
  return g_evaluate(list, handle, p, callback);
}

// Swaps every pointer to `target` in the executable's writable sections; returns how many were replaced.
int SwapPointers(void* target, void* replacement) {
  auto* const base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
  int swapped = 0;
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    if ((sections[i].Characteristics & IMAGE_SCN_MEM_WRITE) == 0) continue;
    auto** slot = reinterpret_cast<void**>(base + sections[i].VirtualAddress);
    const size_t count = sections[i].Misc.VirtualSize / sizeof(void*);
    for (size_t k = 0; k < count; ++k) {
      if (slot[k] != target) continue;
      InterlockedExchangePointer(&slot[k], replacement);
      ++swapped;
    }
  }
  return swapped;
}

}  // namespace

void Initialize(const std::filesystem::path& log_path) {
  g_log_path = log_path;
  g_enabled = true;
  Write("---- probe started");
}

bool enabled() { return g_enabled; }

void OnPresent(uint64_t frame) {
  g_frame = frame;
  if (!g_enabled || g_installed) return;
  HMODULE ngx = GetModuleHandleW(L"_nvngx.dll");
  if (ngx == nullptr) return;
  // The search reads the executable's whole data section (about 40 MB): try every 120 frames, 30 times at most.
  static int tries = 0;
  static uint64_t last_try = 0;
  if (frame - last_try < 120 && last_try != 0) return;
  last_try = frame;
  if (++tries > 30) {
    g_status = "the game's NGX pointers were not found";
    Write(g_status.c_str());
    g_installed = true;
    return;
  }
  auto* const create = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
  auto* const evaluate = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
  if (create == nullptr || evaluate == nullptr) {
    g_status = "NGX exports not found";
    g_installed = true;
    return;
  }
  g_create = reinterpret_cast<CreateFeatureFn>(create);
  g_evaluate = reinterpret_cast<EvaluateFeatureFn>(evaluate);
  const int creates = SwapPointers(create, reinterpret_cast<void*>(&CreateFeatureHook));
  const int evaluates = SwapPointers(evaluate, reinterpret_cast<void*>(&EvaluateFeatureHook));
  if (evaluates == 0) {
    // NGX may not be initialised yet (the pointers are resolved at NVSDK_NGX_D3D12_Init): try again later.
    g_status = "waiting for the game's NGX pointers";
    if (creates != 0) SwapPointers(reinterpret_cast<void*>(&CreateFeatureHook), create);
    return;
  }
  char line[160];
  snprintf(line, sizeof(line), "installed at frame %llu: %d CreateFeature and %d EvaluateFeature pointers",
           static_cast<unsigned long long>(frame), creates, evaluates);
  Write(line);
  g_status = line;
  g_installed = true;
  Arm(60);
}

void Arm(uint32_t evaluations) {
  g_armed = evaluations;
  char line[96];
  snprintf(line, sizeof(line), "---- logging %u evaluations from frame %llu", evaluations,
           static_cast<unsigned long long>(g_frame.load()));
  Write(line);
}

void SetProjectionJitter(uint64_t frame, const float jitter[4]) {
  std::lock_guard lock(g_jitter_mutex);
  g_jitter_frame = frame;
  for (int i = 0; i < 4; ++i) g_jitter[i] = jitter[i];
}

bool HasProjectionJitter(uint64_t frame) {
  std::lock_guard lock(g_jitter_mutex);
  return g_jitter_frame == frame;
}

std::string Status() { return g_status; }

}  // namespace npcmotion::dlss_probe
