# WarVK Render Host Laboratory (CPU, cross-bitness GPU, and x64 core)

Not a rendering replacement and **not linked into the shipping DLL**. The newer
RH2/RH3 labs below perform actual Vulkan work and cross-process presentation;
RH0/RH1 remain CPU-only. There is still no Game.dll reader, product D3D9 proxy,
automatic game integration or deployment. Test clients explicitly create only
their own isolated helpers/windows, never a player game instance.

After the author's 2026-10-29 review, compatibility expansion is paused for
T1 memory ownership and T2 transport feasibility measurements. The development
host's opt-in `WARVK_D3D9_WAIT_PROFILE=1` records up to 128 EVENT intervals of
flight-reuse CPU submission, Vulkan wait-call and resource-retirement wall time.
Nested times must not be added to their containing total. It changes no wire,
resource retirement or admission rule; disabled mode performs no clock reads.
See [current evidence and remaining gates](../../docs/research/2026-10-29-x64-review-t1-t2-baseline.md).
The shared command-ring/delta prototype and real per-frame Lock/Unlock workload
coverage are still outstanding; no migration saving or performance parity claim.

The 2026-10-29 checkpoint adds actual pre-device adapter queries, partial
viewport/rectangle clears, shared production FVF/key/transform preparation and
bounded fixed-function draws (including mixed FF/SM3 stages). The existing
DXVK fixed-function shaders execute in x64. BGRA8 multi-mip/NPOT, bounded upload
transactions, partial locks and LOD pass actual local/remote GPU comparison in
both default and forced optimized pipeline modes. A sustained-draw test found
and repaired the missing optimized FF specialization values using the original
production packer. The fresh shared x86 product passes its configured 119 tests.
Sixteen texture formats/surface ownership and sampler address/filter/LOD/sRGB
now pass separately: E7EFBD5E host default/optimized gates compare 225 complete
images per arm, with 48 rejected transactions and subsequent recovery. Shared
x86 26B411E9 passes its current 119 tests; 96 resource-lifetime rounds pass.
Alpha/fog/scissor/separate alpha blend subsequently passed on host 5BCCD41A:
`x64-effects-gpu-20261029-r3/r4` compare 269 complete images per arm and 60
real IPC refusals/recoveries; the 96-round lifetime gate also passed. Clear
now intersects scissor as well as viewport and caller rectangles. Eight texture
stages and independent samplers subsequently passed on host C68506CF:
`x64-multitexture-gpu-20261029-r3/r4`, 286 complete images per arm, 72 actual
IPC refusals/recoveries, VVL365 clean, and the 96-round lifetime gate. This
includes eight SM3 samplers, public-zero private bindings, and stale-generation
rejection after texture destruction/reuse. The exact build/test failures remain
in the evidence record; no incomplete run was promoted by a later result.
StateBlock recording/capture/apply and private resource retention subsequently
passed on host 80287B63 (`x64-stateblock-gpu-20261029-r5/r6`): 297 whole images
per arm in default and optimized modes, including 11 added images and 246/247
native/remote state checks. The same gate found and repaired the old requirement
for a stage-0 texture in programmable draws; canonical null textures now use
the original DXSO null-sampler semantics, while malformed/stale identities still
fail. Wire tests exhaust optional-resource subsets on both widths. Native null
stream offsets and recaptured-short-buffer rejection are also covered. See
[`StateBlock evidence`](../../docs/research/2026-10-29-x64-stateblock-integration.md).
The same frozen candidate passes `x64-stateblock-lifetime-20261029-r1`: 96 COM
rounds, 576 resources, 256 queries, and the existing raw fault/recovery gate,
with final host resource zero and natural exit. This is not a game memory result.
It is still a bounded 64x64 development profile: RT/reset,
complete game and WarVK pass coverage remain pending. Fixed-function draws still
use ordered synchronous transactions. The isolated gate performs no process-memory measurement.
See [the exact evidence and retained failures](../../docs/research/2026-10-29-x64-fixed-function-integration.md).

The [2026-10-11 migration plan](../../docs/plan/2026-10-11-x64-render-migration.md)
starts x64 development following the author's new decision. Lab success does not
mean the game's renderer has moved or its 32-bit memory footprint has decreased.

## Actual public D3D9 / C2 development slice (2026-10-22/23)

`d3d9_proxy.cpp` now exports a real x86 `Direct3DCreate9` COM factory. A restricted
caller sends original shader tokens, owned geometry/texture uploads,
draw states and query/readback commands to `dxvk_core/d3d9_backend.cpp`, which
uses this tree's **same DXSO and DXVK libraries**. This is not the RH2 hand-built
Vulkan renderer, but it is also **not a complete or shipping D3D9 proxy**.

The profile remains 64x64 with D32F depth. Clear has bounded asynchronous local
admission. The explicit `WARVK_D3D9_BATCH_DRAWS=1` development profile also
copies up to 32 draws into a fixed batch; EndScene flushes its tail, and other
RPCs flush that prefix before their ordered synchronous barrier. Absent this
profile, Draw retains its old synchronous behavior. The new DrawBatch opcode
does not change existing header layouts; old peers reject the new operation.
`WARVK_D3D9_ASYNC_BATCH=1` additionally admits owned batches to the existing four
queue slots. `WARVK_D3D9_GPU_FLIGHTS=4` gives the host four distinct constant
backings, each reused only after the original CPU submission, GPU timeline and
DXVK resource-use witnesses. Both profiles remain explicit and default off.
`WARVK_D3D9_ASYNC_EVENTS=1` requires asynchronous batches and replaces repeated
EVENT Poll RPCs with at most one outstanding WaitEvent command. GetData observes
a connection-scoped GPU watermark locally; a live Query's successful Issue still
supplies its exact serial/revision. The worker waits for the original Vulkan
timeline, never for a future command or a fabricated CPU-complete fence. Query
completion does not grant resource retirement. Old peers reject WaitEvent.
Read-only diagnostics refuse locally staged draws instead of flushing them.
`WARVK_D3D9_GPU_GROUP=4` (requires four flights) records up to four incoming
packets on one original DXVK command list, without increasing constant backing.
EndScene explicitly sends FlushDraws; ordered resource/query/readback operations
also flush a partial group. Diagnostics reject an unsubmitted host group.
The new nonindexed triangle-list path uses a canonical empty IB reference;
other resource identities, initialized ranges and atomic preflight remain required.
`WARVK_D3D9_BATCH_STATE=1` is a default-off within-batch state experiment. Its
correctness tests passed, but timing did not show a consistent benefit. Do not
enable it as a validated performance improvement. Every batch starts with full
state restoration; no cross-frame cache is introduced.
The older batching fixture explicitly restores the full viewport before Clear.
The 2026-10-29 gate separately covers viewport/rectangle-limited Clear through
the original DXVK context; the older timing figures do not measure that work.
See the [submission grouping and coverage report](../../docs/research/2026-10-28-x64-submit-groups-and-nonindexed.md)
for matched timings, CPU-cycle scope, failed fixture receipts and remaining gates.
Offscreen Present is unsupported; two explicitly negotiated isolated Presenter
profiles now use the original `Presenter`, `DxvkSwapchainBlitter` and
`DxvkDevice::presentImage`. The diagnostic profile checks the actual acquired
swapchain image before queuePresent; it does not prove compositor/scanout pixels.
These Presenter/lifetime GPU gates use a genuinely separate non-input Win32 Desktop with
independent native thread/desktop witnesses, no global input or desktop switch.

`D9C2` / `D9R2` deliberately reject old wire versions. The shared resource
registry tracks 64 reusable logical slots with exact ID, generation and type;
all six Draw references are checked before mapping backend slots. Reuse follows
confirmed remote destruction and never substitutes for DXVK GPU retirement.
EVENT Query has its own resource identity and Issue revision, checked together
with the GPU serial. Repeated Issue without new GPU work may keep the same serial
while advancing the revision; it no longer consumes a new slot per Issue.
This does not enlarge the typed capacities or remove x86 resource backing.

### Current evidence and the remaining ownership gates

The source of truth is each dated receipt, not a combination of passing fragments:

- [X1b notification waits](../../docs/research/2026-10-23-x64-notification-waits.md)
  replaces three Connection Sleep polling sites with two connection-owned event
  hints, preserving the queue and deadlines. Dual-architecture CPU and five real
  IPC fault cases pass. Two fresh x86 builds of the same source each pass the
  complete six-case offscreen GPU development slice with VVL365: 96 COM rounds,
  2244 settled commands and 256 Query issues complete in 3.719/1.844 seconds,
  under the original 45-second watchdog. Older failures remain failures. This
  is not a full Presenter/C2 gate, product asynchronous ownership, game coverage,
  memory-saving or FPS result.
- [Host-owned window lifetime](../../docs/research/2026-10-23-x64-host-window-lifetime.md)
  now uses the same CPU-tested adapter in the formal development host, not a
  copied diagnostic backend. A fresh x64 build and two fresh x86 runs each pass
  six actual non-input Desktop GPU cases: top-level/child targets, eight-frame
  drawing, 256-Present clear soak, and quiescent parent-window destruction.
  Independent VVL365 reports zero warnings/errors; primary exits take 93–265 ms
  under the unchanged five-second limit, with empty Jobs and closed Desktops.
  Child destruction by Windows is recorded as external pre-retirement loss,
  never normal target preservation. Each case creates only one surface, so this
  does not prove actual surface recreation, in-flight target loss, full C2, or
  compositor/player pixels. Static handoff tests pass 21/21 with 73 rejected
  in-memory mutants; the GPU reader passes 22/22 pure tests.
- The old [first-draw checkpoint](../../docs/research/2026-10-11-x64-d3d9-first-draw.md)
  retains its failed 15-reference single-process teardown. The later
  [device-owned COM checkpoint](../../docs/research/2026-10-22-device-owned-com-lifetime.md)
  closes that specific reference gate with actual remote/reference runs and
  production-object lifecycle probes; it does not erase the old failure.
- [Presenter r7](../../AutoTest/artifacts/x64-presenter-20261022-r7/receipt.json)
  passed 37 cases. The later C2
  [r8](../../AutoTest/artifacts/x64-presenter-20261022-r8/receipt.json) and
  [r9](../../AutoTest/artifacts/x64-presenter-20261022-r9/receipt.json) both remain
  **failed**: r8's 256-frame clear-soak checked 1,048,576 pixels without error,
  but its primary 5,000 ms host-exit wait timed out; secondary drain saw exit 0
  another 1,328 ms later. In r9 the clear-soak and diagnostic-off WSI passed,
  but diagnostic-on WSI again missed the primary exit deadline and exited 0
  156 ms into secondary drain. Host `normalTeardown=true`, correct pixels and a
  later exit do not satisfy that original deadline. The earlier r7 pass cannot
  certify the newer C2 combination. See the
  [Presenter investigation](../../docs/research/2026-10-22-x64-d3d9-presenter-isolation.md).
- The [R1/Q1 resource and Query checkpoint](../../docs/research/2026-10-22-x64-resource-generation-and-query.md)
  retains [r1](../../AutoTest/artifacts/x64-resource-lifetime-20261022-r1/receipt.json)
  as failed after the 45-second COM-case watchdog. Its
  [r2](../../AutoTest/artifacts/x64-resource-lifetime-20261022-r2/receipt.json)
  passes COM and raw-protocol runs, including resource reuse, legal drawing after
  rejection and Query revisions. Progress/exit markers added for r2 do not prove
  the earlier intermittent stall's cause has been repaired.
- The shared production changes have a
  [fresh full x86 build](../../AutoTest/artifacts/device-owned-presenter-build-20261022/receipt.json)
  and [116/116 configured Meson regressions](../../AutoTest/artifacts/device-owned-presenter-regressions-20261022/receipt.json),
  with no-work and protected DLL identities preserved. This offline result does
  not close the later x64 process-exit failures or imply all Python/game/GPU gates passed.

The [2026-10-28 batching/cost checkpoint](../../docs/research/2026-10-28-x64-draw-batching-and-cost.md)
adds actual original-DXVK batched execution and notification-based submit/retire
waits. Fresh receipts now cover four-flight ownership, asynchronous batches and
single EVENT completion: 97/1,025-draw paired GPU gates, stale-query and whole-batch
refusal/recovery, VVL365, both-width CPU checks and five real pipe faults. The final
FE6ECB host and each independently rebuilt x86 proxy have separate identities in
that report. Timings are microbenchmarks, not game FPS; the remote path remains
slower than local and the full performance acceptance gate is open. A post-run
sample also observed another game's CPU load, not a clean exclusive machine.

Remaining ownership gates include idle target-fault notification and exact host-exit observation,
including owner-thread cancellation/drain without fabricated requests or ACKs.
Only the existing proven-quiescent predicate may authorize GPU cleanup; unknown
or in-flight ownership remains fail-stop. Then validate real surface recreation,
in-flight failures, and the complete Presenter/C2 combination. These current
subsets do not retroactively pass the older exit failures or establish game
performance/memory savings. Calls other than Clear and the explicit draw-batch /
EVENT-completion profiles still block on laboratory RPC; product asynchronous ownership and nonblocking cancellation remain
unfinished. Game.dll processes are refused, and no WarVK game pass is migrated.
Never rename this development DLL into a player installation.

Runners are `AutoTest/run_render_host_d3d9_gate.py`,
`AutoTest/run_render_host_presenter_gate.py` and
`AutoTest/run_render_host_resource_lifetime_gate.py`, with explicit toolchain,
isolated build, preserved reference DLL and a fresh output directory. Use one
build/GPU owner and freeze inputs before running. The separate
`AutoTest/run_d3d9_shared_shipping_compile.py` remains a per-TU compile check;
it is not the full product build cited above.

Read the [protocol and architecture contract](../../docs/plan/2026-09-16-render-host-ipc-v0-contract.md)
before changing the wire format or implementing a transport. This CPU foundation
separates fixed-width byte encoding from connection/map/device/frame lifetimes.
The codec borrows a receive buffer only during synchronous processing; neither a
borrowed view nor a CPU reply is a resource lease or GPU completion witness.

## Implemented

CPU recorder offload E1 is a separate in-process core proof. See the
[staged recorder contract](../../docs/plan/2026-09-16-recorder-cpu-offload-contract.md).
`recorder_ingress.h` is bounded MPSC (not the sample SPSC inbox),
`recorder_event_wire.*` encodes real Event fields, and `recorder_history_store.*`
owns history with a strict trigger/Seal contract. Reproduce with
`AutoTest/run_recorder_offload_core_gate.py --compiler32 <path> --compiler64 <path> --output <new-directory>`.
The gate includes actual queue-to-codec-to-store calls on both widths and an
independent Python golden. No IPC, product memory benefit or GPU claim follows.
E2 now connects this history to RH1 in a separate synthetic laboratory executable.
See the [E2 contract](../../docs/plan/2026-09-16-recorder-offload-e2-contract.md)
and [measured verification](../../docs/agent-history/2026-09-16-recorder-offload-e2-verification.md).
`AutoTest/run_recorder_offload_e2_gate.py --compiler32 <path> --compiler64 <path> --output <new-directory>`
builds actual x86 ingress/worker and x64 history helpers, validates independent
Python event SHA, real private/VA distribution, failures and physical settlement.
The maximum-history case includes a separately reported 55-second idle startup
observation window; it is not a cold-start timing or zero-growth claim.
No product integration, gameplay memory saving or crash-fix claim follows.

- Fixed 96-byte little-endian header and maximum 64-KiB payload.
- Exact version/capability/bitness handshake, connection nonce and ordered requests.
- Begin/sample/end epoch transitions, terminal fault/close and explicit new connection.
- No per-packet heap allocations or OS calls in the codec/state machine.
- Actual native core tests, malformed framing/state tests, deterministic mutation tests,
  and a golden vector independently encoded by Python.
- Separate Windows byte-pipe transport: logon-SID DACL, first instance, local-only,
  actual two-way peer PID verification and retained process/creation-time identity.
- One whole-packet deadline across fragments, final cancellation completion before
  buffer release; only usable in externally supervised CPU lab processes.
- Parent-owned kill-on-close Job assigned atomically at child creation, narrow
  inherited log handle list; no shell, elevation, game memory access or PID-name kill.

## Reproduce offline proof

Use fresh output paths; the gate never cleans or overwrites an old receipt.
`--compiler32` and `--compiler64` must be explicit absolute compiler paths.

```powershell
python AutoTest/run_render_host_protocol_gate.py --compiler32 <i686-g++.exe> --compiler64 <x86_64-clang++.exe> --output AutoTest/artifacts/<new-directory>
```

The gate compiles serially at BelowNormal, validates PE32/i386 and PE32+/AMD64,
runs both executables and compares their golden bytes to the Python vector.
It emits source/compiler/executable hashes and clearly marks IPC/GPU/game tests
as false. Meson users can configure **this directory separately**; root DXVK
Meson does not include it. Do not retrofit this CPU request/reply loop onto the
render thread as per-draw RPC.

## Actual cross-bitness CPU gate (P1)

```powershell
python AutoTest/run_render_host_transport_gate.py --compiler32 <i686-g++.exe> --compiler64 <x86_64-clang++.exe> --output AutoTest/artifacts/<new-directory>
```

Sixteen scenarios cover normal/fragmented echo, nonce/sequence/epoch/capability,
oversize rejection before body read, truncated input, header/body/connect/write
timeouts, wrong actual client PID, occupied pipe name, abrupt parent exit and
64 sequential owned helper lifetimes (43 valid +21 rejected, all settled).
Each timeout must report its actual stage and completed cancellation. An outer
23-second watchdog is failure containment, not a passing timeout mechanism.

Dependency initialization is separately recorded: eight system RNG calls, a
logged no-IPC child that exits normally, and a created/closed security-test pipe.
Handle-growth baselines follow this initialization; original before/after counts
are retained. A separate real 32/64-bit RNG test checks another64 calls for growth.
The lifetime soak requires no handle growth after *each* helper, not only at exit.

Limits: no cross-logon negative ACL test yet, no forced kernel-cancellation stall,
no arbitrary host-executable trust claim and no production asynchronous owner.
Cancellation drain can block in a faulty kernel; do not link this blocking lab
transport into the DLL or render thread. Close write completion is not proof the
peer application processed it. No memory-saving/FPS/crash-fix claim follows.

## Slot ownership core (P2a, not yet shared mapping)

Read the [slot ownership ADR](../../docs/plan/2026-09-16-render-host-slot-ownership-contract.md).
`slot_ledger.*` is a single serialized CPU owner, not a mapped native struct.
It admits four fixed 64-KiB slots, full generation/connection/epoch/frame/size keys,
one-shot move-only local read permits, cancel quarantine and terminal retirement.
It neither stores sample bytes nor authorizes GPU/mapping destruction.

```powershell
python AutoTest/run_render_host_slot_gate.py --compiler32 <i686-g++.exe> --compiler64 <x86_64-clang++.exe> --output AutoTest/artifacts/<new-directory>
```

This tests the actual core on both widths, 10,000 positive metadata records,
capacity/recovery/late-key/permit/cancel/exhaustion paths, and a64-byte descriptor
against an independent Python golden. RH0 wire/resource=0 remains unchanged.
Descriptor decoding alone is not lease admission; a cancelled writer does not
prove the old write stopped, so its slot is never reused in that connection.

## Actual shared bytes (P2b, CPU only)

Read the [RH1 wire and process-ownership ADR](../../docs/plan/2026-09-16-render-host-shared-slots-wire-contract.md).
RH1 uses a separate WVS1/48-byte header, 128-byte control-message cap and pipe name.
RH0 resource fields remain zero; shared bytes are never smuggled through Echo.

The Win32 transaction creates a fixed **256-KiB** paging-file mapping and four
mutexes; its exact owned Win64 child opens a read-only view. The child issues
leases from SlotLedger, copies under a finite-wait mutex into a private 64-KiB
buffer and checks SHA256 before acknowledging CPU consumption. Native pointers,
native structs, atomics and handles are not wire data. This is not zero-copy.

```powershell
python AutoTest/run_render_host_shared_slots_gate.py --compiler32 <i686-g++.exe> --compiler64 <x86_64-clang++.exe> --output AutoTest/artifacts/<new-directory>
python AutoTest/test_render_host_shared_slots_gate.py
```

The41 real scenarios cover1-byte through64-KiB samples, four outstanding slots,
three epochs, reverse-order publication, cancellation quarantine, duplicate write
and publication, every key field, corrupted/partial bytes, abandoned writer thread,
actual mutex timeout, bad wire/handshake, disconnect, abrupt parent exit, occupied
mapping/mutex names and precise container disappearance. Python independently
reconstructs every accepted sample and hash; consumer and acknowledged sample
records must match. Each native width also tests the actual RH1 codec against
an independent72-byte golden. Fragmented mode really issues2674 writes.

Normal host exit closes only its own view; the32-bit transaction waits for its
exact child before releasing its container. No mutual parent/child-exit wait.
Final child destruction waits for actual process exit, not merely a5-second
grace period; a stuck final drain is contained by the failing outer watchdog.
WAIT_ABANDONED releases the granted mutex but never copies uncertain bytes.
Cancellation quarantines rather than falsely claiming the writer stopped.
The32-bit exception runtime's first-use handle increase is separately measured;
64 further identical throws and post-transaction handle counts must be stable.

Limits: these are small synthetic CPU samples, not renderer inputs or an image
history migration. No large-history64-bit store, GPU resource sharing, real
game memory reduction or crash fix is proved. RH1 still uses the supervised,
blocking lab transport; neither it nor mutex/hash calls belong on the render
thread. Cross-logon ACL isolation and hostile same-logon writers are not proved.
The additional shared-container soak is now48 lifetimes in one Win32 producer:
12 normal multi-epoch,12 cancellation,12 late-key after real slot reuse and12
digest-fault cycles. Each checks the same post-initialization handle baseline
and absence of the exact section/mutex names before the next connection.180
samples are acknowledged, including valid sessions after injected failures;
all48 helpers are independently settled by PID/creation time. This is distinct
from P1's64 pipe lifetimes and is not a long-duration game memory benchmark.
The abrupt-parent gate also covers an outstanding Writing lease after a real
64-KiB write. It proves containment/cleanup, not successful consumer completion.

## Process-private producer inbox (P3a, not an IPC adapter yet)

Read the [producer/worker ADR](../../docs/plan/2026-09-16-render-host-producer-inbox-contract.md).
`producer_inbox.h` is a fixed single-producer/single-consumer local queue. It
copies at most64KiB, never waits for a consumer, and records Full/Invalid gaps
without overwriting accepted samples. On both tested widths it occupies262464
bytes plus the worker's65584-byte private Sample. Atomic indices are32-bit and
lock-free on the tested toolchains; no native object layout crosses processes.

```powershell
python AutoTest/run_render_host_producer_inbox_gate.py --compiler32 <i686-g++.exe> --compiler64 <x86_64-clang++.exe> --output AutoTest/artifacts/<new-directory>
```

Actual two-thread tests hold the consumer paused until the producer has completed
2000 attempts and joined:4 samples accepted,1996 explicitly dropped. Other tests
cover50,000 concurrent attempts, exact accepted bytes/FIFO/ordinal gaps, fixed
footprint, consumer-requested stop, drain-after-close, lower-limit exhaustion and
successful fresh queues after terminal cases. C++ allocation guards include
aligned new; they are not a general interceptor for every C allocator/OS API.
No performance or hard realtime guarantee follows from these stress counts.

RH1 `frame` is a consecutive transfer sample sequence,
whereas inbox sourceFrame may repeat and attempt ordinals may have loss gaps.
The explicit bounded WVP1 payload envelope retains all three identities before
the real host consumes inbox samples. Do not silently renumber source frames
or connect a blocking RH1 exchange to the render thread. Slow local-consumer
tests are not actual slow-host IPC tests, and no game memory has been migrated.

### Explicit sample envelope (P3b1)

The [WVP1 contract](../../docs/plan/2026-09-16-render-host-sample-envelope-contract.md)
now has an actual80-byte codec/order validator. It preserves sourceFrame,
possibly-gapped attempt ordinal and consecutive transfer sequence separately,
alongside the exact connection/map/device scope. Effective payload is65456bytes;
`ProducerInbox(scope, envelope::InboxLimits)` rejects overflow before copying.
Any payload validation error is terminal and clears the borrowed output view.
The validator does not issue shared-memory leases or prove GPU completion.

```powershell
python AutoTest/run_render_host_sample_envelope_gate.py --compiler32 <i686-g++.exe> --compiler64 <x86_64-clang++.exe> --output AutoTest/artifacts/<new-directory>
```

Both widths pass56,967 actual checks/10,000 positive samples plus an independent
81-byte Python golden. The updated inbox constructor/limit is separately retested.
### Actual sample worker fixture (P3b2)

The actual Win32 producer, separate I/O worker and Win64 payload consumer now
use this envelope and the SAME RH1 host runtime, not a duplicated state machine.
Capability6 versus old P2 capability2 is exact; neither direction can downgrade.
The host verifies the private-copy SHA and envelope while holding its actual
ReadPermit, before finishRead and ACK. Per-sample written/ACK/source records are
distinct; failed-in-flight bytes also have independently recomputed SHA evidence.

```powershell
python AutoTest/run_render_host_sample_worker_gate.py --compiler32 <i686-g++.exe> --compiler64 <x86_64-clang++.exe> --output AutoTest/artifacts/<new-directory>
```

`worker-r13/receipt.json` has14/14 actual cases,60 IPC lifetimes,287 ACKs and302
written samples (6,318,702bytes). The14 cases include a real1200ms consumer delay,
1000 producer attempts within that delay (4 published/996 Full), later successful
publication, disconnect before the third ACK, a fresh-connection recovery,
16 normal/disconnect cycles,16 rejects for EACH capability mismatch direction,
and five invalid envelopes whose deliberately correct SHA cannot bypass validation.
Only the fixture coordinator has test barriers; tryPush has no I/O/ACK wait.
There is at most one in-flight sample, and every missing ACK remains visible.

All resources belong to explicit owners; actual CRT thread handles and exact child
process handles settle before buffers/mappings are destroyed. An external23-second
watchdog remains mandatory for faulty-kernel drain; it firing is always failure.
Shared protocol regressions remain41/41 in `shared-r9`; Python acceptance negatives
are separate, not replacement runtime evidence.

Cold exact-image startup is separately receipted: two empty-argument host launches
must reject without IPC, repeat handle counts must be equal, then EACH real cycle
must return to that exact baseline. Earlier failures and self-handle diagnostics
are preserved in the [P3b2 audit](../../docs/research/2026-09-16-render-host-worker-lifetime-audit.md).
No arbitrary handle allowance, sleep-until-count-matches or silent retry is used.

Limits of this RH1 worker: synthetic CPU bytes only; worker remains an AutoTest fixture, not a
production asynchronous service. No Vulkan device, cross-process GPU ownership,
render graph, swapchain, game integration, memory saving, FPS or crash-fix claim.

## RH2: actual cross-bitness GPU image ownership

`gpu_wire.*`, `gpu_render_backend.*` and `gpu_host_main.cpp` form an independent
WVG2 protocol, not a widening of RH0's echo/recorder capabilities. The x64 owner
creates geometry and an indexed graphics pipeline, draws to an exported image,
and exchanges ready/consumed GPU semaphores with the exact x86 client. Adapter
and driver UUIDs, generation, map/device epoch, sequence, resource and frame are
all checked. CPU ACK does not authorize GPU retirement.

```powershell
python AutoTest/run_render_host_gpu_gate.py --compiler32 <i686-clang++.exe> --compiler64 <x86_64-clang++.exe> --vulkan-sdk <SDK-directory> --output AutoTest/artifacts/<new-directory>
python -B AutoTest/test_render_host_gpu_gate.py
```

The client performs GPU readback **only as an independent full-image test
oracle**. No CPU image crosses IPC; a game must not use this readback as its
presentation path. Normal, stale-generation, repeated-frame, bad-nonce and
wrong-adapter cases have separate exact verdicts. Native errors cannot stand
in for the intended negative result. A selected subset is not a complete gate.

`--validation` requires a working Khronos layer in both architectures;
`--host-validation` requires it only in x64 and does not claim x86 layer coverage.
Set layer paths only on the test process. Layer failures remain failed evidence.
All GPU objects stay owned until actual fence/consumer completion; uncertain
completion is quarantined to the isolated lab process. An outer watchdog is
mandatory and its intervention is always failure, not successful recovery.

## RH3: x86 window, x64 presentation

```powershell
python AutoTest/run_render_host_wsi_gate.py --compiler32 <i686-clang++.exe> --compiler64 <x86_64-clang++.exe> --vulkan-sdk <SDK-directory> --output AutoTest/artifacts/<new-directory>
```

The x86 parent owns a hidden non-activating HWND and keeps pumping messages; the
x64 child owns the Vulkan surface/swapchain and presents into that exact window.
The test requires x86 to have no `vulkan-1.dll` loaded, resizes/recreates the
swapchain and distinguishes render completion from present-completion fences.
No global input or player-window control is used. Hidden Present success is not
foreground visual/FPS acceptance, game integration, or full WSI fault coverage.

## Existing DXVK core in x64

Unlike RH2's small interoperability backend, [dxvk_core](dxvk_core/README.md)
links the repository's actual `dxvk_lib`: instance/device, allocator, context,
command submission and resource retirement. It requires a fresh independent
Windows x64 root build with `warvk_render_host_core_dev=true` and
`enable_d3d9=false`; root shipping defaults remain unchanged. No game hooks or
StormBreaker are linked into this host.

```powershell
python AutoTest/run_render_host_core_gate.py --build <new-build-directory> --output AutoTest/artifacts/<new-directory>
```

The bootstrap executable is deliberately limited to GPU fill/copy/readback with
distinct CPU submission, GPU timeline and resource-retirement witnesses. The
restricted D3D9 host above is a separate target built on that same core; bootstrap
success alone is not D3D9, WarVK-pass or full-frame evidence. The future product
must reuse this real core, not promote RH2's test renderer into a second independent
graphics engine. Blocking lab transports/destructors still must not be linked
onto the game/render thread.

## X1 / WRC1: bounded x86 commands into actual DXVK x64 buffers

`render_command_queue.h` is a process-private SPSC component, not a shared-memory
ABI: four <=64 KiB batches, no allocation/I/O/internal retries in its calls.
Full is non-consuming backpressure, never permission to lose a render command.
CPU ack, remote acceptance and GPU resource retirement are distinct milestones.

`buffer_commands.*` defines a separate bounded WRC1 resource subset with strict
scope/sequence, four resource IDs, generation tombstones, and private commit
only after the backend succeeds. `dxvk_core/buffer_backend.*` is its actual
DXVK integration: fixed host upload/readback buffers, device-local resources,
unaligned partial copies, GPU timeline and production usage retirement.

```powershell
python AutoTest/run_render_command_queue_gate.py --compiler32 <i686-clang++.exe> --compiler64 <x86_64-clang++.exe> --output AutoTest/artifacts/<new-directory>
python AutoTest/run_render_host_buffer_gate.py --compiler32 <i686-clang++.exe> --compiler64 <x86_64-clang++.exe> --build <existing-isolated-x64-core-build> --output AutoTest/artifacts/<new-directory>
```

The latter gate builds the `warvk-dxvk-buffer-host` target, proves a real x86
producer -> private queue -> dedicated I/O worker -> x64 DXVK chain, and checks
independent full-buffer hashes plus exact bad-generation/map/range rejections.
It does **not** call D3D9 COM APIs or a game. The lab-only transport/process owner
still requires an external watchdog; neither its blocking final drain nor the
fixture's explicit retry policy is a production D3D9 backpressure contract.
The 1 MiB logical resource cap excludes staging, readback, allocator backing and
process overhead. No game memory saving or full renderer migration is claimed.

## X1a: bounded Clear admission in the D3D9 development proxy

Only `Device::Clear` now returns on bounded local admission. Initialize, Draw,
resource operations, Readback, Query, Present and Finish remain ordered synchronous
barriers. The real Connection reuses the four-slot queue plus one worker-private
batch; Full neither consumes a sequence nor drops work. Remote Clear errors latch
terminal before reply progress. An enqueue/CPU ACK is never a GPU retirement proof.

```powershell
python AutoTest/run_d3d9_connection_async_cpu.py --output AutoTest/artifacts/<new-directory>
python AutoTest/test_d3d9_connection_async_cpu_reader.py
python AutoTest/run_d3d9_connection_fault_ipc.py --output AutoTest/artifacts/<another-new-directory>
python AutoTest/test_d3d9_connection_fault_ipc_reader.py
```

The CPU runner builds and executes both architectures on real restricted-Medium
non-input Win32 Desktops. It exercises the actual Connection/queue with synthetic
replies, not COM/IPC/GPU. r2 passed 263 checks per architecture; the first missing-
protocol-link failure is retained. Actual proxy-worker pressure/fault validation
is a separate gate, not inferred from these tests. Admission is bounded at 5s;
call uses a single original 30s deadline, preserving baseline done-first semantics.
The final join/transport cancellation still belong to a blocking laboratory owner;
Game.dll remains prohibited. No shipping asynchronous service or game memory saving
is established. See `docs/research/2026-10-23-x64-bounded-clear-async.md`.

The separate fault runner uses the real x86 Connection, worker, OwnedChild and
authenticated pipe with an x64 CPU adversary. Its fixed five-case r1 passed:
unexpected Clear status/payload, wrong nonce, peer exit and partial-header timeout
while Readback is waiting. The live guarded result remains untouched and later
admission fails. Secondary fault cleanup is not reclassified as primary normal
exit. This gate neither creates a GPU nor calls the public D3D9 COM proxy; actual
GPU success/retirement and product-owner cancellation remain separate gates.
