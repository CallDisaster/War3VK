param(
  [string]$BuildDll = "..\build32\src\d3d9\d3d9.dll",
  [string]$OutputDir = ".\bin",
  [string]$LoaderOutputDir = ".\bin"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Resolve-Path (Join-Path $scriptDir $BuildDll)
$out = Join-Path $scriptDir $OutputDir
$loaderOut = Join-Path $scriptDir $LoaderOutputDir

New-Item -ItemType Directory -Force -Path $out | Out-Null
New-Item -ItemType Directory -Force -Path $loaderOut | Out-Null

$dllOut = Join-Path $out "WarVK.dll"

# `warvk.blp` is intentionally a DLL byte-for-byte carrier for the map-contained
# AI loader. It is not an image asset and must not be used as the WarVK
# lightning texture.
$carrierOut = Join-Path $out "warvk.blp"

Copy-Item -LiteralPath $src -Destination $dllOut -Force
Copy-Item -LiteralPath (Join-Path $scriptDir "loader\warvk_loader.lua") -Destination (Join-Path $loaderOut "warvk_loader.lua") -Force

$aiSource = Join-Path $scriptDir "bin\warvk.ai"
if (!(Test-Path -LiteralPath $aiSource)) {
  $aiSource = Join-Path $scriptDir "loader\warvk.ai"
}
$aiDestination = Join-Path $loaderOut "warvk.ai"
if ((Resolve-Path -LiteralPath $aiSource).Path -ne (Resolve-Path -LiteralPath $aiDestination -ErrorAction SilentlyContinue).Path) {
  Copy-Item -LiteralPath $aiSource -Destination $aiDestination -Force
}

Copy-Item -LiteralPath $src -Destination $carrierOut -Force

Write-Host "[WarVK] Packaged:"
Write-Host "  $dllOut"
Write-Host "  $carrierOut (map-contained DLL carrier, not a texture)"
Write-Host "  $(Join-Path $loaderOut "warvk_loader.lua")"
Write-Host "  $(Join-Path $loaderOut "warvk.ai")"
