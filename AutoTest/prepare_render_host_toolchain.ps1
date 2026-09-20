# Explicit portable laboratory dependency. Does not edit PATH, install a service,
# run an installer or modify the shipping DXVK compiler/build directory.
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$taskRoot='E:\Dev\Toolchains\warvk-rh0-llvm-mingw-20260616'
$taskZip=Join-Path $taskRoot 'llvm-mingw-20260616-ucrt-x86_64.zip'
$taskExtract=Join-Path $taskRoot 'portable'
$taskUrl='https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/llvm-mingw-20260616-ucrt-x86_64.zip'
$taskHash='B9B68A4D276E16FA25802AABA458E4638F64B3884C290AACCDC2D87083B6CA35'
$taskSize=187504083L
if(Test-Path -LiteralPath $taskRoot){throw 'Toolchain destination already exists; no overwrite/reuse.'}
if((Get-PSDrive E).Free -lt 4GB){throw 'Less than 4 GiB free for bounded extraction.'}
New-Item -ItemType Directory -Path $taskRoot | Out-Null
Add-Type -AssemblyName System.Net.Http
$taskHttp=[System.Net.Http.HttpClient]::new()
$taskHttp.Timeout=[TimeSpan]::FromMinutes(15)
$taskFile=$null;$taskResponse=$null;$taskStream=$null
try {
  $taskFile=[System.IO.File]::Open($taskZip,[System.IO.FileMode]::CreateNew,[System.IO.FileAccess]::Write,[System.IO.FileShare]::None)
  $taskResponse=$taskHttp.GetAsync($taskUrl,[System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
  $taskResponse.EnsureSuccessStatusCode() | Out-Null
  $taskStream=$taskResponse.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
  $taskBuffer=New-Object byte[] 1048576
  $taskTotal=0L
  while(($taskRead=$taskStream.Read($taskBuffer,0,$taskBuffer.Length)) -gt 0){
    $taskTotal+=$taskRead
    if($taskTotal -gt $taskSize){throw 'Download exceeds pinned release size.'}
    $taskFile.Write($taskBuffer,0,$taskRead)
  }
  $taskFile.Flush()
} finally {
  if($taskStream){$taskStream.Dispose()};if($taskResponse){$taskResponse.Dispose()}
  if($taskFile){$taskFile.Dispose()};$taskHttp.Dispose()
}
if((Get-Item -LiteralPath $taskZip).Length -ne $taskSize){throw 'Pinned size mismatch; preserve failed download.'}
if((Get-FileHash -LiteralPath $taskZip -Algorithm SHA256).Hash -ne $taskHash){throw 'Pinned SHA mismatch; do not execute/extract.'}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$taskArchive=[System.IO.Compression.ZipFile]::OpenRead($taskZip)
try {
  $taskBase=[System.IO.Path]::GetFullPath($taskExtract)+'\'
  $taskUnpacked=0L
  if($taskArchive.Entries.Count -gt 50000){throw 'Too many archive entries.'}
  foreach($taskEntry in $taskArchive.Entries){
    $taskFinal=[System.IO.Path]::GetFullPath((Join-Path $taskExtract $taskEntry.FullName))
    if(!$taskFinal.StartsWith($taskBase,[StringComparison]::OrdinalIgnoreCase)){throw 'Archive entry escapes destination.'}
    if($taskEntry.FullName.Contains(':')){throw 'Archive entry contains alternate stream.'}
    $taskUnpacked+=$taskEntry.Length
    if($taskUnpacked -gt 3GB){throw 'Uncompressed archive exceeds budget.'}
  }
} finally {$taskArchive.Dispose()}
if(Test-Path -LiteralPath $taskExtract){throw 'Extraction destination appeared; stopping.'}
[System.IO.Compression.ZipFile]::ExtractToDirectory($taskZip,$taskExtract)
[pscustomobject]@{root=$taskRoot;url=$taskUrl;bytes=$taskSize;sha256=$taskHash;unpackedBytes=$taskUnpacked;systemPathModified=$false} | ConvertTo-Json
