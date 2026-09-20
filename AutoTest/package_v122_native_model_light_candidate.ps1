param([string]$OutputDir='C:\Users\Administrator\Desktop\WarVK-v1.22-two-model-lights-74CC-20260914')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=[IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $out) { throw 'CreateNew package directory already exists' }
if (Test-Path -LiteralPath ($out+'.zip')) { throw 'CreateNew zip already exists' }
$dll=Join-Path $root 'build32/src/d3d9/d3d9.dll'
$expected='74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73'
if ((Get-Item -LiteralPath $dll).Length -ne 35486415 -or (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -ne $expected) { throw 'Candidate changed' }
$pe=[IO.BinaryReader]::new([IO.File]::OpenRead($dll))
try {
    if ($pe.ReadUInt16() -ne 0x5A4D) { throw 'Not MZ' }
    $pe.BaseStream.Position=0x3C; $peOffset=$pe.ReadUInt32()
    $pe.BaseStream.Position=$peOffset
    if ($pe.ReadUInt32() -ne 0x4550 -or $pe.ReadUInt16() -ne 0x14C) { throw 'Not PE/i386' }
    $pe.BaseStream.Position=$peOffset+24
    if ($pe.ReadUInt16() -ne 0x10B) { throw 'Not PE32' }
} finally { $pe.Dispose() }
$evidence=Join-Path $root 'AutoTest/artifacts/v122_native_lights_20260914/native_mixed_two_r23'
$receiptPath=Join-Path $evidence 'receipt.json'
if ((Get-Item -LiteralPath $receiptPath).Length -ne 41129 -or (Get-FileHash -LiteralPath $receiptPath -Algorithm SHA256).Hash -ne 'E2FA05598F9539F718A2A55020870E662B341D37E0CD14D64DF8B974529FE3B2') { throw 'Frozen receipt changed' }
$receipt=Get-Content (Join-Path $evidence 'receipt.json') -Raw | ConvertFrom-Json
if (-not $receipt.ok -or -not $receipt.jassStagesPassed -or $receipt.runCount -ne 1 -or $receipt.globalInputUsed -or -not $receipt.restoreOk -or -not $receipt.playerUntouched -or -not $receipt.authorMapUntouched -or $receipt.gpuEvents.Count -or $receipt.newDumps.Count) { throw 'Runtime closure missing' }
$preflight=Get-Content (Join-Path $evidence 'preflight.json') -Raw | ConvertFrom-Json
if ($preflight.candidate.sha256 -ne $expected -or $preflight.candidate.size -ne 35486415) { throw 'Receipt candidate mismatch' }
if ($receipt.colorFallbacks.Count -or $receipt.receiverFallbacks.Count) { throw 'Runtime transaction fallback' }
$launch=Get-Content (Join-Path $evidence 'launch.json') -Raw | ConvertFrom-Json
if (-not $launch.useIsolatedDesktop -or -not $launch.desktop.nonInteractiveOnly -or $launch.desktop.inputDesktopBefore -ne $launch.desktop.inputDesktopAfter) { throw 'Desktop isolation missing' }
$resolution=Get-Content (Join-Path $evidence 'resolution.json') -Raw | ConvertFrom-Json
if (-not $resolution.ok -or $resolution.info.clientRect.width -ne 2560 -or $resolution.info.clientRect.height -ne 1440) { throw 'Resolution mismatch' }
$exitReceipt=Get-Content (Join-Path $evidence 'natural-exit.json') -Raw | ConvertFrom-Json
if (-not $exitReceipt.ok -or -not $exitReceipt.nativeTermination.exact -or -not $exitReceipt.nativeTermination.bindingExact -or $exitReceipt.nativeTermination.exitCode -ne 0) { throw 'Exact native exit missing' }
# Only this run's exact current D3D9 log is accepted. A single early commit is
# insufficient: require both selected sources and a commit AFTER screenshot 4.
$runtimeLog=Join-Path $evidence 'after-11-war3_d3d9.log'
if ((Get-Item -LiteralPath $runtimeLog).Length -ne 128346 -or (Get-FileHash -LiteralPath $runtimeLog -Algorithm SHA256).Hash -ne '91E1EAD4DC3E8EE9C717152B6C4D237AFAAF8F95613D34F08B4CB74419A5BD67') { throw 'Frozen runtime log changed' }
$runtimeText=Get-Content -LiteralPath $runtimeLog -Raw
$tail=$runtimeText -split '\[AsyncScreenshot\] saved id=4 size=2560x1440 ',2
if ($tail.Count -ne 2 -or $tail[1] -notmatch 'receiver committed frame=\d+ automatic=2 cubeLights=2 cubeFaces=12' -or
    $tail[1] -notmatch 'selected frame=\d+ policy=4294967295 .*xyz=-58\.1924,163\.395,150\.935' -or
    $tail[1] -notmatch 'selected frame=\d+ policy=2 .*xyz=295\.873,177\.994,76\.2476' -or
    $tail[1] -notmatch 'color lease frame=\d+ lights=2 draws=\d+ tailDraws=[1-9]\d*') { throw 'Sustained mixed-light tail proof missing' }
# Models are editor-owned input, not a DLL delivery dependency. Never package a
# later user edit as the ED122 input tested by the immutable mixed R23 fixture.
$files=@{ 'd3d9.dll'=$dll;
 'README.md'=(Join-Path $root 'docs/V122_MODEL_LIGHT_CANDIDATE_README.md') }
foreach($relative in @('docs/research/2026-09-14-model-path-point-light-japi.md',
 'docs/research/2026-09-14-two-native-lights-and-tail-transport.md',
 'docs/agent-history/2026-09-14-two-native-lights-runtime-checkpoint.md',
 'docs/agent-history/DEVELOPMENT_CHANGELOG.md','docs/RELEASE_NOTES_1.22.00_DRAFT.md',
 'docs/plan/2026-09-14-v1.22-volumetric-and-japi-workload-assessment.md',
 'docs/plan/2026-09-14-v1.22-integration-and-release-gates.md')) {
    $files[$relative]=Join-Path $root $relative
}
foreach($name in @('action.txt','call.txt','define.txt','README.md','MATH_CURVE_API.md')) {
    $files['WarVK/'+$name]=Join-Path $root ('WarVK/'+$name)
}
foreach($f in Get-ChildItem (Join-Path $root 'WarVK/jass') -File) { $files['WarVK/jass/'+$f.Name]=$f.FullName }
foreach($name in @('receipt.json','preflight.json','launch.json','resolution.json','natural-exit.json','status-3.json','stage-1.png','stage-3.png')) {
    $files['evidence/'+$name]=Join-Path $evidence $name
}
$files['evidence/war3_d3d9.log']=$runtimeLog
[void][IO.Directory]::CreateDirectory($out)
$manifest=@()
foreach($entry in $files.GetEnumerator()) {
    $target=Join-Path $out $entry.Key
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
    $before=(Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256).Hash
    [IO.File]::Copy($entry.Value,$target,$false)
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $before -or (Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256).Hash -ne $before) { throw 'Copy identity changed' }
    $manifest += [pscustomobject]@{path=$entry.Key;bytes=(Get-Item -LiteralPath $target).Length;sha256=$before}
}
$report=[pscustomobject]@{candidate=$true;productAccepted=$false;pointShadowVisualAccepted=$false;dllSha256=$expected;pe='PE32/i386';pythonStaticPassed=148;automaticNativeShadowLights=2;totalLightBudget=16;totalCubeBudget=4;modelBundled=$false;testedModelSha256='ED12243B5560E39BD118DF6591738EDF22C2131DA575E8202C62ADB4EF749C3A';runtimeReceipt='native_mixed_two_r23';sustainedMixedLightGate=$true;files=($manifest|Sort-Object path)}
$bytes=[Text.Encoding]::UTF8.GetBytes(($report|ConvertTo-Json -Depth 5))
$stream=[IO.File]::Open((Join-Path $out 'manifest.json'),[IO.FileMode]::CreateNew)
try {$stream.Write($bytes,0,$bytes.Length)} finally {$stream.Dispose()}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipPath=$out+'.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($out,$zipPath,[IO.Compression.CompressionLevel]::Optimal,$false)
$archive=[IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $expectedEntries=@{}
    foreach($item in $manifest) { $expectedEntries[$item.path]=$item.sha256 }
    $expectedEntries['manifest.json']=(Get-FileHash -LiteralPath (Join-Path $out 'manifest.json') -Algorithm SHA256).Hash
    if ($archive.Entries.Count -ne $expectedEntries.Count) { throw 'ZIP entry count mismatch' }
    foreach($entry in $archive.Entries) {
        if (-not $expectedEntries.ContainsKey($entry.FullName)) { throw 'Unexpected or duplicate ZIP entry' }
        $entryStream=$entry.Open(); $hasher=[Security.Cryptography.SHA256]::Create()
        try { $hash=[BitConverter]::ToString($hasher.ComputeHash($entryStream)).Replace('-','') }
        finally { $entryStream.Dispose(); $hasher.Dispose() }
        if ($hash -ne $expectedEntries[$entry.FullName]) { throw 'ZIP byte identity mismatch' }
        $expectedEntries.Remove($entry.FullName)
    }
    if ($expectedEntries.Count) { throw 'ZIP missing entry' }
} finally { $archive.Dispose() }
Write-Output "CreateNew package and ZIP copied and byte-verified: $out"
[pscustomobject]@{zip=$zipPath;bytes=(Get-Item -LiteralPath $zipPath).Length;sha256=(Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash;dllSha256=$expected;files=$manifest.Count}|ConvertTo-Json
