# Issue #6 render-hook unit cache isolation

## Finding

`TryFillUnitIdentityFromUnitPtr` used a 1024-slot thread-local cache keyed only
by the raw `CUnit*`. The cache stores rawcode, building flags and JASS handle.
Warcraft can reuse the same address for a different unit after a map change, so
this fast path could classify a map-B object with map-A identity without doing
a fresh readable-range check.

## Change

Each entry now carries the current `ShadowModelResourceCache` map epoch. A hit
requires both pointer and epoch equality, and every insertion stamps the epoch.
This adds one relaxed/acquire atomic epoch load to an identity lookup while
preserving the existing fixed-size, allocation-free cache.

## Boundary

This is a CPU identity fix only. It does not change GPU lifetime, CSM admission,
caster counts or the Type0 exact CurrentDraw fallback. It remains an offline
candidate pending the Issue #6 cold-B and A-to-B physical matrix.
