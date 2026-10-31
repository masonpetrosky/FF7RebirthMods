"""Exports textures from the game as DDS (full mip chain) and PNG (top mip).

Usage: export_textures.py <Paks dir> <output dir> <package path> [<package path> ...]
Package paths look like /Game/Environment/Nature/Texture/T_Rock_Edge_17B_C"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from rebirthtex import dds  # noqa: E402
from rebirthtex.game import Game  # noqa: E402


def main(paks_dir: str, out_dir: str, *package_paths: str) -> None:
    game = Game(paks_dir)
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for path in package_paths:
        t = game.load_texture(path)
        name = path.rsplit("/", 1)[1]
        mips = [t.mip_data(i) for i in range(len(t.texture.mips))]
        dds_path = out / f"{name}.dds"
        dds.write(dds_path, t.texture.width, t.texture.height, dds.dxgi_format(t.texture.pixel_format, False), mips)
        subprocess.run([str(ROOT / "bin" / "texconv.exe"), "-nologo", "-y", "-ft", "png", "-m", "1", "-f", "R8G8B8A8_UNORM",
                        "-o", str(out), str(dds_path)], check=True, capture_output=True)
        print(f"{name}: {t.texture.width}x{t.texture.height} {t.texture.pixel_format}, {len(mips)} mips")


if __name__ == "__main__":
    main(*sys.argv[1:])
