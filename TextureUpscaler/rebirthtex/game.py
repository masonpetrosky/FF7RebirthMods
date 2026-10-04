"""Access to the game's texture packages across all IoStore containers."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from . import texture
from .cityhash import ue_hash
from .iostore import BULK_DATA, EXPORT_BUNDLE_DATA, IoStoreReader, chunk_id, chunk_package_id, chunk_type
from .iostore_writer import StoreEntry, parse_container_header
from .zen import ExportEntry, ZenPackage


def package_path_from_file(path: str) -> str:
    """'../../../End/Content/X/Y.uasset' -> '/Game/X/Y'"""
    p = path.replace("../../../", "", 1).rsplit(".", 1)[0]
    if p.startswith("End/Content/"):
        return "/Game/" + p[len("End/Content/"):]
    if p.startswith("Engine/Content/"):
        return "/Engine/" + p[len("Engine/Content/"):]
    return "/" + p


def file_path_from_package(package_path: str) -> str:
    """'/Game/X/Y' -> 'End/Content/X/Y' (relative to the ../../../ mount point)"""
    if package_path.startswith("/Game/"):
        return "End/Content/" + package_path[len("/Game/"):]
    if package_path.startswith("/Engine/"):
        return "Engine/Content/" + package_path[len("/Engine/"):]
    raise ValueError(package_path)


@dataclass
class TexturePackage:
    package_path: str
    package_id: int
    container: str
    package: bytes
    bulk: bytes
    zen: ZenPackage
    export: ExportEntry
    texture: texture.Texture
    store_entry: StoreEntry

    def mip_data(self, index: int) -> bytes:
        mip = self.texture.mips[index]
        if mip.location == "inline":
            start = self.export.data_offset + mip.payload_pos
            return self.package[start:start + mip.size]
        if mip.location == "bulk":
            return self.bulk[mip.offset:mip.offset + mip.size]
        raise NotImplementedError(f"mip stored as {mip.location}")


class Game:
    def __init__(self, paks_dir: str | Path):
        self.paks_dir = Path(paks_dir)
        self.readers = {p.stem: IoStoreReader(p) for p in sorted(self.paks_dir.glob("*.utoc")) if p.stem != "global"}
        self.package_container: dict[int, str] = {}
        for name, reader in self.readers.items():
            for cid in reader.chunk_ids:
                if chunk_type(cid) == EXPORT_BUNDLE_DATA:
                    self.package_container.setdefault(chunk_package_id(cid), name)
        self._store_cache: dict[str, dict[int, StoreEntry]] = {}

    def store_entries(self, container: str) -> dict[int, StoreEntry]:
        if container not in self._store_cache:
            reader = self.readers[container]
            header = reader.read(next(c for c in reader.chunk_ids if chunk_type(c) == 10))
            self._store_cache[container] = parse_container_header(header)[1]
        return self._store_cache[container]

    def load_texture(self, package_path: str) -> TexturePackage:
        package_id = ue_hash(package_path)
        container = self.package_container[package_id]
        reader = self.readers[container]
        package = reader.read(chunk_id(package_id, EXPORT_BUNDLE_DATA))
        bulk_cid = chunk_id(package_id, BULK_DATA)
        bulk = reader.read(bulk_cid) if bulk_cid in reader else b""
        zen = ZenPackage(package)
        exports = [e for e in zen.exports if e.class_index == texture.TEXTURE2D_CLASS]
        if len(exports) != 1:
            raise ValueError(f"{package_path}: expected one Texture2D export, found {len(exports)}")
        tex = texture.parse(zen.export_data(exports[0]))
        return TexturePackage(package_path, package_id, container, package, bulk, zen, exports[0], tex,
                              self.store_entries(container)[package_id])
