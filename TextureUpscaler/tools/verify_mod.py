"""Independently verifies a built mod container against the game's original textures.

Usage: verify_mod.py <Paks dir> <mod .utoc> [content checks: N textures, default all]"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rebirthtex import dds, iostore_writer, pipeline, texture  # noqa: E402
from rebirthtex.game import Game, package_path_from_file  # noqa: E402
from rebirthtex.iostore import BULK_DATA, CONTAINER_HEADER, EXPORT_BUNDLE_DATA, IoStoreReader, chunk_id, chunk_type  # noqa: E402
from rebirthtex.zen import ZenPackage  # noqa: E402


def decode(pixel_format: str, width: int, height: int, data: bytes, tmp: Path) -> np.ndarray:
    dds.write(tmp / "v.dds", width, height, dds.dxgi_format(pixel_format, False), [data])
    pipeline._texconv(["-f", "R8G8B8A8_UNORM", "-m", "1", "-sx", "_x", "-o", str(tmp), str(tmp / "v.dds")])
    return np.frombuffer(dds.read(tmp / "v_x.dds")[3][0], np.uint8).reshape(height, width, 4)


def main(paks: str, utoc: str, limit: str = "0") -> None:
    game, mod = Game(paks), IoStoreReader(utoc)
    _cid, entries = iostore_writer.parse_container_header(mod.read(next(c for c in mod.chunk_ids if chunk_type(c) == CONTAINER_HEADER)))
    packages = [package_path_from_file(p) for i, p in mod.paths.items() if p.endswith(".uasset")]
    errors, diffs = [], []
    for n, path in enumerate(sorted(packages), 1):
        try:
            orig = game.load_texture(path)
            pid = orig.package_id
            pkg_bytes, bulk = mod.read(chunk_id(pid, EXPORT_BUNDLE_DATA)), mod.read(chunk_id(pid, BULK_DATA))
            if entries[pid].export_bundles_size != len(pkg_bytes):
                raise AssertionError("store entry size mismatch")
            if entries[pid].imported_packages != orig.store_entry.imported_packages:
                raise AssertionError("imported packages changed")
            zen = ZenPackage(pkg_bytes)
            export = next(e for e in zen.exports if e.class_index == texture.TEXTURE2D_CLASS)
            tex = texture.parse(zen.export_data(export))
            o = orig.texture
            if (tex.width, tex.height, len(tex.mips)) != (o.width * 2, o.height * 2, len(o.mips) + 1):
                raise AssertionError("unexpected dimensions or mip count")
            if sum(m.size for m in tex.mips if m.location == "bulk") != len(bulk):
                raise AssertionError("bulk chunk has unaccounted bytes")
            for k, m in enumerate(tex.mips[1:]):
                got = bulk[m.offset:m.offset + m.size] if m.location == "bulk" else \
                    pkg_bytes[export.data_offset + m.payload_pos:export.data_offset + m.payload_pos + m.size]
                if got != orig.mip_data(k):
                    raise AssertionError(f"original mip {k} differs")
            if int(limit) == 0 or n <= int(limit):
                top = tex.mips[0]
                with tempfile.TemporaryDirectory() as t:
                    new = decode(tex.pixel_format, top.width, top.height, bulk[top.offset:top.offset + top.size], Path(t))
                    old = decode(o.pixel_format, o.width, o.height, orig.mip_data(0), Path(t))
                small = new[..., :3].astype(np.float32).reshape(o.height, 2, o.width, 2, 3).mean(axis=(1, 3))
                diffs.append((np.abs(small - old[..., :3]).mean(), path))
        except Exception as exc:  # noqa: BLE001
            errors.append(f"{path}: {exc}")
    print(f"{len(packages)} textures checked, {len(errors)} errors")
    for e in errors[:20]:
        print("  ERROR", e)
    if diffs:
        values = sorted(d for d, _ in diffs)
        print(f"content: new top mip vs original after 2x downscale, mean abs diff (0-255): "
              f"median {np.median(values):.2f}, p95 {values[int(0.95 * (len(values) - 1))]:.2f}, max {values[-1]:.2f}")
        for d, p in sorted(diffs, reverse=True)[:5]:
            print(f"  largest: {d:.2f} {p}")


if __name__ == "__main__":
    main(*sys.argv[1:])
