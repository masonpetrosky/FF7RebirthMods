Removes FINAL FANTASY VII REBIRTH's 120 FPS cap and puts one limit on the frames your display shows instead, the frames DLSS Frame Generation makes included. By default that limit is 3 below your display's refresh rate (237 at 240 Hz, 141 at 144 Hz), so the game stays inside a G-SYNC or FreeSync display's range with or without frame generation. It also keeps **NVIDIA Reflex** on when frame generation is off, which the game doesn't do.

- **No more 120 cap.** The game's frame-rate option (30, 60, 90, 120) limits rendered frames only. Without frame generation the game now renders as fast as your GPU allows, up to the limit: 175 instead of 120 fps in our test at 4K.
- **Frame generation counted.** The add-on counts how many frames the display shows per rendered frame (1 without frame generation, 2 with DLSS Frame Generation, 3 or 4 with NVIDIA's multi frame generation override) and limits rendered frames to match: 237 shown from 118.5 rendered with DLSS Frame Generation.
- **Reflex without frame generation.** The game runs Reflex only together with frame generation. With the add-on it stays on: 16 instead of 24 ms from the start of a frame to the screen in our GPU-bound test, for about 2% of the frame rate. `Reflex=0` in the ini gives the game's behaviour back.
- The ReShade overlay shows the frame rates it measures, the limit in use, and what NVIDIA's driver reports: Reflex's low latency mode, G-SYNC and forced V-Sync.

With V-Sync forced in NVIDIA's settings and G-SYNC on, NVIDIA's driver itself holds the frame rate a little below the refresh rate whenever Reflex runs (about 225 at 240 Hz), so the add-on's limit then only matters above that.

**Requires ReShade 6.x with full add-on support** (DirectX 12). Extract the zip into `...\FINAL FANTASY VII REBIRTH\End\Binaries\Win64`. Tested with game build 23447986 (Steam, October 2026), ReShade 6.8.0 and an RTX 5070 Ti. See the README for details and settings.
