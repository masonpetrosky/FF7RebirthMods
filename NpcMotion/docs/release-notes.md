Fixes the smeared outlines of townspeople and other characters with DLSS, TAA and motion blur in FINAL FANTASY VII REBIRTH, and adds DLSS **Balanced** to the game's resolution options. Characters whose animation the game throttles no longer carry motion vectors for movement that didn't happen, and the game's animation budget may interpolate them so they move every frame. It also gives DLSS the 32 jitter positions and the real pre-exposure that NVIDIA's DLSS Programming Guide asks for.

**New in 0.2.0**

- **DLSS Balanced (58%)**: the graphics options' dynamic resolution lower and upper limits offer 58% between 50% and 66%. Set both to 58% for Balanced: the game renders at 1/1.7 of the output size (2259x1271 at 4K) and runs DLSS in its Balanced mode. The game's own code already had this step; only its menu left it out. `BalancedStep=0` in the ini turns it off.
- The ReShade overlay shows the DLSS mode and render size the game uses, e.g. "Balanced: render 2259x1271 -> 3840x2160".
- `dxcompiler.dll` is also found in the Vegetation and Hair Fix's folder (`FF7RebirthVegetationAndHairFix`).

**Requires ReShade 6.x with full add-on support** (DirectX 12). Extract the zip into `...\FINAL FANTASY VII REBIRTH\End\Binaries\Win64`; the zip's ini replaces yours, so keep a copy if you changed it. Tested with game build 23447986 (Steam, October 2026) and ReShade 6.8.0. See the README for details and settings.
