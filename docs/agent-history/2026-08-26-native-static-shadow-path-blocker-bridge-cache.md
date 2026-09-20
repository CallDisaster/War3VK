# 2026-08-26 native static shadow, path-blocker, and bridge/ramp cache candidate

## Scope

User asked for theory-first work only because current memory and CPU pressure make
Warcraft/AutoTest results unreliable. This pass did not launch Warcraft, YDWE, or
AutoTest. It uses source audit, IDA evidence, a Win32 DLL build, and Ninja no-work.

Target issues:

- Disable Warcraft III native/static shadow decals by default.
- Remove bridge/ramp first-see and re-see stutter caused by draw-time shadow VB
  cache churn.
- Stop invisible path blockers from reaching WarVK ShadowMap/CSM.

## IDA evidence

Game.dll 1.27a render-layer shadow producers inspected in the open IDA session:

- `TerrainShadow_RegisterImageEntry` at `0x6f713250` owns the native terrain
  image/stamp slot allocation. It returns `-1` when an entry is not installed,
  so a hook decision returning `-1` is the correct producer-side block.
- `TerrainShadow_ToggleStaticStampFromObject` at `0x6f74db30` calls
  `TerrainShadow_RegisterImageEntry(*(this+37), keyBuf, size, pos, a2+12, 0)`
  on enable and stores the returned slot at doodad entry `+136`. On disable it
  unregisters the previous slot and writes `-1`. Therefore the safe default is:
  block only `enable != 0`, keep cleanup passthrough.
- `TerrainShadow_RegisterImageEntryWithParams` at `0x6f7290b0` and
  `TerrainShadow_RegisterImageEntry_ObjectBridge` at `0x6f76d400` both route to
  the same core register path, so RegisterImage policy must cover default native
  shadow keys even when `NativeShadowMode == 0`.
- Reverse xrefs show the complete `TerrainShadow_RegisterImageEntry` caller set:
  WithParams, selection circle, static stamp, emitter stamp, object bridge,
  MarkColor/Occlusion, FromPoint, and FromTwoPoints. The one initially
  unowned-looking xref at `0x6f76d59f` is the MarkColor/Occlusion branch and
  remains whitelisted.
- `ShadowProjector_Add_Simple` at `0x6f76d790` and
  `ShadowProjector_Add_FromObject` at `0x6f76d800` both allocate/reuse the
  Terrain `+0xB0` projector list through `0x6f7276d0`, then call
  `0x6f713ca0`, which in turn calls
  `TerrainShadow_RegisterImageEntryWithParams`. They are not an independent
  image-slot bypass.
- `ShadowProjector_Add_Bridge` at `0x6f76d8d0` only swaps arguments and jumps to
  `0x6f764ac0`; `0x6f764ac0` performs bridge projector definition lookup/create
  and then calls `ShadowProjector_Add_Simple`. Bridge/ramp native projector
  submission is therefore also covered by the RegisterImage/WithParams policy,
  not a separate shadow producer.
- `ShadowPath_StaticStamp_Toggle` at `0x6f74e420` is a real direct-write bypass:
  on enable it calls `ShadowStamp_WriteByName` at `0x6f713b20`, which prefixes
  `ReplaceableTextures\Shadows\` and reaches `ShadowStamp_WriteCore` at
  `0x6f713920` without passing through RegisterImage. Its disable branch also
  calls `ShadowStamp_WriteByName(..., enable=0)` and must continue to pass
  through for cleanup.
- `ShadowStamp_WriteByName` has two static-stamp calls from
  `ShadowPath_StaticStamp_Toggle`, one short CDoodads cleanup wrapper at
  `0x6f75abb0` that passes `enable=0`, and `ShadowStamp_WriteByName_FromWorld`
  at `0x6f76d720`. The `FromWorld` callers are ActorController, PointController,
  and EffectState update paths, so this pass preserves them rather than
  disabling dynamic actor/effect decals.
- Continuation xref audit confirmed `ShadowStamp_WriteCore(0x6f713920)` has only
  one direct caller, `ShadowStamp_WriteByName(0x6f713b20)`. The audited
  `WriteByName` callsites are `ShadowPath_StaticStamp_Toggle` enable
  (`0x6f74e4c7`), its disable cleanup path (`0x6f74e590`), and
  `ShadowStamp_WriteByName_FromWorld(0x6f76d72f)`. This makes the current
  producer-side StaticStampPath block sufficient for the direct-write static
  stamp family without suppressing runtime actor/point/effect decals.
- `CUnitUIManager_RecordSetStructureShadow` at `0x6f335a00` writes the unit UI
  building-shadow resource at record `+0x50`; the existing UnitUI producer block
  remains valid but was not sufficient for doodad/destructible/static stamp
  decals.

Additional render/GxDevice coverage added during continuation:

- `RenderGlobalPass_DispatchBySelector` at `0x6f76f060` now has a complete
  selector map for selectors `0..16`. Selector `12` is important: it enqueues
  TerrainShadow-owned records through `RenderQueue_AddBatch` before
  `RenderWorldGroup(0)`, so `stage == 11` is not by itself proof of a normal
  world-object producer.
- `RenderQueue_ApplyDrawStateAndSamplerPair` at `0x6f138ee0` materializes
  `CGeosetData` vertex streams (`+0x0c` vertex count, `+0x10` positions,
  `+0x58` normals, `+0x4c` group slots, plus UV dispatch records) before calling
  `GxDevice_UploadDynamicVertices`.
- `GxDevice_UploadDynamicVertices` at `0x6f0e35b0` is a wrapper and not a draw.
  It normalizes null stream strides, increments a wrapper counter, and calls
  `gx_device` vtable `+0x68`.
- `CGxDeviceD3d_UploadDynamicVertices` at `0x6f0eea50` ensures only the
  current-format dynamic VB and shared IB when their pointers are null, then
  locks the per-format VB ring, copies vertices, unlocks, sets FVF, and binds
  stream 0.
- `CGxDeviceD3d_LockDynamicVertexRing` at `0x6f0ee5d0` uses a 0x4000-vertex
  ring per output format. Append locks use `NOOVERWRITE|NOSYSLOCK`; wrap locks
  use `DISCARD|NOSYSLOCK`; the cursor advances only after a successful lock.
- `GxDevice_UploadBindDynamicIndices` at `0x6f0e3550` is also a wrapper, not the
  final draw. It dispatches to vtable `+0x6c`.
- `CGxDeviceD3d_UploadBindDynamicIndices` at `0x6f0eec20` locks the shared index
  ring, copies indices under local SEH, unlocks, calls `SetIndices`, then writes
  the current-format VB base to `this + 0x71c`.
- `GxDevice_DrawIndexedRange` at `0x6f0e3520` wraps index upload/bind and then
  immediately flushes/cleans up.
- `CGxDeviceD3d_FlushIndexedPrimitiveBatch` at `0x6f0ee9f0` is the final D3D9
  `DrawIndexedPrimitive` consumer. It reads `this +0x714/+0x718/+0x70c/+0x71c`
  and the global uploaded vertex count.
- `GxDevice_LockDynamicPreparedPrimitive` at `0x6f0e3580` dispatches through
  vtable `+0x7c`; its D3D implementation at `0x6f0eed00` ensures the current
  dynamic VB/shared IB and locks both rings for caller-side writes. It is not a
  draw.
- `GxDevice_SubmitDynamicPreparedPrimitive` at `0x6f0e3660` dispatches through
  vtable `+0x80`; its D3D implementation at `0x6f0eedb0` unlocks/binds the
  dynamic VB/IB and then issues `DrawIndexedPrimitive`.
- `SceneNode_RenderTransparentBatchPath` at `0x6f139620` now has dispatcher
  coverage: Type0 returns to world RenderQueue dispatch, Type1 recurses sorted
  child SceneNodes, Type2 uses image-like dynamic prepared primitives through
  `0x6f0e3580 -> 0x6f0e3660`, Type3 uploads format-3 dynamic strip/fan batches,
  and Type4 is a model runtime callback/custom-geoset wrapper that still needs
  callback-family classification before it can contribute stable world-caster
  identity.
- `AUCTransparent_AddEntry` at `0x6f137af0` is currently referenced only by
  `RenderBatch_Submit` and `SceneNode_AddTransparentList0/2/3/4`, producing
  transparent types 0, 2, 3, and 4. A later data-ref pass found the direct
  Type5 writer at `0x6f1390c0`; its only current code ref is
  `RenderGlobalPass` selector 7 at `0x6f7140ba`, which queues callback
  `0x6f743b20 -> 0x6f737960` for TerrainShadow projected-entry rendering.
- The transparent producer offsets are now documented: Type0 from
  `sceneNode +0xdc/+0xe0`, Type2 from `+0xe8/+0xec`, Type3 from
  `+0xf4/+0xf8`, and Type4 from `+0xa8/+0xac` 36-byte callback records.
- Static xrefs to the public GxDevice draw/upload wrappers are now grouped:
  normal RenderQueue world geoset, transparent Type0 world path,
  TerrainShadow/ListA/ListB, RenderGlobal selector6/8/15 overlays,
  stage16/18 helpers, and UI/image/effect helpers. No remaining static
  `GxDevice_DrawIndexedRange`, `GxDevice_UploadBindDynamicIndices`,
  `GxDevice_LockDynamicPreparedPrimitive`, or
  `GxDevice_SubmitDynamicPreparedPrimitive` code-ref is currently unclassified
  in this pass.
- `CWorldFrameWar3_RenderStage16UnitBucket` at `0x6f368a90` is an
  overlay/debug/unit auxiliary stage driven by `dword_6FB66E24`; helper
  callbacks generate immediate lines/text, not ordinary world-object identity.
- Stage 18 `CBuildFrame_RenderPlacementAndConstructVisuals -> 0x6f378a40`
  directly uses dynamic Gx upload/draw for placement/build preview geometry.
  This path must remain separate from persistent bridge/ramp world-caster
  identity.
- `CWorldFrameWar3_RenderWorldGroup(0x6f368e30)` feeds `CSprite` records into
  `CSprite_PrepareAndQueueAttachedRenderObject(0x6f184ee0)`, which calls sprite
  vslot `+20` and then queues `CSprite+0x20` through `RenderQueue_AddBatch`.
  The queue record and sprite pointer are therefore not sufficient as stable
  persistent object identity.
- `CDoodads_SubmitModelsToRenderQueue_Thunk(0x6f75ac00)` and
  `CBlightPuffs_SubmitModelsToRenderQueue_Thunk(0x6f75abf0)` both forward to
  `CWorldObjects_SubmitModelsToRenderQueue`. Selector `4` is therefore a doodad
  entry into the same AUWOModel/RenderQueue path, not a separate D3D allocator.
- `CWorld_SetShadowMode(0x6f76f550 -> 0x6f73c950)` only sets or clears terrain
  shadow slot bit `0x20`; it is not a native static-shadow producer. The
  producer gates remain RegisterImage, StaticStamp, and projector paths.
- `CWidget_RegisterFootprintAndShadowMask(0x6f65a140)` and
  `TerrainShadow_DispatchToShape/WriteMaskRegion(0x6f234420/0x6f234710)` are
  shared footprint/mask/fog/path/shared-vision paths. IDA comments were updated
  to remove the old "hook WriteMaskRegion covers all callers" implication:
  coverage is not correctness. These hooks stay diagnostic-only by default.
- Transparent Type4 (`0x6f137460 -> 0x6f13a0b0`) is now narrowed to the
  `CModelComplex/SceneNode +0xa8/+0xac` 36-byte callback record list:
  `+0x20` is the active gate, `+0x0c` is the transparent render callback,
  `+0x10` is the animation/update callback invoked by
  `CModel_AdvanceAnimWithDeltaMs(0x6f12ef70)`, `+0x18` is user payload, and
  `+0x1c` is the runtime wrapper. `CModel_GetAttachmentCount/
  GetAttachmentByIndex(0x6f133600/0x6f133540)` use
  `CModel+0x100/+0x104/+0x108`, so Type4 is not the attachment-array identity
  source. Transparent Type5 exists in `RenderQueue_FlushTransparent` as
  `entry[3](entry[4], entry[5])`; its direct writer is now identified as
  `0x6f1390c0`, used by selector 7 to queue TerrainShadow projected-entry
  callback `0x6f743b20 -> 0x6f737960`.
- `ShadowStamp_WriteByName_FromWorld(0x6f76d720)` is now classified by its
  visible runtime callers: `ShadowPath_PointController_Write(0x6f6a2090)`,
  `ShadowPath_EffectState_Update(0x6f6bcb00)`, and
  `Actor_RuntimeShadowMaskWriter(0x6f41b380)` /
  `TerrainShadow_WriteMaskRegion_FromActorRuntime(0x6f3db260)`. These are
  runtime point/effect/actor decal or mask paths and remain preserved by the
  default static-shadow suppression policy.
- `ExecBatch_Type0_Wrapper(0x6f1f2a70) ->
  ExecBatch_Type0_ToRenderer(0x6f1f2a50) -> Renderer_DrawRange(0x6f042a50) ->
  0x6f0fa140` is a separate renderer range/command bookkeeping stack. The
  audited path does not call the public GxDevice dynamic upload wrappers and
  currently has no stable world-object or ShadowMap caster identity evidence.
- `WeatherEffect_GetOrCreateRenderList78C(0x6f727fd0) ->
  WeatherEffectRenderList_RebuildDispatchByFlag(0x6f745ac0) ->
  WeatherEffectRenderList_SubmitEntry(0x6f746fd0)` is a data-driven
  weather/effect image-list callback stack. SubmitEntry resolves a resource
  name and calls the list callback with center/extents; it is not AUWOModel
  bridge/ramp geometry.
- `Terrain_LoadMapDataAndInitFogPathing(0x6f76e960)` writes
  `CTerrain+0x22EC = 0x6f770d70`; that callback routes to `0x6f33c0b0`
  or `0x6f33db30 -> 0x6f335b70`. The audited callees maintain keyed
  image/effect resource-list entries and callbacks, with no public GxDevice
  dynamic upload wrapper or stable world-caster identity in this path.
- Stage tail coverage now classifies stage16/18/21 after the second flush:
  `CWorldFrameWar3_RenderStage16UnitBucket(0x6f368a90)` and callbacks
  `0x6f36b8a0/0x6f36b920/0x6f36b4e0` emit auxiliary unit/debug primitives,
  `RenderStage16PathingDebugOverlay(0x6f369560)` emits pathing debug grid
  primitives, `CBuildFrame_RenderPlacementAndConstructVisuals(0x6f378a40)` and
  tail iterator `0x6f3acff0` are placement/construct preview domain, and
  stage21 is `RenderTerrainImageByIndex(0x6f76f190 -> 0x6f7373d0)` plus text-tag
  rendering `0x6f26c7f0 -> 0x6f877740 -> 0x6f877910`. None of these is a normal
  world-object caster identity source.
- `CWorldFrameWar3_UpdateWorldFrameAndPreparePasses(0x6f368480)` builds
  camera/frustum state, runs visibility/pre-render hooks, flushes
  terrain/projector shadow passes, and then calls
  `WorldObjectList_QueryVisibleCandidates(0x6f0caa90 -> 0x6f1854a0)` before
  later RenderScene submission. This is the native visibility-rebuild side of
  the bridge/ramp re-see stutter theory.

## Root causes

### Native static shadows

The existing UnitUI building-shadow block covered one producer family, but the
RegisterImage policy returned early for `NativeShadowMode < 1`, and the doodad
static-stamp hook defaulted to passthrough. That left terrain image/static stamp
producers alive in the default release mode. A later xref audit also found
`ShadowPath_StaticStamp_Toggle`, a direct `ShadowStamp_WriteByName` path that
does not call RegisterImage at all.

The candidate keeps `kNativeShadowDefaultMode = 0` for old A/B behavior, but
separates producer removal from that mode flag:

- install the RegisterImage hook by default;
- default-block `StaticStamp` RegisterImage calls;
- default-block likely native shadow texture keys, accepting both slash and
  backslash `ReplaceableTextures/Shadows` path spellings;
- keep selection/occlusion texture keys whitelisted, also accepting slash and
  backslash selection path spellings;
- default-block doodad type-0 static stamp enable, while preserving disable
  cleanup calls.
- install the direct `ShadowPath_StaticStamp_Toggle` hook by default and
  default-block only enable writes, while preserving disable cleanup calls.

### Path blockers

The d3d9 capture side already had multiple path-blocker gates, but
`ShadowRendererCore::buildFrameChunk` accepted resolved semantic packets and
attachments directly into `ShadowSubmissionFrame`. That path could bypass the
richer d3d9-side `War3PacketIsPathBlocker` decisions, especially when the record
had `rawcode == 0` but still carried a jHandle/widget pointer or when the
blocker only appeared as a small below-ground marker mesh.

The candidate adds a final semantic-core submission gate:

- resolve path-blocker rawcode from packet rawcode, `RenderObjectRegistry`
  jHandle lookup, and direct `CWidget` reads guarded by magic `+0x0C ==
  0x2B5DB42C` and rawcode at `+0x30`;
- treat explicit jHandle/CWidget path-blocker evidence as authoritative even if
  the incoming packet already carries a non-zero, non-blocker rawcode;
- reject explicit path blockers before submitting to ShadowMap;
- reject rigid, unitless, below-ground, flat marker geometry bounded by the
  existing path-blocker marker thresholds;
- expose `semanticCoreSkippedPathBlocker` and
  `semanticCoreSkippedPathBlockerGeometryMarker` counters.

The rawcode family remains the full editor path-blocker set
`YTab/YTac/YTpb/YTpc/YTfb/YTfc/YTlb/YTlc`, with the existing second-character
case normalization for `YTlc/Ytlc` variants.

Continuation audit found a second, narrower submission route:
`NativeD3D9BackendRuntime::buildCanonicalFrame` builds a
`ShadowSubmissionFrame` directly from canonical draws and submits it through
`ShadowRendererCore::submitFrame`, bypassing `buildFrameChunk`. The candidate
adds the same rawcode/jHandle/CWidget and below-ground marker gate there before
the packet is appended. It also exposes
`nativeD3D9BackendCanonicalSkippedPathBlockerCount` and
`nativeD3D9BackendCanonicalSkippedPathBlockerGeometryMarkerCount`.
The canonical below-ground flat-marker fallback now also rejects dynamic skinned
unit evidence before considering the shape marker, matching the semantic-core
gate.

The validation runtime's upper-layer supplemental frame path also appended
converted packets directly after dedup. This pass routes those supplemental
packets through the same semantic-core gate before append.

Legacy D3D9 capture append paths converge through `finalizeShadowDrawCommon`,
which rejects path blockers by rawcode, jHandle/widget fallback, anonymous marker
geometry, and the invisible alpha-rigid policy before publishing a
`shadowCasters` entry. Stage13 bridge/ramp retention stores only draws that have
already passed that finalizer, then replays their CPU snapshot after map/camera
freshness checks; it is not a separate path-blocker bypass.

Continuation audit found the immediate D3D9 capture EntryGate still treated
every non-S1 Terrain stage as a path-blocker lane. That was too broad after the
selector7 Type5 TerrainShadow classification: a pure projected shadow/decal
draw with no object identity cannot prove a path blocker, but it could still
pay for `War3BuildShadowSemanticContext`. The lane now requires object evidence
for non-S1 Terrain stages: current object, TLS semantic context, or an explicit
WorldObjects/Decorations/SelectionOverlay batch tag. Stage10 doodads and any
other Terrain draw with real object evidence remain covered.

The same source audit found a second stale-stage risk in legacy caster
classification: any draw that merely inherited `WorldObject` category and stage
`7/10/11/13` could previously be treated as an object caster even when it did
not run inside a CurrentDraw dispatch scope. That is too permissive for
Transparent Type4/Type5 callbacks, which are now known not to be normal world
RenderQueue producers. Stage-only object caster admission now requires an
active Common/Special/TransparentType0 CurrentDraw dispatch scope; true
RenderQueue bridge/ramp draws keep that witness, while transparent callback and
projected TerrainShadow draws do not.

### Bridge/ramp stutter

Previous work made unitless rigid geometry static and gave it a long-lived
draw-time VB cache. The remaining stutter signature still fits allocation churn:
after leaving visibility, `CWorldFrameWar3_UpdateWorldFrameAndPreparePasses`
can rebuild visible candidates before later RenderScene submission. Those
records eventually reach `RenderQueue_AddBatch`, where draw-local/native payload
identities for bridge/ramp style doodads can churn. The strong
`War3DrawTimeVBCacheKey` intentionally contains draw-local identity fields such
as `renderablePart`, `meshPayloadPtr`, `instanceIdentity`, and payload words.
That protects correctness, but it also means a re-seen static object can miss
the cache even when its real static geometry has not changed, then allocate
fresh device-local buffers.

The candidate adds a weak static alias index that never authorizes rendering by
itself. It only migrates an already-proven static backing entry to the new strong
key before allocation. Eligibility excludes GPU-skin, exact unit identity,
dynamic pose/unit evidence, direct producers, and path blockers. The final
submission and replay still use the strong key and normal fingerprint checks.
After audit, alias eligibility was aligned with the final `entry.isStaticGeometry`
test: only real Unit identity or pose evidence makes a record dynamic, so static
doodads/bridges/ramps with stable object evidence are not accidentally excluded
by the broader `WorldObjects/stage11` semantic helper.

Continuation update: if `jHandle`, `worldObjectEntry`, `unitPtr`, and
`sceneNode` are all unavailable after a visibility gap, the weak alias can now
fall back to a bit-exact `D3DTS_WORLD` matrix identity. This is still only a
static-backing migration key and remains constrained by map epoch, model key,
layout/index shape, static eligibility, and the existing fingerprint/strong-key
checks before rendering.

New counters:

- `drawTimeVBCacheStaticAliasHitCount`
- `drawTimeVBCacheStaticAliasMissCount`
- `drawTimeVBCacheStaticAliasMigrationCount`
- `drawTimeVBCacheStaticAliasStaleCount`
- `drawTimeVBCacheStaticEvictIdleCount`
- `drawTimeVBCacheStaticEvictCapacityCount`

If foreground validation later shows `drawTimeVBCacheStaticEvictCapacityCount`
rising near bridge/ramp stutters, the map is exhausting the 64 MiB static cap
instead of merely suffering native identity churn.

Continuation GxDevice audit narrows the root cause further: the native D3D
implementation does not recreate dynamic VB/IB objects on every bridge/ramp
re-see; it creates only when the current-format VB or shared IB pointer is null,
then reuses ring lock/discard/no-overwrite semantics. Therefore the repeating
long-gap stutter still points at WarVK-side shadow resource churn before/around
draw-time capture, not at Warcraft's own dynamic ring allocator. The static
alias migration remains the right candidate because it prevents a transient
native identity from forcing a fresh WarVK `createBuffer` allocation.

Final continuation update: the previous draw-time VB static alias path is not
enough for the default release build because the old experimental draw-time
cache is release-frozen and Stage13 bridge/ramp world-object draws normally do
not consume it. Stage13 now has a separate default content-persistent path:

- `DXVK_WAR3_STAGE13_CONTENT_PERSISTENT_GEOMETRY=1` by default.
- Eligible Stage13 rigid, indexed, fixed-world, non-skinned, non-additive,
  non-dynamic geometry derives an exact referenced-vertex content key from
  expanded draw indices, position layout, optional shared UV layout, material,
  transform, map epoch, and replay domain.
- On first miss it expands the referenced positions once, verifies/rekeys the
  immutable snapshot hash, creates a registry-owned non-indexed persistent
  geometry, and then submits through `finalizeShadowDrawCommon`.
- Later re-sees hit the persistent geometry registry and submit a lightweight
  instance instead of allocating fresh per-frame shadow backing.
- The path never bypasses path-blocker filtering: it only publishes after
  `finalizeShadowDrawCommon`; a later source pass also added a creation-time
  rawcode/jHandle/CWidget/marker precheck so confirmed blockers do not even
  allocate persistent geometry.
- The legacy `finalizeShadowDrawCommon` path now matches semantic/native core:
  jHandle or CWidget blocker evidence can override a wrong non-blocker rawcode.
  Geometry heuristics remain anonymous-only.
- Stage13 content-persistent keeps the same map/device epoch checks as the rest
  of the persistent geometry system.
- `DXVK_WAR3_SHADOW_PERSISTENT_MAX_AGE` now defaults to `3600` frames, still
  bounded by `DXVK_WAR3_SHADOW_PERSISTENT_MB`, so static bridge/ramp geometry is
  not discarded after only a short visibility gap.

New Stage13 proof counters:

- `stage13ContentPersistentEligibleCount`
- `stage13ContentPersistentHitCount`
- `stage13ContentPersistentMissCount`
- `stage13ContentPersistentCreateCount`
- `stage13ContentPersistentRejectCount`
- `stage13ContentPersistentIdentityMismatchCount`

## Source changes

- `src/d3d9/war3/core/war3_internal_test_config.h`
  - Enabled RegisterImage hook installation by default; without this the
    default RegisterImage producer policy is compiled out.
  - Added default RegisterImage shadow-key/static-stamp block knobs.
  - Re-enabled the direct `ShadowPath_StaticStamp_Toggle` hook for production
    and added a default enable-write block knob.
  - Enabled doodad static stamp blocking by default.
  - Added static alias migration knob.
- `src/d3d9/war3/hooks/war3_hook_address_book.h`
  - Updated the doodad/static stamp comment to reflect the default producer
    gate instead of the older mode-gated experiment.
- `src/d3d9/war3/hooks/war3_shadow_filter_policy.cpp`
  - RegisterImage default producer block now runs even when
    `NativeShadowMode == 0`; selection/occlusion sources stay whitelisted.
- `src/d3d9/war3/hooks/war3_hook_shadow.cpp`
  - Comments now document default doodad static stamp block and cleanup
    passthrough.
  - `ShadowPath_StaticStamp_Toggle` now default-blocks enable writes and exposes
    permanent enter/blocked/cleanup counters.
- `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
  - Added final path-blocker semantic-core filter for resolved records and
    supplemental attachments.
  - Routed upper-layer supplemental validation packets through that filter
    before appending them to the validation frame.
- `src/d3d9/war3/shadow/war3_shadow_renderer_core.h`
  - Added semantic-core path-blocker skip counters.
- `src/d3d9/war3/shadow/war3_shadow_native_runtime.*`
  - Added a matching native canonical submission gate so diagnostic/native
    backend frames cannot bypass the semantic-core blocker filter.
  - Matched semantic-core behavior by refusing the below-ground flat-marker
    fallback for dynamic skinned unit evidence.
- `src/d3d9/d3d9_device.h` and `src/d3d9/d3d9_device.cpp`
  - Added static alias key/index, migration, and lifecycle cleanup for
    draw-time VB cache.
  - Aligned static-alias dynamic-unit eligibility with final static-persistence
    classification.
  - Narrowed the immediate path-blocker EntryGate so non-S1 Terrain slow-path
    semantic builds require object evidence.
  - Added the default Stage13 content-persistent geometry path and increased
    the default persistent geometry idle lifetime to `3600` frames.
  - Hardened the Stage13 registry so `sourceHash/layoutHash` only select a
    candidate bucket. A retained geometry is reusable only after its private
    map/device, world, material, layout, alpha and UV proof and every expanded
    source-vertex byte match the current draw. A digest collision now leaves
    the existing entry untouched and falls back to current-frame capture.
  - Moved the final Stage13 path-blocker classification ahead of every
    persistent lookup/hit, so newly acquired rawcode/jHandle/CWidget/marker
    evidence cannot reuse an older non-blocker entry. Content-persistent mode
    also forces the referenced-content scan when the legacy static-retention
    experiment is enabled.
  - Added a private Generic/Stage13Exact registry domain, transactional
    `try_emplace` publication and owner-checked GC erase. The exact proof stays
    caller-owned through fallible geometry/registry/expiry publication, so an
    allocation or queue exception rolls back and retains the current-frame
    fallback instead of destroying the witness. Only the successful final step
    moves the proof into the registry-owned geometry and publishes accounting.
    Reverse-lane key collisions now fail closed without refreshing or
    overwriting either geometry.
  - Reset now clears the new live session's GPU/proof accounting while the old
    maps and their exact witnesses remain fence-owned by the retired session.
    Full expanded-stride CPU proofs are capped independently at
    `min(persistent GPU cap, 64 MiB)` and move into the entry instead of being
    copied.
  - Added diffuse-view, mip-enable and mip-bias identity to the material key;
    the full proof remains the final equality check. A zero snapshot hash is
    never published, and an FNV bucket value which itself lands on the zero
    sentinel is normalized to a nonzero bucket without weakening exact proof
    equality.
- `src/d3d9/d3d9_war3_scene.h`,
  `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`, and
  `src/d3d9/war3/tools/war3_perf_monitor.*`,
  `src/d3d9/war3/tools/war3_frame_capture.*`, and
  `src/d3d9/war3/tools/war3_control_plane.cpp`
  - Exposed the new counters in frame stats, bridge summary, JSON report, and
    workload series.
  - Added direct static-stamp path counters to `shadowRuntimeV2Summary`.
  - Exposed Stage13 content-persistent eligible/hit/miss/create/reject counters
    for the eventual foreground proof pass.
  - Exposed the separate Stage13 exact-identity mismatch counter so a hash
    collision or stale-field mismatch cannot be misreported as an ordinary hit.

## Validation

- `git diff --check -- ...` passed for the touched source files. The only output
  was the repository's existing LF-to-CRLF warning.
- The 2026-08-30 collision/lifetime hardening added and passed three independent gates:
  `test_stage13_content_persistent_exact_identity_model.py` (10/10),
  `test_stage13_content_persistent_identity_static.py` (13/13), and
  `test_shadow_path_blocker_five_source_parity_static.py` (5/5), for 28/28.
  Their
  `py_compile` and directed `git diff --check` also passed.
- Historical pre-hardening `.\build32_safe.cmd src/d3d9/d3d9.dll -j2`
  passed.
- Historical pre-hardening `ninja -C build32 -n` reported no work to do.
- Continuation audit found the first candidate still had
  `kNativeShadowRegisterImageHookEnabled = false`, which compiled out the new
  default RegisterImage policy. This pass enabled that hook and added permanent
  RegisterImage counters to `shadowRuntimeV2Summary`.
- A later continuation audit found `NativeD3D9BackendRuntime` could append
  canonical packets without the semantic-core blocker gate. This pass added a
  native canonical gate and surfaced its skip counters in
  `shadowRuntimeV2Summary`.
- A final submission audit found upper-layer supplemental validation packets
  could append directly after dedup. This pass now applies the semantic-core
  gate before those packets enter the validation frame.
- A final IDA xref audit found the direct `ShadowPath_StaticStamp_Toggle ->
  ShadowStamp_WriteByName -> ShadowStamp_WriteCore` bypass. This pass now
  installs that hook by default and blocks only enable writes.
- A final blocker-gate audit found semantic/native canonical resolution still
  skipped jHandle/CWidget lookup when packet rawcode was non-zero. This pass now
  lets confirmed jHandle/CWidget blocker evidence override such rawcode before
  final ShadowMap submission.
- A later canonical-path audit aligned `NativeD3D9BackendRuntime` with
  semantic core by rejecting dynamic skinned unit evidence before applying the
  below-ground flat-marker fallback.
- A later bridge/ramp audit found alias eligibility was using a broader
  semantic-unit helper than final static persistence. The code now uses the
  same dynamic-unit classification for both, keeping true Unit/pose records out
  while allowing static doodad/bridge/ramp records with stable object evidence.
- A continuation GxDevice audit corrected old research naming:
  `0x6f0e3550` is not the final draw, and `0x6f0ee9f0` is the real D3D9
  `DrawIndexedPrimitive` consumer. The correction is documented in
  `docs/research/war3_render_issues/20_renderqueue_dispatch_layer_reverse/README.md`,
  `docs/research/war3_render_issues/32_2026_08_26_render_layer_coverage/README.md`,
  and `src/d3d9/war3/native/address_book/README.md`, and the corresponding IDA
  functions have 2026-08-26 comments.
- A further transparent-path audit classified `SceneNode_RenderTransparentBatchPath`
  Type0..Type4 and corrected the image-like prepared primitive lock/submit pair:
  `0x6f0e3580/0x6f0eed00` locks dynamic VB/IB, while
  `0x6f0e3660/0x6f0eedb0` performs the final D3D9 submit for that path. This
  keeps bridge/ramp stutter attribution on world RenderQueue records rather
  than image/effect scratch primitives.
- A Type4 follow-up proved the `sceneNode+0xa8/+0xac` 36-byte records are
  CModelComplex runtime callback/custom-geoset records: `record+0x0c` is the
  transparent render callback, `+0x10` is the animation/update callback, and
  `+0x20` is the active gate. Attachment enumeration uses the separate
  `CModel+0x100/+0x104/+0x108` path, so Type4 remains excluded from stable
  ShadowMap caster identity until individual callback families are classified.
- The same pass audited `AUCTransparent_AddEntry` code refs and found no normal
  producer for transparent type5. A follow-up data-ref pass found
  `0x6f1390c0`, a direct type5 writer used only by selector 7 to render
  TerrainShadow projected entries through callback `0x6f743b20`; this moves
  Type5 out of the world-caster suspect set and into the native shadow/decal
  renderer family.
- Direct GxDevice xrefs were then grouped by producer family. The result keeps
  bridge/ramp stutter attribution on the `WorldObjects -> RenderQueue` path,
  keeps native shadow suppression at producer gates for TerrainShadow/ListA/ListB
  families, and marks UI/overlay/effect helpers as non-caster identity sources.
- The `ShadowStamp_WriteByName_FromWorld` taxonomy now identifies normal
  point/effect/actor runtime callers, so the remaining question is unusual
  cinematic/editor modes rather than the main gameplay path.
- The renderer command batch stack was also classified as separate from the
  War3 world RenderQueue/GxDevice dynamic-ring path.
- Weather/effect render-list callbacks were classified as effect/image lists,
  leaving callback target taxonomy as a reverse backlog item rather than a
  bridge/ramp root-cause suspect.
- A final Stage13 source pass added the default content-persistent geometry
  route because the older draw-time VB static alias path is not normally active
  for Stage13 in the release-frozen default. This gives bridge/ramp re-see
  events a registry-owned geometry hit path instead of repeated
  `createBuffer + upload/copy` work.
- The same pass raised `DXVK_WAR3_SHADOW_PERSISTENT_MAX_AGE` fallback from 240
  to 3600 frames, so same-map static geometry survives ordinary visibility
  gaps while remaining capped by `DXVK_WAR3_SHADOW_PERSISTENT_MB`.
- A final blocker-order audit fixed two residual gaps: legacy finalizer
  jHandle/CWidget blocker evidence is no longer masked by a wrong non-blocker
  rawcode, and Stage13 content-persistent performs blocker precheck before
  creating registry-owned geometry.

Historical pre-hardening DLL:

- Path: `build32/src/d3d9/d3d9.dll`
- Size: `34,352,557` bytes
- SHA-256: `7D53DAC1C679D00D619FDFB1FE49F8503D57F658BF27D96D9B5B2C0692506558`
- Last write: `2026/8/26 8:59:17`

This DLL predates the 2026-08-30 exact-identity hardening and is now retained
only as historical evidence. It is source-stale and must not be deployed.

2026-08-30 source-consistent hardened candidate:

- Exact target: `src/d3d9/d3d9.dll`.
- Preflight/actual graph: 77 edges (75 C++ objects, `libdxso.a` link, DLL
  link), with no generator, shader/header generation, or extra target.
- Build discipline: Below Normal parent, total parallelism `-j2`.
- Post-build: exact DLL `ninja -n` no-work.
- PE: PE32 / i386.
- Size: `34,362,820` bytes.
- SHA-256: `BEB35D6AA13739311B9A6AC5D7E43CFB1AE96B07B19E68B2C42AE08F6E0590A8`.
- Frozen ignored artifact:
  `AutoTest/artifacts/native_shadow_stage13_persistent_candidate_beb35d6a_20260830/d3d9.dll`.
- Directed post-build gates: 28/28, plus three-file `py_compile`, directed
  `git diff --check`, and zero compiler/build processes.

This closes only the source-consistent candidate identity. The inherited dirty
worktree currently removes 258 tracked `AutoTest/**` paths, so the available
28/28 directed gates are not a full AutoTest or runnable regression. The
candidate remains offline and is not a stable or player-deliverable DLL.

## Remaining gates

No deployment, Warcraft launch, YDWE launch, broad/restored AutoTest suite, or
Win32 runnable was performed in this pass. Only the three 2026-08-30
candidate-local directed gates listed above have run against the hardened
source. The current `build32` DLL is source-consistent but remains an offline
candidate until the clean regression, integrity/TDR, ABBA, and player-visible
low-angle gates close.

When resources allow foreground validation:

- Native static shadow: default config should show no Warcraft native static
  stamp decals while selection/occlusion remains intact. A/B restore knob:
  `DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW=0`.
  `registerImageEnterCount`, `registerImageBlockedCount`, and source bucket
  counters should prove the RegisterImage hook is actually active. New
  `staticStampPathEnterCount`, `staticStampPathBlockedCount`, and
  `staticStampPathCleanupCount` counters should prove the direct-write static
  stamp bypass is governed without suppressing cleanup.
- Path blockers: invisible blockers should no longer appear in CSM; new
  `semanticCoreSkippedPathBlocker*` and
  `nativeD3D9BackendCanonicalSkippedPathBlocker*` counters should rise
  alongside existing path-blocker rejection counters when blocker records are
  visible.
- Bridge/ramp stutter: on first re-see, expect
  `stage13ContentPersistentMissCount/CreateCount` only for the first proven
  static geometry, then `stage13ContentPersistentHitCount` on later re-sees
  without matching spikes in draw-time VB allocation or persistent reject
  counters. Capacity evictions imply a separate cap-sizing problem.
