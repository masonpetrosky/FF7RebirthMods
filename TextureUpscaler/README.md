# FF7 Rebirth Texture Upscaler

Open-source toolchain that AI-upscales textures from **FINAL FANTASY VII REBIRTH** (PC) on your own
machine and packs them into a `~mods` container the game loads. No game assets are distributed:
you run it against your own installation.

**Status: work in progress.**

## How it works

- `rebirthtex.iostore` / `iostore_writer` read and write the game's UE 4.26 IoStore containers
  (`.utoc`/`.ucas`), including Rebirth's container header variant (four extra bytes after the
  package count). Oodle decompression uses Epic's redistributable `oo2core_9_win64.dll`,
  downloaded on first use and checked against a pinned SHA-256.
- `rebirthtex.zen` and `rebirthtex.texture` parse cooked packages and `FTexturePlatformData`.
- `rebirthtex.resize` adds an upscaled top mip while keeping every original mip byte-for-byte,
  so textures look exactly as authored at normal distances and gain detail up close. Every
  rewritten package is re-parsed and verified.
- `rebirthtex.upscale` runs a spandrel-loaded model on the GPU with tiling and wrap padding
  (seamless for tiling textures), then downsamples the 4x result to 2x in linear light and
  matches its low-frequency colours to the original, so the model adds detail without shifting
  brightness or saturation where the GPU blends between the new and the original top mip.
- `toolsudit_mod.py` ranks the textures in a build by how much they change the look, to pick
  any to keep at their original size (`build --exclude`); see `docsatch.md`.
- texconv (DirectXTex) re-encodes to the texture's original block format.

## Usage

```powershell
py -3.12 -m venv .venv
.venv\Scripts\pip --isolated install torch torchvision --index-url https://download.pytorch.org/whl/cu128
.venv\Scripts\pip --isolated install spandrel numpy pillow
.venv\Scripts\python -m rebirthtex setup                       # texconv, repak, PBRify model (pinned)
.venv\Scripts\python -m rebirthtex inventory "<game>\End\Content\Paks" work\inventory.csv
.venv\Scripts\python -m rebirthtex build --paks "<game>\End\Content\Paks" --inventory work\inventory.csv `
    --model models\4x-PBRify-UpscalerV4.safetensors --match "_NIBL" --out out
```

Copy the three files from `out\` (`.utoc`, `.ucas`, `.pak`) into
`<game>\End\Content\Paks\~mods\`.

## Credits

- Upscaling model: [PBRify Remix](https://github.com/Kim2091/PBRify_Remix) by Kim2091 (CC0).
- [DirectXTex/texconv](https://github.com/microsoft/DirectXTex) (MIT), [repak](https://github.com/trumank/repak) (MIT/Apache-2.0),
  [spandrel](https://github.com/chaiNNer-org/spandrel) (MIT), [PyTorch](https://pytorch.org).
- Format research cross-checked against [CUE4Parse](https://github.com/FabianFG/CUE4Parse) and
  [retoc](https://github.com/trumank/retoc).

FINAL FANTASY VII REBIRTH is a trademark of Square Enix; this project is not affiliated with or
endorsed by Square Enix.
