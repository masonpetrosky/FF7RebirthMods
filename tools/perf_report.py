"""Table of a perf_session.py run: perf_report.py OUT_DIR [--settle S]

Per state: frames per second, frame time, GPU busy and CPU busy (PresentMon medians), and how much of the frame the GPU
was busy (near 100% = GPU-bound; well below = the CPU or something else holds the frame). Per switch: the change from
switching it off, against the "all on" segments right before and after (so slow drift cancels), for every round.
The first --settle seconds of each segment are left out (default 2): pipelines swap and DLSS history settles.
Then the GPU's memory, load and clock from nvidia-smi, and tools/cpu_profile.py's table if the run sampled the CPU.
"""
import argparse
import csv
import json
import os
import statistics
import subprocess
import sys


def load_frames(path):
    """The game's frames on its busiest swap chain: (CPUStartQPC, frame ms, GPU busy ms, CPU busy ms)."""
    rows = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.setdefault(r["SwapChainAddress"], []).append(r)
    if not rows:
        return []
    main = max(rows.values(), key=len)

    def num(v):
        try:
            return float(v)
        except (TypeError, ValueError):
            return None
    frames = []
    for r in main:
        t, ft, gpu, cpu = int(r["CPUStartQPC"]), num(r["MsBetweenPresents"]), num(r["MsGPUBusy"]), num(r["MsCPUBusy"])
        if ft is not None and ft > 0:
            frames.append((t, ft, gpu, cpu))
    frames.sort()
    return frames


def stats(frames):
    ft = [f[1] for f in frames]
    gpu = [f[2] for f in frames if f[2] is not None]
    cpu = [f[3] for f in frames if f[3] is not None]
    if len(ft) < 10:
        return None
    ft_sorted = sorted(ft)
    return {
        "frames": len(ft),
        "fps": 1000.0 * len(ft) / sum(ft),
        "frame_ms": statistics.median(ft),
        "p99_ms": ft_sorted[min(len(ft) - 1, int(0.99 * len(ft)))],
        "gpu_ms": statistics.median(gpu) if gpu else float("nan"),
        "cpu_ms": statistics.median(cpu) if cpu else float("nan"),
        "gpu_share": statistics.median([f[2] / f[1] for f in frames if f[2] is not None]) if gpu else float("nan"),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out_dir")
    parser.add_argument("--settle", type=float, default=2.0)
    args = parser.parse_args()

    timeline = json.load(open(os.path.join(args.out_dir, "timeline.json")))
    freq = timeline["qpc_frequency"]
    frames = load_frames(os.path.join(args.out_dir, "presentmon.csv"))
    if not frames:
        sys.exit("no frames in presentmon.csv")

    segs = []
    for s in timeline["segments"]:
        lo, hi = s["start"] + int(args.settle * freq), s["end"]
        segs.append((s["label"], stats([f for f in frames if lo <= f[0] < hi])))

    print(f"{'state':34s} {'fps':>6s} {'frame':>7s} {'p99':>7s} {'GPU busy':>9s} {'CPU busy':>9s} {'GPU share':>9s}")
    for label, st in segs:
        if st is None:
            print(f"{label:34s}   (too few frames)")
            continue
        print(f"{label:34s} {st['fps']:6.1f} {st['frame_ms']:6.2f}ms {st['p99_ms']:6.2f}ms {st['gpu_ms']:7.2f}ms "
              f"{st['cpu_ms']:7.2f}ms {100 * st['gpu_share']:8.0f}%")

    # Each "X off" against the mean of its neighbours (always "all on").
    deltas = {}
    for i, (label, st) in enumerate(segs):
        if not label.endswith(" off") or st is None or i == 0 or i + 1 >= len(segs):
            continue
        before, after = segs[i - 1][1], segs[i + 1][1]
        if before is None or after is None:
            continue
        d = deltas.setdefault(label[:-4], [])
        d.append({k: st[k] - (before[k] + after[k]) / 2 for k in ("fps", "frame_ms", "gpu_ms", "cpu_ms")})
    if deltas:
        print("\nSwitching each off (vs the 'all on' segments around it; one value per round):")
        for name, rounds in deltas.items():
            def fmt(key, unit):
                return ", ".join(f"{r[key]:+.2f}" for r in rounds) + unit
            print(f"  {name:28s} fps {fmt('fps', ''):18s} frame {fmt('frame_ms', ' ms'):22s} "
                  f"GPU busy {fmt('gpu_ms', ' ms'):22s} CPU busy {fmt('cpu_ms', ' ms')}")
        on = [st for label, st in segs if label == "all on" and st]
        if len(on) >= 2:
            fps = [st["fps"] for st in on]
            print(f"  ('all on' itself varied {min(fps):.1f}-{max(fps):.1f} fps over the run: the noise floor)")

    timer_csv = os.path.join(args.out_dir, "gpu_timer.csv")
    if os.path.exists(timer_csv):
        rows = list(csv.DictReader(open(timer_csv)))
        passes = sorted({r["pass"] for r in rows})
        print("\nGPU time per pass (GPU timer add-on; ms per frame, draws per frame), per state:")
        by_state = {}
        for s in timeline["segments"]:
            lo, hi = s["start"] + int(args.settle * freq), s["end"]
            inside = [r for r in rows if int(r["qpc_start"]) >= lo and int(r["qpc_end"]) <= hi]
            state = by_state.setdefault(s["label"], {})
            for r in inside:
                state.setdefault((r["pass"], r["variant"]), []).append((float(r["ms_per_frame"]),
                                                                        float(r["draws_per_frame"])))
        for label, state in by_state.items():
            total = 0.0
            parts = []
            for p in passes:
                for variant in ("modded", "vanilla"):
                    v = state.get((p, variant))
                    if v:
                        ms = statistics.mean(x[0] for x in v)
                        total += ms
                        parts.append(f"{p} {variant[0]} {ms:.3f} ({statistics.mean(x[1] for x in v):.0f})")
            print(f"  {label:28s} total {total:.2f} ms: " + ", ".join(parts))

    gpu_csv = os.path.join(args.out_dir, "gpu.csv")
    if os.path.exists(gpu_csv):
        rows = list(csv.reader(open(gpu_csv)))[1:]
        vals = [[c.strip() for c in r] for r in rows if len(r) >= 7]
        if vals:
            def col(i):
                out = []
                for v in vals:
                    try:
                        out.append(float(v[i]))
                    except ValueError:
                        pass
                return out
            mem, util, clock, power, temp = col(1), col(2), col(3), col(4), col(5)
            reasons = sorted({v[7] for v in vals if len(v) > 7})
            print(f"\nGPU (nvidia-smi, {len(vals)} s): memory {min(mem):.0f}-{max(mem):.0f} MiB, load median "
                  f"{statistics.median(util):.0f}%, clock median {statistics.median(clock):.0f} MHz, power median "
                  f"{statistics.median(power):.0f} W, {max(temp):.0f} C max, clock event reasons {reasons}")

    if os.path.exists(os.path.join(args.out_dir, "cpu_stacks.tsv")):
        print(flush=True)
        subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "cpu_profile.py"),
                        os.path.join(args.out_dir, "cpu")], check=False)


if __name__ == "__main__":
    main()
