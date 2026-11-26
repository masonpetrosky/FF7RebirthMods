"""Frame-time test of the installed mods at one spot: perf_session.py OUT_DIR [--segment S] [--rounds N] [--profile S]

Start it with the game in front, the party standing still at the spot to test and hands off the controls. It records
PresentMon (frame time, GPU busy, CPU busy) for the whole run and nvidia-smi once a second, and switches the mods that
have a hotkey off and on in turn, each for S seconds (A/B/A, N rounds):
  F7  NPC Motion's motion-vector fix   F8  Vegetation and Hair Fix   F6  its soft edges
  Delete  the ray tracing's sun shadows (FF7RebirthRayTracing; switched off, the game's own shadows)
  Insert  the ray tracing's AO (switched off, the game's own AO)
  F2  the ray tracing's ambient light (stage 5; switched ON, as its ini starts with the game's): only with --only 8
  F17 the ray tracing's reflections (stage 6; ReflectionsKey in its test sessions' ini, a key no keyboard here has;
      switched off: the game's own reflections, with our ambient light on): only with --only 9
(Switch 1, Shader Injector, stays in the list so the numbers keep their meaning, but the game no longer has it
(removed 2026-11-08), and its key, Delete, is the ray tracing's now: switch 6.)
Then it samples the game's busiest CPU threads for --profile seconds with build/cpu_sampler.exe, and with
--profile-off again with the first tested switch off (where the CPU time goes with and without it). At the start it
takes a ReShade screenshot (M6, Page Down) of the measured view.

Keys only ever go to the game: each one waits until the game is in front. A mod that was switched off is always
switched back on, waiting up to 10 minutes for the game to come back to the front (and saying so if it cannot).
The keys must match the installed ini files (see CLAUDE.md "Keys already taken").

Writes OUT_DIR/presentmon.csv, gpu.csv, timeline.json and cpu_*.tsv (and gpu_timer.csv: this run's lines of the GPU
timer add-on's log, if it is installed; see tools/gpu_timer.cpp), then prints tools/perf_report.py's table.
Needs PresentMon 2.6.0 in Documents\\Tools\\PresentMon (runs without elevation for the user's own game).
"""
import argparse
import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRESENTMON = os.path.join(ROOT, "..", "..", "Tools", "PresentMon", "2.6.0", "PresentMon-2.6.0-x64.exe")
SAMPLER = os.path.join(ROOT, "build", "cpu_sampler.exe")
GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH\End\Binaries\Win64"
GAME = os.environ.get("PERF_SESSION_EXE", "ff7rebirth_.exe")  # another exe only for testing this script
SESSION = "FF7RebirthModsPerf"

# label, virtual key, extended key (for the scan code)
SWITCHES = [
    ("Shader Injector", None, False),          # was Delete; removed from the game 2026-11-08 (Delete is switch 6 now)
    ("NPC Motion fix", 0x76, False),           # F7
    ("Vegetation and Hair Fix", 0x77, False),  # F8
    ("soft edges", 0x75, False),               # F6
    # Switches DLSS Ray Reconstruction ON (NPC Motion's RayReconstructionKey; the user's ini sets F13 for tests only):
    # its "off" segments are Ray Reconstruction's. Not in the default run: --only 5.
    ("NPC Motion's Ray Reconstruction (switched on)", 0x7C, False),  # F13
    ("ray tracing's sun shadows", 0x2E, True),  # Delete: FF7RebirthRayTracing's SunShadowsKey (since 2026-11-15)
    ("ray tracing's AO", 0x2D, True),  # Insert: FF7RebirthRayTracing's RtAoKey (since 2026-11-20)
    # Switches the ray tracing's ambient light ON (its GiKey, stage 5, with GiOn=0 in the ini: the game's at the
    # start): its "off" segments are ours. Not in the default run: --only 8.
    ("ray tracing's ambient light (switched on)", 0x71, False),  # F2 (since 2026-11-21)
    # Switches the ray tracing's reflections off (its ReflectionsKey, stage 6, test sessions only: ours at the start
    # while its ambient light is on): its "off" segments are the game's. Not in the default run: --only 9.
    ("ray tracing's reflections", 0x80, False),  # F17 (since 2026-11-25)
]
DEFAULT_SWITCHES = (2, 3, 4, 6, 7)  # switch numbers of the default run: the installed mods' own switches

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32.GetForegroundWindow.restype = wintypes.HWND
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.MapVirtualKeyW.restype = wintypes.UINT
user32.GetAsyncKeyState.restype = ctypes.c_short
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                ctypes.POINTER(wintypes.DWORD)]


def qpc():
    value = ctypes.c_int64()
    kernel32.QueryPerformanceCounter(ctypes.byref(value))
    return value.value


def qpc_frequency():
    value = ctypes.c_int64()
    kernel32.QueryPerformanceFrequency(ctypes.byref(value))
    return value.value


def game_in_front():
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), ctypes.byref(pid))
    handle = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    try:
        buffer = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(1024)
        if not kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return False
        return os.path.basename(buffer.value).lower() == GAME
    finally:
        kernel32.CloseHandle(handle)


def game_running():
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {GAME}", "/NH"], capture_output=True, text=True).stdout
    return GAME in out.lower()


def modifiers_held():
    return any(user32.GetAsyncKeyState(vk) & 0x8000 for vk in (0x10, 0x11, 0x12, 0x5B, 0x5C))  # Shift Ctrl Alt Win


def tap(vk, extended, wait_seconds):
    """Presses a key in the game, once it is in front and no modifier is held; False if that never happened."""
    deadline = time.time() + wait_seconds
    while not game_in_front() or modifiers_held():
        if time.time() > deadline:
            return False
        time.sleep(0.25)
    scan = user32.MapVirtualKeyW(vk, 0)
    flags = 1 if extended else 0  # KEYEVENTF_EXTENDEDKEY
    user32.keybd_event(vk, scan, flags, 0)
    time.sleep(0.08)  # the add-ons read the key once per frame
    user32.keybd_event(vk, scan, flags | 2, 0)  # KEYEVENTF_KEYUP
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out_dir")
    parser.add_argument("--segment", type=float, default=10.0, help="seconds per state (default 10)")
    parser.add_argument("--rounds", type=int, default=2, help="A/B/A rounds per switch (default 2)")
    parser.add_argument("--profile", type=float, default=15.0, help="seconds of CPU sampling at the end, 0 = none")
    parser.add_argument("--profile-off", action="store_true",
                        help="after the CPU sample with all on, another with the first tested switch off")
    parser.add_argument("--only", default="", help="comma-separated switch numbers to test (1-9; 1 = Shader Injector, "
                                                   "no longer installed; 5 = NPC Motion's Ray Reconstruction; 6 = the "
                                                   "ray tracing's sun shadows; 7 = its AO; 8 = its ambient light, "
                                                   "switched on; 9 = its reflections, F17), default 2-4, 6 and 7")
    parser.add_argument("--no-keys", action="store_true", help="press nothing: the same segments, all on")
    args = parser.parse_args()

    if not game_running():
        sys.exit(f"{GAME} is not running")
    if not os.path.exists(PRESENTMON):
        sys.exit(f"PresentMon not found: {PRESENTMON}")
    if args.profile > 0 and not os.path.exists(SAMPLER):
        sys.exit(f"{SAMPLER} missing: run .\\build.ps1 cpu_sampler")
    numbers = [int(i) for i in args.only.split(",")] if args.only else list(DEFAULT_SWITCHES)
    for i in numbers:
        if not 1 <= i <= len(SWITCHES) or SWITCHES[i - 1][1] is None:
            sys.exit(f"no switch {i} to test (1, Shader Injector, is no longer installed)")
    switches = [SWITCHES[i - 1] for i in numbers]
    os.makedirs(args.out_dir, exist_ok=True)
    total = args.segment * (1 + 2 * args.rounds * len(switches)) + args.profile + 15
    if not args.no_keys:
        print("Waiting for the game to be in front...", flush=True)
        while not game_in_front():
            time.sleep(0.25)
        print(f"Starting in 5 s; about {total / 60:.1f} minutes with hands off the controls.", flush=True)
        time.sleep(5.0)

    timeline = {"qpc_frequency": qpc_frequency(), "segment": args.segment, "segments": [], "events": []}
    gpu_log = open(os.path.join(args.out_dir, "gpu.csv"), "w")
    gpu_log.write("timestamp,memory_used_mib,gpu_util_pct,clock_mhz,power_w,temp_c,pstate,clock_event_reasons\n")
    gpu_log.flush()
    smi = subprocess.Popen(["nvidia-smi", "--query-gpu=timestamp,memory.used,utilization.gpu,clocks.gr,power.draw,"
                            "temperature.gpu,pstate,clocks_event_reasons.active", "--format=csv,noheader,nounits",
                            "-lms", "1000"], stdout=gpu_log, stderr=subprocess.DEVNULL)
    pm = subprocess.Popen([PRESENTMON, "--process_name", GAME, "--output_file",
                           os.path.join(args.out_dir, "presentmon.csv"), "--qpc_time", "--no_console_stats",
                           "--session_name", SESSION, "--stop_existing_session"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    off = None  # the switch that is off right now
    try:
        # A ReShade screenshot (M6, Page Down) of the measured view; the 16-bit PNG takes about 2 s to save, so it
        # happens while PresentMon's trace session starts.
        if not args.no_keys and tap(0x22, True, 0):
            timeline["screenshot"] = qpc()
        time.sleep(4.0)

        def segment(label):
            start = qpc()
            print(f"  {label:32s}", end="", flush=True)
            time.sleep(args.segment)
            timeline["segments"].append({"label": label, "start": start, "end": qpc()})
            print(" done", flush=True)

        def press(index, state):
            name, vk, extended = switches[index]
            if args.no_keys:
                return
            if not tap(vk, extended, 0 if state == "off" else 600):
                raise RuntimeError(f"the game left the front; {name} not switched {state}")
            timeline["events"].append({"qpc": qpc(), "switch": name, "state": state})

        segment("all on")
        for _ in range(args.rounds):
            for i, (name, _, _) in enumerate(switches):
                press(i, "off")
                off = i
                segment("all on (no key)" if args.no_keys else f"{name} off")
                press(i, "on")
                off = None
                segment("all on")
    finally:
        if off is not None:
            name, vk, extended = switches[off]
            if tap(vk, extended, 600):
                timeline["events"].append({"qpc": qpc(), "switch": name, "state": "on"})
                print(f"{name} switched back on")
            else:
                print(f"WARNING: {name} is still OFF; press its key in the game")
        subprocess.run([PRESENTMON, "--terminate_existing_session", "--session_name", SESSION],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            pm.wait(timeout=15)
        except subprocess.TimeoutExpired:
            pm.kill()
        smi.terminate()
        gpu_log.close()
        with open(os.path.join(args.out_dir, "timeline.json"), "w") as f:
            json.dump(timeline, f, indent=1)

    def sample(prefix):
        start = qpc()
        subprocess.run([SAMPLER, GAME, str(args.profile), os.path.join(args.out_dir, prefix), "--hz", "250",
                        "--threads", "16"], check=False)
        return {"start": start, "seconds": args.profile}

    if args.profile > 0:
        print(f"Sampling the game's CPU threads for {args.profile:.0f} s...", flush=True)
        timeline["profile"] = sample("cpu")
        if args.profile_off and not args.no_keys:
            name, vk, extended = switches[0]
            if not tap(vk, extended, 0):
                print(f"The game is not in front: no CPU sample with {name} off.")
            else:
                try:
                    print(f"Sampling again with {name} off...", flush=True)
                    time.sleep(2.0)  # past the switch's first frames
                    timeline["profile_off"] = dict(sample("cpu_off"), switch=name)
                finally:
                    if tap(vk, extended, 600):
                        print(f"{name} switched back on")
                    else:
                        print(f"WARNING: {name} is still OFF; press its key in the game")
        with open(os.path.join(args.out_dir, "timeline.json"), "w") as f:
            json.dump(timeline, f, indent=1)
    # The GPU timer add-on's lines from this run, if it is installed (tools/gpu_timer.cpp).
    timer_csv = os.path.join(GAME_DIR, "FF7RebirthGpuTimer.csv")
    if os.path.exists(timer_csv) and timeline["segments"]:
        first, last = timeline["segments"][0]["start"], timeline["segments"][-1]["end"]
        with open(timer_csv) as src, open(os.path.join(args.out_dir, "gpu_timer.csv"), "w") as dst:
            for n, line in enumerate(src):
                if n == 0 or first <= int(line.split(",")[0]) <= last:
                    dst.write(line)
    print("Done; the game can be played again.", flush=True)
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "perf_report.py"), args.out_dir], check=False)


if __name__ == "__main__":
    main()
