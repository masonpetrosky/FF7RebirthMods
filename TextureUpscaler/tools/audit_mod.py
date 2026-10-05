"""Ranks the upscaled textures in a built mod container by how much they depart from the original look,
to pick candidates for `build --exclude`. Each new top mip is box-downscaled back to the original size
(in linear light, like the GPU's mip filtering) and compared with the original top mip:

  lf     low-frequency brightness/colour error, % - visible where the GPU switches between the two mips
  mad    mean absolute difference, 0-255
  grain  ratio of total gradient (new / original) - texture or noise the model added
  sharp  ratio of gradient energy over total gradient (new / original) - soft edges made hard

Usage: audit_mod.py <Paks dir> <mod .utoc> <out .csv> [--sheet <out .png> [N]]
The optional contact sheet shows the N most suspicious textures (default 24) as original | new crops."""

from __future__ import annotations

import csv
import os
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rebirthtex import dds, pipeline, texture  # noqa: E402
from rebirthtex.game import Game, package_path_from_file  # noqa: E402
from rebirthtex.iostore import BULK_DATA, EXPORT_BUNDLE_DATA, IoStoreReader, chunk_id  # noqa: E402
from rebirthtex.zen import ZenPackage  # noqa: E402

LUMA = np.array([0.2126, 0.7152, 0.0722], np.float32)


def decode(pixel_format: str, width: int, height: int, data: bytes, tmp: Path) -> np.ndarray:
    dds.write(tmp / "a.dds", width, height, dds.dxgi_format(pixel_format, False), [data])
    pipeline._texconv(["-f", "R8G8B8A8_UNORM", "-m", "1", "-sx", "_x", "-o", str(tmp), str(tmp / "a.dds")])
    return np.frombuffer(dds.read(tmp / "a_x.dds")[3][0], np.uint8).reshape(height, width, 4)


_LEVELS = np.arange(256) / 255
LINEAR = np.where(_LEVELS <= 0.04045, _LEVELS / 12.92, ((_LEVELS + 0.055) / 1.055) ** 2.4).astype(np.float32)


def to_srgb(x: np.ndarray) -> np.ndarray:
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(np.clip(x, 0, None), 1 / 2.4) - 0.055)


def box_blur(x: np.ndarray, radius: int) -> np.ndarray:
    """Wrapping box blur over the first two axes, from running sums."""
    for _ in range(2):
        n = len(x)
        sums = np.cumsum(np.concatenate([x[n - radius - 1:], x, x[:radius]]), axis=0, dtype=np.float64)
        x = ((sums[2 * radius + 1:] - sums[:n]) / (2 * radius + 1)).astype(np.float32).swapaxes(0, 1)
    return x


def metrics(old: np.ndarray, new: np.ndarray) -> dict[str, float]:
    """old: HxWx4 original, new: 2Hx2Wx4 upscale (uint8). Opaque texels only when the original has alpha."""
    h, w = old.shape[:2]
    mask = (old[..., 3] >= 128).astype(np.float32)
    if mask.sum() < 16:
        mask[:] = 1
    m3 = mask[..., None]
    o = LINEAR[old[..., :3]]
    n = LINEAR[new[..., :3]].reshape(h, 2, w, 2, 3).mean(axis=(1, 3))
    cover = box_blur(m3, 3) + 1e-6
    err = box_blur((n - o) * m3, 3) / cover
    ref = box_blur(o * m3, 3) / cover
    lf = float((np.abs(err) / (ref + 0.02) * m3).sum() / (m3.sum() * 3) * 100)
    o_srgb, n_srgb = old[..., :3].astype(np.float32) / 255, to_srgb(n)
    mad = float((np.abs(n_srgb - o_srgb) * 255 * m3).sum() / (m3.sum() * 3))

    def gradients(img: np.ndarray) -> np.ndarray:
        y = img @ LUMA * 255
        g = np.abs(np.diff(y, axis=0))[:, :-1] + np.abs(np.diff(y, axis=1))[:-1, :]
        return g * mask[:-1, :-1]

    go, gn = gradients(o_srgb), gradients(n_srgb)
    grain = (gn.mean() + 0.5) / (go.mean() + 0.5)
    sharp = ((gn ** 2).mean() + 1) / (gn.mean() + 1) / (((go ** 2).mean() + 1) / (go.mean() + 1))
    return {"lf": lf, "mad": mad, "grain": float(grain), "sharp": float(sharp)}


def suspicion(r: dict) -> float:
    """Single ranking score: anything well beyond the typical result (lf ~1%, grain ~1.05, sharp ~1.1)."""
    return max(r["lf"] / 3, (r["grain"] - 1) / 0.6, (r["sharp"] - 1) / 0.6, r["mad"] / 8)


def sheet(game: Game, mod: IoStoreReader, rows: list[dict], out: Path, crop: int = 256) -> None:
    from PIL import Image, ImageDraw
    tiles = []
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        for r in rows:
            orig = game.load_texture(r["path"])
            o = orig.texture
            zen = ZenPackage(mod.read(chunk_id(orig.package_id, EXPORT_BUNDLE_DATA)))
            tex = texture.parse(zen.export_data(next(e for e in zen.exports if e.class_index == texture.TEXTURE2D_CLASS)))
            top = tex.mips[0]
            bulk = mod.read(chunk_id(orig.package_id, BULK_DATA))
            new = decode(tex.pixel_format, top.width, top.height, bulk[top.offset:top.offset + top.size], tmp)
            old = decode(o.pixel_format, o.width, o.height, orig.mip_data(0), tmp)
            gray = lambda a: Image.fromarray((a[..., :3] * (a[..., 3:] / 255) + 128 * (1 - a[..., 3:] / 255)).astype(np.uint8))
            a = gray(np.asarray(Image.fromarray(old).resize((top.width, top.height), Image.BICUBIC)))
            b = gray(new)
            diff = np.abs(np.asarray(a.convert("L"), np.float32) - np.asarray(b.convert("L"), np.float32))
            c = min(crop, top.width, top.height)
            best, by, bx = -1.0, 0, 0
            for y in range(0, top.height - c + 1, max(1, c // 2)):
                for x in range(0, top.width - c + 1, max(1, c // 2)):
                    v = float(diff[y:y + c, x:x + c].mean())
                    if v > best:
                        best, by, bx = v, y, x
            box = (bx, by, bx + c, by + c)
            tiles.append((r, a.crop(box).resize((crop, crop)), b.crop(box).resize((crop, crop))))
    cols = 2
    canvas = Image.new("RGB", (cols * (2 * crop + 30), ((len(tiles) + cols - 1) // cols) * (crop + 22)), "white")
    draw = ImageDraw.Draw(canvas)
    for i, (r, a, b) in enumerate(tiles):
        x0, y0 = (i % cols) * (2 * crop + 30), (i // cols) * (crop + 22)
        draw.text((x0 + 2, y0 + 2), f"{i + 1}. {r['path'].rsplit('/', 1)[1]}  lf {r['lf']:.1f}% grain {r['grain']:.2f} "
                  f"sharp {r['sharp']:.2f}", fill="black")
        canvas.paste(a, (x0, y0 + 18))
        canvas.paste(b, (x0 + crop + 4, y0 + 18))
    canvas.save(out)


_worker: dict = {}


def _start_worker(paks: str, utoc: str) -> None:
    _worker["game"], _worker["mod"] = Game(paks), IoStoreReader(utoc)


def _audit(path: str) -> dict:
    game, mod = _worker["game"], _worker["mod"]
    orig = game.load_texture(path)
    zen = ZenPackage(mod.read(chunk_id(orig.package_id, EXPORT_BUNDLE_DATA)))
    tex = texture.parse(zen.export_data(next(e for e in zen.exports if e.class_index == texture.TEXTURE2D_CLASS)))
    top, o = tex.mips[0], orig.texture
    bulk = mod.read(chunk_id(orig.package_id, BULK_DATA))
    with tempfile.TemporaryDirectory() as t:
        new = decode(tex.pixel_format, top.width, top.height, bulk[top.offset:top.offset + top.size], Path(t))
        old = decode(o.pixel_format, o.width, o.height, orig.mip_data(0), Path(t))
    return {"path": path, "size": o.width, **metrics(old, new)}


def main(paks: str, utoc: str, out_csv: str, *rest: str) -> None:
    mod = IoStoreReader(utoc)
    packages = sorted(package_path_from_file(p) for p in mod.paths.values() if p.endswith(".uasset"))
    rows = []
    # Each texture is independent; a third of the CPU threads leaves room for a build running alongside.
    with ProcessPoolExecutor(max(1, (os.cpu_count() or 3) // 3), initializer=_start_worker,
                             initargs=(paks, utoc)) as pool:
        for n, row in enumerate(pool.map(_audit, packages, chunksize=8), 1):
            rows.append(row)
            if n % 1000 == 0:
                print(f"{n}/{len(packages)} audited", flush=True)
    for r in rows:
        r["suspicion"] = suspicion(r)
    rows.sort(key=lambda r: -r["suspicion"])
    with open(out_csv, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows({k: (f"{v:.3f}" if isinstance(v, float) else v) for k, v in r.items()} for r in rows)
    for key in ("lf", "mad", "grain", "sharp"):
        values = np.array([r[key] for r in rows])
        print(f"{key:6s} median {np.median(values):6.2f}  p95 {np.percentile(values, 95):6.2f}  "
              f"p99 {np.percentile(values, 99):6.2f}  max {values.max():6.2f}")
    print(f"{sum(r['suspicion'] > 1 for r in rows)} of {len(rows)} beyond the typical range; most suspicious:")
    for r in rows[:15]:
        print(f"  {r['suspicion']:5.2f}  lf {r['lf']:5.1f}%  mad {r['mad']:5.1f}  grain {r['grain']:4.2f}  "
              f"sharp {r['sharp']:4.2f}  {r['path']}")
    if rest and rest[0] == "--sheet":
        count = int(rest[2]) if len(rest) > 2 else 24
        sheet(Game(paks), mod, rows[:count], Path(rest[1]))
        print(f"contact sheet: {rest[1]}")


if __name__ == "__main__":
    main(*sys.argv[1:])
