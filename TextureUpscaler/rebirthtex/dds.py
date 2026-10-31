"""Minimal DDS (DX10 header) reading and writing for block-compressed mip chains."""

from __future__ import annotations

import struct

# DXGI_FORMAT values
DXGI = {
    "PF_DXT1": (71, 72),      # BC1_UNORM, BC1_UNORM_SRGB
    "PF_DXT5": (77, 78),      # BC3
    "PF_BC4": (80, 80),
    "PF_BC5": (83, 83),
    "PF_BC6H": (95, 95),      # BC6H_UF16
    "PF_BC7": (98, 99),
    "PF_B8G8R8A8": (87, 91),
    "PF_G8": (61, 61),        # R8_UNORM
}

DDSD_CAPS, DDSD_HEIGHT, DDSD_WIDTH, DDSD_PIXELFORMAT, DDSD_MIPMAPCOUNT, DDSD_LINEARSIZE = 0x1, 0x2, 0x4, 0x1000, 0x20000, 0x80000
DDSCAPS_COMPLEX, DDSCAPS_TEXTURE, DDSCAPS_MIPMAP = 0x8, 0x1000, 0x400000
DDPF_FOURCC = 0x4


def dxgi_format(pixel_format: str, srgb: bool) -> int:
    linear, srgb_format = DXGI[pixel_format]
    return srgb_format if srgb else linear


def write(path, width: int, height: int, dxgi: int, mips: list[bytes]) -> None:
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE
    caps = DDSCAPS_TEXTURE | (DDSCAPS_COMPLEX | DDSCAPS_MIPMAP if len(mips) > 1 else 0)
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, flags, height, width, len(mips[0]), 0, len(mips))
    header += struct.pack("<II4s20x", 32, DDPF_FOURCC, b"DX10")
    header += struct.pack("<IIIII", caps, 0, 0, 0, 0)
    header += struct.pack("<IIIII", dxgi, 3, 0, 1, 0)  # DX10: format, TEXTURE2D, misc, array size, misc2
    with open(path, "wb") as f:
        f.write(header)
        for mip in mips:
            f.write(mip)


def read(path) -> tuple[int, int, int, list[bytes]]:
    """Returns (width, height, dxgi format, mip payloads) for a DX10-header DDS written by texconv."""
    data = open(path, "rb").read()
    if data[:4] != b"DDS ":
        raise ValueError(f"{path} is not a DDS file")
    height, width = struct.unpack_from("<II", data, 12)
    mip_count = max(1, struct.unpack_from("<I", data, 28)[0])
    fourcc = data[84:88]
    if fourcc != b"DX10":
        raise ValueError(f"{path}: expected a DX10 header")
    dxgi = struct.unpack_from("<I", data, 128)[0]
    pos = 148
    block, size = _block_info(dxgi)
    mips, w, h = [], width, height
    for _ in range(mip_count):
        n = ((w + block - 1) // block) * ((h + block - 1) // block) * size
        mips.append(data[pos:pos + n])
        pos += n
        w, h = max(1, w // 2), max(1, h // 2)
    return width, height, dxgi, mips


def _block_info(dxgi: int) -> tuple[int, int]:
    if dxgi in (71, 72, 80):
        return 4, 8
    if dxgi in (77, 78, 83, 95, 98, 99):
        return 4, 16
    if dxgi in (87, 91, 28, 29):
        return 1, 4
    if dxgi == 61:
        return 1, 1
    raise ValueError(f"unsupported DXGI format {dxgi}")
