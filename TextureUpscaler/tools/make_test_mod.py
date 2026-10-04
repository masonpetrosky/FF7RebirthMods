"""Builds a diagnostic mod that fills BC1 textures with solid white, to verify that the game loads
containers written by this project.

Usage: make_test_mod.py <Paks dir> <output dir> <inventory.csv> <path substring>
Every BC1 texture whose package path contains the substring is included."""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rebirthtex import iostore_writer as w  # noqa: E402
from rebirthtex import texture  # noqa: E402
from rebirthtex.cityhash import ue_hash  # noqa: E402
from rebirthtex.iostore import BULK_DATA, CONTAINER_HEADER, EXPORT_BUNDLE_DATA, IoStoreReader, chunk_id, chunk_type  # noqa: E402
from rebirthtex.zen import ZenPackage  # noqa: E402

WHITE_BC1_BLOCK = bytes.fromhex("ffffffff00000000")
MOD_NAME = "FF7RebirthTextureUpscaler_Test_P"


def whiten(reader: IoStoreReader, package_id: int):
    package_cid = chunk_id(package_id, EXPORT_BUNDLE_DATA)
    bulk_cid = chunk_id(package_id, BULK_DATA)
    package = bytearray(reader.read(package_cid))
    bulk = bytearray(reader.read(bulk_cid)) if bulk_cid in reader else bytearray()
    pkg = ZenPackage(bytes(package))
    export = next(e for e in pkg.exports if e.class_index == texture.TEXTURE2D_CLASS)
    tex = texture.parse(pkg.export_data(export))
    if tex.pixel_format != "PF_DXT1":
        return None
    for mip in tex.mips:
        fill = WHITE_BC1_BLOCK * (mip.size // 8)
        if mip.location == "inline":
            start = export.data_offset + mip.payload_pos
            package[start:start + mip.size] = fill
        elif mip.location == "bulk":
            bulk[mip.offset:mip.offset + mip.size] = fill
        else:
            return None
    return package_cid, bytes(package), bulk_cid, bytes(bulk)


def main(paks_dir: str, out_dir: str, inventory_csv: str, substring: str) -> None:
    rows = [r for r in csv.DictReader(open(inventory_csv, encoding="utf-8"))
            if substring in r["path"] and r["format"] == "PF_DXT1" and r["path"].endswith(".uasset")]
    by_container: dict[str, list[str]] = {}
    for r in rows:
        by_container.setdefault(r["container"], []).append(r["path"])
    container_id = ue_hash(MOD_NAME)
    chunks = [None]
    entries = {}
    for container, paths in sorted(by_container.items()):
        reader = IoStoreReader(Path(paks_dir) / f"{container}.utoc")
        header = reader.read(next(c for c in reader.chunk_ids if chunk_type(c) == CONTAINER_HEADER))
        _cid, store = w.parse_container_header(header)
        for path in paths:
            relative = path.replace("../../../", "", 1).rsplit(".", 1)[0]
            package_id = ue_hash(relative.replace("End/Content/", "/Game/", 1))
            result = whiten(reader, package_id)
            if result is None:
                continue
            package_cid, package, bulk_cid, bulk = result
            entries[package_id] = store[package_id]
            chunks.append((package_cid, package, relative + ".uasset"))
            if bulk:
                chunks.append((bulk_cid, bulk, relative + ".ubulk"))
    chunks[0] = (w.container_header_chunk_id(container_id), w.build_container_header(container_id, entries), None)
    out = Path(out_dir)
    w.write_container(out / f"{MOD_NAME}.utoc", container_id, chunks)
    repak = Path(__file__).resolve().parents[1] / "bin" / "repak.exe"
    with tempfile.TemporaryDirectory() as empty:
        subprocess.run([str(repak), "pack", "--version", "V11", "--mount-point", "/", empty, str(out / f"{MOD_NAME}.pak")],
                       check=True, capture_output=True)
    print(f"wrote {len(entries)} textures to {out / MOD_NAME}.utoc/.ucas/.pak")


if __name__ == "__main__":
    main(*sys.argv[1:5])
