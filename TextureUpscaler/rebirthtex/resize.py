"""Adds higher-resolution top mips to a cooked Texture2D package, keeping every original mip.

Layout changes (UE 4.26 cooked texture in a Zen package):
  * FTexturePlatformData: SizeX/SizeY become the new size, NumMips grows by the number of new
    mips and their FTexture2DMipMap entries are inserted in front of the existing ones.
  * The new mips' payloads (largest first) are prepended to the package's BulkData chunk, so every
    existing bulk mip's BulkDataOffsetInFile moves by their total size.
  * SkipOffset (absolute end of the platform data in the original cooked file) and the
    export's CookedSerialSize grow by the size of the inserted mip entries.
  * The package store entry's ExportBundlesSize becomes the new package size.
"""

from __future__ import annotations

import dataclasses
import struct

from . import texture
from .game import TexturePackage
from .iostore_writer import StoreEntry
from .zen import ZenPackage


@dataclasses.dataclass
class ResizedTexture:
    package: bytes
    bulk: bytes
    store_entry: StoreEntry


def _bulk_offset_pos(mip: texture.Mip) -> int:
    size_fields = 16 if mip.bulk_flags & texture.BULKDATA_SIZE_64BIT else 8
    return mip.header_pos + 4 + 4 + size_fields


def add_top_mip(tp: TexturePackage, payload: bytes, width: int, height: int) -> ResizedTexture:
    return add_top_mips(tp, [(payload, width, height)])


def add_top_mips(tp: TexturePackage, mips: list[tuple[bytes, int, int]]) -> ResizedTexture:
    """mips: (payload, width, height) for each new mip, largest first, each half the size of the one before
    and the last one twice the original top mip."""
    tex = tp.texture
    top = tex.mips[0]
    if top.location != "bulk":
        raise ValueError("top mip is not stored as bulk data")
    expect_w, expect_h = tex.width, tex.height
    for payload, width, height in reversed(mips):
        expect_w, expect_h = expect_w * 2, expect_h * 2
        if (width, height) != (expect_w, expect_h):
            raise ValueError("new mips must each double the size")
        if payload and len(payload) != texture.mip_size(tex.pixel_format, width, height):
            raise ValueError("payload size does not match the pixel format and dimensions")
    payload = b"".join(p for p, _w, _h in mips)
    width, height = mips[0][1], mips[0][2]

    export = bytearray(tp.zen.export_data(tp.export))
    old_size = len(export)

    for mip in tex.mips:
        if mip.location == "bulk":
            struct.pack_into("<q", export, _bulk_offset_pos(mip), mip.offset + len(payload))

    flags = top.bulk_flags
    entry, offset = b"", 0
    for data, w, h in mips:
        n = len(data)
        entry += struct.pack("<II", 1, flags)
        entry += struct.pack("<qq", n, n) if flags & texture.BULKDATA_SIZE_64BIT else struct.pack("<ii", n, n)
        entry += struct.pack("<q", offset) + struct.pack("<iii", w, h, 1)
        offset += n

    struct.pack_into("<ii", export, tex.platform_data_pos, width, height)
    num_mips_pos = top.header_pos - 4
    if struct.unpack_from("<i", export, num_mips_pos)[0] != len(tex.mips):
        raise ValueError("unexpected platform data layout")
    struct.pack_into("<i", export, num_mips_pos, len(tex.mips) + len(mips))
    skip_pos = tex.platform_data_pos - 8
    struct.pack_into("<q", export, skip_pos, struct.unpack_from("<q", export, skip_pos)[0] + len(entry))
    export[top.header_pos:top.header_pos] = entry

    start = tp.export.data_offset
    package = bytearray(tp.package[:start] + bytes(export) + tp.package[start + old_size:])
    index = tp.zen.exports.index(tp.export)
    base = tp.zen.export_map_offset + index * ZenPackage.EXPORT.size
    struct.pack_into("<Q", package, base + 8, tp.export.cooked_serial_size + len(entry))
    for i, e in enumerate(tp.zen.exports):
        if e.cooked_serial_offset > tp.export.cooked_serial_offset:
            struct.pack_into("<Q", package, tp.zen.export_map_offset + i * ZenPackage.EXPORT.size,
                             e.cooked_serial_offset + len(entry))

    store = dataclasses.replace(tp.store_entry, export_bundles_size=len(package))
    resized = ResizedTexture(bytes(package), payload + tp.bulk, store)
    _verify(tp, resized, mips)
    return resized


def _verify(original: TexturePackage, resized: ResizedTexture, mips: list[tuple[bytes, int, int]]) -> None:
    """Re-parses the result and checks every mip resolves to the expected bytes."""
    zen = ZenPackage(resized.package)
    if zen.export_data_end != len(resized.package):
        raise AssertionError("export sizes do not add up")
    export = next(e for e in zen.exports if e.class_index == texture.TEXTURE2D_CLASS)
    tex = texture.parse(zen.export_data(export))
    added = len(mips)
    if (tex.width, tex.height, len(tex.mips)) != (mips[0][1], mips[0][2], len(original.texture.mips) + added):
        raise AssertionError("resized header mismatch")
    for new, (payload, width, height) in zip(tex.mips, mips):
        if (new.width, new.height) != (width, height) or resized.bulk[new.offset:new.offset + new.size] != payload:
            raise AssertionError("new mip payload mismatch")
    for new, index in zip(tex.mips[added:], range(len(original.texture.mips))):
        old = original.texture.mips[index]
        if (new.width, new.height, new.location) != (old.width, old.height, old.location):
            raise AssertionError("existing mip changed")
        expected = original.mip_data(index)
        if new.location == "bulk":
            got = resized.bulk[new.offset:new.offset + new.size]
        else:
            start = export.data_offset + new.payload_pos
            got = resized.package[start:start + new.size]
        if got != expected:
            raise AssertionError(f"mip {index} data mismatch")
