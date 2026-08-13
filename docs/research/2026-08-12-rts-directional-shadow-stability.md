# RTS directional-shadow stability candidate

## Scope

This note records the sources and equations used by the development-only RTS
directional-shadow candidate. It is not a Virtual Shadow Map port and does not
change the Release default. The implementation is independently expressed and
does not copy Unreal Engine source, comments, or assets.

## Primary references

- Microsoft, *Cascaded Shadow Maps*:
  https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps
- Microsoft, *Common Techniques to Improve Shadow Depth Maps*:
  https://learn.microsoft.com/en-us/windows/win32/dxtecharts/common-techniques-to-improve-shadow-depth-maps
- Epic Games, *Virtual Shadow Maps in Unreal Engine*:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine
- Andrew Lauritzen, Marco Salvi, Aaron Lefohn, *Sample Distribution Shadow
  Maps* (Intel publication and original course material):
  https://www.intel.com/content/www/us/en/developer/articles/technical/sample-distribution-shadow-maps.html

The Epic EULA checkout at
`E:\Mycode\Source\References\UnrealEngine` was inspected read-only for the
architecture of directional clipmap levels, fixed world-space level radii,
origin quantization and conservative fallback. No code or text was transferred
to WarVK.

## Mapping to WarVK

Microsoft identifies two independent causes relevant here: insufficient
shadow-texel density produces perspective aliasing, while continuously changing
light projection bounds produces shimmering. WarVK already uses four 4096 CSM
layers, a stable light basis, a StableSphere fit and light-space texel snapping.
That stabilizes the matrix but the three-dimensional frustum sphere can spend
much of its area away from the RTS ground receivers, so distant thin foliage can
still have too few world texels.

Epic's directional clipmaps keep equal-resolution levels whose world radii grow
by powers of two and allocate work only where receivers require it. WarVK does
not have a virtual page table or Nanite-style GPU page marking, so this candidate
borrows only the architectural invariants: fixed world texel size per level,
power-of-two level growth, a quantized origin and explicit coverage validation.

For cascade `c`, resolution `N` and base world texel size `t0`, the candidate
uses:

```
t(c) = t0 * 2^c
R(c) = N * t(c) / 2
```

The sub-frustum is intersected with a conservative horizontal receiver height
band instead of a single zero-thickness plane. Its vertices are projected onto
the light right/up axes, expanded by a fixed world-space padding, and the center
is snapped to `t(c)`:

```
C' = round(C / t(c)) * t(c)
```

The candidate is accepted only if the entire padded footprint remains inside
`[C' - R(c), C' + R(c)]` on both axes. Non-finite inputs, an empty intersection,
or failed coverage return invalid and the existing StableSphere calculation is
used. This makes the experiment fail-closed: it cannot silently trade receiver
coverage for sharper shadows.

## Deliberate exclusions

- No virtual page table, cached VSM page or UE invalidation implementation.
- No random Poisson rotation, hashed alpha, TAA or larger PCF footprint.
- No change to split selection, replay, caster identity, Arena or point shadow.
- No Release environment-variable route. The DLL must be configured with
  `-Dwarvk_rts_shadow_candidate_dev=true`, and mode `1` must then be requested
  explicitly.

## Evidence and remaining gates

The value test covers finite input, fixed density, sub-texel camera motion,
one-texel origin steps, zoom/coverage failure, Y-up/Z-up and signed light bases.
It proves only the numeric policy. A development DLL still needs fixed-camera
and slow-pan framebuffer ROI comparison for terrain, trees, grass and cascade
boundaries. It must not become a Release default without that physical visual
gate and a no-regression workload measurement.
