# 2026-09-18 阶段 E（实机半）预备装置：启动环境快照。
#
# 硬约束：本脚本只读。它不部署 DLL、不覆盖任何游戏文件、不启动任何进程。
# （AGENTS.md：部署/启动需用户明确请求。）
#
# 白名单来源：从 src/ 下机械枚举的 DXVK_WAR3_* 变量（不是猜的）。
param([string]$OutDir = "E:\Work\warvk-capture")
$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$required = @(
  "DXVK_WAR3_FRAME_EVIDENCE",
  "DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT"
)
$context = @(
  "DXVK_WAR3_FRAME_EVIDENCE_CASTERS", "DXVK_WAR3_FRAME_EVIDENCE_DRAWS",
  "DXVK_WAR3_FRAME_EVIDENCE_INPUTS", "DXVK_WAR3_FRAME_EVIDENCE_OUTPUT",
  "DXVK_WAR3_FRAME_EVIDENCE_PRE_FRAMES", "DXVK_WAR3_FRAME_EVIDENCE_POST_FRAMES",
  "DXVK_WAR3_FRAME_EVIDENCE_VA_HEADROOM_MB",
  "DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED",
  "DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS",
  "DXVK_WAR3_SHADOW_METADATA_CAPTURE", "DXVK_WAR3_SHADOW_METADATA_ALPHA",
  "DXVK_WAR3_SHADOW_METADATA_BLOCKER",
  "DXVK_WAR3_SHADOW_POSE_FULL_TRACE", "DXVK_WAR3_SHADOW_STAGE_LIFECYCLE",
  "DXVK_WAR3_NATIVE_DOODAD_STATIC_STAMP", "DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW",
  "DXVK_WAR3_PERF_LEVEL", "DXVK_WAR3_PERF_MONITOR", "DXVK_WAR3_PERF_RECORD_ON_START",
  "DXVK_WAR3_DISABLE", "DXVK_WAR3_PROFILE", "DXVK_WAR3_INTERNAL_TEST_API"
)

$devDll = "E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914\build32\src\d3d9\d3d9.dll"
$siteDll = "E:\Work\Warcraft III\d3d9.dll"

$snap = [ordered]@{}
$snap.capturedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
$snap.host = $env:COMPUTERNAME
$snap.user = $env:USERNAME
$snap.required = [ordered]@{}
$snap.context = [ordered]@{}
$snap.allDxvkVars = [ordered]@{}
$snap.binaries = [ordered]@{}

foreach ($n in $required) { $snap.required[$n] = [Environment]::GetEnvironmentVariable($n) }
foreach ($n in $context) { $snap.context[$n] = [Environment]::GetEnvironmentVariable($n) }
Get-ChildItem Env: | Where-Object { $_.Name -like "DXVK_*" } | Sort-Object Name | ForEach-Object { $snap.allDxvkVars[$_.Name] = $_.Value }

try { $osi = Get-CimInstance Win32_OperatingSystem; $snap.os = "$($osi.Caption) $($osi.Version) build $($osi.BuildNumber)" } catch { $snap.os = "unavailable" }
try { $cpui = Get-CimInstance Win32_Processor | Select-Object -First 1; $snap.cpu = $cpui.Name } catch { $snap.cpu = "unavailable" }
try { $snap.gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) | driver $($_.DriverVersion)" }) } catch { $snap.gpu = @("unavailable") }

foreach ($p in @($devDll, $siteDll)) {
  if (Test-Path $p) {
    $fi = Get-Item $p
    $hh = (Get-FileHash $p -Algorithm SHA256).Hash
    $snap.binaries[$p] = [ordered]@{ bytes = $fi.Length; sha256 = $hh; mtimeUtc = $fi.LastWriteTimeUtc.ToString("o") }
  } else {
    $snap.binaries[$p] = $null
  }
}

$stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss")
$jsonPath = Join-Path $OutDir ("startup-env-" + $stamp + ".json")
$snap | ConvertTo-Json -Depth 6 | Set-Content -Path $jsonPath -Encoding UTF8

$out = @()
$out += ("# 启动环境快照 " + $stamp)
$out += ""
$out += "## 必填项（缺失 => 证据子系统零操作、采集会静默为空）"
$missing = @()
foreach ($n in $required) {
  $v = $snap.required[$n]
  if ($null -eq $v -or $v -eq "") {
    $missing += $n
    $out += ("- " + $n + " = <MISSING>")
  } else {
    $out += ("- " + $n + " = " + $v)
  }
}
$out += ""
$out += "## 上下文旋钮（记录当时值；本脚本不设置任何变量）"
foreach ($n in $context) { $out += ("- " + $n + " = " + $snap.context[$n]) }
$out += ""
$out += "## 全量 DXVK_* （白名单之外也记，避免漏记本身成为盲点）"
foreach ($k in $snap.allDxvkVars.Keys) { $out += ("- " + $k + " = " + $snap.allDxvkVars[$k]) }
$out += ""
$out += "## 主机"
$out += ("- OS: " + $snap.os)
$out += ("- CPU: " + $snap.cpu)
foreach ($g in $snap.gpu) { $out += ("- GPU: " + $g) }
$out += ""
$out += "## 二进制哈希"
foreach ($k in $snap.binaries.Keys) {
  $b = $snap.binaries[$k]
  if ($null -eq $b) {
    $out += ("- " + $k + " => <absent>")
  } else {
    $out += ("- " + $k + " => " + $b.bytes + " B  " + $b.sha256 + "  mtime " + $b.mtimeUtc)
  }
}
$mdPath = Join-Path $OutDir ("startup-env-" + $stamp + ".md")
$out | Set-Content -Path $mdPath -Encoding UTF8
Write-Output ("json = " + $jsonPath)
Write-Output ("md   = " + $mdPath)
if ($missing.Count -gt 0) { Write-Output ("MISSING REQUIRED: " + ($missing -join ", ")) } else { Write-Output "required env vars: all present" }