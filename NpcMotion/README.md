# FF7 Rebirth NPC Motion

A [ReShade](https://reshade.me) add-on for **FINAL FANTASY VII REBIRTH** (PC) that fixes the motion vectors of
townspeople and other characters whose animation the game throttles, so DLSS (and the game's TAA and motion blur)
no longer smears their outlines, and lets the game's animation budget interpolate them so they move every frame. It
also sets the DLSS jitter sequence and pre-exposure the way NVIDIA's DLSS Programming Guide asks (the guide's extra
texture mip bias is available but off by default; see [DLSS](#dlss)).

## The problem

Rebirth (Unreal Engine 4.26) limits how much time it spends on character animation with Unreal's animation
budget allocator: characters at medium distance are animated only every second or third frame, and Square Enix
turned the budget's interpolation off (`a.Budget.MaxInterpolatedComponents=0`). Every skinned mesh keeps two bone
buffers on the GPU, the current pose and the previous one, and the velocity pass compares them. On a frame without
an animation update, UE 4.26 still binds the older pose as the previous one, so the motion vectors repeat the last
update's movement while the character stands still on screen. In Kalm, 37-64 of about 270 character draws per frame
carried motion that had not happened, up to 4 pixels at 1080p render resolution. DLSS then reprojects those
characters' history from the wrong place. (UE 4.27 added the missing check.)

## What the add-on does

- **Motion vectors**: at every draw whose vertex shader reads `PreviousBoneMatrices`, if the draw's `BoneMatrices`
  buffer was already a current bone buffer in the previous frame (the game allocates a new one for every pose
  update), the mesh was not updated, so the add-on points the draw's `PreviousBoneMatrices` descriptor at the current
  bones. The character's skinning motion is zero for that frame; its movement through the world still produces
  motion vectors. Shaders are recognised by name (DXIL reflection), not by hash.
- **Interpolation** (`InterpolatedCharacters`): before the world loads, the add-on sets the budget's
  `MaxInterpolatedComponents` (both of the game's budget profiles) so throttled characters get an interpolated pose
  on the frames they are not evaluated. In Kalm this left 5 of about 207 character draws per frame without a new pose, at nearly the same frame rate
  (100-102 fps against about 104 before, though measured with different DLSS presets). Switching the budget off completely (`AnimationBudget=0`) animates everyone every
  frame but made the game CPU-bound (about 60 instead of 100 fps on an i7-13700KF).

## DLSS

Rebirth runs DLSS super resolution through Square Enix's own NGX integration. Checked against NVIDIA's DLSS
Programming Guide (31 March 2026) at 4K output in Performance mode (render 1920x1080):

- **Jitter** (`JitterPhases`): the game keeps Unreal's 8 jitter positions at every render scale; the guide asks for at
  least 32 in Performance mode. The add-on raises `r.TemporalAASamples` to 32.
- **Pre-exposure** (`DlssPreExposure`): the engine pre-exposes the scene colour, but the game tells DLSS the
  pre-exposure is 1; the guide asks for the real value at every evaluation. The add-on hooks the game's NGX calls and
  passes the engine's value, read from the View buffer at the frame's first character draw (a frame without one gets
  the latest value read).
- **Texture mip bias** (`ExtraMipBias`, off by default): the game samples textures with a bias of log2(render width /
  output width), -1 in Performance mode, and the guide asks for one level more: -2, i.e. `ExtraMipBias=-1`, which the
  add-on adds while the image is upscaled (not at DLAA). Screenshots of one spot in Kalm at -2, -1 and -2 again
  (`tools/shot_bands.py`) showed no measurable gain in median texture detail: fine detail -2 against -1 had a median
  ratio of 1.000, against 0.994 between the two -2 shots. But about a tenth of the tiles had at least 5% more fine
  detail in both -2 shots, where 1.4% differ that much between the two, so some surfaces do get a little sharper; the
  test does not show which, or how they look in motion. With the
  [FF7RebirthTextureUpscaler](https://github.com/masonpetrosky/FF7RebirthTextureUpscaler) mod installed, near walls and
  beams already show their top mip at -1, so they cannot gain. A positive bias made textures obviously blurry, so the
  bias does reach the materials.

Switched back and forth while playing (F5, F4), the jitter and pre-exposure changes made no noticeable difference;
they are on because the guide asks for them. The keys (F5, F4, End) and the overlay's checkboxes switch these settings
for the session; the ini sets what the game starts with.

## Installation

Requires ReShade 6.x **with full add-on support** for `ff7rebirth_.exe` (DirectX 12) and the
[FF7 Rebirth Hair Fix](https://github.com/masonpetrosky/FF7RebirthHairFix) (the add-on uses its `FF7RebirthHairFix\dxcompiler.dll`; a
`FF7RebirthNpcMotion\dxcompiler.dll` next to the add-on works too). Copy `FF7RebirthNpcMotion.addon64` and
`FF7RebirthNpcMotion.ini` into `...\FINAL FANTASY VII REBIRTH\End\Binaries\Win64`.

## Settings

`FF7RebirthNpcMotion.ini` (the ReShade overlay's Add-ons tab shows per-frame counters). The keys work while the game's
window is in front and neither Alt nor Ctrl is held.

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | Fix the motion vectors of characters that were not updated this frame. |
| `ToggleKey` | 118 (F7) | Virtual-key code that switches the fix on and off; 0 = none. |
| `InterpolatedCharacters` | 64 | Characters the animation budget may interpolate; -1 = the game's value (0). Startup. |
| `AnimationBudget` | 1 | 0 = switch the game's animation budget off at startup (expensive, see above). |
| `LogStats` | 0 | 1 = write counters and the frame rate to `ReShade.log` every 600 frames. |
| `JitterPhases` | 32 | Jitter positions per sequence for DLSS; 0 = the game's 8. `JitterKey` (116, F5) switches between the two for the session. |
| `DlssPreExposure` | 1 | Pass the engine's pre-exposure to DLSS. `PreExposureKey` (115, F4) switches it for the session. 0 with `DlssProbe=0` leaves the game's DLSS calls alone. |
| `ExtraMipBias` | 0 | Extra texture mip bias on top of the game's; 0 = leave the game's code alone. A non-zero value patches the game's bias at startup; `MipBiasKey` (35, End) switches it for the session, and the overlay's slider changes and saves it. |
| `DlssProbe` | 0 | Development: log what the game passes to DLSS to `FF7RebirthNpcMotion_dlss.log` (F6 logs 120 evaluations). |

## Building

Visual Studio 2022 or newer with the C++ x64 tools: `.\build.ps1` builds `build\FF7RebirthNpcMotion.addon64`,
`build\reflect_test.exe` (checks the bone register lookup on shaders saved from a capture) and
`build\bias_routine_test.exe` (runs the texture bias routine the way the game enters it).

## Investigation

`docs/recon.md` is the log: RenderDoc captures of Kalm (the game needs a small in-memory patch to run under RenderDoc,
`tools/psopatch.py`), the base pass velocity, the bone buffers across three consecutive frames, the engine code that
binds them, and the in-game measurements. `tools/` holds the capture and analysis scripts; `tools/decode_png16.ps1`
and `tools/shot_bands.py` measure texture detail in screenshots of one spot (ReShade's 16-bit HDR PNGs).

## License

MIT; see [LICENSE](LICENSE). `src/descriptors.*` is adapted from ReShade's descriptor tracking example
(Copyright (C) 2021 Patrick Mours, BSD-3-Clause OR MIT); `third_party/reshade` and `third_party/imgui` carry their own
licenses. FINAL FANTASY VII REBIRTH is a trademark of Square Enix; this project is not affiliated with or endorsed by
Square Enix.
