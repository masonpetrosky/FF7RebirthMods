# FF7 Rebirth: NPC motion vectors, investigation log

Question (2026-10-12): in Kalm, do NPCs get motion vectors with DLSS? The Elden Ring add-on had to generate them
because that game draws none for enemies (`EldenRingDLSS/src/skin_motion.*`).

## Setup

- Game: Steam app 2909400, UE 4.26.1 (Square Enix fork, Streamline + DLSS/FSR plugins), `ff7rebirth_.exe`
  (2026-05-27 build). User settings: 4K output, DLSS at 50% (render 1920x1080), HDR.
- RenderDoc 1.46 via `tools/launch_rdc.ps1` (ReShade off for the session, `steam_appid.txt`). The game asserts in
  `WindowsD3D12PipelineState.cpp:746` because RenderDoc refuses D3D12 pipeline libraries with data. The game turns
  the driver PSO cache on in `[WindowsNoEditor DeviceProfile]` (`D3D12.PSO.DriverOptimizedDiskCache=1`) and ignored
  both a user `Engine.ini` `[SystemSettings]` and a user `DeviceProfiles.ini` override (`-CVars=`/`+CVars=`), so
  `tools/psopatch.py` patches `FD3D12PipelineStateCache::Init` in memory while RenderDoc holds the new process
  (`--opt-delay-for-debugger 8`): RVA `0x46D6DBD` `setne sil` -> `xor esi, esi` (bUseAPILibaries and the driver
  blob cache off, the engine's stock default). Every RenderDoc start resets the game's driver PSO cache
  (`Saved/D3DDriverByteCodeBlob_*.ushaderprecache`), so the next normal start shows "Preparing shaders" once.
- Under RenderDoc DLSS/Streamline do not run: the game falls back to its TAA at the render size (dispatch reading
  the velocity texture, then the final composite to 3840x2160).
- Game config (unpacked from `pakchunk0-WindowsNoEditor.pak` with repak, unencrypted, into `gamedata/config`):
  `DefaultEngine.ini [SystemSettings]` turns on the animation budget allocator: `a.Budget.Enabled=1`,
  `BudgetMs=3.5` (`a.BudgetFor60FPS.BudgetMs=1.6`), `MaxTickRate=3`, `MaxInterpolatedComponents=0`, plus
  `a.OptimizedSkeletalMeshTick=1`. Town NPCs are Square Enix "MobCrowd"/"MobPrefab" actors (exe strings), with
  UE's AnimationSharing plugin present.

## Capture `ff7r_frame8775.rdc` (Kalm market square, camera still, NPCs standing/chatting)

- 2087 actions: the environment is GPU-driven (498 ExecuteIndirect), characters are ordinary draws.
- **Velocity is written in the base pass** (`r.BasePassOutputsVelocity` style): the G-buffer pass (events
  12323-13958, 177 draws) has 7 targets, the 7th is `tex2304` 1920x1080 R16G16_UNORM (UE's GBufferVelocity). The
  separate velocity pass has only 2 draws (15013, 15023). Encoding: 0,0 = not written (readers use camera motion
  from depth); otherwise `(enc - 32767/65535) / (0.499 * 0.5)` in clip units.
- 159 of the 177 base pass draws are GPU-skinned; every one binds `BoneMatrices` and `PreviousBoneMatrices`
  (sub-allocated from 8 MB pools, one 64-byte bone = 4 float4 rows, `.xyz` used, layout `float4x3`), with
  `PerBoneMotionBlur=1`, `Primitive_DrawsVelocity=1`, `Primitive_OutputVelocity=1`.
- `tools/velocity_view.py` (brightness only): every NPC, the dog, Cloud and the lamp posts are written; 6.6% of
  pixels; motion p99 0.88 px, max 1.65 px.
- Pixel history (`tools/rdc_pixhist.py`): each NPC pixel's velocity comes from its own skinned base pass draw.
- Bones (`tools/rdc_bonerange.py`, over the bones each draw's vertices use): previous and current differ for the
  NPCs (up to 7.5 cm translation terms), so the previous pose is real, not a copy of the current one.
- Per-vertex motion from the vertex shader's `CURRENT_`/`PREVIOUS_CLIP_SPACE_POSITION` outputs
  (`tools/rdc_postvs_motion.py`, TAA jitter removed) matches the velocity buffer: Cloud max 1.28 px (buffer 1.24),
  dog 1.63 (1.65), a gesturing NPC 1.10 (1.07). One NPC walking behind the market roof (hidden) moves 1-2 px.

So within one frame the NPCs' motion vectors are present and consistent with the two poses the GPU holds. Not
the Elden Ring situation.

## Three consecutive frames: the cause (`ff7r_frame8977/8978/8979.rdc`, 2026-10-12)

Kalm square, camera still, a man in a suit walking toward the camera beside Cloud, a crowd at mid distance.
`tools/rdc_skinstate.py` dumps every skinned base pass draw (bones its vertices use, current and previous, and the
vertex shader's clip positions); `tools/skin_frames.py` matches draws across frames (buffers, first index, nearest
position) and compares, TAA jitter removed:

| | 8977 -> 8978 | 8978 -> 8979 |
| --- | --- | --- |
| animation updated: previous = last frame's current bones, motion vectors exact | 145 | 164 |
| not updated, previous already equal to current (zero skinning motion) | 32 | 33 |
| **not updated, previous = the pose before the last update** | **53** | **34** |
| worst wrong motion of such a draw | 4.1 px | 2.0 px |

On a frame without an animation update the pose does not move on screen, but the motion vectors repeat the last
update's motion (e.g. 3.6/4.1 px p90/max claimed, 0 px actual). The walking man near the camera updates every frame
and is correct; the throttled ones are mostly the mid-distance crowd. Bone buffer views behave as the rule needs:
an update binds a new BoneMatrices buffer (and the old one as previous); a skipped frame binds the same two
buffers as the frame before.

The game's code: `FGPUSkinVertexFactoryShaderParameters::GetElementShaderBindings` (RVA `0x797B28`, found from the
`TGPUSkinVertexFactory` type registrations at `0x6DEBE0`: ConstructParameters `0x4D88740` -> Bind `0x4D90C1C`,
GetParameterTypeElementShaderBindings `0x4D88BCC`) binds, at `0x797DD7`, `PreviousBoneMatrices =
BoneBuffer[CurrentBuffer ^ ((CurrentRevision - PreviousRevision) <= 1)]` with `PerBoneMotionBlur = 1`
(ShaderData: `+0x10` BoneBuffer[2] {RHI, SRV}, `+0x30` CurrentBuffer, `+0x34`/`+0x38` previous/current revision):
UE 4.26.1's stock logic. UE 4.27 added the check that the bone data was updated this frame
(`bBoneDataUpdatedThisFrame`) and binds the current bones otherwise.

## The fix: `FF7RebirthNpcMotion.addon64`

At every draw whose vertex shader reads `PreviousBoneMatrices` (found by name in the DXIL, registers by DXC
reflection: t0/t1, space 0 in every skinned shader seen), the add-on resolves both descriptors of the draw's
vertex-stage table (descriptor tracking adapted from the Elden Ring add-on) to buffer + offset. If the
`BoneMatrices` buffer was some draw's `BoneMatrices` in the previous frame, the mesh was not updated: the add-on
copies that descriptor over the `PreviousBoneMatrices` slot of the table (each draw gets its own table in the
game's online heap: root parameter 2, two descriptors). Skinning motion becomes zero for that frame, the
character's own movement (Primitive_PreviousLocalToWorld) still produces motion. Races between frames can only
miss a fix (a buffer from two frames back is never in the set), not apply a wrong one.

## In game (2026-10-12, Kalm, 4K, DLSS Performance, RTX 5070 Ti, i7-13700KF)

- Motion vector fix alone (`ToggleKey` F7 for A/B): every skinned draw resolved (0 unresolved), 37-64 of 260-290
  skinned draws per frame were not updated and got the fix; about 104 fps (preset K). The user: "a moderate
  improvement maybe. Nothing earth shattering": the throttled crowd still moves in steps of 2-3 frames.
- The animation budget: `a.Budget.*` and Square Enix's second set `a.BudgetFor60FPS.*` (`DefaultEngine.ini`
  `[SystemSettings]`; `a.BudgetFor60FPS.Enabled` there is not a real variable). The allocator copies one set into its
  parameters (`0x1951D00`: flag at allocator `+0x1E1` picks the 60 FPS set, set by `0x1951CE4`; `0x27A586E` copies
  `a.Budget.*`). `FAnimationBudgetAllocator::Update` (`0xFCFD33`) skips budgeting unless `a.Budget.Enabled == 1`.
  Variables (FAutoConsoleVariableRef targets, found by name like the add-on does): `a.Budget.Enabled` `0x90E7A90`,
  `a.Budget.MaxInterpolatedComponents` `0x6EEC43C`, `a.BudgetFor60FPS.MaxInterpolatedComponents` `0x6EEC3CC`.
  Square Enix's own `End.Animation.*` distances (e.g. `ForceUpdateCameraDistance` 2000) sit on top.
- `AnimationBudget=0` (a.Budget.Enabled 0 at startup): every character animated every frame, no frozen ones, but about
  61 fps with the GPU at 55-60%: CPU-bound, roughly 6-7 ms of animation per frame. Too expensive.
- **`InterpolatedCharacters=64`** (both MaxInterpolatedComponents, budget on): 5 of about 207 skinned draws per frame
  not updated (the add-on fixes those), 100-102 fps with the GPU at 83-90% (preset M by then). The user: "seems good
  to me". These are the defaults.

## DLSS integration against NVIDIA's DLSS Programming Guide (31 March 2026), 2026-10-12

Super resolution runs through Square Enix's own NGX integration: the NGX static library in the executable resolves
`NVSDK_NGX_D3D12_CreateFeature`/`EvaluateFeature` from the driver's `_nvngx.dll` and keeps the pointers in its data
section (Streamline 2.8 only does frame generation and Reflex). The development probe (`DlssProbe=1`,
`src/dlss_probe.*`, F6 logs 120 evaluations) swaps those pointers. What the game passes (Kalm, preset M via the NVIDIA
app, DLSS 310.9): flags `IsHDR|MVLowRes|DepthInverted|AutoExposure`, colour R11G11B10F and motion vectors RG16F at
1920x1080 (scale 1920, 1080), output 3840x2160, no exposure texture, pre-exposure 1, constant render size. The jitter
equals `View.TemporalJitterPixels` of the same frame (NVIDIA's Unreal plugin convention). The probe reads the frame's
View values at its first skinned draw (vertex shader `b1`, a root descriptor; ReShade 6.8 reports the root
descriptors of a version 1.1 root signature as `push_descriptors_with_ranges_and_flags`).

- **Jitter**: 8 positions. The jitter code (`0x1200E45`) wraps at `r.TemporalAASamples` (Unreal's 8; the engine's
  temporal upscaling path, which would raise it, is not used) and always uses Halton. Guide 3.7.1.1: at least 32 in
  Performance mode. `JitterPhases=32` (F5) gives the 32-position sequence (confirmed in the log).
- **Pre-exposure**: the engine pre-exposes the scene colour (`View_PreExposure` 0.000198 in Kalm, 0.0625 on the title
  screen) and the game passes 1. Guide 3.9.2: pass it at every evaluation. `DlssPreExposure=1` (F4) does; the log
  shows the passed value equal to the View value. At first only an evaluation after a character draw in the same
  frame got it, so a frame without one fell back to 1, about 5000 times the value in Kalm; since 2026-10-15 such a
  frame gets the latest value read (not yet checked in game).
- **Mip bias**: `View_MaterialTextureMipBias` is 0, but the materials do not use it. They sample every texture through
  one shared sampler (`View_SharedAnisotropic4XWrappedSampler`) with `SampleBias(View_TemporalSamplerBias.y)`, a
  Square Enix addition (View offset 2480). The jitter setup stores it at `0x1201185` (view `+0xC40`):
  (s, log2 s, s, log2 s), s = render width / output width, so -1 in Performance mode. Guide 3.5 asks for
  log2(s) - 1 (-2), no less than log2(s) when that shimmers. The log2 is `FMath::Log2`: `logf` times the constant
  at `0x6FE4068`, which every Log2 in the engine shares (not a place to change anything). `r.MipMapLODBias` (Unreal's
  global bias, into each texture's own sampler; changing it live works through the console variable sinks) should not
  reach these materials, which sample through the shared View sampler instead. Two screenshots at 0 and -2 agree
  (`shot_bands.py` tiles with a two-shot motion mask, 599 still, textured tiles): the 2-4 px energy median is 0.996,
  and 0.5% of the tiles have at least 5% more fine detail at -2, where `ExtraMipBias` below gives 11-12% measured the
  same way. A median alone could not have told: `ExtraMipBias` leaves it unchanged too. `ExtraMipBias` sends the four
  stores to a routine that adds the extra bias to `.y` and `.w` while the game's value is negative
  (`src/texture_bias_code.hpp`, checked by `tests/bias_routine_test`).
- The game creates a D3D12 device at startup only to destroy it again, so ReShade loads the add-on twice. The add-on
  puts back everything it changed (console variables, NGX pointers, the stores) when it is unloaded, so the second
  load finds the game's own values (the first jitter A/B compared 32 with 32 because of this).
- In game: F5 (8/32) and F4 (pre-exposure) made no difference the user noticed.

## `ExtraMipBias` in game (2026-10-14)

Kalm market square, 4K, DLSS Performance (preset M), HDR, with the FF7RebirthTextureUpscaler mod installed (2x top
mips for about 10,000 environment colour textures of 256-1024 px). Camera still, ReShade screenshots (F11) at a bias of
-2 (extra on), -1 (End: the game's) and -2 again, about 20 s apart; the add-on's log line `texture bias` (read from
the View buffer the draws bind) confirmed each value.

- Method (`tools/decode_png16.ps1`, `tools/shot_bands.py`): ReShade saves HDR screenshots as 16-bit PNGs (PQ,
  BT.2020), of which Pillow reads only the high byte, so WIC decodes them. PQ-encoded luminance, FFT octave bands,
  64-pixel tiles. Tiles that move between the two -2 shots (a 16-pixel block mean changes by 0.01 PQ or more) or touch
  the frame border are dropped, and the textured half of the rest is compared: 566 tiles.
- Fine detail (2-4 px) energy, -2 against -1: median 1.000 (10th-90th percentile 0.977-1.053), against 0.994
  (0.980-1.003) between the two -2 shots; the coarser bands agree within 0.1%. Enlarged crops of roofs, walls, wood
  and a sign look identical. The user: "I feel like it looks better but that could just be placebo". No measurable
  gain in median texture detail, but the upper tail is not noise: 10.6% of the tiles have at least 5% more fine detail
  in the first -2 shot than at -1 and 12.5% in the second, while 1.4% differ that much between the two -2 shots and
  1.6% have that much more at -1. So some surfaces do get a little sharper at -2; this test does not show which, or
  how they look in motion.
- The -1 shot differs from the -2 shots more than they differ from each other (mean squared band difference ratio
  1.9-2.2 against the first -2 shot, 1.6-1.8 against the second, similar at every scale) without any change in the
  median detail energy. Not explained; part of it may come from the method, since the motion mask keeps the tiles
  where the two -2 shots agree.
- Control: `ExtraMipBias=3` through the overlay (a bias of +2) made textures obviously blurry: the cobblestone grain
  and the engraving on Cloud's sword are gone (the user: "3 is obviously blurrier"). Fine-band energy is 0.76 of the
  -1 shot in the tiles a two-shot mask keeps; the real loss is larger, since that mask drops the tiles the blur
  changed most. So the redirected stores reach the materials, characters included.
- Why, in part: a bias only chooses among the mips a texture has. The texture mod's probe build (its added top mips
  drawn as a checkerboard, DLSS Performance, the game's -1) showed walls, beams and boards near the camera at their
  top mip, so -2 asks them for a level that does not exist. The floor and farther surfaces showed their original mips
  there, so -2 can give them a finer level, which would fit the tiles that gained. Untested: whether texture streaming
  holds finer mips of distant surfaces back (`r.Streaming.PoolSize` is 4000 MB at `TextureQuality@2` in
  `WindowsScalability.ini`; `r.Streaming.Boost` and the other streaming variables exist in the executable).
- Result: `ExtraMipBias` defaults to 0, which leaves the game's code alone. `JitterPhases=32` and `DlssPreExposure=1`
  stay on: the guide asks for them and they showed no downside.

## Open

1. A RenderDoc capture with the add-on active, run through `skin_frames.py`: the stale rows should be gone.
2. Crowded places (Gold Saucer, Junon) may have more than 64 budgeted characters; the rest are still throttled and
   covered by the motion vector fix.
3. The extra mip bias: which surfaces gain at -2 and how they look in motion; whether a larger streaming pool or
   `r.Streaming.Boost` gives distant surfaces finer mips.
4. DLAA (s = 1): the game's bias is 0 where the guide asks for -1; the extra bias only applies while the game's value
   is negative.
5. The pre-exposure for frames without a character draw (the latest value read, since 2026-10-15): check in game that
   the counter in the overlay's DLSS hooks line grows only in such frames and nothing flickers.
