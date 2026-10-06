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


_probe_blocks: dict[str, tuple[bytes, bytes]] = {}


def probe_payload(pixel_format: str, width: int, height: int, square: int = 64) -> bytes:
    """A black/white checkerboard with `square`-texel squares, encoded in the texture's block format.

    Used in place of the upscaled top mip to check in game that the added mip is actually displayed:
    surfaces near the camera turn into a checkerboard and look normal again farther away."""
    if pixel_format not in _probe_blocks:
        white = np.full((4, 4, 4), 255, dtype=np.uint8)
        black = np.zeros((4, 4, 4), dtype=np.uint8)
        black[..., 3] = 255
        with tempfile.TemporaryDirectory() as tmp_name:
            _probe_blocks[pixel_format] = (encode_mip(white, pixel_format, Path(tmp_name)),
                                           encode_mip(black, pixel_format, Path(tmp_name)))
    white_block, black_block = _probe_blocks[pixel_format]
    blocks_x, blocks_y, per_square = (width + 3) // 4, (height + 3) // 4, square // 4
    rows = [b"".join(white_block if (bx // per_square + parity) % 2 == 0 else black_block for bx in range(blocks_x))
            for parity in (0, 1)]
    return b"".join(rows[(by // per_square) % 2] for by in range(blocks_y))


@dataclass
class Result:
    package_path: str
    status: str  # "upscaled", "cached", "skipped: ...", "failed: ..."
    seconds: float = 0.0


class Pipeline:
    def __init__(self, game: Game, model_path: str, work_dir: Path, srgb: bool = True, cached_only: bool = False,
                 color_fix: bool = True, fallback_cache: Path | None = None, probe: bool = False):
        self.game = game
        self.cached_only = cached_only
        self.probe = probe
        self.model_path = model_path
        self.work_dir = Path(work_dir)
        self.srgb = srgb
        self.color_fix = color_fix
        # Results made with other settings (e.g. before an upgrade) to reuse where these settings have none.
        self.fallback_cache = Path(fallback_cache) if fallback_cache else None
        self._upscaler = None

    @property
    def upscaler(self):
        if self._upscaler is None:
            from .upscale import Upscaler  # imports torch lazily
            self._upscaler = Upscaler(self.model_path, bf16=True)
        return self._upscaler

    def _cache_path(self, tp: TexturePackage) -> Path:
        digest = hashlib.sha1(tp.mip_data(0)).hexdigest()[:16]
        variant = Path(self.model_path).stem + ("-cf" if self.color_fix else "")
        return self.work_dir / "cache" / variant / f"{tp.package_path.rsplit('/', 1)[1]}_{digest}.bin"

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
        for found in (cache, self.fallback_cache / cache.name if self.fallback_cache else None):
            if found is not None and found.exists():
                payload = probe_payload(tex.pixel_format, width, height) if self.probe else found.read_bytes()
                return Result(package_path, "cached", time.time() - start), add_top_mip(tp, payload, width, height)
        if self.cached_only:
            return Result(package_path, "skipped: not upscaled yet"), None

        with tempfile.TemporaryDirectory() as tmp_name:
            tmp = Path(tmp_name)
            rgba = decode_mip(tp, 0, tmp)
            if rgba[..., :3].std(axis=(0, 1)).max() < 1.0:
                return Result(package_path, "skipped: flat"), None
            rgb = rgba[..., :3].astype(np.float32) / 255.0
            # Transparent texels hold no real colour (BC1 stores them black), so they don't steer the colour fix.
            weight = rgba[..., 3].astype(np.float32) / 255.0 if rgba[..., 3].min() < 255 else None
            up = self.upscaler.upscale(rgb, wrap=True, shrink=2, srgb=self.srgb, match=self.color_fix, mask=weight)
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
        if self.probe:
            payload = probe_payload(tex.pixel_format, width, height)
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
