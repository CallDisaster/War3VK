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

## A-B-B-A result

The visible-desktop `Off A -> Hash B1 -> Hash B2 -> Off A2` run completed on
Turtle Rock with 16 identical locked-sun steps per round. All four fresh game
processes completed without device loss, Arena/replay failure or an incomplete
directional publication. The aggregate image metrics were:

- hard-cutoff edge-toggle p95 mean: `0.243596`;
- hashed edge-toggle p95 mean: `0.241909`;
- relative improvement: `0.6926%`;
- dark-coverage ratio: `1.000210`;
- fine-edge-length ratio: `0.997996`.

Coverage was preserved, but the edge-toggle improvement is far below the 40%
promotion gate. `alphaShadowHashed` therefore remains Release-default Off and
this candidate is retained only as a bounded negative experiment. The result
narrows the next investigation toward CSM projection/texel-grid stability and
receiver sampling of sub-texel silhouettes; it does not justify changing the
alpha contract or claiming that Issue #4 is visually fixed.

Artifact: `AutoTest/artifacts/shadow_alpha_coverage_abba/20260811_020854`.
