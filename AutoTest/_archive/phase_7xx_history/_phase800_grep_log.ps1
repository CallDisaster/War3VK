$path = 'E:\Work\War3\war3_d3d9.log'
$pattern = 'WMR_FLAGS|WMR_DUMP|BUILDING SHADOW|PATH BLOCKER|ProjectorFromObject|BlockedFourCC'
$matches = Get-Content $path | Select-String -Pattern $pattern
Write-Host "=== Total matched lines: $($matches.Count) ==="
$matches | Select-Object -Last 30 | ForEach-Object { Write-Host $_.Line }
