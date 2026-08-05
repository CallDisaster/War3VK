# Warcraft III 1.27a Hybrid Ray Tracing

Last updated: 2026-07-14

## Status

- A0 linear software point-light contact ray, A1 half-resolution Hi-Z
  traversal, and the first A2 homogeneous-footprint reconstruction slice are
  implemented. The overall feature remains **default off**; A1/A2 is the
  preferred implementation after the user opts in, while A0 is the
  format/pipeline fallback and explicit A/B path.
- Both paths reuse the receiver depth copy. A cold default-off run creates no
  A1 helper, image, pipeline, UBO, copy, dispatch, or extra depth tap. The
  receiver UBO is now 736 bytes and carries the current projection, bounded
  ray controls, and a shared projection-derived depth-clear contract.
- Runtime and visual acceptance are pending because Warcraft III is occupied by
  map development today. No FPS or image-quality claim is made from static work.
- In-process hardware RT is currently unavailable on the tested x86 NVIDIA ICD.
- The current combined build-only artifact is
  `gpu_skin_p4_build_only_isolated_gpu_skin_bucket_shared_jobcache_monotonic_frame__20260714_074947`.
  It completed a full 71/71 x86 build, regenerated `war3_gpu_skin.h` and
  `war3_volumetric_light.h`, and compiled/linked Pipeline, Shadow, Volumetric,
  Shader API, Hybrid RT, and GPU-skin compute/manager. Current target errors and
  warnings are both zero; the remaining 187 warnings are pre-existing files.
  `launchPerformed=false` and `deployPerformed=false`.
  DLL SHA256:
  `833B5372149CAFE0D9E93612EB510F8D5D9AC6DFBEAB4E91C4FE84C56E670F3E`.
- Publication freshness no longer aliases the three-entry resource ring.
  `War3HybridRayResult::producedFrameSerial` and the Hi-Z owner use monotonic
  64-bit frame identity, and consumers require an exact current serial plus the
  immutable light generation. Camera fallback is bounded to age one. An old A1
  image can therefore remain allocated for lifetime safety but cannot be
  relabeled or consumed as the current frame after a skipped/failed producer.

## Hardware RT capability boundary

The same RTX 4060 Ti and NVIDIA 610.74 driver expose different Vulkan features
to 64-bit and 32-bit clients.

Read-only probes:

```powershell
$out = & 'C:\Windows\System32\vulkaninfo.exe' 2>$null
$out | Select-String 'VK_KHR_acceleration_structure|VK_KHR_ray_query|VK_KHR_ray_tracing_pipeline|VK_KHR_deferred_host_operations|accelerationStructure\s*=|rayQuery\s*=|rayTracingPipeline\s*=|bufferDeviceAddress\s*='

$out = & 'C:\Windows\SysWOW64\vulkaninfo.exe' 2>$null
$out | Select-String 'deviceName\s*=|VK_KHR_acceleration_structure|VK_KHR_ray_query|VK_KHR_ray_tracing_pipeline|VK_KHR_deferred_host_operations|accelerationStructure\s*=|rayQuery\s*=|rayTracingPipeline\s*=|bufferDeviceAddress\s*='
```

Observed result:

| Client | Result |
|---|---|
| 64-bit ICD | AS revision 13, ray-query revision 1, RT-pipeline revision 1, deferred-host-operations revision 4; all relevant features true |
| 32-bit ICD | Same RTX 4060 Ti and `bufferDeviceAddress=true`, but none of the four RT extensions/features are enumerated |

`E:\Work\War3\war3_d3d9.log` identifies the active build as x86 and likewise
contains no RT extension. Therefore this is not a GPU capability failure and it
is not a universal Vulkan/x86 rule: the current Windows NVIDIA x86 ICD simply
does not expose the required extensions. All future hardware paths must still
probe at runtime.

Even with a different ICD, this DXVK branch still lacks AS/ray-query device
feature chains, loader entry points, descriptor support, AS object and scratch
lifetime management, build/query barriers, and BLAS/TLAS generation policy.
Dynamic GPU-skinned geometry would additionally need AS build-input usage and a
`COMPUTE_SHADER_WRITE -> ACCELERATION_STRUCTURE_BUILD_READ` dependency.

## A0: software screen-space point-light ray

Implementation locations:

- settings and environment parsing:
  `src/d3d9/d3d9_war3_settings.h`, `src/d3d9/d3d9_war3_pipeline.cpp`
- current projection and bounded controls in the receiver UBO:
  `src/d3d9/d3d9_war3_shadow.cpp`
- depth reconstruction, short view-space ray march, and cube/contact resolve:
  `subprojects/war3fx/shaders/war3_shadow_receiver.frag`
- interactive controls: `src/d3d9/war3/ui/war3_imgui.cpp`

For a visible receiver point, the shader marches a short ray toward the point
light. Every sample is projected through the current camera projection, mapped
back into the world viewport, and compared with the copied scene depth after
reconstructing view position. A conservative crossing requires:

```text
depthBias < abs(rayViewZ) - abs(sceneViewZ) < thickness
```

Illegal matrices, exact clear depth, off-screen samples, missing depth, and step
budget exhaustion all fail soft to fully visible. The resolve is:

```glsl
pointShadowFactor = min(cubeShadowFactor, contactFactor);
```

The software ray can add short-range occlusion but cannot brighten a complete
cube shadow or invent an off-screen blocker.

### Controls

| Environment variable | Default | Bound |
|---|---:|---:|
| `DXVK_WAR3_POINT_RAY_SHADOW` | 0 | boolean |
| `DXVK_WAR3_POINT_RAY_SHADOW_HIZ` | 1 after overall opt-in | boolean |
| `DXVK_WAR3_POINT_RAY_SHADOW_MAX_LIGHTS` | 1 | 1..2 |
| `DXVK_WAR3_POINT_RAY_SHADOW_STEPS` | 12 | 4..32 |
| `DXVK_WAR3_POINT_RAY_SHADOW_HIZ_VISITS` | 24 | 8..64 |
| `DXVK_WAR3_POINT_RAY_SHADOW_MAX_DISTANCE` | 480 | 32..2400 world units |
| `DXVK_WAR3_POINT_RAY_SHADOW_THICKNESS` | 24 | 1..160 world units |
| `DXVK_WAR3_POINT_RAY_SHADOW_START_OFFSET` | 10 | 1..96 world units |
| `DXVK_WAR3_POINT_RAY_SHADOW_STRENGTH` | 0.85 | 0..1 |

The critical cost gate is independent from the direct-light count. Only the
canonical shadow-capable prefix is eligible, and each light retains its authored
shadow-intensity gate. Default cost is at most one ray times 12 samples per
shaded pixel; the exposed hard ceiling is two rays times 32 samples. It never
expands to all 16 direct lights.

### Acceptance gates

- off: zero new resource/copy/dispatch/depth taps;
- default-on budget: one important light, 12 samples;
- 2560x1440 incremental GPU p95 target <= 0.75 ms, hard exploratory ceiling
  1.0 ms; otherwise remain default off;
- no new hard cutoff at point-light range;
- complete cube result is never brightened;
- off-screen unknown geometry falls back to cube/fully lit;
- resize/reset and a second process must not retain stale camera or depth state.

## A1: half-resolution Hi-Z software ray

Implementation locations:

- orchestration, Vulkan resources, barriers, and exact result publication:
  `src/d3d9/war3/render/war3_hybrid_ray_tracing.h/.cpp`
- full-depth to half-resolution min/max seed:
  `subprojects/war3fx/shaders/war3_hiz_seed.comp`
- mip reduction:
  `subprojects/war3fx/shaders/war3_hiz_reduce.comp`
- bounded hierarchical traversal and visibility/confidence output:
  `subprojects/war3fx/shaders/war3_point_contact_hiz.comp`
- final depth-agreement resolve and cube merge:
  `subprojects/war3fx/shaders/war3_shadow_receiver.frag`

The helper is created lazily only after the overall contact-ray feature is
enabled. It records raw built-in compute commands on the existing command list
after the receiver depth copy; it does not call `DxvkContext::dispatch()` and
therefore does not introduce a per-draw render-pass spill.

The seed writes half-resolution `R32G32_SFLOAT`:

```text
R = minimum positive view depth
G = +maximum depth when all covered full-resolution samples are valid
G = -maximum depth when coverage is mixed
R = G = 0 when no valid sample exists
```

Reduction preserves that validity sign. For odd Vulkan mip extents, the final
destination cell absorbs the orphan source row/column, matching the traversal's
last-cell boundary. The contact shader writes one `R16G16_SFLOAT` array layer
per eligible canonical point light: visibility in R and confidence in G.

Correctness gates are deliberately conservative:

- far-clear raw depth is inferred from the inverse projection before applying
  viewport MinZ/MaxZ, so reversed projection and reversed viewport mapping are
  independent;
- if that inference is ambiguous, the optional pass returns no A1 result;
- 2x2 depth discontinuities are rejected and confidence decays with depth
  spread; the receiver accepts only a matching representative depth;
- one raw D16/D24/D32 depth quantum is projected into view-Z uncertainty;
  excessive uncertainty returns confidence zero, and acceptable uncertainty
  can only shrink the blocker interval;
- mixed/empty cells, illegal matrices, off-screen traversal, exact-test limit,
  or the 8..64 node-visit budget all return `(visibility=1, confidence=0)`;
- mip0 no longer samples only the 1/3 and 2/3 positions of a 2x2 full-depth
  footprint. The projected rational ray is split at exact internal
  `x/y = cell + 0.5` boundaries, yielding one to three positive-length
  full-resolution pixel intervals. Each interval loads depth once; its midpoint
  establishes only half-open pixel ownership, while blocker classification uses
  the complete ray-depth interval inside that pixel. Quantization is explicitly
  three-state: an expanded possible window may prove a definite miss; possible
  overlap without overlap in the shrunken reliable window returns unknown; only
  reliable overlap chooses a representative from the actual intersection.
  Constant/near-constant ray depth must lie strictly inside that reliable window.
  The global eight-load ceiling is unchanged. Corner ties, line-on-edge ownership,
  unrepresentable intervals, failed/inconsistent roots, or ownership disagreement
  all fail soft to unknown. A thin blocker in the third crossed pixel or on
  either side of the ownership midpoint can therefore no longer be skipped and
  followed by confident visibility;
- hierarchical cell traversal no longer rejects exits within a fixed
  `1e-4` world distance or advances by `max(1e-3,d*1e-5)`. Those epsilons could
  jump an entire short projected cell and return confident visibility past a
  real blocker. The DDA now stops at the exact rational screen boundary,
  assigns half-open cell ownership from projected direction, and verifies that
  every crossing enters exactly the adjacent old-mip cell. Failed roots,
  contradictory axes, corner ties, zero/unrepresentable progress, and ownership
  disagreement all return unknown. A confident visible result is emitted only
  after the final conservatively tested segment reaches `traceLength`;
- before reconstructing the A2 footprint plane or entering Hi-Z traversal,
  each layer rejects receivers outside that light's authored range. Inside the
  range it reuses the receiver falloff `(1-x^2)^2/(1+6x^2)` as a deterministic
  relevance signal: configured visits remain at the source and smoothly fall
  toward the hard floor of 8 in the dim outer range. Confidence also fades to
  zero there, so reduced work can only fall back to cube/fully lit and cannot
  synthesize an occluder;
- receiver resolve is `1 - confidence * (1 - visibility)`, then the existing
  `min(cube, contact)` merge. A1 can neither brighten cube shadowing nor create
  a trusted blocker from unknown data.

The contact pass is sparse per light rather than a full half-resolution 3-D
dispatch. CPU code projects the exact view-space `dist < range` sphere consumed
by the shader through a conservative eight-corner AABB. Camera/near-plane
crossing or ambiguous projection falls back to the full target; a wholly
behind-camera or provably off-screen sphere may produce an empty region. Bounds
use absolute viewport pixels, outward half-resolution rounding, and a two-cell
guard, so non-zero/odd viewport origins remain covered.

Sparse publication cannot rely on old image contents. Every active visibility
array layer is therefore transfer-cleared in `GENERAL` to `(1,0)` before contact
dispatch. The existing 48-byte push ABI is retained by using `srcMip` as the
single light layer and the dispatch quartet as `x/y/width/height`; each non-empty
ROI dispatches with `z=1`. Barriers cover previous compute/fragment/transfer
access, clear-to-compute write-after-write, and final transfer-or-compute writes
to fragment reads. ROI-outside and empty layers consequently remain explicitly
fully visible with zero confidence instead of inheriting a trusted stale shadow.
Before any allocation, layout transition, clear, or Hi-Z seed, CPU code now
computes all conservative light ROIs once. If every eligible sphere is proven
offscreen/behind, `Run()` publishes no A1 result and records zero GPU work; the
receiver has already cleared its current-frame A1 views/generations, so cached
resources cannot become stale evidence. Non-finite or camera-plane-crossing
inputs remain full fail-soft and do not take this early-out.

A1 now supports at most two canonical shadow lights without multiplying the
configured traversal budget per light. One-light behavior retains the original
`mix(8, configured, sqrt(rangeRelevance))` expression. With two contributing
lights, each gets eight fail-soft base visits and the remaining
`configured - 8` visits are divided by the receiver's actual normalized
`maxRGB * intensity * shadowStrength` times the same direct-light range
attenuation. Therefore the overlapping-pixel sum is bounded by
`configured + 8` (32 at the default 24), rather than 48. The visibility array
capacity only grows from one to two layers; active clear, dispatch, barrier,
and publication ranges remain exact each frame. The scalar UBO is now 320
bytes; the 48-byte push ABI is unchanged.

The receiver reconstructs the half-resolution result with a depth-guided 2x2
bilinear gather instead of a nearest texel. For each tap, spatial weight is
multiplied by confidence, representative-depth agreement, and occlusion. The
sum is deliberately not renormalized: unknown, mixed, invalid, or mismatched
data contributes zero occlusion, so missing evidence can only approach fully
lit. The canonical half-resolution owner (`fullPixel / 2`) must itself be
trusted before any neighbor may contribute; this prevents a valid adjacent ROI
cell from lending a blocker into a transfer-cleared or unknown anchor. Edge
taps clamp to the image boundary while retaining their original bilinear
weight, preserving constant fields at odd extents and screen borders.

This resolve changes the receiver cost per eligible contact light from two
texture fetches to at most eight (four visibility/confidence plus four Hi-Z
depth taps). At 2560x1440 and one light that is +22.1 million fetches, or about
126.6 MiB of raw requested texel bytes before cache effects. It is therefore a
quality/correctness trade that remains subject to the isolated GPU p95 gate,
not a proven performance win.

Publication requires an exact frame index, immutable light generation,
resource generation, and layer count tuple. Views are cleared at the start of
every receiver run, resize allocates a complete replacement generation before
publication, and all A1 resources are tracked before the first recorded Vulkan
command. Device teardown now waits for GPU idle before destroying raw built-in
pipelines. Runtime disabling after prior use intentionally retains the cache
until its GPU-safe owner is destroyed; cold default-off remains zero-allocation.

At 2560x1440:

| Resource | Footprint |
|---|---:|
| Hi-Z `RG32F`, full mip chain | 9.375 MiB |
| Visibility/confidence `RG16F`, one light | 3.516 MiB |
| Total, one light | 12.891 MiB |
| Total, two lights | 16.407 MiB |

These are persistent allocation sizes, not measured runtime bandwidth. The
adaptive range/relevance budget reduces traversal work rather than allocation.
The two-light extension changes only the UBO tail described above; resource
publication and the push ABI remain stable. A1 is only build-verified today;
no visual, cull-rate, or GPU-time claim is made yet.

## A2: homogeneous-footprint plane reconstruction

The first A2 slice is implemented in
`subprojects/war3fx/shaders/war3_point_contact_hiz.comp`; runtime acceptance is
still pending. It deliberately does not introduce temporal history or require a
motion vector.

A1 selected one receiver from the already loaded homogeneous 2x2 full-depth
footprint, then loaded left/right/up/down around that receiver to reconstruct a
normal. Those four extra samples had two problems:

- they repeated four full-depth fetches and, including raw-depth uncertainty,
  up to twelve view-position reconstructions per scheduled half-resolution
  texel and light;
- because the selected receiver can be any corner of the 2x2 footprint, the
  neighbour stencil can reach across the foreground/background edge that the
  Hi-Z homogeneity gate intentionally excluded. A representative-pixel change
  can then change the normal and normal-offset origin even when the accepted
  2x2 surface itself remains stable.

A2 retains the four depth samples already used for receiver selection and forms
the two triangles `(p00,p01,p10)` and `(p10,p11,p01)`. Both use the same
screen-space winding, `down x right`. Each edge is normalized before the cross
product, each triangle rejects a squared sine below `1e-8`, and the two unit
normals must have dot agreement above `0.5`. Agreement from `0.5` to `0.98`
only raises confidence through `smoothstep`; it never changes visibility or
turns uncertain geometry into a trusted blocker. Degenerate triangles,
non-finite values, disagreement, and partial 2x2 footprints at odd target edges
write the existing unknown value `(visibility=1, confidence=0)`. The four
depth-quantization errors are combined with `max`, so reusing the footprint is
no less conservative than using the chosen representative alone.

The resulting unit normal is flipped toward `-receiverView` before applying the
existing bounded origin offset. Triangle winding depends only on screen-pixel
ordering and reconstructed view positions, not on whether raw depth is normal
or reversed; viewport depth mapping is consumed earlier by
`loadDepthPixel`. Therefore reversed depth cannot reverse the trusted normal.

This removes exactly four depth fetches from every regular scheduled contact
invocation. A full 2560x1440 ROI contains 921,600 half-resolution texels, so the
static upper-bound reduction is 3,686,400 depth fetches and up to 11,059,200
view reconstructions per light, or twice those values for two lights. ROI,
Hi-Z allocation, push/UBO ABI, barriers, publication generations, and the cold
default-off zero-work contract are unchanged.

An independent CPU numerical harness sampled 99,980 valid random planar
footprints: the minimum two-triangle agreement was `1.000000000000`, and every
camera-facing flip remained positive. It also round-tripped 400,000 random view
positions across normal/reversed projection crossed with normal/reversed
viewport mapping; maximum relative position error was `6.574e-12`. A 2,001
sample sweep of the agreement gate proved bounded monotonic confidence with
exact rejection through `0.5`. This is numerical/static evidence, not a GPU
shader compile or runtime visual result.

Runtime acceptance must compare A1/A2 on planar floors, curved terrain, thin
foreground/background edges, odd render-target sizes, normal and reversed
depth, resize/reset, and a second process. Required evidence is: no trusted
shadow at odd/degenerate footprints, no edge-crossing normal flicker, unchanged
cube fallback, and an improved or neutral `Shadow/PointRayHiZ` GPU p95.

## Temporal follow-up

Motion-vector reprojection may be considered only when light, camera, viewport,
depth, and resource generations all match. Do not jump directly to GI: first
prove A1/A2 contact-shadow correctness and cost on the isolated light test map.
The first runtime gate must cover reversed depth, resize/reset, thin
foreground/background edges, cube seams, range fade, and a second process
before temporal history is considered.

## Hardware follow-up

The realistic hardware experiment is a 64-bit sidecar using exportable Win32
memory and external semaphores. It would build BLAS/TLAS and write visibility
for the x86 receiver. This is materially more complex than A0/A1 because it adds
cross-process allocation, synchronization, resize/reset, watchdog, and copy
costs. It should not start until software contact rays demonstrate a visual win.

The old `d3d9_war3_pathtrace.*` files are only a draw-count stub. Their hot draw
call has been removed. They must not be presented as a path tracer; later cleanup
should delete them or rename them to render statistics.
