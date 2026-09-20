# v1.22 final validation matrix

- taskId: `validation-release-r2`
- status: offline preparation; not release acceptance
- scope: read-only audit of the AutoTest/runtime/package pipeline plus an executable offline manifest validator and a frozen final-validation plan.
- batch limits: no build/Ninja/Meson/compiler/game/GPU/deploy/DLL copy/package generation/Git mutation. Existing dirty state is preserved.
- ownership: root owns ImGui, device, diagnostics bridge, Meson, version, AGENTS, all changelogs, and Render Stats UI.

## 1. Candidate and package contract

The final candidate identity is not frozen by this plan.  A release candidate must be produced by a later root-authorized clean build and pinned with exact bytes and SHA-256 before any runtime run.

The validator accepts one supported manifest shape only.  Unsupported aliases are rejected rather than silently migrated:

- `schema` must be integer `1`; `kind` must be `warvk-offline-candidate`; `scope` must be `player` or `author`.
- `target` must contain exactly `path`, `sha256`, and `size`; both a valid 64-hex SHA-256 and a positive integer size are required.  `target.path` must be exactly `d3d9.dll`; `dll_name` aliases are not accepted.  Paths must be exact normalized relative paths with no `..`, colon/drive/ADS alias, backslash, dot, empty component, trailing space/dot, or Windows reserved device name; there is no basename fallback.
- `files` must be a non-empty object that exhaustively lists every package payload file except the manifest itself.  Every entry must contain exactly `sha256` and `size`, and every actual package byte must match its declared identity.  Any unlisted extra file or missing declared file fails.
- `flags` must contain exactly the offline acceptance flags and every value must be a real JSON boolean `false`: `releaseReady`, `productAccepted`, `gameplayValidated`, `gpuAccepted`, `visualAccepted`, `visualRecoveryProven`, `fissureFixed`, `stable`, `rootCauseReady`.  String values such as `"true"` and top-level alias flags fail.
- `buildConfig` must contain exactly `architecture=win32-i386`, `waterExperimental=false`, `nativeAutoLights=false`, and `x64Product=false`.  This is a self-declaration, not independent build provenance: the validator reports `buildConfigProvenance=self-declared-not-independent` and `buildEvidenceVerified=false`.  Source/config evidence identity is verified only when such evidence is actually available; absent evidence, no product acceptance is invented.  Binary feature exclusion therefore comes from explicit config provenance, not filename guessing.
- Malformed `scope` values (non-string, list, object, null) fail closed without an uncaught exception.
- `limits` must be a non-empty array of non-empty strings.

Candidate PE identity is parsed from the bytes: required machine `0x14C`, optional-header magic `0x10B`, no AMD64/PE32+ machine or magic.  The COFF `IMAGE_FILE_LARGE_ADDRESS_AWARE` bit is read and reported as actual `largeAddressAware`; it is not hardcoded and is not equated with x64.  A 32-bit DLL's LAA flag is a DLL property, not proof about patched `War3.exe`.

Package scope is explicit, not extension or substring based:

- `player` root files: `d3d9.dll`, `README.{md,txt}`, `LICENSE`, `LICENSE.txt`, `COPYING`, `COPYING.txt`, `CHANGELOG.{md,txt}`, `VERSION`, `pack.json`.
- `author` additionally allows reviewed prefixes `WarVK/`, `assets/`, `docs/`, `subprojects/war3fx/`, and `water/`, subject to an explicit extension policy: `.j`, `.json`, `.md`, `.txt`, `.csv`, `.png`, `.jpg`, `.jpeg`, `.gif`, and `.svg`, plus reviewed root basenames such as `LICENSE` and `README.md`.
- Legitimate `LICENSE.txt`, author `pack.json`, catalog JSON under `docs/`, and v1.21 Water assets under an author prefix are accepted.  Blanket `.txt`/`.json`/`.png` or Water-substring rejection is removed.
- Game assets, dumps, raw evidence, x64 payloads, archives, executables, and other binaries are rejected even when nested under an author prefix.  Game/binary/evidence rejection is enforced by the explicit scope/extension policy and provenance, not by broad filename substrings.

Offline validator:

```powershell
py -B AutoTest\release_v122_package_audit.py `
  --package-dir <package-dir> `
  --manifest <package-dir>\manifest.json `
  --archive <package.zip> `
  --expect-sha256 <SHA256> --expect-size <bytes>
```

The audit is read-only.  It does not copy, deploy, build, launch, or modify evidence.  Synthetic tests are in `AutoTest/test_release_v122_package_audit.py`.

## 2. Existing pipeline audit

`AutoTest/run_snapshot_pressure_canary.py` already contains the correct fail-closed primitives:

- `FROZEN_BINDING_SCHEMA = 2` and a fixed route: 20 s baseline, six 10 s offsets, 80 s relief, camera angle of attack 335, camera duration 1.5 s;
- `--apply` requires `--frozen-manifest`; the runner does not self-approve current live bytes;
- manifest validation requires exact candidate/liveRecovery/map/game/exe identities, `2560x1440`, isolated desktop, `globalInputUsed=false`, heavy forensics off, census on, explicit upload range `0` or `1`, profile `full_default`, and 384 MiB resident cap;
- binding/frozen identity checks compare exact SHA-256 and size before mutation;
- ready handling requires owner runtime status fields (`module.state=Running`, `jassReady`, `runtimeReady`, `gameStarted`, `inGameRenderReady`); `menu-ready-not-started` is not ready;
- only own isolated non-interactive desktops may issue one bounded HWND-scoped `SPACE` continue pulse; timeout is not retried and is not widened;
- restoration returns live to exact pre-run bytes and records zero-process/dump/GPU-event evidence.

Night evidence showed the main live blocker: the difficulty-selection dialog remained visible in the captured pressure/relief frames, so the run is not gameplay or visual acceptance.  A bounded isolated click exists in `run_life_and_death_tdr_scenario` (client-width 50%, client-height 24%) but it explicitly records `dialogDismissedVerified=false` because PostMessage acceptance is not proof that the dialog consumed the click.  The snapshot-pressure route must not treat that click alone as ready.

Existing package scripts (`package_v122_diagnostic_preparation.py`, `package_skin_palette_contract_candidate.py`, `parent-final-verification.py`) already perform byte-copy/archive identity checks, but they are generation-oriented and do not fail closed on every release-scope/raw-evidence/acceptance risk.  The new audit fills that read-only gate.

## 3. Final clean configuration gates

No gate below is executed or claimed by this batch.

1. Source and build freeze: all writers stopped, existing dirty state preserved; clean independent build directory; release configuration; heavy recorder/evidence flags off; experimental Consume/Observer/Water/native auto-lights/x64 paths not enabled; `b_ndebug` and build options checked by value, not only by `buildtype` name; exact DLL identity recorded.
2. Offline package audit: run the validator against the candidate directory/archive and frozen source record; fail on any identity, PE, scope, raw-evidence, x64/Water, or acceptance-claim mismatch.
3. Frozen runtime manifest: exact candidate/liveRecovery/map/game/exe identities, fixed route, `2560x1440`, isolated desktop, zero global input, heavy forensics off, census on, upload range explicit.
4. Ready gate: `moduleRunning`, `jassReady`, `runtimeReady`, `gameStarted`, `inGameRenderReady` all true; difficulty dialog absent or independently verified dismissed before the first phase marker.  Otherwise fail without running the route.
5. CSM pressure/return: five producer-domain phase markers; page resident/cap rejects and required-caster omission counters recorded; caster min/mean must not decrease, no zero-caster frame, no `renderedCurrentPartialShadowMap`, required complete serial advances, and relief must satisfy the existing thresholds (at least 60 relief frames, 60 sustained valid frames, max invalid run 6, at least 8 fresh complete serial advances).
6. Screenshot/perf OFF/ON: two runs differ only by `DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE=0/1`; each run captures normal-start, two pressure points, and relief-end at true 2560x1440 with async screenshot and exact exit; no total-counter benefit claim unless matched-window cut proof exists.
7. Exit/restore: native game exit 0 or exact owned termination semantics already documented; no forced termination; live DLL restored to frozen bytes; zero residual processes, new dumps, or GPU events.
8. JAPI functional gate: sun on/off/direction/color, local fog sphere/box/cylinder create/modify/destroy/8-slot cap, Froxel Medium/High (`volumetric.setBackend` 1/2), density/scattering/quality, and Guide behavior through Froxel low-angle/edge motion.  Guide has no separate JASS setter; verify it through the Froxel/volume path and visual frames.
9. Resolution gate: client/window reported as true `2560x1440`; no stretched/fallback resolution may be accepted as the requested target.

## 4. Frozen low-input route proposal

No game is launched in this batch.  The proposed route is intentionally identical to the runner's `FROZEN_ROUTE`, with the difficulty precondition made explicit:

```text
baseline 20 s
offset (0,0) 10 s
offset (900,0) 10 s
offset (900,900) 10 s
offset (0,900) 10 s
offset (-900,0) 10 s
offset (0,0) 10 s
return/relief 80 s
angleOfAttack 335, cameraDuration 1.5
```

Input policy:

- launch only on an owner-created isolated non-interactive desktop; no desktop switch, no foreground, no global cursor/keyboard input;
- at most one bounded HWND-scoped `SPACE` pulse for continue after launch;
- if the map still presents a difficulty dialog, use at most one bounded HWND-scoped click only after the root adds an independently verified difficulty-dismissal signal; otherwise fail before phase markers;
- all camera motion goes through the internal control plane/JASS route, not system input;
- the route is not valid if the difficulty overlay is visible in any phase image or if the ready classification is not `in-game-ready`.

This keeps the low-input shape while refusing to convert an unverified dialog click into a false ready/visual pass.

## 5. JAPI coverage vs export/catalog

Current catalog: 109 wire commands and 109 mapped public `WarVK*` JASS functions (the 112 `WarVK*` functions in `WarVK/jass/warvk_api.j` include three non-command helpers).  Feature-mask classification gives:

| Class | Count | Notes |
| --- | ---: | --- |
| Implemented/supported commands | 98 | feature mask is a subset of `kImplementedFeatureMask` |
| Declared unsupported commands | 11 | `outline.*` 3, `bloom.*` 3, `postfx.*` 3, `aa.*` 2; all return `UnsupportedFeature` |
| Source-level commands referenced by tests | 77 | 76 supported + `bloom.setEnabled` as expected-unsupported; two protocol-negative tokens (`missing.command`, unrelated missing token) are excluded |
| Supported commands not referenced by source-level tests | 22 | see list below |

The 22 currently untested supported commands are:

```text
csm.setTuning
curve.derivativeComponent
dayNight.setEnabled
dayNight.setSpeed
dayNight.setTime
lightingClock.setDayDuration
lightingCycle.resetColorTemperatureProfile
lightning.create
lightning.setColor
lightning.setEnabled
lightning.setEndpoints
lightning.setFormulaCurve
lightning.setPolylineCurve
lightning.setWidth
managedObject.isAlive
managedObject.type
stats.drawCallCount
stats.framesPerSecond
stats.frameTimeMilliseconds
time.frameIndex
time.visualSeconds
volumetricFog.setSettings
```

The historical `v122_jass_vm_scenario.j` covers 48 command names/83 assertions, consistent with the release note's "49 public functions" once one helper is counted.  That is source-level limited coverage, not clean-config runtime or visual acceptance.  The 10 unsupported commands other than `bloom.setEnabled` are not exercised even as unsupported in the current source-level tests; they must be classified explicitly in the release matrix rather than treated as supported.

## 6. Render Stats read-only review (root owns UI)

Root's current `drawRenderStatsPanel` reads only published/independent sources: `QueryShadowProducerRuntimeDiagnostics`, `ShadowArena_QueryMemoryStats`, `QueryShadowReplayDiagnostics`, `QueryShadowDisplayStats`, `QueryCsmResolutionDiagnostics`, `War3Stage11SnapshotResidentCapBytes`, and `GlobalMemoryStatusEx`.  The panel already throttles to 4 Hz, says sources are independently sampled, and explicitly marks page resident/cache references as non-additive and VA as not physical VRAM.

Read-only suggestions:

- Keep the existing published getters.  If FPS/frame time/draw-call fields are desired, reuse the already published `war3shader::GetFrameTime()` / `GetDrawCalls()` semantics (the same values behind `stats.framesPerSecond`, `stats.frameTimeMilliseconds`, `stats.drawCallCount`), not a second monitor.
- Keep `requestedResolution`/`effectiveResolution`/`fallbackReason` visible for the final true-2560x1440 gate.
- Do not subtract fields from independent samples or combine page resident with static cache references; the current truthfulness labels should remain.
- Do not add per-frame scene locks or semantic-builder calls merely to make the panel fresher; 4 Hz diagnostics are the correct boundary.

Offline verification only: `python -B AutoTest/test_render_stats_panel_static.py` ran 7/7 OK.  This does not validate the UI at runtime, does not compile the new metadata, and does not authorize a render-stats release claim.

## 7. Uncovered gates and next bounded task

Uncovered by this batch: clean build, compiler/CPU targets, game/GPU, visual/pixel acceptance, difficulty-dialog dismissal, full 109-command runtime validation, long-run/stability, clean package generation, release version/changelog update, merge/push/tag/release.

Next bounded task after root freezes writers and no game process remains: produce the clean configuration and exact frozen manifest, then run the validator and the offline runner unit contracts; only after that may root authorize a separate game/GPU lease for the OFF/ON route.  This plan does not start that task.
