"""Captures frame times (PresentMon) and GPU memory (nvidia-smi) while FF7 Rebirth runs.

Usage: perf_capture.py <label> [seconds]      e.g. perf_capture.py baseline 90

Writes work/perf/<label>_<timestamp>.csv (PresentMon), ..._gpu.csv (VRAM/utilization once per
second) and ..._meta.json (which mods were installed, game settings). PresentMon needs
administrator rights, so Windows shows a UAC prompt for it; the rest runs unelevated.
"""

from __future__ import annotations

import csv
import json
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRESENTMON = ROOT / "bin" / "PresentMon.exe"
GAME_PROCESS = "ff7rebirth_.exe"
GAME_DIR = Path(r"C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH")
SETTINGS = Path.home() / "OneDrive" / "Documents" / "My Games" / "FINAL FANTASY VII REBIRTH" / "Saved" / "Config" / \
    "WindowsNoEditor" / "GameUserSettings.ini"


def game_running() -> bool:
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {GAME_PROCESS}", "/NH"], capture_output=True, text=True).stdout
    return GAME_PROCESS.lower() in out.lower()


def sample_gpu(path: Path, stop: threading.Event) -> None:
    query = "timestamp,memory.used,memory.total,utilization.gpu,clocks.gr,temperature.gpu,power.draw"
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["time", "memory_used_mib", "memory_total_mib", "gpu_util_pct", "clock_mhz", "temp_c", "power_w"])
        while not stop.is_set():
            out = subprocess.run(["nvidia-smi", f"--query-gpu={query}", "--format=csv,noheader,nounits"],
                                 capture_output=True, text=True).stdout.strip().splitlines()
            if out:
                writer.writerow([v.strip() for v in out[0].split(",")])
                f.flush()
            stop.wait(1.0)


def metadata(label: str, seconds: int) -> dict:
    mods_dir = GAME_DIR / "End" / "Content" / "Paks" / "~mods"
    mods = sorted((p.name, p.stat().st_size) for p in mods_dir.glob("*")) if mods_dir.exists() else []
    win64 = GAME_DIR / "End" / "Binaries" / "Win64"
    settings = SETTINGS.read_text(encoding="utf-8", errors="replace") if SETTINGS.exists() else ""
    return {
        "label": label,
        "started": datetime.now().isoformat(timespec="seconds"),
        "seconds": seconds,
        "mods": mods,
        "reshade_installed": (win64 / "dxgi.dll").exists(),
        "hairfix_installed": (win64 / "FF7RebirthHairFix.addon64").exists(),
        "texture_quality": next((l.split("=")[1] for l in settings.splitlines() if l.startswith("sg.TextureQuality=")), None),
    }


def main(label: str, seconds: str = "90") -> None:
    seconds_i = int(seconds)
    if not PRESENTMON.exists():
        raise SystemExit("bin/PresentMon.exe missing; run: python -m rebirthtex setup")
    if not game_running():
        raise SystemExit(f"{GAME_PROCESS} is not running")
    out_dir = ROOT / "work" / "perf"
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = f"{label}_{datetime.now():%Y%m%d_%H%M%S}"
    frames = out_dir / f"{stem}.csv"
    (out_dir / f"{stem}_meta.json").write_text(json.dumps(metadata(label, seconds_i), indent=2))

    stop = threading.Event()
    sampler = threading.Thread(target=sample_gpu, args=(out_dir / f"{stem}_gpu.csv", stop), daemon=True)
    sampler.start()
    args = ["--process_name", GAME_PROCESS, "--output_file", str(frames), "--timed", str(seconds_i),
            "--terminate_after_timed", "--stop_existing_session", "--no_console_stats"]
    arg_list = ",".join("'" + a.replace("'", "''") + "'" for a in args)
    print(f"capturing {seconds_i}s as '{label}' (approve the UAC prompt for PresentMon)...", flush=True)
    start = time.time()
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    f"Start-Process -FilePath '{PRESENTMON}' -ArgumentList {arg_list} -Verb RunAs -Wait -WindowStyle Hidden"],
                   check=True)
    stop.set()
    sampler.join()
    if not frames.exists():
        raise SystemExit("PresentMon produced no output (UAC declined, or the game was not presenting?)")
    print(f"done in {time.time() - start:.0f}s -> {frames}")


if __name__ == "__main__":
    main(*sys.argv[1:])
