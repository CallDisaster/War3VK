# Native point-light transactional color candidate

**18:20 revision: the two-light relighting design below is superseded, not
accepted.** R8-R12 found a visible regression. Current candidate isolates ONE
native contribution, preserves its original FFP response, and only modulates
its visibility. Other native lights stay original; authored lights stay separate.

Implementation checkpoint, not release acceptance. The successful isolated
producer run `v122_native_lights_20260914/native_light_producer_r2` proves actual
stock MPQ bytes, template/clone ownership and native selection. It does not
prove enhanced lighting or cube-shadow output. R1's fixed-load-base rejection
remains invalid; R2 validates pinned PE relocations without disabling ASLR.

## Why a second color attachment

D3D9 FFP combines emissive, ambient and per-light material-weighted diffuse and
specular before saturation, interpolation and texture-stage operations.
Subtracting a guessed light from the final scene color cannot invert those
operations. Native direct suppression before a late receiver pass is unsafe:
that pass can reject its inputs after earlier draws have finished.

Keep RT0's original computation and native calls unchanged. On an admitted
draw, initialize a private same-format color attachment from RT0, then emit
both original and alternate FFP colors in the same draw. The alternate removes
only generation-bound reviewed point-light direct/specular terms; ambient,
sun and all unclaimed lights remain. Run the same texture stages and fog on
both colors. Alpha testing and coverage use the original output. Reject
operations where alpha depends on changed RGB, programmable shaders, feedback,
MSAA, extra application render targets, incomplete coverage and stale leases.
Independent blending uses the original RT0 mask and alpha-swizzle semantics
for the alternate attachment, but each attachment's own destination value.

At the late receiver, use the alternate only with a complete same-frame lease,
all automatic lights within the fixed budget, and successful cube/receiver
resources. Final fullscreen rendering is the commit. Every earlier failure
discards the alternate: RT0 already contains the complete native fallback.
Never expose native pointers to GPU/worker code. Author lights retain priority;
automatic lights do not allocate or steal JAPI handles.

The first candidate used exclusive author priority. R3 correctly rejected
automatic takeover because the user's copied test map contains an authored
JAPI light; that run is not consumer acceptance. The revised candidate packs
the frozen author shadow prefix first, then at most two automatic lights,
then remaining author direct lights, within 4 cubes/16 lights. On an automatic
failure, reconstruct the exact frozen author snapshot, use original settings
and make at most one bounded canonical shadow attempt with the existing replay
references. Never deep-copy the scene or re-query mutable author state for
fallback. A diagnostic forced fallback will test this path separately.

The extra attachment costs GPU bandwidth, not a second geometry draw. Measure
that cost and inspect actual shadow images before delivering a player candidate.
Specialized FFP shaders must carry both interfaces; an existing ubershader
without the extra output cannot participate silently.

R6 first actual receiver commit: two stock LanternPost lights / twelve complete
cube faces. R7 forced failure retained the native baseline, no automatic commit,
four screenshots and normal exit. Both preserved player DLL/map with no GPU
events/dumps. R6 actual paired SPIR-V passed independent validation. Images show
changed local illumination; occluder-specific visual proof remains pending.
The first-two-draw policy preferentially filled the budget with the original
map's lamps. Revised admission uses the preceding complete census only to rank
two generations by projected screen-center distance. Actual values and lifetime
are always claimed and validated afresh; new lights wait one census, unsupported
or destroyed lights remain native. This does not grant cross-frame data reuse.

## Primary references and mapping

- [Microsoft D3D9 diffuse lighting](https://learn.microsoft.com/en-us/windows/win32/direct3d9/diffuse-lighting):
  material-weighted per-light terms and post-lighting color processing map to
  `D3D9FFShaderCompiler::compileVS`; keep its original branch intact.
- [Khronos framebuffer blending](https://docs.vulkan.org/spec/latest/chapters/framebuffer.html):
  attachment-local blending maps to a private RT1 with RT0's blend settings.
- [Khronos shader interfaces](https://docs.vulkan.org/spec/latest/chapters/interfaces.html):
  explicit vertex/fragment linkage and fragment output locations map to
  private COLOR2/3 varyings and color output location 1.

This is an independent WarVK implementation; no third-party engine code copied.

## R8-R12 visual rejection and corrected transport

8D49D6321D9634BAF5968AE20C1C7765A68460A4881A3844C218E6FA3A475364 /
35,441,370 bytes submitted actual TorchHuman + LanternPost cubes, including
coexistence with one authored JAPI light (3 lights / 18 faces). This was not a
visible acceptance: R8/R9 flames were visible but nearby illumination was weak.
R10 debug mode 6 showed most of the Torch footprint occluded. R11 forced native
fallback visibly illuminated the ground/footman. Ground Z=0, light Z=91.1175,
so the light was not below the terrain. R12 with
1DA5AF69D8F5B744983E45F0592D5678857387446B3CC377D16EF368E1C9BE06 /
35,441,602 bypassed ONLY native shadow intensity, retained takeover/cubes, and
still looked weak. Therefore visibility alone is not the complete root cause.
All used isolated 2560x1440, normal exit 0, restored A570; player B1FCC and
author map unchanged, zero recorded GPU events/new dumps. These are correctness
diagnostics, not foreground performance or release acceptance.

Two separate issues:

1. The existing late authored-point receiver multiplies already-lit color by a
   new diffuse light. Its input is NOT albedo. Removing native direct light then
   using that dark counterfactual as albedo cannot preserve original lighting.
2. The pinned TorchHuman opaque cup extends to Z=102.507; its light pivot is
   (0,0.420795,91.1175), inside that geometry. The read-only pinned geometry tool
   `analyze_native_torch_emitter_geometry.py` finds 86/96 downward rays blocked
   by its first opaque geoset. This is asset evidence, separate from proving
   which runtime caster belongs to the light.

Corrected candidate formula (one light, arbitrary original FFP texture/fog and
blend calculation preserved independently in the two attachments):

`C(V) = C0 - (C0 - C1) * (1 - V)`

`C0` is actual original color; `C1` is its exact draw-time counterfactual without
the one claimed native direct/specular contribution. At V=1 the change is zero;
at V=0 it is C1. No albedo inversion, guessed brightness, additive replacement
or color clamping is used to derive the contribution. Existing sun modulation
is applied to both endpoints equally; authored lights remain the original path.
The native slot is skipped by that authored relighting loop. One native light
is an explicit first-candidate budget: two independently shadowed lights cannot
be attributed from a single combined C1. Do not silently reinstate two.

Exact source/clone generation is revalidated before recording emitter fallback
indices. Only same-frame fallback snapshots whose runtimeModelPtr equals that
validated owner can be excluded, and only from private light ID -1. Other
lights still see the emitter geometry. Unknown owners are NOT guessed by range,
model name, or bounds. Worker input contains replay indices, no native pointer;
the persistent experimental planner is not eligible for this transaction.

The MRT image stays GENERAL at the external-render boundary. An explicit
COLOR_ATTACHMENT_WRITE -> FRAGMENT_SHADER/SAMPLED_READ dependency publishes
it for the receiver; command-list Rc tracking protects in-flight lifetime.
The binding is sampled only on a fully committed native slot. This maps to the
[Vulkan memory-dependency contract](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).
The native diffuse/material, saturation and attenuation contract is the
[Microsoft D3D9 lighting equation](https://learn.microsoft.com/en-us/windows/win32/direct3d9/diffuse-lighting)
and [attenuation definition](https://learn.microsoft.com/en-us/windows/win32/direct3d9/attenuation-and-spotlight-factor).

R13 (2D9720A427BF3945CB27F43469CEFF724691F05588A224A6567DD7F32B050A3D /
35,450,347 bytes) is INVALID for visual acceptance: new shader binding 14 was
written but omitted from the manually authored descriptor-set layout. The
image showed a black region. Its runner ok=true only denotes process/restore
closure, not a shader or lighting pass. No GPU event/dump occurred; this does
not validate descriptor correctness. The layout is now extended to 15 bindings
and static tests compare both layout declarations and descriptor writes.

R13 also logged zero exact emitter fallback matches. The improved owner proof
is now annotated at three actual caster publication sites as a non-wrapping
generation, with optional draw-time enrichment accepted only when the exact
visible record matches its cache key. Both semantic caster and fallback indices
can be mapped to the final replay list. No shared-part-only owner lookup is
used: a part may belong to multiple instances. Exclusion remains restricted
to the independently audited policy 8 / 98D1 TorchHuman resource; lamps embedded
in walls do not cause an entire wall to disappear from its own shadow map.

46 targeted static/algebra cases and 52 existing point-shadow cases pass.
Embedded receiver SPIR-V passes spirv-val with the actually enabled Vulkan
scalarBlockLayout feature; running without that feature flag rejects the
pre-existing scalar uniform padding, not the new instruction stream.
An intermediate native-owner annotation build failed because the draw-time
cache entry has no runtimeModelPtr; it now uses only the exact matching visible
record. Corrected build and isolated visual/fallback checks remain pending.
No corrected DLL has been delivered.
