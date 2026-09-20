# Warcraft III Render Layer Coverage Ledger - 2026-08-26

> Scope: IDA + source-level coverage for the three current issues: native/static shadow suppression,
> bridge/ramp re-see stutter, and path blockers leaking into WarVK ShadowMap. This is not a claim that the
> whole render layer is fully reversed. It is the current audited frontier and the next checklist for making
> that claim without hidden gaps.

## Ground Rules

- No Warcraft/YDWE/AutoTest session was launched for this pass. Evidence is from the open IDA database,
  current source, prior checked-in research, build/no-work checks, and conservative static reasoning.
- Historical `WriteMaskRegion / DispatchToShape / ListA` documents are retained as research history, but they
  are not production-default authority for this candidate. The current default chain is producer-side native
  shadow gates plus final WarVK ShadowMap submit gates.
- Any render-layer function below is considered covered only for the behavior stated here. Unnamed branches,
  uncommon stage users, and alpha/special dispatch variants remain explicit reverse backlog items.

## Main Frame And Stage Chain

| Function | Address | Current finding |
| --- | ---: | --- |
| `CWorldFrameWar3_RenderScene` | `0x6F3681C0` | Main world render frame dispatcher. It orders world, terrain-shadow/decal, water, UI/world-tail stages and performs two `FlushSortedItems` windows. |
| `CWorldFrameWar3_UpdateWorldFrameAndPreparePasses` | `0x6F368480` | Per-frame pre-render/update root. It builds camera/frustum state, runs visibility/pre-render hooks, flushes terrain/projector shadow passes, and triggers visible-candidate queries before later RenderScene submission. |
| `CWorldFrameWar3_DispatchStage` | `0x6F363020` | Switch over stages `0..21`. Stage `11` dispatches selector `12` and `RenderWorldGroup(0)`. Stage `12/13` dispatch world groups `1/2`. Stage `16` is a unit/debug bucket. Stage `18` handles placement/construct visuals. Stage `21` runs selector `13`, terrain image by index, and text tags. |
| `CWorldFrameWar3_RenderWorldGroup` | `0x6F368E30` | Reads group owners at `this + 0x16C/+0x170/+0x174`, iterates 0x18-byte records, and submits their `CSprite` references. The record address is reusable storage, not a stable object identity. |
| `CSprite_PrepareAndQueueAttachedRenderObject` | `0x6F184EE0` | If `CSprite +0x20` is non-null, calls sprite vslot `+20` for prepare, then queues that batch through `RenderQueue_AddBatch`. The `CSprite` and queue-record addresses are not sufficient by themselves as persistent object identity. |
| `CWorldObjects_SubmitModelsToRenderQueue` | `0x6F75AC10` | Iterates AUWOModel records with stride `0x188`, tests `AUWOModel + 0x84` render-class bits, submits the primary model at `+0x58`, then secondary records at `+0x164` with stride `0x5C`. Bridges/ramps enter the native RenderQueue here. |
| `CDoodads_SubmitModelsToRenderQueue_Thunk` | `0x6F75AC00` | `CDoodads` vtable `+36` forwards to `CWorldObjects_SubmitModelsToRenderQueue`; it is not a separate doodad-only draw allocator. |
| `CBlightPuffs_SubmitModelsToRenderQueue_Thunk` | `0x6F75ABF0` | Uses the same `CWorldObjects_SubmitModelsToRenderQueue` body. |
| `RenderQueue_AddBatch` | `0x6F139190` | Recurses `SceneNode` children, builds transparent lists, and reuses LOS/visibility state around `sceneNode + 0xD4`. This explains why objects can leave recognition and re-enter with fresh native transient identities. |
| `RenderBatch_Submit` | `0x6F1375C0` | Emits RenderQueue records: `+0x00 renderablePart`, `+0x04 flags`, `+0x08 layerIndex`, `+0x0C per-submit ordinal`, `+0x10 layer-state record`; also writes `sceneNode` into `renderablePart + 0x14`. |
| `WorldObjectList_QueryVisibleCandidates` | `0x6F0CAA90 -> 0x6F1854A0` | Forced query wrapper does `Scene_QueryFlushSync(0x6F139860)` and then recursive visible-candidate traversal. This is the native place where visibility loss/re-entry can rebuild membership before RenderQueue enqueue. |

Current render order from `CWorldFrameWar3_RenderScene`:

```text
optional 0
1, 13
FlushSortedItems
19, 9, 2, 3, 8, optional 17, 14, 5, 10, optional 12, 11
FlushSortedItems
4, 7, 6, 20
if activeQueue == 0: 15, 18, 21
```

Selector mapping from `RenderGlobalPass_DispatchBySelector(0x6F76F060)`:

| Selector | Current finding |
| ---: | --- |
| `0` | TerrainShadow mixed prepare path. Can prepare/render ListA, ListB type 4, `RenderLayer(1,1,0)`, and node `+0x7E4` depending on terrain flags. |
| `1` | `TerrainShadow_RenderLayer(1,0,0)`. |
| `2` | `TerrainShadow_RenderLayer(0,1,1)`. |
| `3` | `TerrainShadow_RenderLayer(0,1,2)`. |
| `4` | `CDoodads` vtable slot `+36`; confirmed thunk to `CWorldObjects_SubmitModelsToRenderQueue`. |
| `5` | Terrain sub-object vtable slot `+36` when terrain flags `0x400/0x800` are both set. The base `CWorldObjects` slot also maps to `CWorldObjects_SubmitModelsToRenderQueue`; keep subtype-specific proof in IDA before treating this as any narrower producer. |
| `6` | Five-entry render/update loop gated by terrain flag `0x2000`. |
| `7` | `sub_6F714040` TerrainShadow projected-entry path. It queues transparent Type5 callbacks through `0x6F1390C0 -> 0x6F743B20 -> 0x6F737960`; not a normal world-object producer. |
| `8` | Terrain list `+0x78C` render path gated by terrain flag `0x1000`. |
| `9` | `TerrainShadow_RenderListB(type=1)`. |
| `10` | `TerrainShadow_RenderListB(type=2)`. |
| `11` | `TerrainShadow_RenderListB(type=3)`. |
| `12` | Stage-11 pre-world `TerrainShadow_Selector12_EnqueueBatches`; it can call `RenderQueue_AddBatch` before `RenderWorldGroup(0)`. Stage 11 alone is therefore not proof of world-object ownership. |
| `13` | `CTextTagManager_RenderPass`. |
| `14` | Terrain flag `0x4000` path: setup, list prep, `TerrainShadow_RenderListB(type=4)`. |
| `15` | Terrain/overlay tail list from `sub_6F766760`. |
| `16` | `TerrainShadow_RenderListB(type=5)`. |

Tail-stage coverage:

| Stage | Address / function | Current finding |
| ---: | --- | --- |
| `16` | `CWorldFrameWar3_RenderStage16UnitBucket(0x6F368A90)` | Unit/debug auxiliary buckets driven by `dword_6FB66E24` bits `0x10`, `0x200`, `0x60`, `0x100`, plus `RenderStage16PathingDebugOverlay` on bit `0x80`. The callbacks generate immediate lines/text/overlays, not ordinary `CWorldObjects` identity. |
| `18` | `CBuildFrame_RenderPlacementAndConstructVisuals(0x6F3C4330)` and `0x6F378A40` | Build placement/construct preview. `0x6F378A40` directly issues `GxDevice_UploadDynamicVertices` and `GxDevice_DrawIndexedRange`; keep it separate from persistent world-caster identity. |
| `21` | selector `13`, `RenderTerrainImageByIndex(0x6F76F190)`, `CGameState_RenderEmbeddedTextTags(0x6F26C7F0)` | Text tags and terrain image tail. It can draw, but it is not a normal world-object producer. |

## GxDevice Dynamic Upload And Draw Chain

| Function | Address | Current finding |
| --- | ---: | --- |
| `RenderQueue_ApplyDrawStateAndSamplerPair` | `0x6F138EE0` | Materializes CGeosetData streams and calls `GxDevice_UploadDynamicVertices`. Confirmed fields include `CGeosetData +0x0C = vertexCount`, `+0x10 = positions`, `+0x58 = normals`, `+0x4C = groupSlots`, with UV streams resolved from layer dispatch records. |
| `GxDevice_UploadDynamicVertices` | `0x6F0E35B0` | Global wrapper. Normalizes null stream strides, increments a wrapper counter, then dispatches `gx_device` vtable `+0x68`. It is not a draw. |
| `CGxDeviceD3d_UploadDynamicVertices` | `0x6F0EEA50` | D3D implementation. Ensures only the current-format dynamic VB and shared IB when their pointers are null, locks the per-format dynamic VB ring, copies vertices, unlocks, sets FVF, and binds stream 0. |
| `CGxDeviceD3d_LockDynamicVertexRing` | `0x6F0EE5D0` | Per-output-format 0x4000-vertex ring. Append locks use `NOOVERWRITE|NOSYSLOCK`; wrap locks use `DISCARD|NOSYSLOCK`; cursor advances only after successful `Lock`. |
| `GxDevice_UploadBindDynamicIndices` | `0x6F0E3550` | Global wrapper for vtable `+0x6C`, not the final draw. Older docs called this the draw because many callers flush immediately after it. |
| `CGxDeviceD3d_UploadBindDynamicIndices` | `0x6F0EEC20` | Locks the shared index ring, copies indices under a local SEH handler, unlocks, calls `SetIndices`, and writes the current-format vertex-ring base to `this + 0x71C`. |
| `GxDevice_DrawIndexedRange` | `0x6F0E3520` | Wrapper that calls `GxDevice_UploadBindDynamicIndices`, then immediately calls `GxDevice_FlushPrimitiveBatch` and `GxDevice_StateCleanup74`. |
| `CGxDeviceD3d_FlushIndexedPrimitiveBatch` | `0x6F0EE9F0` | The final D3D9 `DrawIndexedPrimitive` consumer. It reads primitive selector `this +0x714`, index count `+0x718`, start index `+0x70C`, base vertex `+0x71C`, and global upload vertex count. |
| `GxDevice_LockDynamicPreparedPrimitive` | `0x6F0E3580` | Global wrapper for vtable `+0x7C`. It reserves dynamic VB/IB write ranges and returns mapped pointers for image-like prepared primitive batches. It is not the final draw. |
| `CGxDeviceD3d_LockDynamicPreparedPrimitive` | `0x6F0EED00` | D3D implementation of the `0x6F0E3580` path. Ensures current-format dynamic VB/shared IB, locks both rings, and returns caller write pointers. |
| `GxDevice_SubmitDynamicPreparedPrimitive` | `0x6F0E3660` | Global wrapper for vtable `+0x80`. It pairs with `0x6F0E3580` and performs the submit step. |
| `CGxDeviceD3d_SubmitDynamicPreparedPrimitive` | `0x6F0EEDB0` | Unlocks the dynamic VB/IB, binds FVF/stream/index state, then issues the final D3D9 `DrawIndexedPrimitive` for the prepared primitive batch. |

Dispatch fan-out rule:

- `RenderQueue_Dispatch_Common(0x6F13A5E0)` updates world matrix, binds state, begins a primitive batch, and unconditionally flushes/end-batches once.
- `RenderQueue_Dispatch_Special(0x6F13A780)` can use `DispatchSpecialBatch(0x6F13A4A0)` or `FallbackMultiPass(0x6F13A180)`.
- `DispatchSpecialAlphaBatches(0x6F13A830)` and `FallbackMultiPass` can flush multiple sub-batches.
- Therefore one vertex upload can precede zero, one, or multiple final DIPs. Any WarVK takeover or attribution must key by dispatch scope, upload epoch, and DIP ordinal, not by simple call adjacency.

## Transparent And Image-Like Dispatch Coverage

`SceneNode_RenderTransparentBatchPath(0x6F139620)` is now classified at the dispatcher level:

| Path | Address | Current finding |
| --- | ---: | --- |
| Type 0 | `0x6F13A0E0 -> 0x6F13A140` | World-batch transparent path. It checks the renderable-part visibility byte, updates the world matrix, then dispatches through the same special/fallback RenderQueue machinery. This can carry normal world-object identity, but only through the enclosing RenderQueue record. |
| Type 1 | `0x6F198C00` | Sorted child SceneNode path. It walks weighted entries and recurses into `SceneNode_RenderTransparentBatchPath`; it is not a direct geometry identity source. |
| Type 2 | `0x6F19DFF0` | Image-like dynamic prepared primitive path used by `+0x7E4/+0x78C` style terrain/overlay lists. It locks dynamic VB/IB through `0x6F0E3580`, writes quads via `0x6F19EBF0 -> 0x6F19E1D0`, then submits through `0x6F0E3660`. Do not treat this as bridge/ramp world geometry. |
| Type 3 | `0x6F19BC20` | Format-3 dynamic strip/fan style path. It uploads interleaved `20B` vertices, binds dynamic indices, and flushes per alpha/state batch. It is image/effect-like scratch geometry, not persistent world-object geometry. |
| Type 4 | `0x6F137460 -> 0x6F13A0B0` | CModelComplex/runtime callback record path. `SceneNode_AddTransparentList4` walks `sceneNode+0xA8/+0xAC` 36-byte records, using `+0x20` as the active gate and the local point at `+0x00` for sorting. Dispatch acquires the wrapper at `+0x1C` and calls render callback `+0x0C` with `(wrapper, wrapper+0x64, record+0x18)`. The same record is updated by `CModel_AdvanceAnimWithDeltaMs(0x6F12EF70)` through callback `+0x10`. Attachment handles are a separate `CModel+0x100/+0x104/+0x108` path, so Type4 is not an attachment-array identity source. |
| Type 5 | `0x6F1390C0 -> 0x6F743B20 -> 0x6F737960` | Direct transparent-array writer used by `RenderGlobalPass` selector 7. It writes `type=5`, stores callback/args in `+0x0C/+0x10/+0x14`, and the callback renders TerrainShadow projected entries through dynamic vertices and `0x6F705090`. It is a native shadow/decal path, not bridge/ramp world geometry. |

This closes the earlier ambiguity where transparent paths were grouped only as "alpha variants." The important
engineering split is: Type 0 can still be a world RenderQueue record, Type 1 is recursion, Type 2/3 are dynamic
prepared primitive scratch paths, and Type 4 is a model runtime callback/custom-geoset path that needs callback-target
classification before it can be trusted as geometry ownership. Bridge/ramp cache work should remain anchored to
`CWorldObjects_SubmitModelsToRenderQueue` and RenderQueue world-batch records, not to these auxiliary transparent paths.

Transparent producer coverage:

- `RenderBatch_Submit(0x6F1375C0)` adds Type0 transparent entries for non-opaque RenderQueue records.
- `SceneNode_AddTransparentList0(0x6F137540)` adds Type0 records from `sceneNode +0xDC/+0xE0` 104-byte entries.
- `SceneNode_AddTransparentList2(0x6F1374C0)` adds Type2 records from `sceneNode +0xE8/+0xEC` pointer lists.
- `SceneNode_AddTransparentList3(0x6F137790)` adds Type3 records from `sceneNode +0xF4/+0xF8` 356-byte entries.
- `SceneNode_AddTransparentList4(0x6F137460)` adds Type4 records from `sceneNode +0xA8/+0xAC` 36-byte
  CModelComplex callback records: `+0x20` active gate, `+0x0C` transparent render callback, `+0x10` animation/update
  callback, `+0x18` user payload, and `+0x1C` runtime wrapper. `CModel_GetAttachmentCount/GetAttachmentByIndex`
  use `CModel+0x100/+0x104/+0x108`, so Type4 is not the attachment list.
- `TerrainShadow_QueueTransparentType5(0x6F1390C0)` is a separate direct writer to the same 24-byte transparent
  array. The only current code ref is `RenderGlobalPass` selector 7 at `0x6F7140BA`, which passes callback
  `0x6F743B20` and TerrainShadow entry/pass arguments.
- Other direct `SceneNode_RenderTransparentBatchPath` callers seen so far are `0x6F0A4450`, `0x6F0A6CF0`,
  `CWorld_PreRenderRootStage0(0x6F186300)`, and `Terrain_ShadowListA_PrepareResources(0x6F7374B0)`. These are
  root/aux or terrain-list contexts and require caller context before they are allowed as persistent caster identity.
- `TerrainShadow_List78C_RenderOne(0x6F7469E0)` dispatches Type2 image-like prepared primitive entries directly.

## Direct GxDevice Draw Callsite Coverage

Static code-ref coverage for the public GxDevice draw/upload wrappers currently resolves as follows:

| Callsite cluster | Addresses / functions | Current finding |
| --- | --- | --- |
| Normal RenderQueue world path | `RenderQueue_ApplyDrawStateAndSamplerPair(0x6F138EE0)`, `CGeosetData_SubmitSingleIndexedSlice(0x6F138F70)`, `RenderQueue_Dispatch_Common(0x6F13A5E0)`, `DispatchSpecialAlphaBatches(0x6F13A830)`, `FallbackMultiPass(0x6F13A180)` | This is the authoritative world-object geoset path. A single stream upload can be consumed by common, special, or multipass flushes. |
| Transparent world path | Type0 entries from `RenderBatch_Submit`/`SceneNode_AddTransparentList0`, then `0x6F13A0E0/0x6F13A140/0x6F13A390` | Same RenderQueue contract as above, but sorted through the transparent queue. Eligible world identity still comes from the original RenderQueue record. |
| Model callback/custom geoset Type4 | `SceneNode_AddTransparentList4(0x6F137460)`, `RenderQueue_TransparentDispatchType4(0x6F13A0B0)`, `CModel_AdvanceAnimWithDeltaMs(0x6F12EF70)`, `AUCCustomGeoset` allocators `0x6F136870/0x6F1353D0` | Model-runtime callback records. They can render through callback targets, but the record itself is not object ownership proof and must stay out of persistent ShadowMap caster identity until each callback family is classified. |
| TerrainShadow ListA/ListB/profile2/type5 | `0x6F1390C0`, `0x6F705090`, `0x6F705120`, `0x6F714040`, `0x6F736C20`, `0x6F736D50`, `0x6F7370A0`, `0x6F737310`, `0x6F737500`, `0x6F737620`, `0x6F737780`, `0x6F737800`, `0x6F737860`, `0x6F737960`, `0x6F743B20` | Native terrain-shadow/decal renderers. `0x6F1390C0` queues selector-7 Type5 callbacks, and `0x6F705120` expands strip/fan style primitive types into temporary index lists before `DrawIndexedRange`. This family is governed by producer-side native shadow gates, not by broad draw suppression. |
| Terrain/overlay tail selectors | `RenderGlobalPass` selector `6 -> 0x6F87CE90`, selector `15 -> 0x6F87A9D0`, selector `8 -> 0x6F7469E0` | Auxiliary terrain grid/list/tail overlays. They use dynamic vertices, but are not stable `CWorldObjects` identity sources. |
| Stage 16/18 direct helpers | `CWorldFrameWar3_RenderStage16UnitBucket(0x6F368A90)`, bucket callbacks `0x6F36B8A0/0x6F36B920/0x6F36B4E0`, pathing overlay `0x6F369560`, placement helpers `0x6F378850/0x6F3789A0/0x6F378A40`, tail iterator `0x6F3ACFF0` | Stage16 emits auxiliary unit/debug/pathing overlay primitives. Stage18 emits build-placement/construct preview primitives and preview callbacks. They can draw, but should stay out of persistent caster identity and ShadowMap world-caster production. |
| Stage 21 tail | `RenderTerrainImageByIndex(0x6F76F190) -> CTerrain_RenderTerrainImageByIndex(0x6F7373D0)`, `CGameState_RenderEmbeddedTextTags(0x6F26C7F0) -> CTextTagManager_RenderPass(0x6F877740) -> 0x6F877910` | Terrain image tail renders a TerrainShadow ListB entry; text tags render through text helpers. Neither is a normal world-object caster source. |
| UI/image/effect helpers | `0x6F0B7C90 -> 0x6F0C0260`, `0x6F0DC380`, `0x6F0DF070`, `0x6F0DF3F0`, `0x6F133F00`, `0x6F3A3120`, `0x6F3ACCF0`, transparent Type2/Type3 | Dynamic quad, image-like, strip/ribbon, viewport, or overlay helpers. They explain direct GxDevice traffic but do not match the bridge/ramp re-see world-object stutter signature. |
| Renderer command stack | `ExecBatch_Type0_Wrapper(0x6F1F2A70) -> ExecBatch_Type0_ToRenderer(0x6F1F2A50) -> Renderer_DrawRange(0x6F042A50) -> 0x6F0FA140` | Separate renderer range/command bookkeeping stack. It does not use the public GxDevice dynamic upload wrappers in this audited path and currently has no stable world-object or ShadowMap caster identity evidence. |
| Weather/effect callback list | `Terrain_LoadMapDataAndInitFogPathing(0x6F76E960)` installs `CTerrain+0x22EC = 0x6F770D70`; callback routes to `0x6F33C0B0` or `0x6F33DB30 -> 0x6F335B70`; existing list path includes `WeatherEffect_GetOrCreateRenderList78C(0x6F727FD0) -> WeatherEffectRenderList_RebuildDispatchByFlag(0x6F745AC0) -> WeatherEffectRenderList_SubmitEntry(0x6F746FD0)` | Data-driven weather/effect image/resource list maintenance. The audited callees resolve or create resource/list entries and call list callbacks with center/extents; they do not provide AUWOModel bridge/ramp geometry or a stable ShadowMap world-caster identity. |

This xref pass covers all current static code references to `GxDevice_DrawIndexedRange(0x6F0E3520)`,
`GxDevice_UploadBindDynamicIndices(0x6F0E3550)`, `GxDevice_LockDynamicPreparedPrimitive(0x6F0E3580)`,
`GxDevice_SubmitDynamicPreparedPrimitive(0x6F0E3660)`, `GxDevice_UploadDynamicVertices(0x6F0E35B0)`, and
`GxDevice_FlushPrimitiveBatch(0x6F0E3540)`. It does not prove that no virtual or data-driven callback can reach the
device through an undiscovered vtable path; that remains part of the broader complete-renderer reverse objective.

## Native Shadow Producer Coverage

| Producer path | Address / source | Current handling |
| --- | --- | --- |
| `TerrainShadow_RegisterImageEntry` | `0x6F713250` | Hook default is enabled. Return-address/source-key policy blocks native shadow producers before mode checks, matching both slash and backslash `ReplaceableTextures/Shadows` path forms, while preserving selection circles, MarkColor/Occlusion, and selected runtime decal paths. |
| `TerrainShadow_RegisterImageEntryWithParams` | `0x6F7290B0` | Calls `RegisterImageEntry` at `0x6F7291D7`; `-1` from the hook makes wrapper return `0` and lets `0x6F713CA0` recycle the slot. Covered by the RegisterImage policy. |
| `CTerrainUberSplats/projector allocator` | `0x6F713CA0` | Allocates/reuses 0x50 projector entries, then calls `RegisterImageEntryWithParams`. Covered indirectly by the RegisterImage policy. |
| `ShadowProjector_Add_Simple` | `0x6F76D790` | Calls terrain setup and `0x6F713CA0`. Covered indirectly. It remains a diagnostic hook point, not the default broad kill switch. |
| `ShadowProjector_Add_FromObject` | `0x6F76D800` | Routes object-owned projectors into the WithParams/RegisterImage chain. FourCC filter is gated by config and only blocks confirmed path-blocker rawcodes. |
| `ShadowProjector_Add_Bridge` | `0x6F76D8D0` | Thin wrapper into `0x6F764AC0`, which resolves/creates a bridge projector definition and then calls `Add_Simple -> 0x6F713CA0 -> WithParams -> RegisterImageEntry`. Not an independent bypass. |
| `TerrainShadow_ToggleStaticStampFromObject` | `0x6F74DB30` | Calls RegisterImage at `0x6F74DBF5`; covered by RegisterImage. |
| `TerrainShadow_ToggleEmitterStamp` | `0x6F74DE40` | Calls RegisterImage at `0x6F74DF50`; covered by RegisterImage. |
| `ShadowPath_StaticStamp_Toggle` | `0x6F74E420` | Direct static-stamp writer. The hook blocks enable writes by default and passes disable cleanup calls through. |
| `ShadowStamp_WriteByName` | `0x6F713B20` | Prefixes `ReplaceableTextures\Shadows\` and calls `ShadowStamp_WriteCore`. Its caller map is now audited: direct static path and `FromWorld` wrapper only. |
| `ShadowStamp_WriteCore` | `0x6F713920` | Mutates terrain/shadow stamp bytes and marks a dirty rect. Direct caller is only `ShadowStamp_WriteByName`; do not hook this broad byte writer for default suppression. |
| `ShadowStamp_WriteByName_FromWorld` | `0x6F76D720` | Runtime decal convenience wrapper; preserved by default because actor, point, and effect controller paths can use it for non-static effects. |
| `CUnitUIManager_RecordSetUnitShadow` | `0x6F3358C0` | Writes type record `+0x4C`. Existing hook clears native unit blob/shadow names. |
| `CUnitUIManager_RecordSetStructureShadow` | `0x6F335A00` | Writes type record `+0x50`. Existing hook clears native building shadow names while leaving UberSplat `+0x48` intact. |
| `CWidget_RegisterFootprintAndShadowMask` | `0x6F65A140` | Central lifecycle sync for CREATE/DESTROY/MOVE/RESTORE, fog/path/shared-vision, and shadow-layer bits. It is diagnostic-only by default because broad blocking corrupts non-shadow mask state. |
| `TerrainShadow_DispatchToShape` / `TerrainShadow_WriteMaskRegion` | `0x6F234420` / `0x6F234710` | Shared mask shape/write implementation. It is not a visual-shadow-only producer gate; keep default rejection off unless a fresh foreground A/B proves a newly isolated caller. |

IDA caller coverage for `TerrainShadow_RegisterImageEntry` currently accounts for:

- `RegisterImageEntryWithParams`
- selection circle path
- `StaticStampFromObject`
- `EmitterStamp`
- object bridge path
- MarkColor/Occlusion path
- point projector path
- two-point projector path

`ShadowStamp_WriteByName` caller map:

- `0x6F74E4C7`: `ShadowPath_StaticStamp_Toggle` enable branch. Default-blocked by the StaticStampPath hook.
- `0x6F74E590`: `ShadowPath_StaticStamp_Toggle` disable branch. Preserved as cleanup.
- `0x6F76D72F`: `ShadowStamp_WriteByName_FromWorld` wrapper. Preserved because its callers are actor, point, and effect runtime decal updates.

`ShadowStamp_WriteByName_FromWorld` runtime taxonomy:

- `ShadowPath_PointController_Write(0x6F6A2090)` resolves a stamp resource and writes by point coordinates.
- `ShadowPath_EffectState_Update(0x6F6BCB00)` is a runtime effect state machine that calls either the point writer or the FromWorld wrapper.
- `Actor_RuntimeShadowMaskWriter(0x6F41B380)` / `TerrainShadow_WriteMaskRegion_FromActorRuntime(0x6F3DB260)` rebuild actor runtime footprint/mask state and can call `TerrainShadow_WriteMaskRegion`; it is a dynamic actor mask path, not a name-based static stamp producer.

Backlog before claiming "no omissions":

- Reconcile `ShadowStamp_WriteByName_FromWorld` against cinematic/editor-only modes only if those modes must share release defaults; normal actor/point/effect runtime callers are now classified.
- Continue searching for undiscovered vtable/data-driven render callbacks beyond the currently classified stage `16/18/21` tail paths.
- Keep `WriteMaskRegion` and `DispatchToShape` default-off unless a fresh foreground A/B proves a current native shadow footprint path that bypasses the UnitUI/RegisterImage/StaticStamp gates and can be classified without touching fog/path/shared-vision state.

## Path Blocker ShadowMap Leakage

WarVK had more than one path into the ShadowMap. The old final sweep on `scene.shadowCasters` already removed many blockers, but newer semantic/native paths could bypass that sweep. This candidate closes those paths at their final submit point instead of relying on one upstream hook.

| WarVK path | Current gate |
| --- | --- |
| D3D9 scene-caster path | Legacy append paths converge through `finalizeShadowDrawCommon`, which rejects by rawcode, jHandle/widget fallback, invisible alpha-rigid policy, and anonymous marker geometry before publication. The existing final sweep remains as a last consumer-side guard. Stage13 retention only stores draws that already passed this finalizer and replays their CPU snapshot after map/camera freshness checks. |
| Semantic core resolved packets | `SemanticCoreShouldSubmitResolvedPacket` rejects confirmed path-blocker rawcodes, jHandle/CWidget-derived blocker identity, and anonymous below-ground flat marker geometry. |
| NativeD3D9 canonical frame packets | `CanonicalShouldSubmitPacket` applies the same rawcode/jHandle/CWidget/geometry-marker rejection before appending to the canonical frame. Its below-ground flat-marker fallback now rejects dynamic skinned unit evidence the same way as semantic core. |
| Object projector path | `ShadowProjector_Add_FromObject` keeps a config-gated FourCC block for confirmed path-blocker shadow projectors. |

The immediate D3D9 capture EntryGate now also requires object evidence before
running the slow path-blocker semantic build on non-S1 Terrain stages. A bare
WorldObject stage is not enough either: it must have object/TLS/tag evidence or
an active CurrentDraw dispatch scope from Common/Special/TransparentType0. This
keeps true doodad/destructible blockers covered through current object, TLS
semantic, world-object batch tags, and active world dispatches, while avoiding
empty semantic probes for selector7 TerrainShadow projected entries, weather,
decal-only Terrain passes, and transparent callbacks with only residual stage
state.

Important failure mode closed: a packet with a wrong or unrelated non-zero rawcode can still be rejected if its
jHandle/CWidget evidence proves a path blocker. The rawcode test is no longer allowed to mask stronger identity
evidence.

The shared blocker rawcode family remains the full editor set
`YTab/YTac/YTpb/YTpc/YTfb/YTfc/YTlb/YTlc`, with the existing second-character case normalization retained for
`YTlc/Ytlc` style variants.

Runtime counters added for the eventual foreground pass:

- `semanticCoreSkippedPathBlocker`
- `semanticCoreSkippedPathBlockerGeometryMarker`
- `nativeD3D9BackendCanonicalSkippedPathBlockerCount`
- `nativeD3D9BackendCanonicalSkippedPathBlockerGeometryMarkerCount`
- static/projector path counters from the native shadow hooks

## Bridge And Ramp Re-See Stutter

Theory chain supported by IDA and source:

1. Bridges/ramps enter the normal world-object render flow through `CWorldObjects_SubmitModelsToRenderQueue`.
2. `CWorldFrameWar3_UpdateWorldFrameAndPreparePasses` refreshes camera/frustum state, runs visibility/pre-render
   hooks, and calls `WorldObjectList_QueryVisibleCandidates` before later RenderScene submission.
3. `RenderQueue_AddBatch` recurses `SceneNode` children and visibility/LOS state; when an object leaves the native
   recognition/visibility set and later returns, `renderablePart`, `meshPayloadPtr`, `stream1Ptr`, or other payload
   addresses can churn.
4. WarVK's draw-time VB cache strong key historically included those transient fields. A re-seen static object could
   miss the cache even when its real static geometry had not changed.
5. A cache miss reaches the expensive `createBuffer + upload/copy` path, producing the observed burst of stutter.
   Once the transient identity remains stable for a while, the strong key hits again and the stutter fades.

Current fix:

- A separate static alias key exists for the draw-time VB cache, but that path is not sufficient for the default release
  build because the old experimental draw-time cache is release-frozen and Stage13 bridge/ramp world-object draws
  normally do not consume it.
- Stage13 therefore has a default content-persistent geometry route gated by
  `DXVK_WAR3_STAGE13_CONTENT_PERSISTENT_GEOMETRY=1`.
- Eligible Stage13 rigid, indexed, fixed-world, non-skinned, non-additive geometry derives an exact content key from
  expanded draw indices, referenced position bytes, optional shared UV bytes, layout, material, transform, map epoch,
  and replay domain.
- On first miss, WarVK expands the referenced positions once, verifies/rekeys the immutable snapshot hash, creates a
  registry-owned non-indexed persistent geometry, and submits through `finalizeShadowDrawCommon`.
- On later re-sees, the same content key hits the persistent geometry registry and submits a lightweight instance rather
  than allocating fresh per-frame shadow backing.
- Path-blocker safety is preserved because the Stage13 content-persistent path only publishes after
  `finalizeShadowDrawCommon`; it also prechecks rawcode/jHandle/CWidget/marker evidence before creating persistent
  geometry, so confirmed blockers do not allocate registry-owned backing.
- Legacy `finalizeShadowDrawCommon` now matches semantic/native core by letting jHandle or CWidget blocker evidence
  override a wrong non-blocker rawcode. Geometry heuristics remain anonymous-only.
- Stage13 content-persistent still shares map/device epoch checks and the persistent pool cap.
- `DXVK_WAR3_SHADOW_PERSISTENT_MAX_AGE` now defaults to `3600` frames, still bounded by
  `DXVK_WAR3_SHADOW_PERSISTENT_MB`, so static bridge/ramp geometry is not discarded after a short visibility gap.

Runtime counters for foreground proof:

- `stage13ContentPersistentEligibleCount`
- `stage13ContentPersistentHitCount`
- `stage13ContentPersistentMissCount`
- `stage13ContentPersistentCreateCount`
- `stage13ContentPersistentRejectCount`

Expected proof in game: bridge/ramp first-see should show bounded miss/create, and re-see should show persistent hits
without matching `createBuffer` allocation/copy spikes. Rising persistent rejects or capacity evictions would indicate a
cap-sizing or eligibility issue rather than the originally suspected native identity churn alone.

## Verification State

Completed in this source candidate:

- 32-bit DLL build completed with `build32_safe.cmd`.
- `ninja -C build32 -n` reported no work.
- Candidate DLL at `build32/src/d3d9/d3d9.dll`:
  - size: `34,352,557` bytes
  - SHA-256: `7D53DAC1C679D00D619FDFB1FE49F8503D57F658BF27D96D9B5B2C0692506558`

Not completed by design:

- No Warcraft foreground A/B.
- No YDWE launch.
- No AutoTest launch.
- No deployment to the game directory.

## Next Reverse Chapters

Before declaring render-layer reverse complete, cover these remaining areas with the same address/function-level
standard:

- Audit virtual/data-driven render callbacks that may reach GxDevice without a direct static code reference. Transparent
  Type5's direct writer is now classified as selector-7 TerrainShadow projected-entry rendering; the remaining callback
  backlog is caller taxonomy for Type4 and data-driven effect lists.
- Finish caller-specific classification for transparent Type4 callback users before allowing that path to contribute
  stable world-caster identity. Current source gates keep stage-only legacy
  object capture dispatch-backed, so Type4 cannot enter ShadowMap merely through
  residual WorldObject stage/category state.
- Finish `ShadowStamp_WriteByName_FromWorld` mode taxonomy beyond the actor/point/effect runtime callers already
  identified.
- Native object identity lifetimes for `CSprite`, `SceneNode`, `RenderablePart`, `MeshData`, and AUWO secondary model
  records across visibility loss, map reset, and model reload.
