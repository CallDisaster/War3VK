# Helper for invoking IDA MCP from Task F.
param(
  [Parameter(Mandatory)] [string] $Tool,
  [Parameter(Mandatory)] [string] $ArgsJson,
  [Parameter(Mandatory)] [string] $OutFile,
  [int] $TimeoutSec = 60
)

$payload = @{
  jsonrpc = '2.0'
  id      = 1
  method  = 'tools/call'
  params  = @{
    name      = $Tool
    arguments = ($ArgsJson | ConvertFrom-Json)
  }
} | ConvertTo-Json -Depth 12 -Compress

$res = Invoke-RestMethod -Uri 'http://127.0.0.1:13337/mcp' -Method Post -ContentType 'application/json' -Body $payload -TimeoutSec $TimeoutSec
$text = ($res.result.content | ForEach-Object { $_.text }) -join "`n"
$dir = Split-Path -Parent $OutFile
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
[IO.File]::WriteAllText($OutFile, $text, [Text.UTF8Encoding]::new($false))
"WROTE $OutFile bytes=$($text.Length)"
