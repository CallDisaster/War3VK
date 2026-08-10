# Issue #5 exact terrain bounds provenance candidate — 2026-08-11

## Scope

This candidate changes only the development Observe evidence path for terrain
bounds. The production Meson option remains
`warvk_shadow_observers_dev=false`; Release therefore performs no new bounds
scan and no terrain/object/union culling. Mode `2` is still rejected by the
compile-time observer policy.

The official Unreal Engine `release` source at commit `71fe36aac5a8` was used
only as a read-only architecture reference. Its renderer takes bounds from a
scene-owned primitive proxy and updates the world-space bounds with the scene
transform before visibility work. No Unreal code, shader, data or asset was
copied into WarVK.

## Closed metadata gaps

- Indexed draws no longer use D3D9 `MinVertexIndex/NumVertices` as a bounds
  proof. A pure policy helper admits only the vertex domain scanned from the
  current, validated IB bytes. Non-indexed draws keep their exact contiguous
  draw range.
- Exact index-domain evidence is retained even when the range already spans
  the complete position buffer and the freeze optimizer does not rebase it.
- An S1 persistent miss may compute local bounds only from a generation-
  validated CPU-readable position span and the exact vertex domain. The
  immutable persistent geometry owns that proof under its registry generation.
- An S1 early-cache hit recomputes world bounds from the persistent local bounds
  and the current draw transform, and refreshes the current frame stamp. If the
  immutable proof is absent, the hit explicitly returns to Unknown/fail-visible.
- Stage10/current-frame terrain fallback uses the same exact-domain policy.

This closes provenance only. It does not alter CSM splits, resolution, caster
selection, workload budgets or replay publication, and it does not authorize
Consume.

## Offline validation

- 30/30 targeted Python static contracts passed.
- `war3_terrain_bounds_provenance` and four related Win32 runnables passed.
- Default Release DLL built with observer option false and reached Ninja
  no-work; SHA-256:
  `A5672357D28F6246A293D92A557A4ED73FF8A5B51E6DE6062EB4A0E3FFD298FB`.
- Development Observe DLL built with observer option true and reached Ninja
  no-work; SHA-256:
  `471BDECA210D6E8B5141BB719DE6B082CD8E985BB84687D9356DE32AD10BC9C7`.

## Remaining physical gate

The development DLL must run the visible-desktop 10,000-frame life-and-death
Observe gate. Required evidence is non-zero terrain proof acceptance, zero
false negatives, unchanged authoritative caster/replay counts, no device loss,
and an Observe overhead not exceeding 0.15 ms/frame. A useful Consume candidate
also requires at least 25% proven terrain cascade work reduction. Until those
conditions are measured, Issue #5 remains open and Release remains Off.
