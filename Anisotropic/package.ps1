# Builds the add-on and assembles the release zip in dist\.
param([string]$Version = '')
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not $Version) {
  $Version = (Select-String -Path CMakeLists.txt -Pattern 'project\(FF7RebirthAnisotropic VERSION ([0-9.]+)').Matches[0].Groups[1].Value
}

& "$PSScriptRoot\build.ps1" | Out-Null

$stage = Join-Path $PSScriptRoot "build\stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\FF7RebirthAnisotropic\licenses" | Out-Null
Copy-Item build\FF7RebirthAnisotropic.addon64 $stage
Copy-Item config\FF7RebirthAnisotropic.ini $stage
Copy-Item LICENSE "$stage\FF7RebirthAnisotropic\licenses\LICENSE.txt"
Copy-Item THIRD_PARTY_NOTICES.md "$stage\FF7RebirthAnisotropic\licenses\"
Copy-Item third_party\reshade\LICENSE.md "$stage\FF7RebirthAnisotropic\licenses\reshade-LICENSE.md"
Copy-Item third_party\imgui\LICENSE.txt "$stage\FF7RebirthAnisotropic\licenses\imgui-LICENSE.txt"

New-Item -ItemType Directory -Force dist | Out-Null
$zip = Join-Path $PSScriptRoot "dist\FF7RebirthAnisotropic-v$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Get-Item $zip
