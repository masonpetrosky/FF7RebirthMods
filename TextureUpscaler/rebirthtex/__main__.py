"""Command line: python -m rebirthtex <setup|inventory|build> ..."""

from __future__ import annotations

import argparse
import csv
import re
import sys
import time
from collections import deque
from pathlib import Path

from .game import package_path_from_file


def select(inventory_csv: str, pattern: str, min_size: int, max_size: int, suffix: str) -> list[str]:
    regex = re.compile(pattern)
    out = []
    for r in csv.DictReader(open(inventory_csv, encoding="utf-8")):
        if r["error"] or not r["path"].endswith(".uasset"):
            continue
        name = r["path"].rsplit("/", 1)[1]
        size = max(int(r["width"]), int(r["height"]))
        if (regex.search(r["path"]) and min_size <= size <= max_size and name.endswith(suffix + ".uasset")
                and r["format"] in ("PF_DXT1", "PF_BC7")):
            out.append((size, package_path_from_file(r["path"])))
    # Smallest first: those gain the most from upscaling, so an interrupted run still covers the best cases.
    return [path for _size, path in sorted(set(out))]


def select_list(list_csv: str, inventory_csv: str) -> tuple[list[str], dict[str, int]]:
    """A per-texture selection: CSV with columns texture (package path) and factor (2 or 4)."""
    sizes = {package_path_from_file(r["path"]): max(int(r["width"]), int(r["height"]))
             for r in csv.DictReader(open(inventory_csv, encoding="utf-8")) if not r["error"]}
    factors = {r["texture"]: int(r["factor"]) for r in csv.DictReader(open(list_csv, encoding="utf-8"))}
    # Smallest first, as in select().
    return sorted(factors, key=lambda p: (sizes.get(p, 0), p)), factors


def cmd_build(args: argparse.Namespace) -> None:
    from .game import Game
    from .pipeline import Pipeline, write_mod

    if args.list:
        packages, factors = select_list(args.list, args.inventory)
    else:
        packages = select(args.inventory, args.match, args.min_size, args.max_size, args.suffix)
        factors = {}
    if args.exclude:
        lines = (line.split("#", 1)[0].strip() for line in open(args.exclude, encoding="utf-8"))
        excluded = {line for line in lines if line}
        packages = [path for path in packages if path not in excluded]
        print(f"{len(excluded)} textures excluded by {args.exclude}", flush=True)
    print(f"{len(packages)} textures selected", flush=True)
    game = Game(args.paks)
    pipe = Pipeline(game, args.model, Path(args.work), cached_only=args.cached_only, color_fix=not args.no_color_fix,
                    fallback_cache=args.fallback_cache, probe=args.probe)
    done, counts, start = [], {}, time.time()
    # ETA comes from recent upscale times: cached items replay instantly after a resume, and textures
    # are processed smallest first, so an overall average would badly underestimate what is left.
    recent: deque[float] = deque(maxlen=50)
    for i, path in enumerate(packages, 1):
        try:
            result, r = pipe.process(path, factors.get(path, 2))
        except Exception as exc:  # noqa: BLE001 - keep going, report at the end
            print(f"[{i}/{len(packages)}] FAILED {path}: {exc}", flush=True)
            counts["failed"] = counts.get("failed", 0) + 1
            continue
        key = result.status.split(":")[0]
        counts[key] = counts.get(key, 0) + 1
        if r is not None:
            done.append(path)
        if result.status == "upscaled":
            recent.append(result.seconds)
        per_item = sum(recent) / len(recent) if recent else (time.time() - start) / i
        eta = per_item * (len(packages) - i) / 3600
        print(f"[{i}/{len(packages)}] {result.status:9s} {result.seconds:5.1f}s eta {eta:4.1f}h {path}", flush=True)
    print(f"processed in {(time.time() - start) / 60:.1f} min: {counts}; writing container...", flush=True)
    # Results are re-created from the on-disk cache so they never all sit in memory at once.
    utoc = write_mod(Path(args.out), args.mod_name, ((path, pipe.process(path, factors.get(path, 2))[1]) for path in done))
    print(f"wrote {len(done)} textures to {utoc}")


def main(argv: list[str]) -> None:
    parser = argparse.ArgumentParser(prog="rebirthtex")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("setup", help="download texconv, repak and the upscaling model")
    inv = sub.add_parser("inventory", help="list every texture in the game")
    inv.add_argument("paks")
    inv.add_argument("csv")
    build = sub.add_parser("build", help="upscale textures and write a ~mods container")
    build.add_argument("--paks", required=True)
    build.add_argument("--inventory", required=True)
    build.add_argument("--model", required=True)
    build.add_argument("--match", default=".", help="regex on the package file path")
    build.add_argument("--suffix", default="_C", help="texture name suffix, e.g. _C for color maps")
    build.add_argument("--min-size", type=int, default=256)
    build.add_argument("--max-size", type=int, default=1024)
    build.add_argument("--work", default="work")
    build.add_argument("--out", default="out")
    build.add_argument("--mod-name", default="FF7RebirthTextureUpscaler_P")
    build.add_argument("--cached-only", action="store_true", help="only pack textures that are already upscaled")
    build.add_argument("--exclude", help="file of package paths (one per line, # comments) to keep at original size")
    build.add_argument("--no-color-fix", action="store_true",
                       help="keep the model's colours (default: match each texture's local colours to the original)")
    build.add_argument("--fallback-cache", help="cache folder whose results to reuse when these settings have none")
    build.add_argument("--list", help="CSV (texture, factor) of package paths to upscale by 2x or 4x; replaces "
                       "--match/--suffix/--min-size/--max-size (see tools/texel_density.py)")
    build.add_argument("--probe", action="store_true",
                       help="replace every added top mip with a black/white checkerboard, to check in game that it is displayed")
    args = parser.parse_args(argv)
    if args.command == "setup":
        from .setup_deps import main as setup_main
        setup_main()
    elif args.command == "inventory":
        from .inventory import main as inventory_main
        inventory_main(args.paks, args.csv)
    else:
        cmd_build(args)


if __name__ == "__main__":
    main(sys.argv[1:])
