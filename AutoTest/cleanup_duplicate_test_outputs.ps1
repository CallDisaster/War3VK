param([switch]$Apply)
$ErrorActionPreference='Stop'
$base=[IO.Path]::GetFullPath('E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics')
$allowed=@('.log','.tga','.png','.bmp')
$all=@()
$roots=@(Get-ChildItem -LiteralPath $base -Directory | Where-Object {$_.Name -like 'dxvk*'})
foreach($tree in $roots){
  $root=Join-Path $tree.FullName 'AutoTest\artifacts'
  if(!(Test-Path -LiteralPath $root -PathType Container)){continue}
  if((Get-Item -LiteralPath $root).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Reparse root: $root"}
  $all+=@(Get-ChildItem -LiteralPath $root -File -Recurse -Force | Where-Object {
    $_.Length -ge 1MB -and $_.Extension.ToLowerInvariant() -in $allowed -and
    !($_.Attributes -band [IO.FileAttributes]::ReparsePoint)
  })
}
$plan=[Collections.Generic.List[object]]::new()
$hashed=0
foreach($group in ($all|Group-Object Length|Where-Object {$_.Count -gt 1})){
  $keepers=@{}
  # Protected recent validation and investigation copies are never removed.
  $ordered=$group.Group|Sort-Object @{Expression={if($_.FullName -match 'shortcut_final_r15|shortcut_hud_r14|issue8_localized'){0}else{1}}},FullName
  foreach($file in $ordered){
    $hash=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    $hashed++
    if($hashed%150 -eq 0){Write-Output "Hashed $hashed bounded test outputs"}
    if(!$keepers.ContainsKey($hash)){$keepers[$hash]=$file.FullName;continue}
    if($file.FullName -match 'shortcut_final_r15|shortcut_hud_r14|issue8_localized'){continue}
    $plan.Add([pscustomobject]@{path=$file.FullName;bytes=$file.Length;sha256=$hash;retained=$keepers[$hash]})
  }
}
$bytes=($plan|Measure-Object bytes -Sum).Sum
Write-Output "Exact duplicates: $($plan.Count); reclaimable GiB: $([math]::Round($bytes/1GB,3))"
if(!$Apply){return}
$receiptRoot=Join-Path $PSScriptRoot 'artifacts\cleanup_20260915'
if(Test-Path -LiteralPath $receiptRoot){throw 'Unique cleanup receipt directory already exists'}
[void](New-Item -ItemType Directory -Path $receiptRoot)
[IO.File]::WriteAllText((Join-Path $receiptRoot 'plan.json'),(ConvertTo-Json -InputObject @($plan.ToArray()) -Depth 4))
$tracked=@{}
foreach($tree in $roots){
  $paths=@(& git -C $tree.FullName ls-files --cached)
  if($LASTEXITCODE -ne 0){continue}
  foreach($rel in $paths){$tracked[[IO.Path]::GetFullPath((Join-Path $tree.FullName $rel))]=$true}
}
function CheckPath([string]$path){
  $full=[IO.Path]::GetFullPath($path)
  if(!$full.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase) -or
     $full -notmatch '\\dxvk[^\\]*\\AutoTest\\artifacts\\' -or
     [IO.Path]::GetExtension($full).ToLowerInvariant() -notin $allowed -or $tracked.ContainsKey($full)){throw "Out of scope: $full"}
  $node=Get-Item -LiteralPath $full
  while($node -and $node.FullName -ne $base){
    if($node.Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Reparse path: $full"}
    $node=if($node.PSIsContainer){$node.Parent}else{$node.Directory}
  }
  return $full
}
$deleted=[Collections.Generic.List[object]]::new()
try{
  foreach($item in $plan){
    $target=CheckPath $item.path
    $kept=CheckPath $item.retained
    if($target -eq $kept){throw 'Self-retention'}
    if((Get-Item -LiteralPath $target).Length -ne $item.bytes -or
       (Get-Item -LiteralPath $kept).Length -ne $item.bytes -or
       (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $item.sha256 -or
       (Get-FileHash -LiteralPath $kept -Algorithm SHA256).Hash -ne $item.sha256){throw 'Identity changed; stopped'}
    Remove-Item -LiteralPath $target -Force
    $deleted.Add($item)
  }
}finally{
  [IO.File]::WriteAllText((Join-Path $receiptRoot 'deleted.json'),(ConvertTo-Json -InputObject @($deleted.ToArray()) -Depth 4))
  Write-Output "Removed $($deleted.Count) byte-identical copies; retained originals listed in $receiptRoot"
  Get-PSDrive E | Select-Object Name,Free
}
