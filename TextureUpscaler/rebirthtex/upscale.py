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
    def upscale(self, image: np.ndarray, wrap: bool = True, shrink: int = 1, srgb: bool = True, match: bool = False,
                mask: np.ndarray | None = None) -> np.ndarray:
        """image: HxWx3 float32 in [0, 1] (sRGB-encoded for color). Returns (H*s/shrink)x(W*s/shrink)x3:
        shrink box-filters the model output on the GPU, in linear light when srgb is set. match applies
        match_colors against the input; mask (HxW in [0, 1], e.g. alpha) marks the input texels it uses."""
        source, out = self.infer(image, wrap)
        if shrink > 1:
            out = downscale(out, shrink, srgb)
        if match:
            weights = None if mask is None else torch.from_numpy(mask)[None, None].cuda()
            out = match_colors(out, source, srgb, wrap, weights)
        return out[0].permute(1, 2, 0).cpu().numpy()

    @torch.inference_mode()
    def infer(self, image: np.ndarray, wrap: bool = True) -> tuple[torch.Tensor, torch.Tensor]:
        """Runs the model over the whole image in tiles. Returns (input, output) as NCHW GPU tensors, the output
        at the model's full scale and clamped to [0, 1]."""
        h, w, _ = image.shape
        pad = self.overlap
        source = torch.from_numpy(image).permute(2, 0, 1)[None].cuda()
        # Wrap padding keeps tileable textures seamless; reflect is the fallback for atlases.
        x = F.pad(source, (pad, pad, pad, pad), mode="circular" if wrap else "reflect")
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
        # in place: a 4096 px texture's 4x output is 3.3 GB per copy on the GPU
        out.div_(weight)
        del weight
        out = out[:, :, pad * s:(pad + h) * s, pad * s:(pad + w) * s].clamp_(0, 1)
        return source, out

    @torch.inference_mode()
    def upscale_levels(self, image: np.ndarray, factors: list[int], wrap: bool = True, srgb: bool = True,
                       match: bool = False, mask: np.ndarray | None = None) -> list[np.ndarray]:
        """Like upscale, but returns one image per factor in `factors` (each dividing the model scale), all
        box-filtered from the same model output, so stacked mips agree with each other."""
        source, out = self.infer(image, wrap)
        weights = None if mask is None else torch.from_numpy(mask)[None, None].cuda()
        levels = []
        for factor in factors:
            level = downscale(out, self.scale // factor, srgb) if factor < self.scale else out
            if match:
                level = match_colors(level, source, srgb, wrap, weights)
            levels.append(level[0].permute(1, 2, 0).cpu().numpy())
        return levels

    @torch.inference_mode()
    def upscale_normals(self, xy: np.ndarray, factors: list[int], wrap: bool = True) -> list[np.ndarray]:
        """Tangent-space normal map: xy is HxWx2 in [-1, 1] (Z is reconstructed). Runs the model on the encoded
        normal, keeps the original's low frequencies (each original texel's block of the result is shifted so
        its average matches the original normal) and renormalizes. Returns HxWx2 X/Y per factor in `factors`."""
        z = np.sqrt(np.clip(1.0 - (xy ** 2).sum(-1, keepdims=True), 0.0, 1.0))
        original = np.concatenate([xy, z], -1).astype(np.float32)
        source, out = self.infer((original + 1.0) * 0.5, wrap)
        normals = out.mul_(2.0).sub_(1.0)
        del out
        reference = source * 2.0 - 1.0
        s = self.scale
        offset = reference - F.avg_pool2d(normals, s)
        offset = F.pad(offset, (1, 1, 1, 1), mode="circular" if wrap else "replicate")
        normals += F.interpolate(offset, scale_factor=s, mode="bilinear", align_corners=False)[..., s:-s, s:-s]
        normals /= normals.norm(dim=1, keepdim=True).clamp(min=1e-6)
        levels = []
        for factor in factors:
            level = F.avg_pool2d(normals, s // factor) if factor < s else normals
            level = level / level.norm(dim=1, keepdim=True).clamp(min=1e-6)
            levels.append(level[0, :2].permute(1, 2, 0).cpu().numpy())
        return levels


def srgb_to_linear(x: torch.Tensor) -> torch.Tensor:
    return torch.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(x: torch.Tensor) -> torch.Tensor:
    return torch.where(x <= 0.0031308, x * 12.92, 1.055 * x.clamp(min=0) ** (1 / 2.4) - 0.055)


def downscale(image: torch.Tensor, factor: int, srgb: bool, rows: int = 2048) -> torch.Tensor:
    """Box-filters an NCHW image by an integer factor, in linear light for sRGB data (in strips of `rows`
    output rows, so a 16384 px image never needs full-size temporaries)."""
    if not srgb:
        return F.avg_pool2d(image, factor)
    out_h = image.shape[2] // factor
    strips = []
    for y in range(0, out_h, rows):
        part = image[:, :, y * factor:min(out_h, y + rows) * factor]
        strips.append(linear_to_srgb(F.avg_pool2d(srgb_to_linear(part), factor)))
    return torch.cat(strips, 2)


def _blur(t: torch.Tensor, sigma: float, wrap: bool) -> torch.Tensor:
    """Separable Gaussian blur of an NCHW tensor."""
    r = max(1, math.ceil(3 * sigma))
    k = torch.exp(-torch.arange(-r, r + 1, dtype=t.dtype, device=t.device) ** 2 / (2 * sigma ** 2))
    k = (k / k.sum()).repeat(t.shape[1], 1, 1, 1)
    mode = "circular" if wrap else "replicate"
    t = F.conv2d(F.pad(t, (r, r, 0, 0), mode=mode), k.view(-1, 1, 1, 2 * r + 1), groups=t.shape[1])
    return F.conv2d(F.pad(t, (0, 0, r, r), mode=mode), k.view(-1, 1, 2 * r + 1, 1), groups=t.shape[1])


def match_colors(image: torch.Tensor, reference: torch.Tensor, srgb: bool, wrap: bool,
                 weight: torch.Tensor | None = None, sigma: float = 2.0, max_gain: float = 2.0) -> torch.Tensor:
    """Removes the model's colour and brightness drift but keeps its detail: scales `image` (NCHW, an integer
    factor larger than `reference`) by the ratio of the two images' local means in linear light (Gaussian
    sigma in reference texels), so the new top mip matches the original where the GPU switches between them.
    A gain rather than an offset leaves dark texels of high-contrast textures alone. `weight` (N1HW at
    reference size, e.g. alpha) leaves texels such as transparent ones out of the means."""
    factor = image.shape[-1] // reference.shape[-1]
    linear = srgb_to_linear if srgb else (lambda v: v)
    w = torch.ones_like(reference[:, :1]) if weight is None else weight
    coverage = _blur(w, sigma, wrap).clamp(min=1e-3)

    def local_mean(t: torch.Tensor) -> torch.Tensor:
        return _blur(t * w, sigma, wrap) / coverage

    image = linear(image)
    eps = 1e-3
    gain = (local_mean(linear(reference)) + eps) / (local_mean(F.avg_pool2d(image, factor)) + eps)
    gain = F.pad(gain.clamp(1 / max_gain, max_gain), (1, 1, 1, 1), mode="circular" if wrap else "replicate")
    gain = F.interpolate(gain, scale_factor=factor, mode="bilinear", align_corners=False)
    out = (image * gain[..., factor:-factor, factor:-factor]).clamp(0, 1)
    return linear_to_srgb(out) if srgb else out
