"""Checks the Frame Limit add-on in the running game: framelimit_session.py OUT_DIR [--segment S] [--rounds N]

Start it with the game in front, the party standing still and hands off the controls. It records PresentMon for the
whole run and presses the add-on's ToggleKey to go through its three states in turn, each for S seconds: Limit,
TestLimit, the game's own limit. The installed FF7RebirthFrameLimit.ini needs ToggleKey, TestLimit and LogStats=1;
the script reads them from there, and the state the add-on is in from ReShade.log. At the start it takes a ReShade
screenshot (M6, Page Down) of the measured view.

Then it prints, per state (the first 1.5 s after each switch left out), the frames shown and rendered per second from
PresentMon (a rendered frame keeps the GPU busy for milliseconds, a generated one for well under one; the two kinds
alternate with DLSS Frame Generation) and the add-on's own counts from ReShade.log, and checks:
  - TestLimit: frames shown within 1% of TestLimit. Pick one the GPU reaches anywhere, e.g. 100 with frame generation
    (50 rendered).
  - Limit: frames shown never more than 1% above Limit, and within 2% of it if the game's own state showed more.
  - the game's: rendered frames at most its 120 option (119.88) + 1%.
  - each state: rendered frames = shown / (shown per rendered frame), within 2%.
Writes OUT_DIR/presentmon.csv, timeline.json and reshade_framelimit.log.
"""
import argparse
import csv
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from perf_session import GAME, GAME_DIR, PRESENTMON, game_in_front, game_running, qpc, qpc_frequency, tap  # noqa: E402

SESSION = "FF7RebirthFrameLimitCheck"
INI = os.path.join(GAME_DIR, "FF7RebirthFrameLimit.ini")
LOG = os.path.join(GAME_DIR, "ReShade.log")
GAME_OPTION = 119.88  # the game's 120 fps option: t.MaxFPS = 120 x 0.999
SETTLE = 1.5          # seconds left out after each switch
STATES = ["limit", "test", "game"]  # the order ToggleKey goes through them


def read_ini():
    values = {}
    with open(INI, encoding="utf-8-sig") as f:
        for line in f:
            m = re.match(r"\s*(\w+)\s*=\s*(-?\d+)", line)
            if m:
                values[m.group(1)] = int(m.group(2))
    return values


def log_lines():
    with open(LOG, encoding="utf-8", errors="replace") as f:
        return [line.rstrip("\n") for line in f if "[FrameLimit]" in line]


def current_state(lines):
    """The add-on's state from its log: the last switch, else the state it started in."""
    state = None
    for line in lines:
        if "frame-rate limiter (rva" in line:
            state = "game" if "(off: Enabled=0" in line else "limit"
        elif "] Limit off:" in line:
            state = "game"
        elif "] Limit on:" in line:
            state = "test" if "(TestLimit)" in line else "limit"
    return state


def log_seconds(line):
    """Seconds since midnight of a ReShade.log line (HH:MM:SS:mmm)."""
    m = re.match(r"(\d\d):(\d\d):(\d\d):(\d\d\d)", line)
    if not m:
        return None
    h, mi, s, ms = (int(g) for g in m.groups())
    return h * 3600 + mi * 60 + s + ms / 1000.0


def seconds_of_day(t):
    d = datetime.fromtimestamp(t)
    return d.hour * 3600 + d.minute * 60 + d.second + d.microsecond / 1e6


def number(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out_dir")
    parser.add_argument("--segment", type=float, default=10.0, help="seconds per state (default 10)")
    parser.add_argument("--rounds", type=int, default=2, help="rounds through the three states (default 2)")
    args = parser.parse_args()

    if not game_running():
        sys.exit(f"{GAME} is not running")
    ini = read_ini()
    key, test_limit, limit = ini.get("ToggleKey", 0), ini.get("TestLimit", 0), ini.get("Limit", -1)
    if key == 0 or test_limit <= 0 or ini.get("LogStats", 0) == 0:
        sys.exit(f"{INI} needs ToggleKey, TestLimit and LogStats=1 (has {ini})")
    extended = 0x21 <= key <= 0x2E  # the navigation keys
    state = current_state(log_lines())
    if state is None:
        sys.exit("ReShade.log has no Frame Limit startup line: is the add-on installed and working?")
    os.makedirs(args.out_dir, exist_ok=True)

    print("Waiting for the game to be in front...", flush=True)
    while not game_in_front():
        time.sleep(0.25)
    total = args.segment * 3 * args.rounds + 15
    print(f"Starting in 5 s; about {total / 60:.1f} minutes with hands off the controls.", flush=True)
    time.sleep(5.0)

    timeline = {"qpc_frequency": qpc_frequency(), "segments": [], "presses": [], "ini": ini}
    pm = subprocess.Popen([PRESENTMON, "--process_name", GAME, "--output_file",
                           os.path.join(args.out_dir, "presentmon.csv"), "--qpc_time", "--no_console_stats",
                           "--session_name", SESSION, "--stop_existing_session"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def press():
        nonlocal state
        if not tap(key, extended, 600):
            raise RuntimeError("the game left the front")
        timeline["presses"].append(qpc())
        state = STATES[(STATES.index(state) + 1) % 3]
        time.sleep(0.3)

    try:
        if tap(0x22, True, 0):  # ReShade screenshot (M6, Page Down) of the view, while PresentMon starts
            timeline["screenshot"] = qpc()
        time.sleep(4.0)
        while state != "limit":
            press()
        for _ in range(args.rounds):
            for expected in STATES:
                start, wall = qpc(), time.time()
                print(f"  {expected:6s}", end="", flush=True)
                time.sleep(args.segment)
                timeline["segments"].append({"state": expected, "start": start, "end": qpc(),
                                             "wall_start": wall, "wall_end": time.time()})
                print(" done", flush=True)
                press()
    finally:
        presses = 0
        while state != "limit" and presses < 3:  # leave it on Limit
            try:
                press()
            except RuntimeError:
                print("WARNING: could not switch the add-on back to Limit; press its key in the game")
                break
            presses += 1
        subprocess.run([PRESENTMON, "--terminate_existing_session", "--session_name", SESSION],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            pm.wait(timeout=15)
        except subprocess.TimeoutExpired:
            pm.kill()
        with open(os.path.join(args.out_dir, "timeline.json"), "w") as f:
            json.dump(timeline, f, indent=1)
    print("Done; the game can be played again.", flush=True)
    time.sleep(1.0)  # the add-on's last stats line
    lines = log_lines()
    with open(os.path.join(args.out_dir, "reshade_framelimit.log"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    report(args.out_dir, timeline, lines, limit, test_limit)


def report(out_dir, timeline, lines, limit, test_limit):
    freq = timeline["qpc_frequency"]
    rows = []
    with open(os.path.join(out_dir, "presentmon.csv"), encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            if r["Application"].lower() == GAME:
                rows.append((int(r["TimeInQPC"]), number(r["MsGPUBusy"]), number(r["MsBetweenDisplayChange"])))
    rows.sort()
    if limit < 0:  # 3 below the display's refresh rate: the add-on logs the rate
        rates = [int(m.group(1)) for line in lines for m in [re.search(r"display runs at (\d+) Hz", line)] if m]
        limit = rates[-1] - 3 if rates and rates[-1] > 3 else 0
    results = {s: [] for s in STATES}
    print(f"\n{'state':6s} {'shown':>7s} {'rendered':>9s} {'ratio':>6s}  {'display p1/p50/p99 ms':>22s}   add-on")
    for seg in timeline["segments"]:
        a, b = seg["start"] + int(SETTLE * freq), seg["end"]
        seconds = (b - a) / freq
        inside = [r for r in rows if a <= r[0] < b]
        rendered = sum(1 for r in inside if r[1] is not None and r[1] > 1.0)
        shown = len(inside) / seconds
        rendered_fps = rendered / seconds
        ratio = shown / rendered_fps if rendered_fps else 0.0
        display = sorted(r[2] for r in inside if r[2] is not None and r[2] > 0)
        pct = (lambda p: display[min(len(display) - 1, int(p * len(display)))]) if display else (lambda p: 0.0)
        w0, w1 = seconds_of_day(seg["wall_start"] + SETTLE), seconds_of_day(seg["wall_end"])
        stats = [line for line in lines if "Shown " in line and (t := log_seconds(line)) is not None and w0 <= t <= w1 + 0.5]
        addon = ""
        if stats:
            m = re.search(r"Shown ([\d.]+) fps, rendered ([\d.]+) fps, (\d) per rendered frame.*?rendered ([\d.]+);", stats[-1])
            if m:
                addon = f"shown {m.group(1)}, rendered {m.group(2)}, {m.group(3)} per frame, limit {m.group(4)}"
        results[seg["state"]].append((shown, rendered_fps, ratio))
        print(f"{seg['state']:6s} {shown:7.1f} {rendered_fps:9.1f} {ratio:6.2f}  "
              f"{pct(0.01):6.2f} {pct(0.5):6.2f} {pct(0.99):6.2f}   {addon}")

    checks = []
    def check(ok, text):
        checks.append(ok)
        print(f"{'PASS' if ok else 'FAIL'}: {text}")
    print()
    for shown, rendered, ratio in results["test"]:
        check(abs(shown - test_limit) <= 0.01 * test_limit, f"TestLimit {test_limit}: {shown:.1f} shown")
    game_shown = max((s for s, _, _ in results["game"]), default=0.0)
    for shown, rendered, ratio in results["limit"]:
        check(shown <= 1.01 * limit, f"Limit {limit}: {shown:.1f} shown, not above")
        if game_shown > limit:
            check(shown >= 0.98 * limit, f"Limit {limit}: {shown:.1f} shown reaches it (the game's state showed {game_shown:.1f})")
    for shown, rendered, ratio in results["game"]:
        check(rendered <= 1.01 * GAME_OPTION, f"the game's limit: {rendered:.1f} rendered, at most {GAME_OPTION}")
    for state in STATES:
        for shown, rendered, ratio in results[state]:
            whole = round(ratio)
            check(whole >= 1 and abs(ratio - whole) <= 0.02 * whole,
                  f"{state}: {shown:.1f} shown / {rendered:.1f} rendered = {ratio:.3f} per rendered frame")
    print(f"\n{sum(checks)} of {len(checks)} checks passed" + ("" if all(checks) else "; SOME FAILED"))


if __name__ == "__main__":
    main()
