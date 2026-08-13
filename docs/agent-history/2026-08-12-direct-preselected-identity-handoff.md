# Direct preselected identity handoff candidate

## Problem

Object-grouped DirectGrouped preselection already resolves each selected
record's canonical selection identity, including the bounded
`VisibleRenderableRegistry` lookup. BuildEligible then discarded that scalar
result and repeated the same lookup/hash for every selected record when writing
`recordSelectionKey`.

## Change

- The existing record-index scratch now has an aligned `uint64_t` selection-key
  scratch.
- Group selection appends the already computed key beside its snapshot index.
- BuildEligible consumes that exact scalar for `recordSelectionKey` instead of
  querying the registry a second time.
- The uncapped/non-grouped fallback stores zero and retains historical
  on-demand key resolution.
- Compact WorkTable Consume behavior is unchanged: a sealed work item remains
  authoritative only under its existing gate.

## Boundary

The handoff contains no pointers, records, packets or GPU resources and is
cleared at the start of every call. It does not merge objects, change grouping,
enable an experimental mode or alter submission identity. The candidate is not
deployed and still requires fixed-scene A/B measurement.
