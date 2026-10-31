"""Decode a UE4 velocity texture dump (rdc_texdump.py .bin + .txt) and write a brightness-only view.

Usage: velocity_view.py <dump base path without extension> <out.png> [scale px for full brightness, default 8]
UE 4.26 writes velocity as R16G16_UNORM: 0,0 = nothing written (the reader reconstructs camera motion from depth),
otherwise v = (encoded - 32767/65535) / (0.499 * 0.5) in screen units (x: -2..2 across the view).
The image: black = nothing written, dark grey = written but (almost) still, brighter = more motion.
Prints statistics: share of pixels written, motion in pixels.
"""
import sys

import numpy as np
from PIL import Image

base, out = sys.argv[1], sys.argv[2]
scale = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0
w, h, fmt = open(base + ".txt").read().split()
w, h = int(w), int(h)
raw = np.fromfile(base + ".bin", dtype=np.uint16)
if fmt.startswith("R16G16B16A16"):
    raw = raw.reshape(h, w, 4)[..., :2]
elif fmt.startswith("R16G16"):
    raw = raw.reshape(h, w, 2)
else:
    raise SystemExit("unexpected format " + fmt)
if "FLOAT" in fmt:
    v = raw.view(np.float16).astype(np.float32)
    written = np.any(v != 0, axis=-1)
else:
    enc = raw.astype(np.float32) / 65535.0
    written = np.any(raw != 0, axis=-1)
    v = (enc - 32767.0 / 65535.0) / (0.499 * 0.5)
# screen units (-1..1 across the view) to pixels; UE's velocity is in clip space units: x * 0.5 * width
px = np.stack([v[..., 0] * 0.5 * w, v[..., 1] * 0.5 * h], -1)
mag = np.hypot(px[..., 0], px[..., 1])
mag[~written] = 0
img = np.where(written, 40 + 215 * np.clip(mag / scale, 0, 1), 0).astype(np.uint8)
Image.fromarray(img).save(out)
share = written.mean() * 100
if written.any():
    m = mag[written]
    print("%s %dx%d: %.2f%% of pixels written; motion px median %.2f p90 %.2f p99 %.2f max %.2f"
          % (fmt, w, h, share, np.median(m), np.percentile(m, 90), np.percentile(m, 99), m.max()))
else:
    print("%s %dx%d: nothing written" % (fmt, w, h))
