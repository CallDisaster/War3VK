# Bounded shadow input evidence — diagnostic candidate

This extends the schema-6 CPU recorder and schema-2 final-color ring. It is not
a fissure fix and is not a complete GPU replay. Old captures are not upgraded.
Gate `DXVK_WAR3_FRAME_EVIDENCE_INPUTS=1` requires the existing evidence gate and
an active session. Off does not allocate slots or issue diagnostic copies.

## Exact scope and perturbation

Capture is at the command-recording owner, after replay resolution and preparation,
before directional rendering begins. Surface CSM and volume-sun invocations are
separate batches. All Stage11 prepared draws are eligible; signatures 52/105 and
614/1587 are prioritized, but are **not** asserted to identify a unique model.
576 invocation slots, 256 KiB each (144 MiB raw capacity), at most 128 draws per
invocation. Index-count 105/1587 prioritization also covers native draws whose
bound vertex span names a complete 16384-vertex backing. Physical raw ranges are
deduplicated within an invocation only. Shader-referenced vertices are gathered
by the actual GPU IB, not inferred from D3D9 vertex-count hints. Unsupported usage,
range errors, byte/draw caps, lock contention and incomplete completion are explicit.
Do not describe a bounded partial capture as complete. No on-arm disk streaming,
CPU GPU wait, source mapping, game/input control, or timing acceptance is added.
Extra GPU copies/barriers, metadata, host memory writes and allocations still
perturb the workload. An armed run is not a performance benchmark or proof that
the observer cannot hide a race. Retained images keep their real QPC timestamps.

## Vulkan contract and primary references

- [vkCmdCopyBuffer2](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyBuffer2.html):
  copies execute outside rendering, in the same owner command list as consumption.
- [VkCopyBufferInfo2](https://docs.vulkan.org/refpages/latest/refpages/source/VkCopyBufferInfo2.html):
  validate TRANSFER_SRC, resolved backing and full ranges; independent TRANSFER_DST
  staging storage cannot overlap a source allocation. Refuse unsupported sources.
- [Synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html):
  source writes become visible to transfer reads; transfers precede subsequent
  source uses; transfer writes become visible to host reads. A submission timeline
  value is queried, never inferred from frame delay. Staging reuse additionally
  checks the command-list write-use owner. Actual coherent host memory properties
  are verified. No read or export is allowed before completion and finalization.

Exact consumed 4x4 matrix bytes already exist in the CPU-owned coherent upload pool.
Its allocation-renaming and read-use contract prevents writes while in flight. Copy
the current selected 256-matrix palette, or the static draw's one world matrix,
from that upload only. This is not mapping arbitrary device-local geometry and
does not add TRANSFER_SRC to the production matrix buffer. All buffer inputs use
real GPU reads, including direct static input/palette when present.

### Schema 2: exact indexed word gather

R16 safely rejected 105828 spans because virtual `DxvkBuffer::info().usage`
omitted TRANSFER_SRC. This was an instrumentation failure, not rendering failure.
Audit of `DxvkMemoryAllocator::createBufferResource/allocateMemory` showed internal
non-owning, non-imported pooled allocations use global VkBuffers created with
`MinGlobalBufferUsage` (TRANSFER_SRC included). Raw-copy admission uses that
specific proof only; dedicated/imported cases retain their explicit usage checks.
`usageProof`: 1 declared raw usage; 2 internal global-pool proof; 3 BDA gather.
There is no change to allocator behavior or production source usage flags.

R16 also showed native position bindings are 512 KiB each, even for tiny objects.
Dumping all of them is inappropriate for the bounded 32-bit recorder. Schema 2
uses an independent compute shader that copies uint words for each referenced
index, preserving raw index, final index, validity and original stride bytes.
It performs no floating-point math or skinning. The source remains unchanged.
The actual IB and selected uploaded matrices are exported separately.

[Khronos BDA guidance](https://docs.vulkan.org/guide/latest/buffer_device_address.html)
requires the feature, actual device-address usage and allocation support. DXVK
already provides these; the provider checks nonzero/aligned resolved addresses,
owner/pinned backing, lengths and usage instead of deriving addresses from handles.
For a 16-bit index, the rounded four-byte load is proven within the physical
allocation and only the requested half-word is used. Negative baseVertex underflow,
positive overflow and per-vertex source bounds are checked before the source word
load. Invalid addresses yield validity=0 with zero payload, not a read. Output
bounds are reserved before dispatch. SPIR-V declares Shader and
PhysicalStorageBufferAddresses only (no ShaderInt64 capability).
The pipeline is command-list tracked through GPU completion. Existing external
rendering invalidates DXVK state; normal shadow pipelines still rebind. One pre/post
memory dependency encloses the invocation rather than serializing between gathers.
Added dependencies may still mask a synchronization race; a good captured input
does not exclude such a race in the baseline. This is not zero-impact instrumentation.

## Wire format

CPU DirectionalDraw words40/41 carry the low/high 32 bits of `inputs.batches[].id`.
Join additionally by owner, render frame, map/device epoch, volume flag and draw
index. An index is local to this invocation, **never a persistent object identity**.
Each batch contains QPC, render/upload serial, scene key, objectBase, completion
values, eligible/omitted/focus counts and a binary offset. Each span has status,
buffer/offset/size, logical owner, pinned allocation, storage generation and usage.
Statuses: 0 absent; 1 copied; 2 no owner; 3 no transfer-source usage; 4 invalid range;
5 byte cap; 6 unsupported; 7 matrix range. A copied span still requires batch GPU
completion. `inputs.bin` contains no implicit padding beyond the exported used range.
Schema 2 adds `sourceBytes` (original binding extent), `encoding` (0 raw, 1 gather)
and `usageProof`. For encoding1, exported `bytes` is `count * (16 + stride)`.
Each little-endian record contains uint32 `rawIndex, finalIndex, valid, reservedZero`,
followed by exactly `stride` original bytes. Original sourceOffset/sourceBytes
still match actual bindings. Readers check index witnesses against the raw IB,
reject invalid records or inconsistent repeated vertices, and reindex for analysis
only. No source or production draw is modified by this reindexing.

Draw `words` layout is the authoritative `WORDS` list in
`AutoTest/analyze_frame_inputs.py` (40 unsigned words). Negative vertexOffset is
two's-complement int32. World matrix bits are raw 16 little-endian float words.
Spans: position/index/blend/UV/matrices/direct immutable source/direct palette.

Provenance (16 uint64 decimal strings):

0 producer site (1 semantic append, 2 current-frame producer, 3 fast append,
4 compact compatibility; 0 unknown); 1 runtimeModelPtr; 2 cache outcome bitfield;
3 found entry frame; 4 semantic alpha payload state; 5 producer frame;
6 immutable model generation; 7 effective material alpha mode; 8 canonical world
transform source; 9 skinned/authoritativeSkinned; 10 material signature;
11 metadata contract present; 12 modelKey; 13 modelResourcePtr; 14 geoset index;
15 canonical geometry source. Nonsemantic producers may leave unavailable fields
zero. Unknown is not an inferred identity or a validated path.

Cache outcome low byte: 0 gate/precondition inactive; 1 not exact logical slice;
2 exact slice but no entry; 3 found entry. Bits8 complete backing,9 exact key,
10 exact frame,11 static,12 GPU-skin lease,16 override actually applied. These are
diagnostic assignments only; existing branch conditions and fallback stay intact.
Part, layer, rawcode/handle, alpha frame, metadata/geometry hashes and lifecycle
are recorded separately. Canonical material metadata is not a copy of native MDX.

## Reconstruction / remaining gaps

The Python reader checks recursive JSON duplicates, binary ranges, session/nonce,
actual physical bindings and timeline completion. It reconstructs legacy rigid and
indexed-weight blending in the shader's row-vector convention:
`world = sum(weight_i * (position * uploadedMatrix_i))`; `clip = world * lightVP`.
It preserves zero-explicit-weight indexing and negative baseVertex semantics, rejects
nonfinite/out-of-range input, and reports unsupported direct-shader reconstruction.
This is float64 numeric analysis, **not bit-identical shader execution**.

Still missing: pixel-to-draw-ID output, alpha texture pixels, intermediate depth/
shadow-factor/volume images, mounted model path proof for anonymous draws, and
original MDX bone hierarchy. Captured group indices plus expanded matrices are
enough to reconstruct the selected legacy shader input, not to prove native pose
generation was correct. `rootCauseReady=false` remains fixed. The next player
recording should only be requested after a moving-camera canary has reconstructed
actual captured inputs and shown truthful limits. No release approval follows.

`DXVK_WAR3_FRAME_EVIDENCE_OUTPUT` optionally names an existing absolute local
directory; malformed paths fail instead of falling back. Defaults remain beside
the game. Export uses CreateNew; partial files and old evidence are never replaced.

## Verification checkpoint

R16: 18 images; capture drops0; raw geometry safely rejected by virtual usage;
0 reconstructed. Invalid for delivery. Restored test DLL74CC, player5B6D untouched;
no dump/GPU event. R17 schema2: 576 retained invocations, 13600 reconstructed draw
records, focus4951/4951 including 16 stride12/flags3 and 4935 stride32. Capture
drops0; nonfocused spans hit byte caps and remain explicitly incomplete. Restore,
player/map preservation and no new dump/GPU event passed. These counts are draw
records across invocations, not independent objects or a fissure-root-cause proof.
Reader/static23/23; SPIR-V Vulkan1.3 validation passed with exact push offsets
0/8/16/24..52. R18 first session captured112 frames with1.001175s prehistory and
focus5043/5043. Its second session saved images, but the old60s parent watchdog
terminated the offline postprocessor: **not a watcher pass**. The decoder now
vectorizes the same checks; re-reading that frozen capture takes14.768s and keeps
13516/focus5043 identical reconstruction counts. Raw-schema frozen postprocessing
has a separate180s watchdog; recording duration is unchanged.

R19 passed: one isolated2560x1440 process, zero global input; moving-camera short
session and a second player-watcher session. Watcher retained102 images with
1.0068159s prehistory, reconstructed12048 records including focus5522/5522, no
invocation drops. Nonfocus byte caps remain explicit. HUD completion was visually
checked in `input_watch_r19_20260915/hud-complete-inputs.png` (only that crop viewed).
No new GPU events/dumps; test74CC/player5B6D/map9268 restored/preserved.
Candidate35,805,448 bytes /6D6C7F3C3C834CE56692881A5C46DE6086D9AF2B7F93C2C6848E39A5D5F594B1.
This approves a bounded diagnostic player recording, not product acceptance.
The packaging smoke uses synthetic selectors40–41, not an observed fissure; it
round-tripped10 lossless PNGs and window-selected CPU/input data, retaining full
source hashes and every missing/capacity record inside those selected batches.

R17 supplies a negative control: a614/1587 part switches32→12 while reconstructed
bounding extrema differ by about5e-5 world units. This is not full per-vertex or
pixel equivalence, but reinforces that fallback presence alone is not the cause.
