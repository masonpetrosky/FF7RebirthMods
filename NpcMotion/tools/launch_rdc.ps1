# Starts FINAL FANTASY VII REBIRTH under RenderDoc 1.46 for frame captures and waits for the game to exit.
# For the session only: ReShade (dxgi.dll) is switched off and a steam_appid.txt lets the game start without Steam
# relaunching it; both are undone when the game exits (or with -Restore after a crash of this script).
# RenderDoc refuses D3D12 pipeline libraries and the game asserts on that (WindowsD3D12PipelineState.cpp:746); the
# game turns them on in its device profile and ignores user ini overrides, so psopatch.py switches them off in memory
# while RenderDoc holds the new process (--opt-delay-for-debugger); see psopatch.py.
# Captures land in captures\ff7r_frame<N>.rdc; F12 or PrtScn captures in game, tools\rdc_trigger.py remotely.
param([switch]$Restore)
$ErrorActionPreference = 'Stop'

$game = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH'
$win64 = Join-Path $game 'End\Binaries\Win64'
# RenderDoc 1.46 and a Python with the standard library; by default the shared copies in the Tools folder beside the
# Projects folder that holds this repository (override with RENDERDOC_DIR and NPCMOTION_PYTHON).
$tools = Join-Path $PSScriptRoot '..\..\..\Tools'
$rd = Join-Path ($env:RENDERDOC_DIR ?? (Join-Path $tools 'RenderDoc\RenderDoc_1.46_64')) 'renderdoccmd.exe'
$python = $env:NPCMOTION_PYTHON ?? (Join-Path $tools 'Python\analysis\Scripts\python.exe')
$captures = Join-Path $PSScriptRoot '..\captures' | Resolve-Path
$reshade = Join-Path $win64 'dxgi.dll'
$reshadeOff = Join-Path $win64 'dxgi.dll.rdc-off'
$appid = Join-Path $win64 'steam_appid.txt'
$patchLog = Join-Path $captures 'psopatch.log'

function Undo-Session {
  if (Test-Path $reshadeOff) {
    if (Test-Path $reshade) { throw "both dxgi.dll and dxgi.dll.rdc-off exist in $win64" }
    Rename-Item $reshadeOff 'dxgi.dll'
    Write-Host 'ReShade restored (dxgi.dll).'
  }
  if (Test-Path $appid) { Remove-Item $appid; Write-Host 'steam_appid.txt removed.' }
}

if ($Restore) { Undo-Session; return }

if (Get-Process -Name 'ff7rebirth_', 'ff7rebirth' -ErrorAction SilentlyContinue) { throw 'The game is already running.' }
if (Test-Path $reshade) { Rename-Item $reshade 'dxgi.dll.rdc-off' }
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
