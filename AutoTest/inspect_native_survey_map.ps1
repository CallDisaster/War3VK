param([Parameter(Mandatory=$true)][string]$InputMap,
      [Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
if ([IntPtr]::Size -ne 4) { throw 'Use the explicit 32-bit PowerShell host' }
$mapPath = (Resolve-Path -LiteralPath $InputMap).Path
$artifactRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'artifacts'))
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if ([IO.Path]::GetDirectoryName($outputPath) -ne $artifactRoot -or (Test-Path -LiteralPath $outputPath)) {
  throw 'A fresh direct AutoTest/artifacts child is required'
}
for ($item=[IO.DirectoryInfo]$artifactRoot; $null -ne $item; $item=$item.Parent) {
  if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Reparse output refused' }
}
[void][IO.Directory]::CreateDirectory($outputPath)
$lease=[IO.File]::Open($mapPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
$before=(Get-FileHash -LiteralPath $mapPath -Algorithm SHA256).Hash
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SurveyMapStorm {
 const string Dll=@"E:\Work\War3\YDWE1.32.13 - MemoryHack\bin\StormLib.dll";
 [DllImport(Dll,CharSet=CharSet.Unicode,SetLastError=true)] [return:MarshalAs(UnmanagedType.I1)]
 public static extern bool SFileOpenArchive(string p,uint pr,uint fl,out IntPtr a);
 [DllImport(Dll,SetLastError=true)] [return:MarshalAs(UnmanagedType.I1)]
 public static extern bool SFileOpenFileEx(IntPtr a,[MarshalAs(UnmanagedType.LPStr)] string n,uint s,out IntPtr f);
 [DllImport(Dll)] public static extern uint SFileGetFileSize(IntPtr f,out uint high);
 [DllImport(Dll)] [return:MarshalAs(UnmanagedType.I1)]
 public static extern bool SFileReadFile(IntPtr f,byte[] b,uint sz,out uint read,IntPtr o);
 [DllImport(Dll)] [return:MarshalAs(UnmanagedType.I1)] public static extern bool SFileCloseFile(IntPtr f);
 [DllImport(Dll)] [return:MarshalAs(UnmanagedType.I1)] public static extern bool SFileCloseArchive(IntPtr a);
}
'@
$archive=[IntPtr]::Zero; $file=[IntPtr]::Zero
try {
  if (-not [SurveyMapStorm]::SFileOpenArchive($mapPath,0,0x100,[ref]$archive)) { throw 'Read-only MPQ open failed' }
  $missing=[IntPtr]::Zero
  if ([SurveyMapStorm]::SFileOpenFileEx($archive,'__SURVEY_MAP_MUST_NOT_EXIST__',0,[ref]$missing) -or $missing -ne [IntPtr]::Zero) {
    throw 'StormLib bool ABI mismatch'
  }
  $entry='war3map.j'
  if (-not [SurveyMapStorm]::SFileOpenFileEx($archive,$entry,0,[ref]$file)) {
    $entry='scripts\war3map.j'
    if (-not [SurveyMapStorm]::SFileOpenFileEx($archive,$entry,0,[ref]$file)) { throw 'Script not found' }
  }
  [uint32]$high=0; [uint32]$read=0
  $size=[SurveyMapStorm]::SFileGetFileSize($file,[ref]$high)
  if ($high -ne 0 -or $size -gt 16777216) { throw 'Script exceeds read budget' }
  $bytes=New-Object byte[] $size
  if (-not [SurveyMapStorm]::SFileReadFile($file,$bytes,$size,[ref]$read,[IntPtr]::Zero) -or $read -ne $size) {
    throw 'Incomplete script read'
  }
  $target=Join-Path $outputPath 'war3map.j'
  $stream=[IO.File]::Open($target,[IO.FileMode]::CreateNew)
  try { $stream.Write($bytes,0,$bytes.Length) } finally { $stream.Dispose() }
  $after=(Get-FileHash -LiteralPath $mapPath -Algorithm SHA256).Hash
  if ($before -ne $after) { throw 'Source map changed' }
  $receipt=[ordered]@{source=$mapPath;sha256=$before;unchanged=$true;readOnly=$true;entry=$entry;
    output=$target;bytes=$size;scriptSha256=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash}
  $json=$receipt | ConvertTo-Json
  $record=[IO.File]::Open((Join-Path $outputPath 'receipt.json'),[IO.FileMode]::CreateNew)
  try { $data=[Text.Encoding]::UTF8.GetBytes($json); $record.Write($data,0,$data.Length) } finally { $record.Dispose() }
  $json
} finally {
  if ($file -ne [IntPtr]::Zero) { [void][SurveyMapStorm]::SFileCloseFile($file) }
  if ($archive -ne [IntPtr]::Zero) { [void][SurveyMapStorm]::SFileCloseArchive($archive) }
  $lease.Dispose()
}
