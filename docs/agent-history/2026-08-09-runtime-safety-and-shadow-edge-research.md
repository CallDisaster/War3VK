# Runtime safety closeout and moving shadow-edge research (2026-08-09)

## Scope and boundary

This candidate closes the source-level runtime risks found during the release
audit and records the follow-up investigation of continuously crawling or
jumping shadow edges.  The safety changes were compiled and exercised by the
offline/static test matrix.  No DLL was deployed and Warcraft was not started.
The shadow algorithm findings below are a design and validation handoff; they
do not claim that the visual artifact has already been fixed.

The audit preserved the unrelated Hotfix3 release-freeze edits, external
`StormBreaker` worktree state, `PlayerCrash/`, and existing build logs.

## Source-level risks closed

### 1. Legacy `warvk:cmd` authority was part of the release binary surface

The legacy JASS shader-command bridge is now compile-time denied unless the
dedicated developer capability
`WARVK_ENABLE_LEGACY_JASS_COMMANDS_DEV=1` is explicitly present.  The normal
DLL build does not define it; only the paired developer policy runnable does.
There is no environment-variable bypass.  Even a developer build rejects an
unknown command and rejects a known command whose public JAPI feature bit is
not published.

The legacy float parser now rejects whitespace/control characters, trailing
data, range overflow, NaN and infinity.  Release-deny, developer-allow and
finite-number behavior are executable contracts in
`war3_jass_legacy_command_policy_{release,dev}_test`.

### 2. A JASS-VM rebuild could release render-owned lightning textures

`Hook_InitJassNatives` now calls the CPU-only `japi::ResetAuthorState()`.
Lightning author records and templates are cleared under the runtime mutex,
but cached D3D textures are not released on the JASS/hook thread.  The full
`War3LightningRuntime::reset()` remains part of the Present-owned map
transition, after the old shadow session has been quarantined.

### 3. Mutable render settings crossed the asynchronous command stream by raw
pointer

External JAPI/UI/bridge writers now edit a shared-lifetime, mutex-protected
mailbox through `War3SettingsWrite`.  The render owner applies one complete
revision at `OnFrameStart`; each queued `War3PipelineInput` owns an immutable
`shared_ptr<const War3RenderSettings>` snapshot.  A queued command therefore
cannot read a torn configuration or a destroyed pipeline member.

Day/night resolution no longer casts away const.  The shadow pass publishes
direction, color and render time to a command-private
`War3FrameLightingState`, while revision-checked mailbox feedback supplies the
next render-owner frame.  A stale CS command cannot overwrite a newer
JASS/UI edit.

The JAPI dispatcher acquires the mailbox only for commands that mutate it
directly.  CPU math/lightning work, point-light-manager calls and delegated
volumetric APIs do not hold the settings mutex across another API call.  This
closes both same-thread self-deadlock and the former settings/LightManager
lock-order inversion.

### 4. Active-device lookup and new-device map identity were not one
publication transaction

Raw `GetActiveDevice()` and `GetActivePipeline()` escape APIs were removed
from `src/d3d9`.  External users run inside `RunWithActiveDevice`, use an
identity-only `IsActiveDevice`, or query `HasActivePipeline`.  Constructor
publication, destructor revocation, map-reset request/fallback and the
Present transition share the publication mutex.

A successfully constructed replacement device now mints and commits a fresh
process-monotonic map epoch inside the same serialized CPU semantic reset that
publishes the device.  Epoch minting occurs under the semantic writer mutex,
so a constructor cannot reserve epoch N, lose to a no-owner fallback that
commits N+1, and later roll global registries back to N.

The active-device/settings pair publishes in fail-closed order: active device
then mailbox on construction, mailbox revocation then device revocation on
teardown.  Already acquired writers keep only the retired mailbox alive and
cannot dereference a destroyed pipeline.

The current `RunWithActiveDevice` transaction deliberately holds a recursive
publication mutex across the complete callback because shadow Arena allocation
can re-enter it on the same render lane.  Warcraft's current device is not
created with `D3DCREATE_MULTITHREADED`, and the audited DIP/upload/flush calls
remain on that lane, so the independent lifecycle review found no release
blocker.  If WarVK later supports a multithreaded D3D9 device, replace this
callback-wide mutex ownership with an explicit lifetime lease before allowing
other threads to enter the device.

### 5. D3D9 Reset could mix a new device epoch with old receiver resources

`Reset`/`ResetEx` now close all shadow admission before GPU-skin device rebind.
A committed new device epoch is only a request; Present quarantines the old
Arena/session, emits the completion signal, retires device-stamped shadow
owners, and CS-orders `War3ShadowReceiverPass::InvalidateMapEpoch` before it
marks the epoch applied.  Every producer gate checks map request/applied,
device request/applied, session-ready and rebind-pending state.

If `SetDevice` fails, the rebind-pending bit stays set across retry frames and
the system fails to no shadow.  Neither equality with the old epoch nor an
internal test command can reopen it.  A map transition may coalesce an already
committed device transition, but cannot authorize an uncommitted rebind.

### 6. Map-B could inherit CPU identity aliases before its first populated
collection

The common CPU semantic reset now clears the process-global render queues,
model/pose/attachment registries, render state, semantic contracts, packet
caches, alpha payloads and shadow metadata without touching GPU owners.
`SceneCollector::CollectWorldObjects` invalidates tracked handles, raw pointer
maps, resolver state and `CUnit* -> handle` TLS at function entry, before all
empty/malformed-world early returns.

The internal map-reset probe now publishes only a Present request.  It no
longer calls the map reset directly or mistakes an asynchronous request for an
immediately applied native-bridge generation.

## Verification boundary

At closeout, the candidate passed:

- all 76 static AutoTest scripts;
- all 20 configured Win32 Meson runnable tests;
- the production DLL link and a subsequent `ninja -C build32 -n` no-work
  check;
- focused JAPI safety, settings-mailbox, cross-map lifecycle, persistent
  package and residency-census contracts;
- `git diff --check` (line-ending conversion warnings only).

This proves source/build/offline contracts.  It does not replace a foreground
map test, Reset/device-lost test, cross-map A-to-B test or player visual
acceptance.  No deployment was performed.

## Shadow-edge investigation

### Runtime evidence narrows the current artifact to DirectInline

The existing report
`E:/Work/War3/WarVK/Log/war3_perf_report_2026_08_08_22_51_32.html`
contains 3,794 receiver frames with requested/effective/shader mode all
DirectInline.  Visibility, motion-vector, history-write and history-advance
counts are zero.  Requested/effective CSM resolution is 4096 and adaptive
resolution is unused.  Consequently that captured run cannot be explained by
Temporal TAA, a 2048 fallback, or adaptive-resolution switching.

### P0-D1: CSM filters depth before comparing it

The default is `War3ShadowFilterMode::Linear`.  Both shadow samplers are
ordinary non-comparison samplers; the default path selects the linear one.
`war3_shadow_receiver.frag` and `war3_shadow_visibility.frag` call
`texture(sampler2DArray(...))` and then perform one software
`refDepth <= filteredDepth` test.

That operation is not PCF.  With neighboring depths `{0.2, 1.0}`, a horizontal
midpoint and reference `0.5`, the current path filters to `0.6` and returns
fully lit `1.0`.  Correct compare-then-bilinear semantics compare to `{0, 1}`
and return `0.5`.

This is exactly the ordering rejected by the original Reeves/Salesin/Cook PCF
paper: depth values must be compared first and the binary results filtered
afterward.  Microsoft likewise documents that directly filtering depth merely
moves the hard edge.  See:

- https://doi.org/10.1145/37401.37435
- https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps
- https://docs.vulkan.org/spec/latest/chapters/samplers.html
- https://docs.vulkan.org/spec/latest/chapters/textures.html

Recommended implementation:

1. Keep a raw-nearest sampler for PCSS blocker search, R8 caster mask, debug
   depth and point shadows.
2. Add independent compare-nearest and compare-linear samplers with
   `VK_COMPARE_OP_LESS_OR_EQUAL`.
3. Split the receiver push constants into raw and comparison sampler indices.
4. Use `sampler2DArrayShadow` for CSM PCF.  Linear/Nearest should select
   comparison filtering, not prefiltered raw depth.
5. Use the existing software compare-first 2x2 implementation in
   `war3_volumetric_light.frag` as the numerical reference.

Changing only to raw-nearest is a diagnostic, not the release fix: sub-texel
kernels can collapse several taps onto one texel.

### P0-D2: default Poisson rotation is a periodic world-space stripe field

The default uses Poisson16 with rotation enabled.  Both advertised Screen and
World modes currently enter the same `rotateMode > 0.5` branch.  Its seed is:

```glsl
fract(dot(worldPos.xy, vec2(0.03125, 0.015625)))
```

It repeats every 32/64 world units along the axes and rotates rapidly along a
linear phase ramp; it is neither a hash nor blue noise.  The visibility pass
already disables it because feeding that pattern into Temporal shows up as
edge crawl, yet the default DirectInline path still uses it.

Recommended first release behavior is rotation Off plus a fixed zero-centroid,
pair-symmetric disk/tent kernel.  Stochastic rotation should only be revisited
after the Temporal path is correct and should use a real spatiotemporal
sequence, not the current periodic dot product.  Relevant primary work:

- https://research.nvidia.com/publication/2021-12_scalar-spatiotemporal-blue-noise-masks
- https://eheitzresearch.wordpress.com/762-2/

### P1-D1: StableWall hard-switches to a quantized, 4-5x wider footprint

The ordinary default footprint is approximately a 0.7-texel Poisson disk.
StableWall is selected by hard thresholds derived from depth-reconstructed
normal, view grazing and moving-light grazing.  It then floors UV to a texel
center and switches to a 5x5 grid with minimum radius 1.50 in Direct and 1.75
in visibility.  Because the grid offsets are `[-2,2] * radius`, support jumps
to about +/-3.0 or +/-3.5 texels.  Threshold toggling changes filter family and
footprint by roughly 4-5x; the `floor` adds whole-texel steps.

Add explicit StableWall Off/Current/Factor diagnostics.  After correct PCF and
receiver-plane depth are available, remove the UV floor and use one shared
deterministic kernel with a continuous factor.  At minimum, Direct and
visibility constants and boundary behavior must be identical.

### P1-D2: directional PCF uses one center reference depth for every tap

All directional taps reuse one `refDepth`, even on sloped receivers.  For a
large kernel, each offset corresponds to a different receiver-plane depth;
the center value produces regular acne/bands that move with sample phase.
Microsoft's CSM sample applies derivatives to the comparison depth for each
offset.  Implement a bounded, finite, kernel-wide receiver-plane proof and use
`ref_i = ref0 + gradient dot offset`; if proof fails, the whole kernel must use
one conservative fallback.  Express bias from per-cascade world texel size
and Z span instead of mixing XY inverse-resolution directly into normalized Z.

### P1-D3: moving sun rotates the light basis despite stable translation snap

The CSM implementation already uses practical splits, stable spheres, radius
quantization, transported basis, double-precision center snapping and a fixed
Z scale.  This is substantially stronger than a naive camera-fitted CSM and
matches the stable-CSM motivation described by Valient/Guerrilla:

- https://www.guerrilla-games.com/media/News/Files/GDC09_Valient_Rendering_Technology_Of_Killzone_2_Extended_Presenter_Notes.pdf
- https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-10-parallel-split-shadow-maps-programmable-gpus

However, translation snap cannot freeze a light basis that rotates every
frame.  The 480-second day and smoothed Warcraft clock make that rotation
continuous, while `stableSnapWhenSunMoving=true` adds discrete center steps.
Run lock-sun/moving-sun and moving-sun snap-on/snap-off A/B only after the two
P0 sampling defects are isolated.  Do not quantize sun time as the fix; it
turns crawl into periodic pops.

## Temporal-only defects (not the current DirectInline root cause)

1. Motion reconstruction and receiver reprojection add pixel-center offsets
   inconsistently.  Under zero motion the current derivation samples history
   at `(pix + 1.0) / size` instead of `(pix + 0.5) / size`.
2. Several receiver early returns skip `imageStore`, history images are not
   explicitly initialized, yet C++ advances the ping-pong after the fullscreen
   draws were recorded.  Undefined/stale pixels can become valid history.
3. History stores `currentViewDepth/currentFarSplit` and compares it next frame
   against a different camera's current domain.  RG linear sampling also mixes
   foreground/background depth.
4. Visibility and Direct have drifted: C1 out-of-bounds handling, StableWall
   radii, clear-depth handling and PCSS availability differ.

Required Temporal repair order:

1. Make zero-motion reprojection an identity; add the center offset exactly
   once at final texture addressing.
2. Clear every write history to `{lit, invalidDepth}` or otherwise prove every
   pixel defined; debug frames must not advance incomplete history.
3. Store device/NDC depth and compare it against the previous-camera depth
   obtained from `worldPos * prevViewProj`; use nearest depth or a manually
   validated 2x2 visibility reconstruction.
4. Share/generated-hash the common Direct/visibility CSM helper and explicitly
   reject Prepass/Temporal when an unsupported PCSS mode is requested.

Temporal reprojection reference:
https://github.com/playdeadgames/temporal/blob/master/GDC2016_Temporal_Reprojection_AA_INSIDE.pdf

## Alpha and point-shadow-specific findings

Alpha casters default to hard cutoff, no mip and a far-cascade cutoff increase
of up to 0.05.  The same leaf therefore has a different silhouette in each
cascade.  First A/B `alphaShadowFarAlphaRefBias=0`; later use
coverage-preserving mip semantics.  Do not default to stochastic alpha before
Temporal is repaired.  Original hashed-alpha research:
https://research.nvidia.com/labs/rtr/publication/wyman2019improved/

The point-shadow surface path already uses raw nearest sampling, a symmetric
kernel, radial depth and per-tap receiver-plane intersection with a whole
kernel fallback.  Do not replace it with one fixed-Dref comparison-linear cube
sample: each cube tap has a different ray/reference.  If its known moire
remains after physical validation, test resolution/radius/bias first and then
the hard tangent-basis branch near the +/-Y poles.  A branchless revised ONB is
a later candidate:
https://graphics.pixar.com/library/OrthonormalB/

PCSS should retain its three distinct stages -- raw blocker search, penumbra
estimate, then PCF -- as specified in the original technique:
https://developer.download.nvidia.com/SDK/9.5/Samples/MEDIA/docPix/docs/PCSS.pdf

## Execution and physical validation plan

1. Add test-only controls for comparison filtering, rotation and StableWall;
   add an edge-continuity analyzer sampled at 0.03-0.05 seconds.
2. Implement split raw/comparison samplers and compare-first CSM; add the
   2x2 `{0.2,1.0}`/reference-0.5 regression.
3. Default rotation Off and install a deterministic symmetric kernel.
4. Remove StableWall UV floor/hard family switching and unify Direct/prepass.
5. Add directional receiver-plane reference and world-scale cascade bias.
6. Repair Temporal coordinates, full history definition and previous-camera
   depth domain; only then evaluate Temporal as a quality option.
7. Restore alpha coverage parity across cascades.
8. Physically validate the existing point-shadow receiver-plane candidate.

The physical matrix must independently cover opaque directional receivers,
wall/low-angle receivers, cascade boundaries, alpha foliage and point debug 6.
Metrics should include changed-pixel fraction, mask IoU/Jaccard, bidirectional
contour-distance p95, temporal standard deviation and signed contour motion.
The existing temporal MAD mask excludes high-change edge pixels and is not a
valid metric for this artifact.  Foreground visual acceptance is required;
isolated-desktop automation cannot replace it.
