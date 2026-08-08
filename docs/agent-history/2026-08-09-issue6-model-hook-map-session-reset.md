# Issue #6 model-hook map-session reset

## Finding

The Present-owned map transition called `model::Shutdown()`. That function
marked the bootstrap/full model hooks as uninstalled and inactive even though
their MinHook detours remained installed and the next map did not run a normal
full reinstall. At the same time, several producer caches survived the map:

- the 65,536-entry blended-palette slot table;
- renderable-part to palette/runtime-model bindings and palette snapshots;
- the cached Game.dll global palette-buffer address;
- runtime-model to pose-array ranges;
- attachment child/parent runtime links and palette-tree dedupe state.

These stores contain raw Warcraft pointers, palette slots or game frame tags.
The next map can reuse all three domains, so a frame-age check cannot prove that
an entry belongs to the new map.

## Change

`model::ResetMapSession()` is now separate from process-level `Shutdown()`.
The Present transaction mints and installs the new model-resource map epoch,
then invalidates the raw-pointer producer caches without changing hook install
or active state. A newly created D3D9 owner performs the same cache reset.

The reset uses the cache publication words (`valid` and `renderablePart`) to
invalidate the large inline palette arrays instead of rewriting their matrix
payloads. The pose-range and parent-link maps are cleared under their existing
locks. `Shutdown()` still performs the map reset before disabling process-level
state.

## Validation boundary

The source contract, Win32 build and runnable suites validate ordering and the
separation between map reset and process shutdown. This candidate is not
deployed and Issue #6 is not declared fixed until cold-B, A-to-B and A-to-B-to-A
physical tests show equal steady-state performance and no stale publications.
