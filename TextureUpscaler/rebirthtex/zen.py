"""Parser for UE 4.26 Zen (IoStore) packages: summary, name map, export map and export data."""

from __future__ import annotations

import struct
from dataclasses import dataclass


@dataclass
class ExportEntry:
    cooked_serial_offset: int
    cooked_serial_size: int
    object_name: str
    outer_index: int
    class_index: int
    super_index: int
    template_index: int
    global_import_index: int
    object_flags: int
    filter_flags: int
    data_offset: int = 0  # position of this export's data inside the package chunk


class ZenPackage:
    SUMMARY = struct.Struct("<QQII10i")  # FMappedName x2, PackageFlags, CookedHeaderSize, 10 offsets/sizes
    EXPORT = struct.Struct("<QQQQQQQQIB3x")  # FExportMapEntry, 72 bytes

    def __init__(self, data: bytes):
        self.data = data
        (name, source_name, self.package_flags, self.cooked_header_size,
         names_off, names_size, hashes_off, hashes_size, self.import_map_offset, self.export_map_offset,
         self.export_bundles_offset, self.graph_data_offset, self.graph_data_size, _pad) = self.SUMMARY.unpack_from(data, 0)
        self.names = self._load_names(data[names_off:names_off + names_size], hashes_size // 8 - 1)
        self.name = self.mapped_name(name)
        self.header_size = self.graph_data_offset + self.graph_data_size

        self.imports = list(struct.unpack_from(f"<{(self.export_map_offset - self.import_map_offset) // 8}Q", data,
                                               self.import_map_offset))
        count = (self.export_bundles_offset - self.export_map_offset) // self.EXPORT.size
        self.exports: list[ExportEntry] = []
        for i in range(count):
            v = self.EXPORT.unpack_from(data, self.export_map_offset + i * self.EXPORT.size)
            self.exports.append(ExportEntry(v[0], v[1], self.mapped_name(v[2]), *v[3:10]))
        self._locate_export_data()

    @staticmethod
    def _load_names(blob: bytes, count: int) -> list[str]:
        names, pos = [], 0
        for _ in range(count):
            b0, b1 = blob[pos], blob[pos + 1]
            pos += 2
            wide, length = b0 & 0x80, ((b0 & 0x7F) << 8) | b1
            if wide:
                names.append(blob[pos:pos + 2 * length].decode("utf-16-le"))
                pos += 2 * length
            else:
                names.append(blob[pos:pos + length].decode("latin-1"))
                pos += length
        return names

    def mapped_name(self, value: int) -> str:
        index = value & 0x3FFFFFFF
        number = value >> 32
        base = self.names[index] if index < len(self.names) else f"<name {index}>"
        return f"{base}_{number - 1}" if number else base

    def _locate_export_data(self) -> None:
        """Export data follows the header in export-bundle serialize order."""
        pos = self.export_bundles_offset
        bundle_count_guess = []
        # Bundle headers: (FirstEntryIndex, EntryCount) until the entries begin; read the first header to size it.
        first_entry, entry_count = struct.unpack_from("<II", self.data, pos)
        # Headers come first; the number of headers is the first entry index offset implied by layout.
        headers = []
        while True:
            fe, ec = struct.unpack_from("<II", self.data, pos)
            headers.append((fe, ec))
            pos += 8
            total = sum(h[1] for h in headers)
            # Entries start right after the last header; stop once headers cover all entries contiguously.
            if fe + ec == total and pos + 8 * total <= self.graph_data_offset and self._entries_valid(pos, total):
                break
            if len(headers) > len(self.exports) * 2 + 2:
                raise ValueError("could not parse export bundles")
        offset = self.header_size
        for fe, ec in headers:
            for k in range(ec):
                local_index, command = struct.unpack_from("<II", self.data, pos + 8 * (fe + k))
                if command == 1:  # Serialize
                    e = self.exports[local_index]
                    e.data_offset = offset
                    offset += e.cooked_serial_size
        self.export_data_end = offset
        del bundle_count_guess, first_entry, entry_count

    def _entries_valid(self, pos: int, total: int) -> bool:
        seen = set()
        for k in range(total):
            local_index, command = struct.unpack_from("<II", self.data, pos + 8 * k)
            if local_index >= len(self.exports) or command > 1:
                return False
            seen.add((local_index, command))
        return len(seen) == total

    def export_data(self, e: ExportEntry) -> bytes:
        return self.data[e.data_offset:e.data_offset + e.cooked_serial_size]
