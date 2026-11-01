"""Where the game's CPU time goes, from build/cpu_sampler.exe's output: cpu_profile.py PREFIX [--threads N] [--top N]

Every sample is charged to an owner: walking its stack from the leaf, Windows, the D3D12 runtime and the NVIDIA driver
are passed over, and the first other module owns it (the game, ReShade, Shader Injector, RenoDX, one of our add-ons,
DLSS, an overlay...). So a driver call made by an add-on counts for the add-on. Calls ReShade passes on for the game
(every D3D12 command goes through its wrappers) land on ReShade too; "ReShade own" is the part whose leaf is in
ReShade's code. A sample whose leaf is a wait (NtWaitForSingleObject...) counts as waiting. Samples are weighted by how
much of the interval the thread actually ran.
Prints, per busy thread, the share of its running time per owner, then the same summed over the sampled threads.
"""
import argparse
import collections
import os

PASS_THROUGH = {"Windows", "D3D12 runtime", "NVIDIA driver"}
WAITS = ("NtWaitFor", "ZwWaitFor", "NtDelayExecution", "ZwDelayExecution", "NtRemoveIoCompletion",
         "ZwRemoveIoCompletion", "NtWaitForAlertByThreadId", "ZwWaitForAlertByThreadId", "NtSignalAndWait",
         "ZwSignalAndWait", "NtYieldExecution", "ZwYieldExecution", "NtUserMsgWaitForMultipleObjects",
         "NtWaitForWorkViaWorkerFactory", "ZwWaitForWorkViaWorkerFactory", "NtGdiDdDDIWaitForSynchronizationObject",
         "NtWaitForKeyedEvent", "ZwWaitForKeyedEvent", "SwitchToThread")
ADDONS = {"ff7rebirthvegetationandhairfix.addon64": "Veg+Hair Fix", "ff7rebirthnpcmotion.addon64": "NPC Motion",
          "ff7rebirthanisotropic.addon64": "Anisotropic", "ff7rebirthshaderinjectorsync.addon64": "Injector Sync"}


def category(path, system):
    name = os.path.basename(path).lower()
    folder = os.path.dirname(path).lower()
    if name == "ff7rebirth_.exe":
        return "game"
    if name in ADDONS:
        return ADDONS[name]
    if name.startswith("renodx"):
        return "RenoDX"
    if name.startswith("nvngx"):
        return "DLSS"
    if name in ("d3d12core.dll", "d3d12.dll", "d3d12sdklayers.dll", "dxgi.dll") and system:
        return "D3D12 runtime"
    if name == "d3d12core.dll":
        return "D3D12 runtime"  # the game's Agility SDK copy
    if folder.endswith("\\binaries\\win64"):
        if name == "dxgi.dll":
            return "ReShade"
        if name == "dsound.dll":
            return "Shader Injector"
        if name == "dxcompiler.dll":
            return "dxcompiler"
        return "game: " + name
    if "\\driverstore\\" in folder or name.startswith("nv"):
        return "NVIDIA driver"
    if "gameoverlayrenderer" in name:
        return "Steam overlay"
    if "eosovl" in name or "eossdk" in name:
        return "Epic online services"
    if system:
        return "Windows"
    if folder.endswith("\\ff7rebirthvegetationandhairfix") or folder.endswith("\\ff7rebirthnpcmotion"):
        return "dxcompiler"
    return "other: " + name


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("prefix")
    parser.add_argument("--threads", type=int, default=10, help="busiest threads to list (default 10)")
    parser.add_argument("--top", type=int, default=0, help="also list each owner's N hottest code offsets")
    args = parser.parse_args()

    modules = {}
    for line in open(args.prefix + "_modules.tsv", encoding="utf-8").read().splitlines()[1:]:
        index, base, size, system, path = line.split("\t", 4)
        modules[index] = (category(path, system == "1"), os.path.basename(path))
    symbols = {}
    if os.path.exists(args.prefix + "_symbols.tsv"):
        for line in open(args.prefix + "_symbols.tsv", encoding="utf-8").read().splitlines():
            key, name = line.split("\t", 1)
            symbols[key] = name
    threads = {}
    for line in open(args.prefix + "_threads.tsv", encoding="utf-8").read().splitlines()[1:]:
        index, tid, name, sampled, busy, cpu_s, samples, copied = line.split("\t")
        threads[tid] = {"name": name or "(unnamed)", "sampled": sampled == "1", "busy": float(busy),
                        "owners": collections.Counter(), "weight": 0.0, "hot": collections.Counter()}

    header = ""
    for line in open(args.prefix + "_stacks.tsv", encoding="utf-8"):
        if line.startswith("#"):
            header = line[1:].strip()
            continue
        tid, weight, stack = line.rstrip("\n").split("\t")
        w = float(weight)
        frames = stack.split(";") if stack else []
        t = threads[tid]
        t["weight"] += w
        if not frames:
            t["owners"]["(no stack)"] += w
            continue
        leaf_symbol = symbols.get(frames[0], "") or (symbols.get(frames[1], "") if len(frames) > 1 else "")
        if leaf_symbol.startswith(WAITS):
            t["owners"]["waiting"] += w
            continue
        owner, owner_frame = None, None
        for k, frame in enumerate(frames):
            cat = modules.get(frame.split(":")[0], ("unknown code", "?"))[0]
            if cat not in PASS_THROUGH:
                owner, owner_frame = cat, frame
                if cat == "ReShade":
                    owner = "ReShade own" if k == 0 else "ReShade -> runtime/driver"
                elif cat == "game" and k > 0:
                    owner = "game -> runtime/driver"
                break
        if owner is None:
            owner = "Windows/driver only"
        t["owners"][owner] += w
        if owner_frame:
            t["hot"][(owner, owner_frame)] += w

    print(f"CPU sampling: {header}")
    busy = sorted((t for t in threads.values() if t["sampled"] and t["weight"] > 0), key=lambda t: -t["busy"])
    total = collections.Counter()
    for t in busy:
        total.update(t["owners"])
    for t in busy[:args.threads]:
        parts = sorted(t["owners"].items(), key=lambda kv: -kv[1])
        text = ", ".join(f"{k} {100 * v / t['weight']:.0f}%" for k, v in parts if v / t["weight"] >= 0.01)
        print(f"  {t['name'][:28]:28s} busy {100 * t['busy']:3.0f}%: {text}")
    all_weight = sum(total.values())
    print("  all sampled threads: " + ", ".join(f"{k} {100 * v / all_weight:.1f}%"
                                               for k, v in sorted(total.items(), key=lambda kv: -kv[1])
                                               if v / all_weight >= 0.002))
    if args.top:
        hot = collections.Counter()
        for t in busy:
            hot.update(t["hot"])
        by_owner = collections.defaultdict(list)
        for (owner, frame), w in hot.items():
            by_owner[owner].append((w, frame))
        for owner, items in sorted(by_owner.items()):
            items.sort(reverse=True)
            print(f"  hottest in {owner}: " + ", ".join(
                f"{modules.get(f.split(':')[0], ('', '?'))[1]}+{f.split(':')[1]} {100 * w / all_weight:.2f}%"
                for w, f in items[:args.top]))


if __name__ == "__main__":
    main()
