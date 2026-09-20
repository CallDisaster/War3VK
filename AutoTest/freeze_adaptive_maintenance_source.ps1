param([Parameter(Mandatory=$true)][string]$Receipt)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$mirror='E:\WarVK-Builds\v1.22-maintenance-20260920-r1\source'
if (!(Test-Path -LiteralPath $mirror -PathType Container)) { throw 'Missing previously validated dependency mirror' }
if (Test-Path -LiteralPath $Receipt) { throw 'Receipt already exists' }
$manifest=Get-Content -Raw 'E:\WarVK-Builds\v1.22-maintenance-20260920-r1\source-revision3.json' | ConvertFrom-Json
foreach($dep in $manifest.dependencies.PSObject.Properties) {
  $gitlink=(& git -C $repo ls-tree HEAD -- $dep.Name)
  if ($gitlink -notmatch [regex]::Escape($dep.Value.commit)) { throw "Gitlink drift: $($dep.Name)" }
}
# Only tracked/untracked source files, never ignored build artifacts or dirty
# submodule contents. Copy is a build-input snapshot, not an edit to live source.
$paths=@(& git -C $repo -c core.quotepath=false ls-files --cached --others --exclude-standard)
$files=[ordered]@{}
foreach($rel in $paths) {
  if ($rel -match '(^|/)\.\.(/|$)' -or [IO.Path]::IsPathRooted($rel)) { throw 'Unsafe relative source path' }
  $src=Join-Path $repo $rel
  if (!(Test-Path -LiteralPath $src -PathType Leaf)) { continue }
  if ($rel -match '^(subprojects/StormBreaker|subprojects/imgui|src/minhook|smaa)/') { throw 'Unexpected nested gitlink file' }
  $dst=Join-Path $mirror $rel
  $before=(Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
  if (!(Test-Path -LiteralPath $dst) -or (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash -ne $before) {
    New-Item -ItemType Directory -Path (Split-Path $dst -Parent) -Force | Out-Null
    [IO.File]::Copy($src,$dst,$true)
  }
  if ((Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash -ne $before -or
      (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash -ne $before) { throw "Copy identity drift: $rel" }
  $files[$rel]=@{size=(Get-Item -LiteralPath $src).Length;sha256=$before}
}
$result=@{sourceHead=(& git -C $repo rev-parse HEAD);sourceRoot=$repo;mirror=$mirror;
  dependencies=$manifest.dependencies;files=$files;status=(& git -C $repo status --short)}
$bytes=[Text.Encoding]::UTF8.GetBytes(($result | ConvertTo-Json -Depth 8))
$stream=[IO.File]::Open($Receipt,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
try {$stream.Write($bytes,0,$bytes.Length)} finally {$stream.Dispose()}
Write-Output "Frozen $($files.Count) source files; receipt $Receipt"
