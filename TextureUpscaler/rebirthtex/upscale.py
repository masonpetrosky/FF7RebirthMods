"""AI upscaling of texture images with spandrel-loaded models (tiled, seam-aware)."""

from __future__ import annotations

import importlib
import math

import numpy as np
import torch
import torch.nn.functional as F
from spandrel import ModelLoader


def _cache_dat_masks() -> None:
    """DAT rebuilds its shifted-window attention masks on the CPU (and copies them to the GPU) in every
    shifted block whenever the input isn't its 64 px training size - about a third of the run time.
    They only depend on the window geometry, so keep them on the GPU per tile shape (output unchanged)."""
    attention = importlib.import_module("spandrel.architectures.DAT.__arch.DAT").Adaptive_Spatial_Attention
    if getattr(attention, "_masks_cached", False):
        return
    build, cache = attention.calculate_mask, {}

    def calculate_mask(self, H, W, dtype=None):
        key = (H, W, dtype, tuple(self.split_size), tuple(self.shift_size))
        if key not in cache:
            if len(cache) >= 4:
                cache.clear()
            cache[key] = tuple(mask.cuda() for mask in build(self, H, W, dtype=dtype))
        return cache[key]

    attention.calculate_mask = calculate_mask
    attention._masks_cached = True


def _spans(size: int, tile: int, overlap: int) -> list[tuple[int, int]]:
    """The fewest equal tiles of at most `tile` px covering [0, size), neighbours overlapping by 2 * overlap."""
    n = max(1, math.ceil((size - 2 * overlap) / (tile - 2 * overlap)))
    length = min(size, math.ceil((size - 2 * overlap) / n) + 2 * overlap)
    starts = [round(i * (size - length) / (n - 1)) for i in range(n)] if n > 1 else [0]
    return [(start, start + length) for start in starts]


class Upscaler:
    # 576: a 1024 texture plus its wrap padding (1088 px) splits into 2x2 tiles; 512 took 3x3 (1.8x the work).
    def __init__(self, model_path: str, tile: int = 576, overlap: int = 32, half: bool = True, bf16: bool = False):
        self.model = ModelLoader().load_from_file(model_path).cuda().eval()
        if self.model.architecture.id == "DAT":
            _cache_dat_masks()
        self.scale = self.model.scale
        self.half = half and self.model.supports_half
        # bfloat16 autocast for models without fp16 support (e.g. DAT); output matches fp32 within ~0.3/255.
        self.bf16 = bf16 and not self.half and self.model.supports_bfloat16
        if self.half:
            self.model.half()
        self.tile = tile
        self.overlap = overlap

    @torch.inference_mode()
    def _run(self, x: torch.Tensor) -> torch.Tensor:
        if self.bf16:
            with torch.autocast("cuda", dtype=torch.bfloat16):
                return self.model(x).float()
        return self.model(x.half() if self.half else x).float()

    @torch.inference_mode()
    def upscale(self, image: np.ndarray, wrap: bool = True, shrink: int = 1, srgb: bool = True) -> np.ndarray:
        """image: HxWx3 float32 in [0, 1] (sRGB-encoded for color). Returns (H*s/shrink)x(W*s/shrink)x3:
        shrink box-filters the model output on the GPU, in linear light when srgb is set."""
        h, w, _ = image.shape
        pad = self.overlap
        x = torch.from_numpy(image).permute(2, 0, 1)[None].cuda()
        # Wrap padding keeps tileable textures seamless; reflect is the fallback for atlases.
        x = F.pad(x, (pad, pad, pad, pad), mode="circular" if wrap else "reflect")
        s = self.scale
        out = torch.zeros((1, 3, (h + 2 * pad) * s, (w + 2 * pad) * s), device="cuda")
        weight = torch.zeros_like(out[:, :1])
        for y0, y1 in _spans(h + 2 * pad, self.tile, self.overlap):
            for x0, x1 in _spans(w + 2 * pad, self.tile, self.overlap):
                patch = self._run(x[:, :, y0:y1, x0:x1])
                ph, pw = patch.shape[2], patch.shape[3]
                # Feathered blend weights so tile borders don't show.
                ramp = self.overlap * s
                wy = torch.ones(ph, device="cuda")
                wx = torch.ones(pw, device="cuda")
                if ramp:
                    r = torch.linspace(0.05, 1, ramp, device="cuda")
                    wy[:ramp], wy[-ramp:] = torch.minimum(wy[:ramp], r), torch.minimum(wy[-ramp:], r.flip(0))
                    wx[:ramp], wx[-ramp:] = torch.minimum(wx[:ramp], r), torch.minimum(wx[-ramp:], r.flip(0))
                wgt = (wy[:, None] * wx[None, :])[None, None]
                out[:, :, y0 * s:y1 * s, x0 * s:x1 * s] += patch * wgt
                weight[:, :, y0 * s:y1 * s, x0 * s:x1 * s] += wgt
        out = (out / weight)[:, :, pad * s:(pad + h) * s, pad * s:(pad + w) * s].clamp(0, 1)
        if shrink > 1:
            out = downscale(out, shrink, srgb)
        return out[0].permute(1, 2, 0).cpu().numpy()


def srgb_to_linear(x: torch.Tensor) -> torch.Tensor:
    return torch.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(x: torch.Tensor) -> torch.Tensor:
    return torch.where(x <= 0.0031308, x * 12.92, 1.055 * x.clamp(min=0) ** (1 / 2.4) - 0.055)


def downscale(image: torch.Tensor, factor: int, srgb: bool) -> torch.Tensor:
    """Box-filters an NCHW image by an integer factor, in linear light for sRGB data."""
    data = srgb_to_linear(image) if srgb else image
    data = F.avg_pool2d(data, factor)
    return linear_to_srgb(data) if srgb else data
