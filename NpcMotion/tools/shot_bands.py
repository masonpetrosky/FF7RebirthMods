"""Texture detail in screenshots of one still view: shot_bands.py <A> <B> [<A2>] [--motion PQ] [--crop X Y SIZE OUT]

A and B are screenshots with one setting changed; A2 (recommended) repeats A's setting, which gives the noise floor
and the motion mask. Inputs are .rgb48 files from tools/decode_png16.ps1 (ReShade's 16-bit HDR screenshots, PQ /
BT.2020). Needs numpy (and Pillow for --crop).

The shots are compared as PQ-encoded luminance (even perceptual steps), split with an FFT into octave bands, per
64-pixel tile:
  energy  mean squared band-passed luminance: texture detail at that scale (A/B > 1 = more detail in A)
  D(X,Y)  mean squared difference of the band-passed shots: any change, not only sharpness
A tile counts when it lies inside the frame border, does not move (no 16-pixel block mean changes by --motion or more
between A and A2, or between A and B without A2) and is textured (fine detail above the median of such tiles).
Judge motion from shots with the same setting: a setting that changes shading also changes coarse brightness, and a
mask built from A and B hides exactly the tiles it affects.

--crop X Y SIZE OUT writes, instead of measuring, the SIZE x SIZE pixel square whose top-left corner is (X, Y) of A, B
(and A2) side by side to the image OUT, tone-mapped for SDR and enlarged 3x.
"""
import argparse
import os

import numpy as np

M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
TILE, BLOCK = 64, 16
BANDS = (("2-4 px", 0, 1), ("4-8 px", 1, 2), ("8-16 px", 2, 4), ("16-32 px", 4, 8))  # (name, sigma in, sigma out)


def pq_to_nits(code):
    p = np.power(np.clip(code, 0, 1), 1 / M2)
    return 10000 * np.power(np.maximum(p - C1, 0) / (C2 - C3 * p), 1 / M1)


def nits_to_pq(nits):
    y = np.power(np.clip(nits / 10000, 0, 1), M1)
    return np.power((C1 + C2 * y) / (1 + C3 * y), M2)


def load(path):
    with open(path + ".txt") as f:
        width, height = (int(v) for v in f.read().split())
    rgb = pq_to_nits(np.fromfile(path, dtype="<u2").reshape(height, width, 3) / 65535.0).astype(np.float32)
    luminance = 0.2627 * rgb[..., 0] + 0.6780 * rgb[..., 1] + 0.0593 * rgb[..., 2]  # BT.2020
    return rgb, nits_to_pq(luminance).astype(np.float32)


def mean_blocks(x, size):
    h, w = x.shape[0] // size * size, x.shape[1] // size * size
    return x[:h, :w].reshape(h // size, size, w // size, size).mean(axis=(1, 3))


def save_crop(shots, x0, y0, size, out):
    from PIL import Image

    to709 = np.array([[1.6605, -0.5876, -0.0728], [-0.1246, 1.1329, -0.0083], [-0.0182, -0.1006, 1.1187]], np.float32)
    first = shots[0][0]
    y = 0.2627 * first[..., 0] + 0.6780 * first[..., 1] + 0.0593 * first[..., 2]
    key = float(np.exp(np.mean(np.log(y + 1e-3))))  # the log-average luminance of A maps to 0.18
    white = float(np.percentile(0.18 * y / key, 99.7))
    tiles = []
    for rgb, _ in shots:
        x = np.clip(0.18 * (rgb[y0:y0 + size, x0:x0 + size] @ to709.T) / key, 0, None)
        x = x * (1 + x / (white * white)) / (1 + x)
        s = np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(np.maximum(x, 0.0031308), 1 / 2.4) - 0.055)
        tile = Image.fromarray((np.clip(s, 0, 1) * 255 + 0.5).astype(np.uint8))
        tiles.append(tile.resize((3 * size, 3 * size), Image.NEAREST))
    side = Image.new("RGB", (len(tiles) * (3 * size + 12) - 12, 3 * size))
    for i, tile in enumerate(tiles):
        side.paste(tile, (i * (3 * size + 12), 0))
    side.save(out)
    print("crop written to %s (left to right: %s)" % (out, ", ".join(["A", "B", "A2"][:len(tiles)])))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("a2", nargs="?")
    parser.add_argument("--motion", type=float, default=0.01, help="block mean change (PQ) that counts as motion")
    parser.add_argument("--crop", nargs=4, metavar=("X", "Y", "SIZE", "OUT"))
    args = parser.parse_args()

    shots = [load(p) for p in (args.a, args.b, args.a2) if p]
    lum = [s[1] for s in shots]
    height, width = lum[0].shape
    if any(x.shape != (height, width) for x in lum):
        parser.error("the shots differ in size")
    if args.crop:
        x0, y0, size = (int(v) for v in args.crop[:3])
        if x0 < 0 or y0 < 0 or size < 1 or x0 + size > width or y0 + size > height:
            parser.error("--crop: the square must lie inside the %dx%d frame" % (width, height))
        save_crop(shots, x0, y0, size, args.crop[3])
        return
    rows, cols = height // TILE, width // TILE
    border = np.zeros((rows, cols), bool)
    border[1:-1, 1:-1] = True
    reference = lum[2] if len(lum) == 3 else lum[1]
    k = TILE // BLOCK
    change = np.abs(mean_blocks(lum[0], BLOCK) - mean_blocks(reference, BLOCK))
    change = change[: rows * k, : cols * k].reshape(rows, k, cols, k).max(axis=(1, 3))
    static = border & (change < args.motion)
    if not static.any():
        raise SystemExit("no still tiles: the shots do not show the same view (or --motion is too small)")
    print("%dx%d, %d tiles: %d inside the border, %d of them without motion (%s, block change < %.3f PQ)" % (
        width, height, rows * cols, border.sum(), static.sum(), "A vs A2" if len(lum) == 3 else "A vs B", args.motion))
    if len(lum) == 2:
        print("note: no A2, so the motion mask comes from A and B and can hide what the setting changed")

    fy = np.fft.fftfreq(height).astype(np.float32)[:, None]
    fx = np.fft.rfftfreq(width).astype(np.float32)[None, :]
    f2 = fx * fx + fy * fy
    spectra = [np.fft.rfft2(x) for x in lum]
    textured = None
    for name, s_in, s_out in BANDS:
        gain = (np.exp(-2 * np.pi ** 2 * s_in * s_in * f2) if s_in else 1) - np.exp(-2 * np.pi ** 2 * s_out ** 2 * f2)
        band = [np.fft.irfft2(sp * gain, s=(height, width)).astype(np.float32) for sp in spectra]
        energy = [mean_blocks(x * x, TILE)[:rows, :cols] for x in band]
        if textured is None:  # chosen once, from the finest band of all shots alike (no selection bias)
            detail = sum(energy) / len(energy)
            textured = static & (detail > np.median(detail[static]))
            if not textured.any():
                raise SystemExit("no textured still tiles")
            print("textured static tiles: %d\n" % textured.sum())
        t = textured
        ratio_ab = energy[0][t] / energy[1][t]
        line = "%-9s energy A/B median %.3f (10th-90th pct %.3f-%.3f)" % (
            name, np.median(ratio_ab), *np.percentile(ratio_ab, [10, 90]))
        if len(lum) == 3:
            ratio_noise = energy[0][t] / energy[2][t]
            both = 0.5 * (energy[0][t] + energy[2][t]) / energy[1][t]
            d_ab = mean_blocks((band[0] - band[1]) ** 2, TILE)[:rows, :cols][t]
            d_aa = mean_blocks((band[0] - band[2]) ** 2, TILE)[:rows, :cols][t]
            line += " | A,A2 mean/B %.3f | noise A/A2 %.3f (%.3f-%.3f) | D(A,B)/D(A,A2) %.2f" % (
                np.median(both), np.median(ratio_noise), *np.percentile(ratio_noise, [10, 90]),
                np.median(d_ab / d_aa))
        print(line)


if __name__ == "__main__":
    main()
