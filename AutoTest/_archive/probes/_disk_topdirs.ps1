$root = 'C:\'
Write-Host "Top 15 dirs by size in $root"
Get-ChildItem $root -Directory -Force -ErrorAction SilentlyContinue | ForEach-Object {
    $d = $_
    try {
        $size = (Get-ChildItem $d.FullName -Recurse -Force -ErrorAction SilentlyContinue -File | Measure-Object Length -Sum).Sum
    } catch {
        $size = 0
    }
    if ($null -eq $size) { $size = 0 }
    [pscustomobject]@{ Name = $d.Name; GB = [math]::Round(($size / 1GB), 2) }
} | Sort-Object GB -Descending | Select-Object -First 15
