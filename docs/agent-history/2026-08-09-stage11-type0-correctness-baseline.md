# Stage11 Transparent Type0 correctness baseline — 2026-08-09

## Scope

This baseline starts from GitHub `origin/main` commit
`f6529c6d3707f048da0c47eb1ca9942d05f25fb1` and isolates the construction-
attachment repair on branch `codex/stage11-exact-attachment-fallback-20260809`.
The older WIP tree remains preserved in the existing stash and is not mixed
into this baseline.

## Root cause

Warcraft III routes construction attachments such as the Undead `UBirth.mdx`
claw through the transparent Type0 native dispatcher rather than the ordinary
Common/Special render-queue dispatchers. The previous shadow contract therefore
had no authoritative child draw scope or normal material layer for this lane.
It compared parent-building semantic identity against child attachment geometry
and rejected otherwise valid Stage11 draws.

`UBirth.mdx` also uses indexed fixed-function vertex blending with
`D3DVBF_0WEIGHTS`. This mode has a matrix-index attribute but no explicit weight
attribute. Replay validation previously treated the missing weight attribute as
an incomplete blend stream and rejected the caster.

## Fix

- Install a resident semantic boundary for Transparent Type0 while keeping
  transparent Types 1–4 as opt-in performance probes.
- Publish the synchronous Type0 child scene node, renderable part and mesh
  payload with an explicit unknown-layer sentinel; never forge layer zero.
- Require an independently proven owner witness before bridging parent semantic
  identity to child geometry.
- Capture the exact same-frame VB/IB/UV and current D3D matrix palette through
  the existing bounded freeze path. Dynamic or unstable data remains owned by
  current-frame Arena/freeze storage.
- Keep the Type0 contract value-only and local to the synchronous draw; it is
  not published as a reusable cross-frame identity.
- Accept indexed zero-weight blending when the bounded matrix-index stream is
  present, while continuing to reject missing or out-of-range blend storage.
- Allow the non-additive depth-writing transparent building attachment case
  without making arbitrary particle or additive effects cast opaque shadows.

The global cross-frame draw-time VB cache, source fingerprint reuse and identity
grace remain disabled. The semantic layer answers *which child draw this is*;
the exact same-frame capture answers *what geometry and pose it uses now*.

## Verification

- 67/67 `AutoTest/test_*_static.py` modules passed.
- 16/16 Win32 Meson runnable tests passed.
- Win32 `d3d9.dll` build passed and `ninja -C build32 -n` reported no work.
- `git diff --check` passed.
- Candidate DLL: 33,852,268 bytes.
- Candidate/deployed SHA-256:
  `1F5B6B2361B0D5133C33AA6B2E564ADE0FE894F5EC5ED2376C989F7F231D37A7`.
- User physical verification: the Undead construction claw shadow no longer
  flickers in game.

Night Elf and other race construction attachments still require explicit
physical regression coverage. This baseline does not claim to fix cross-map
lifecycle behavior or unrelated static-tree/cascade-culling issues.
