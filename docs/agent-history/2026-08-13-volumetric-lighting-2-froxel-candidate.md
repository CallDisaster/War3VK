# 2026-08-13 Volumetric Lighting 2.0 Froxel candidate

> Superseded for low-pitch depth/shadow semantics by
> `2026-08-13-common-froxel-shadow-interval-candidate.md`. In particular, the
> statement below that integration performs no shadow lookup is historical.

## Scope

This work implements the research request to retain the established
view-ray-march backend while adding an opt-in production-oriented Froxel
backend. It does not deploy the DLL, start Warcraft/YDWE, or claim player
foreground visual/performance acceptance.

The formula and Vulkan contract are recorded in
`docs/research/2026-08-13-warvk-volumetric-lighting-2-froxel.md`.

## Implemented

- Added bounded quality tiers:
  - `LegacyRayMarch` (0), still the default;
  - `FroxelMedium` (1): 32-pixel XY tiles, 48 logarithmic Z slices;
  - `FroxelHigh` (2): 16-pixel XY tiles, 64 logarithmic Z slices.
- Added a hard 2,100,000-cell admission budget, device 3D-image limit check,
  and explicit RGBA16F sampled/storage/color-attachment feature check. Any
  failed admission falls back to the legacy backend.
- Added `RGBA16F` 3D current lighting/extinction and ping-pong history images.
- Added compute injection of:
  - bounded global medium and optional height profile;
  - all selected Sphere/Box/Cylinder local fog volumes;
  - volume-sun or camera-CSM directional visibility with cascade fallback and
    transition blend;
  - at most two relevance-selected point lights and exact published cube
    shadows.
- Added an eight-phase low-discrepancy sub-cell jitter so a thin caster is not
  permanently missed by a fixed Froxel center.
- Added world-position temporal reprojection, seven-neighbour variance
  clipping and bounded reactive feedback.
- History reads require the exact map/device epoch, consecutive frame serial,
  grid/near/far contract and bounded camera displacement. Backend/resource
  changes invalidate history.
- Added a compute integration stage using Beer-Lambert transmittance and the
  closed-form constant-segment source integral. This stage samples only the
  stable 3D field and scene depth; it performs no shadow lookup.
- Reused the existing radiance/depth-aware composite.
- Preserved low-end compatibility: the legacy effect image does not require
  storage-image support.
- Added `volumetric.setBackend` and
  `WarVKSetVolumetricBackend(0..2)`, public constants, YDWE action metadata,
  environment override `DXVK_WAR3_VOLUMETRIC_BACKEND`, and an ImGui selector.
- Updated map-author documentation. A local-only candidate uses:

```jass
call WarVKSetVolumetricEnabled(true)
call WarVKSetVolumetricBackend(WARVK_VOLUMETRIC_BACKEND_FROXEL_HIGH)
call WarVKSetGlobalVolumetricMediumEnabled(false)
set fogId = WarVKCreateSphereFogVolume(x, y, z, 420.00, 0.55, 0.25)
```

The DLL and the updated `WarVK` catalog/JASS package must both be distributed
for the new command to exist. Merely compiling the map against a newer wrapper
does not update an already deployed proxy DLL.

## Validation

- 78/78 `AutoTest/test_*_static.py` scripts passed, including the new 13-case
  Froxel resource/shader/history/JAPI contract test.
- 21/21 Meson Win32 runnable tests passed.
- Win32 DLL build passed.
- `ninja -C build32 -n`: no work to do.
- `git diff --check`: clean.
- Candidate DLL:
  - path: `build32/src/d3d9/d3d9.dll`
  - size: 34,245,871 bytes
  - SHA-256: `9BAAD705AE24E1DFB5F0DFB7B54DDA9746B412914BEF07EF3073F14E04049E61`

## Concurrent-work boundary

Before implementation, the existing dirty worktree contained the separate
optimization work in `src/dxvk/dxvk_device_info.cpp`,
`src/dxvk/dxvk_device_info.h`, `src/dxvk/meson.build`,
`src/vulkan/vulkan_loader.h`, `subprojects/StormBreaker`, and unrelated
PlayerCrash/build-log artifacts. This candidate did not edit those files.

The pre-edit SHA-256 baselines of the principal target files were:

- `d3d9_war3_volumetric_light.cpp`:
  `7570DFCA4E5B75D96C68A599717285F2F91B7014F7EDFC3784D3D63E98010F36`
- `d3d9_war3_volumetric_light.h`:
  `6D495205A3D21D55C33198D72B99E1DBA60B5B36D43C5FA00D8FF2F4C113DAF0`
- `d3d9_war3_settings.h`:
  `239917F8D6E396E2F5F362A40A13653B1A32A6F153D8A98CFA771F555C331326`
- `subprojects/war3fx/meson.build`:
  `219F75DDAD7D39A3220C7BEBE6E15EE19E69D5F4C2A70E92D1C2760A3783C831`

## Required physical gates

Before changing the default from `LegacyRayMarch`, run foreground A/B on the
same map/camera route for at least:

- ordinary top-down view, low pitch and close caster;
- still camera and slow pan (banding/stability);
- moving/animated caster (reactive rejection and trails);
- global medium off with each local Sphere/Box/Cylinder;
- point shadow on/off and volume-sun/CSM fallback;
- 1080p, 1440p and 4K GPU time/residency;
- Reset and the release-scoped single-map lifecycle.

No current evidence proves the Destiny-style appearance, eliminates all ghost
trails, or establishes 4K foreground performance. The current implementation
also has camera/world-position reprojection but no exact per-object volumetric
motion vectors; dynamic-caster changes rely on variance/reactive rejection.
