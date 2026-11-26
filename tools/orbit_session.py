"""What a switch does while the camera moves (smear, lag): orbit_session.py OUT_DIR --toggle NAME=VK [options]

Start it with the game in front, the party still, the camera where the test looks, and hands off the controls. For
each state (base, then NAME switched by its key, base again; --rounds times) it turns the camera with relative mouse
moves (--speed pixels every 1/100 s, to the right, for --seconds) while recording the screen in HDR (as
noise_session.py does: ffmpeg ddagrab, 16-bit float scRGB, each new frame once, at most --fps a second), keeps
recording --hold seconds after the turn (the history catching up), then turns the camera back without recording, so
that every state sees the same turn from the same start. With --move KEYS (e.g. W:S) it instead holds the first key
(Cloud runs forward, the camera following) and returns with the second; --speed 0 with it: no mouse turn. A switched
state is always switched back, and a held key always let go.
Then, per state, <state>-NN.png for every recorded frame (SDR, noise_session.py's mapping, half size; the raw frames
stay in OUT_DIR for full-size crops: delete them afterwards), and printed: how far the picture moved between the first
and last frame of the turn (the mean absolute difference of PQ luminance; near 0: the camera did not turn, so the game
took no injected mouse input), and each frame's mean absolute PQ difference from the same frame of the base state
before it (where the switched state differs as the camera moves; the two base states give its floor).
The ray tracing's reflections (stage 6, ReflectionsKey=128, F17, in its test sessions' ini: ours at the start):
--toggle gamerefl=0x80 (base ours, gamerefl-on the game's).
"""
import argparse
import ctypes
import json
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from noise_session import FFMPEG, FRAME_BYTES, H, W, frames_of, pq, to_sdr  # noqa: E402
from perf_session import game_in_front, game_running, tap  # noqa: E402

user32 = ctypes.WinDLL("user32", use_last_error=True)
MOUSEEVENTF_MOVE = 0x0001
STEP_SECONDS = 0.01


def turn(speed, seconds):
    """Relative mouse moves to the right (speed < 0: to the left), every STEP_SECONDS, while the game is in front."""
    end = time.perf_counter() + seconds
    next_step = time.perf_counter()
    while time.perf_counter() < end:
        if game_in_front():
            user32.mouse_event(MOUSEEVENTF_MOVE, int(speed), 0, 0, 0)
        next_step += STEP_SECONDS
        time.sleep(max(0.0, next_step - time.perf_counter()))


KEYEVENTF_KEYUP = 0x0002


def key(vk, down):
    """A letter key (W, A, S, D) down or up in the game, by its scan code as the keyboard sends it."""
    scan = user32.MapVirtualKeyW(vk, 0)
    user32.keybd_event(vk, scan, 0 if down else KEYEVENTF_KEYUP, 0)


def move(args, forward):
    """The turn (--speed) and, with --move, its key held (the first going, the second coming back)."""
    held = None
    if args.move:
        held = ord(args.move.split(":")[0 if forward else 1].upper())
        while not game_in_front():
            time.sleep(0.1)
        key(held, True)
    try:
        turn(args.speed if forward else -args.speed, args.seconds)
    finally:
        if held is not None:
            key(held, False)


def record_turn(path, args):
    """Records while the camera turns (and Cloud runs, with --move) and for --hold seconds after; returns the frames
    recorded."""
    frames = int((args.seconds + args.hold) * args.fps) + 2
    cmd = [FFMPEG[0], "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
           f"ddagrab=output_idx=0:framerate={args.fps}:output_fmt=16bit:dup_frames=0:draw_mouse=0",
           "-vf", "hwdownload,format=rgbaf16le", "-frames:v", str(frames), "-fps_mode", "passthrough",
           "-f", "rawvideo", path]
    recorder = subprocess.Popen(cmd)
    time.sleep(0.3)  # ffmpeg's first frame
    move(args, True)
    try:
        recorder.wait(timeout=args.hold + 30)
    except subprocess.TimeoutExpired:
        recorder.kill()
    return os.path.getsize(path) // FRAME_BYTES if os.path.exists(path) else 0


def luminance_pq(frame):
    return pq((0.2126 * frame[..., 0] + 0.7152 * frame[..., 1] + 0.0722 * frame[..., 2]) * 80.0)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out_dir")
    parser.add_argument("--toggle", required=True, metavar="NAME=VK[:ext]", help="the switch, e.g. gamerefl=0x80")
    parser.add_argument("--seconds", type=float, default=2.0, help="the turn's length (default 2)")
    parser.add_argument("--speed", type=float, default=6.0, help="mouse pixels every 1/100 s (default 6)")
    parser.add_argument("--hold", type=float, default=1.0, help="seconds recorded after the turn (default 1)")
    parser.add_argument("--fps", type=int, default=10, help="frames recorded a second at most (default 10)")
    parser.add_argument("--settle", type=float, default=3.0, help="seconds after a switch (default 3)")
    parser.add_argument("--rounds", type=int, default=1, help="times through base and switched (default 1)")
    parser.add_argument("--move", default="", metavar="GO:BACK",
                        help="movement keys held while turning, going and coming back (e.g. W:S; default none)")
    args = parser.parse_args()

    name, _, key = args.toggle.partition("=")
    vk_text, _, ext = key.partition(":")
    vk, extended = int(vk_text, 0), ext == "ext"
    if not FFMPEG:
        sys.exit("ffmpeg (Gyan.FFmpeg.Essentials, winget) not found")
    if not game_running():
        sys.exit("the game is not running")
    os.makedirs(args.out_dir, exist_ok=True)
    states = []
    for r in range(args.rounds):
        states += ["base", name + "-on"]
    states.append("base")
    labels, switched = [], False
    try:
        for i, state in enumerate(states):
            want_on = state != "base"
            if want_on != switched:
                if not tap(vk, extended, 600):
                    sys.exit("the game never came back to the front")
                switched = want_on
                time.sleep(args.settle)
            label = f"{i}-{state}"
            raw = os.path.join(args.out_dir, label + ".raw")
            n = record_turn(raw, args)
            move(args, False)  # back to the start, not recorded
            time.sleep(1.0)
            labels.append((label, raw, n))
            print(f"{label}: {n} frames", flush=True)
    finally:
        if switched:
            print("switching back", flush=True)
            tap(vk, extended, 600)
    report = {}
    base_frames = None
    for label, raw, n in labels:
        lum = []
        for j, frame in enumerate(frames_of(raw)):
            Image.fromarray(to_sdr(frame)[::2, ::2]).save(os.path.join(args.out_dir, f"{label}-{j:02d}.png"))
            lum.append(luminance_pq(frame).astype(np.float32))
        turned = int(args.seconds * args.fps)
        moved = float(np.mean(np.abs(lum[min(turned, len(lum) - 1)] - lum[0]))) if lum else 0.0
        entry = {"frames": n, "moved": moved}
        if label.endswith("-base"):
            if base_frames is not None:
                entry["from_base"] = [float(np.mean(np.abs(a - b))) for a, b in zip(lum, base_frames)]
            base_frames = lum
        elif base_frames is not None:
            entry["from_base"] = [float(np.mean(np.abs(a - b))) for a, b in zip(lum, base_frames)]
        report[label] = entry
        print(f"{label}: the picture moved {moved:.4f} PQ through the turn"
              + ("" if "from_base" not in entry else
                 ", from the base before it per frame " + " ".join(f"{d:.4f}" for d in entry["from_base"])),
              flush=True)
    with open(os.path.join(args.out_dir, "orbit.json"), "w") as f:
        json.dump({"args": vars(args), "states": report}, f, indent=1)


if __name__ == "__main__":
    main()
