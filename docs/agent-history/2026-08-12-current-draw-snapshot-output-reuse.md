# CurrentDraw snapshot caller-owned output candidate

## Problem

DirectGrouped rebuilt a fresh `std::vector<CurrentDrawContractRecord>` for every
semantic frame. The records are non-owning identity and generation values, but
the vector capacity itself was discarded after each call. This allocation sits
before the approximately 0.5 ms/frame BuildEligible path and is independent of
all experimental Consume modes.

## Change

- Added a caller-owned output overload for
  `SnapshotPublishedCurrentDrawContracts` while preserving the existing
  value-returning API as a wrapper.
- The output overload clears and rebuilds every logical record using the same
  snapshot policy, ordering, pruning and bounded top-K behavior.
- DirectGrouped reuses only the vector allocation on its render thread.
- A scope-exit guard clears the logical records on every return path, so no raw
  scene identity remains cached between calls; only empty vector capacity is
  retained.
- `CurrentDrawContractRecord` remains a non-owning POD-style value and the
  scratch contains no packet, `Rc`, `shared_ptr`, palette, GPU resource or
  publication authority.

## Boundary

This candidate does not change caster eligibility, order, alpha/blocker policy,
map/device generation, replay or GPU lifetime. It does not enable WorkTable,
Claim, Persistent Package or culling Consume. Offline tests and a DLL build can
only prove source equivalence and lifecycle boundaries; a fixed-scene A/B is
still required before assigning a frame-time gain.
