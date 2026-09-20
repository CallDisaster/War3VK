# Frame Evidence Recorder v1 — reusable incident capture architecture

Status 2026-09-15 overnight: CPU/control + final-color GPU history + actual draw
parameters have passed limited isolated canaries. Intermediate GPU/raw inputs
and automatic anomaly detection remain incomplete. See the overnight checkpoint.
NOT ready for player sample collection. Native model light
takeover remains deferred. Do not ship this diagnostic as a rendering fix.

## Modules and ownership

- `frame_evidence_core`: CPU-only bounded event ring, state machine and tokens.
  No Vulkan, D3D9, JASS, filesystem, codec or global renderer dependencies.
- `frame_evidence`: opt-in runtime adapter, QPC/thread identity, nonblocking
  producer admission, stable session identity and control-plane export. No new
  background thread; existing pipe worker serializes immutable frozen copies.
- Source adapters: Present, pipeline/pass execution, then final shadow draw and
  resources. Values are copied while their owner is valid. No raw dereference
  by the exporter, and no reinterpretation of `frameIndex` as a frame identity.
- GPU evidence provider (subsequent phase): same-command-stream bounded copies,
  exact fence completion, format/subresource descriptions, quarantine on unknown
  submission. Integrate with—not silently repurpose—the native screenshot queue.
- Offline importer: strict schema/duplicate-key validation, explicit incomplete
  reasons, time/frame/pass graph, image marker mapping and resource dictionary.
  UI/video are consumers of this schema, not authorities inventing frame IDs.

## Identity and protocol

Session is process nonce + monotonic generation. Frame key includes owner identity,
pipeline presentation serial, map epoch and device epoch. Present call ordinal is
a SEPARATE counter; store the observed pipeline serial next to it, never add/subtract
to guess a match. Pass spans carry begin/end IDs and parent span, CPU thread/QPC.
Scope end retains its session token; an old in-flight command cannot close a new
session. CPU recorded command boundaries are NOT GPU start/completion timestamps.

Schema 1 CPU-only archives remain readable; schema 2 adds typed metadata. Both export
uint64 as decimal strings (no JavaScript precision loss), matrix
values as exact float32 bits, bounded labels and explicit payload semantics.
Missing source/map/module hashes and pixels are capabilities marked unavailable,
not zeros that mean success. Future schema changes require a new version/adapter.

## Ring / trigger / export

States: Idle -> Armed -> Triggered -> Frozen; fault can freeze early. Re-arm is
rejected until explicit discard of frozen evidence. Arm allocation happens on the
control plane only. Armed retains the most recent bounded EVENTS (not a promised
number of seconds/frames); eviction count and first retained sequence are reported.
Triggered preserves its prehistory, appends a bounded number of Present terminal
events, and freezes early on full capacity. Missing/late Present can be manually
frozen; no unbounded memory growth. Trigger is a CPU observation point, not a GPU
anomaly frame guess. Queued CS work and unmatched spans are diagnosed offline.

Render/CS producers now use independent cells, not a shared control-plane try_lock;
a busy/newer-lap collision increments an explicit loss counter. No filesystem,
formatting, hashing, GPU waits or allocations in event append. Export requires Frozen and CreateNew; failed export leaves the
frozen source intact. No automatic overwrite, reset, retention deletion or upload.
Default gate `DXVK_WAR3_FRAME_EVIDENCE=0`; no storage/thread allocated when disabled.

Current implementation/field map and deliberately missing capabilities:
`../research/2026-09-15-frame-evidence-schema.md`. Final-color GPU history and game
hotkey are implemented; heuristic detection, intermediate depth/vis history, full
raw input dictionary and video ID burn-in remain pending. Existing canaries are
not a replacement for those requirements.

## Implementation milestones / mandatory acceptance

1. CPU spine: core state/fault tests, multithread/token protection, Present and
   pipeline spans, strict offline importer, real-loop CPU tests, clear incompleteness.
2. Render provenance: final actual caster/draw state; per-cascade matrices and
   alpha/UV/sampler/geometry/palette input versions; immutable dictionary + bounded
   dynamic raw blobs. Test ID reuse/relocation/map/device reset and resource retirement.
3. Pixels: pre/post shadow, current/history vis, scene depth, CSM/volumeSun layers,
   fog boundaries and final image; exact completion and draw-ID attribution. No global
   GPU drain/LockRect, no guessed two-frame completion, no reuse of unresolved storage.
4. Frame marker + normal-color review UI/video: pixels and log share ID, missing or
   repeated images explicit. 60fps DVR cannot prove coverage of 100+FPS engine draws.
   Budget overflow makes evidence incomplete rather than silently skipping frames.
5. End-to-end canary: known marked frame must resolve to the correct source/pass,
   before/after pixels and dynamic inputs. Exercise full queue, cancellation, failed
   write, device lost, early Present returns, multiple pass calls/frame and full teardown.
6. Isolated 2560x1440 correctness + stats-off overhead baseline, then player delivery.
   Do not ask the user to hunt another rare flicker until 2–5 are proven together.

All original player PNGs stay reusable. The full recorder is incomplete until every
required provider is present AND that particular run has zero relevant evidence loss.
No count-only pass can promote it into a root-cause verdict.

## Sources and constraints

[Vulkan synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
requires explicit execution/memory visibility dependencies; submitted does not mean
CPU-readable. [Desktop frame metadata](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_outdupl_frame_info)
allows accumulated desktop updates; that API is not a promise to retain every render.
These references were read for the preceding pixel investigation; GPU provider work
must additionally audit actual format/copy constraints before implementation.
