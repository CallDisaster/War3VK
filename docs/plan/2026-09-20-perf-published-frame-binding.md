# Perf published-frame binding contract — 2026-09-20

Status: architecture-b07 source/static review. Production source read-only.
This document records the parent-implemented producer contract and the independent
lock-chain review. It is not a compile, runtime, GPU, or visual acceptance.

## 1. Contract

`War3PerfMonitor::queryPublishedPerfState()` is the single public read of the
completed perf frame and accumulation domain.

Returned fields:
- `enabled`, `recording`: copied under the monitor mutex from the existing atomics.
- `valid`: true only when monitor is enabled/recording, the reset-domain epoch is
  nonzero, and at least one completed history row exists.
- `businessFrameSerial`: `m_frameHistory.back().workload.businessFrameSerial`.
- `frameEpoch`: `m_frameHistory.back().frameEpoch`.
- `producerAccumulationEpoch`: current nonzero aggregate-reset domain.

The completed row is the last row appended by
`War3PerfMonitor::endFrame()` -> `archiveFrame()` inside the swapchain Present
path. This is the `(start,end]` boundary: a phase marker reports the last
completed business frame, not the current in-flight Present and not a scene
`VisibleRenderableRegistry` frame number.

## 2. Reset domain

`War3PerfMonitor::resetShadowBudgetAggregateLocked()` is the only aggregate
assignment (`m_shadowBudgetAggregate = {};`). It is called from exactly:
- `War3PerfMonitor::shutdown()`
- `War3PerfMonitor::resetHistory()`

The helper increments `m_producerAccumulationEpoch` only when it is nonzero.
`UINT64_MAX -> 0` is exhausted forever; there is no reset-to-1 fallback and no
reuse of old epoch identities.

`captureExportSnapshotLocked()` copies `m_shadowBudgetAggregate` and
`producerAccumulationEpoch` under the same mutex, and
`shadowBudgetSummary.producerAccumulationEpoch` is emitted from that snapshot.
The epoch therefore identifies the cumulative-counter domain, while
`producerSealFrameSerialLast` remains a last-observation gauge.

## 3. Completed-cut limitation

`producerSealFrameSerialLast` must not be treated as a completed history cut:
- it is updated from `War3ShadowCaptureStats.producerSealFrameSerial` in
  `noteShadowBudgetFrame()`;
- the aggregate is mutex-owned and copied beside history, but that does not
  prove the last seal observation belongs to the same completed Present as
  `m_frameHistory.back()`;
- manual report export is not restricted to `endFrame()` ordering, so the
  aggregate may be ahead of the last archived history row even though auto
  export happens after `archiveFrame()`.

The analyzer keeps this as `scopeSemantics = "...not a completed-frame cut..."`.
No source proof exists yet for using nonzero cumulative deltas as a cut.
All-zero reports can pass; nonzero post-relief growth remains uncovered until an
exact producer cut proof is supplied.

## 4. Lock-chain review

Direct paths:
- control plane `get_runtime_status`, `wait_until`, `get_shadow_runtime_summary`,
  `get_hot_shadow_probe`
  -> `QueryRuntimeStatusSnapshot`
  -> `BuildRuntimeStatusSnapshot`
  -> `perf.queryPublishedPerfState()` (one `m_mutex` lock).
- `ExportRuntimeStatusSnapshot` -> same hub build path.
- `ExportRuntimeStatusSnapshot` callers:
  - `War3Events::fireOnGameStart` (holds `War3Events::m_mutex`)
  - `War3Events::reset` (holds `War3Events::m_mutex`)
  - `MarkInGameRenderReady` (only on the first ready CAS; may call
    `fireOnGameStart` first)
  - `LogRuntimeSummaryOnce`, `LogRuntimeHealthPeriodic` (bounded status logging)
- No `War3PerfMonitor` method calls into `War3Events` while holding
  `War3PerfMonitor::m_mutex`. The observed nested order is
  `War3Events::m_mutex -> War3PerfMonitor::m_mutex`; no reverse order was found.
- `queryPublishedPerfState()` releases `m_mutex` before
  `BuildRuntimeStatusSnapshot` proceeds to frame/shadow/lightning snapshots.
  It is one O(1) lock on an existing status-query path, not a new per-frame lock.
- The hub calls `queryPublishedPerfState()` exactly once. Both
  `war3_control_plane.cpp::ToJson` and
  `war3_diagnostics_hub.cpp::BuildRuntimeStatusJson` serialize the same
  `snapshot.perf` values; neither takes a second monitor snapshot.

Residual lock-chain risk: `War3Events::fireOnGameStart()` holds its mutex while
the hub build takes the monitor mutex. This is consistent with the existing
on-game-start callback path, but it is a real nested lock pair and should be
covered by tests before any future code adds a monitor-to-events callback.

## 5. Runner matrix and report env

`BuildPerfEnvJson()` kNames now contains the frozen canary matrix:
- base required keys, including `DXVK_WAR3_INTERNAL_TEST_API`,
  `DXVK_WAR3_INTERNAL_EXIT_TEST`, autotest disable keys,
  `DXVK_WAR3_PERF_LEVEL`, history/cap/profile/scenario/OBS keys,
  `DXVK_WAR3_STAGE11_BUDGET_CENSUS`, and
  `DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE`;
- all 11 `HEAVY_FORENSICS_OFF` keys.

Static check result: 121 kNames entries, all unique; the runner's required
matrix and heavy list are subsets of kNames. No additional required key
omissions were found after the parent edit. The report serializer skips empty
values, so the frozen manifest must continue to set required keys explicitly.

## 5.1 Operational boundary risk (static)

The runner route marks `pressure-end`, applies one `camera.apply`, then marks
`relief-start` (`run_snapshot_pressure_canary.py:715-719`). `camera.apply` is
processed synchronously by `ProcessPendingInternalTestRequest()` from main-loop
hooks (`war3_internal_test_api.cpp:456-520`;
`war3_hook_lifecycle.cpp:2058,2388,2432`), and the control-plane call waits only
for that request, not for a completed Present. Because
`queryPublishedPerfState()` returns the last completed history row, both markers
can observe the same `businessFrameSerial` and `perfFrameEpoch` if no Present is
archived between them. The analyzer currently requires strict increases
(`analyze_night_pressure_recovery.py:546-551`) and would fail such a run.

This is an operational boundary risk, not a source-lock defect. It needs runtime
or validation evidence; do not weaken the analyzer without proving the actual
route always crosses a completed Present between these markers.

## 6. Static evidence

New test:
`AutoTest/test_perf_published_frame_binding_static.py`

It guards:
- one locked `queryPublishedPerfState()` body using `m_frameHistory.back()`;
- no `m_lastBusinessFrameSerial` shortcut in that query;
- one hub query call and shared JSON fields in both serializers;
- one aggregate assignment and exactly two reset-helper call sites;
- no epoch-to-1 reuse;
- export snapshot pairing aggregate and epoch, plus summary JSON emission;
- unique env names and frozen runner matrix coverage;
- runner consuming `perf` fields and analyzer `(start,end]` boundaries.

Run command (readonly apart from the two new files):

`py -3 -B AutoTest/test_perf_published_frame_binding_static.py -v`

Expected: 7 tests, exit 0.

## 7. Not proven

- No compiler/Ninja/Meson build.
- No game, GPU, deployment, isolated desktop, or live-site run.
- Mocks/fixtures do not prove source/runtime equivalence.
- `producerSealFrameSerialLast` completed-cut proof remains absent.
- The `War3Events -> monitor` nested lock pair is statically reviewed but not
  stress-tested.