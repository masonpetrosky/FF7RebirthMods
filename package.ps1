# Builds the add-ons and assembles one mod's release zip in dist\, e.g. .\package.ps1 NpcMotion.
# The version defaults to the one in <Mod>\CMakeLists.txt.
param(
  [Parameter(Mandatory)][ValidateSet('Anisotropic', 'FrameLimit', 'NpcMotion', 'ShaderInjectorSync', 'VegetationAndHairFix')]
  [string]$Mod,
  [string]$Version = '')
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$name = "FF7Rebirth$Mod"
if (-not $Version) {
  $Version = (Select-String -Path "$Mod\CMakeLists.txt" -Pattern "project\($name VERSION ([0-9.]+)").Matches[0].Groups[1].Value
}
# Add-ons that read shader reflection or compile shaders at run time ship dxcompiler.dll.
$shipsDxc = $Mod -in 'NpcMotion', 'VegetationAndHairFix'

& "$PSScriptRoot\build.ps1" -Target $name | Out-Null

if ($shipsDxc) {
  # Pinned DirectX Shader Compiler release; only dxcompiler.dll is shipped.
  $dxcUrl = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip'
  $dxcSha256 = 'AD31B1FC8443175D204F77A611FDB3EF2EC42759BDC2F1167368DE24A4A7E7F1'
  $dxcZip = Join-Path $PSScriptRoot 'build\dxc.zip'
  if (-not (Test-Path $dxcZip) -or (Get-FileHash $dxcZip -Algorithm SHA256).Hash -ne $dxcSha256) {
    Invoke-WebRequest $dxcUrl -OutFile $dxcZip
    if ((Get-FileHash $dxcZip -Algorithm SHA256).Hash -ne $dxcSha256) { throw 'DXC download does not match the pinned SHA-256.' }
  }
  $dxcDir = Join-Path $PSScriptRoot 'build\dxc'
  if (Test-Path $dxcDir) { Remove-Item $dxcDir -Recurse -Force }
  Expand-Archive $dxcZip $dxcDir
}

$stage = Join-Path $PSScriptRoot "build\stage\$Mod"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$licenses = "$stage\$name\licenses"
New-Item -ItemType Directory -Force $licenses | Out-Null
Copy-Item "build\$Mod\$name.addon64" $stage
Copy-Item "$Mod\config\$name.ini" $stage
if ($shipsDxc) {
  Copy-Item "$dxcDir\bin\x64\dxcompiler.dll" "$stage\$name\"
  Copy-Item "$dxcDir\LICENSE-LLVM.txt" "$licenses\dxcompiler-LICENSE-LLVM.txt"
  Copy-Item "$dxcDir\LICENCE-MIT.txt" "$licenses\dxcompiler-LICENSE-MIT.txt"
}
Copy-Item LICENSE "$licenses\LICENSE.txt"
Copy-Item "$Mod\THIRD_PARTY_NOTICES.md" $licenses
Copy-Item third_party\reshade\LICENSE.md "$licenses\reshade-LICENSE.md"
Copy-Item third_party\imgui\LICENSE.txt "$licenses\imgui-LICENSE.txt"
# Only this add-on compiles in DxilHash from the DirectX Shader Compiler sources.
if ($Mod -eq 'VegetationAndHairFix') { Copy-Item third_party\dxc\LICENSE.TXT "$licenses\dxc-LICENSE.txt" }

New-Item -ItemType Directory -Force dist | Out-Null
$zip = Join-Path $PSScriptRoot "dist\$name-v$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Get-Item $zip
