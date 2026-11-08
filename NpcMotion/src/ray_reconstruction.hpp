// DLSS Ray Reconstruction in place of the game's DLSS Super Resolution (docs/ray-reconstruction.md).
//
// The game evaluates super resolution through the DLSS hooks (dlss_probe.cpp). With Ray Reconstruction wanted, the
// hook hands its evaluation here instead: a compute pass turns the base pass's G-buffer into RR's guides (diffuse and
// specular albedo, normals with roughness), and an RR feature, created in the game's own NGX context through the
// driver's _nvngx.dll exports, is evaluated on the same command list with the game's inputs. Anything missing (no
// G-buffer seen, no View matrices yet, RR not available, a failed call) leaves that frame to the game's call.
// The guides pass records plain D3D12 calls on the game's command list right before NGX's evaluation; NGX replaces the
// list's pipeline state and descriptor heaps itself, and the game restores its own after the call.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct NVSDK_NGX_Parameter;

namespace npcmotion::rr {

// The frame's view matrices, as Unreal keeps them in the View uniform buffer (row major, row vectors).
struct Matrices {
  float translated_world_to_view[16];  // View_TranslatedWorldToView
  float view_to_clip[16];              // View_ViewToClip
};

// Where messages go (0 info, 1 warning, 2 error); addon.cpp passes ReShade's log.
void SetLogger(void (*log)(int level, const char* text));
// Wanted for this session (the ini's RayReconstruction, the overlay, the key).
void SetWanted(bool on);
bool wanted();
// Development: shows one guide over DLSS's output (0 off, 1 diffuse albedo, 2 specular albedo, 3 normals, 4 roughness).
void SetGuideView(int guide);
int guide_view();

// The base pass's G-buffer targets A, B and C (native resources) with the formats (DXGI) of their render target views.
void OnGBuffer(ID3D12Resource* a, ID3D12Resource* b, ID3D12Resource* c, uint32_t format_a, uint32_t format_b,
               uint32_t format_c);
// A barrier's states (D3D12_RESOURCE_STATES) for a resource; the new one is kept if it is one of the G-buffer targets.
void OnBarrier(const void* resource, uint32_t old_state, uint32_t new_state);

// Development: logs the next `lines` G-buffer binds and barriers, presents and evaluations with thread and time
// (armed when Ray Reconstruction is switched on), to see the order the game records them in.
void Trace(int lines);
void TraceMarker(const char* what);

// From the DLSS hook in place of the game's evaluation: true when Ray Reconstruction ran (the game's call is then
// skipped), false to leave this frame to the game's super resolution. `pre_exposure` is the value the hook passes.
bool Evaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* game, float pre_exposure, const Matrices* matrices);
// The game created its super resolution feature (again): Ray Reconstruction follows its sizes at the next evaluation.
void OnGameCreate();

// One line for the overlay and the log.
std::string Status();
// Releases the RR feature, its parameters and the D3D12 objects (the add-on unloads while the game runs).
void Shutdown();

}  // namespace npcmotion::rr
