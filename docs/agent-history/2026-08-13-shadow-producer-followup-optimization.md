# 2026-08-13 Shadow producer follow-up optimization

## Evidence from the 09:27 report

The production-snapshot candidate reduced the former producer bottleneck while
preserving the final shadow workload:

- Populate: 0.833 -> 0.303 ms/frame (-63.6%).
- DirectGrouped: 0.691 -> 0.146 ms/frame (-78.9%).
- BuildEligible: 0.505 -> 0.032 ms/frame (-93.7%).
- Only 89 of 296,644 current-draw records built a new direct packet; the rest
  were already owned by the exact Stage11 producer.
- The report contains no producer-incomplete frame, required-caster omission,
  replay rejection, Arena overflow/partial publication, or identity-set
  mismatch.

This is not a same-camera A-B-B-A result. It proves that duplicate producer
work was removed, but it does not by itself prove an end-to-end FPS gain.

## Follow-up changes

1. Release builds no longer execute development-only S1 generation-proof
   observation or Stage13 retention hashes when their routes are disabled.
2. The exact draw-time cache producer now visits a frame-local value-key active
   ledger rather than scanning every inactive cache node. Canonical map lookup
   and all freshness/identity checks remain authoritative; ordinal exhaustion
   falls back to the old full scan.
3. Run builds and resolves one immutable replay snapshot. Directional CSM,
   volume-sun, and point-shadow consumers borrow that snapshot instead of
   copying the roughly 1.4 KiB draw record array again. Each consumer retains
   its own pre-clear producer/replay validation.
4. Fallback live-category counters are updated once per append. The previous
   legacy path rescanned the growing fallback vector after every append: 63
   fallbacks per frame caused 2,016 classification visits. The only erase path
   still performs one full rebuild.
5. An empty S1 early cache no longer computes a key/source fingerprint, and a
   key miss no longer scans every active VB stream. A hit still validates the
   identical fingerprint before reuse and still evicts aliases fail-closed.

## Remaining measured bottlenecks

- ShadowCapture/Gates: 0.507 ms/frame, including DrawTimeCapture 0.188 ms.
- ShadowCapture/PostGate: 0.235 ms/frame.
- Directional ShadowMap: 0.346 ms/frame for about 656 main CSM draws plus the
  terrain caster-mask draws.

Receiver and Shadow/Main scopes overlap; they must not be added. The actual
fullscreen receiver draw is only about 0.014 ms. The remaining ShadowMap CPU
work is primarily per-draw command recording, so a material improvement now
requires proved early culling or strict batching rather than descriptor/UBO
micro-caching.

The source already contains sampled Gate/PostGate/DirectionalShadowMap phase
breakdowns. The next data run should use a clean development observer build;
all Consume modes remain Off. That observer run is attribution evidence only,
not a Release FPS result.

## Validation boundary

All focused static contracts and Win32 value runnables passed, the Win32 D3D9
DLL linked with at most two build jobs, and the exact DLL target reports
no-work. No DLL was deployed and no game/foreground visual or same-camera
A-B-B-A run was performed. Visual stability, long-run TDR behavior, and the
end-to-end frame-time change therefore remain physical acceptance gates.
