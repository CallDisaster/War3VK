param(
    [Parameter(Mandatory=$true)][string]$InputMap,
    [Parameter(Mandatory=$true)][string]$ExtractedScript,
    [string]$OutputMap,
    [string]$ReplacementScript,
    [string]$ImportModel,
    [string]$ImportName
)
$ErrorActionPreference = 'Stop'
if ([IntPtr]::Size -ne 4) { throw 'Run using SysWOW64 Windows PowerShell (32-bit)' }
$inputPath = (Resolve-Path -LiteralPath $InputMap).Path
$extractPath = [IO.Path]::GetFullPath($ExtractedScript)
if (Test-Path -LiteralPath $extractPath) { throw 'Extract target already exists' }
$stormPath = 'E:\Work\War3\YDWE1.32.13 - MemoryHack\bin\StormLib.dll'
if (-not (Test-Path -LiteralPath $stormPath)) { throw 'Missing pinned local StormLib' }
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class V122Storm {
    const string Dll = @"E:\Work\War3\YDWE1.32.13 - MemoryHack\bin\StormLib.dll";
    [DllImport(Dll, CharSet=CharSet.Unicode, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.I1)]
    public static extern bool SFileOpenArchive(string path, uint priority, uint flags, out IntPtr archive);
    [DllImport(Dll, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.I1)]
    public static extern bool SFileOpenFileEx(IntPtr archive, [MarshalAs(UnmanagedType.LPStr)] string name, uint scope, out IntPtr file);
    [DllImport(Dll, SetLastError=true)]
    public static extern uint SFileGetFileSize(IntPtr file, out uint high);
    [DllImport(Dll, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.I1)]
    public static extern bool SFileReadFile(IntPtr file, byte[] data, uint size, out uint read, IntPtr overlapped);
    [DllImport(Dll)][return: MarshalAs(UnmanagedType.I1)] public static extern bool SFileCloseFile(IntPtr file);
    [DllImport(Dll)][return: MarshalAs(UnmanagedType.I1)] public static extern bool SFileCloseArchive(IntPtr archive);
    [DllImport(Dll, CharSet=CharSet.Unicode, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.I1)]
    public static extern bool SFileAddFileEx(IntPtr archive, string source, [MarshalAs(UnmanagedType.LPStr)] string name, uint flags, uint compression, uint nextCompression);
}
'@
$archive = [IntPtr]::Zero
$file = [IntPtr]::Zero
$entry = 'war3map.j'
try {
    if (-not [V122Storm]::SFileOpenArchive($inputPath, 0, 0x100, [ref]$archive)) { throw 'Read-only MPQ open failed' }
    $sentinel=[IntPtr]::Zero
    if ([V122Storm]::SFileOpenFileEx($archive,'__WARVK_MUST_NOT_EXIST_77e08620__',0,[ref]$sentinel) -or $sentinel -ne [IntPtr]::Zero) { throw 'MPQ bool ABI/existence predicate failed' }
    if (-not [V122Storm]::SFileOpenFileEx($archive, $entry, 0, [ref]$file)) {
        $entry = 'scripts\war3map.j'
        if (-not [V122Storm]::SFileOpenFileEx($archive, $entry, 0, [ref]$file)) { throw 'No canonical map script' }
    }
    [uint32]$high = 0
    $size = [V122Storm]::SFileGetFileSize($file, [ref]$high)
    if ($high -ne 0 -or $size -gt 16777216) { throw 'Invalid script size' }
    $bytes = New-Object byte[] $size
    [uint32]$read = 0
    if (-not [V122Storm]::SFileReadFile($file, $bytes, $size, [ref]$read, [IntPtr]::Zero) -or $read -ne $size) { throw 'Incomplete MPQ read' }
    $stream = [IO.File]::Open($extractPath, [IO.FileMode]::CreateNew)
    try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
} finally {
    if ($file -ne [IntPtr]::Zero) { [void][V122Storm]::SFileCloseFile($file) }
    if ($archive -ne [IntPtr]::Zero) { [void][V122Storm]::SFileCloseArchive($archive) }
}
if ($OutputMap -or $ReplacementScript) {
    if (-not $OutputMap -or -not $ReplacementScript) { throw 'Both replacement paths required' }
    $outputPath = [IO.Path]::GetFullPath($OutputMap)
    $allowed = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'artifacts\v122_visual_api_20260914')) + '\'
    if (-not $outputPath.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Only new isolated artifact map may be edited' }
    if (Test-Path -LiteralPath $outputPath) { throw 'Output map already exists' }
    $replacementPath = (Resolve-Path -LiteralPath $ReplacementScript).Path
    [IO.File]::Copy($inputPath, $outputPath, $false)
    $archive = [IntPtr]::Zero
    try {
        if (-not [V122Storm]::SFileOpenArchive($outputPath, 0, 0, [ref]$archive)) { throw 'Copied MPQ open failed' }
        if (-not [V122Storm]::SFileAddFileEx($archive, $replacementPath, $entry, [uint32]2147484160, 2, 2)) { throw 'Copied map script replacement failed' }
        if ($ImportModel -or $ImportName) {
            if (-not $ImportModel -or $ImportName -cne 'war3mapImported\WarVKReview\TorchHumanUser32059406.mdx') { throw 'Only exact new fixture model path permitted' }
            $modelPath=(Resolve-Path -LiteralPath $ImportModel).Path
            $modelBytes=[IO.File]::ReadAllBytes($modelPath)
            if ($modelBytes.Length -gt 4194304) { throw 'Model bound' }
            $check=[IntPtr]::Zero
            if ([V122Storm]::SFileOpenFileEx($archive,$ImportName,0,[ref]$check)) {
                [void][V122Storm]::SFileCloseFile($check); throw 'Import target already exists'
            }
            if (-not [V122Storm]::SFileAddFileEx($archive,$modelPath,$ImportName,512,2,2)) { throw 'New model import failed' }
            try {
                if (-not [V122Storm]::SFileOpenFileEx($archive,$ImportName,0,[ref]$check)) { throw 'Model readback open failed' }
                [uint32]$modelHigh=0; [uint32]$modelRead=0
                $modelSize=[V122Storm]::SFileGetFileSize($check,[ref]$modelHigh)
                if ($modelHigh -ne 0 -or $modelSize -ne $modelBytes.Length) { throw 'Model readback size' }
                $readback=New-Object byte[] $modelSize
                if (-not [V122Storm]::SFileReadFile($check,$readback,$modelSize,[ref]$modelRead,[IntPtr]::Zero) -or $modelRead -ne $modelSize) { throw 'Model readback bytes' }
                if ([Convert]::ToBase64String($readback) -cne [Convert]::ToBase64String($modelBytes)) { throw 'Model readback mismatch' }
            } finally { if ($check -ne [IntPtr]::Zero) { [void][V122Storm]::SFileCloseFile($check) } }
        }
    } finally {
        if ($archive -ne [IntPtr]::Zero) { [void][V122Storm]::SFileCloseArchive($archive) }
    }
}
Write-Output "entry=$entry size=$size extracted=$extractPath"
