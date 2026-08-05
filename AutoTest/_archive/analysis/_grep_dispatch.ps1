$path = 'E:\Work\War3\war3_d3d9.log'
$matches = Select-String -Path $path -Pattern 'DispatchToShape|TerrainShadow|Shadow.*install' -SimpleMatch:$false
Write-Host "=== Total: $($matches.Count) ==="
$matches | Select-Object -First 20 | ForEach-Object { Write-Host $_.Line }
