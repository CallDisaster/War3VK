# Directional CSM compare-first PCF candidate

## Scope

This candidate addresses receiver-side directional-shadow edge crawl only. It
does not change CSM resolution, caster production, Arena ownership, point-shadow
filtering, cross-map lifecycle, culling or TAA history reprojection.

## Source changes

- Split directional-shadow sampling into raw-nearest and depth-comparison
  samplers. PCSS blocker search, caster mask, diagnostics, volumetric raw reads
  and point shadows remain on raw-nearest.
- Use `sampler2DArrayShadow` with `VK_COMPARE_OP_LESS_OR_EQUAL` for CSM PCF.
- Query `VK_FORMAT_D32_SFLOAT` linear-filter support. Unsupported devices use a
  manual compare-first 2x2 bilinear fallback instead of filtering raw depth.
- Replace the default Poisson16 kernel with exact positive/negative pairs whose
  centroid is zero, and disable periodic world-position rotation by default.
- Remove StableWall receiver-UV texel snapping and the Grid5x5 hard switch.
  Direct and visibility sources now use the same PCF family with continuous
  radius and bias weights.

## Verification

- Numerical regression: the footprint `{0.2, 1.0; 0.2, 1.0}` at reference
  depth `0.5` resolves to visibility `0.5`; the removed raw-depth-first path
  resolved to `1.0`.
- All `AutoTest/test_*_static.py` scripts passed.
- Meson Win32 runnable suite passed: 20/20.
- Win32 `d3d9.dll` build passed and `ninja -C build32 -n` reports no work.
- `git diff --check` passed.
- Candidate DLL: 34,115,047 bytes, SHA-256
  `F8F29D26DDF471827742C7CBB838508EAC296792E574482A93EF7C2DF22B015A`.

## Remaining physical gate

The candidate is not yet a release baseline. A visible-desktop physical test
must compare fixed-camera/moving-sun and fixed-sun/moving-camera shots on ground,
slopes, walls, alpha-tested trees and all cascade transitions. Point shadows and
caster continuity must remain unchanged.
