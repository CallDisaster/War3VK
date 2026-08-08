# Issue #6 map identity isolation — 2026-08-09

## Investigation result

The render-owned Arena quarantine, completion fence, retired shadow sessions,
receiver invalidation and point-shadow worker drain were already present. The
first proven cross-map gaps were CPU identity state outside those resource
owners:

- shadow lifecycle tombstones were process-global and keyed by raw Warcraft
  pointers or JASS handles, without a map epoch;
- device fallback state retained the previous camera, per-draw upload and
  cached world render targets across the map reset safe point.

An object address reused by map B could therefore still match a removal event
from map A. The previous camera could also authorize one stale fallback frame
while the new map was establishing its first camera publication.

## Implemented contract

- Every object-identity tombstone is stamped with the current non-zero map
  epoch. Reset advances the domain and clears active raw identities.
- Draining, querying and acknowledging a tombstone all reject a different map
  epoch.
- A producer-stage disabled policy remains process-scoped. Changing maps must
  not silently turn a user-disabled producer back on.
- The receiver begins a new epoch at the current tombstone serial and policy
  revision, so it cannot replay old history into the new map.
- The Present-owned session reset clears the old camera, per-draw upload,
  cached RT/DS and previous semantic pointer identities.
- GPU resources are still retired through their existing completion-fence
  queues; this change does not replace fence ownership with a CPU clear.

## Verification

- The new pure Win32 lifecycle runnable proves A-to-B pointer reuse rejection,
  current-epoch removal/acknowledgement and stage-policy persistence.
- 68/68 static test modules passed.
- 17/17 Win32 Meson runnable tests passed.
- Win32 DLL build passed; `ninja -C build32 -n` reported no work.
- `git diff --check` passed.
- Candidate DLL: 33,868,305 bytes.
- Candidate SHA-256:
  `85F65175C292C510035704AA1C3D57A4AA124AF785396E44E20536EB3DDD11D3`.

This is a bounded lifecycle fix, not a claim that Issue #6 is fully resolved.
The candidate was not deployed and no cold-B / A-to-B / A-to-B-to-A physical
comparison was performed. Persistent owners, retirement residency and first
complete CSM publication still require the planned cross-map census and user
physical acceptance.
