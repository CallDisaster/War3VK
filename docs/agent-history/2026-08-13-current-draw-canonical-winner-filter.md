# CurrentDraw canonical winner filtering candidate

## Evidence

The 2026-08-13 09:27 performance report contains 296,644 canonical
CurrentDraw records over 3,081 frames. Exact Stage11 ownership rejected
296,555 of them before packet construction, while only 89 packets were built.
`SnapshotPreselect` still cost 0.073 ms/frame because those records were first
copied into the value snapshot and ordered before the existing owner gate.

## Candidate

The Release bounded local snapshot now retains TLS cache slot indexes while it
selects the same exact-dedupe winner and applies the same canonical order. Only
after that winner prefix is fixed does a synchronous, non-owning callback
discard records already owned or rejected by the exact Stage11 producer.
Survivors alone are materialized with `SnapshotRecordWithGrace`.

The filter is unavailable to global publication, online top-K, development
observer and full pose-trace paths. A rejected winner cannot revive a weaker
duplicate or backfill a lower-priority record. Existing raw-record and
exact-owner-skip counters retain their prefilter meaning. If a full trace is
armed concurrently with a filtered snapshot, that racing CurrentDraw trace
frame is omitted rather than mislabeled as complete; the following frame uses
the unfiltered path.

## Validation boundary

Focused CurrentDraw, Stage11, replay, cross-map, final-caster and release
static contracts passed, as did six related Win32 runnables. The Win32 DLL was
built with at most two compiler jobs and reached an exact-target no-work state.
No DLL was deployed and no Warcraft process was started.

This is a CPU snapshot-copy candidate, not culling or a reduction in final
casters. Its measured upper bound is the old 0.073 ms/frame scope; the expected
gain is smaller and must be established with a fixed-scene A-B-B-A run. Offline
equivalence tests do not establish player-visible correctness or performance.
