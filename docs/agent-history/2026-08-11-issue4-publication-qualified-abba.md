# Issue #4 publication-qualified shadow A-B-B-A

Date: 2026-08-11

This stage corrected a physical-test blind spot rather than changing the
Release shadow algorithm. A locked-sun command is not sufficient proof that a
new directional CSM was published. Paused or producer-incomplete scenes can
keep sampling an older complete map while screenshots continue to succeed.

`run_shadow_alpha_coverage_abba.py` now keeps simulation live, uses a bounded
camera workload by default, captures a preflight publication serial and waits
for `shadowMapRenderSerial` to increase after every sun step. Full-map vision
and pausing are opt-in. A recorded round is invalid if any step fails to publish
a fresh map. `run_shadow_filter_abba.py` reuses the same contract so alpha and
receiver-filter experiments cannot silently diverge.

Two four-process A-B-B-A gates completed without TDR or publication timeout:

- directional hashed alpha versus hard cutoff: `-0.3153%` edge-toggle p95
  improvement, coverage ratio `0.999980`, edge-length ratio `0.999327`;
- Grid5x5 versus Poisson16 at radius `0.70`: `-5.54%` edge-toggle p95
  improvement, coverage ratio `1.018435`, edge-length ratio `1.094714`.

Neither candidate is eligible for promotion. Hashed alpha remains default Off
and the existing Poisson16 filter remains unchanged. These gates are valid for
candidate rejection because each step proves a new CSM publication; they do not
claim that the metric separates all legitimate sun-driven shadow movement from
perceptual shimmer. Player/front-display visual confirmation remains required.

Artifacts:

- `AutoTest/artifacts/shadow_alpha_coverage_abba/20260811_023543`
- `AutoTest/artifacts/shadow_filter_abba/20260811_023947`

The superseded paused artifact
`AutoTest/artifacts/shadow_alpha_coverage_abba/20260811_020854` must not be used
as physical evidence.
