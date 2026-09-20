# 2026-09-20 Stage11 census retention dimensions (memory-b02)

Status: offline prototype changes only.  No compiler/game lease, no Ninja,
no deployment, no `device.cpp/.h` edit in this lane.  The parent owns the
production wiring described below.

## 1. What changed

`src/d3d9/war3/tools/war3_stage11_budget_census.h` writer now emits schema 2.
Schema 1 artifacts are frozen and untouched.  The strict reader
`AutoTest/analyze_stage11_budget_census.py` accepts both:

- schema 1: old fields only; the new retention dimensions are reported as
  `None` and `tagDimensionsAvailable=false`, never as zero occupancy.
- schema 2: exact new fields below, with closure and cross-stat checks.

Owner categories (`touched/failed/recentCache/coldCache/retired`) keep their
old meaning.  The new `RangeTags` is independent of owner priority:

```cpp
struct RangeTags {
  bool staticTag;                 // mandatory bool; false = not-static/unknown
  TagState failedRetained;        // Unknown/False/True
  TagState touchedThisFrame;      // Unknown/False/True
};
```

`isStaticGeometry=false` is only a non-static/unknown tag.  It is never called
"dynamic" and never proves a short lifetime.  If the failure or touched label
is not available, the caller must use `TagState::Unknown` (or
`RangeTags::AcknowledgedUnknownAux`), which is exported as unknown bytes.

## 2. Schema 2 page fields

Disjoint static-tag partition:

| Field | Meaning |
| --- | --- |
| `staticTagOnlyBytes` | covered only by static-tagged references |
| `notStaticTagOnlyBytes` | covered only by non-static/unknown-tagged references |
| `mixedTagOverlapBytes` | overlapping byte ranges covered by both static and non-static references; this is not a page-level mixed flag |
| `staticTaggedUnionBytes` | `staticTagOnlyBytes + mixedTagOverlapBytes` |
| `notStaticTaggedUnionBytes` | `notStaticTagOnlyBytes + mixedTagOverlapBytes` |

Closure: `staticTagOnlyBytes + notStaticTagOnlyBytes + mixedTagOverlapBytes`
equals the old owner-union total exactly.  Alias/overlap is deduplicated by the
same endpoint sweep that already computes `owned[Owner]`.

`mixedTagOverlapBytes` must not be inferred from page membership.  A page with
disjoint static and non-static ranges reports `mixedTagOverlapBytes == 0` while
both `staticTaggedUnionBytes` and `notStaticTaggedUnionBytes` are non-zero.
Whole-page mixing must inspect both union totals.

Cross-statistics over the same intervals:

| Field | Meaning |
| --- | --- |
| `failedRetainedUnionBytes` | any reference with failure tag true |
| `touchedUnionBytes` | any reference with touched tag true |
| `touchedAndFailedUnionBytes` | intersection of the two true sets |
| `touchedOrFailedUnionBytes` | union of the two true sets |
| `tagUnknownUnionBytes` | any reference with failure or touched tag unknown |
| `tagUnknownReferences` | number of references with any unknown aux tag |

Reader checks include `failedUnion <= owned`, `touchedAndFailed <= failed`,
`touchedAndFailed <= touched`, `touchedOrFailed <= owned`,
`touchedOrFailed + touchedAndFailed == failed + touched`,
`tagUnknownUnion <= owned`, and reference-count bounds.  `touchedOrFailed` is
the explicit union; `touchedAndFailed` is the intersection.  They are cross
statistics and are not added into the owner partition.

## 3. Parent device patch (not applied)

The parent's inspect loop should construct one explicit tag value per stream
span and pass it to the new `range` signature:

```cpp
const bool touchedThisFrame =
    entry.lastAttemptFrameSerial == frame ||
    entry.lastAccessFrameSerial == frame;
const dxvk::war3::stage11_census::RangeTags tags =
    dxvk::war3::stage11_census::RangeTags::Known(
        entry.isStaticGeometry,
        entry.captureComplete ? dxvk::war3::stage11_census::TagState::False
                              : dxvk::war3::stage11_census::TagState::True,
        touchedThisFrame ? dxvk::war3::stage11_census::TagState::True
                         : dxvk::war3::stage11_census::TagState::False);
collector->range(pi, offset, capacity, ownerClass, tags, age);
```

The current owner priority calculation is unchanged.  If a caller cannot
provide one of the aux labels, it must pass `TagState::Unknown`, not false.
Old test callers must be updated the same way or use
`RangeTags::AcknowledgedUnknownAux`.

No device.cpp/.h edit is made by memory-b02; the patch above is a precise
hand-off point for the parent.

## 4. Bounds and cost

- Limits remain `MaxPages=128`, `MaxSlices=32768`, `MaxEntries=16384`, 4 cuts.
- `m_points` is still a fixed `2 * MaxSlices` array; the sweep still has no
  per-entry heap allocation, no unbounded hash table, no GPU readback or copy.
- `Collector` remains covered by the existing fixed `static_assert` less than
  4 MiB.  `History` copies are still bounded at 4 samples.
- The new work is integer count maintenance at existing endpoints.
  No measured p95/p99 or player-visible claim is made.

## 5. Verification plan

1. validation lane compiles `AutoTest/test_stage11_budget_census.cpp` and runs
   `--json`, then the real Writer -> JSON -> production reader roundtrip in
   `AutoTest/test_stage11_budget_census_static.py`.
2. `AutoTest/analyze_stage11_budget_census.py` negative tests cover duplicate
   keys, NaN, schema mismatch, alias/tag closure, cross-stat bounds, and old
   schema 1 dimension-unavailable output.
3. Parent/validation review the device inspect patch above before any
   production census is used for P3 decisions.
4. No shadow recovery, memory saving, or GPU ownership claim is made by this
   document.

## 6. memory-b03 correction and fixtures

- The b02 sweep had an inversion: `Known False` entered the unknown count and
  `Unknown` was not counted.  b03 corrects both failed and touched branches;
  `Known False` adds neither true nor unknown, while `Unknown` adds unknown.
- `test_stage11_budget_census.cpp` now has an explicit 9-way aux tri-state
  combination test, known+unknown overlap/alias test, and a fixed-seed 256 B
  bitmap oracle covering owner priority, static partition, failed/touched true
  sets, intersection/union and unknown union.
- The strict reader now rejects boolean schema, out-of-range first stage,
  zero page capacity, missing schema 2 device identity, and slice/reference
  closure mismatches.  Summary dimensions are scoped as active, retired and
  all, with separate unknown ratios; `cacheReferencedCapacity` remains the
  active-only legacy field and `retainedPageCapacity` remains all pages.
- The C++ fixture writes non-zero device/map/device-epoch identity and a strict
  writer -> JSON -> reader roundtrip remains a validation-lane gate.
- `test_stage11_lifetime_page_policy_static.py` carries a fixed-seed synthetic
  allocation/retention/release model for six scenarios, with the same external
  release events for grouped and mixed-tail strategies.  It tracks active CPU
  page capacity separately from peak physical backing, and checks no hole
  reuse and no early page reclaim.  This is synthetic, not a player result.
- `test_stage11_lifetime_page_policy.cpp` has a cost fixture for 32 pages and
  prints `policyPlanLoopUs`, `policySelected` and the 496 pair-check upper
  bound.  `test_stage11_budget_census.cpp` already prints `sizeof(Collector)`,
  `sizeof(History)`, max alias sweep CPU time and checks no heap allocations.
  The create gate being closed still executes validation and scan branches; it
  is not a zero-instruction mode.

## 7. memory-b06 exact UV resolver and model-only anchor example

The parent census call site uses `War3ResolveDrawTimeUvCensusSpan` from
`src/d3d9/war3/render/war3_draw_time_snapshot_lifetime.h`.  A shared-UV alias
requires exact page-pointer and offset identity, then measures
`positionCapacity`; zero or stale `uvCapacity` is representation noise for the
alias.  The collector's byte union deduplicates the alias while reference
counters keep the logical UV binding.  Inconsistent alias state is rejected
through `InvalidRange`, not silently dropped.

The synthetic long-anchor grouping example is model-only.  Its lower
active-CPU page capacity does not include simultaneous CS/GPU-in-flight short
slices or retired physical backing, so it is not a measured physical-memory or
runtime gain and does not authorize P3.