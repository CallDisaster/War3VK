# Issue #6 widget and scene identity reset

## Finding

Two hot identity caches accepted reusable Warcraft addresses without a map
session boundary:

- the widget hook kept `widgetPtr` and JASS-handle aliases for the lifetime of
  the process;
- `SceneCollector` kept a thread-local `CUnit* -> jHandle` result for up to
  4096 collector calls.

Both values may be reused immediately by the next map. A capacity or periodic
GC therefore cannot protect the first frames after a map transition.

## Change

- `War3Renderer::ResetMapSession`, which is called from the Present-owned map
  transition, now clears the widget pointer and handle indexes while retaining
  the installed hook.
- The scene collector TLS lookup records the current shadow model map epoch and
  clears itself before the first lookup in a different epoch.

No identity grace, pointer fingerprint, cross-frame geometry cache, replay
validator or public JAPI behavior changed.

## Evidence boundary

Static contracts and the Win32 build prove the invalidation is wired into the
correct reset owner. A physical cold-B versus A-to-B-to-A run is still required
to prove the cross-map performance and visual issue closed.
