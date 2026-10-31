# FF7 Rebirth Shader Injector Sync

A [ReShade](https://reshade.me) add-on for **FINAL FANTASY VII REBIRTH** (PC) that makes
[Shader Injector](https://github.com/frostbone25/ShaderInjector) apply its shaders every time the game starts, also when
the game starts quickly.

## The problem

Shader Injector (the game loads it as `dsound.dll`) sleeps 5 seconds after it loads before it hooks Direct3D 12. From a
cold start the game takes longer than that to create its device and its pipeline library, so Shader Injector sees them.
When the game starts again from a warm file cache, for example right after it was closed, it created its Direct3D 12
device 0.9 s after it started, and its pipeline library soon after, before those hooks existed. Shader Injector hooks a
pipeline library's functions only when it sees `CreatePipelineLibrary`, so every pipeline the game then loaded from its
library, among them the lighting, water and SSGI ones, bypassed it, and for the whole session the game kept its own
shaders for them. Shader Injector's log then shows only a few `begin replacement` lines and reports

    ShaderAutomaticDiscovery->MaybeLogNeverSeenCompatibleShaderTypes: no capture attempt for PixelShader enabledPackages=36 enabledReplacements=8

## What the add-on does

It holds the game when it creates its first Direct3D 12 command queue (ReShade's `init_command_queue` event), until
Shader Injector's log (`ShaderInjector\Logs\ShaderInjector.log`) says its startup is complete
(`Hooks->CleanupDummyObjects: complete`), at most `WaitSeconds`. Then it creates an empty pipeline library on the game's
device. Every pipeline library is the same class (ReShade's wrapper, or the runtime's own), so Shader Injector then hooks
the functions the game's library uses as well, even if the game created its library first. On a cold start Shader
Injector is already done and the game does not wait.

It cannot hold the game any earlier: ReShade keeps a lock for all of `D3D12CreateDevice` (including the `create_device`
and `init_device` events), and Shader Injector creates a throwaway device of its own before it hooks, so a hold there
keeps Shader Injector waiting for as long as the hold lasts. Shader Injector's own throwaway objects are never held, a log
from an earlier session is never trusted, and without Shader Injector the add-on does nothing.

`ReShade.log` and the overlay's Add-ons tab say what happened, for example

    [ShaderInjectorSync] The game created its Direct3D 12 device 2.3 s and its first command queue 4.6 s after it started. Held it 1.5 s until Shader Injector had hooked Direct3D 12, then had Shader Injector hook the pipeline libraries.

## Installation

Put `FF7RebirthShaderInjectorSync.addon64` and `FF7RebirthShaderInjectorSync.ini` in
`...\FINAL FANTASY VII REBIRTH\End\Binaries\Win64`, next to ReShade 6.x with add-on support and Shader Injector. To
uninstall, delete both.

## Settings

`FF7RebirthShaderInjectorSync.ini`:

| Key | Default | Meaning |
| --- | --- | --- |
| `WaitSeconds` | 15 | Longest wait for Shader Injector's hooks at startup; 0 = never wait. |

## Building

Visual Studio 2022 or newer with the C++ x64 tools. In the repository root, `.\build.ps1` builds every add-on (this one
is `build\ShaderInjectorSync\FF7RebirthShaderInjectorSync.addon64`), and `.\package.ps1 ShaderInjectorSync` also
assembles a zip in `dist\`.

## License

MIT; see [LICENSE](../LICENSE). `third_party/reshade` and `third_party/imgui` in the repository root carry their own
licenses. FINAL FANTASY VII
REBIRTH is a trademark of Square Enix; this project is not affiliated with or endorsed by Square Enix.
