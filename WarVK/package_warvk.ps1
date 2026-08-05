param(
  [string]$BuildDll = "..\build32\src\d3d9\d3d9.dll",
  [string]$OutputDir = ".\bin"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Resolve-Path (Join-Path $scriptDir $BuildDll)
$out = Join-Path $scriptDir $OutputDir

New-Item -ItemType Directory -Force -Path $out | Out-Null
$dllOut = Join-Path $out "d3d9.dll"
Copy-Item -LiteralPath $src -Destination $dllOut -Force

Write-Host "[WarVK] Packaged:"
Write-Host "  $dllOut"
Write-Host "  Install this proxy beside war3.exe before starting the game."
