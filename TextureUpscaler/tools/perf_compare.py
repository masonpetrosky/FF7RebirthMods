"""Summarizes and compares perf_capture.py runs.

Usage: perf_compare.py <label> [<label> ...]     e.g. perf_compare.py baseline upscaled

Every capture in work/perf whose name starts with "<label>_" is included. Stats are computed per
run and then averaged per label, so run-to-run noise is visible (do several runs per label).
"""

from __future__ import annotations

import csv
import statistics
import sys
from pathlib import Path

PERF = Path(__file__).resolve().parents[1] / "work" / "perf"
HITCH_FACTOR = 2.5   # a hitch is a frame slower than 2.5x the run's median...
HITCH_MIN_MS = 25.0  # ...and slower than 25 ms


def column(rows: list[dict], name: str) -> list[float]:
    out = []
    for r in rows:
        v = r.get(name)
        if v not in (None, "", "NA"):
            try:
                out.append(float(v))
            except ValueError:
                pass
    return out


def pct(values: list[float], p: float) -> float:
    s = sorted(values)
    return s[min(len(s) - 1, int(round(p / 100 * (len(s) - 1))))]


def low_fps(frame_ms: list[float], share: float) -> float:
    worst = sorted(frame_ms, reverse=True)[:max(1, int(len(frame_ms) * share))]
    return 1000.0 / statistics.mean(worst)


def run_stats(path: Path) -> dict:
    rows = list(csv.DictReader(open(path, newline="", encoding="utf-8-sig", errors="replace")))
    if rows and "FrameType" in rows[0]:  # with frame generation, judge the frames the game rendered
        app = [r for r in rows if r["FrameType"] in ("Application", "NotSet", "")]
        rows = app or rows
    ft = [v for v in column(rows, "MsBetweenPresents") if v > 0]
    if len(ft) < 100:
        raise ValueError(f"{path.name}: only {len(ft)} frames")
    median = statistics.median(ft)
    hitches = [v for v in ft if v > max(HITCH_FACTOR * median, HITCH_MIN_MS)]
    minutes = sum(ft) / 60000
    gpu = column(rows, "MsGPUBusy") or column(rows, "MsGPUTime")
    stats = {
        "frames": len(ft),
        "avg_fps": 1000 * len(ft) / sum(ft),
        "low1_fps": low_fps(ft, 0.01),
        "low01_fps": low_fps(ft, 0.001),
        "median_ms": median,
        "p99_ms": pct(ft, 99),
        "p999_ms": pct(ft, 99.9),
        "max_ms": max(ft),
        "hitches_per_min": len(hitches) / minutes,
        "gpu_busy_ms": statistics.median(gpu) if gpu else float("nan"),
    }
    gpu_csv = path.with_name(path.stem + "_gpu.csv")
    if gpu_csv.exists():
        mem = column(list(csv.DictReader(open(gpu_csv, newline=""))), "memory_used_mib")
        stats["vram_avg_mib"] = statistics.mean(mem) if mem else float("nan")
        stats["vram_max_mib"] = max(mem) if mem else float("nan")
    return stats


FIELDS = [("avg_fps", "avg FPS", "{:.1f}", +1), ("low1_fps", "1% low", "{:.1f}", +1), ("low01_fps", "0.1% low", "{:.1f}", +1),
          ("median_ms", "median ms", "{:.2f}", -1), ("p99_ms", "p99 ms", "{:.2f}", -1), ("p999_ms", "p99.9 ms", "{:.2f}", -1),
          ("max_ms", "max ms", "{:.1f}", -1), ("hitches_per_min", "hitches/min", "{:.2f}", -1),
          ("gpu_busy_ms", "GPU busy ms", "{:.2f}", -1), ("vram_avg_mib", "VRAM avg MiB", "{:.0f}", -1),
          ("vram_max_mib", "VRAM max MiB", "{:.0f}", -1)]


def main(labels: list[str]) -> None:
    summary = {}
    for label in labels:
        runs = sorted(p for p in PERF.glob(f"{label}_*.csv") if not p.stem.endswith("_gpu"))
        if not runs:
            raise SystemExit(f"no captures for '{label}' in {PERF}")
        stats = [run_stats(p) for p in runs]
        print(f"\n{label}: {len(runs)} run(s)")
        for p, s in zip(runs, stats):
            print(f"  {p.stem}: {s['avg_fps']:.1f} fps, 1% low {s['low1_fps']:.1f}, p99 {s['p99_ms']:.2f} ms, "
                  f"hitches/min {s['hitches_per_min']:.2f}, VRAM max {s.get('vram_max_mib', float('nan')):.0f} MiB")
        summary[label] = {k: statistics.mean(s[k] for s in stats if k in s) for k, *_ in FIELDS if any(k in s for s in stats)}
        summary[label]["_spread"] = {k: (statistics.stdev([s[k] for s in stats]) if len(stats) > 1 else 0.0) for k, *_ in FIELDS
                                     if all(k in s for s in stats)}

    base = labels[0]
    print("\n" + "metric".ljust(14) + "".join(l.rjust(18) for l in labels) + ("   change vs " + base if len(labels) > 1 else ""))
    for key, title, fmt, better in FIELDS:
        if key not in summary[base]:
            continue
        line = title.ljust(14)
        for label in labels:
            value = summary[label].get(key, float("nan"))
            spread = summary[label]["_spread"].get(key, 0.0)
            line += (fmt.format(value) + (" ±" + fmt.format(spread) if spread else "")).rjust(18)
        if len(labels) > 1 and summary[base].get(key):
            delta = (summary[labels[-1]].get(key, float("nan")) - summary[base][key]) / summary[base][key] * 100
            line += f"   {delta:+.1f}%"
        print(line)


if __name__ == "__main__":
    main(sys.argv[1:])
