# Issue #5 terrain cascade bounds observer — 2026-08-09

## Why this stage exists

The current high-pressure trace submits every terrain caster to all four CSM
cascades. In the sampled frame, 122 terrain casters contributed to 189 total
casters and produced 756 cascade draws. The same frame moved about 67.4 MiB
through the exact shadow Arena. Arena capacity was not the immediate limit;
terrain work was duplicated before any authoritative cascade rejection was
allowed.

Older terrain culling code was controlled by a boolean and did not expose why
a bound was trusted. This stage replaces that implicit contract with an
explicit `Off / Observe / Consume` mode and a pure bounds-evidence policy.

## Safety contract

- The default remains `Off`, so this commit does not change rendered output.
- `Observe` computes the exact final-replay decision but never changes the
  cascade visibility mask.
- `Consume` is intentionally limited to terrain C2/C3. C0/C1 remain visible.
- Only bounds computed from a validated, CPU-readable, current-frame terrain
  position span with a non-zero content generation can authorize rejection.
- Guessed scene-node spheres, stale generations, skinned or animated sources,
  non-finite values and invalid radii remain fail-visible.
- Volume-sun rendering forces this experiment off.

The receiver reports candidates, accepted proofs, fail-visible decisions and
per-cascade would-cull counts through runtime status and performance JSON.
This stage acts only at final CSM replay; it does not yet reduce capture,
freeze or Arena traffic. Moving the decision earlier requires a separately
proven frame/resource-stamped visibility snapshot because current CSM matrices
are created after capture.

## Verification

- 68/68 `AutoTest/test_*_static.py` modules passed.
- 16/16 Win32 Meson runnable tests passed.
- Win32 DLL build passed; `ninja -C build32 -n` reported no work.
- `git diff --check` passed.
- Candidate DLL: 33,868,311 bytes.
- Candidate SHA-256:
  `6C87405CE92AF34481A3ABC85EA85C4D8D21F56C625345637AFDB8F034CB57C4`.

No physical game result is claimed. The isolated-desktop runner was already
quarantined after a prior failure and correctly refused to launch; it did not
fall back to the visible desktop. The candidate was not deployed. Observe and
Consume still require a real runtime A/B before Consume can become a default.
