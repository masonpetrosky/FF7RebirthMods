# FF7 Rebirth mods

Fixes and improvements for FINAL FANTASY VII REBIRTH (PC), one folder per mod. Each mod is downloaded and installed on
its own: its zip is on its Nexus Mods page and on this repository's Releases page. Until 2026-10-31 each mod had its
own repository; their releases up to then stay there (FF7RebirthVegetationAndHairFix, FF7RebirthNpcMotion,
FF7RebirthAnisotropic, FF7RebirthTextureUpscaler, all archived).

| Mod | What it does | Version | Nexus Mods |
|---|---|---|---|
| [Vegetation and Hair Fix](VegetationAndHairFix) | Removes the grainy dithering on foliage under DLSS, DLAA and FSR, and gives hair and fur soft edges | 0.3.1 | [2443](https://www.nexusmods.com/finalfantasy7rebirth/mods/2443) |
| [NPC Motion](NpcMotion) | Fixes the stale motion vectors that smear crowds under DLSS, plus DLSS settings checked against NVIDIA's guide | 0.2.0 | [2441](https://www.nexusmods.com/finalfantasy7rebirth/mods/2441) |
| [Anisotropic Filtering](Anisotropic) | 16x anisotropic filtering instead of the game's fixed 4x | 0.1.0 | [2442](https://www.nexusmods.com/finalfantasy7rebirth/mods/2442) |
| [Frame Limit](FrameLimit) | Replaces the game's 30/60/90/120 limit on rendered frames with one limit on the frames the display shows, frame generation included (3 below the refresh rate by default), and NVIDIA Reflex without frame generation | 0.1.0 | [2498](https://www.nexusmods.com/finalfantasy7rebirth/mods/2498) |
| [Texture Upscaler](TextureUpscaler) | AI texture upscaling that builds a `~mods` package from your own install | source only | [2440](https://www.nexusmods.com/finalfantasy7rebirth/mods/2440) |
| [Shader Injector Sync](ShaderInjectorSync) | Holds the game at startup until Shader Injector has hooked Direct3D 12, so quick relaunches keep its shaders | not released | |

All but the Texture Upscaler are ReShade add-ons: they need ReShade 6.x with add-on support, and each zip is extracted
into `FINAL FANTASY VII REBIRTH\End\Binaries\Win64`. The add-ons work together and with RenoDX HDR and Shader
Injector. Each folder's README has the details, settings and default keys.

## Building

Visual Studio 2022 or newer with the C++ x64 tools (CMake and Ninja come with it) and the Windows SDK.

```powershell
.\build.ps1                 # every add-on: build\<Mod>\FF7Rebirth<Mod>.addon64 and each mod's test tools
.\package.ps1 NpcMotion     # one mod's release zip: dist\FF7RebirthNpcMotion-v<version>.zip
```

The Texture Upscaler is Python; see its README. A tag `<Mod>-v<version>`, such as `NpcMotion-v0.2.0`, builds that mod
on GitHub and publishes its zip as a release.

## Layout

- One folder per mod, with its source, `config\` (the shipped ini), `docs\` and its own tools.
- `third_party\`: the ReShade and Dear ImGui headers and the DXIL hash from the DirectX Shader Compiler, shared by the
  add-ons, each with its license.
- `tools\`: scripts for investigating the game itself: starting it under RenderDoc, reading frame captures, finding
  code in the executable, measuring screenshots.

## License

MIT; see [LICENSE](LICENSE). The third-party components carry their own licenses, listed in each mod's
`THIRD_PARTY_NOTICES.md`. FINAL FANTASY VII REBIRTH is a trademark of Square Enix; these mods are not affiliated with
or endorsed by Square Enix.
