"""Estimates how large each environment texture's texels are in the world, and so how many screen pixels one texel
covers up close, from the cooked game data (no game running needed).

Data used (all from the IoStore containers):
- static meshes: each material slot's UV channel density (FStaticMaterial.UVChannelData.LocalUVDensities, world cm
  per UV unit for UV channels 0-3) and the slot's material;
- materials (material instances and materials): the environment textures they reference, plus their parents';
- textures: width/height from the inventory (work/inventory.csv).
A texture's texel size on a mesh = LocalUVDensities[channel] / texture width, assuming the material samples it at
UV scale 1 (the cooked TextureStreamingData only lists placeholder textures in this game). Tiling textures whose name
carries a world size (T_Tile_..._8m_...) use that size instead. Mesh placement scale in the levels is NOT included.

Checked 2026-10-24 against a RenderDoc capture at a limestone cliff (tools/rd_texel.py, frame 10633): the measured
UV0 density there (the mesh shader passes UV0 in COLOR0.xy) was 12,500-17,200 cm per UV, against 12,225 for
Cliff_Limestone_10B in the mesh data (placement scale about 1.0-1.4); texel size 3.1-4.2 cm vs 3.2 cm predicted, and
at 25 m on a surface facing the camera 4.4 screen px per texel measured vs 4.3 predicted. The game's horizontal FOV
there was 60 degrees (View_ViewToClip[0][0] = 1.732).

usage:
  texel_density.py scan  [--workers N]      -> work/texel/scan.json (meshes + materials), resumable
  texel_density.py report [--distance-m 3] [--hfov 60] [--width 3840]   -> work/texel/textures.csv + summary
  texel_density.py show <texture or mesh substring>
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import multiprocessing as mp
import re
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from rebirthtex.cityhash import cityhash64, script_object_index, ue_hash  # noqa: E402
from rebirthtex.game import Game, package_path_from_file  # noqa: E402
from rebirthtex.iostore import EXPORT_BUNDLE_DATA, chunk_id  # noqa: E402
from rebirthtex.zen import ZenPackage  # noqa: E402

PAKS = r"C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH\End\Content\Paks"
OUT = ROOT / "work" / "texel"
STATIC_MESH = script_object_index("/Script/Engine.StaticMesh")
MATERIAL_CLASSES = {script_object_index("/Script/Engine.MaterialInstanceConstant"): "MIC",
                    script_object_index("/Script/Engine.Material"): "Material"}
TYPE_SHIFT = 62


def import_hash(object_path: str) -> int:
    s = "".join("/" if c in ".:" else c.lower() for c in object_path)
    return cityhash64(s.encode("utf-16-le")) & ~(3 << TYPE_SHIFT)


def resolve_imports(z: ZenPackage) -> list[str | None]:
    """Package-import paths for the import map (None for script imports and nulls)."""
    cands = {}
    for n in z.names:
        if n.startswith("/Game/"):
            base = n.rsplit("/", 1)[-1]
            cands[import_hash(n + "." + base)] = n
    out = []
    for v in z.imports:
        out.append(cands.get(v & ~(3 << TYPE_SHIFT)) if (v >> TYPE_SHIFT) == 2 else None)
    return out


# ------------------------------------------------------------------ unversioned header for one struct
def read_header(d: bytes, p: int, max_index: int):
    """Unversioned property header of a struct whose schema has max_index + 1 properties.
    Returns (list of present schema indices, set of zero indices, new pos) or None if it doesn't parse."""
    present, zero_flags, index = [], [], 0
    while True:
        if p + 2 > len(d):
            return None
        v = struct.unpack_from("<H", d, p)[0]
        p += 2
        skip, has_zero, last, num = v & 0x7F, (v >> 7) & 1, (v >> 8) & 1, v >> 9
        index += skip
        if num == 0 and not last or index + num - 1 > max_index:
            return None
        for _ in range(num):
            present.append(index)
            zero_flags.append(has_zero)
            index += 1
        if last:
            break
    zeros = set()
    nbits = sum(zero_flags)
    if nbits:
        nbytes = 1 if nbits <= 8 else 2 if nbits <= 16 else 4 * ((nbits + 31) // 32)
        mask = int.from_bytes(d[p:p + nbytes], "little")
        p += nbytes
        bit = 0
        for idx, flag in zip(present, zero_flags):
            if flag:
                if mask >> bit & 1:
                    zeros.add(idx)
                bit += 1
    return present, zeros, p


def parse_static_material(d: bytes, p: int, z: ZenPackage, imports: list):
    """FStaticMaterial at p: 0 MaterialInterface, 1 MaterialSlotName, 2 ImportedMaterialSlotName, 3 UVChannelData
    {0 bInitialized, 1 bOverrideDensities, 2..5 LocalUVDensities[4]}. Returns (dict, end) or None."""
    h = read_header(d, p, 3)
    if not h:
        return None
    present, zeros, p = h
    if 3 not in present or 0 not in present or 0 in zeros:
        return None
    out = {"material": None, "slot": None, "densities": [0.0] * 4, "initialized": False}
    for idx in present:
        if idx in zeros:
            continue
        if idx == 0:
            ref = struct.unpack_from("<i", d, p)[0]
            p += 4
            if ref >= 0 or -ref - 1 >= len(imports) or imports[-ref - 1] is None:
                return None
            out["material"] = imports[-ref - 1]
        elif idx in (1, 2):
            ni, nn = struct.unpack_from("<II", d, p)
            p += 8
            if ni >= len(z.names) or nn > 1000:
                return None
            if idx == 1:
                out["slot"] = z.names[ni]
        elif idx == 3:
            h2 = read_header(d, p, 5)
            if not h2:
                return None
            pres2, zeros2, p = h2
            if not pres2:
                return None
            for j in pres2:
                if j in zeros2:
                    continue
                if j in (0, 1):
                    b = d[p]
                    p += 1
                    if b not in (0, 1):
                        return None
                    if j == 0:
                        out["initialized"] = bool(b)
                else:
                    f = struct.unpack_from("<f", d, p)[0]
                    p += 4
                    if not (0 <= f < 1e7):
                        return None
                    out["densities"][j - 2] = f
    return out, p


PROPERTY_SCAN_BYTES = 64 * 1024  # StaticMaterials sits in the property block at the start, before any inline render data


def scan_mesh(z: ZenPackage, data: bytes, imports: list):
    slots, p, seen = [], 0, set()
    end = min(len(data), PROPERTY_SCAN_BYTES) - 16
    while p < end:
        r = parse_static_material(data, p, z, imports)
        if r:
            sm, end = r
            key = (sm["material"], sm["slot"])
            if key not in seen:
                seen.add(key)
                slots.append(sm)
            p = end
        else:
            p += 1
    return slots


def scan_package(g: Game, file_path: str):
    pp = package_path_from_file(file_path)
    pid = ue_hash(pp)
    cont = g.package_container.get(pid)
    if cont is None:
        return None
    z = ZenPackage(g.readers[cont].read(chunk_id(pid, EXPORT_BUNDLE_DATA)))
    classes = {e.class_index for e in z.exports}
    rec = {"path": pp}
    if STATIC_MESH in classes:
        imports = resolve_imports(z)
        e = next(e for e in z.exports if e.class_index == STATIC_MESH)
        rec["kind"] = "mesh"
        rec["slots"] = scan_mesh(z, z.export_data(e), imports)
        return rec
    kind = next((MATERIAL_CLASSES[c] for c in classes if c in MATERIAL_CLASSES), None)
    if kind:
        rec["kind"] = kind
        names = [n for n in z.names if n.startswith("/Game/")]
        rec["textures"] = [n for n in names if n.rsplit("/", 1)[-1].startswith("T_")]
        rec["materials"] = [n for n in names if n != pp and re.match(r"(M|MI|RMI|MM|RM)_", n.rsplit("/", 1)[-1])]
        return rec
    return None


_g = None


def _worker(paths):
    global _g
    if _g is None:
        _g = Game(PAKS)
    out = []
    for p in paths:
        try:
            r = scan_package(_g, p)
        except Exception as ex:  # keep going; record the failure
            r = {"path": package_path_from_file(p), "kind": "error", "error": repr(ex)[:200]}
        if r:
            out.append(r)
    return out


def cmd_scan(args):
    OUT.mkdir(parents=True, exist_ok=True)
    g = Game(PAKS)
    files = sorted({p for r in g.readers.values() for p in r.paths.values()
                    if p.endswith(".uasset") and "/End/Content/Environment/" in p and not p.rsplit("/", 1)[-1].startswith("T_")})
    print(f"{len(files)} environment packages (textures excluded)", flush=True)
    chunks = [files[i:i + 400] for i in range(0, len(files), 400)]
    t0, results = time.time(), []
    with mp.Pool(args.workers) as pool:
        for i, part in enumerate(pool.imap_unordered(_worker, chunks), 1):
            results.extend(part)
            if i % 10 == 0 or i == len(chunks):
                print(f"  {i}/{len(chunks)} chunks, {len(results)} records, {time.time() - t0:.0f} s", flush=True)
    (OUT / "scan.json").write_text(json.dumps(results))
    kinds = {}
    for r in results:
        kinds[r["kind"]] = kinds.get(r["kind"], 0) + 1
    print("records:", kinds)


# ------------------------------------------------------------------ report
def load_inventory():
    inv = {}
    with open(ROOT / "work" / "inventory.csv", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["error"]:
                continue
            inv[package_path_from_file(r["path"])] = (int(r["width"]), int(r["height"]), r["format"])
    return inv


TILE_SIZE = re.compile(r"_(\d+(?:\.\d+)?)m(?:_|$)", re.I)


def texture_usages(scan):
    mats = {r["path"]: r for r in scan if r["kind"] in ("MIC", "Material")}
    meshes = [r for r in scan if r["kind"] == "mesh"]

    def textures_of(m, depth=0, seen=None):
        seen = seen or set()
        if m in seen or depth > 8 or m not in mats:
            return set()
        seen.add(m)
        out = {t for t in mats[m]["textures"] if t.startswith("/Game/Environment/")}
        for parent in mats[m]["materials"]:
            out |= textures_of(parent, depth + 1, seen)
        return out

    uses = {}
    for mesh in meshes:
        for s in mesh["slots"]:
            if not s["material"]:
                continue
            for t in textures_of(s["material"]):
                uses.setdefault(t, []).append((mesh["path"], s["material"], s["densities"]))
    return uses


def pixel_cm(distance_m, hfov_deg, width_px):
    return 2.0 * distance_m * 100.0 * math.tan(math.radians(hfov_deg) / 2.0) / width_px


MAX_OUTPUT = 8192  # largest texture we would write (a 4096 px texture can only go 2x)


def cmd_report(args):
    scan = json.loads((OUT / "scan.json").read_text())
    inv = load_inventory()
    uses = texture_usages(scan)
    near, far = pixel_cm(args.distance_m, args.hfov, args.width), pixel_cm(10.0, args.hfov, args.width)
    rows = []
    for t, us in uses.items():
        if t not in inv:
            continue
        w, h, fmt = inv[t]
        if max(w, h) < 64:  # flat-colour placeholders
            continue
        name = t.rsplit("/", 1)[-1]
        m = TILE_SIZE.search(name)
        if m:
            texel = float(m.group(1)) * 100.0 / w
            basis, n_meshes = "name", len({u[0] for u in us})
        else:
            dens = [u[2][0] for u in us if u[2][0] > 0]
            if not dens:
                continue
            texel = max(dens) / w  # the most stretched use (largest world area per texel)
            basis, n_meshes = "uv0", len(dens)
        rows.append({"texture": t, "width": w, "height": h, "format": fmt, "basis": basis, "meshes": n_meshes,
                     "far_only": "FarLand" in name, "texel_cm": round(texel, 3),
                     f"px_per_texel_{args.distance_m:g}m": round(texel / near, 2), "px_per_texel_10m": round(texel / far, 2)})
    colour = [r["texel_cm"] for r in rows if r["texture"].endswith(("_C", "_CM")) and not r["far_only"]]
    norm = sorted(colour)[len(colour) // 2]
    for r in rows:
        need = r["texel_cm"] / norm
        cap = max(1, MAX_OUTPUT // max(r["width"], r["height"]))
        r["vs_norm"] = round(need, 2)
        r["suggest"] = 1 if r["far_only"] or need <= 1.5 else min(cap, 2 if need <= 3 else 4)
    rows.sort(key=lambda r: -r["vs_norm"])
    with open(OUT / "textures.csv", "w", newline="", encoding="utf-8") as f:
        wtr = csv.DictWriter(f, fieldnames=list(rows[0]))
        wtr.writeheader()
        wtr.writerows(rows)
    print(f"{len(rows)} textures with a mesh use. Game norm (median colour texture, far terrain excluded): "
          f"{norm:.3f} cm per texel = {norm / near:.1f} screen px at {args.distance_m:g} m, {norm / far:.2f} px at 10 m "
          f"(hfov {args.hfov}, {args.width} px)")
    suf = lambda r: r["texture"].rsplit("_", 1)[-1]
    for group in (("C", "CM"), ("N",), ("M",), ("R",), ("O",)):
        sel = [r for r in rows if suf(r) in group]
        if not sel:
            continue
        cnt = {k: sum(1 for r in sel if r["suggest"] == k) for k in (1, 2, 4)}
        print(f"  _{'/_'.join(group)}: {len(sel)} textures; to reach the norm: as is {cnt[1]}, 2x {cnt[2]}, 4x {cnt[4]}")


def cmd_show(args):
    rows = list(csv.DictReader(open(OUT / "textures.csv", encoding="utf-8")))
    for r in rows:
        if args.pattern.lower() in r["texture"].lower():
            print(r)
    scan = json.loads((OUT / "scan.json").read_text())
    for rec in scan:
        if rec["kind"] == "mesh" and args.pattern.lower() in rec["path"].lower():
            print(rec["path"], [(s["material"].rsplit("/", 1)[-1] if s["material"] else None, s["densities"]) for s in rec["slots"]])


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("--workers", type=int, default=12)
    r = sub.add_parser("report")
    r.add_argument("--distance-m", type=float, default=3.0)
    r.add_argument("--hfov", type=float, default=60.0)
    r.add_argument("--width", type=int, default=3840)
    sh = sub.add_parser("show")
    sh.add_argument("pattern")
    a = ap.parse_args()
    {"scan": cmd_scan, "report": cmd_report, "show": cmd_show}[a.cmd](a)
