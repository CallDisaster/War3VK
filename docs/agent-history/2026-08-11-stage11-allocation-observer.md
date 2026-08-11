# Stage11 allocation-source observer

The development-only shadow observer build now distinguishes why an exact
Stage11 caster requests a new position backing. The observer never allocates,
binds, copies, culls, or changes a canonical draw. Default Release builds keep
the capability compiled out; it is reachable only with
`-Dwarvk_shadow_observers_dev=true` and
`DXVK_WAR3_STAGE11_ALLOC_OBSERVER=1`.

The first hidden-desktop 45 second low-camera run completed without switching
the Windows input desktop. AutoTest restored the previously deployed DLL by
exact SHA-256 after its owned War3 process exited.

Observed totals over 564 Stage11 observer frames:

- 6,519 position-allocation requests;
- 2,061 requests for a newly inserted exact cache key;
- 4,458 retries of an existing entry with no backing;
- zero capacity-growth requests and zero GPU-skin lease detach requests;
- 2,136 static requests and 4,383 dynamic requests;
- 5,040 budget deferrals, of which 3,971 were missing-backing retries;
- 8,970 unique and 3,991 duplicate valid static position proofs;
- zero invalid proofs and zero observer-set overflow.

This rejects the hypothesis that changing the 64 MiB inactive static-cache
target alone can close the flicker. The dominant visible failure mechanism is
burst debt behind the 32-allocation gate: a deferred entry remains empty and
is retried on later producer batches. A correct optimization therefore needs a
separately owned, completion-gated package/page allocator or earlier proven
admission; it must not turn per-entry buffers into aliases without immutable
ownership and consumer last-use fencing.

This run is diagnostic evidence only. It is not player visual acceptance and
does not authorize any culling or persistent-package Consume route.
