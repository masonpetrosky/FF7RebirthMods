# Running long batches

Upscaled mips are cached in `work\cache\<model>-cf\` (`work\cache\<model>\` with `--no-color-fix`),
so a build can be stopped at any time and restarted with the same command; finished textures are
reused instantly. To reuse results made with other settings - for example a cache from before the
colour fix existed - add `--fallback-cache work\cache\<folder>`: textures found there are packed as
they are, everything else is upscaled with the current settings. `--exclude <file>` keeps the listed
package paths (one per line, `#` comments allowed) at their original size, e.g. textures where the
model changed the look instead of adding detail.

`tools\batch_supervisor.py` runs a build only while the game is closed: it stops the build
within 30 s of the game starting and restarts it once the game has been closed for 10 minutes.
Launch it detached (it keeps running without a terminal) from the project folder:

```powershell
$proj = (Get-Location).Path
$paks = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH\End\Content\Paks'
$build = "`"$proj\.venv\Scripts\python.exe`" -m rebirthtex build --paks `"$paks`" --inventory work\inventory.csv --model models\4x-PBRify-UpscalerV4.safetensors --match /End/Content/Environment/ --suffix _C --min-size 256 --max-size 1024 --out work\full --mod-name FF7RebirthTextureUpscaler_P"
$verify = "`"$proj\.venv\Scripts\python.exe`" tools\verify_mod.py `"$paks`" work\full\FF7RebirthTextureUpscaler_P.utoc 200"
Start-Process python -ArgumentList @('tools\batch_supervisor.py', 'work\full.log', '--', 'cmd', '/c', "$build && $verify") -WorkingDirectory $proj -WindowStyle Hidden
```

Progress and the supervisor's pause/resume events go to `work\full.log`. To stop everything:

```powershell
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'batch_supervisor|rebirthtex build|verify_mod' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```

On an RTX 5070 Ti a 1024 px texture takes about 6.4 s with the PBRify DAT model.
