"""Writes uncompressed UE 4.26 IoStore containers (.utoc/.ucas) in the layout FF7 Rebirth uses."""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field
from pathlib import Path

from .iostore import CONTAINER_HEADER, TOC_MAGIC, chunk_id

BLOCK_SIZE = 65536
FLAG_INDEXED = 8
TOC_VERSION_DIRECTORY_INDEX = 2
NAME_HASH_VERSION = 0xC1640000


@dataclass
class StoreEntry:
    """FPackageStoreEntry (32 bytes) plus the imported package ids it points to."""
    export_bundles_size: int
    export_count: int
    export_bundle_count: int
    load_order: int
    pad: int
    imported_packages: list[int] = field(default_factory=list)


def parse_container_header(data: bytes) -> tuple[int, dict[int, StoreEntry]]:
    """Parses FF7 Rebirth's container header (UE 4.26 with 4 extra bytes after PackageCount)."""
    container_id, _package_count, _extra = struct.unpack_from("<QIi", data, 0)
    pos = 16
    for _ in range(2):  # Names, NameHashes
        pos += 4 + struct.unpack_from("<i", data, pos)[0]
    count = struct.unpack_from("<i", data, pos)[0]
    pos += 4
    ids = struct.unpack_from(f"<{count}Q", data, pos)
    pos += 8 * count
    pos += 4  # store entries byte size
    entries = {}
    for i, package_id in enumerate(ids):
        base = pos + 32 * i
        size, exports, bundles, load_order, pad, n_imports, rel = struct.unpack_from("<QiiIIiI", data, base)
        imports = list(struct.unpack_from(f"<{n_imports}Q", data, base + 24 + rel)) if n_imports else []
        entries[package_id] = StoreEntry(size, exports, bundles, load_order, pad, imports)
    return container_id, entries


def build_container_header(container_id: int, entries: dict[int, StoreEntry]) -> bytes:
    ids = list(entries)
    out = bytearray(struct.pack("<QIi", container_id, len(ids), 0))
    out += struct.pack("<i", 0)                               # Names
    out += struct.pack("<iQ", 8, NAME_HASH_VERSION)           # NameHashes (version only)
    out += struct.pack(f"<i{len(ids)}Q", len(ids), *ids)      # PackageIds
    table = bytearray()
    imports_blob = bytearray()
    imports_base = 32 * len(ids)
    for i, package_id in enumerate(ids):
        e = entries[package_id]
        field_pos = 32 * i + 24
        rel = imports_base + len(imports_blob) - field_pos if e.imported_packages else 0
        table += struct.pack("<QiiIIiI", e.export_bundles_size, e.export_count, e.export_bundle_count,
                             e.load_order, e.pad, len(e.imported_packages), rel)
        imports_blob += struct.pack(f"<{len(e.imported_packages)}Q", *e.imported_packages)
    store = table + imports_blob
    out += struct.pack("<i", len(store)) + store
    out += struct.pack("<i", 0)  # CulturePackageMap
    out += struct.pack("<i", 0)  # PackageRedirects
    return bytes(out)


def _fstring(s: str) -> bytes:
    raw = s.encode("utf-8") + b"\0"
    return struct.pack("<i", len(raw)) + raw


def _directory_index(mount_point: str, files: list[tuple[str, int]]) -> bytes:
    """Builds FIoDirectoryIndexResource for (path relative to mount point, toc entry index) pairs."""
    none = 0xFFFFFFFF
    strings: list[str] = []
    string_index: dict[str, int] = {}

    def intern(s: str) -> int:
        if s not in string_index:
            string_index[s] = len(strings)
            strings.append(s)
        return string_index[s]

    dirs = [[none, none, none, none]]  # name, first child, next sibling, first file
    file_entries: list[list[int]] = []  # name, next file, user data
    children: dict[tuple[int, str], int] = {}
    for path, entry in files:
        parts = path.split("/")
        d = 0
        for part in parts[:-1]:
            key = (d, part)
            if key not in children:
                children[key] = len(dirs)
                dirs.append([intern(part), none, dirs[d][1], none])
                dirs[d][1] = children[key]
            d = children[key]
        file_entries.append([intern(parts[-1]), dirs[d][3], entry])
        dirs[d][3] = len(file_entries) - 1

    out = bytearray(_fstring(mount_point))
    out += struct.pack("<i", len(dirs)) + b"".join(struct.pack("<4I", *e) for e in dirs)
    out += struct.pack("<i", len(file_entries)) + b"".join(struct.pack("<3I", *e) for e in file_entries)
    out += struct.pack("<i", len(strings)) + b"".join(_fstring(s) for s in strings)
    return bytes(out)


def write_container(utoc_path: Path, container_id: int, chunks: list[tuple[bytes, bytes, str | None]],
                    mount_point: str = "../../../") -> None:
    """chunks: iterable of (chunk id, data, path relative to the mount point or None).

    Chunk data is streamed straight to the .ucas, so arbitrarily large containers can be written."""
    utoc_path = Path(utoc_path)
    utoc_path.parent.mkdir(parents=True, exist_ok=True)
    ids: list[bytes] = []
    paths: list[str | None] = []
    blocks: list[tuple[int, int, int]] = []
    offsets: list[tuple[int, int]] = []
    metas = bytearray()
    virtual = 0
    written = 0
    with open(utoc_path.with_suffix(".ucas"), "wb") as ucas:
        for cid, data, path in chunks:
            ids.append(cid)
            paths.append(path)
            offsets.append((virtual, len(data)))
            for start in range(0, len(data), BLOCK_SIZE):
                piece = data[start:start + BLOCK_SIZE]
                blocks.append((written, len(piece), len(piece)))
                padding = -len(piece) % 16
                ucas.write(piece + b"\0" * padding)
                written += len(piece) + padding
            virtual += -(-len(data) // BLOCK_SIZE) * BLOCK_SIZE
            metas += hashlib.sha1(data).digest() + b"\0" * 12 + b"\0"

    files = [(path, i) for i, path in enumerate(paths) if path]
    directory = _directory_index(mount_point, files) if files else b""

    header = bytearray(144)
    header[0:16] = TOC_MAGIC
    struct.pack_into("<B3xIIIIIIIIIQ", header, 16, TOC_VERSION_DIRECTORY_INDEX, 144, len(ids), len(blocks), 12,
                     0, 32, BLOCK_SIZE, len(directory), 0, container_id)
    header[80] = FLAG_INDEXED if files else 0

    toc = bytearray(header)
    for cid in ids:
        toc += cid
    for offset, length in offsets:
        toc += offset.to_bytes(5, "big") + length.to_bytes(5, "big")
    for offset, comp, raw in blocks:
        toc += offset.to_bytes(5, "little") + comp.to_bytes(3, "little") + raw.to_bytes(3, "little") + b"\0"
    toc += directory
    toc += metas
    utoc_path.write_bytes(toc)


def container_header_chunk_id(container_id: int) -> bytes:
    return chunk_id(container_id, CONTAINER_HEADER)
