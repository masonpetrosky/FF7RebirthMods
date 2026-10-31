"""Oodle decompression via Epic's redistributable oo2core DLL (downloaded once and hash-checked)."""

from __future__ import annotations

import ctypes
import hashlib
import os
import urllib.request
from pathlib import Path

# Same pinned build that retoc/repak use.
_URL = ("https://github.com/WorkingRobot/OodleUE/raw/refs/heads/main/Engine/Source/Programs/Shared/"
        "EpicGames.Oodle/Sdk/2.9.10/win/redist/oo2core_9_win64.dll")
_SHA256 = "6f5d41a7892ea6b2db420f2458dad2f84a63901c9a93ce9497337b16c195f457"

_lib = None


def cache_dir() -> Path:
    path = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "FF7RebirthTextureUpscaler"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _load():
    global _lib
    if _lib is not None:
        return _lib
    dll = cache_dir() / "oo2core_9_win64.dll"
    if not dll.exists():
        data = urllib.request.urlopen(_URL, timeout=60).read()
        digest = hashlib.sha256(data).hexdigest()
        if digest != _SHA256:
            raise RuntimeError(f"oo2core_9_win64.dll hash mismatch: {digest}")
        dll.write_bytes(data)
    lib = ctypes.WinDLL(str(dll))
    fn = lib.OodleLZ_Decompress
    fn.restype = ctypes.c_ssize_t
    fn.argtypes = [ctypes.c_void_p, ctypes.c_ssize_t, ctypes.c_void_p, ctypes.c_ssize_t,
                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t,
                   ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ssize_t, ctypes.c_int]
    _lib = lib
    return lib


def decompress(data: bytes, raw_size: int) -> bytes:
    out = ctypes.create_string_buffer(raw_size)
    n = _load().OodleLZ_Decompress(data, len(data), out, raw_size, 1, 0, 0, None, 0, None, None, None, 0, 3)
    if n != raw_size:
        raise RuntimeError(f"Oodle decompression failed ({n} of {raw_size} bytes)")
    return out.raw
