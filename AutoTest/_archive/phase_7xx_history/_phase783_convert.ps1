# Phase 7.83 conversion script (v2 - simpler).
# Step 1: lock_guard<std::mutex> → unique_lock<std::shared_mutex>.
#   This keeps semantics identical (still exclusive lock everywhere).
# Step 2: Translate m_frameNumber operations to atomic-compatible code.
#   - `++m_frameNumber;`  → `m_frameNumber.fetch_add(1u, std::memory_order_relaxed);`
#   - `m_frameNumber` as r-value → `m_frameNumber.load(std::memory_order_relaxed)`
# After this, code compiles; we then manually downgrade specific const methods to shared_lock
# in a follow-up edit.

$ErrorActionPreference = "Stop"

function Convert-File($path) {
    $src = Get-Content $path -Raw
    if ($null -eq $src) { return }

    $orig = $src
    $src = $src -replace 'std::lock_guard<std::mutex>', 'std::unique_lock<std::shared_mutex>'
    $src = $src -replace '\+\+m_frameNumber;', 'm_frameNumber.fetch_add(1u, std::memory_order_relaxed);'

    # Replace m_frameNumber read usages (NOT followed by . meaning method call already).
    # We only target r-value contexts via regex. Skip lines that already contain .fetch_add / .load / .store.
    $lines = $src -split "`r?`n"
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i]
        # If line contains a fetch_add/load/store call on m_frameNumber, leave alone.
        if ($line -match 'm_frameNumber\.(fetch_add|load|store|compare_exchange)') { continue }
        # Replace `m_frameNumber` r-value occurrences (not LHS of `=` and not after `++`).
        $lines[$i] = [regex]::Replace($line, '(?<![.\w+])m_frameNumber(?![.\w])', {
            param($m)
            $idx = $m.Index
            $rest = $line.Substring($idx + $m.Length)
            if ($rest -match '^\s*=[^=]') {
                # LHS of assignment. Leave for manual fix (should not exist after step 1).
                return $m.Value
            }
            return 'm_frameNumber.load(std::memory_order_relaxed)'
        })
    }
    $src = $lines -join "`r`n"

    if ($src -ne $orig) {
        Set-Content -Path $path -Value $src -NoNewline -Encoding UTF8
        Write-Host "Rewrote $path"
    } else {
        Write-Host "No change: $path"
    }
}

Convert-File "src\d3d9\war3\model\war3_model_registry.cpp"
