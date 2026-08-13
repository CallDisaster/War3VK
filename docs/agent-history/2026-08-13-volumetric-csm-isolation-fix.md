# 2026-08-13 Volumetric / surface CSM isolation fix

## Player evidence and binary identity

The player reported that enabling volumetric lighting produced no visible
scattering or volumetric shadow, while a small piece of ordinary surface CSM
detail disappeared.

The foreground Warcraft installation was using:

- `E:/Work/War3/d3d9.dll`
- size `35,102,059` bytes
- SHA-256 `974295790A87CAAE73D903C4D4450EA9297672C6263FCCFD08C204F23394BF01`

That binary is byte-identical to the build from
`dxvk-night-cull-20260811` / `codex/perf-production-snapshot-20260812`.
Binary string inspection proves that it contains the legacy volumetric path
and shader-work admission diagnostics, but does **not** contain
`volumetric.setBackend` or the Froxel submission diagnostics. The player was
therefore not running the Volumetric Lighting 2.0 candidate.

The captured `E:/Work/War3/war3_d3d9.log` also contains no
`DXVK War3Volumetric` submission marker. This does not prove which early gate
rejected the pass, but it proves that a completed volumetric draw/composite was
not recorded by that run.

## Root cause of the surface CSM change

`War3ShadowReceiverPass::Run` used the volumetric enabled bit to force the
ordinary surface CSM `farCasterDepthExtension` to at least 384 world units.
That changes the C2/C3 light-space depth interval and can reduce useful depth
precision. It also violates the expected isolation contract: an optional
post-effect toggle must not alter the surface shadow projection.

The volume-sun producer already owns a separate fixed-radius ortho shadow map
and an independent `volumeSunDepthExtension`, so modifying the surface CSM is
neither necessary nor safe.

The same producer gate also rendered the dedicated volume-sun map whenever the
effect switch was on, even if there was no non-zero global medium, active local
fog volume, effective volume intensity, or energetic sun for the later volume
consumer.

## Fix

- Removed the volumetric-enabled override of surface CSM
  `farCasterDepthExtension`. Authored/diagnostic surface CSM values remain
  bounded exactly as before, independently of the volumetric toggle.
- Added a no-lock producer admission gate. The separate volume-sun map is now
  rendered only when all of these are true:
  - post processing, volumetrics, and volume-sun shadows are enabled;
  - volumetric intensity and sample count are non-zero;
  - either the global medium has non-zero density or at least one active local
    fog volume exists;
  - the sun is enabled, above the configured minimum intensity, and has
    non-zero finite radiance;
  - the existing CSM/replay publication contracts are valid.
- Local volume visibility and bounded snapshot selection remain authoritative
  in the volumetric pass. The shadow producer uses only the manager's atomic
  `HasActiveVolumes` probe and does not consume mutable JASS data directly.

## Validation

- `AutoTest/test_war3_volumetric_froxel_static.py`: 14/14
- `AutoTest/test_war3_local_volumetric_fog_static.py`: 10/10
- `AutoTest/test_shadow_continuity_trace_static.py`: 4/4
- `AutoTest/test_shadow_cross_map_lifecycle_static.py`: 24/24
- Win32 DLL link completed.
- `ninja -C build32 -n`: no work to do.

Current volume-tree candidate:

- `build32/src/d3d9/d3d9.dll`
- size `34,245,871` bytes
- SHA-256 `7345EF3D9B9F89EE94858AFA62803077E84B77D44F10E68767DBAC5F29B324EF`
- contains `volumetric.setBackend` and Froxel submission diagnostics.

This candidate has not been deployed or launched. It does not yet include the
separate performance branch's latest changes, so the binary identity mismatch
must be resolved in an isolated integration step before a foreground A/B. The
static/build evidence does not prove visual output, CSM identity in a captured
frame, temporal quality, or 4K performance.
