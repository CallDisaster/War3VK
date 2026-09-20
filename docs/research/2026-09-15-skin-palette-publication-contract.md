# Palette publication contract candidate

## Evidence and scope

The independent audit and local numerical replay proved stretched world-space
triangles in VIDEO25–29, including a 17.105469 -> 390.213730 edge. Surface and
volume-sun submissions share the bad geometry. They did not prove which native
allocator/writer last owned the questionable matrix slot. This candidate closes
identified unsafe source-selection paths; it is not a completed root-cause proof.

The switch is process scoped: `DXVK_WAR3_SKIN_PALETTE_CONTRACT=1`.
Default 0 retains the legacy behavior for comparison. The exact captured decoder's
provenance correction and per-cell CPU synchronization apply in both modes.
No shader algorithm, synchronization fence, group-index clamping, edge-length
threshold, Water feature, or native-model-light default is changed.

## Admission

1. A producer callback records its observed runtimeModel, part, current native
   frame/slot, decoded actual group count, and a CPU-owned copy of those matrices.
   A nonblocking per-cell lock protects publication/copy; contention denies the
   attempt rather than waiting. Copy completion rechecks slot/frame and CPU epoch.
2. An invalid current slot cannot reread an earlier cached slot. Strict live
   refresh only queries an owned publication for the same model/part/epoch/current
   frame/current slot and sufficient actual group count. A miss terminates the
   attempt; no global arena/cache/PoseRegistry/CModel guessed remap is tried.
3. A replacement carries its source/count/hash/space with its matrix vector.
   Captured snapshot provenance comes from the exact selected decoder entry,
   including FromRecord. A Ready status is not a source label.
4. Canonical checks selected identity/count/hash, source and World/VertexGroups.
   Owned World matrices use identity world composition, as captured World
   matrices do. Freshness is checked again before canonical.
5. A failed proof rejects the individual semantic slice. This can omit a shadow;
   disappearance of the artifact alone is NOT a pass. Native snapshot overrides
   clear the unused semantic source annotation.

CPU publication epoch/ticket are NOT native allocation generation. The latter
stays 0. Valid slot + callback model/part is a bounded producer observation, not
independent proof that a native writer never overwrote that slot beforehand.
This residual risk and visual shadow coverage require the next player recording.

## Evidence extension

Existing CPU schema6, input schema2, seven binary spans, and the 16-value provenance
array retain their meaning. Each input draw adds `skinPaletteSelection` schema1:
source, space, domain, runtimeModel, part, meshPayload, ownerEpoch,
publicationTicket, captureSerial, hash, slotAllocationGeneration, slot,
actualGroupCount, frameTag. This is the selected CPU source, not GPU shader-output
confirmation. Source 0 is unknown/not consumed; 1 captured writer; 2 raw arena;
3 owned part publication; 4/5 legacy slot paths; 6/7 Pose/CModel. Strict accepted
semantic inputs allow only 1/3; unsupported/missing values never gain authority.

CPU label `skin-selection/v1` uses existing generic kind12 (ShadowState).
It is not a kind9 CasterInput awaiting a GPU binding event. Reason0 is accepted;
nonzero reasons are rejected.
data[0..11] = source, runtimeModel, part, CPU epoch, publication ticket, capture
serial, hash, native allocation generation (0), slot, actual count, native frame,
canonical readiness reason. words32[0..5] = space, domain, freshness accepted,
JASS handle, layer, required maximum group. deviceEpoch in this CPU admission
event is unknown/0; do not join it to a GPU binding by fabricated epoch equality.
The exact GPU event/batch linkage remains the existing input reader's job.

One dedicated OwnedPartSnapshot source counter is copied through scene -> bridge
-> diagnostics/control plane -> both separate report objects. Do not count these
as DrawTimeCaptured or SubmitTimeGlobalSlot. Evidence emission requires the
existing inputs gate and an active bounded ring; no disk write is added to armed
render work. The source header/per-cell flag adds fixed CPU memory and a try-lock,
not a global lock or a full-table scan. Performance effect is not yet measured.

## Acceptance

Offline executable tests exercise the production canonical implementation and
the selection predicates, not a parallel Python simulation. Static wiring and
existing frame-input/packet/bounds readers supplement them. Exact DLL build,
PE32/i386 and no-work are build proof only.

Next player test: same map/2560x1440, move/rotate camera through the affected
Footman group, observe normal shadows as well as fissures, Ctrl+Shift+C after an
anomaly. Retain CPU JSON, inputs JSON/binary, history manifest/TGAs, launcher
identity/env and VIDEO bad-frame numbers. Run both the frozen numerical reader
and the added selection reader. A verdict requires comparing shadow population,
rejection reasons and reconstructed edge changes; no retry-count or lack-of-
visible-fissure claim substitutes for those checks.

Candidate is unvalidated gameplay, not v1.22 stable or release accepted.
