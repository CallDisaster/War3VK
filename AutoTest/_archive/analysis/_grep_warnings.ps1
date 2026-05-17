$lines = Get-Content build_log.txt
$matches = $lines | Where-Object { $_ -match 'warning:' -and $_ -notmatch 'war3_game_struct' }
Write-Host "=== Total: $($matches.Count) warnings (excluding game_struct OPCode reorder) ==="
$matches | Select-Object -First 30 | ForEach-Object { Write-Host $_ }
