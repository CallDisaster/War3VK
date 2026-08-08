# Issue #6 render-identity cache reset — 2026-08-09

## Deterministic stale aliases found

Three hot caches retained positive results across map sessions while their keys
were raw Warcraft addresses or reusable JASS handles:

1. render-object `CUnit*/jHandle -> rawcode/flags/kind` and handle-to-unit
   maps;
2. the model-hook TLS set of pointers previously validated as runtime models;
3. the semantic shadow-core TLS `renderablePart -> palette slot` table.

Periodic capacity eviction does not protect the first frame of map B. Warcraft
may immediately reuse a map-A pointer or handle, allowing the old type or
palette slot to be accepted without reading the new object.

## Fix

- `War3Renderer::ResetMapSession()` now clears all process-global render-object
  identity aliases and the current-batch TLS context before publishing the
  empty map-transition frame.
- `model::ResetMapSession()` advances a validation generation. Each hook thread
  clears its positive runtime-model TLS set before its first lookup in the new
  session.
- `ShadowValidationRuntime::reset()` advances an independent palette-cache
  generation. Each participating thread clears its renderable-part palette
  table before scanning for a hit.

The generation check occurs before every positive-hit path. The reset does not
uninstall MinHook detours, destroy hooks, or reuse old publications.

No game was launched and no DLL was deployed for this stage. A physical
`A -> B -> A` census remains required before Issue #6 can be closed.
