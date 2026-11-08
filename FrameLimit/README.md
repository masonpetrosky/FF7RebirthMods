# FF7 Rebirth Frame Limit

A [ReShade](https://reshade.me) add-on for **FINAL FANTASY VII REBIRTH** (PC) that replaces the game's frame-rate limit,
which counts rendered frames only and stops at 120, with one limit on the frames the display shows, the ones frame
generation makes included. By default that is 3 below the display's refresh rate: 237 at 240 Hz.

## The problem

The graphics options offer 30, 60, 90 and 120 fps and no unlimited entry. The option sets the console variable
`t.MaxFPS` to the option x 0.999 (119.88 for 120). Each game frame, Unreal's `UEngine::UpdateTimeAndHandleMaxTickRate`
first asks the game's tick-rate handler, NVIDIA Streamline's, which runs Reflex's sleep, gives Reflex no frame limit
and leaves the limit to the engine; the engine then waits until 1 / `t.MaxFPS` has passed since the frame before. So:

- Without frame generation the game never renders more than 120 frames a second, even on a 240 Hz display.
- With DLSS Frame Generation the limit still counts rendered frames only: the 120 option lets the display show up to
  240 frames a second (measured: 128.6 shown from about 64 rendered), and with NVIDIA's 3x or 4x override up to 360 or
  480, above the refresh rate of most displays.

`t.MaxFPS` in a user `Engine.ini` does not help: the game ignores that file (with `t.MaxFPS=0` there, the running game
still had 119.88 from its options).

## What the add-on does

It points the engine's one read of `t.MaxFPS` (in `UEngine::GetMaxTickRate`) at a value of its own: `Limit` divided by
the number of frames the display shows per rendered frame. It counts that number: generated frames pass through
ReShade's swap chain, so ReShade sees every shown frame, and a small routine in the Streamline handler's vtable counts
the engine's calls, one per game frame. It takes the nearest whole number of presents per game frame (1 to 4) once two
quarter-second windows in a row agree. With `Limit=237`:

| Frame generation | Shown per rendered frame | Rendered frames limited to | Shown frames limited to |
| --- | --- | --- | --- |
| off | 1 | 237 | 237 |
| DLSS Frame Generation | 2 | 118.5 | 237 |
| NVIDIA's 4x override | 4 | 59.25 | 237 |

Everything else that reads `t.MaxFPS`, the options menu among them, still sees the game's own value, so the menu keeps
working, saving and showing the option; the option just no longer limits anything while the add-on's limit is on.

While the game's own VSync is on and frame generation off, the game also sets `rhi.SyncInterval` to refresh rate /
option (2 for 120 fps at 240 Hz), which limits the frame rate too: the add-on lowers it to 1 while its limit is on, so
VSync waits for one refresh, not two. (With frame generation on, the game turns its own VSync off.)

Without the Streamline handler (another GPU vendor, or a game version where it is not found), the shown frames per
rendered frame follow the game's frame generation setting (`r.FrameGeneration.Method`): 2 when it is on.

## Installation

Put `FF7RebirthFrameLimit.addon64` and `FF7RebirthFrameLimit.ini` in
`...\FINAL FANTASY VII REBIRTH\End\Binaries\Win64`, next to ReShade 6.x with add-on support. To uninstall, delete both.

## Settings

`FF7RebirthFrameLimit.ini`:

| Key | Default | Meaning |
| --- | --- | --- |
| `Limit` | -1 | Frames per second the display shows, generated frames included. -1 = 3 below the refresh rate of the display the game is on; 0 = no limit. |
| `Enabled` | 1 | 1 = use `Limit`, 0 = the game's own frame-rate option. |
| `ToggleKey` | 0 | Virtual-key code that switches between `Limit` and the game's own limit for the session (121 = F10). |
| `TestLimit` | 0 | Testing: a second limit that `ToggleKey` also switches to (`Limit`, `TestLimit`, the game's). |
| `LogStats` | 0 | 1 = write the measured frame rates and the limit in use to `ReShade.log` every 5 seconds. |

The overlay's Add-ons tab shows the frame rates it measures (shown and rendered), the shown frames per rendered frame,
the limit in use and the game's own option, and has a checkbox for the limit and a slider for `Limit`, which saves it.

## Building

Visual Studio 2022 or newer with the C++ x64 tools. In the repository root, `.\build.ps1` builds every add-on (this one
is `build\FrameLimit\FF7RebirthFrameLimit.addon64`), and `.\package.ps1 FrameLimit` also assembles a zip in `dist\`.
`build\FrameLimit\frame_limit_test.exe` runs the add-on's searches and changes on the installed game's executable,
mapped as an image (none of its code runs), and tests the call counter.

## License

MIT; see [LICENSE](../LICENSE). `third_party/reshade` and `third_party/imgui` in the repository root carry their own
licenses. FINAL FANTASY VII REBIRTH is a trademark of Square Enix; this project is not affiliated with or endorsed by
Square Enix.
