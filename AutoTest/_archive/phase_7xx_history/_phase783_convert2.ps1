$ErrorActionPreference = "Stop"

function Convert-File($path, $extraAtomicNames) {
    $src = Get-Content $path -Raw
    if ($null -eq $src) { return }
    $orig = $src

    $src = $src -replace 'std::lock_guard<std::mutex>', 'std::unique_lock<std::shared_mutex>'

    foreach ($name in @('m_frameNumber') + $extraAtomicNames) {
        $src = $src -replace ("\+\+" + [regex]::Escape($name) + ";"),
            ($name + ".fetch_add(1u, std::memory_order_relaxed);")
    }

    $lines = $src -split "`r?`n"
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i]
        foreach ($name in @('m_frameNumber') + $extraAtomicNames) {
            if ($line -match ($name + '\.(fetch_add|load|store|compare_exchange)')) {
                continue
            }
            $line = [regex]::Replace($line, '(?<![.\w+])' + [regex]::Escape($name) + '(?![.\w])', {
                param($m)
                $idx = $m.Index
                $rest = $line.Substring($idx + $m.Length)
                if ($rest -match '^\s*=[^=]') {
                    return $m.Value
                }
                return $m.Value + '.load(std::memory_order_relaxed)'
            })
        }
        $lines[$i] = $line
    }
    $src = $lines -join "`r`n"

    if ($src -ne $orig) {
        Set-Content -Path $path -Value $src -NoNewline -Encoding UTF8
        Write-Host "Rewrote $path"
    } else {
        Write-Host "No change: $path"
    }
}

Convert-File "src\d3d9\war3\model\war3_model_resource_cache.cpp" @('m_revision')
