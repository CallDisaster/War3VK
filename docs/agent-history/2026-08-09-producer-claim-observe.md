# Producer Claim Ledger Observe — 2026-08-09

## Why this experiment exists

The 2026-08-08 performance report recorded 273,668 DirectGrouped packet builds
over 3,600 frames. Draw-time current-frame geometry submitted 396,028 exact
records, while all other semantic producers together added only 508 records.
DirectGrouped averaged 1.522 ms and its BuildEligible phase averaged 1.199 ms.
This makes duplicate control-plane work a high-value target, but it does not
prove that a reduced identity can safely skip packet construction.

## Implemented boundary

- `DXVK_WAR3_SEMANTIC_PRODUCER_CLAIM_LEDGER=0` remains the release default.
- Mode 1 builds same-frame prediction keys and compares them with the existing
  post-build exact-owner decision. It never changes caster selection.
- Mode 2 is deliberately denied and behaves as Observe. It increments a
  dedicated denial counter and never skips packet construction.
- The key includes non-zero map/device epoch and frame serial, stable object
  identity, renderable part, mesh payload, layer and the known payload word.
  The strict variant also includes `payloadWord11C`; the logical variant omits
  that known draw-local field so real-map evidence can determine whether it is
  identity or churn.
- Raw pointers are used only inside this same-frame, multi-field diagnostic
  key. They do not authorize cross-frame reuse, cache publication or replay.
- The predictor does not use `VisibleRenderableRegistry`'s retained winner,
  because one renderable part may be shared by multiple instances.

The reduced key intentionally lacks a proven immutable source generation,
material/alpha identity and consumer mask before packet construction. It is
therefore not an Enforce key. Exact-rejected current-frame owners are also not
inserted in this first submitted-record catalog, so false negatives can expose
that missing claim source instead of silently treating the predictor as
complete.

## Closed diagnostics

Both performance-report summaries now export:

- mode, exact key count, candidates, canonical owners, missing keys and
  unresolved packet builds;
- strict and logical predictions, matches, false positives and false
  negatives;
- denied Consume requests.

The previous exact-producer visible/fresh/claimed/submitted and
DirectGrouped-owned skip counters remain available in the same summaries.

## Verification and next gate

- 69/69 static test modules passed.
- 17/17 Win32 Meson runnable tests passed.
- Win32 DLL build passed after regenerating the affected objects with one
  Ninja process; `ninja -C build32 -n` reported no work.
- `git diff --check` passed.

This candidate is not deployed and no performance gain is claimed. The next
physical run must enable mode 1 for at least 10,000 frames and prove zero false
positives, characterize false negatives and keep Observe overhead below
0.15 ms/frame. Enforce remains forbidden until source/material/alpha identity
is available before the expensive build and the selected key reaches zero
mismatch on the target maps.
