// FF7 Rebirth Frame Limit
// ReShade add-on that replaces FINAL FANTASY VII REBIRTH's frame-rate limit, which counts rendered frames only, with a
// limit on the frames the display shows, the ones frame generation makes included.
//
// The game's frame-rate option (30, 60, 90 or 120 fps; it has no unlimited entry) sets t.MaxFPS to the option x 0.999,
// 119.88 for 120. Each game frame, Unreal's UEngine::UpdateTimeAndHandleMaxTickRate first asks the tick-rate handlers,
// of which the game has one, NVIDIA Streamline's: it runs Reflex's sleep, gives Reflex no frame limit and leaves the
// limit to the engine, which then waits until 1 / t.MaxFPS has passed since the frame before. The limit therefore counts
// rendered frames: with DLSS Frame Generation the 120 option lets the display show 240 frames a second, with NVIDIA's
// 3x or 4x override 360 or 480.
//
// The add-on points the engine's one read of t.MaxFPS (in UEngine::GetMaxTickRate) at a value of its own: Limit divided
// by the number of frames the display shows per rendered frame. It counts that: generated frames pass through ReShade's
// swap chain, so its present event sees every shown frame, and a small routine in the Streamline handler's vtable counts
// the engine's calls, one per game frame. Everything else that reads t.MaxFPS, the options menu included, still sees the
// game's own value. While the game's own VSync is on and frame generation off, the game also sets rhi.SyncInterval to
// refresh rate / option (2 for 120 fps at 240 Hz), which limits the frame rate as well; the add-on lowers it to 1 while
// its limit is on, so VSync waits for one refresh, not two.
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

#include "game_code.hpp"

extern "C" __declspec(dllexport) const char* NAME = "FF7 Rebirth Frame Limit";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Replaces FINAL FANTASY VII REBIRTH's 30/60/90/120 limit on rendered frames with one limit on the frames the "
    "display shows, frame generation included.";

using namespace reshade::api;
namespace code = framelimit::game_code;

namespace {

namespace fs = std::filesystem;

constexpr const char kHandlerClass[] = ".?AVFStreamlineMaxTickRateHandler@@";

// Settings (FF7RebirthFrameLimit.ini, section [FrameLimit]).
fs::path g_ini_path;
int g_limit = -1;      // shown frames per second; 0 = no limit; below 0 = 3 below the display's refresh rate
int g_test_limit = 0;  // a second limit the toggle key also switches to; 0 = none
int g_toggle_key = 0;  // virtual-key code, 0 = none
bool g_log_stats = false;

// Which limit applies: the game's own frame-rate option, Limit, or TestLimit.
enum class Mode { game, limit, test };
std::atomic<Mode> g_mode{Mode::limit};

// What was found in the game's executable.
code::Image g_image;
float* const* g_max_fps = nullptr;      // t.MaxFPS: the pointer to its values (game thread, render thread)
int32_t* const* g_frame_gen = nullptr;  // r.FrameGeneration.Method: 0 = off
int32_t* const* g_sync = nullptr;       // rhi.SyncInterval
uint8_t* g_limiter_load = nullptr;      // the engine limiter's load of t.MaxFPS, pointed at g_value
int32_t g_limiter_disp = 0;             // its own displacement
code::LimitValue g_value;               // what the engine waits for
void** g_handler_slot = nullptr;        // FStreamlineMaxTickRateHandler::HandleMaxTickRate, counted by g_frames
void* g_handler_function = nullptr;     // the game's function in that slot
code::CallCounter g_frames;
bool g_active = false;  // the engine's limiter reads g_value
std::string g_status;   // what happened at startup, for the overlay

// Measured and applied while presenting. g_mutex guards everything below.
std::mutex g_mutex;
std::atomic<uint64_t> g_presents{0};
int64_t g_qpf = 1;
int64_t g_window_start = 0;  // the current measuring window (a quarter second)
uint64_t g_window_presents = 0, g_window_frames = 0;
int g_per_frame = 1;     // shown frames per rendered frame, as the limit uses it
bool g_counted = false;  // g_per_frame was counted (not taken from the frame generation setting)
int g_pending = 0, g_pending_windows = 0;
double g_shown_fps = 0.0, g_rendered_fps = 0.0;
float g_game_value = -1.0f;  // the game's own t.MaxFPS, as last seen
float g_rendered_limit = 0.0f;
int g_refresh = 0;  // Hz of the display the game window is on
HMONITOR g_monitor = nullptr;
int64_t g_monitor_checked = 0, g_refresh_queried = 0;
int32_t g_game_sync = -1;  // rhi.SyncInterval the game had set before the add-on lowered it to 1; -1 = not lowered
int64_t g_stats_logged = 0;
bool g_toggle_down = false;

void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("[FrameLimit] " + message).c_str());
}

std::string Format(const char* format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  return text;
}

int64_t Qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

int ReadIniInt(const wchar_t* key, int fallback) {
  return static_cast<int>(GetPrivateProfileIntW(L"FrameLimit", key, fallback, g_ini_path.c_str()));
}

void WriteIniInt(const wchar_t* key, int value) {
  WritePrivateProfileStringW(L"FrameLimit", key, std::to_wstring(value).c_str(), g_ini_path.c_str());
}

// ---------------------------------------------------------------------------------------------
// The limit

// The refresh rate of a display (whole Hz), 0 if unknown.
int RefreshRate(HMONITOR monitor) {
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info))) return 0;
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0;
  return mode.dmDisplayFrequency > 1 ? static_cast<int>(mode.dmDisplayFrequency) : 0;
}

// Which display the game is on, checked once a second; its refresh rate is asked for when the window moves to another
// display, and every 30 s while a limit follows it (a mode change), since asking the driver takes time.
void UpdateRefreshRate(HWND window, int64_t now) {
  if (now - g_monitor_checked < g_qpf) return;
  g_monitor_checked = now;
  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  const bool follows = g_limit < 0;
  if (monitor == g_monitor && !(follows && now - g_refresh_queried > 30 * g_qpf)) return;
  const int refresh = RefreshRate(monitor);
  if (refresh != g_refresh) Log(reshade::log::level::info, Format("The game's display runs at %d Hz.", refresh));
  g_monitor = monitor;
  g_refresh = refresh;
  g_refresh_queried = now;
}

// The ini's Limit in words.
std::string LimitSetting() {
  if (g_limit < 0) return "3 below the display's refresh rate";
  if (g_limit == 0) return "none";
  return std::to_string(g_limit) + " shown frames per second";
}

// The shown frames per second a mode allows, 0 = no limit.
int ShownLimit(Mode mode) {
  const int limit = mode == Mode::test ? g_test_limit : g_limit;
  if (limit >= 0) return limit;
  return g_refresh > 3 ? g_refresh - 3 : 0;
}

std::string ShownLimitText(Mode mode) {
  const int shown = ShownLimit(mode);
  if (shown == 0) return mode == Mode::limit && g_limit < 0 ? "no limit (the display's refresh rate is unknown)" : "no limit";
  std::string text = std::to_string(shown) + " shown frames per second";
  if (mode == Mode::limit && g_limit < 0) text += " (" + std::to_string(g_refresh) + " Hz display, minus 3)";
  return text;
}

int32_t ReadInt(int32_t* const* variable) {
  if (variable == nullptr || *variable == nullptr) return -1;
  return std::atomic_ref<int32_t>((*variable)[0]).load(std::memory_order_relaxed);
}

float GameValue() {
  if (*g_max_fps == nullptr) return 0.0f;
  return std::atomic_ref<float>((*g_max_fps)[0]).load(std::memory_order_relaxed);
}

void WriteInt(int32_t* const* variable, int32_t value) {
  std::atomic_ref<int32_t>((*variable)[0]).store(value, std::memory_order_relaxed);
  std::atomic_ref<int32_t>((*variable)[1]).store(value, std::memory_order_relaxed);
}

// While the add-on's limit is on, a VSync interval of more than one refresh (the game's own limit with VSync) becomes 1.
void ApplySyncInterval(bool ours) {
  if (g_sync == nullptr || *g_sync == nullptr) return;
  const int32_t now = ReadInt(g_sync);
  if (ours && now > 1) {
    Log(reshade::log::level::info, Format("rhi.SyncInterval: the game's %d (VSync every %d refreshes) lowered to 1 while "
                                          "the limit is on.", now, now));
    g_game_sync = now;
    WriteInt(g_sync, 1);
  } else if (!ours && g_game_sync > 1) {
    if (now == 1) {
      WriteInt(g_sync, g_game_sync);
      Log(reshade::log::level::info, Format("rhi.SyncInterval: back to the game's %d.", g_game_sync));
    }
    g_game_sync = -1;
  }
}

void Apply() {
  const float game = GameValue();
  if (game != g_game_value) {
    g_game_value = game;
    Log(reshade::log::level::info, Format("The game's frame-rate limit (t.MaxFPS): %.2f.", game));
  }
  const Mode mode = g_mode.load();
  float rendered = game;
  if (mode != Mode::game) {
    const int shown = ShownLimit(mode);
    rendered = shown > 0 ? static_cast<float>(shown) / static_cast<float>(g_per_frame) : 0.0f;
  }
  if (rendered != g_value.Get()) g_value.Set(rendered);
  g_rendered_limit = rendered;
  ApplySyncInterval(mode != Mode::game);
}

// Counts presents and game frames over the last quarter second: the shown frames per rendered frame are the nearest
// whole number of presents per game frame (1 to 4), taken when two windows in a row agree. Without counted game frames
// (no Streamline handler) they come from the frame generation setting: 2 when it is on.
void Measure(int64_t now) {
  const uint64_t presents = g_presents.load(std::memory_order_relaxed);
  const uint64_t frames = g_frames.count != nullptr ? g_frames.Count() : 0;
  const double seconds = static_cast<double>(now - g_window_start) / static_cast<double>(g_qpf);
  const uint64_t dp = presents - g_window_presents, df = frames - g_window_frames;
  const bool first = g_window_start == 0;
  g_window_start = now;
  g_window_presents = presents;
  g_window_frames = frames;
  if (first || seconds > 2.0) return;  // the first window, or the game stopped presenting for a while
  g_shown_fps = static_cast<double>(dp) / seconds;
  g_rendered_fps = static_cast<double>(df) / seconds;

  int candidate;
  bool counted;
  if (df >= 4) {
    candidate = std::clamp(static_cast<int>(std::lround(static_cast<double>(dp) / static_cast<double>(df))), 1, 4);
    counted = true;
  } else if (df == 0 && dp >= 4) {
    candidate = ReadInt(g_frame_gen) > 0 ? 2 : 1;
    counted = false;
  } else {
    return;  // too few frames to tell
  }
  if (candidate == g_per_frame) {
    g_pending_windows = 0;
    g_counted = counted;
    return;
  }
  if (candidate != g_pending) g_pending = candidate, g_pending_windows = 0;
  if (++g_pending_windows < 2) return;
  g_per_frame = candidate;
  g_counted = counted;
  g_pending_windows = 0;
  const int shown = ShownLimit(g_mode.load() == Mode::test ? Mode::test : Mode::limit);
  Log(reshade::log::level::info,
      Format("%d shown frame%s per rendered frame (%s)%s.", candidate, candidate == 1 ? "" : "s",
             counted ? "counted" : "from the frame generation setting",
             shown > 0 ? Format(": rendered frames limited to %.2f per second for %d shown", float(shown) / candidate, shown).c_str()
                       : ""));
}

void LogStats(int64_t now) {
  if (!g_log_stats || now - g_stats_logged < 5 * g_qpf) return;
  g_stats_logged = now;
  const Mode mode = g_mode.load();
  Log(reshade::log::level::info,
      Format("Shown %.1f fps, rendered %.1f fps, %d per rendered frame (%s); limit: %s, rendered %.2f; the game's "
             "t.MaxFPS %.2f; rhi.SyncInterval %d; r.FrameGeneration.Method %d.",
             g_shown_fps, g_rendered_fps, g_per_frame, g_counted ? "counted" : "setting",
             mode == Mode::game ? "the game's" : ShownLimitText(mode).c_str(), g_rendered_limit, g_game_value,
             ReadInt(g_sync), ReadInt(g_frame_gen)));
}

void SetMode(Mode mode) {
  g_mode = mode;
  Log(reshade::log::level::info, mode == Mode::game ? "Limit off: the game's own frame-rate option."
                                                    : "Limit on: " + ShownLimitText(mode) + (mode == Mode::test ? " (TestLimit)." : "."));
}

bool KeyPressed(int vk, bool& was_down) {
  if (vk == 0) return false;
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool pressed = down && !was_down;
  was_down = down;
  if (!pressed || (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
    return false;
  DWORD process = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &process);
  return process == GetCurrentProcessId();
}

// Every present, generated frames included (frame generation may present them from another thread).
void OnPresent(command_queue*, swapchain* swapchain, const rect*, const rect*, uint32_t, const rect*) {
  g_presents.fetch_add(1, std::memory_order_relaxed);
  if (!g_active) return;
  std::unique_lock lock(g_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return;
  const int64_t now = Qpc();
  UpdateRefreshRate(static_cast<HWND>(swapchain->get_hwnd()), now);
  if (KeyPressed(g_toggle_key, g_toggle_down)) {
    const Mode mode = g_mode.load();
    SetMode(mode == Mode::limit ? (g_test_limit > 0 ? Mode::test : Mode::game)
                                : mode == Mode::test ? Mode::game : Mode::limit);
  }
  if (now - g_window_start >= g_qpf / 4) Measure(now);
  Apply();
  LogStats(now);
}

// ---------------------------------------------------------------------------------------------
// Overlay

void OnOverlay(effect_runtime*) {
  if (!g_active) {
    ImGui::TextWrapped("Not working: %s", g_status.c_str());
    return;
  }
  std::unique_lock lock(g_mutex);
  const Mode mode = g_mode.load();
  bool on = mode != Mode::game;
  if (ImGui::Checkbox("Limit the frames the display shows", &on)) SetMode(on ? Mode::limit : Mode::game);
  ImGui::SetItemTooltip("Off: the game's own frame-rate option, which limits rendered frames only.\n"
                        "Switches for this session; Enabled in the ini sets it for the next start.");
  bool automatic = g_limit < 0;
  if (ImGui::Checkbox("A little below the display's refresh rate", &automatic)) {
    g_limit = automatic ? -1 : (g_refresh > 3 ? g_refresh - 3 : 120);
    WriteIniInt(L"Limit", g_limit);
  }
  ImGui::SetItemTooltip("3 below: %d at %d Hz. Keeps a G-SYNC or FreeSync display in its variable refresh range.",
                        g_refresh - 3, g_refresh);
  if (!automatic) {
    static int pending = -1;
    if (pending < 0 || !ImGui::IsAnyItemActive()) pending = g_limit;
    ImGui::SliderInt("Shown frames per second", &pending, 0, 480, pending == 0 ? "no limit" : "%d");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      g_limit = pending;
      WriteIniInt(L"Limit", g_limit);
      Log(reshade::log::level::info, "Limit set to " + ShownLimitText(Mode::limit) + ".");
    }
    ImGui::SetItemTooltip("Saved as Limit in the ini. 0 = no limit.");
  }
  ImGui::Spacing();
  ImGui::Text("Now: %.1f frames shown and %.1f rendered per second", g_shown_fps, g_rendered_fps);
  ImGui::Text("Shown per rendered frame: %d (%s)", g_per_frame,
              g_counted ? "counted" : "from the frame generation setting: no game frames counted");
  ImGui::SetItemTooltip("1 without frame generation, 2 with DLSS Frame Generation, 3 or 4 with NVIDIA's multi frame\n"
                        "generation override.");
  if (mode == Mode::game)
    ImGui::Text("Rendered frames: the game's limit (%.2f per second)", g_rendered_limit);
  else if (g_rendered_limit > 0.0f)
    ImGui::Text("Limit: %s, so %.2f rendered", ShownLimitText(mode).c_str(), g_rendered_limit);
  else
    ImGui::Text("Limit: none");
  const int option = static_cast<int>(std::lround(g_game_value / 0.999f));
  ImGui::Text("The game's frame-rate option: %s%s", option > 0 ? (std::to_string(option) + " fps").c_str() : "no limit",
              mode == Mode::game ? "" : " (replaced)");
  ImGui::SetItemTooltip("t.MaxFPS = %.2f. The game's options menu still sets and saves it.", g_game_value);
  if (g_sync != nullptr) {
    if (g_game_sync > 1)
      ImGui::Text("VSync interval: 1 (the game's %d lowered while the limit is on)", g_game_sync);
    else
      ImGui::Text("VSync interval: %d%s", ReadInt(g_sync), ReadInt(g_sync) == 0 ? " (the game's VSync is off)" : "");
  }
  if (g_toggle_key != 0)
    ImGui::Text("Toggle key: virtual-key %d (%s)", g_toggle_key,
                g_test_limit > 0 ? "Limit, TestLimit, the game's" : "this limit or the game's");
  if (mode == Mode::test) ImGui::Text("TestLimit in use: %d shown frames per second", g_test_limit);
}

// ---------------------------------------------------------------------------------------------
// Startup and unload

void Initialize(HMODULE module) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(module, path, MAX_PATH);
  g_ini_path = fs::path(path).parent_path() / "FF7RebirthFrameLimit.ini";
  g_limit = ReadIniInt(L"Limit", -1);
  g_test_limit = std::max(0, ReadIniInt(L"TestLimit", 0));
  g_toggle_key = ReadIniInt(L"ToggleKey", 0);
  g_log_stats = ReadIniInt(L"LogStats", 0) != 0;
  g_mode = ReadIniInt(L"Enabled", 1) != 0 ? Mode::limit : Mode::game;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpf = f.QuadPart;

  const int64_t start = Qpc();
  g_image = code::Sections(GetModuleHandleW(nullptr));
  const wchar_t* const names[3] = {L"t.MaxFPS", L"r.FrameGeneration.Method", L"rhi.SyncInterval"};
  uint8_t* objects[3];
  code::FindAutoVariables(g_image, names, 3, objects);
  if (objects[0] == nullptr) {
    g_status = "t.MaxFPS was not found in the game's executable (another game version?); the game keeps its own limit.";
    Log(reshade::log::level::warning, g_status);
    return;
  }
  g_max_fps = reinterpret_cast<float* const*>(objects[0] + code::kAutoVariableData);
  if (objects[1] != nullptr) g_frame_gen = reinterpret_cast<int32_t* const*>(objects[1] + code::kAutoVariableData);
  if (objects[2] != nullptr) g_sync = reinterpret_cast<int32_t* const*>(objects[2] + code::kAutoVariableData);
  if (*g_max_fps == nullptr) {
    g_status = "t.MaxFPS has no values yet; the game keeps its own limit.";
    Log(reshade::log::level::warning, g_status);
    return;
  }
  const std::vector<uint8_t*> loads = code::FindLoads(g_image, g_max_fps);
  uint8_t* const site = code::FindLimiterLoad(g_image, loads);
  if (site == nullptr) {
    g_status = Format("the engine's frame-rate limiter was not found (%zu reads of t.MaxFPS; another game version?); "
                      "the game keeps its own limit.", loads.size());
    Log(reshade::log::level::warning, g_status);
    return;
  }
  int32_t disp = 0;
  if (!code::MakeLimitValue(site, GameValue(), &g_value) ||
      !code::Displacement(site, code::kLimiterLoadSize, g_value.pointer, &disp)) {
    g_status = "no memory within reach of the engine's frame-rate limiter; the game keeps its own limit.";
    Log(reshade::log::level::warning, g_status);
    return;
  }
  memcpy(&g_limiter_disp, site + code::kLimiterLoadSize - 4, 4);
  if (!code::WriteDisplacement(site, code::kLimiterLoadSize, disp)) {
    g_status = "the engine's frame-rate limiter could not be changed; the game keeps its own limit.";
    Log(reshade::log::level::warning, g_status);
    return;
  }
  g_limiter_load = site;
  g_active = true;

  std::string frames;
  void** const vtable = code::FindVtable(g_image, kHandlerClass);
  void* const function = vtable != nullptr ? vtable[code::kHandleMaxTickRateSlot] : nullptr;
  if (function != nullptr && g_image.text.Contains(function) && code::MakeCallCounter(function, &g_frames) &&
      code::WritePointer(&vtable[code::kHandleMaxTickRateSlot], g_frames.code)) {
    g_handler_slot = &vtable[code::kHandleMaxTickRateSlot];
    g_handler_function = function;
    frames = Format("game frames counted at the Streamline tick-rate handler (vtable rva 0x%X)", g_image.Rva(vtable));
  } else {
    g_frames = {};
    frames = "the Streamline tick-rate handler was not found, so shown frames per rendered frame follow the frame "
             "generation setting (2 when it is on)";
  }
  const double ms = 1000.0 * static_cast<double>(Qpc() - start) / static_cast<double>(g_qpf);
  g_status = "working";
  Log(reshade::log::level::info,
      Format("The engine's frame-rate limiter (rva 0x%X) reads the add-on's limit; %s; r.FrameGeneration.Method %s, "
             "rhi.SyncInterval %s. Limit: %s%s. Found in %.0f ms.",
             g_image.Rva(site), frames.c_str(), g_frame_gen != nullptr ? "found" : "not found",
             g_sync != nullptr ? "found" : "not found", LimitSetting().c_str(),
             g_mode.load() == Mode::game ? " (off: Enabled=0, the game's own limit applies)" : "", ms));
}

// Puts back what was changed in the game's code and values. The routine and the value page stay: a thread may still be
// in them.
void Restore() {
  if (g_handler_slot != nullptr) code::WritePointer(g_handler_slot, g_handler_function);
  g_handler_slot = nullptr;
  if (g_limiter_load != nullptr) code::WriteDisplacement(g_limiter_load, code::kLimiterLoadSize, g_limiter_disp);
  g_limiter_load = nullptr;
  if (g_game_sync > 1 && ReadInt(g_sync) == 1) WriteInt(g_sync, g_game_sync);
  g_game_sync = -1;
  g_active = false;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      Initialize(module);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_overlay(nullptr, OnOverlay);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_overlay(nullptr, OnOverlay);
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_addon(module);
      // ReShade unloads its add-ons with the device while the game keeps running (the game creates a device at startup
      // only to destroy it again): put the game's code back, so a later load finds it as the game has it. Not needed
      // when the process ends.
      if (reserved == nullptr) {
        std::lock_guard lock(g_mutex);
        Restore();
      }
      break;
  }
  return TRUE;
}
