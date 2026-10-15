// Hooks the game's calls into NGX (DLSS) for development and small corrections: logs what the game hands to DLSS
// when it creates and evaluates the super resolution feature (with the View values the frame was rendered with), and
// can pass the engine's pre-exposure, which the game leaves at 1.
// The game links NGX's static library, which resolves NVSDK_NGX_D3D12_CreateFeature and EvaluateFeature from the
// driver's _nvngx.dll and keeps the pointers in its data section; the hooks swap those pointers for wrappers.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace npcmotion::dlss_probe {

// View uniform buffer values of a frame, read at its first skinned draw.
struct ViewValues {
  float jitter[4] = {};      // View_TemporalAAJitter: current xy, previous zw (clip units)
  float taa_params[4] = {};  // View_TemporalAAParams: index, sequence length, jitter in pixels
  float mip_bias = 0;        // View_MaterialTextureMipBias
  float pre_exposure = 0;    // View_PreExposure
  float sampler_bias[4] = {};  // View_TemporalSamplerBias: .y is the bias the game's materials sample with
};

// Installs the hooks once NGX is loaded. `log_path` empty = no logging.
void Initialize(const std::filesystem::path& log_path, bool pass_pre_exposure);
// Puts the game's NGX pointers back and closes the log (before the add-on is unloaded).
void Shutdown();
bool enabled();   // hooks wanted (logging or pre-exposure)
bool logging();   // the probe log is on
// Once per presented frame.
void OnPresent(uint64_t frame);
// Logs the next `evaluations` DLSS evaluations in full.
void Arm(uint32_t evaluations);
void SetViewValues(uint64_t frame, const ViewValues& values);
bool HasViewValues(uint64_t frame);
ViewValues LatestViewValues();
// Why the View values of a frame could not be read (counted, shown in the log).
enum class ViewFailure { kNoReflection, kNoRootParameter, kNotBound, kMapFailed, kCount };
void CountViewFailure(ViewFailure reason);
// Live console variables shown in the log (nullptr when not found).
void SetConsoleVariables(const int32_t* temporal_aa_samples, const int32_t* temporal_aa_method);
void SetPassPreExposure(bool pass);
bool pass_pre_exposure();
std::string Status();

}  // namespace npcmotion::dlss_probe
