# War3 Shadow Semantic Cost

This note tracks the cost of keeping shadow semantic tracking alive on the hot
path and defines the questions we need to answer before we can move that work
upstream.

## What we care about

- `semanticBridgeHit / semanticBridgeMiss / semanticBridgeBypassed`
- main-thread CPU cost added by shadow-specific object tracking
- how much of the current draw-time bridge can be replaced by model or pose
  identity

## Boundary

This is research-only scaffolding. It does not change the current shadow
capture path. The purpose is to keep the cost model and replacement candidates
visible while we move toward a lighter contract.

## Milestone 1 baseline

The current long-term baseline is gathered through named AutoTest scenarios.

- `low_pressure_static_reuse`
  - expected to show static persistent reuse and low semantic churn
- `dynamic_shadow_pressure`
  - expected to show high `fallbackDrawCount` and large semantic bridge totals
- `semantic_cost_probe`
  - reserved for future profile-heavy runs once more upstream hooks are in

At this stage, the most important counters are:

- `semanticBridgeHit`
- `semanticBridgeMiss`
- `semanticBridgeBypassed`
- `fallbackDrawCount`

These are now exported through both `shadowBudgetSummary` and
`shadowRuntimeV2Summary`, so future reverse-engineering work can compare old
and new contracts without changing the report readers again.

## Restarted baseline evidence (2026-04-03)

After the Docker/WSL memory incident, the baseline loop was restarted and the
named scenarios were re-run successfully.

- low pressure static reuse
  - report: `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_03_20_42_30.html`
  - `staticPersistentCount=40365`
  - `fallbackDrawCount=130333`
  - `semanticBridgeHit=136500`
  - `semanticBridgeMiss=0`
  - `semanticBridgeBypassed=183218`
- dynamic shadow pressure
  - report: `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_03_20_43_52.html`
  - `staticPersistentCount=83590`
  - `fallbackDrawCount=176033`
  - `semanticBridgeHit=247132`
  - `semanticBridgeMiss=0`
  - `semanticBridgeBypassed=94788`

Interpretation for the roadmap:

- the reporting chain is now stable enough for unattended runs
- semantic bridge traffic is large enough that we should treat hot-path object
  identity as a first-class optimization target
- static reuse is real, but dynamic units still pay the bridge + fallback cost,
  so the next meaningful win still depends on moving to model/pose level
  contracts

## Source-level hotspot map (main-thread audit, 2026-04-03)

The current high-cost shadow semantic bridge is not concentrated in one place;
it is the combination of several always-on paths:

1. `war3_hook_render.cpp`
   - `Hook_WorldObjects_RenderGroup`
   - once `NeedsShadowObjectIdentity()` is true, `needCollectObjects` becomes
     true for world groups, so the collector runs broadly
2. `war3_scene_collector.cpp`
   - `CollectWorldObjects`
   - `shadowLiteTracking` disables the normal tracked-handle filter and keeps
     group `0/1/2` object collection alive for shadow identity purposes
   - this is where we still pay list iteration, handle recovery, pointer-map
     reconstruction, and registry feeding
3. `war3_render_exec_batch.cpp`
   - `ExecBatchProcessor::Begin`
   - every relevant draw can still perform:
     - cached-object lookups
     - `RenderObjectRegistry` entry / scene-node lookups
     - TLS semantic packing
     - handle normalization / fallback recovery
4. `d3d9_device.cpp`
   - draw-time shadow capture still consumes the semantic context and falls
     back to freeze classification when no higher-level contract is present

### Current largest cost centers

From the code path shape, the most expensive bridge components are likely:

1. `SceneCollector::CollectWorldObjects`
   - full list scans for world groups
   - tracked-handle snapshots and pointer-map rebuilds
   - per-entry handle resolution / unit pointer inference
2. `ExecBatchProcessor::Begin`
   - repeated registry / cache probing at draw time
   - rebuilding TLS semantic context for large numbers of dynamic draws
3. `NeedsShadowObjectIdentity()` as a broad switch
   - today it is close to "turn on world semantic collection", which is too
     wide for the long-term design

### Replacement direction

The long-term replacement should be:

- higher-level object/model-instance registration upstream
- draw-time TLS bridge only as fallback
- SceneCollector full scans narrowed to:
  - outline/bloom/path-blocker needs
  - shadow fallback-only emergency recovery
