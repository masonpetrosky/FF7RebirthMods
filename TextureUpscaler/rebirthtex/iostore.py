"""Reader for Unreal Engine 4.26 IoStore containers (.utoc/.ucas) as used by FF7 Rebirth."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

from . import oodle

TOC_MAGIC = b"-==--==--==--==-"

# EIoChunkType (UE 4.26)
EXPORT_BUNDLE_DATA = 2
BULK_DATA = 3
OPTIONAL_BULK_DATA = 4
MEMORY_MAPPED_BULK_DATA = 5
CONTAINER_HEADER = 10

FLAG_COMPRESSED = 1
FLAG_ENCRYPTED = 2
FLAG_SIGNED = 4
FLAG_INDEXED = 8


def chunk_id(package_id: int, chunk_type: int, index: int = 0) -> bytes:
    """FIoChunkId: uint64 id, uint16 index, uint8 pad, uint8 type."""
    return struct.pack("<QHBB", package_id, index, 0, chunk_type)


@dataclass
class TocHeader:
    version: int
    header_size: int
    entry_count: int
    compressed_block_count: int
    compressed_block_entry_size: int
    compression_method_count: int
    compression_method_length: int
    compression_block_size: int
    directory_index_size: int
    partition_count: int
    container_id: int
    flags: int


class IoStoreReader:
    def __init__(self, utoc_path: str | Path):
        self.utoc_path = Path(utoc_path)
        self.ucas_path = self.utoc_path.with_suffix(".ucas")
        self.name = self.utoc_path.stem
        data = self.utoc_path.read_bytes()
        if data[:16] != TOC_MAGIC:
            raise ValueError(f"{self.utoc_path} is not an IoStore TOC")
        v = struct.unpack_from("<B3xIIIIIIIIIQ", data, 16)
        self.header = TocHeader(*v[:11], flags=data[80])
        h = self.header
        if h.flags & FLAG_ENCRYPTED:
            raise ValueError(f"{self.name} is encrypted")

        pos = h.header_size
        n = h.entry_count
        self.chunk_ids = [data[pos + 12 * i: pos + 12 * i + 12] for i in range(n)]
        pos += 12 * n
        self.offsets = []
        for i in range(n):
            e = data[pos + 10 * i: pos + 10 * i + 10]
            self.offsets.append((int.from_bytes(e[0:5], "big"), int.from_bytes(e[5:10], "big")))
        pos += 10 * n
        self.blocks = []
        for i in range(h.compressed_block_count):
            e = data[pos + 12 * i: pos + 12 * i + 12]
            self.blocks.append((int.from_bytes(e[0:5], "little"), int.from_bytes(e[5:8], "little"),
                                int.from_bytes(e[8:11], "little"), e[11]))
        pos += 12 * h.compressed_block_count
        self.methods = ["none"]
        for i in range(h.compression_method_count):
            raw = data[pos + i * h.compression_method_length: pos + (i + 1) * h.compression_method_length]
            self.methods.append(raw.rstrip(b"\0").decode().lower())
        pos += h.compression_method_count * h.compression_method_length
        if h.flags & FLAG_SIGNED:
            hash_size = struct.unpack_from("<i", data, pos)[0]
            pos += 4 + 2 * hash_size + 20 * h.compressed_block_count
        self.paths: dict[int, str] = {}
        if h.flags & FLAG_INDEXED and h.directory_index_size:
            self._read_directory_index(data[pos: pos + h.directory_index_size])
        self.index = {cid: i for i, cid in enumerate(self.chunk_ids)}

    def _read_directory_index(self, d: bytes) -> None:
        pos = 0

        def fstring() -> str:
            nonlocal pos
            n = struct.unpack_from("<i", d, pos)[0]
            pos += 4
            if n == 0:
                return ""
            if n < 0:
                s = d[pos: pos - 2 * n].decode("utf-16-le")
                pos += -2 * n
            else:
                s = d[pos: pos + n].decode("utf-8", "replace")
                pos += n
            return s.rstrip("\0")

        mount = fstring()
        count = struct.unpack_from("<i", d, pos)[0]
        pos += 4
        dirs = [struct.unpack_from("<4I", d, pos + 16 * i) for i in range(count)]
        pos += 16 * count
        count = struct.unpack_from("<i", d, pos)[0]
        pos += 4
        files = [struct.unpack_from("<3I", d, pos + 12 * i) for i in range(count)]
        pos += 12 * count
        count = struct.unpack_from("<i", d, pos)[0]
        pos += 4
        strings = [fstring() for _ in range(count)]
        none = 0xFFFFFFFF

        def walk(index: int, prefix: str) -> None:
            while index != none:
                name, first_child, next_sibling, first_file = dirs[index]
                path = prefix + (strings[name] + "/" if name != none else "")
                f = first_file
                while f != none:
                    fname, next_file, user_data = files[f]
                    self.paths[user_data] = path + strings[fname]
                    f = next_file
                walk(first_child, path)
                index = next_sibling

        if dirs:
            walk(0, mount)

    def __contains__(self, cid: bytes) -> bool:
        return cid in self.index

    def read(self, cid: bytes) -> bytes:
        return self.read_entry(self.index[cid])

    def read_entry(self, entry: int) -> bytes:
        offset, length = self.offsets[entry]
        if length == 0:
            return b""
        bs = self.header.compression_block_size
        first, last = offset // bs, (offset + length - 1) // bs
        out = bytearray()
        with open(self.ucas_path, "rb") as f:
            for b in range(first, last + 1):
                block_offset, comp_size, raw_size, method = self.blocks[b]
                f.seek(block_offset)
                raw = f.read(comp_size)
                if method:
                    if self.methods[method] != "oodle":
                        raise NotImplementedError(self.methods[method])
                    raw = oodle.decompress(raw, raw_size)
                out += raw[:raw_size]
        start = offset - first * bs
        return bytes(out[start: start + length])


def chunk_type(cid: bytes) -> int:
    return cid[11]


def chunk_package_id(cid: bytes) -> int:
    return struct.unpack_from("<Q", cid, 0)[0]
