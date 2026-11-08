# FF7 Rebirth Frame Limit

A [ReShade](https://reshade.me) add-on for **FINAL FANTASY VII REBIRTH** (PC) that replaces the game's frame-rate limit,
which counts rendered frames only and stops at 120, with one limit on the frames the display shows, the ones frame
generation makes included. By default that is 3 below the display's refresh rate: 237 at 240 Hz. It also keeps
**NVIDIA Reflex** on when frame generation is off, which the game doesn't do.

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

## NVIDIA Reflex without frame generation

The game has no Reflex option. It sends Reflex's latency markers whenever Reflex is available, but each frame it gives
Reflex Low Latency mode only while DLSS frame generation is on: the mode comes from the same check that gates frame
generation's own work (frame generation available and enabled), so with frame generation off Reflex is off. With
`Reflex=1` (the default) the add-on sends that one call to a small routine of its own, which answers "on" while Reflex
is to stay on and asks the game's check otherwise. Frame generation itself is left alone. Reflex then keeps the render
queue short whenever the GPU is the limit, which lowers the delay from input to the screen. With V-Sync forced on in
NVIDIA's settings and G-SYNC active, the driver also holds the frame rate a little below the refresh rate while
Reflex runs, as it already does with frame generation.

The overlay shows what the driver itself reports for the game's device (`NvAPI_D3D_GetSleepStatus`, once a second):
whether Reflex's low latency mode is on, whether G-SYNC is active and whether V-Sync is forced in NVIDIA's settings.
`ReShade.log` gets a line whenever that changes.

## Measured

2026-11-08, RTX 5070 Ti, 4K 240 Hz G-SYNC display with V-Sync forced in the NVIDIA app, the camera on the sky so the
GPU can go fast, PresentMon over two rounds of 8 s per state (`tools/framelimit_session.py`, all checks passed):

| Frame generation | State | Shown fps | Rendered fps | GPU busy |
| --- | --- | --- | --- | --- |
| off | the game's 120 option | 119.8 | 119.8 | 72% |
| off | Limit 237 | 175.4 | 175.4 | 100% (the GPU's limit in that view) |
| off | TestLimit 100 | 100.0 | 100.0 | 60% |
| DLSS-G | the game's 120 option | 224.6 | 112.3 | 91% |
| DLSS-G | Limit 237 | 224.9 | 112.5 | 91% |
| DLSS-G | TestLimit 100 | 100.0 | 50.0 | 41% |

The add-on counted 2 shown frames per rendered frame within 2 s of frame generation being switched on. With frame
generation, neither limit was reached (112.5 rendered is below both 118.5 and 119.88) and the GPU had 9% to spare:
NVIDIA's driver held the shown frames at about 225 itself. It does that while V-Sync is forced in its control panel or
app, G-SYNC is on and Reflex runs (frame generation turns Reflex on), a little below the refresh rate. `Limit` only
decides above that cap, for example with V-Sync off. Frame pacing under a limit is the same as under the game's own:
the add-on changes the value Unreal waits for, not the wait.

With V-Sync off in the NVIDIA app (same view, DLSS-G on), the limit holds: 236.9 shown from 118.5 rendered (GPU 92-93%
busy), against 240.0 shown from 120.0 rendered (94%) under the game's 120 option, right at the display's refresh rate.
PresentMon cannot tell whether frames tear here: with frame generation its display times put a fifth to a third of the
frames less than one refresh after the one before, with V-Sync on (where nothing can tear) as much as with it off.

Reflex without frame generation (same view, frame generation off, V-Sync forced on and G-SYNC active as the driver
reported, `ReflexKey` switching every 8 s, PresentMon's PC latency from Reflex's own markers,
`tools/framelimit_session.py --switch reflex`, all checks passed):

| Reflex | Shown fps | GPU busy | PC latency median | Driver: low latency mode |
| --- | --- | --- | --- | --- |
| kept on by the add-on | 169.2, 170.8 | 99-100% | 15.6, 15.8 ms | on |
| the game's (off) | 173.2, 172.8 | 100% | 23.6, 24.0 ms | off |

Kept on, Reflex takes about 8 ms (a third) off the time from the simulation's start to the frame on the screen, for
about 2% of the frame rate, and the driver's own report followed each switch within a second. Frame generation stayed
off (one frame shown per frame rendered in every state).

## Installation

Extract the zip into `...\FINAL FANTASY VII REBIRTH\End\Binaries\Win64`, next to ReShade 6.x with add-on support:
`FF7RebirthFrameLimit.addon64` and `FF7RebirthFrameLimit.ini` go there. To uninstall, delete both.

## Compatibility

- Tested with game build 23447986 (Steam, October 2026), ReShade 6.8.0 and an RTX 5070 Ti.
- Works next to the other add-ons in this repository, RenoDX HDR and Shader Injector.
- The frame limit works on any GPU. Without NVIDIA's Streamline (AMD, Intel), the shown frames per rendered frame
  follow the game's frame generation setting; Reflex needs an NVIDIA GPU.
- Use one frame-rate unlocker at a time: another mod that changes the game's limit (for example a custom cap in
  another fix) and this add-on would both decide.
- If a game update moves the code the add-on looks for, it changes nothing and says so in `ReShade.log` and the
  overlay; the game then keeps its own limit and Reflex behaviour.

## Settings

`FF7RebirthFrameLimit.ini`:

| Key | Default | Meaning |
| --- | --- | --- |
| `Limit` | -1 | Frames per second the display shows, generated frames included. -1 = 3 below the refresh rate of the display the game is on; 0 = no limit. |
| `Enabled` | 1 | 1 = use `Limit`, 0 = the game's own frame-rate option. |
| `ToggleKey` | 0 | Virtual-key code that switches between `Limit` and the game's own limit for the session (33 = Page Up). |
| `TestLimit` | 0 | Testing: a second limit that `ToggleKey` also switches to (`Limit`, `TestLimit`, the game's). |
| `LogStats` | 0 | 1 = write the measured frame rates and the limit in use to `ReShade.log` every 5 seconds. |
| `Reflex` | 1 | 1 = NVIDIA Reflex on also without frame generation, 0 = the game's (on only with frame generation). |
| `ReflexKey` | 0 | Virtual-key code that switches `Reflex` for the session, for comparisons. |

The overlay's Add-ons tab shows the frame rates it measures (shown and rendered), the shown frames per rendered frame,
the limit in use, the game's own option, Reflex's state and the driver's report. It has checkboxes for the limit and
for Reflex (for the session) and a slider for `Limit`, which saves it.

## Building

Visual Studio 2022 or newer with the C++ x64 tools. In the repository root, `.\build.ps1` builds every add-on (this one
is `build\FrameLimit\FF7RebirthFrameLimit.addon64`), and `.\package.ps1 FrameLimit` also assembles a zip in `dist\`.
`build\FrameLimit\frame_limit_test.exe` runs the add-on's searches and changes on the installed game's executable,
mapped as an image (none of its code runs), and tests the call counter.

## License

MIT; see [LICENSE](../LICENSE). `third_party/reshade` and `third_party/imgui` in the repository root carry their own
licenses. FINAL FANTASY VII REBIRTH is a trademark of Square Enix; this project is not affiliated with or endorsed by
Square Enix.
