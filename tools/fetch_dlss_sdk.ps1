# Downloads the parts of NVIDIA's DLSS SDK that NPC Motion builds against into third_party\dlss: the NGX headers for
# Ray Reconstruction (NpcMotion/src/ray_reconstruction.cpp), and the static library for the offline test
# (NpcMotion/tests/rr_test.cpp). The SDK stays under NVIDIA's license (third_party\dlss\LICENSE.txt) and is not
# committed to this repository; CI runs this script before building.
param(
  [string]$Version = 'v310.9.1',
  [string]$Destination = (Join-Path $PSScriptRoot '..\third_party\dlss')
)
$ErrorActionPreference = 'Stop'

$dst = [System.IO.Path]::GetFullPath($Destination)
$base = "https://raw.githubusercontent.com/NVIDIA/DLSS/$Version"
# The include closure of nvsdk_ngx_helpers_dlssd_d3d.h at the pinned SDK version. No GitHub CLI or login is needed.
$headers = @(
  'nvsdk_ngx.h', 'nvsdk_ngx_defs.h', 'nvsdk_ngx_params.h', 'nvsdk_ngx_helpers.h', 'nvsdk_ngx_helpers_d3d.h',
  'nvsdk_ngx_helpers_cuda.h', 'nvsdk_ngx_defs_dlssd.h', 'nvsdk_ngx_params_dlssd.h', 'nvsdk_ngx_helpers_dlssd_d3d.h'
)
$files = @{
  'LICENSE.txt' = 'LICENSE.txt'
  'lib/Windows_x86_64/x64/nvsdk_ngx_s.lib' = 'lib/nvsdk_ngx_s.lib'
}
foreach ($h in $headers) { $files["include/$h"] = "include/$h" }

foreach ($src in $files.Keys) {
  $out = Join-Path $dst $files[$src]
  New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
  Invoke-WebRequest -Uri "$base/$src" -OutFile $out -UseBasicParsing
}
Set-Content -Path (Join-Path $dst 'VERSION') -Value $Version
Get-ChildItem $dst -Recurse -File | Select-Object @{n = 'File'; e = { $_.FullName.Substring((Resolve-Path $dst).Path.Length + 1) } }, Length
