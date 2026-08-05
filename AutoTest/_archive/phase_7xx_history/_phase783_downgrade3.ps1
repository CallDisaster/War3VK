$ErrorActionPreference = "Stop"
$path = "src\d3d9\war3\render\war3_shadow_object_registry.cpp"
$src = Get-Content $path -Raw
$orig = $src

$readOnlyPatterns = @(
    'find[A-Z][\w]*\(',
    '::snapshot\(',
    '::recordCount\(',
    'Count\(\) const'
)

$lines = $src -split "`r?`n"
$count = 0
for ($i = 0; $i -lt $lines.Length - 1; $i++) {
    $sig = $lines[$i]
    $lockLine = $lines[$i + 1]
    if ($lockLine -notmatch '^  std::unique_lock<std::shared_mutex> lock\(m_mutex\);$') { continue }
    if ($sig -notmatch ' const \{$') { continue }
    $isReadOnly = $false
    foreach ($p in $readOnlyPatterns) { if ($sig -match $p) { $isReadOnly = $true; break } }
    if (-not $isReadOnly) { continue }
    $lines[$i + 1] = '  std::shared_lock<std::shared_mutex> lock(m_mutex);'
    $count++
}
$src = $lines -join "`r`n"
if ($src -ne $orig) {
    Set-Content -Path $path -Value $src -NoNewline -Encoding UTF8
    Write-Host "Downgraded $count locks to shared in $path"
} else {
    Write-Host "No change in $path"
}
