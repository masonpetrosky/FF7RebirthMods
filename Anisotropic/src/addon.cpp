// FF7 Rebirth Anisotropic Filtering
// ReShade add-on that sets the anisotropic filtering level of FINAL FANTASY VII REBIRTH's texture samplers.
//
// The game has no anisotropic filtering option, and its config files do not set r.MaxAnisotropy, so texture samplers
// get Unreal's default of 4x. So do the two samplers Square Enix added to the View uniform buffer, through which the
// materials sample their textures (SharedAnisotropic4XWrappedSampler and SharedAnisotropic4XClampedSampler). Floors
// and roads seen at a shallow angle therefore blur a short way from the camera. In game, every anisotropic sampler
// asks for 4x except one at 8x and one at 1x. The add-on gives the anisotropic samplers the game creates, in
// descriptor heaps and as static samplers in root signatures, the level set by MaxAnisotropy (16 by default); samplers
// the game creates at 1x (anisotropic filtering off) and samplers without anisotropic filtering are left alone. The
// game creates its samplers when it starts, so the level applies at startup.
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

extern "C" __declspec(dllexport) const char* NAME = "FF7 Rebirth Anisotropic Filtering";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Raises the anisotropic filtering of FINAL FANTASY VII REBIRTH's textures from the game's fixed 4x to 16x.";

using namespace reshade::api;

namespace {

namespace fs = std::filesystem;

constexpr int kDefaultLevel = 16;
constexpr int kLevels[] = {0, 2, 4, 8, 16};  // offered by the overlay; 0 = leave the game's samplers alone

fs::path g_ini_path;
int g_level = kDefaultLevel;       // MaxAnisotropy when the add-on loaded: in effect this session
int g_next_level = kDefaultLevel;  // MaxAnisotropy in the ini now; the overlay changes it for the next start

// An anisotropic sampler as the game asked for it.
struct SamplerKey {
  uint32_t filter, address_u, address_v, address_w, compare;
  float max_anisotropy, mip_lod_bias, min_lod, max_lod;

  auto Tie() const {
    return std::tie(filter, address_u, address_v, address_w, compare, max_anisotropy, mip_lod_bias, min_lod, max_lod);
  }
  bool operator<(const SamplerKey& other) const { return Tie() < other.Tie(); }
};
std::mutex g_samplers_mutex;
std::map<SamplerKey, uint32_t> g_anisotropic;  // creations per description, descriptor heaps and root signatures
std::atomic<uint32_t> g_changed{0};             // anisotropic samplers given the add-on's level
std::atomic<uint32_t> g_other{0};               // samplers without anisotropic filtering, left alone
std::atomic<uint32_t> g_static{0};              // anisotropic static samplers in the root signatures created

void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("[Anisotropic] " + message).c_str());
}

int ReadIniInt(const wchar_t* key, int fallback) {
  return static_cast<int>(GetPrivateProfileIntW(L"Anisotropic", key, fallback, g_ini_path.c_str()));
}

void WriteIniInt(const wchar_t* key, int value) {
  WritePrivateProfileStringW(L"Anisotropic", key, std::to_wstring(value).c_str(), g_ini_path.c_str());
}

// D3D12_ANISOTROPIC_FILTERING_BIT: set in every anisotropic filter, with or without comparison or min/max reduction.
bool IsAnisotropic(filter_mode filter) { return (static_cast<uint32_t>(filter) & 0x40) != 0; }

const char* AddressName(uint32_t mode) {
  switch (static_cast<texture_address_mode>(mode)) {
    case texture_address_mode::wrap: return "wrap";
    case texture_address_mode::mirror: return "mirror";
    case texture_address_mode::clamp: return "clamp";
    case texture_address_mode::border: return "border";
    case texture_address_mode::mirror_once: return "mirror-once";
  }
  return "?";
}

std::string Describe(const SamplerKey& key) {
  char text[128];
  snprintf(text, sizeof(text), "%gx, %s/%s/%s, mip bias %+.2f, filter 0x%X", key.max_anisotropy, AddressName(key.address_u),
           AddressName(key.address_v), AddressName(key.address_w), key.mip_lod_bias, key.filter);
  return text;
}

std::string LevelName(int level) { return level > 0 ? std::to_string(level) + "x" : "the game's own"; }

bool OnCreateSampler(device*, sampler_desc& desc) {
  if (!IsAnisotropic(desc.filter)) {
    ++g_other;
    return false;
  }
  const SamplerKey key{static_cast<uint32_t>(desc.filter),    static_cast<uint32_t>(desc.address_u),
                       static_cast<uint32_t>(desc.address_v), static_cast<uint32_t>(desc.address_w),
                       static_cast<uint32_t>(desc.compare_op), desc.max_anisotropy,
                       desc.mip_lod_bias,                     desc.min_lod,
                       desc.max_lod};
  bool first;
  {
    std::lock_guard lock(g_samplers_mutex);
    first = g_anisotropic[key]++ == 0;
  }
  // At 1x the game has turned anisotropic filtering off for the sampler.
  const bool change = g_level > 0 && desc.max_anisotropy >= 2.0f && desc.max_anisotropy != static_cast<float>(g_level);
  if (first)
    Log(reshade::log::level::info,
        "The game asked for " + Describe(key) + (change ? "; set to " + LevelName(g_level) + "." : "; left alone."));
  if (!change) return false;
  desc.max_anisotropy = static_cast<float>(g_level);
  ++g_changed;
  return true;
}

// Static samplers go through create_sampler too, before their root signature is created; this only counts them.
void OnInitPipelineLayout(device*, uint32_t param_count, const pipeline_layout_param* params, pipeline_layout) {
  uint32_t found = 0;
  for (uint32_t i = 0; i < param_count; ++i) {
    if (params[i].type != pipeline_layout_param_type::descriptor_table_with_flags &&
        params[i].type != pipeline_layout_param_type::push_descriptors_with_ranges_and_flags)
      continue;
    const auto& table = params[i].descriptor_table_with_flags;
    for (uint32_t r = 0; r < table.count; ++r) {
      const descriptor_range_with_flags& range = table.ranges[r];
      if (range.static_samplers == nullptr) continue;
      for (uint32_t s = 0; s < range.count; ++s)
        if (IsAnisotropic(range.static_samplers[s].filter)) ++found;
    }
  }
  if (found != 0 && g_static.fetch_add(found) == 0)
    Log(reshade::log::level::info, "Root signatures carry anisotropic static samplers too; they get the same level.");
}

void OnOverlay(effect_runtime*) {
  ImGui::Text("Anisotropic filtering this session: %s", LevelName(g_level).c_str());
  if (ImGui::BeginCombo("Level from the next start", LevelName(g_next_level).c_str())) {
    for (int level : kLevels) {
      if (ImGui::Selectable(LevelName(level).c_str(), level == g_next_level)) {
        g_next_level = level;
        WriteIniInt(L"MaxAnisotropy", level);
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Saved as MaxAnisotropy in the ini. The game creates its samplers when it starts,\n"
                        "so a new level applies the next time the game starts.");
  if (g_next_level != g_level) ImGui::Text("Restart the game to switch to %s.", LevelName(g_next_level).c_str());

  ImGui::Spacing();
  std::vector<std::pair<SamplerKey, uint32_t>> samplers;
  {
    std::lock_guard lock(g_samplers_mutex);
    samplers.assign(g_anisotropic.begin(), g_anisotropic.end());
  }
  ImGui::Text("Anisotropic samplers the game asked for (%u created with %s):", g_changed.load(),
              g_level > 0 ? LevelName(g_level).c_str() : "a level of their own");
  for (const auto& [key, count] : samplers) ImGui::BulletText("%s (created %u times)", Describe(key).c_str(), count);
  if (samplers.empty()) ImGui::BulletText("none yet");
  if (g_static != 0) ImGui::Text("Of them static samplers in root signatures: %u", g_static.load());
  ImGui::Text("Other samplers, left alone: %u", g_other.load());
}

void Initialize(HMODULE module) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(module, path, MAX_PATH);
  g_ini_path = fs::path(path).parent_path() / "FF7RebirthAnisotropic.ini";
  const int level = ReadIniInt(L"MaxAnisotropy", kDefaultLevel);
  g_level = level <= 0 ? 0 : std::min(level, 16);
  g_next_level = g_level;
  Log(reshade::log::level::info, g_level > 0 ? "Giving the game's anisotropic samplers " + LevelName(g_level) + "."
                                             : "MaxAnisotropy=0: leaving the game's samplers alone.");
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      Initialize(module);
      reshade::register_event<reshade::addon_event::create_sampler>(OnCreateSampler);
      reshade::register_event<reshade::addon_event::init_pipeline_layout>(OnInitPipelineLayout);
      reshade::register_overlay(nullptr, OnOverlay);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_overlay(nullptr, OnOverlay);
      reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
