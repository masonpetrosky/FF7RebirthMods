"""Downloads the third-party tools and the upscaling model, pinned by SHA-256."""

from __future__ import annotations

import hashlib
import io
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# (url, sha256 of the download, member to extract or None for a plain file, destination)
DEPENDENCIES = [
    ("https://github.com/microsoft/DirectXTex/releases/download/may2026/texconv.exe",
     "dcfdec10244e02cf5037fba089c55fb7e1326b1c8181742d77d15fa5cb5eef06", None, ROOT / "bin" / "texconv.exe"),
    ("https://github.com/trumank/repak/releases/download/v0.2.3/repak_cli-x86_64-pc-windows-msvc.zip",
     "6720d602144d75df477a99d5bedb6ea780997546afc335901d4937cafeaa73fa", "repak.exe", ROOT / "bin" / "repak.exe"),
    ("https://github.com/Kim2091/PBRify_Remix/releases/download/v1.7.2_ComfyOnly/PBRify_Remix_1.7.2_ComfyUI_ONLY.zip",
     "d9755a1e97cd299eb61f8a9420f52478dc7df5da0fdc9e60890c763eae3a8c95", "4x-PBRify-UpscalerV4.safetensors",
     ROOT / "models" / "4x-PBRify-UpscalerV4.safetensors"),
]


def main() -> None:
    for url, sha256, member, dest in DEPENDENCIES:
        if dest.exists():
            print(f"ok       {dest.relative_to(ROOT)}")
            continue
        print(f"download {url}")
        data = urllib.request.urlopen(url, timeout=120).read()
        digest = hashlib.sha256(data).hexdigest()
        if digest != sha256:
            raise SystemExit(f"hash mismatch for {url}: {digest}")
        if member:
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                name = next(n for n in z.namelist() if n.rsplit("/", 1)[-1] == member)
                data = z.read(name)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        print(f"wrote    {dest.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
