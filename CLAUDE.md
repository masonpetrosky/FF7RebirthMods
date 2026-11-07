# FF7RebirthMods

All of our FINAL FANTASY VII REBIRTH mods in one repository, so a change can be made, built and tested across all of
them at once. Each mod still ships on its own (its own zip, Nexus page and release tag).

## Layout

| Folder | What it is | Ships as |
|---|---|---|
| `VegetationAndHairFix/` | ReShade add-on: undithered foliage, soft-edged hair and fur | `FF7RebirthVegetationAndHairFix.addon64` + `.ini` + folder with `dxcompiler.dll` |
| `NpcMotion/` | ReShade add-on: NPC motion vectors, DLSS settings; `docs/recon.md` is the main investigation log of the game's renderer | `FF7RebirthNpcMotion.addon64` + `.ini` + folder with `dxcompiler.dll` |
| `Anisotropic/` | ReShade add-on: 16x anisotropic filtering | `FF7RebirthAnisotropic.addon64` + `.ini` |
| `ShaderInjectorSync/` | ReShade add-on: holds startup until Shader Injector has hooked D3D12 | `FF7RebirthShaderInjectorSync.addon64` + `.ini` |
| `TextureUpscaler/` | Python: AI texture upscaling into a `~mods` IoStore package (its own `.venv`, `bin/`, `models/`, `work/`, all git-ignored) | `FF7RebirthTextureUpscaler_P.pak/.ucas/.utoc` in `Paks\~mods` |
| `third_party/` | ReShade and Dear ImGui headers, DXIL hash (DXC); one copy for every add-on | |
| `tools/` | Game-wide investigation scripts: `launch_rdc.ps1` (game under RenderDoc), `rdc.sh` + `rdc_*.py` (read captures), `disasm.py`, `xref*.py`, `callers.py`, `memread.py` (the exe), `decode_png16.ps1` + `shot_bands.py` (HDR screenshots), `perf_session.py` + `perf_report.py` (frame times at one spot with each mod switched off and on by its key), `cpu_sampler.cpp` + `cpu_profile.py` (where the game's CPU time goes, per thread and module), `gpu_timer.cpp` (development add-on: Shader Injector's passes timed on the GPU) | |
| `captures/` | git-ignored: every RenderDoc capture of the game and what the tools dumped from them (e.g. the Kalm capture `ff7r_frame8775.rdc`) | |
| `gamedata/` | git-ignored: the game's own config files, unpacked from its pak | |

Mod-specific tools stay in the mod (`NpcMotion/tools/` skinning scripts, `TextureUpscaler/tools/`). Paths in docs are
relative to the repository root unless the doc says otherwise.

## Build, check, package

- `.\build.ps1` builds every add-on and test tool into `build\<Mod>\`; `.\build.ps1 FF7RebirthNpcMotion` builds one
  target. Each `<Mod>/CMakeLists.txt` also builds alone (`cmake -S <Mod>`).
- Bit-for-bit comparisons: set `$env:CL='/Brepro'; $env:_LINK_='/Brepro'` before building and two builds of the same
  source give identical files. That is how the 2026-10-31 move into this repository was checked against the five
  original repositories (all add-ons, test tools and release zips identical).
- `.\package.ps1 <Mod>` makes `dist\FF7Rebirth<Mod>-v<version>.zip` (version from `<Mod>/CMakeLists.txt`; the two
  add-ons that use `dxcompiler.dll` get the pinned DXC release).
- Releasing a mod: bump `project(... VERSION)` in its `CMakeLists.txt`, rewrite its `docs/release-notes.md`, commit,
  tag `<Mod>-v<version>` and push the tag; CI builds the zip and creates the GitHub release. Nexus uploads are
  separate and need the user's OK per action. The Nexus pages' "GitHub" download mirror points at the mod's release
  here (`releases/tag/<Mod>-v<version>`; `releases/latest` would be whichever mod released last), for all three
  add-ons since 2026-11-07: NPC Motion 0.2.0, and Anisotropic 0.1.0 and Vegetation and Hair Fix 0.3.0, published here
  that day with the same zips as their former repositories' releases (not marked latest). Move the mirror at every
  release.

## Working on the game

- The game's Win64 folder is `C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH\End\Binaries\Win64`.
  The game locks loaded add-ons: replace files only with the game closed, and copy what you replace to
  `..\..\Backups\FF7Rebirth\YYYY-MM-DD-before-<change>\` first. Which build of each mod is installed is recorded in
  `..\README.md`.
- Next to our add-ons in Win64: ReShade 6.8 (`dxgi.dll`; since Shader Injector creates a dummy swap chain first, the
  game window reads `ReShade2.ini`, while add-on settings such as RenoDX's stay in `ReShade.ini`), RenoDX HDR
  (`renodx-ff7rebirth.addon64`), Shader Injector (`dsound.dll`, `ShaderInjector\`). SCSKiller removed its recorder
  (`d3d12.dll`) on 2026-10-30; its log is `AppData\Local\Programs\SCSKiller\data\app.log`.
- Keys already taken: Home = ReShade overlay, Page Down = ReShade screenshot, Insert/Delete = Shader Injector menu
  and toggle, F3 and F11 = the game (F11 toggles windowed mode), F4/F5/F7 and End = NPC Motion, F6/F8/F9 = Vegetation
  and Hair Fix (set in the installed ini; the shipped ini has no keys). Check this list before adding a key.
- RenderDoc: `tools\launch_rdc.ps1` starts the game under RenderDoc 1.46 with every hook DLL switched off for the
  session (DLSS does not run under RenderDoc) and `tools\psopatch.py` working around the game's pipeline-library
  assert. Captures land in `captures\`. `tools/rdc.sh <script>` runs a capture script headless.
- How the add-ons share the game: NPC Motion and Vegetation and Hair Fix each load their own `dxcompiler.dll`
  (NPC Motion also looks in the Vegetation and Hair Fix folder). Shader Injector hooks ReShade's D3D12 wrappers, and
  its dummy D3D12 device is the game's own on a one-GPU PC, so no extra device events reach the add-ons.
- Performance: `tools/perf_session.py <out>` (game in front, party standing still) records PresentMon and toggles
  each mod by its key; `.\build.ps1 cpu_sampler` first for its CPU profile. Results go to `captures\perf\`. Measured
  2026-10-31 at a Grasslands spot (4K, DLSS Performance): GPU-bound at 63 fps; Shader Injector costs 1.7 ms of the
  15.8 ms frame, soft edges 0.1 ms, the NPC Motion fix nothing, and the Vegetation and Hair Fix saves 0.3 ms (a cutout
  draws faster than the dithering). NPC Motion's descriptor mirror takes 13% of the RHIThread (`NpcMotion/docs/recon.md`).
- Shader Injector itself is frostbone25's project: our shader changes to it live in the separate clone
  `..\ShaderInjector` as one branch per upstream pull request, not here.

## History

Until 2026-10-31 these were five repositories: FF7RebirthVegetationAndHairFix, FF7RebirthTextureUpscaler,
FF7RebirthNpcMotion, FF7RebirthAnisotropic and FF7RebirthShaderInjectorSync. Each came in with its full history
(`git log --follow` works across the move); their tags are now `<Mod>-v<version>`, commit IDs changed (the
old-to-new maps are kept with the original repositories in `..\..\Backups\Projects\2026-10-31-before-ff7-monorepo\`),
and NPC Motion's shelved wind motion vector experiment is the branch `shelved/npcmotion-wind-motion-vectors`.
