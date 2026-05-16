$path = 'E:\Work\War3\war3_d3d9.log'
$pattern = 'PATH BLOCKER|ProjectorFromObject|BlockedFourCC|Projector |ShadowProjector|fourcc'
$matches = Get-Content $path | Select-String -Pattern $pattern
Write-Host "=== Total matched lines: $($matches.Count) ==="
$matches | Select-Object -Last 50 | ForEach-Object { Write-Host $_.Line }
