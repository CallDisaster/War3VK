param(
  [string[]] $Addrs,
  [string]   $OutDir
)

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

foreach ($addrHex in $Addrs) {
  $payload = @{
    jsonrpc = '2.0'
    id      = 1
    method  = 'tools/call'
    params  = @{
      name      = 'decompile'
      arguments = @{ addr = $addrHex }
    }
  } | ConvertTo-Json -Depth 12 -Compress
  try {
    $r = Invoke-RestMethod -Uri 'http://127.0.0.1:13337/mcp' -Method Post -ContentType 'application/json' -Body $payload -TimeoutSec 90
    $text = ($r.result.content | ForEach-Object { $_.text }) -join "`n"
    $file = Join-Path $OutDir ("decomp_" + $addrHex.Replace('0x','') + ".txt")
    [IO.File]::WriteAllText($file, $text, [Text.UTF8Encoding]::new($false))
    "OK $addrHex -> $file ($($text.Length) bytes)"
  } catch {
    "ERR $addrHex : $_"
  }
}
