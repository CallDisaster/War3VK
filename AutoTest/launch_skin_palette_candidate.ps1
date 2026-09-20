param(
  [string]$GameDir='E:\Work\Warcraft III',
  [string]$MapPath='',
  [string]$EvidenceDir='',
  [switch]$LegacyComparison
)
$ErrorActionPreference='Stop'
$packageRoot=Split-Path -Parent $PSScriptRoot
$manifestPath=Join-Path $packageRoot 'candidate-manifest.json'
if(-not(Test-Path -LiteralPath $manifestPath)){throw 'Use the frozen candidate package containing candidate-manifest.json.'}
$manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if($manifest.productAccepted -ne $false -or $manifest.gameplayValidated -ne $false){throw 'Unexpected candidate manifest.'}
$expected=$manifest.files.'d3d9.dll'.sha256
if($expected -notmatch '^[0-9A-F]{64}$'){throw 'Candidate identity missing.'}
# Process-local only. The existing launcher records effective env and verifies
# the installed DLL hash. Neither script installs/replaces DLLs or closes games.
$matrix=@{
  DXVK_WAR3_SKIN_PALETTE_CONTRACT=if($LegacyComparison){'0'}else{'1'}
  DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT='1'
  DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH='1'
  DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_ON_LAG='1'
  DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_EVERY_FRAME='1'
}
$saved=@{}
foreach($name in $matrix.Keys){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try {
  foreach($name in $matrix.Keys){[Environment]::SetEnvironmentVariable($name,$matrix[$name],'Process')}
  & (Join-Path $PSScriptRoot 'launch_frame_history_player.ps1') -GameDir $GameDir -MapPath $MapPath -EvidenceDir $EvidenceDir -ExpectedDllSha $expected
} finally {
  foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
}
