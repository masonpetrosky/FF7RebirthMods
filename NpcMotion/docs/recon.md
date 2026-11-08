# FF7 Rebirth: NPC motion vectors, investigation log

Question (2026-10-12): in Kalm, do NPCs get motion vectors with DLSS? The Elden Ring add-on had to generate them
because that game draws none for enemies (`EldenRingDLSS/src/skin_motion.*`).

## Setup

- Game: Steam app 2909400, UE 4.26.1 (Square Enix fork, Streamline + DLSS/FSR plugins), `ff7rebirth_.exe`
  (2026-05-27 build). User settings: 4K output, DLSS at 50% (render 1920x1080), HDR.
- RenderDoc 1.46 via `tools/launch_rdc.ps1` (ReShade and the other hook DLLs off for the session, `steam_appid.txt`). The game asserts in
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
- `NpcMotion/tools/velocity_view.py` (brightness only): every NPC, the dog, Cloud and the lamp posts are written; 6.6% of
  pixels; motion p99 0.88 px, max 1.65 px.
- Pixel history (`tools/rdc_pixhist.py`): each NPC pixel's velocity comes from its own skinned base pass draw.
- Bones (`NpcMotion/tools/rdc_bonerange.py`, over the bones each draw's vertices use): previous and current differ for the
  NPCs (up to 7.5 cm translation terms), so the previous pose is real, not a copy of the current one.
- Per-vertex motion from the vertex shader's `CURRENT_`/`PREVIOUS_CLIP_SPACE_POSITION` outputs
  (`NpcMotion/tools/rdc_postvs_motion.py`, TAA jitter removed) matches the velocity buffer: Cloud max 1.28 px (buffer 1.24),
  dog 1.63 (1.65), a gesturing NPC 1.10 (1.07). One NPC walking behind the market roof (hidden) moves 1-2 px.

So within one frame the NPCs' motion vectors are present and consistent with the two poses the GPU holds. Not
the Elden Ring situation.

## Three consecutive frames: the cause (`ff7r_frame8977/8978/8979.rdc`, 2026-10-12)

Kalm square, camera still, a man in a suit walking toward the camera beside Cloud, a crowd at mid distance.
`NpcMotion/tools/rdc_skinstate.py` dumps every skinned base pass draw (bones its vertices use, current and previous, and the
vertex shader's clip positions); `NpcMotion/tools/skin_frames.py` matches draws across frames (buffers, first index, nearest
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

## CPU cost (2026-10-31, Grasslands)

`tools/perf_session.py` at an open-world spot (4K, DLSS Performance, every mod installed): the GPU held the frame
(63 fps, GPU busy 99-100%), and switching the fix off and on with F7 changed nothing (within ±0.03 ms). The CPU
sampler (`build/cpu_sampler.exe`, 15 s at 250 Hz) found the add-on's own code in 13% of the RHIThread's samples (that
thread was busy 52% of the time), 2-4% on the task-graph threads, 4.4% of all sampled thread time. A rebuild of the
installed commit (`c0283c9`, identical to the installed file but for 6 timestamp bytes) with a linker map puts the
hottest offsets in the descriptor mirror: `Descriptors::OnCopyDescriptorTables` and the `concurrent_unordered_map`
lookup of its heaps (two per copy), plus `concurrent_vector` capacity checks. The game copies descriptors for nearly
every draw, so this runs thousands of times per frame on the thread that submits them. It costs no frames where the
GPU is the limit; in CPU-bound places (Kalm, about 100 fps with the GPU at 83-90%) it may.

## Frame-rate limit (2026-10-31)

The options menu offers 30, 60, 90 and 120 fps (`$menu_options_fps_30` to `_120`); there is no unlimited entry. Its
apply function (`0x10BB87C`, argument = menu mode: 1-4 with `r.VSync` on, 5-8 with it off, anything else = no limit,
which the game uses itself while a flag at `0x8F32ADA` is set) writes three console variables through `0x10BBA98`
(float to string, then `IConsoleVariable::Set`) with `ECVF_SetByScalability` (`0x01000000`), the lowest priority:

- `t.MaxFPS` = option x 0.999 (constant at `0x5F0A5E0`), so the 120 entry limits to 119.88;
- `r.DynamicRes.FrameTimeBudget` = 1000 / that (dynamic resolution is off, so it changes nothing);
- `rhi.SyncInterval` = refresh rate / option while VSync is on and `r.FrameGeneration.Method` is 0, else 0.

The `IConsoleVariable*` of these, of `r.VSync` and of `r.FrameGeneration.Method` are looked up once at startup
(`0x3AF6CA2`-`0x3AF6DEC`, slots `0x91D8540`-`0x91D8560`). `0x10BC030` reads `t.MaxFPS` back and turns it into a menu
mode; only the 30 fps mode changes other state (a few timing constants). A value that is none of the four gives mode 0
with VSync off, the game's own unlimited path. Lyall's FF7RebirthFix (0.0.10, February 2025) replaces the 120 entry
with a mid-function hook in `0x10BBA98`; its pattern still matches this executable once.

Who waits (2026-11-08; the line above used to say the Streamline handler limits, which is wrong):
`UEngine::UpdateTimeAndHandleMaxTickRate` (around `0x7943D3`) gets the tick rate from `GetMaxTickRate` (vtable
+0x2D0), computes the wait (1 / rate - delta time), then calls every `IMaxTickRateHandlerModule` (`call [rax+0x28]`,
slot 5) and waits itself unless one returns true. The game's one handler, `FStreamlineMaxTickRateHandler` (8-byte
object, vtable `0x5B127C8`, slot 5 = `0x2E8EED4`), ignores the rate: it builds `sl::ReflexOptions` with
`frameLimitUs = 0` (`mov dword [rsp+0x4C], 0`, virtual key 0x7C), sets them, runs Reflex's sleep and returns false. So
Unreal's own sleep-and-spin wait enforces `t.MaxFPS`, once per game frame: rendered frames only.
`GetMaxTickRate` reads `t.MaxFPS` at `0x155BEAA` (`mov rax, [TAutoConsoleVariable 0x90BF990 + 0x10]`, then
`vmovss xmm1, [rax + r9*4]`) and uses it whenever it is above 0 (`vcmpltss` + `vblendvps`), in place of the smoothed
rate (`bSmoothFrameRate=false` in the game's `BaseEngine.ini`, so 0 is no limit at all). The only other read is the
virtual `UEngine::GetMaxFPS` (`0x9C9DE8`). The read-back `0x10BC030` (called at startup `0x3AF6DF3`, `0x3AE0864`, and
from `0xFE76A4`, `0x10BA7E4`) maps `t.MaxFPS` back to a mode and calls the apply function with it. A value a mod
writes into `t.MaxFPS` (none of the four) becomes mode 0 there with the game's VSync off (`t.MaxFPS` 0), and with it on
the 60 or 30 fps VSync mode, picked by `rhi.SyncInterval` (1 or 2). The Frame Limit add-on (`FrameLimit/`) therefore
redirects the load at `0x155BEAA` instead and leaves `t.MaxFPS` to the game.

No config file the game ships sets `t.MaxFPS` (the device profiles only set streaming, MassiveEnvironment, D3D12 and
dynamic resolution variables). In Unreal, `t.MaxFPS` under `[SystemSettings]` in the user's
`Saved\Config\WindowsNoEditor\Engine.ini` (`ECVF_SetBySystemSettingsIni`, `0x04000000`) would outrank the menu, but
this game does not apply it: with `t.MaxFPS=0` there before the start, the running game still had 119.88 set by
`SetByScalability` (variable object read through slot `0x91D8550`: flags at +0x18, value at +0x48).

In game (2026-10-31, menu at 120, the game's DLSS frame generation on, a light spot): PresentMon counted 128.6 frames
shown per second, rendered frames (about 15.3 ms of GPU time each) alternating with generated ones (about 0.06 ms),
so about 64 rendered fps (`captures/perf/2026-10-31-framecap`). The 119.88 limit does not count generated frames: with
frame generation the 120 entry allows at least 240 shown frames, and the GPU is what limits it.

## Dynamic resolution limits and DLSS Balanced (2026-11-07)

Question (the user): can the game get DLSS Balanced? It has no DLSS quality setting. The graphics options set the
render scale only through the dynamic resolution lower and upper limits (`$menu_options_graphic_dynamic_resolution_limit`
and `..._upper_limit`; both at one value give a fixed scale), and both lists offer 33, 50, 66 and 100%.

- **Applying a choice**: `0x10BB5FC` (anti-aliasing mode, lower, upper) looks both values up in a table of six
  20-byte entries at `0x5BD4560` (value; percentage; cap on the upper limit; the two as fractions): 100, 75, 66
  (66.667), 58 (58.824 = 1/1.7), 50 and 33 (33.333), all with cap 100. A value not in the table is used as the
  percentage itself. Upper = min(its entry's percentage, the lower entry's cap), then at least the lower. Mode 2 uses a
  copy at `0x5BD45E0` (only while the flag at `0x91D85D9` is set) whose 33 entry caps the upper limit at 33.3: that is
  DLSS, whose Ultra Performance takes only its exact size (below). The setters (`0x10BB790` lower, `0x10BB724` upper)
  clamp to 0..100 and set `r.DynamicRes.MinScreenPercentage` / `MaxScreenPercentage` with `ECVF_SetByCode`
  (`0x08000000`), above the device profile's 75/100 (`WindowsDeviceProfiles.ini`, with `r.DynamicRes.OperationMode=2`).
  Callers: the options menu (`0x10B9C38`, the same code again in `0xFE4C80`; option 0x45 = lower limit, 0x4E = upper,
  each moving the other along if needed) and the startup apply (`0x3AE08EA`), from the settings bytes +0x372 (lower),
  +0x37A (upper) and +0x375 (anti-aliasing mode). The menu stores the value (`mov [rbx+0x372], dil`), not its place
  in the list, and finds the place again by value (`0x159A4BC`, `0x159A4F0`; graphics presets do the same), so a
  longer list leaves saved settings alone.
- **The menu's lists**: the options menu builder (`0x159D394`, 64 KB) fills the lower-limit array (+0x5D0) from
  `0x5B87098`..`0x5B870A8` and the upper one (+0x5E0) from `0x5B87088`..`0x5B87098`, two constant lists {33, 50, 66,
  100}, with a label per value (`..._select_format`). Each loop's bounds are RIP-relative loads: `lea r14, [begin]` at
  `0x15A4FAD` (lower) and `0x15A53EF` (upper); `lea rax, [end]; add r14, 4; cmp r14, rax` at `0x15A50FA` and
  `0x15A553C`. Nothing else reads the lists (`0x15A8955` loads the byte list stored right after them). So 58 and 75
  are in the apply table but in no menu list.
- **DLSS quality mode**: the DLSS pass (`0x134479C`) picks NGX's mode from the render scale: >= 0.66 Quality (2),
  >= 0.58 Balanced (1), >= 0.5 Performance (0), >= 0.33 Ultra Performance (3), else Balanced. The evaluation
  (`0x1343AB8`) recreates the feature when the output size, the mode or the preset changes, asks NGX for that mode's
  optimal settings and creates the feature for the optimal render size (`0x2690B7C`, creation flags 0x4B). So the
  game's 100% runs a Quality feature created for 2560x1440 (at 4K) with 3840x2160 frames, its 58.8% would run a
  Balanced one created for 2227x1253 with 2259x1271 frames, and dynamic resolution between its own 50 and 66% already
  passes through Balanced.
- **Offline check** (`NpcMotion/tools/dlss_subrect_check.cpp`; DLSS 310.9.1, RTX 5070 Ti, 4K output, the game's creation flags; a
  still synthetic scene with sharp edges rendered with jitter at the input size, 64 frames, error against the scene
  supersampled at 4K): NGX's optimal sizes at 4K are Ultra Performance 1280x720 (fixed), Performance 1920x1080,
  Balanced 2227x1253 (0.5799), Quality 2560x1440, DLAA 3840x2160 (3802x2138..3840x2160); Performance, Balanced and
  Quality take any input from 1920x1080 to 3840x2160. 2259x1271 frames on a feature created for 2227x1253 gave exactly
  the error of a feature created for 2259x1271 (preset K 0.00608, L 0.01686, M 0.01557, the same at the right and
  bottom edges, where a cropped or rescaled read would show), and 3840x2160 frames on a Quality feature created for
  2560x1440 exactly that of one created for 3840x2160 (K 0.00351). Every creation and evaluation succeeded. Within a
  mode's range the creation size does not change the output.
- **The add-on** (`BalancedStep=1`, `src/resolution_menu.hpp`): at startup it finds the lists (once in `.rdata`) and
  the four loads (each once, the end after its begin), writes {33, 50, 58, 66, 100} into memory within 2 GB of the
  code and points all four loads at it; ReShade's unload puts the displacements back. `build/NpcMotion/resolution_menu_test.exe` runs
  the same search on the installed executable: the original loops read 33, 50, 66, 100, the redirected ones 33, 50,
  58, 66, 100, on the file and on the image mapped in memory (list 22 MB below the code; search 78 ms).
- Not yet seen in game: 58% in both lists and its label, the render size, and the probe's `create` line (expected
  quality=1, render=2227x1253, then `render_subrect=2259x1271`).

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
6. The descriptor mirror's CPU cost (see "CPU cost"): cache the last heaps looked up instead of two concurrent-map
   finds per copy, and check the gain with `tools/perf_session.py` in a CPU-bound place.
7. `BalancedStep` in game (see "Dynamic resolution limits"): the menu entries, the render size and the DLSS mode.
