"""Lists every Texture2D in the game with its resolution, format and where its mips live."""

from __future__ import annotations

import csv
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from . import texture
from .iostore import EXPORT_BUNDLE_DATA, IoStoreReader, chunk_type
from .zen import ZenPackage

FIELDS = ["container", "path", "export", "width", "height", "format", "mips", "slices", "top_mip_location",
          "inline_mips", "bulk_mips", "optional_mips", "compression", "lod_group", "error"]


def scan_container(utoc: str) -> list[dict]:
    reader = IoStoreReader(utoc)
    rows = []
    for entry, cid in enumerate(reader.chunk_ids):
        if chunk_type(cid) != EXPORT_BUNDLE_DATA:
            continue
        data = reader.read_entry(entry)
        if b"PF_" not in data:
            continue
        try:
            pkg = ZenPackage(data)
        except Exception as exc:  # noqa: BLE001 - report and keep going
            rows.append({"container": reader.name, "path": reader.paths.get(entry, ""), "error": f"package: {exc}"})
            continue
        compression = next((n for n in pkg.names if n.startswith("TC_")), "")
        lod_group = next((n for n in pkg.names if n.startswith("TEXTUREGROUP_")), "")
        for e in pkg.exports:
            if e.class_index != texture.TEXTURE2D_CLASS:
                continue
            row = {"container": reader.name, "path": reader.paths.get(entry, ""), "export": e.object_name,
                   "compression": compression, "lod_group": lod_group}
            try:
                t = texture.parse(pkg.export_data(e))
                locations = [m.location for m in t.mips]
                row.update(width=t.width, height=t.height, format=t.pixel_format, mips=len(t.mips),
                           slices=t.num_slices, top_mip_location=locations[0] if locations else "",
                           inline_mips=locations.count("inline"), bulk_mips=locations.count("bulk"),
                           optional_mips=locations.count("optional"))
            except Exception as exc:  # noqa: BLE001
                row["error"] = f"texture: {exc}"
            rows.append(row)
    return rows


def main(paks_dir: str, out_csv: str) -> None:
    utocs = sorted(str(p) for p in Path(paks_dir).glob("*.utoc") if p.stem != "global")
    with ProcessPoolExecutor() as pool, open(out_csv, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        writer.writeheader()
        for utoc, rows in zip(utocs, pool.map(scan_container, utocs)):
            writer.writerows(rows)
            print(f"{Path(utoc).stem}: {len(rows)} textures", flush=True)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
