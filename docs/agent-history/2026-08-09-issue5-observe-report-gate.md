# Issue #5 non-mutating Observe report gate — 2026-08-09

## Why another gate was required

The terrain and union-consumer observers originally exported only the latest
frame through `runtime_status.json`. An external process polling four times a
second cannot prove 10,000 consecutive Present frames and can miss a single
unsafe prediction between polls. Therefore runtime polling is useful for live
health only; it is not sufficient to admit a culling Consume path.

The existing performance recorder already aggregates the observer counters on
every recorded Present. This change exposes those aggregates through
`read_perf_report` as `shadowCullObserveSummary` and adds the standalone,
read-only analyzer:

```powershell
py AutoTest/analyze_issue5_shadow_observe.py \
  E:\Work\War3\WarVK\Log\war3_perf_report_auto.html \
  --require-admission-ready
```

The analyzer never launches, focuses, reprioritizes or stops Warcraft III.

## Required process configuration

The candidate must be launched from a fresh process with:

```text
DXVK_WAR3_CSM_TERRAIN_BOUNDS_MODE=1
DXVK_WAR3_UNION_CONSUMER_CULL_MODE=1
DXVK_WAR3_PERF_RECORD_AFTER_GAME_START=1
DXVK_WAR3_PERF_HISTORY_FRAMES=12000
DXVK_WAR3_PERF_AUTO_EXPORT_SEC=120
```

Mode `1` is Observe. It computes predictions but does not alter output. Mode
`2` is not authorized by this work.

## Admission contract

The report is eligible for later engineering review only when all of the
following are true:

- both observers report mode 1;
- at least 10,000 Presents and 10,000 union-observer frames were recorded;
- union candidates are non-zero;
- false negatives are zero;
- terrain/object applied-cull counts are zero, proving Observe did not mutate
  output;
- incomplete and budget-exceeded frames are zero.

The report also calculates terrain C2/C3 would-cull rate and the percentage of
eligible static/rigid candidates outside both far cascades. These numbers are
opportunity measurements, not permission to enable Consume.

## Current evidence boundary

The old 2026-08-08 report contains only 3,600 frames and both modes were Off,
so it correctly fails this gate. No game was launched and no DLL was deployed
for this stage. A physical Observe run remains required.
