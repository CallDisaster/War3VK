# Directional alpha-cutout shadow coverage stability

Date: 2026-08-11

Scope: Directional CSM caster coverage for foliage, grass, fences and other
thin alpha-cutout silhouettes. Point-shadow code, TAA, CSM layout/resolution,
caster production and GPU lifetime are explicitly out of scope.

## Observed boundary

The fresh 4096 DirectInline baseline is stable with a fixed camera and a fixed
sun: the measured shadow-edge changed fraction is approximately 0.055% between
internal framebuffer captures. Moving the camera or stepping the sun produces
large edge changes, while replay remains complete and Arena/device-loss counters
stay zero. This separates the remaining visual defect from random caster loss:
it is primarily sampling/coverage aliasing on thin alpha-tested silhouettes.

## Primary references

1. Chris Wyman and Morgan McGuire, *Hashed Alpha Testing*, I3D 2017:
   <https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf>
2. Chris Wyman and Morgan McGuire, *Improved Alpha Testing Using Hashed
   Sampling*, I3D 2019:
   <https://research.nvidia.com/labs/rtr/publication/wyman2019improved/>
3. Microsoft, *Cascaded Shadow Maps*:
   <https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps>
4. Khronos Vulkan shader execution and derivative rules:
   <https://docs.vulkan.org/spec/latest/chapters/shaders.html>

The 2017 method replaces a fixed cutout threshold with a stable threshold
derived from surface-anchored 3D coordinates. Pixel-scale stability requires
screen derivatives, logarithmically adjacent quantization scales, interpolation
between those scales and a CDF remap that restores a uniform threshold
distribution. Nearby magnified geometry should retain traditional alpha test;
the paper recommends a quadratic fade over roughly six LOD levels. No frame or
time seed is used.

## WarVK mapping

- `war3_shadow_caster_vert.vert` publishes the immutable pre-transform vertex
  position as the surface coordinate. It deliberately excludes frame serial,
  palette index, Arena address, sampler identity and other transient salts.
- `war3_shadow_caster_frag.frag` computes UV and surface derivatives before any
  data-dependent discard, samples alpha with explicit gradients, evaluates the
  two log-scale hashes and applies the CDF remap.
- The final threshold blends from the draw's authored `alphaRef` to the uniform
  hashed threshold using the six-level quadratic minification ramp. A derivative
  or finite-value failure returns to the authored hard cutoff.
- The candidate is directional-only. The separate point-shadow fragment shader
  is unchanged, preserving the already accepted point-shadow receiver fix.

## Unreal Engine architecture reference

The external official Epic source checkout is used only as a read-only
architecture reference. Its shadow-depth pass routes masked materials through a
material clipping contract instead of duplicating material visibility logic in
the shadow renderer (`ShadowDepthPixelShader.usf` and `MaterialTemplate.ush`).
WarVK follows the same architectural lesson—one explicit directional caster
coverage contract—without copying Unreal Engine implementation code, shader
code, assets or data into this repository.

## Candidate and physical gates

This first implementation remains Release-default Off. Offline contracts can
prove bounded math, deterministic surface anchoring, derivative ordering and
point-shadow isolation, but cannot prove player-visible quality. Promotion is
allowed only after an A-B-B-A physical gate demonstrates all of the following:

- edge-toggle p95 improves by at least 40%;
- fine-edge coverage/length retains at least 99% of the hard-cutoff baseline;
- GPU delta is at most 0.30 ms and CPU delta at most 0.15 ms;
- no producer/replay/Arena/device-loss, UBirth or point-shadow regression.

If the candidate misses any gate, Release remains on the hard cutoff and the
result is retained only as a bounded experiment. No offline result in this
document claims that the player-reported shimmer has been fixed.

## Superseded first A-B-B-A attempt

The first artifact at
`AutoTest/artifacts/shadow_alpha_coverage_abba/20260811_020854` is not valid
physical evidence. The scene was paused and the runner did not prove that the
directional shadow map had been republished after each locked-sun command.
Repeated captures could therefore compare an old depth publication against new
settings. Its percentages must not be used for promotion or rejection.

## Publication-qualified A-B-B-A result

The corrected runner leaves the producer live and requires
`shadowMapRenderSerial` to increase after every sun step before recording the
image. The visible-desktop `Off A -> Hash B1 -> Hash B2 -> Off A2` run completed
on Turtle Rock with 12 publication-qualified steps per round. All four fresh
processes completed without device loss or a directional publication timeout.
The aggregate metrics were:

- hard-cutoff edge-toggle p95 mean: `0.329167`;
- hashed edge-toggle p95 mean: `0.330205`;
- relative improvement: `-0.3153%`;
- dark-coverage ratio: `0.999980`;
- fine-edge-length ratio: `0.999327`.

The candidate preserves coverage but does not reduce this scene's temporal edge
change. `alphaShadowHashed` therefore remains Release-default Off. This is a
bounded negative experiment, not proof that hashed alpha is universally
ineffective and not proof that Issue #4 is visually fixed. The sun-step metric
also includes legitimate physical shadow movement, so it can reject a candidate
that demonstrably fails to improve the result but cannot by itself identify all
sources of player-visible shimmer.

Artifact: `AutoTest/artifacts/shadow_alpha_coverage_abba/20260811_023543`.

## Receiver-filter control

The same publication-qualified harness compared the Release Poisson16 kernel
at radius `0.70` against the existing Grid5x5 kernel at the same radius. Grid5x5
made edge-toggle p95 `5.54%` worse and increased measured fine-edge length by
`9.47%`; it therefore also failed the image gate. This rules out blindly
expanding the existing PCF support as a safe default and avoids treating added
blur as temporal stability.

Artifact: `AutoTest/artifacts/shadow_filter_abba/20260811_023947`.
