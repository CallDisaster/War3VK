# WarVK Render Host Laboratory (RH0 / RH1)

Not a rendering replacement and **not linked into the shipping DLL**. There is
currently no Game.dll reader, D3D9 proxy, GPU import, swapchain,
automatic game integration or deployment in this directory. The CPU test client
explicitly creates its own isolated helper and no other application.

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

Limits: synthetic CPU bytes only; worker remains an AutoTest fixture, not a
production asynchronous service. No Vulkan device, cross-process GPU ownership,
render graph, swapchain, game integration, memory saving, FPS or crash-fix claim.
