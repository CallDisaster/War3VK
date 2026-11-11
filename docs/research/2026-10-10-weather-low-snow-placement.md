# Low-snow placement and thin-layer audit — 2026-10-10

Read-only lane: `/root/weather_api`. Source inspection at Taipei approximately 04:07; existing camera receipt added at approximately 04:10.
Scope: explain why the isolated `lowSnow=0.8` phase was reported by the integration owner as visually close to baseline. This lane did not modify production code, raise a budget, build, launch a game, sample runtime terrain, deploy, commit or publish. The only authored file is this report.

## Plain-language conclusion

**The central canary scene does not support the hypothesis that its low-snow layer is buried below the terrain.** Both the original map and the actual query-after candidate contain the same terrain member. All 169 terrain vertices within 768 world units of the camera target are flat at Z=0, with no ramp flags. The fixture sets the weather bottom to Z=0. Moving that bottom upward without further evidence would mask a different problem rather than fix a demonstrated placement error.

There are two separate issues to evaluate:

1. The current low-snow model is a narrow, exponentially thinning volume. Under a normal downward view, even its ideal integrated effect is much weaker than the separate all-height haze knob. `lowSnow=0.8` does not mean that 80% of the screen should become white. A diagnostic CPU ray estimate gives about 13% opacity before gusts, not a full blizzard.
2. There is a concrete coordinate-contract mismatch in the current Froxel consumer: injection and temporal reconstruction describe a **viewport-local** normalized grid, while integration reconstructs the correct viewport-local world ray but samples that grid with **full render-target** UV. When the world viewport is smaller than or offset within the backing image, those are different rays. A thin layer is especially sensitive to this difference. Whether this particular physical run had a non-full viewport remains a runtime evidence gate; the source mismatch itself is directly inspectable.

Separately, fixed 64-slice/2-sample Froxel integration can underestimate a very thin ground layer near the final surface slice. The simplified CPU probe below demonstrates the mechanism but does **not** certify the actual GPU error or identify it as the sole visual cause.

## 1. Frozen artifact and observation identity

The inspected physical-run receipt is:

`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/AutoTest/artifacts/weather-night-20261010/canary-query-after/run.json`

- Candidate DLL: 34,451,456 B, SHA-256 `D93A5065FE94C2A95D5C647467490DC7E601FB96C87267A162094D1E4D5C85E4`.
- Candidate map: 5,875,516 B, SHA-256 `C34DADFC1DC566605AEB5E2426939BEE455D53C202A8862B9DCE597CB8A6B2D1`.
- Source map: `E:/Work/Warcraft III/Maps/Test/WorldEditTestMap.w3x`, 5,861,385 B, SHA-256 `692E92F43BAC99EF8ECED7833E137B22475914F592DA7F78FCC24B54C221E95F`.
- Receipt records protected source map, player DLL and Game.dll as unchanged after the test.
- This lane did not re-open unchanged screenshots; the visual statement comes from the integration owner's already completed image review, not a new visual certification by this lane.

A receipt snapshot during the isolated low-snow phase contains:

| Field | Recorded value |
| --- | --- |
| JASS stage / elapsed time | 3 / 48.5 s |
| Frozen frame / map epoch / device epoch | 14864 / 1 / 1 |
| Frozen weather id / revision / history revision | 1 / 8 / 3 |
| Snowfall / haze / lowSnow | 0 / 0 / 0.8000000119 |
| Wind angle / speed / gust | 90 / 100 / 0.2000000030 |
| Weather volume witness | `composite-submitted`, same frame and epochs |
| Witness extent / current resource bytes | 2560×1440 / 38,233,088 |
| Ordinary volumetric witness | not run / not submitted |

This rules out “the requested lowSnow value never reached the observed render input” for that sampled frame. A submitted composite does not prove density, pixel correctness, visual quality or peak allocation.

## 2. The layer follows the authored world-Z bottom, not the terrain

Current source anchors, all under
`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/`:

- `AutoTest/weather_canary_20261010.j:60`: region `(-3500,-3500)..(3500,3500)`, bottom Z=0, top Z=2200, feather=0.10.
- Same file, lines 185–187: target XY=(0,0), requested target distance=1800, requested attack angle=305 degrees.
- `src/d3d9/d3d9_war3_volumetric_light.cpp:1964`: region bottom/top and haze/lowSnow/gust are copied directly into the weather UBO.
- `subprojects/war3fx/shaders/war3_weather_density.glsl:64`: `low = exp(-max(p.z - regionMin.z, 0.0) / 96.0)`.

The low-snow term is `mask * gust * clamp(lowSnow,0,1) * low * 0.0015`. The 96 units are an **e-folding height**, not a hard 96-unit slab. XY sides and the top feather out; the bottom does not feather. Samples outside the authored box still return zero.

| World Z above the region bottom | Relative low-snow density |
| ---: | ---: |
| 0 | 1 |
| 96 | 0.367879 |
| 192 | 0.135335 |
| 256 | 0.069483 |
| 384 | 0.018316 |
| 512 | 0.004828 |

There is no terrain query or terrain-relative rebasing in this handoff. Therefore high terrain elsewhere in a map genuinely can be above almost all of this layer. That is a **product semantic limitation**, but it must not be used to explain a flat-Z=0 central test that contradicts it.

## 3. Read-only W3E evidence contradicts the buried-layer explanation at the target

The two archives were opened with the existing local StormLib ABI (`SFileOpenArchive` read-only flag 0x100). `war3map.w3e` was read into memory and parsed without extraction or archive mutation. The parser accepted only the observed W3E v11 seven-byte vertex layout, checked counts/total bytes and refused ambiguous ground bit15.

Height interpretation follows the current FORGE parser, not a height inferred from a screenshot:

- `E:/Mycode/Source/Repos/War3MapEditor64/src/Core/MapData/MapTerrain.cpp:163–181`.
- `E:/Mycode/Source/Repos/War3MapEditor64/include/War3MapCore/MapData/MapTerrain.h:41–42`.
- Formula: `(groundRaw-8192)/4 + 128*(cliffLevel-2)`.
- This is the raw terrain layer grid, not a claim about arbitrary roof surfaces, cliff models, water material offsets or final GPU terrain triangulation.

Results for both the source archive and physical query-after candidate:

| Field | Result |
| --- | --- |
| Member size / SHA-256 | 29,644 B / `AA72F490C635DC2F7F9A1C4EC3075CAE6B8FF9C9D3BB74143C63C41F67FB7445` |
| Version / dimensions / southwest origin | 11 / 65×65 vertices / (-4096,-4096) |
| Whole-map grid Z range | -256..512 |
| Authored weather XY box | 3025 vertices, Z=-256..512 |
| Central target neighborhood, XY ±768 | 169 vertices, all Z=0, zero ramp flags |
| Central nine vertices, XY in {-128,0,128}² | each raw=8192, cliffLevel=2, Z=0, ramp=false |

The archive-level SHAs were also re-read and matched the physical-run preparation receipt. The identical W3E member proves that replacing only the canary JASS script did not rebase or raise this terrain.

This is high-confidence **offline scene-data evidence**. Actual JASS camera target Z and rendered camera matrices remain separate domains.

The integration owner's subsequently completed stress-a receipt includes the already-requested native camera snapshot. This lane only read that saved receipt; it issued no game query:

`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/AutoTest/artifacts/weather-night-20261010/stress-a/run.json`

- Separate DLL: 34,451,456 B / `04E7137C54D83645D0AD1A7BFCF68BEE18E7EB60B1FC8FD52983CDDB432A3DF4`.
- Separate map: 5,875,660 B / `0896255FBFBDFECBA41F9F61DFD7E40C688AAB634CD2B49C8349DE7AABD6330A`.
- Native sample request at Taipei 04:01:49: `ok`, `resolved`, `invoked` and `signaturesValidated` all true, seven field invocations, execution on `war3-main-loop`.
- Target XYZ=(0,0,-1.2550381422), target distance=1800, attack angle=305.0000305 degrees, Z offset=0, FOV=70.0000076 degrees, rotation=90 degrees, far Z=5000.

This does not describe a target located on a Z=512 plateau; its target height is close to the offline central grid Z=0. The approximately -1.26-unit difference also means that “raw terrain grid Z” must not be falsely equated with the actual native camera target Z. It does not prove the live inverse-projected ground plane, viewport or GPU pixels. The snapshot is from a **different artifact identity**, not same-frame evidence for query-after frame 14864. The saved stress-a `runtimeAcceptance` is false, so a successful camera query does not convert that whole run to an accepted test.

## 4. World reconstruction is real inverse projection; the UV domain is the gap

The C++ owner copies `input.scene.worldCamera.invViewProj` directly into the UBO (`d3d9_war3_volumetric_light.cpp:1796`). Camera XYZ is taken from the inverse view matrix at lines 3030–3033, not from normalized depth treated as a height.

Froxel injection (`war3_volumetric_froxel_inject.comp:125–145`):

1. Convert the normalized XY cell coordinate directly to camera clip XY.
2. Multiply clip by the inverse view-projection and homogeneous-divide.
3. Normalize the world-space ray from the camera.
4. Place the sample at the shared logarithmic radial slice distance.
5. Evaluate the common weather density in those world coordinates.

Temporal reconstruction uses that same viewport-local grid convention (`war3_volumetric_froxel_temporal.comp:55–74`) and reprojects to the same normalized camera UV.

Integration (`war3_volumetric_froxel_integrate.comp:62–132`) correctly does a different first step: full output UV → complete depth-image pixel → active camera viewport-local UV. It reconstructs the world ray from **that viewport-local UV** and terminates at inverse-projected opaque depth. But the loop around lines 595–599 samples the volume at `vec3(uv, slice)`, where `uv` is still the normalized **full output** coordinate.

Consequently:

- Full-extent viewport: the domains are effectively the same except the documented D3D9 pixel-center convention.
- Smaller/offset world viewport: injection density and integration surface termination refer to different camera rays.
- Uniform haze is relatively insensitive to such a mismatch; an exponential ground layer is not.
- This affects the shared Froxel path, not merely the weather API or the bottom-Z authoring.

The narrow candidate fix is to expose the existing computed viewport-local UV from `reconstructRayAndLimit` and sample the viewport-local volume with it. Injection and temporal should remain on one shared grid contract. This lane did not implement that change. Any integration change needs shader compilation/reflection, partial/full-viewport reference tests and a new physical A/B identity; source reasoning alone is not a closed visual fix.

## 5. Thin-layer sampling and expected strength: analytical, not GPU proof

Weather currently forces Froxel Medium, with 64 logarithmic radial slices on [20,10000], two injection samples per cell, and temporal accumulation disabled. These values are in `d3d9_war3_volumetric_light.cpp:2840–2855`, `:2140–2195` and `war3_volumetric_froxel_inject.comp:256–278`.

The injected medium includes an independent source term:

`source += weatherSigma * vec3(0.30,0.34,0.40)`

at `war3_volumetric_froxel_inject.comp:242`. Thus “no sun/author fog made all snow ambient disappear” does not describe this inspected source. The Legacy fallback also has independent bounded ambient and clips to the weather box before its 16 probes (`war3_volumetric_light.frag:694–724`).

A bounded PowerShell CPU calculation evaluated a **simplified one-dimensional** normal-view ray with:

- Ground Z=0; bottom Z=0; lowSnow=0.8.
- Assumed camera-to-ground radial distance=1800 and downward angle=55 degrees.
- Camera Z≈1474.474 derived from those assumptions, not claimed as the actual render camera.
- No gust modulation, no XY texture filtering, no projection-pixel effects, no RGB composite/readback and no scene obstructions.

The continuous exponential integral gives optical depth≈0.140633 and opacity≈13.119%. For comparison, the actual haze-only phase uses haze=0.65 over the full available air column; on the same 1800-unit toy ray, its unmodulated opacity would be approximately 47.5%, not 13%. This is why equal-looking knob values cannot establish an equal intended screen effect.

The final logarithmic slice containing the assumed ground hit is radial [1741.465,1919.049], spanning world Z≈+47.949 to -97.519. Its density samples describe the **whole cell**, while integration only uses the short segment above the opaque ground. Because below-box samples return zero, a last-cell sample can discard precisely the dense segment that remains visible.

Using the production two-sample Z-offset pattern over the eight phases in this toy ray gave:

| Phase | Approximate optical depth | Approximate opacity | Fraction of continuous optical depth |
| ---: | ---: | ---: | ---: |
| 0 | 0.113370 | 10.718% | 80.61% |
| 1 | 0.102384 | 9.732% | 72.80% |
| 2 | 0.085055 | 8.154% | 60.48% |
| 3 | 0.103284 | 9.813% | 73.44% |
| 4 | 0.089607 | 8.571% | 63.72% |
| 5 | 0.108791 | 10.308% | 77.36% |
| 6 | 0.093252 | 8.904% | 66.31% |
| 7 | 0.125131 | 11.762% | 88.98% |

This is a reproducible **CPU reference-mechanism observation**, not the live GPU transmittance. It does not prove a hard zero, complete absence of low snow, or a reason to expand slice/sample/VRAM budgets. The physical camera, viewport-local reconstruction, XY filter and actual per-frame view must be closed first. Also, modest optical opacity can produce an even subtler visible RGB difference when the ambient snow tint is close to the underlying ground color.

## 6. Minimum next evidence and acceptance boundary

1. Native target placement is now checked by the existing `camera.snapshot` receipt above. It reports target Z/distance/attack angle and invocation/signature evidence; do not confuse it with the independently captured D3D9 view/projection or claim the failed overall stress-a run passed.
2. Obtain the actual rendered world viewport from existing diagnostics, or add a narrow by-value diagnostic under the existing developer observation gate if none exists. No new geometry scan or terrain capture is required.
3. If the world viewport is partial, correct the consumer UV-domain mismatch first and compare the same frozen map/camera/low-only phase at full and partial viewports. Keep all weather density, budget and artifact identity fields unchanged for a causal A/B.
4. After ray alignment, compare expected ray optical depth with low-only pixels or a bounded render probe. If the effect is correctly weak, report that as the v1 artistic calibration boundary; do not call it a buried layer or silently raise density.
5. Terrain-following snow is a later explicitly authored feature: region bottom now is a flat world-Z datum. Ground-following density would require a separately proven terrain-height representation and lifetime/coordinate contract. A roof/building snow-deposition system is another different feature, not implied by this volume.

## 7. Source identity of this audit

SHA-256 values read before any proposed viewport fix:

- `war3_weather_density.glsl`: `B1E55625CC75AA8AFE3805341458CA486CBFFC1422D56699FF9AD6B37412A965`.
- `war3_volumetric_froxel_inject.comp`: `03130A1492DCF867892FADCA51F3AD251C10885C17638BB494A119952BE020E6`.
- `war3_volumetric_froxel_integrate.comp`: `EB3E5ABF1A38F9931720D9C926CA1C5773FA20F7680BE3DBDAD1AE266FC11231`.
- `war3_volumetric_froxel_temporal.comp`: `686A89BE3232BEEDBA56B8FC7FC55CA95A1DF31C449D8E3159192A591EC2C5DC`.

No “layer buried”, “visual fixed”, “GPU density certified”, “all-map terrain following” or “budget expansion required” conclusion is authorized by this report.
