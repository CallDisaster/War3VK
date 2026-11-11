# 2026-10-10 Weather render lane receipt

Scope: programmable outdoor snow for the authorized music/weather v1. This
lane owns only new WeatherPass, snow shaders, source/ABI checks and this report.
Shared pipeline, JAPI, Froxel integration and full build are root-owned.

## Implemented source

- `src/d3d9/d3d9_war3_weather.h/.cpp`
- `subprojects/war3fx/shaders/war3_weather_snow_vert.vert`
- `subprojects/war3fx/shaders/war3_weather_snow_frag.frag`
- `AutoTest/test_war3_weather_shader_contract.py`
- `AutoTest/test_war3_weather_spirv_abi.py`

The original workorder's `.vert`/`.frag` names had the same basename and would
collide in generated headers. Root authorized `_vert.vert`/`_frag.frag`, matching
`war3_weather_snow_vert.h`/`war3_weather_snow_frag.h` and their symbols.

## Rendering and bounded motion

The pass records one `cmdDraw(6, 16384, 0, 0)` at BeforeUi. There is no vertex
buffer, particle storage buffer, CPU per-particle loop, compute ping-pong or
GPU readback. Each world cell owns 16 procedural particles; a 32 x 32 set of
96-unit cells covers a bounded neighborhood near the camera.

Cell identity uses world lattice coordinates, seed and lane. Camera movement
selects the nearby lattice window; camera rotation only changes projection.
Horizontal transport consumes the runtime's continuous wind integral. It does
not multiply current wind by elapsed time. A bounded oscillation adds gusts;
vertical descent recycles with fade at the top/bottom. Region and distance
feathers fade particle entry/exit, including cell-window changes.

Wind displacement reduces by 786432 world units. Hashes repeat at exactly the
same 8192-cell period, so reducing the displacement does not change the physical
particle distribution. Root owns matching periodic volumetric density code.

All shaders share the exact 128-byte scalar/row-major push layout with C++.
The eye XYZ is explicit. Media coefficients are packed at 8-bit precision for
display; author/runtime API values retain full float precision. Wind speed and
angle are consumed by the runtime integral. There are no CPU gameplay effects.

## Depth, synchronization and resource ownership

Legal directly sampled depth is preferred. The original image must have sampled
usage, the format must support sampling, and views must identify one single-
sample mip/layer matching the target extent. A depth-only 2D-array sample view
is used. The exact original image layout is queried, transitioned and restored;
combined depth/stencil aspects stay together. Color uses compatible attachment
layout (including GENERAL) and LOAD, then returns to its exact original layout.

Production D3D9 attachment-only surfaces can lack sampled usage. Root authorized
a fallback when the source has transfer-source usage: one lazy depth-copy image
with sampled/transfer-destination usage. Every reuse records the preceding
fragment-read -> transfer-write -> fragment-read dependencies through
War3OwnedImageLayoutState; it never assumes that N frames means GPU retirement.
The source is restored immediately after copying its depth aspect. The copy is
then used by this draw. Format/extent replacements are transactional.

The retained copy allocation has a 32 MiB hard limit, checked by minimum pixel
size before creation and actual `storage()->getMemoryInfo().size` afterwards.
At 2560 x 1440 a 4-byte format's pixel payload is about 14.06 MiB; this is
arithmetic, not an actual device measurement. Unsupported MSAA and other illegal
targets produce named diagnostics and no weather draw. Allocation/pipeline
failures before recording produce named unavailability rather than changing
existing shadow/world logic.

The command list tracks original depth/color, fallback copy, sampler and
War3TrackedVkPipeline owner until GPU completion. Destroying a pass or replacing
a cache reference therefore cannot destroy its pipeline/allocation in flight.
The disabled path allocates nothing. On enabled->disabled it drops owned copy
references; already queued command lists keep their own references until done.

Projection depth polarity comes from the existing production
InferWar3FarClearRaw helper, carried in an unused high sampler-index bit.
Reverse viewport ranges are rejected: the native D3D9 viewport path clamps
such ranges, so copying their unmodified values would mismatch scene depth.
Positive weather viewport plus shader Y-flip matches native negative-height
viewport orientation; both X/Y retain native's 0.5-pixel raster offset.
Fragments reject occluded snow and soften intersections in normalized depth.
Softness is an approximation, not a proven constant-world-distance intersection.

## Snow appearance and medium integration

Snow outputs premultiplied RGB/alpha and blends ONE/ONE_MINUS_SRC_ALPHA. Target
alpha is preserved. Flakes rotate and vary size in world-derived projection;
they do not use additive emissive blending or become per-flake shadow casters.

The fragment shader includes root's `war3_weather_density.glsl`. The eye->particle
ray is clipped to the weather AABB, then four bounded midpoint samples integrate
only that interval using haze/lowSnow/gust and the same bounded region. This
prevents a thin weather region from being missed by all four whole-ray samples.
The interval is limited by the flake, not the opaque scene surface. Finite sample
count still approximates spatial density and may need visual tuning.

Outdoor volume is the acceptance scope. Automatic roof shelter, collision,
terrain-height following, footprints and surface accumulation are not present.
Cold-white snow has fixed base reflectance; local light/color adaptation remains
an artistic limitation. War fog, translucent effects and real-camera readability
need actual game review.

## Diagnostics

QueryWar3WeatherDiagnostics returns one mutex-protected value. The unused system
does not lock per frame; an off transition publishes once. Fields include stage,
fixed capacity, estimated active particles, current copy allocation bytes,
frame/map/device/id/time and whether `cmdDraw` was recorded.

`activeParticles` is a CPU expectation from snowfall intensity, before region,
frustum and depth tests, not a GPU surviving-particle count. `resourceBytes`
describes current extra particle/image backing in this pass, excluding retired
allocations still tracked by command lists, sampler/pipeline driver allocations
and Froxel textures. `submitted` means commands recorded, not GPU completion or
visual correctness.

Named rejection stages include missing targets, stale frame/epoch, missing or
invalid exact-frame camera, unsupported MSAA/color/depth, undefined layout,
depth-copy-budget, depth-copy-unavailable, pipeline-unavailable and duplicate
frame. A successful pass does not execute twice for the same frame/map/device.
The diagnostics also retain the exact enabled frame's WeatherFrameSnapshot,
so readers can inspect the actual consumed parameters. Off transitions carry
an empty snapshot. MAX_ENUM divergent-layout sentinels are rejected before any
recording instead of becoming invalid Vulkan barrier layouts.

## Verification in this lane

- `python AutoTest/test_war3_weather_shader_contract.py`: 25/25, EXIT 0.
- `python AutoTest/test_war3_weather_spirv_abi.py --build-dir build32_weather_20261010
  --report AutoTest/artifacts/weather-night-20261010/spirv-abi-reflection.json`:
  18 checks PASS, 0 failures, EXIT 0. This reads actual compiled uint32 shader
  headers generated by root's build; this lane did not launch a compiler.
- `git diff --check` for this lane's files: EXIT 0 (new untracked files also
  received the source checks above).
- ABI tests parse actual C++ and both actual GLSL declarations and compare
  names, offsets and sizes, rather than merely checking a declaration string.
- Additional checks cover immutable input, budget, depth transfer dependency,
  exact layout restoration, live-owner tracking, premultiplied blending and
  eye->flake medium path.
- Reference math checks cover world identity under camera rotation, integral
  continuity across wind change, periodic displacement wrapping, native narrow
  positive depth-range bias and a thin ray-box medium counterexample. These
  are source/reference-math tests, not actual GPU execution.
- Compiled reflection verifies the snow VS/FS push-constant member offsets
  (0,64,80,96,112,116,120,124), 128-byte extent, RowMajor and MatrixStride=16;
  actual Froxel/Legacy FogVolumeBlock weather offsets (528,544,560,576), 592-byte
  extent; execution stages and descriptor set/binding/types against C++ layouts.
  Four modules were reflected: snow vertex 2735 words, snow fragment 4184 words,
  Froxel inject 7557 words, Legacy volumetric 27301 words. Missing or stale
  generated headers cause WAIT/exit 2, never a skipped pass.

## Read-only follow-up audit boundaries

The 32 MiB limit caps the currently retained depth-copy allocation. On rapid
resolution/format changes, old allocations may also remain alive until queued
GPU commands complete; peak resident memory is not proven <=32 MiB. Full and
partial viewport X/Y transforms have reference-math parity, not GPU proof.

The native depth viewport's 0.001 lower-depth-range bias when MinZ<0.5 is now
copied exactly. The legal [0.2,0.2001] counterexample expands to [0.2,0.201] in
both native and weather reference mappings rather than using the raw MaxZ.
Current camera snapshots do not distinguish whether native scissor was enabled;
weather clips to the world viewport, so native sub-viewport scissor equivalence
is not claimed. The radius-bounded camera neighborhood can under-cover a far
zoomed-out target. Four clipped midpoint medium samples still approximate density
variation within the weather interval; they are not exact quadrature.
These limitations are reported rather than described as covered visual gates.

No shader/DLL build or game launch was performed by this lane. Root owns that
lease and will attach actual integration results. No deployment, commit or
release action occurred.
