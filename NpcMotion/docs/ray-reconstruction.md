# DLSS Ray Reconstruction in place of Super Resolution (plan, 2026-11-08)

## Goal

Let NPC Motion run DLSS Ray Reconstruction (RR) instead of the game's DLSS Super Resolution (SR), switchable live, so
that the random per-pixel noise of screen-space effects is denoised instead of upscaled: Shader Injector's one-ray SSGI
and SSR and its contact shadows. SR treats that noise as detail or rejects it, so it boils (2026-11-08 at the Costa del
Sol bar: `captures/noise/2026-11-08-costa-bar*`, `tools/noise_session.py`). If RR cleans it, Shader Injector's
stronger reflections (its x100000 SSR weight, cut to x100 that day to stop the boiling) may come back.

## Why it can work here

- The game ships RR: `Engine/Plugins/StreamlineSubset/Binaries/ThirdParty/Win64/nvngx_dlssd.dll` 310.3.0 (with
  `sl.dlss_d.dll`), unused by the game itself.
- NPC Motion already wraps the game's NGX `CreateFeature` and `EvaluateFeature` (`src/dlss_probe.cpp`) and sees every
  input of the SR evaluation (colour, output, depth, motion vectors, jitter, reset, scales, pre-exposure).
- The driver's `_nvngx.dll` exports `NVSDK_NGX_D3D12_AllocateParameters`, `GetCapabilityParameters`,
  `CreateFeature`, `EvaluateFeature`, `ReleaseFeature` and `DestroyParameters`: the add-on can create an RR feature in
  the game's NGX context with the SDK's headers only (no static library, no second `Init`).
- `Persona3ReloadDLSS` (Unreal 4.27) has a working RR integration (`src/dlss.cpp`, `src/rr_guides.hlsl`). There RR cut
  foliage flicker about 3x (temporal deviation 1.12-1.28 -> 0.41-0.43) and cost about +1.9 ms of GPU time over SR at
  4K Performance on the same PC (RTX 5070 Ti); flat textures about 10% softer.

## Design

1. **Evaluation.** In `EvaluateFeatureHook`, with RR on, the game's SR call is replaced by: our guide pass, then RR's
   `EvaluateFeature` on the same command list with the game's inputs (colour, output, depth, motion vectors, jitter,
   motion vector scale, reset, render subrect, pre-exposure as NPC Motion passes it, frame time), our guides and the
   view matrices. Any failure falls back to the game's SR call for that frame (logged once, counted).
2. **Feature.** Created at the first RR evaluation and again when the output size, the render size limits or the mode
   change, with the create flags of the game's SR feature (as `CreateFeatureHook` sees them), denoise mode DLUnified,
   roughness packed in the normals' .w, hardware depth, NVIDIA's default RR preset. Released when RR is switched off
   for a while, at shutdown and before the game's NGX goes away.
3. **Guides** (port of `rr_guides.hlsl`, with this game's G-buffer as Shader Injector's 1.0.0.5 decoder reads it):
   GBufferA = normal x 0.5 + 0.5 and, in .a, the packed specular; GBufferB = metallic, -, roughness, shading model
   (low 4 bits of .a); GBufferC = base colour, material AO. Diffuse albedo = base colour x (1 - metallic); specular
   albedo = EnvBRDFApprox2(F0, roughness^2, N.V) with the fork's F0: dielectric = clamp(s < 2/3 ? 0.06 s : 0.36 s -
   0.20, 0, 0.16) of s = GBufferA.a, F0 = dielectric + metallic x (base colour - dielectric); world normal with the
   linear roughness in .w. Unlit pixels (shading model 0: sky, cleared) get NVIDIA's defaults. Hair (shading model
   7) keeps a real normal in GBufferA in this game (its strand direction is in GBufferD, unlike stock Unreal 4) and
   the game's fixed roughness 0.48; skin's roughness x 0.93655 and the eye's metallic 0, as the game decodes them.
4. **G-buffer.** Found at the base pass: the render target set with the scene colour, GBufferA-E and velocity,
   recognised by count, size and formats (logged on first sight). States followed through barriers as in P3R; the
   guide pass reads them at the evaluation.
5. **Matrices.** TranslatedWorldToView (View rows c20-c23) and ViewToClip (c36-c39) join the View values NPC Motion
   already reads every frame (`CaptureViewValues`); layout from Shader Injector's 1.0.0.5 includes.
6. **Jitter.** RR wants at least 32 jitter phases: `JitterPhases=32` is already the user's setting.
7. **Settings.** `RayReconstruction` (0/1, overlay checkbox), `RayReconstructionKey` (0 = none; F13 = 124 for scripted
   A/B tests, a key the user's keyboard doesn't have); counters and the RR status in the overlay and ReShade.log.

## Stages and checks

Stage 1, development build, one game session at the Costa del Sol bar (install needs the user's OK):

- [ ] The RR feature is created in the game's NGX context (log: success). If NGX says the feature isn't found,
      `nvngx_dlssd.dll` isn't on the game's NGX search path: put the game's own copy next to the executable (game
      folder change: OK + backup) and retry.
- [ ] The base pass's G-buffer is found (log: formats and sizes); guides dumped once and checked offline: diffuse
      albedo = base colour on non-metals, unit normals, plausible roughness, hair normals smooth over the head.
- [ ] With RR on, RR runs every frame with 0 fallbacks; switching back to SR works (reset on each switch); no NGX
      errors in ReShade.log; frame generation still works.
- [ ] Noise A/B with the RR key (noise_session-style bursts, A/B/A): floor flicker p99 with RR at most SR's; dots on
      hair not worse; no new artifacts on the soft-edged hair fringe, character outlines, fog, transparency, HUD.
- [ ] Shader Injector's SSR temporarily back at x100000 (its menu): RR's floor flicker p99 within 20% of SR's at x100,
      and the glossy window reflections kept. If so, recommend the stronger reflections with RR.
- [ ] Cost: PresentMon GPU busy, RR vs SR at the same spot (expected +1.5-2.5 ms at 4K Performance).
- [ ] Sharpness: Laplacian energy of flat textures, RR vs SR; then the user's eye.

Stage 2: user-facing setting and defaults from stage 1, docs (README, shipped ini, release notes), release with the
user's OK.

## Risks

- RR is trained on ray-traced noise. Screen-space noise looks alike per pixel, but its disocclusions differ (P3R mixed
  RR with Unreal's screen-space AO without trouble).
- The soft-edged hair fringe (Vegetation and Hair Fix) is blended into the colour before DLSS while the guides show
  only the opaque core: RR may smear or drop the fringe.
- RR replaces SR's preset: the user's SR preset choice doesn't apply under RR.
- A second NGX feature in the game's context: release it before the game's NGX shuts down (device loss, exit).
