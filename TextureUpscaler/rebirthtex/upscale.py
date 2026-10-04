"""AI upscaling of texture images with spandrel-loaded models (tiled, seam-aware)."""

from __future__ import annotations

import numpy as np
import torch
import torch.nn.functional as F
from spandrel import ModelLoader


class Upscaler:
    def __init__(self, model_path: str, tile: int = 512, overlap: int = 32, half: bool = True, bf16: bool = False):
        self.model = ModelLoader().load_from_file(model_path).cuda().eval()
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
    def upscale(self, image: np.ndarray, wrap: bool = True) -> np.ndarray:
        """image: HxWx3 float32 in [0, 1] (sRGB-encoded for color). Returns (H*s)x(W*s)x3."""
        h, w, _ = image.shape
        pad = self.overlap
        x = torch.from_numpy(image).permute(2, 0, 1)[None].cuda()
        # Wrap padding keeps tileable textures seamless; reflect is the fallback for atlases.
        x = F.pad(x, (pad, pad, pad, pad), mode="circular" if wrap else "reflect")
        s = self.scale
        out = torch.zeros((1, 3, (h + 2 * pad) * s, (w + 2 * pad) * s), device="cuda")
        weight = torch.zeros_like(out[:, :1])
        step = self.tile - 2 * self.overlap
        for y0 in range(0, h + 2 * pad - 2 * self.overlap, step):
            for x0 in range(0, w + 2 * pad - 2 * self.overlap, step):
                y1, x1 = min(y0 + self.tile, h + 2 * pad), min(x0 + self.tile, w + 2 * pad)
                y0c, x0c = max(0, y1 - self.tile), max(0, x1 - self.tile)
                patch = self._run(x[:, :, y0c:y1, x0c:x1])
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
                out[:, :, y0c * s:y1 * s, x0c * s:x1 * s] += patch * wgt
                weight[:, :, y0c * s:y1 * s, x0c * s:x1 * s] += wgt
        out = (out / weight)[:, :, pad * s:(pad + h) * s, pad * s:(pad + w) * s]
        return out[0].permute(1, 2, 0).clamp(0, 1).cpu().numpy()


def srgb_to_linear(x: np.ndarray) -> np.ndarray:
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(x: np.ndarray) -> np.ndarray:
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(np.clip(x, 0, None), 1 / 2.4) - 0.055)


def downscale(image: np.ndarray, factor: int, srgb: bool) -> np.ndarray:
    """Box-filters by an integer factor, in linear light for sRGB data."""
    h, w, c = image.shape
    data = srgb_to_linear(image) if srgb else image
    data = data.reshape(h // factor, factor, w // factor, factor, c).mean(axis=(1, 3))
    return linear_to_srgb(data) if srgb else data
