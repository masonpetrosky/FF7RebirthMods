"""Turns the texture mod on or off for A/B testing (the game must be closed).

Usage: mod_switch.py on|off|status [mod name]
"Off" moves the mod's files to Paks\\~mods_disabled, "on" moves them back."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

PAKS = Path(r"C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH\End\Content\Paks")
ENABLED = PAKS / "~mods"
DISABLED = PAKS / "~mods_disabled"


def main(action: str, mod_name: str = "FF7RebirthTextureUpscaler_P") -> None:
    if action != "status":
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ff7rebirth_.exe", "/NH"], capture_output=True, text=True).stdout
        if "ff7rebirth_.exe" in out.lower():
            raise SystemExit("close the game first")
    src, dst = (DISABLED, ENABLED) if action == "on" else (ENABLED, DISABLED)
    if action in ("on", "off"):
        dst.mkdir(exist_ok=True)
        for f in src.glob(f"{mod_name}.*"):
            shutil.move(str(f), dst / f.name)
    on = sorted(f.name for f in ENABLED.glob(f"{mod_name}.*")) if ENABLED.exists() else []
    off = sorted(f.name for f in DISABLED.glob(f"{mod_name}.*")) if DISABLED.exists() else []
    print(f"enabled: {on or 'none'}\ndisabled: {off or 'none'}")


if __name__ == "__main__":
    main(*sys.argv[1:])
