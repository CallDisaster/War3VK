# 2026-08-09 integrated night checkpoint

## Correctness line

- User-confirmed Type0/UBirth source baseline: `b785a15`.
- Current integrated source baseline: `203932c`.
- Tag: `codex/integrated-correctness-baseline-20260809`.
- build32 DLL SHA-256:
  `56566E0418E2C51AE22C7978E5934AC45ACDB32191F7E02E15C83EBB3FAF8190`.
- Backup:
  `E:\Work\War3\WarVK\Backup\d3d9_integrated_correctness_203932c_56566E04.dll`.

The integrated line contains the user-confirmed same-frame Type0 source
recovery and the point-shadow receiver-bias shader that was physically verified
on sibling commit `778491d`. The integrated DLL itself has not yet received a
combined physical regression run.

## Issue #5

Terrain and generation-backed object bounds remain Off/Observe by default.
`AutoTest/analyze_issue5_shadow_observe.py` rejects all existing reports because
they were recorded with both modes Off and fewer than the required 10,000
Observe frames. No Consume path was enabled.

## Issue #6

The Present-owned map transition now invalidates the remaining proven CPU alias
sources found in the audit:

- render-object handle/rawcode/unit metadata;
- runtime-model positive validation TLS;
- renderable-part to palette-slot TLS;
- widget pointer/JASS-handle identity indexes;
- SceneCollector CUnit-to-handle TLS;
- model hooks use map-session reset without pretending the installed detours
  were unloaded.

Together with earlier epoch, registry, hot-cache and retired-session work, this
closes the deterministic source-level stale-alias findings. Cold-B, A-to-B and
A-to-B-to-A physical census is still required.

## Issue #7 and ReBAR

The point receiver no longer mixes exact per-tap receiver-plane depth with
centre-depth fallback inside one PCF kernel. Unproven planes use a bounded slope
fallback. The current adapter exposes a 15.73 GiB non-host-visible primary VRAM
heap and only a 214 MiB host-visible device-local heap, so the planned full-ReBAR
direct uploader is ineligible and was not implemented.

## Verification

- 73/73 static test scripts passed.
- 18/18 Win32 Meson runnable tests passed.
- Win32 DLL and generated receiver shader built with `-j2`.
- `ninja -C build32 -n` reported no work.
- `git diff --check` passed before each code checkpoint.

No DLL was deployed, no game was launched, and nothing was pushed to GitHub.
