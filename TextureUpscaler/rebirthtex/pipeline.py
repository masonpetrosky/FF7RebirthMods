"""Upscales selected game textures and packs them into a ~mods container."""

from __future__ import annotations

import hashlib
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from . import dds, iostore_writer
from .cityhash import ue_hash
from .game import Game, TexturePackage, file_path_from_package
from .iostore import BULK_DATA, EXPORT_BUNDLE_DATA, chunk_id
from .resize import ResizedTexture, add_top_mip

ROOT = Path(__file__).resolve().parents[1]
TEXCONV = ROOT / "bin" / "texconv.exe"
REPAK = ROOT / "bin" / "repak.exe"

# Formats the pipeline re-encodes, and the DXGI format texconv should produce for each.
ENCODE = {"PF_DXT1": "BC1_UNORM", "PF_BC7": "BC7_UNORM"}
R8G8B8A8_UNORM = 28


def _texconv(args: list[str]) -> None:
    subprocess.run([str(TEXCONV), "-nologo", "-y", "-dx10", *args], check=True, capture_output=True)


def decode_mip(tp: TexturePackage, index: int, tmp: Path) -> np.ndarray:
    """Decodes one mip to an HxWx4 uint8 array (raw values, no color-space conversion)."""
    mip = tp.texture.mips[index]
    src = tmp / "src.dds"
    dds.write(src, mip.width, mip.height, dds.dxgi_format(tp.texture.pixel_format, False), [tp.mip_data(index)])
    _texconv(["-f", "R8G8B8A8_UNORM", "-m", "1", "-sx", "_rgba", "-o", str(tmp), str(src)])
    width, height, _fmt, mips = dds.read(tmp / "src_rgba.dds")
    return np.frombuffer(mips[0], dtype=np.uint8).reshape(height, width, 4)


def encode_mip(rgba: np.ndarray, pixel_format: str, tmp: Path) -> bytes:
    height, width, _ = rgba.shape
    src = tmp / "up.dds"
    dds.write(src, width, height, R8G8B8A8_UNORM, [np.ascontiguousarray(rgba, dtype=np.uint8).tobytes()])
    _texconv(["-f", ENCODE[pixel_format], "-m", "1", "-sx", "_bc", "-o", str(tmp), str(src)])
    return dds.read(tmp / "up_bc.dds")[3][0]


@dataclass
class Result:
    package_path: str
    status: str  # "upscaled", "cached", "skipped: ...", "failed: ..."
    seconds: float = 0.0


class Pipeline:
    def __init__(self, game: Game, model_path: str, work_dir: Path, srgb: bool = True, cached_only: bool = False):
        self.game = game
        self.cached_only = cached_only
        self.model_path = model_path
        self.work_dir = Path(work_dir)
        self.srgb = srgb
        self._upscaler = None

    @property
    def upscaler(self):
        if self._upscaler is None:
            from .upscale import Upscaler  # imports torch lazily
            self._upscaler = Upscaler(self.model_path, bf16=True)
        return self._upscaler

    def _cache_path(self, tp: TexturePackage) -> Path:
        digest = hashlib.sha1(tp.mip_data(0)).hexdigest()[:16]
        model = Path(self.model_path).stem
        return self.work_dir / "cache" / model / f"{tp.package_path.rsplit('/', 1)[1]}_{digest}.bin"

    def process(self, package_path: str) -> tuple[Result, ResizedTexture | None]:
        start = time.time()
        tp = self.game.load_texture(package_path)
        tex = tp.texture
        if tex.pixel_format not in ENCODE:
            return Result(package_path, f"skipped: format {tex.pixel_format}"), None
        if tex.mips[0].location != "bulk":
            return Result(package_path, "skipped: top mip not in bulk data"), None
        width, height = tex.width * 2, tex.height * 2
        cache = self._cache_path(tp)
        if cache.exists():
            payload = cache.read_bytes()
            return Result(package_path, "cached", time.time() - start), add_top_mip(tp, payload, width, height)
        if self.cached_only:
            return Result(package_path, "skipped: not upscaled yet"), None

        with tempfile.TemporaryDirectory() as tmp_name:
            tmp = Path(tmp_name)
            rgba = decode_mip(tp, 0, tmp)
            if rgba[..., :3].std(axis=(0, 1)).max() < 1.0:
                return Result(package_path, "skipped: flat"), None
            rgb = rgba[..., :3].astype(np.float32) / 255.0
            from .upscale import downscale
            up = downscale(self.upscaler.upscale(rgb, wrap=True), 2, srgb=self.srgb)
            out = np.empty((height, width, 4), dtype=np.uint8)
            out[..., :3] = np.clip(up * 255.0 + 0.5, 0, 255).astype(np.uint8)
            if rgba[..., 3].min() < 255:
                from PIL import Image
                alpha = Image.fromarray(rgba[..., 3]).resize((width, height), Image.LANCZOS)
                out[..., 3] = np.asarray(alpha)
            else:
                out[..., 3] = 255
            payload = encode_mip(out, tex.pixel_format, tmp)
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_bytes(payload)
        return Result(package_path, "upscaled", time.time() - start), add_top_mip(tp, payload, width, height)


def write_mod(out_dir: Path, mod_name: str, items) -> Path:
    """items: iterable of (package path, ResizedTexture); consumed lazily so memory stays flat."""
    container_id = ue_hash(mod_name)
    entries = {}

    def chunks():
        for package_path, r in items:
            package_id = ue_hash(package_path)
            entries[package_id] = r.store_entry
            relative = file_path_from_package(package_path)
            yield chunk_id(package_id, EXPORT_BUNDLE_DATA), r.package, relative + ".uasset"
            yield chunk_id(package_id, BULK_DATA), r.bulk, relative + ".ubulk"
        yield (iostore_writer.container_header_chunk_id(container_id),
               iostore_writer.build_container_header(container_id, entries), None)

    out_dir.mkdir(parents=True, exist_ok=True)
    utoc = out_dir / f"{mod_name}.utoc"
    iostore_writer.write_container(utoc, container_id, chunks())
    with tempfile.TemporaryDirectory() as empty:
        subprocess.run([str(REPAK), "pack", "--version", "V11", "--mount-point", "/", empty,
                        str(out_dir / f"{mod_name}.pak")], check=True, capture_output=True)
    return utoc
