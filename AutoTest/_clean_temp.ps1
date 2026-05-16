$temp = 'C:\Users\Administrator\AppData\Local\Temp'
Write-Host "Top 10 large dirs in $temp"
Get-ChildItem $temp -Directory -Force -ErrorAction SilentlyContinue | ForEach-Object {
    $d = $_
    $size = (Get-ChildItem $d.FullName -Recurse -Force -ErrorAction SilentlyContinue -File | Measure-Object Length -Sum).Sum
    [pscustomobject]@{ Name = $d.Name; MB = [math]::Round(($size / 1MB), 1) }
} | Sort-Object MB -Descending | Select-Object -First 10
Write-Host ""
Write-Host "Top 10 files in $temp"
Get-ChildItem $temp -File -Force -ErrorAction SilentlyContinue |
    Sort-Object Length -Descending |
    Select-Object -First 10 Name, @{N='MB'; E={ [math]::Round(($_.Length / 1MB), 1) }}
