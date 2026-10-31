# Decodes 16-bit PNG screenshots (ReShade saves HDR ones as 16-bit RGB, PQ / BT.2020) to raw little-endian RGB48 with
# Windows' own decoder (WIC), without colour management, for tools/shot_bands.py. Pillow only reads the high byte of
# each 16-bit sample.
#   decode_png16.ps1 <out dir> <png>...   writes <out dir>\<png name>.rgb48 and <png name>.rgb48.txt ("width height")
# Paths are taken literally (no wildcards); for a folder: decode_png16.ps1 out (Get-ChildItem *.png).FullName
param([Parameter(Mandatory)][string]$OutDir,
      [Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Pngs)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationCore

# Absolute, since .NET (below) and PowerShell would resolve a relative path against different folders.
$OutDir = (New-Item -ItemType Directory -Force $OutDir).FullName
$options = [Windows.Media.Imaging.BitmapCreateOptions]::PreservePixelFormat -bor
           [Windows.Media.Imaging.BitmapCreateOptions]::IgnoreColorProfile
foreach ($png in $Pngs) {
  $path = (Resolve-Path -LiteralPath $png).ProviderPath
  $stream = [IO.File]::OpenRead($path)
  try {
    $decoder = [Windows.Media.Imaging.PngBitmapDecoder]::new($stream, $options,
                                                              [Windows.Media.Imaging.BitmapCacheOption]::OnLoad)
    $frame = $decoder.Frames[0]
    if ($frame.Format -ne [Windows.Media.PixelFormats]::Rgb48) { throw "$path is $($frame.Format), not 16-bit RGB." }
    $stride = $frame.PixelWidth * 6
    $pixels = [byte[]]::new($stride * $frame.PixelHeight)
    $frame.CopyPixels($pixels, $stride, 0)
    $out = Join-Path $OutDir ([IO.Path]::GetFileNameWithoutExtension($path) + '.rgb48')
    [IO.File]::WriteAllBytes($out, $pixels)
    Set-Content -LiteralPath "$out.txt" -Value "$($frame.PixelWidth) $($frame.PixelHeight)"
    '{0}: {1}x{2} -> {3}' -f (Split-Path $path -Leaf), $frame.PixelWidth, $frame.PixelHeight, $out
  } finally {
    $stream.Dispose()
  }
}
