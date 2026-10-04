"""Parsing of cooked UE 4.26 Texture2D exports (FTexturePlatformData)."""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

# EBulkDataFlags
BULKDATA_PAYLOAD_AT_END_OF_FILE = 0x1
BULKDATA_SIZE_64BIT = 0x2000
BULKDATA_FORCE_INLINE_PAYLOAD = 0x40
BULKDATA_PAYLOAD_IN_SEPARATE_FILE = 0x100
BULKDATA_UNUSED = 0x20
BULKDATA_OPTIONAL_PAYLOAD = 0x800
BULKDATA_MEMORY_MAPPED_PAYLOAD = 0x4000

TEXTURE2D_CLASS = 0x5B93BCA796D1FA6F  # FPackageObjectIndex of /Script/Engine.Texture2D

# Bytes per 4x4 block (block-compressed) or per pixel (uncompressed).
PIXEL_FORMATS = {
    "PF_DXT1": (4, 8), "PF_DXT3": (4, 16), "PF_DXT5": (4, 16), "PF_BC4": (4, 8), "PF_BC5": (4, 16),
    "PF_BC6H": (4, 16), "PF_BC7": (4, 16), "PF_B8G8R8A8": (1, 4), "PF_R8G8B8A8": (1, 4), "PF_G8": (1, 1),
    "PF_G16": (1, 2), "PF_FloatRGBA": (1, 8), "PF_A32B32G32R32F": (1, 16), "PF_R16F": (1, 2), "PF_V8U8": (1, 2),
}


@dataclass
class Mip:
    width: int
    height: int
    depth: int
    bulk_flags: int
    size: int
    offset: int            # offset inside the bulk chunk (or original file for inline data)
    payload_pos: int = -1  # position of inline payload inside the export data
    header_pos: int = 0    # position of this mip's FTexture2DMipMap inside the export data

    @property
    def location(self) -> str:
        f = self.bulk_flags
        if f & BULKDATA_UNUSED or self.size == 0:
            return "unused"
        if self.payload_pos >= 0:
            return "inline"
        if f & BULKDATA_OPTIONAL_PAYLOAD:
            return "optional"
        if f & BULKDATA_MEMORY_MAPPED_PAYLOAD:
            return "mmap"
        return "bulk"


@dataclass
class Texture:
    width: int
    height: int
    pixel_format: str
    num_slices: int
    packed: int
    first_mip: int
    mips: list[Mip] = field(default_factory=list)
    platform_data_pos: int = 0  # position of SizeX inside the export data
    end_pos: int = 0            # position just after the mip array

    @property
    def is_cube(self) -> bool:
        return bool(self.packed & 0x80000000)


def mip_size(pixel_format: str, width: int, height: int) -> int:
    block, size = PIXEL_FORMATS[pixel_format]
    return ((width + block - 1) // block) * ((height + block - 1) // block) * size


def parse(export: bytes) -> Texture:
    """Locates and parses the first FTexturePlatformData in a Texture2D export."""
    k = export.find(b"PF_")
    while k >= 0:
        length = struct.unpack_from("<i", export, k - 4)[0]
        if 4 <= length <= 32 and export[k + length - 1] == 0:
            try:
                return _parse_at(export, k - 16)
            except (struct.error, ValueError, KeyError):
                pass
        k = export.find(b"PF_", k + 1)
    raise ValueError("no texture platform data found")


def _parse_at(d: bytes, pos: int) -> Texture:
    start = pos
    size_x, size_y, packed = struct.unpack_from("<iiI", d, pos)
    pos += 12
    length = struct.unpack_from("<i", d, pos)[0]
    pixel_format = d[pos + 4:pos + 4 + length - 1].decode("ascii")
    pos += 4 + length
    if packed & (1 << 30):  # bHasOptData: FOptTexturePlatformData
        pos += 8
    first_mip, num_mips = struct.unpack_from("<ii", d, pos)
    pos += 8
    if not (0 < size_x <= 16384 and 0 < size_y <= 16384 and 0 < num_mips <= 16):
        raise ValueError("implausible texture header")
    tex = Texture(size_x, size_y, pixel_format, packed & 0x3FFFFFFF, packed, first_mip, platform_data_pos=start)
    for _ in range(num_mips):
        header_pos = pos
        pos += 4  # bCooked
        flags = struct.unpack_from("<I", d, pos)[0]
        pos += 4
        if flags & BULKDATA_SIZE_64BIT:
            count, size, offset = struct.unpack_from("<qqq", d, pos)
            pos += 24
        else:
            count, size, offset = struct.unpack_from("<iiq", d, pos)
            pos += 16
        payload_pos = -1
        inline = not (flags & (BULKDATA_PAYLOAD_AT_END_OF_FILE | BULKDATA_PAYLOAD_IN_SEPARATE_FILE |
                               BULKDATA_OPTIONAL_PAYLOAD | BULKDATA_MEMORY_MAPPED_PAYLOAD | BULKDATA_UNUSED))
        if inline and size > 0:
            payload_pos = pos
            pos += size
        w, h, z = struct.unpack_from("<iii", d, pos)
        pos += 12
        tex.mips.append(Mip(w, h, z, flags, size, offset, payload_pos, header_pos))
    tex.end_pos = pos
    return tex
