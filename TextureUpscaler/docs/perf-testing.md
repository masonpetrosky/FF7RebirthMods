# Performance testing

Higher-resolution textures can cost performance in two ways: more VRAM (if the texture
streaming pool overflows, the engine drops mips and textures get blurrier) and streaming
hitches when bigger mips load. The tools below compare frame times and VRAM with the mod on
and off.

## Tools

| Script | Purpose |
| --- | --- |
| `tools/perf_capture.py <label> [seconds]` | Records PresentMon frame times plus nvidia-smi VRAM/utilization once per second. Needs a UAC approval for PresentMon. |
| `tools/perf_compare.py <label> [<label> ...]` | Per-run and averaged stats (avg FPS, 1%/0.1% lows, p99/p99.9 frame time, hitches/min, GPU busy, VRAM) with % change vs the first label. |
| `tools/mod_switch.py on\|off\|status` | Moves the mod between `Paks\~mods` and `Paks\~mods_disabled` (game closed). |

## Protocol

1. Nothing else using the GPU: pause any running upscale batch (it resumes from its cache).
2. Same settings for every run: same DLSS mode and frame generation state, no frame-rate cap
   (a cap hides differences; if one is unavoidable, compare **GPU busy ms** instead of FPS).
3. Two scenarios from the same save:
   - **static**: stand still with a detailed view for 60 s (steady-state GPU cost);
   - **traverse**: run a fixed route through an area with upscaled textures for 90 s (streaming).
4. Alternate configurations to cancel out thermal drift and shader-cache warm-up, restarting the
   game when switching (mods load at startup), for example:
   `baseline`, `upscaled`, `baseline`, `upscaled` (at least two runs each).
   Label runs per scenario, e.g. `static_baseline`, `static_upscaled`, `traverse_baseline`, ...
5. Compare: `python tools/perf_compare.py traverse_baseline traverse_upscaled`.

Differences smaller than the run-to-run spread (the ± values) are noise.
