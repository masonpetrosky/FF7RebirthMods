# Starts FINAL FANTASY VII REBIRTH under RenderDoc 1.46 for a frame capture and waits for the game to exit.
# For the session only, the D3D hook DLLs in the game folder are switched off (ReShade dxgi.dll, Shader Injector
# dsound.dll, a d3d12.dll proxy such as SCSKiller's recorder) and a steam_appid.txt lets the game start without Steam
# relaunching it; all of it is undone when the game exits (or with -Restore after a crash of this script).
# RenderDoc refuses D3D12 pipeline libraries and the game asserts on that, so psopatch.py turns the game's driver PSO
# cache off in memory while RenderDoc holds the new process (--opt-delay-for-debugger); see psopatch.py.
# Captures land in work\captures\ff7r_frame<N>.rdc; F12 captures in game.
param([switch]$Restore)
$ErrorActionPreference = 'Stop'

$game = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH'
$win64 = Join-Path $game 'End\Binaries\Win64'
$tools = Join-Path $PSScriptRoot '..\..\..\Tools'
$rd = Join-Path ($env:RENDERDOC_DIR ?? (Join-Path $tools 'RenderDoc\RenderDoc_1.46_64')) 'renderdoccmd.exe'
$python = $env:REBIRTHTEX_PYTHON ?? (Join-Path $tools 'Python\analysis\Scripts\python.exe')
$captures = Join-Path $PSScriptRoot '..\work\captures'
New-Item -ItemType Directory -Force $captures | Out-Null
$captures = Resolve-Path $captures
$hooks = 'dxgi.dll', 'dsound.dll', 'd3d12.dll'
$appid = Join-Path $win64 'steam_appid.txt'
$patchLog = Join-Path $captures 'psopatch.log'

function Undo-Session {
  foreach ($h in $hooks) {
    $off = Join-Path $win64 "$h.rdc-off"
    if (Test-Path $off) {
      if (Test-Path (Join-Path $win64 $h)) { throw "both $h and $h.rdc-off exist in $win64" }
      Rename-Item $off $h
      Write-Host "$h restored."
    }
  }
  if (Test-Path $appid) { Remove-Item $appid; Write-Host 'steam_appid.txt removed.' }
}

if ($Restore) { Undo-Session; return }

if (Get-Process -Name 'ff7rebirth_', 'ff7rebirth' -ErrorAction SilentlyContinue) { throw 'The game is already running.' }
foreach ($h in $hooks) {
  $path = Join-Path $win64 $h
  if (Test-Path $path) { Rename-Item $path "$h.rdc-off"; Write-Host "$h off for this session." }
}
Set-Content -Path $appid -Value '2909400' -NoNewline -Encoding ascii
try {
  $patcher = Start-Process -FilePath $python -ArgumentList '-I', (Join-Path $PSScriptRoot 'psopatch.py'), "`"$patchLog`"" `
    -NoNewWindow -PassThru
  Write-Host "Launching under RenderDoc; captures go to $captures"
  & $rd capture -w --opt-delay-for-debugger 8 -d $win64 -c (Join-Path $captures 'ff7r') `
    (Join-Path $win64 'ff7rebirth_.exe') End
  Write-Host "renderdoccmd exited with $LASTEXITCODE"
  if (-not $patcher.HasExited) { $patcher.Kill() }
  Write-Host "psopatch exit code: $($patcher.ExitCode)"
} finally {
  Undo-Session
}
