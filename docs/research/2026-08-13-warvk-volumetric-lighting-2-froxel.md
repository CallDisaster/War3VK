# WarVK Volumetric Lighting 2.0: Froxel / temporal contract

Date: 2026-08-13

Status: implementation contract for an opt-in candidate. The legacy
view-ray-march backend remains the Release default until foreground physical
validation is complete.

## Primary references

- Bartlomiej Wronski, *Volumetric Fog: Unified compute shader based solution to
  atmospheric scattering*, SIGGRAPH 2014. The reference establishes a
  view-frustum-aligned volume, logarithmic depth distribution, light/shadow
  injection, temporal reprojection and front-to-back integration as one
  production pipeline:
  <https://www.advances.realtimerendering.com/s2014/wronski/bwronski_volumetric_fog_siggraph2014.pdf>
- Sebastien Hillaire, *Physically Based and Unified Volumetric Rendering in
  Frostbite*, SIGGRAPH 2015. The reference separates participating-media
  properties from lighting, injects local media and shadowed lights, and
  integrates scattering/transmittance for composition:
  <https://www.ea.com/news/physically-based-unified-volumetric-rendering-in-frostbite?isLocalized=true>
- NVIDIA, *Fast, Flexible, Physically-Based Volumetric Light Scattering*. The
  reference supplies the single-scattering/Beer-Lambert basis and bounded
  epipolar/volumetric implementation considerations:
  <https://developer.nvidia.com/sites/default/files/akamai/gameworks/downloads/papers/NVVL/Fast_Flexible_Physically-Based_Volumetric_Light_Scattering.pdf>
- Khronos, *Vulkan Specification*. Storage-image writes followed by compute
  reads and sampled reads require explicit availability/visibility; image
  layout and descriptor layout must agree:
  <https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html>

No Unreal Engine source or asset is used by this design or implementation.

## WarVK mapping

The established `War3VolumetricLightPass` remains the system owner. It exposes
two backends:

- `LegacyRayMarch`: the current 4--16 step fragment path.
- `FroxelMedium` / `FroxelHigh`: compute density/light injection, temporal
  reconstruction, compute ray integration, then the existing bilateral
  composite.

The Froxel path consumes the exact immutable publications already accepted by
the legacy path:

- `GetVolumetricShadowSnapshot` and
  `GetVolumetricSunShadowSnapshot` for directional occlusion;
- `GetVolumetricPointShadowSnapshot` for complete six-face point cubes;
- `War3FogVolumeFrameSnapshot` for at most eight finite Sphere/Box/Cylinder
  entries;
- the current `War3PipelineInput` map/device epochs and monotonic frame serial.

It does not create an alternative caster, light, volume or epoch truth.

## Froxel coordinates and budget

`x/y` are view-frustum tiles and `z` is exponential camera distance. For slice
boundary `k` in `[0,N]`:

```text
d(k) = near * exp(log(far / near) * k / N)
```

The cell center uses the geometric mean of its two boundaries. This provides
more resolution close to the camera without allowing camera pitch to change
the world-space distance represented by a slice.

Candidate tiers are:

| Tier | XY tile footprint | Z slices | Maximum cells at 4K |
| --- | ---: | ---: | ---: |
| Medium | 32 x 32 pixels | 48 | 391,680 |
| High | 16 x 16 pixels | 64 | 2,073,600 |

`2,100,000` cells is a hard admission budget. An extent outside the contract
fails back to legacy rather than dispatching unbounded work. This is an
execution bound, not a foreground performance claim.

## Medium and lighting injection

For each Froxel center `x`, total extinction is

```text
sigma_t(x) = I_global * sigma_global(x)
           + sum_i sigma_local_i(x)
```

where local media use the already-published finite world-to-local transforms
and the Sphere/Box/Cylinder signed support plus authored edge feather. The
global height profile is

```text
rho_h(x) = 1 + strength * exp(-falloff * max(height(x)-base, 0))
```

and the bounded author density response remains compatible with the legacy
backend:

```text
sigma(density) = 0.0006 * density / (1 + 0.5 * density)
```

Single-scattering source stored in RGB is

```text
Q(x,w) = sigma_s(x) * phase(w, lightDirection)
       * (L_sun * V_sun(x) + sum_j L_j(x) * V_j(x))
```

with `sigma_s = albedo * sigma_t`. Directional visibility is sampled from the
published volume-sun map first, then the accepted camera CSM fallback. Point
visibility uses only exact published cube layers; missing or stale layers are
fail-lit for that point contribution and never substitute an incompatible
image view.

## Temporal reconstruction and bounded denoise

The current cell center is reconstructed in world space and projected by the
matrix that generated the readable history:

```text
prevClip = float4(worldPosition, 1) * previousViewProjection
```

Injection uses an eight-frame bounded low-discrepancy offset inside each cell
(phase zero is the exact center). This converts sub-cell shadow coverage into a
temporal signal instead of permanently missing a narrow caster whose shadow
never crosses a fixed center. The history itself is reprojected at the stable
cell center; disabling temporal accumulation also fixes injection at phase zero
to avoid deliberate shimmer without a reconstruction stage.

The previous camera position converts the world point back to the previous
exponential slice. History is readable only when all of these hold:

- same non-zero map and device epoch;
- exactly consecutive presentation frame;
- same grid dimensions, near/far distribution and backend tier;
- no resource recreation or explicit backend/temporal disable;
- camera displacement is within half the represented far distance.

The shader also rejects out-of-frustum/out-of-depth reprojection. A bounded
seven-cell cross neighborhood provides mean and variance. Reprojected history
is clipped to

```text
[mean - gamma * sqrt(variance), mean + gamma * sqrt(variance)]
```

and a source/extinction disagreement reduces history weight (reactive
feedback). This candidate has camera/world-position reprojection but no
per-object volumetric motion vectors; animated-caster disagreement is handled
by reactive rejection and variance clipping rather than falsely claiming exact
world motion.

## Integration and synchronization

For each view ray, only the stable 3D field is sampled; no shadow lookup occurs
inside integration. For a slice segment of length `ds`:

```text
T_segment = exp(-sigma_t * ds)
L         = L + T * Q * (1 - T_segment) / max(sigma_t, epsilon)
T         = T * T_segment
```

Integration stops at reconstructed scene depth (or the represented far
distance for a clear-depth pixel) and writes `RGBA16F = (L, T)` to the existing
low-resolution effect surface. The established depth/radiance-aware composite
then reconstructs it at full resolution.

All 3D volumes remain in `GENERAL`. The command list inserts:

1. injection write -> temporal sampled read;
2. temporal write -> integration sampled read;
3. effect `SHADER_READ_ONLY_OPTIMAL` -> `GENERAL` for storage write;
4. integration write -> effect sampled read and transition back to
   `SHADER_READ_ONLY_OPTIMAL`.

The pass updates/invalidates history only while recording at the Present-owned
render point. It never frees GPU resources from JASS or map-unload callbacks.
Tracked `Rc` ownership keeps resources named by older command lists alive.

## Validation boundary

Static shader/ABI tests, Win32 runnable tests, a 32-bit DLL build and a Ninja
no-work check can validate integration and bounded contracts. They cannot prove
the requested Destiny-style appearance, low-pitch stability, absence of ghost
trails, or 4K foreground GPU time. Those require an opt-in player foreground
A/B before changing the Release default.

The subsequent 2026-08-13 player regression proved that the test map only
called `WarVKSetVolumetricEnabled(true)` while the source default remained
`LegacyRayMarch`; its generated JASS never called `WarVKSetVolumetricBackend`
and the runtime log contained no Froxel submission. The current High-default
candidate exists specifically to make this foreground A/B execute the new
backend without a hidden extra authoring requirement. It is not release-default
acceptance by itself.
