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
#include <share.h>
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
std::atomic<bool> g_logging{false};
std::atomic<bool> g_pass_pre_exposure{false};
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
std::atomic<uint64_t> g_pre_exposure_passed{0};
std::atomic<uint64_t> g_pre_exposure_earlier{0};  // of those, a value read in an earlier frame
const int32_t* g_taa_samples = nullptr;
const int32_t* g_taa_method = nullptr;

// The View values of the latest frame with a skinned draw (written at its first one, read at the evaluation).
std::mutex g_view_mutex;
uint64_t g_view_frame = 0;
ViewValues g_view;
std::atomic<uint64_t> g_view_frames{0};
std::atomic<uint64_t> g_view_failures[static_cast<int>(ViewFailure::kCount)];

// Jitter positions of recent evaluations (rounded to 1/1000 px), to estimate the sequence length.
std::mutex g_history_mutex;
std::vector<std::pair<int, int>> g_history;

void Write(const char* text) {
  if (!g_logging) return;
  std::lock_guard lock(g_log_mutex);
  if (g_log == nullptr) g_log = _wfsopen(g_log_path.c_str(), L"a", _SH_DENYNO);  // readable while the game runs
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
  const uint64_t frame = g_frame.load();
  ViewValues view;
  bool view_now = false;
  {
    std::lock_guard lock(g_view_mutex);
    view = g_view;
    view_now = g_view_frame == frame && frame != 0;
  }
  const float game_pre_exposure = GetF(p, "DLSS.Pre.Exposure");
  // The game passes a pre-exposure of 1 although Unreal pre-exposes the scene colour (View.PreExposure). A frame
  // without a character draw before the evaluation gets the latest value read: going back to the game's 1 would make
  // the value DLSS sees jump between frames (0.0002 against 1 in Kalm).
  if (g_pass_pre_exposure && view.pre_exposure > 0) {
    const_cast<NgxParameter*>(p)->Set("DLSS.Pre.Exposure", view.pre_exposure);
    ++g_pre_exposure_passed;
    if (!view_now) ++g_pre_exposure_earlier;
  }
  if (!g_logging) return g_evaluate(list, handle, p, callback);

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
    const long long rw = GetU(p, "DLSS.Render.Subrect.Dimensions.Width");
    const long long rh = GetU(p, "DLSS.Render.Subrect.Dimensions.Height");
    const double w = rw > 0 ? static_cast<double>(rw) : 1.0, h = rh > 0 ? static_cast<double>(rh) : 1.0;
    char line[2560];
    snprintf(line, sizeof(line),
             "eval #%llu frame=%llu handle=%p jitter=(%.4f,%.4f) view_now=%d proj_jitter_px=(%.4f,%.4f) "
             "prev=(%.4f,%.4f) taa_params=(%.0f,%.0f,%.4f,%.4f) mip_bias=%.3f texture_bias=%.3f "
             "view_pre_exposure=%.6g "
             "game_pre_exposure=%.6g passed_pre_exposure=%.6g cvars samples=%d method=%d view_failures=%llu/%llu/%llu/%llu "
             "mv_scale=(%.4f,%.4f) reset=%lld render_subrect=%lldx%lld size=%lldx%lld out=%lldx%lld sharpness=%.3f "
             "exposure_scale=%.4f dt_ms=%.3f flags=%s %s %s %s %s %s %s distinct_jitter=%zu/%zu",
             static_cast<unsigned long long>(n), static_cast<unsigned long long>(frame), handle, jx, jy,
             view_now ? 1 : 0, view.jitter[0] * w / 2, view.jitter[1] * h / 2, view.jitter[2] * w / 2,
             view.jitter[3] * h / 2, view.taa_params[0], view.taa_params[1], view.taa_params[2], view.taa_params[3],
             view.mip_bias, view.sampler_bias[1], view.pre_exposure, game_pre_exposure, GetF(p, "DLSS.Pre.Exposure"),
             g_taa_samples ? *g_taa_samples : -1, g_taa_method ? *g_taa_method : -1,
             static_cast<unsigned long long>(g_view_failures[0].load()),
             static_cast<unsigned long long>(g_view_failures[1].load()),
             static_cast<unsigned long long>(g_view_failures[2].load()),
             static_cast<unsigned long long>(g_view_failures[3].load()), GetF(p, "MV.Scale.X"), GetF(p, "MV.Scale.Y"),
             GetI(p, "Reset"), rw, rh, GetU(p, "Width"), GetU(p, "Height"), GetU(p, "OutWidth"), GetU(p, "OutHeight"),
             GetF(p, "Sharpness"), GetF(p, "DLSS.Exposure.Scale"), GetF(p, "FrameTimeDeltaInMsec"),
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

void Initialize(const std::filesystem::path& log_path, bool pass_pre_exposure) {
  g_log_path = log_path;
  g_logging = !log_path.empty();
  g_pass_pre_exposure = pass_pre_exposure;
  g_enabled = true;
  Write("---- probe started");
}

void Shutdown() {
  if (g_create != nullptr) SwapPointers(reinterpret_cast<void*>(&CreateFeatureHook), reinterpret_cast<void*>(g_create));
  if (g_evaluate != nullptr)
    SwapPointers(reinterpret_cast<void*>(&EvaluateFeatureHook), reinterpret_cast<void*>(g_evaluate));
  std::lock_guard lock(g_log_mutex);
  if (g_log != nullptr) fclose(g_log);
  g_log = nullptr;
}

bool enabled() { return g_enabled; }
bool logging() { return g_logging; }

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
  snprintf(line, sizeof(line), "hooked at frame %llu: %d CreateFeature and %d EvaluateFeature pointers",
           static_cast<unsigned long long>(frame), creates, evaluates);
  Write(line);
  g_status = line;
  g_installed = true;
  if (g_logging) Arm(60);
}

void Arm(uint32_t evaluations) {
  g_armed = evaluations;
  char line[96];
  snprintf(line, sizeof(line), "---- logging %u evaluations from frame %llu", evaluations,
           static_cast<unsigned long long>(g_frame.load()));
  Write(line);
}

void SetViewValues(uint64_t frame, const ViewValues& values) {
  std::lock_guard lock(g_view_mutex);
  g_view_frame = frame;
  g_view = values;
  ++g_view_frames;
}

bool HasViewValues(uint64_t frame) {
  std::lock_guard lock(g_view_mutex);
  return g_view_frame == frame;
}

ViewValues LatestViewValues() {
  std::lock_guard lock(g_view_mutex);
  return g_view;
}

void CountViewFailure(ViewFailure reason) { ++g_view_failures[static_cast<int>(reason)]; }

void SetConsoleVariables(const int32_t* temporal_aa_samples, const int32_t* temporal_aa_method) {
  g_taa_samples = temporal_aa_samples;
  g_taa_method = temporal_aa_method;
}

void SetPassPreExposure(bool pass) { g_pass_pre_exposure = pass; }
bool pass_pre_exposure() { return g_pass_pre_exposure; }

std::string Status() {
  float pre_exposure;
  {
    std::lock_guard lock(g_view_mutex);
    pre_exposure = g_view.pre_exposure;
  }
  char extra[240];
  snprintf(extra, sizeof(extra),
           "; View values read in %llu frames (pre-exposure %g); pre-exposure passed %llu times (%llu with an earlier "
           "frame's value)",
           static_cast<unsigned long long>(g_view_frames.load()), pre_exposure,
           static_cast<unsigned long long>(g_pre_exposure_passed.load()),
           static_cast<unsigned long long>(g_pre_exposure_earlier.load()));
  return g_status + extra;
}

}  // namespace npcmotion::dlss_probe
