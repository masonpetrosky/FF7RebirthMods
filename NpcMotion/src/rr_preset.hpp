// Which DLSS Ray Reconstruction model to ask for (docs/ray-reconstruction.md, "Presets"): the ini's
// RayReconstructionPreset, or by the graphics card when it is Auto. Preset F (DLSS 4.5's "RR2") runs its network in
// FP8, which only RTX 40 (Ada) and newer do in hardware: reviewers measured it up to 29% slower on an RTX 2080 Ti and
// about 19% on an RTX 3090 against DLSS 4. RTX 20 (Turing) and RTX 30 (Ampere) cards get preset D, DLSS 4's model.
// Preset E is a variant of D for games with a depth-of-field guide (this game gives none), offered as a choice only.
// Header-only and free of NGX, so tests/rr_preset_test.cpp checks it on card names.
// SPDX-License-Identifier: MIT

#pragma once

#include <cctype>
#include <cstdint>
#include <string>

namespace npcmotion::rr {

// NVSDK_NGX_RayReconstruction_Hint_Render_Preset values (nvsdk_ngx_defs_dlssd.h, SDK 310.9.16).
constexpr uint32_t kPresetAuto = 0, kPresetD = 4, kPresetE = 5, kPresetF = 6;

// The ini's text: "Auto" (or empty, or anything else), "D", "E" or "F" (letters in either case).
inline uint32_t PresetFromText(const std::string& text) {
  if (text.size() != 1) return kPresetAuto;
  switch (std::toupper(static_cast<unsigned char>(text[0]))) {
    case 'D': return kPresetD;
    case 'E': return kPresetE;
    case 'F': return kPresetF;
    default: return kPresetAuto;
  }
}

inline char PresetLetter(uint32_t preset) {
  return preset == kPresetD ? 'D' : preset == kPresetE ? 'E' : preset == kPresetF ? 'F' : '?';
}

// Preset F for an RTX 40 or newer card, D for an RTX 20 or 30 (no FP8 in hardware); `why` says which rule chose it.
// The card's name as DXGI gives it: "NVIDIA GeForce RTX 3080", "NVIDIA GeForce RTX 4060 Laptop GPU", "NVIDIA RTX A4000"
// (Ampere), "Quadro RTX 6000" (Turing), "NVIDIA RTX 2000 Ada Generation", "NVIDIA RTX PRO 6000 Blackwell ...".
// Anything else gets F, NVIDIA's default model.
inline uint32_t AutoPreset(const std::string& adapter, std::string* why) {
  std::string name;
  for (const char c : adapter) name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto says = [&](const char* word) { return name.find(word) != std::string::npos; };
  if (says("ada") || says("blackwell") || says("rtx pro")) {
    *why = "RTX 40 or newer (Ada or Blackwell), FP8 in hardware";
    return kPresetF;
  }
  if (says("quadro rtx") || says("titan rtx")) {
    *why = "RTX 20 generation (Turing), no FP8 in hardware";
    return kPresetD;
  }
  const size_t rtx = name.find("rtx ");
  if (rtx != std::string::npos) {
    size_t at = rtx + 4;
    if (at + 1 < name.size() && name[at] == 'a' && std::isdigit(static_cast<unsigned char>(name[at + 1]))) {
      *why = "RTX A series (Ampere), no FP8 in hardware";
      return kPresetD;
    }
    size_t digits = 0;
    while (at + digits < name.size() && std::isdigit(static_cast<unsigned char>(name[at + digits]))) ++digits;
    if (digits == 4) {
      const int generation = name[at] - '0';
      if (generation == 2 || generation == 3) {
        *why = "RTX " + std::to_string(generation) + "0 series, no FP8 in hardware";
        return kPresetD;
      }
      if (generation >= 4) {
        *why = "RTX " + std::to_string(generation) + "0 series, FP8 in hardware";
        return kPresetF;
      }
    }
  }
  *why = "not a card the rule knows: NVIDIA's default model";
  return kPresetF;
}

}  // namespace npcmotion::rr
