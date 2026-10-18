# FF7 Rebirth Anisotropic Filtering

A [ReShade](https://reshade.me) add-on for **FINAL FANTASY VII REBIRTH** (PC) that raises the anisotropic filtering
of the game's textures from its fixed 4x to 16x. Anisotropic filtering keeps textures sharp on surfaces seen at a
shallow angle, such as floors and roads; at 4x they blur a short way from the camera.

## The problem

Rebirth (Unreal Engine 4.26) has no anisotropic filtering option, and nothing in its configuration sets
`r.MaxAnisotropy` (Square Enix's scalability settings leave it out), so its texture samplers get Unreal's default of
4x. So do the two samplers Square Enix added to the View uniform buffer, `SharedAnisotropic4XWrappedSampler` and
`SharedAnisotropic4XClampedSampler`, through which the materials sample their textures. Logged over a short play
session, every anisotropic sampler the game created asked for 4x (with wrap, clamp and mirror addressing in various
combinations) except one at 8x and one at 1x, and no root signature carried an anisotropic static sampler.

## What the add-on does

The add-on handles ReShade's `create_sampler` event, which sees every sampler the game creates, in descriptor heaps and
as static samplers in root signatures. An anisotropic sampler that asks for 2x or more gets `MaxAnisotropy` (16 by
default) instead. Samplers the game creates at 1x (anisotropic filtering off) and samplers without anisotropic
filtering are left alone, and the mip bias, addressing and LOD range stay as the game set them.

The add-on reads the level when the game starts, and Unreal reuses each sampler it has created for the rest of the
session, so a new level applies from the next start. `ReShade.log` lists each anisotropic sampler the first time the
game creates it, for example

    [Anisotropic] The game asked for 4x, wrap/wrap/wrap, mip bias +0.00, filter 0x55; set to 16x.

and the overlay's Add-ons tab lists them with the number of times each was created.

Tested with ReShade 6.8.0 alongside [RenoDX](https://github.com/clshortfuse/renodx)'s FF7 Rebirth add-on, the
[FF7 Rebirth Hair Fix](https://github.com/masonpetrosky/FF7RebirthHairFix) and
[FF7 Rebirth NPC Motion](https://github.com/masonpetrosky/FF7RebirthNpcMotion). The frame-rate cost has not been
measured.

## Installation

1. Install **ReShade 6.x with add-on support** (the "with full add-on support" download from
   [reshade.me](https://reshade.me)). Point it at
   `...\FINAL FANTASY VII REBIRTH\End\Binaries\Win64\ff7rebirth_.exe` and choose **DirectX 12**.
2. Download the latest `FF7RebirthAnisotropic-v*.zip` from [Releases](../../releases) and extract it into the same
   `Win64` folder, so you end up with:
   ```
   Win64\FF7RebirthAnisotropic.addon64
   Win64\FF7RebirthAnisotropic.ini
   Win64\FF7RebirthAnisotropic\licenses\...
   ```

Tested with game build 23447986 (Steam, October 2026). To uninstall, delete `FF7RebirthAnisotropic.addon64`,
`FF7RebirthAnisotropic.ini` and the `FF7RebirthAnisotropic` folder.

## Settings

`FF7RebirthAnisotropic.ini`:

| Key | Default | Meaning |
| --- | --- | --- |
| `MaxAnisotropy` | 16 | Level for the game's anisotropic samplers, 1-16; 0 = leave the game's samplers alone. The overlay's Add-ons tab offers 2x, 4x, 8x and 16x and saves the choice here. Applies from the next start. |

## Building

Visual Studio 2022 or newer with the C++ x64 tools: `.\build.ps1` builds `build\FF7RebirthAnisotropic.addon64`, and
`.\package.ps1` also assembles the release zip in `dist\`; pushing a `v*` tag builds and publishes it on GitHub.

## License

MIT; see [LICENSE](LICENSE). `third_party/reshade` and `third_party/imgui` carry their own licenses. FINAL FANTASY VII
REBIRTH is a trademark of Square Enix; this project is not affiliated with or endorsed by Square Enix.
