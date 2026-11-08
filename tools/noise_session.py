"""Where the picture flickers at one spot, with each mod switched off in turn: noise_session.py OUT_DIR [--frames N]

Start it with the game in front, the party and the camera still, and hands off the controls. It records the screen in
HDR (ffmpeg ddagrab, 16-bit float scRGB, each new frame at most once) for each state, A/B/A:
  all on, then each switch off and all on again: Vegetation and Hair Fix (F8), its soft edges (F6), NPC Motion's
  fix (F7), its DLSS jitter phases (F5) and its DLSS pre-exposure (F4) (Shader Injector on Delete was the first
  switch until it was removed from the game on 2026-11-08)
letting each state settle --settle seconds first. A switched-off mod is always switched back on. Then, per state:
  - <state>-frame.png: the first frame in SDR (white 203 nits, extended Reinhard to 1000 nits, sRGB), full size;
  - <state>-mean.png: the mean of the frames, same mapping;
  - <state>-flicker.png: per-pixel temporal standard deviation of PQ-encoded luminance, white = flickers
    (0.04 PQ, about a tenth of the way from black to white in nits' perceptual steps);
  - <state>-std.npy: that deviation as float16, for crops and comparisons;
and for each switch, <n>-<switch>-share.png: the flicker that goes away with it off (white = that switch's flicker;
the lower of the two neighbouring all-on states minus the off state), and <n>-<switch>-removes.png the reverse. The
A/A floor (how much two all-on states differ) is printed with it: a share near the floor is no share. The raw frames
stay in OUT_DIR (about 1 GB a state) for full-size crops; delete them afterwards. Keys must match the installed inis
(CLAUDE.md "Keys already taken").
--toggle NAME=VK compares something a key switches ON instead (states 'base' and 'NAME-on'), e.g. --toggle rr=0x7C
--rounds 2 for NPC Motion's DLSS Ray Reconstruction against the game's DLSS (RayReconstructionKey=124, F13).
"""
import argparse
import glob
import json
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from perf_session import game_in_front, game_running, tap  # noqa: E402

FFMPEG = glob.glob(os.path.join(os.environ["LOCALAPPDATA"], "Microsoft", "WinGet", "Packages",
                                "Gyan.FFmpeg.Essentials_*", "ffmpeg-*-essentials_build", "bin", "ffmpeg.exe"))
W, H = 3840, 2160
FRAME_BYTES = W * H * 8
FLICKER_WHITE = 0.04  # PQ deviation drawn white
# name, label, virtual key, extended key (for the scan code)
SWITCHES = [
    ("vegetation", "Vegetation and Hair Fix", 0x77, False),  # F8
    ("softedges", "soft edges", 0x75, False),                # F6
    ("npcmotion", "NPC Motion fix", 0x76, False),            # F7
    ("jitter", "DLSS jitter phases", 0x74, False),           # F5
    ("preexposure", "DLSS pre-exposure", 0x73, False),       # F4
]


def record(path, frames, timeout=60):
    """Records the next `frames` new frames of the main display (no duplicates) as raw rgbaf16le."""
    cmd = [FFMPEG[0], "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
           "ddagrab=output_idx=0:framerate=240:output_fmt=16bit:dup_frames=0:draw_mouse=0",
           "-vf", "hwdownload,format=rgbaf16le", "-frames:v", str(frames), "-fps_mode", "passthrough",
           "-f", "rawvideo", path]
    start = time.time()
    subprocess.run(cmd, check=True, timeout=timeout)
    return time.time() - start


def pq(nits):
    """SMPTE ST 2084 encoding of luminance in nits: a perceptual scale (0..1)."""
    y = np.clip(nits / 10000.0, 0.0, 1.0)
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    yp = np.power(y, m1)
    return np.power((c1 + c2 * yp) / (1.0 + c3 * yp), m2)


def to_sdr(rgb_scrgb):
    """scRGB (1.0 = 80 nits) to an SDR sRGB image: white 203 nits, extended Reinhard to 1000 nits."""
    nits = np.maximum(rgb_scrgb, 0.0) * 80.0
    x = nits / 203.0
    white = 1000.0 / 203.0
    mapped = np.clip(x * (1.0 + x / (white * white)) / (1.0 + x), 0.0, 1.0)
    srgb = np.where(mapped <= 0.0031308, 12.92 * mapped, 1.055 * np.power(mapped, 1 / 2.4) - 0.055)
    return (np.clip(srgb, 0, 1) * 255 + 0.5).astype(np.uint8)


def gray(values, white):
    return Image.fromarray((np.clip(values / white, 0, 1) * 255 + 0.5).astype(np.uint8))


def frames_of(raw_path):
    n = os.path.getsize(raw_path) // FRAME_BYTES
    with open(raw_path, "rb") as f:
        for _ in range(n):
            yield np.frombuffer(f.read(FRAME_BYTES), np.float16).reshape(H, W, 4)[..., :3].astype(np.float32)


def analyse(raw_path, out_prefix):
    mean_rgb = np.zeros((H, W, 3), np.float64)
    mean_pq = np.zeros((H, W), np.float64)
    m2 = np.zeros((H, W), np.float64)
    n = 0
    for frame in frames_of(raw_path):
        if n == 0:
            Image.fromarray(to_sdr(frame)).save(out_prefix + "-frame.png")
        mean_rgb += frame
        v = pq((0.2126 * frame[..., 0] + 0.7152 * frame[..., 1] + 0.0722 * frame[..., 2]) * 80.0)
        n += 1
        delta = v - mean_pq
        mean_pq += delta / n
        m2 += delta * (v - mean_pq)
    mean_rgb /= max(n, 1)
    std = np.sqrt(m2 / max(n - 1, 1)).astype(np.float32)
    Image.fromarray(to_sdr(mean_rgb.astype(np.float32))).save(out_prefix + "-mean.png")
    gray(std, FLICKER_WHITE).save(out_prefix + "-flicker.png")
    np.save(out_prefix + "-std.npy", std.astype(np.float16))
    tiles = std.reshape(18, H // 18, 32, W // 32).mean(axis=(1, 3))
    order = np.argsort(tiles.ravel())[::-1][:6]
    worst = [{"x": int(c * (W // 32)), "y": int(r * (H // 18)), "mean": float(tiles[r, c])}
             for r, c in zip(*np.unravel_index(order, tiles.shape))]
    return {"frames": n, "median": float(np.median(std)), "mean": float(std.mean()),
            "p99": float(np.percentile(std, 99)), "worst_tiles_120px": worst}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out_dir")
    parser.add_argument("--frames", type=int, default=16, help="frames per state (default 16)")
    parser.add_argument("--settle", type=float, default=3.0, help="seconds after a switch (default 3)")
    parser.add_argument("--only", default="", help="comma-separated switches, e.g. vegetation,jitter (default all)")
    parser.add_argument("--toggle", action="append", default=[], metavar="NAME=VK[:ext]",
                        help="instead of the mods' switches, a key that switches something ON, e.g. rr=0x7C (NPC "
                             "Motion's Ray Reconstruction on F13); the states are then 'base' and '<NAME>-on'")
    parser.add_argument("--rounds", type=int, default=1, help="times through the switches (default 1)")
    parser.add_argument("--analyse-only", action="store_true", help="analyse the raw frames already in OUT_DIR")
    parser.add_argument("--capture-only", action="store_true", help="record, then leave the analysis for later")
    args = parser.parse_args()
    if not FFMPEG:
        sys.exit("ffmpeg not found (winget Gyan.FFmpeg.Essentials)")
    if args.toggle:
        switches = []
        for toggle in args.toggle:
            name, _, key = toggle.partition("=")
            switches.append((name, name, int(key.removesuffix(":ext"), 0), key.endswith(":ext")))
        base, suffix = "base", "-on"
    else:
        switches = [s for s in SWITCHES if not args.only or s[0] in args.only.split(",")]
        base, suffix = "all-on", "-off"
    switches = switches * args.rounds
    os.makedirs(args.out_dir, exist_ok=True)

    states = []
    if args.analyse_only:
        for raw in sorted(glob.glob(os.path.join(args.out_dir, "*.raw"))):
            states.append({"name": os.path.basename(raw)[3:-4], "raw": raw})
        base = "base" if any(s["name"] == "base" for s in states) else "all-on"
    else:
        if not game_running():
            sys.exit("the game is not running")
        print("Waiting for the game to be in front...", flush=True)
        while not game_in_front():
            time.sleep(0.25)
        print("Starting in 3 s; hands off the controls and the camera for about a minute.", flush=True)
        time.sleep(3.0)
        off = None

        def capture(name):
            time.sleep(args.settle)
            if not game_in_front():
                raise RuntimeError("the game left the front")
            path = os.path.join(args.out_dir, f"{len(states):02d}-{name}.raw")
            seconds = record(path, args.frames)
            states.append({"name": name, "raw": path, "seconds": seconds})
            print(f"  {name:18s} {args.frames} frames in {seconds:.2f} s", flush=True)

        try:
            capture(base)
            for key, label, vk, extended in switches:
                if not tap(vk, extended, 0):
                    raise RuntimeError("the game left the front")
                off = (key, label, vk, extended)
                capture(f"{key}{suffix}")
                if not tap(vk, extended, 600):
                    raise RuntimeError(f"could not switch {label} back")
                off = None
                capture(base)
        finally:
            if off is not None:
                if tap(off[2], off[3], 600):
                    print(f"{off[1]} switched back")
                else:
                    print(f"WARNING: {off[1]} is still switched; press its key in the game")
        print("Done; the game can be played again.", flush=True)
        if args.capture_only:
            return
        print("Analysing...", flush=True)

    for i, s in enumerate(states):
        s.update(analyse(s["raw"], os.path.join(args.out_dir, f"{i:02d}-{s['name']}")))
        print(f"  {s['name']:18s} frames {s['frames']:3d}  median {s['median']:.4f}  mean {s['mean']:.4f}  "
              f"p99 {s['p99']:.4f}", flush=True)

    def std_of(i):
        return np.load(os.path.join(args.out_dir, f"{i:02d}-{states[i]['name']}-std.npy")).astype(np.float32)

    def tiles_of(a):
        return a.reshape(18, H // 18, 32, W // 32).mean(axis=(1, 3))

    def share_of(i, neighbours):
        """Flicker present in both neighbouring states but not in state i, and the reverse."""
        on = [std_of(j) for j in neighbours]
        mine = std_of(i)
        return np.clip(np.minimum.reduce(on) - mine, 0, None), np.clip(mine - np.maximum.reduce(on), 0, None)

    # A/A floor: the same estimator with an all-on state in the middle (its all-on neighbours two states away), so a
    # switch's share can be told from the scatter of the measurement itself, tile by tile.
    ons = [i for i, s in enumerate(states) if s["name"] == base]
    floor_tiles, floor_means = [], []
    for a, k, b in zip(ons, ons[1:], ons[2:]):
        adds, removes = share_of(k, (a, b))
        floor_tiles.append(np.maximum(tiles_of(adds), tiles_of(removes)))
        floor_means.append(float(max(adds.mean(), removes.mean())))
        if len(floor_tiles) == 1:
            gray(adds, FLICKER_WHITE).save(os.path.join(args.out_dir, "aa-share.png"))
    floor_tile = np.max(floor_tiles, axis=0) if floor_tiles else np.zeros((18, 32))
    floor = max(floor_means) if floor_means else float("nan")
    print(f"A/A floor (an all-on state treated as a switch): mean {floor:.5f}, largest tile {floor_tile.max():.5f}")
    for i, s in enumerate(states):
        if s["name"] == base:
            continue
        neighbours = [j for j in (i - 1, i + 1) if 0 <= j < len(states) and states[j]["name"] == base]
        adds, removes = share_of(i, neighbours)
        switch = s["name"].rsplit("-", 1)[0]
        gray(adds, FLICKER_WHITE).save(os.path.join(args.out_dir, f"{i:02d}-{switch}-share.png"))
        gray(removes, FLICKER_WHITE).save(os.path.join(args.out_dir, f"{i:02d}-{switch}-removes.png"))
        s["adds_mean"], s["removes_mean"] = float(adds.mean()), float(removes.mean())
        over = tiles_of(adds) - floor_tile
        order = np.argsort(over.ravel())[::-1][:4]
        s["adds_tiles_120px"] = [{"x": int(c * (W // 32)), "y": int(r * (H // 18)), "over_floor": float(over[r, c])}
                                 for r, c in zip(*np.unravel_index(order, over.shape))]
        tiles_over = int((over > 0).sum())
        print(f"  {s['name']:18s} flicker only in the {base} states {s['adds_mean']:.5f}, only in this one "
              f"{s['removes_mean']:.5f} (floor {floor:.5f}); tiles above the floor: {tiles_over} of 576, most at "
              + ", ".join(f"({t['x']},{t['y']}) +{t['over_floor']:.4f}" for t in s["adds_tiles_120px"][:3]))
    summary = {"floor": floor, "floor_means": floor_means, "states": states}
    with open(os.path.join(args.out_dir, "states.json"), "w") as f:
        json.dump(summary, f, indent=1)


if __name__ == "__main__":
    main()
