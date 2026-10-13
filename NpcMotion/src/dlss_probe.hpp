// Development: logs what the game hands to DLSS (NGX) when it creates and evaluates the super resolution feature,
// and the projection jitter the frame was rendered with, to check the integration (e.g. for preset M).
// The game links NGX's static library, which resolves NVSDK_NGX_D3D12_CreateFeature and EvaluateFeature from the
// driver's _nvngx.dll and keeps the pointers in its data section; the probe swaps those pointers for wrappers.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace npcmotion::dlss_probe {

// Turns the probe on; the log goes to `log_path`.
void Initialize(const std::filesystem::path& log_path);
bool enabled();
// Once per presented frame: installs the wrappers as soon as NGX is loaded.
void OnPresent(uint64_t frame);
// Logs the next `evaluations` DLSS evaluations in full.
void Arm(uint32_t evaluations);
// The frame's projection jitter as the engine rendered it: View_TemporalAAJitter (current xy, previous zw, clip units).
void SetProjectionJitter(uint64_t frame, const float jitter[4]);
bool HasProjectionJitter(uint64_t frame);
std::string Status();

}  // namespace npcmotion::dlss_probe
