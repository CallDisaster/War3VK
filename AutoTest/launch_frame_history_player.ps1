param(
  [string]$GameDir='E:\Work\Warcraft III',
  [string]$MapPath='',
  [string]$ExpectedDllSha='6D6C7F3C3C834CE56692881A5C46DE6086D9AF2B7F93C2C6848E39A5D5F594B1',
  [string]$EvidenceDir='',
  [switch]$ExternalWatcher
)
$ErrorActionPreference='Stop'
$folder=[IO.Path]::GetFullPath($GameDir)
$dll=Join-Path $folder 'd3d9.dll'
$game=Join-Path $folder 'war3.exe'
if(-not(Test-Path -LiteralPath $game)){throw 'war3.exe missing'}
if((Get-FileHash -LiteralPath (Join-Path $folder 'Game.dll') -Algorithm SHA256).Hash -ne 'E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A'){throw 'Unvalidated Game.dll version; launch was not attempted.'}
if((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -ne $ExpectedDllSha){throw 'Install the packaged candidate only after exiting the game; this launcher never overwrites DLLs.'}
$games=@(Get-CimInstance Win32_Process | Where-Object {$_.Name -match '^(war3|warcraft iii)\.exe$'})
if($games.Count){throw 'A game is already running; nothing was closed or replaced.'}
$watch=Join-Path $PSScriptRoot 'frame_history_watch.py'
if($ExternalWatcher -and -not(Test-Path -LiteralPath $watch)){throw 'Keep AutoTest scripts together'}
$evidenceRoot=if($EvidenceDir){[IO.Path]::GetFullPath($EvidenceDir)}else{Join-Path $folder 'WarVK\Log\FrameEvidence'}
if(([IO.DriveInfo]::new([IO.Path]::GetPathRoot($evidenceRoot))).AvailableFreeSpace -lt 4GB){throw 'At least 4 GB free disk space is required for an incident package; use -EvidenceDir on another drive.'}
if(!(Test-Path -LiteralPath $evidenceRoot)){[void](New-Item -ItemType Directory -Path $evidenceRoot)}
$python=if($ExternalWatcher){Get-Command py -ErrorAction Stop}else{$null}
$run=Join-Path $evidenceRoot ('launch-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
if(Test-Path -LiteralPath $run){throw 'Launch identity path already exists'}
[void](New-Item -ItemType Directory -Path $run)
# All environment changes are confined to this launcher and its child processes.
$env:DXVK_WAR3_FRAME_EVIDENCE='1'
$env:DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED=if($ExternalWatcher){'0'}else{'1'}
$env:DXVK_WAR3_FRAME_EVIDENCE_INPUTS='1'
$env:DXVK_WAR3_FRAME_EVIDENCE_OUTPUT=$evidenceRoot
$env:DXVK_WAR3_FRAME_EVIDENCE_CASTERS='0'
$env:DXVK_WAR3_FRAME_EVIDENCE_DRAWS='1'
$env:DXVK_WAR3_ASYNC_SCREENSHOT='1'
$env:DXVK_WAR3_IMGUI_BEFORE_UI='0'
$env:DXVK_WAR3_NATIVE_MODEL_LIGHTS='0'
$env:DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER='0'
$env:DXVK_WAR3_DEBUG_CONSOLE='0'
$env:DXVK_WAR3_PERF_RECORD_ON_START='0'
$env:DXVK_WAR3_PERF_RECORD_ON_GAME_START='0'
$env:PYTHONIOENCODING='utf-8'
$pins=@($dll,(Join-Path $folder 'Game.dll'),$game)
if($MapPath){$pins+=([IO.Path]::GetFullPath($MapPath))}
$identityFile=Join-Path $run 'launcher-identities.json'
$identity=@{files=@($pins|ForEach-Object {@{path=$_;size=(Get-Item -LiteralPath $_).Length;sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash}});
  mapPathSpecified=[bool]$MapPath;mapLoadedVerified=$false;environment=@{};utc=[DateTime]::UtcNow.ToString('o')}
Get-ChildItem Env: | Where-Object {$_.Name -like 'DXVK_WAR3_*'} | ForEach-Object {$identity.environment[$_.Name]=$_.Value}
[IO.File]::WriteAllText($identityFile,($identity|ConvertTo-Json -Depth 5))
$params=@{FilePath=$game;WorkingDirectory=$folder;PassThru=$true}
if($MapPath){$map=[IO.Path]::GetFullPath($MapPath);if(-not(Test-Path -LiteralPath $map)){throw 'Map missing'};$params.ArgumentList=@('-loadfile',('"'+$map+'"'))}
$process=Start-Process @params
if($ExternalWatcher){
  $watcher=Start-Process -FilePath $python.Source -ArgumentList @(('"'+$watch+'"'),'--pid',"$($process.Id)",'--identity-file',('"'+$identityFile+'"')) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $run 'watcher.log') -RedirectStandardError (Join-Path $run 'watcher-error.log')
  try {$watcher.PriorityClass='BelowNormal'} catch {Write-Warning 'Could not lower watcher priority; game was not changed.'}
}
if($ExternalWatcher){Write-Output "Game PID $($process.Id). Explicit external-watcher mode selected."}
else{Write-Output "Game PID $($process.Id). DLL recorder is self-contained; Python is not required."}
Write-Output 'Enter your map, wait for the HUD to show at least 1.00 seconds, then press Ctrl+Shift+C. Use 2560x1440 as before.'
Write-Output "Launch identities: $run ; raw incident output: $evidenceRoot"
Write-Output 'No need to enable performance recording or watch the shadow-factor view. One trigger only; the package remains an incomplete diagnostic candidate.'
