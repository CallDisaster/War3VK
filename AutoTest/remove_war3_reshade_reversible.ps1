$ErrorActionPreference = 'Stop'
$gameRoot = [IO.Path]::GetFullPath('E:\Work\War3')
$backupRoot = 'E:\Work\WarVK-Backups\war3-reshade-20260914'
if (Test-Path -LiteralPath $backupRoot) { throw 'Backup exists; refusing reuse.' }
$processes = @(Get-CimInstance Win32_Process | Where-Object {
  $_.Name -match '^(war3|Warcraft III|YDWE|WorldEditor)(\.exe)?$'
})
if ($processes.Count) { throw 'Game/editor processes present.' }
function Identity([string] $path) {
  $item = Get-Item -LiteralPath $path
  if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point: $path" }
  [pscustomobject]@{path=$item.FullName; size=$item.Length; sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
}
$dllBefore = Identity "$gameRoot\d3d9.dll"
if ($dllBefore.sha256 -ne 'A5701AF3F3724683E559B4F142F7E45DD0EFCA8F6B3DF31C26842EBC10675A7A') { throw 'Live DLL changed.' }
$appList = Identity 'C:\ProgramData\ReShade\ReShadeApps.ini'
if ($appList.sha256 -ne '5E067D57F4E9407F00E391C66677136DE4F0F80C0CD713A746BF272A1F1D7608') { throw 'ReShade app list changed.' }
$names = @('ReShade.ini','ReShade.log','ReShadePreset.ini','reshade-shaders',
  'dlss5-bridge.addon64','renodx-dlss-0901BUILD.addon64','nvngx_dlssnr.dll')
$records = @()
foreach ($name in $names) {
  $path = [IO.Path]::GetFullPath((Join-Path $gameRoot $name))
  if (-not $path.StartsWith($gameRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Scope violation.' }
  $item = Get-Item -LiteralPath $path
  if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point.' }
  if ($item.PSIsContainer) {
    foreach ($child in (Get-ChildItem -LiteralPath $path -Force -Recurse)) {
      if ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Nested reparse point.' }
      if (-not $child.PSIsContainer) { $records += Identity $child.FullName }
    }
  } else { $records += Identity $path }
}
$null = New-Item -ItemType Directory -Path $backupRoot
[IO.File]::Copy($appList.path, (Join-Path $backupRoot 'ReShadeApps.ini.before'), $false)
# Move only the exact validated game-local items, using one shell and no overwrite.
foreach ($name in $names) {
  $destination = Join-Path $backupRoot $name
  if (Test-Path -LiteralPath $destination) { throw 'Destination collision.' }
  Move-Item -LiteralPath (Join-Path $gameRoot $name) -Destination $destination
}
foreach ($record in $records) {
  $relative = $record.path.Substring($gameRoot.Length + 1)
  $after = Identity (Join-Path $backupRoot $relative)
  if ($after.size -ne $record.size -or $after.sha256 -ne $record.sha256) { throw "Backup mismatch: $relative" }
}
$dllAfter = Identity "$gameRoot\d3d9.dll"
if ($dllAfter.sha256 -ne $dllBefore.sha256) { throw 'WarVK DLL changed.' }
$receipt = [ordered]@{ backup=$backupRoot; movedFiles=$records; appListBefore=$appList;
  warvkBefore=$dllBefore; warvkAfter=$dllAfter; globalReShadeBinariesUnchanged=$true;
  note='Local config removed. Global per-app list edited separately by apply_patch. No game launched.' }
$json = $receipt | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText((Join-Path $backupRoot 'receipt.json'), $json, [Text.UTF8Encoding]::new($false))
[pscustomobject]@{Backup=$backupRoot;MovedFiles=$records.Count;WarvkUnchanged=$true} | ConvertTo-Json
