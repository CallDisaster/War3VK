param(
  [string]$Python = "python"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

Write-Host "[War3AutoTest] Starting MCP server..." -ForegroundColor Cyan
& $Python ".\war3_autotest_mcp.py"

