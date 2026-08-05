# Phase 7.83 Step 2: Downgrade specific const-read methods to shared_lock.
$ErrorActionPreference = "Stop"
$path = "src\d3d9\war3\model\war3_model_registry.cpp"
$src = Get-Content $path -Raw
$orig = $src

# All read-only methods that should be shared. These are matched by signature:
# - any `bool Foo::findXxx(... ) const {`
# - `std::vector<...> Foo::snapshot() const {`
# - `size_t Foo::recordCount() const {`, `*Count() const {`
# - `Foo* Foo::resolveDirectModelResourcePtr(...) const {`
# - `bool Foo::isDirectModelResourcePtr(...) const {`
# Pattern: function header line `... const {` immediately followed by
# `  std::unique_lock<std::shared_mutex> lock(m_mutex);`
# Where the function name matches read-only pattern.

# We do per-line walk: find sequences of (signature `...const {`) -> next line lock.
# If the signature contains find / snapshot / recordCount / Count\( / isDirect / resolveDirect /
# completeIdentityCount / runtime\w+Count, downgrade.

$lines = $src -split "`r?`n"
$readOnlyPatterns = @(
    'find[A-Z][\w]*\(',
    '::snapshot\(',
    '::recordCount\(',
    'Count\(\) const',
    '::isDirectModelResourcePtr\(',
    '::resolveDirectModelResourcePtr\(',
    '::hasResolvedGeoset\(',
    '::completeIdentityCount\(',
    '::runtimeBoundCount\(',
    '::runtimeCreationProvenanceCount\(',
    '::runtimeResolveProvenanceCount\(',
    '::runtimeSourceObjectCount\(',
    '::runtimeOwnerIdentityCount\(',
    '::readyPoseCount\(',
    '::spriteFramePoseCount\(',
    '::matrixPaletteCount\(',
    '::geosetRecordCount\(',
    '::readyGeosetCount\(',
    '::modelResourceCount\(',
    '::runtimeModelRecordCount\(',
    '::revision\(',
    '::findGeosetByPtrRef\(',
    '::findGeosetByDataRef\(',
    '::snapshotGeosets\(',
    '::snapshotModels\(',
    '::snapshotRuntimeModels\('
)

$count = 0
for ($i = 0; $i -lt $lines.Length - 1; $i++) {
    $sig = $lines[$i]
    $lockLine = $lines[$i + 1]
    if ($lockLine -notmatch '^  std::unique_lock<std::shared_mutex> lock\(m_mutex\);$') {
        continue
    }
    if ($sig -notmatch ' const \{$') { continue }
    $isReadOnly = $false
    foreach ($p in $readOnlyPatterns) {
        if ($sig -match $p) { $isReadOnly = $true; break }
    }
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
