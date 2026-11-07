Removes the grainy, screen-door dithering on trees, grass and other foliage in FINAL FANTASY VII REBIRTH when using DLSS, DLAA or FSR, and gives hair and fur soft edges.

**New in 0.3.1:**
- **New default threshold: 0.40 for hair and foliage.** Hair's **Solid above** (`Threshold`) was 0.50, so more of the hair is drawn solid and it looks fuller. The scenery **Opacity threshold** (`SceneryThreshold`) was 0.33, so leaves and grass are a little finer at the edges. Both are still adjustable in the ReShade overlay.

**Updating from 0.3.0:** extract the zip over the old files. The zip's ini replaces yours and brings the new defaults; if you changed settings, keep a copy of your ini first. From Vegetation Fix 0.2.0 or Hair Fix 0.1.0, see Upgrading in the README.

**Requires ReShade 6.x with add-on support.** Extract the zip into `...\FINAL FANTASY VII REBIRTH\End\Binaries\Win64`. See the README for details and settings.
