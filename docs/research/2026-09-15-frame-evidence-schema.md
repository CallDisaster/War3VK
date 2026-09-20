# Frame Evidence schema evolution and adapter boundaries

## Current schema 6 and history schema 2 (morning fix)

CPU capacity cap is262144; reservation semantics remain those ofschema5. History
schema2 records `preMilliseconds`, `retainedPreFrames`, `trimmedPreFrames`, exact
`preSpanTicks` and `durationSatisfied`; the reader recomputes duration from image
QPC values and rejects a false one-second claim. Default player configuration is
256 allocated pre slots +4 post slots with a1000ms target, not a fixed256-frame export.
Full-resolution1440p allocation is3,833,856,000bytes (3.57GiB), with4GiB hard limit
and1GiB additional heap-budget headroom admission. No FPS limiting/downsampling.
`Ctrl+Shift+C` now enters the installed D3D9WindowProc; the earlierF8 branch was in
an unused hook and was NOT a physically validated input path. See the dedicated
shortcut/one-second fix checkpoint for corrected evidence and HUD verification.

## Current schema 5 (overnight revision)

Schema1–4 exports remain readable; the detailed schema2 table below is historical.
Current5 adds `reserved` and independent-cell concurrent writing. `accepted` counts
successfully published events including overwritten prehistory; `evicted` counts
retired successful cells, so retained count = accepted - evicted. `reserved` counts
unique tickets including rejected cell collisions; producerLosses includes these.
Sequence is strictly increasing in export but may have explicitly accounted gaps.
QPC is monotonic per CPU thread, not globally ordered by ticket across threads.

Render/CS producers never take the control-plane mutex. Each ticket selects a cell;
busy/older-lap collisions cannot overwrite an in-flight or newer writer. Writer
entry/exit and state form one seq_cst order. Frozen export rejects undrained writers;
serialization uses64KiB batches, not an additional all-event JSON object graph.
Maximum CPU capacity is131072 (~50MiB Event payload plus cell bookkeeping), not a
promise of any fixed seconds. Default remains8192; the player watcher requests131072.

Schema3 added kind16 HistoryCopyRecorded and17 CsmState. History copy stores ordinal,
Present, dimensions and the explicit source frame/epochs. CSM stores per-cascade
matrix bits and image/publication/history state; it does not store depth pixels.
Schema4/5 kind18 DirectionalDraw is recorded immediately before the actual directional
draw: data=drawIndex,cascade,pipeline,VB handle/offset/size,IB handle/offset/size,
alphaView,geometryHash,rawcode. words0..15=MVP bits;16=actual pc.flags;17=alphaRef;
18=sampler,19=paletteOffset,20=blendCount,21=indexed,22=count,23=first,24=vertexOffset,
25=indexType,26=jHandle,27=stage,28=VB stride,29=positionOffset,30=uvOffset,31=uvFormat,
32=paletteIndex,33=direct GPU skin,34=volumeSun path,35/36=target VkImage,37=resolution,
38/39=shadow render serial. It is not a raw VB/IB/palette/texture snapshot.

`DXVK_WAR3_FRAME_EVIDENCE_DRAWS=1` enables these high-density draw records; the
normal caster input provider remains separate. R10 links68 frames to584 surface
draws/388 alpha-tested draws each, with0 producer losses. VolumeSun entries remain
separate, not folded into the surface reconciliation count. Full capability flags
remain false because intermediate GPU images, raw resources and pixel draw-ID are
not available. The old try-lock writer is superseded, not silently called equivalent.

## Historical schema 2

This stage was CPU metadata + native-screenshot correlation scaffolding, not a complete
GPU incident recorder. Schema 1 CPU-only exports are retained/readable; schema 2
adds typed metadata and uses `words32` instead of pretending every word is float.

All 64-bit values use canonical unsigned decimal strings. Signed vertexOffset is
stored as its exact 32-bit two's-complement word. Object/Vulkan handle values are
diagnostic identities within this process/epoch, not durable resource identities
and not proof of initialized/current GPU content. No pointer is dereferenced by
the exporter. `owner` is the DxvkDevice address, not a filesystem or API handle.

## Events

| Kind | Meaning | Payload |
| --- | --- | --- |
| 1 / 2 | Present begin / end | End data0=ordinary PresentImage return, data1=unwinding, data2=exact screenshot-service Present ordinal (0 unavailable), data3=swapchain address; key.frame=counter observed at entry, NOT relabelled +1 |
| 3 / 4 | Pipeline begin / end | key is explicit pipeline frame/map/device tuple; end data0=method returned |
| 5 / 6 | Pass begin / end | Parent pipeline span; end parent identifies its own begin, data0=method returned; no GPU timing claim |
| 7 | Camera/input header | data0=valid,1=settingsRevision,2=normal caster count,3=fallback count,4=insertion point,5=GPU ring slot,6=debugMode,7=shadowTaaMode,8=volumetric enabled; words contain exact view/proj/viewProj float32 bits |
| 8 | Reserved marker | Not currently a heuristic or hotkey |
| 9 | Finalized normal caster INPUT metadata | Parent pipeline; data=index/rawcode/jHandle/part address/layer/metadata hash/geometry hash/bounds generation/alpha-metadata-frame/payload-complete/source-map/source-device. These values are separate, never OR-combined identities |
| 10 | Caster backing INPUT metadata | Parent kind9; data=index,VB handle/offset/size,IB handle/offset/size,UV handle/offset/size,blend handle/offset. All offsets/sizes remain full uint64 |
| 11 | Omitted caster metadata | data0=count, data1=0 provider disabled or 1 cap exceeded |
| 12 | Shadow method reconciliation | Common shadow Run exit after counter reset, including early returns; not only a special test/alternate call path |
| 13 | Screenshot slot prepared | data0=request serial,1=service Present ordinal,2=slot timeline target,3/4=width/height; prepared is NOT enqueued or completed |
| 14 | Screenshot saved | Same request data, data5=1; only after existing exact GPU query, save/close/rename success. Retains the request's session, never attributes old work to current session |
| 15 | Screenshot copy command recorded | CS-thread event after copy and signal recording, request metadata as above, key explicitly observes device/frame/epochs at capture submission. Still not GPU completion |

Kinds1–6 reserve data11 for label truncation; no other payload is misinterpreted as
that flag. Key.thread/QPC are producer CPU values. Global sequence is accepted
append order under the short ring lock; it is not a GPU execution serial.

Caster kind9 words: 0=indexCount,1=firstIndex,2=vertexOffset bits,3=vertexCount,
4=firstVertex,5=numVertices,6=alphaTest,7=alphaBlend,8=objectKind,9=stage (all ones
means unknown),10/11=actualIndexMin/Max,12=alphaRef bits,13=boundsRadius bits,
16..31=worldMatrix bits. Kind10 words: position stride/offset/format,indexType,
UV stride/offset/format/binding,sampler index,blend enabled,palette index,
replayBindingsResolved. Texture pixels, full palettes, raw VB/IB, fallback-caster
records and final per-cascade submitted draw membership remain absent.

Kind12 data: casters,replayDraws,prepared,drawn,culled,historyValidBefore/After,
historyAdvanced,sampleSource,taaMode,mapRenderSerial,historyInvalidationMask.
Words0..8: receiverExecuted,currentVisibilityExecuted,historyWriteExecuted,
replayRejected,lastRejectReason,cascade0..3 drawn counts. Empty/failing method
results are preserved; no pixel validity is synthesized from these numbers.

## Controls and limits

`DXVK_WAR3_FRAME_EVIDENCE=1` opts in. `DXVK_WAR3_FRAME_EVIDENCE_CASTERS=1` separately
enables up to512 normal finalized caster inputs per pipeline call. Default is off
because per-caster records can rapidly consume the temporal window; both disabled
provider and bounded truncation produce explicit kind11 records. This is not final
submitted draw proof and `actualCasterInputs` remains false in the capability map.

Pipe command `frame_evidence`: arm(capacity256..32768, default8192), status,
trigger(session,postPresents0..120), freeze(session), export(session), discard(session).
CLI `AutoTest/frame_evidence_control.py` calls this existing local pipe. No new
network service, worker thread, shell/remote command path or arbitrary export path.
Exports are CreateNew under the process executable's WarVK/Log/FrameEvidence.

Armed retains75% of event capacity, reserving25% for post-trigger evidence. Bounds
are in EVENTS, not seconds/frames. Triggered full capacity freezes incomplete,
never evicts pinned prehistory. State3 frozen permits off-thread serialization;
failure retains the frozen source/any partial output. Re-arm requires explicit
discard. New sessions reject old scope ends and old asynchronous screenshot saves.

## Screenshot canary

The strict offline reader requires one prepared/copy/saved triple with identical
request ID, ordinal, target value, dimensions and device owner. It checks one
matching ordinary Present span, one exact same-frame/epoch/thread pipeline begin
and end before copy, PID in native filename, decoded dimensions and file SHA.
No +1 offset, timestamp-nearest or last-key-wins fallback is allowed. Failure
means the association is unavailable, not that the image is relabelled.

This links a REQUESTED native screenshot only. It does not continuously capture,
retain pre-trigger images, burn IDs into external DVR, or guarantee an unfinished
save survives a recorder freeze. The root capabilities remain false for pixels,
GPU completion coverage, video mapping, full caster inputs and exact DLL/map pin.
Optional `imageLinkVerified=true` is deliberately narrower than `captureComplete`.

## Next GPU-provider contract

A separate bounded allocation/retirement owner must preserve image versions before
manual trigger and accommodate actual human reaction time. Full CSM layers every
frame are not a default promise. Budget/retention/projection-ID provenance and
measured perturbation are admission conditions, not later optimizations.
Poll the exact fence/slot generation; do not infer readiness from two frame delays.
Uncertain submission/query failure must quarantine; Reset does not free in-flight
storage. Copy source layout/usage, sample count, aspect/mip/layer range and buffer
addressing must be checked by the backend adapter.

Primary references read on2026-09-15:
[vkCmdCopyImageToBuffer](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImageToBuffer.html),
[vkGetSemaphoreCounterValue](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetSemaphoreCounterValue.html).
No new GPU copy primitive or synchronization policy was introduced in this phase;
the existing async screenshot copy/fence path only gained diagnostic events.
