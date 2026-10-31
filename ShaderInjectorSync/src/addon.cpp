// FF7 Rebirth Shader Injector Sync
// ReShade add-on that holds FINAL FANTASY VII REBIRTH at startup until Shader Injector has hooked Direct3D 12.
//
// Shader Injector (frostbone25's lighting overhaul, which the game loads as dsound.dll) sleeps 5 seconds after it loads
// before it installs its Direct3D 12 hooks. From a cold start the game takes longer than that to create its device. When
// it starts again from a warm file cache, for example right after it was closed, it creates its device within about two
// seconds and its pipeline library right after, before those hooks exist. Shader Injector hooks a pipeline library's
// functions only when it sees CreatePipelineLibrary, so every pipeline the game then loads from its library, the
// lighting, water and SSGI ones among them, bypasses it, and its shaders do not replace the game's for the whole session
// (its log then reports "no capture attempt" for most of its enabled replacements).
//
// The add-on holds the game when it creates its first command queue, until Shader Injector's log says its startup is
// complete, at most WaitSeconds. It cannot hold the game earlier: ReShade keeps a lock for all of D3D12CreateDevice, and
// Shader Injector creates a throwaway device of its own before it hooks, so a hold in create_device or init_device keeps
// Shader Injector waiting for as long as the hold lasts. Once Shader Injector has hooked Direct3D 12, the add-on creates
// an empty pipeline library on the game's device. All pipeline libraries are the same class (ReShade's wrapper, or the
// runtime's own), so Shader Injector then hooks the functions the game's library uses too. Shader Injector's own
// throwaway objects are never held, and without Shader Injector the add-on does nothing.
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

extern "C" __declspec(dllexport) const char* NAME = "FF7 Rebirth Shader Injector Sync";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Holds FINAL FANTASY VII REBIRTH at startup until Shader Injector has hooked Direct3D 12, so its shaders replace "
    "the game's even when the game starts quickly.";

using namespace reshade::api;

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr int kDefaultWaitSeconds = 15;
// Shader Injector 2.2.1 writes the first line at the very end of its startup, after it has enabled its hooks and released
// its throwaway device, queue, command list and swap chain (ReShade keeps the device's queues in a list without a lock,
// so the game must not create queues while Shader Injector still releases its own). The others mean it gave up.
constexpr const char* kReadyMarker = "Hooks->CleanupDummyObjects: complete";
constexpr const char* kFailedMarkers[] = {"Hooks->Initialize: Failed to create dummy device",
                                          "Hooks->Initialize: MH_EnableHook failed"};

fs::path g_ini_path;
int g_wait_seconds = kDefaultWaitSeconds;
std::atomic<bool> g_device_seen{false};
std::atomic<bool> g_queue_seen{false};
double g_device_seconds = 0.0;  // seconds from process start to the game's first Direct3D 12 device
std::string g_status;           // what happened at startup, for the overlay; written once, before g_status_set
std::atomic<bool> g_status_set{false};

void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("[ShaderInjectorSync] " + message).c_str());
}

void SetStatus(reshade::log::level level, const std::string& message) {
  Log(level, message);
  g_status = message;
  g_status_set.store(true, std::memory_order_release);
}

std::string Seconds(double seconds) {
  char text[32];
  snprintf(text, sizeof(text), "%.1f s", seconds);
  return text;
}

FILETIME ProcessStart() {
  FILETIME creation, exit_time, kernel_time, user_time;
  GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel_time, &user_time);
  return creation;
}

double SecondsSinceProcessStart() {
  const FILETIME start = ProcessStart();
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  const auto ticks = [](const FILETIME& t) { return (static_cast<int64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  return static_cast<double>(ticks(now) - ticks(start)) / 1e7;
}

bool AddressInModule(const void* address, HMODULE module) {
  const auto base = reinterpret_cast<const BYTE*>(module);
  const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const auto p = static_cast<const BYTE*>(address);
  return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
}

// Shader Injector creates a device and a queue of its own to find the functions it hooks; those must never be held.
bool CalledFrom(HMODULE module) {
  void* frames[64];
  const USHORT count = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
  for (USHORT i = 0; i < count; ++i)
    if (AddressInModule(frames[i], module)) return true;
  return false;
}

// Reads Shader Injector's log if this process has written to it (it starts a new log when it loads).
bool ReadLogOfThisSession(const fs::path& path, const FILETIME& process_start, std::string& text) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  FILETIME write_time{};
  LARGE_INTEGER size{};
  bool ok = GetFileTime(file, nullptr, nullptr, &write_time) && CompareFileTime(&write_time, &process_start) >= 0 &&
            GetFileSizeEx(file, &size) && size.QuadPart < (64ll << 20);
  if (ok) {
    text.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ok = text.empty() || ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) != FALSE;
    text.resize(read);
  }
  CloseHandle(file);
  return ok;
}

enum class Wait { ready, failed, timed_out };

Wait WaitForShaderInjector(const fs::path& log_path, double& waited) {
  const FILETIME process_start = ProcessStart();
  const Clock::time_point start = Clock::now();
  const Clock::time_point deadline = start + std::chrono::seconds(g_wait_seconds);
  std::string text;
  for (;;) {
    if (ReadLogOfThisSession(log_path, process_start, text)) {
      waited = std::chrono::duration<double>(Clock::now() - start).count();
      if (text.find(kReadyMarker) != std::string::npos) return Wait::ready;
      for (const char* marker : kFailedMarkers)
        if (text.find(marker) != std::string::npos) return Wait::failed;
    }
    if (Clock::now() >= deadline) {
      waited = std::chrono::duration<double>(Clock::now() - start).count();
      return Wait::timed_out;
    }
    Sleep(25);
  }
}

// Creates an empty pipeline library on the game's device as the game sees it (through ReShade, D3D12CreateDevice returns
// the existing device of an adapter, wrapped the way the game got it), so that Shader Injector, whose CreatePipelineLibrary
// hook sits on that wrapper, hooks the pipeline library functions.
bool CreatePipelineLibraryOnGameDevice(ID3D12Device* native) {
  const LUID luid = native->GetAdapterLuid();
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
  const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
  const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  const auto create_factory = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
  const auto create_device = d3d12 ? reinterpret_cast<CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
  if (create_device == nullptr) return false;

  IDXGIAdapter1* adapter = nullptr;  // the game's adapter; without it, the default one
  IDXGIFactory4* factory = nullptr;
  if (create_factory != nullptr && SUCCEEDED(create_factory(IID_PPV_ARGS(&factory)))) {
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) adapter = nullptr;
    factory->Release();
  }
  ID3D12Device1* device1 = nullptr;
  const HRESULT hr = create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device1));
  if (adapter != nullptr) adapter->Release();
  if (FAILED(hr) || device1 == nullptr) return false;

  bool created = false;
  const LUID device_luid = device1->GetAdapterLuid();
  if (device_luid.LowPart == luid.LowPart && device_luid.HighPart == luid.HighPart) {
    ID3D12PipelineLibrary* library = nullptr;
    if (SUCCEEDED(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library))) && library != nullptr) {
      library->Release();
      created = true;
    }
  }
  device1->Release();
  return created;
}

void OnInitDevice(device* device) {
  if (device->get_api() != device_api::d3d12) return;
  const HMODULE injector = GetModuleHandleW(L"dsound.dll");
  if (injector != nullptr && CalledFrom(injector)) return;
  if (!g_device_seen.exchange(true)) g_device_seconds = SecondsSinceProcessStart();
}

void OnInitCommandQueue(command_queue* queue) {
  device* const device = queue->get_device();
  if (device->get_api() != device_api::d3d12) return;
  const HMODULE injector = GetModuleHandleW(L"dsound.dll");
  if (injector != nullptr && CalledFrom(injector)) return;
  if (g_queue_seen.exchange(true)) return;

  const std::string created = "The game created its Direct3D 12 device " + Seconds(g_device_seconds) +
                              " and its first command queue " + Seconds(SecondsSinceProcessStart()) + " after it started. ";
  if (injector == nullptr) {
    SetStatus(reshade::log::level::info, created + "Shader Injector (dsound.dll) is not loaded, so there is nothing to wait for.");
    return;
  }
  wchar_t module_path[MAX_PATH];
  GetModuleFileNameW(injector, module_path, MAX_PATH);
  const fs::path log_path = fs::path(module_path).parent_path() / L"ShaderInjector" / L"Logs" / L"ShaderInjector.log";
  std::error_code error;
  if (!fs::is_directory(log_path.parent_path(), error)) {
    SetStatus(reshade::log::level::info, created + "The dsound.dll here has no ShaderInjector\\Logs folder next to it, so it "
                                                   "is not Shader Injector; nothing to wait for.");
    return;
  }
  if (g_wait_seconds <= 0) {
    SetStatus(reshade::log::level::info, created + "WaitSeconds=0: not waiting for Shader Injector.");
    return;
  }

  double waited = 0.0;
  switch (WaitForShaderInjector(log_path, waited)) {
    case Wait::ready: {
      const std::string held = waited < 0.05 ? "Shader Injector had already hooked Direct3D 12, so it did not have to wait"
                                             : "Held it " + Seconds(waited) + " until Shader Injector had hooked Direct3D 12";
      if (CreatePipelineLibraryOnGameDevice(reinterpret_cast<ID3D12Device*>(device->get_native())))
        SetStatus(reshade::log::level::info, created + held + ", then had Shader Injector hook the pipeline libraries.");
      else
        SetStatus(reshade::log::level::warning, created + held + ", but could not create a pipeline library on the game's "
                                                                 "device, so pipelines from the game's library may bypass it.");
      break;
    }
    case Wait::failed:
      SetStatus(reshade::log::level::warning, created + "Shader Injector's log says its hooks failed, so the game goes on "
                                                        "without waiting.");
      break;
    case Wait::timed_out:
      SetStatus(reshade::log::level::warning,
                created + "Shader Injector had not finished hooking Direct3D 12 after " + std::to_string(g_wait_seconds) +
                    " s (WaitSeconds), so the game goes on; some of its shaders may stay the game's this session.");
      break;
  }
}

void OnOverlay(effect_runtime*) {
  if (g_status_set.load(std::memory_order_acquire))
    ImGui::TextWrapped("%s", g_status.c_str());
  else
    ImGui::TextUnformatted("The game has not created its first Direct3D 12 command queue yet.");
  ImGui::Text("Longest wait: %d s (WaitSeconds in the ini).", g_wait_seconds);
}

void Initialize(HMODULE module) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(module, path, MAX_PATH);
  g_ini_path = fs::path(path).parent_path() / "FF7RebirthShaderInjectorSync.ini";
  g_wait_seconds = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"ShaderInjectorSync", L"WaitSeconds",
                                                                     kDefaultWaitSeconds, g_ini_path.c_str())),
                              0, 120);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      Initialize(module);
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
      reshade::register_overlay(nullptr, OnOverlay);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_overlay(nullptr, OnOverlay);
      reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
