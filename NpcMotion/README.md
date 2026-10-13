# FF7 Rebirth NPC Motion

A [ReShade](https://reshade.me) add-on for **FINAL FANTASY VII REBIRTH** (PC) that fixes the motion vectors of
townspeople and other characters whose animation the game throttles, so DLSS (and the game's TAA and motion blur)
no longer smears their outlines, and lets the game's animation budget interpolate them so they move every frame.

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

## Installation

Requires ReShade 6.x **with full add-on support** for `ff7rebirth_.exe` (DirectX 12) and the
[FF7 Rebirth Hair Fix](https://github.com/masonpetrosky/FF7RebirthHairFix) (the add-on uses its `FF7RebirthHairFix\dxcompiler.dll`; a
`FF7RebirthNpcMotion\dxcompiler.dll` next to the add-on works too). Copy `FF7RebirthNpcMotion.addon64` and
`FF7RebirthNpcMotion.ini` into `...\FINAL FANTASY VII REBIRTH\End\Binaries\Win64`.

## Settings

`FF7RebirthNpcMotion.ini` (the ReShade overlay's Add-ons tab shows per-frame counters):

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | Fix the motion vectors of characters that were not updated this frame. |
| `ToggleKey` | 118 (F7) | Virtual-key code that switches the fix on and off; 0 = none. |
| `InterpolatedCharacters` | 64 | Characters the animation budget may interpolate; -1 = the game's value (0). Startup. |
| `AnimationBudget` | 1 | 0 = switch the game's animation budget off at startup (expensive, see above). |
| `LogStats` | 0 | 1 = write counters and the frame rate to `ReShade.log` every 600 frames. |

## Building

Visual Studio 2022 or newer with the C++ x64 tools: `.\build.ps1` builds `build\FF7RebirthNpcMotion.addon64` and
`build\reflect_test.exe` (checks the bone register lookup on shaders saved from a capture).

## Investigation

`docs/recon.md` is the log: RenderDoc captures of Kalm (the game needs a small in-memory patch to run under RenderDoc,
`tools/psopatch.py`), the base pass velocity, the bone buffers across three consecutive frames, the engine code that
binds them, and the in-game measurements. `tools/` holds the capture and analysis scripts.

## License

MIT; see [LICENSE](LICENSE). `src/descriptors.*` is adapted from ReShade's descriptor tracking example
(Copyright (C) 2021 Patrick Mours, BSD-3-Clause OR MIT); `third_party/reshade` and `third_party/imgui` carry their own
licenses. FINAL FANTASY VII REBIRTH is a trademark of Square Enix; this project is not affiliated with or endorsed by
Square Enix.
