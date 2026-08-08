# Non-terrain CSM bounds authorization candidate (2026-08-09)

## Finding

The final directional CSM culler had two different authorization rules:

- terrain bounds were checked through `War3EvaluateBoundsCullEvidence`;
- non-terrain C2/C3 draws used `boundsCenter` and `boundsRadius` directly.

The building, destructible and attachment paths currently synthesize many of
those spheres from object-kind constants, scene-node locations or root
transforms. They do not publish `ExactLocalGeoset` or `ExactCurrentWorld`
provenance. Those diagnostic bounds therefore could remove a required caster,
including construction attachments and distant static objects.

## Change

The sphere/cascade math is now separated from culling authority. Diagnostic
bounds still run through the old math and contribute `wouldCull` telemetry, but
an actual non-terrain C2/C3 skip requires the same exact, current-generation
proof as terrain. Unknown, diagnostic, animated, skinned, stale or non-finite
bounds fail visible.

New counters expose non-terrain candidate, proof accepted, fail-visible,
would-cull and applied-cull totals in runtime status and performance reports.

This candidate does not restore cross-frame VB/IB fingerprints or caches and
does not alter the Type0 exact CurrentDraw fallback.

## Cost boundary

At present, non-terrain producers do not publish an exact bounds provenance, so
the change intentionally disables their unsafe far-cascade skips. It can
increase C2/C3 GPU work. The safe performance follow-up is generation-backed
geoset/static-package bounds, not re-enabling guessed spheres.

The DLL is an offline correctness candidate only. It was not deployed and no
game process was started. Physical acceptance must compare the confirmed Type0
baseline against this candidate in:

1. near/mid/far UBirth construction animation;
2. trees and building shadows;
3. the low-angle high-pressure `life_and_death` route.

## Offline verification

- 69/69 static test modules passed.
- 17/17 Win32 Meson runnable tests passed.
- Win32 DLL build passed with `-j2`.
- `ninja -C build32 -n` reported no work.
- `git diff --check` passed.
- Candidate SHA-256 before commit:
  `EFE4C1F90AB1CA988F8E226E777F66C23D71C2E0D5973A59192FBF6714B794F3`.
