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

## Open

1. A RenderDoc capture with the add-on active, run through `skin_frames.py`: the stale rows should be gone.
2. Crowded places (Gold Saucer, Junon) may have more than 64 budgeted characters; the rest are still throttled and
   covered by the motion vector fix.
