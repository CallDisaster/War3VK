# Issue #5 visible-desktop Observe baseline — 2026-08-11

## Scope

This record covers the development-only terrain-bounds and union-consumer
Observe build.  The Release build policy remains `Off`, mode `2` remains
unreachable, and no caster was removed by these runs.

The physical test used the visible desktop, the original life-and-death map
copied to the AutoTest short path, 4096 CSM, DirectInline, a 120-second spawn
hold and a low-angle 5x5 patrol.  The test did not use an isolated desktop.

## Report schema correction

Current performance reports place the live per-frame culling counters under
`shadowRuntimeV2Summary`.  The analyzer only read the historical top-level
schema and could therefore report a live Observe run as `Off`.  The analyzer
now prefers the bounded current schema, falls back to `shadowBudgetSummary`,
and retains compatibility with the old top-level fields.

## Physical evidence

The 360-second run produced 10,787 report frames and 9,519 union Observe
frames.  It completed without a Vulkan device-lost incident or a new Windows
GPU event.  The report showed:

- terrain: 2,037,882 candidates, 0 accepted bounds proofs, 0 would-cull;
- object: 1,745,554 candidates, 0 accepted bounds proofs, 0 would-cull;
- union: 19,502 candidates, 39,004 per-cascade proofs, 0 would-cull;
- 1,269 producer-incomplete frames and 1,055 budget-exceeded frames;
- average frame time 33.450 ms and average measured GPU time 1.828 ms.

The run is therefore not a Consume admission.  Extending it mechanically to
10,000 union frames would not create an optimization opportunity: the final
terrain/object replay currently receives no authoritative bounds proof, and
the producer correctness gate is already open on a material number of frames.

## CPU attribution

The same report keeps the consumer GPU cost small relative to the frame:
`Shadow/Main` was about 1.664 ms GPU and the aggregate GPU time was about
1.828 ms.  The largest measured CPU costs were Warcraft's native world-frame
preparation (about 16.174 ms self), native sorted-item flush (about 3.532 ms
self), semantic Populate (about 2.737 ms inclusive), DirectGrouped (about
1.595 ms), BuildEligible (about 1.112 ms), and ShadowCapture/Gates (about
1.072 ms).

This is structural attribution, not a WarVK-vs-original performance delta.
A matching Release-Off A/B is still required before assigning all native or
producer time to WarVK.

## Next gate

Before changing culling behavior, add Observe-only rejection provenance for
the existing authoritative bounds policy.  It must distinguish missing or
diagnostic provenance, source generation, frame stamp, identity proof,
dynamic/skinned/animated sources and non-finite/invalid bounds.  Only a
specific, generation- and epoch-safe metadata gap demonstrated by that
histogram may be repaired.  Consume remains disabled.

