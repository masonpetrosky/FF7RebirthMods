// What every one of our add-ons does the same way: its log lines, its settings file and its keys. Each add-on calls
// ff7r::InitAddon(module, "<Name>") first thing when it loads: its log lines are tagged [<Name>], its settings are
// FF7Rebirth<Name>.ini next to the add-on, section [<Name>].
// The add-on includes <reshade.hpp> before this header (and <imgui.h> before that, with ImTextureID defined: ReShade
// sets up its overlay functions only when it sees ImGui).
// SPDX-License-Identifier: MIT
#pragma once

#ifndef RESHADE_API_VERSION
#error "include <reshade.hpp> (after <imgui.h>) before common/addon.hpp"
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>

namespace ff7r {

// The add-on these helpers serve (one per add-on file).
struct Addon {
  std::string tag;               // the log lines' prefix: "[<Name>] "
  std::wstring section;          // the settings' section: <Name>
  std::filesystem::path folder;  // the add-on file's folder
  std::filesystem::path ini;     // its settings: FF7Rebirth<Name>.ini in that folder
};

inline Addon& ThisAddon() {
  static Addon addon;
  return addon;
}

inline void InitAddon(HMODULE module, const char* name) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(module, path, MAX_PATH);
  Addon& addon = ThisAddon();
  addon.tag = std::string("[") + name + "] ";
  addon.section = std::wstring(name, name + strlen(name));
  addon.folder = std::filesystem::path(path).parent_path();
  addon.ini = addon.folder / (L"FF7Rebirth" + addon.section + L".ini");
}

inline void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, (ThisAddon().tag + message).c_str());
}

inline int ReadIniInt(const wchar_t* key, int fallback) {
  const Addon& addon = ThisAddon();
  return static_cast<int>(GetPrivateProfileIntW(addon.section.c_str(), key, fallback, addon.ini.c_str()));
}

inline void WriteIniInt(const wchar_t* key, int value) {
  const Addon& addon = ThisAddon();
  WritePrivateProfileStringW(addon.section.c_str(), key, std::to_wstring(value).c_str(), addon.ini.c_str());
}

// `fallback` when the key is missing or not a finite number.
inline float ReadIniFloat(const wchar_t* key, float fallback) {
  const Addon& addon = ThisAddon();
  wchar_t text[32] = {};
  GetPrivateProfileStringW(addon.section.c_str(), key, L"", text, 32, addon.ini.c_str());
  wchar_t* end = nullptr;
  const float value = wcstof(text, &end);
  return end != text && std::isfinite(value) ? value : fallback;
}

// With two decimals.
inline void WriteIniFloat(const wchar_t* key, float value) {
  const Addon& addon = ThisAddon();
  wchar_t text[32];
  swprintf(text, 32, L"%.2f", value);
  WritePrivateProfileStringW(addon.section.c_str(), key, text, addon.ini.c_str());
}

// Whether the key `vk` (a virtual-key code; 0 = none) went down since the last call with the same `was_down`. A key
// counts only while the game's window is in front and neither Alt nor Ctrl is held (Alt+F4 closes the game).
inline bool KeyPressed(int vk, bool& was_down) {
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

}  // namespace ff7r
