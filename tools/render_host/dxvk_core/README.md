# Actual DXVK x64 core and restricted D3D9 host (development only)

These targets link the repository's real `dxvk_lib`, not a second Vulkan renderer.
They are not included in the game DLL and have no Game.dll readers or game hooks.
The separate development D3D9 proxy and host described below are still restricted
laboratory executables, not a migrated Warcraft renderer or a memory-saving release.

The root build must be independently configured for **Windows x86_64** with
`-Denable_d3d9=false -Dwarvk_render_host_core_dev=true`. The bootstrap target is
`warvk-dxvk-core-host`; its output is
`tools/render_host/dxvk_core/warvk-dxvk-core-host.exe` below that build directory.
Do not reconfigure the existing shipping x86 build or disable x86 game-structure
assertions to obtain an x64 binary.

## What a successful bootstrap run proves

The executable constructs the production `Rc<DxvkInstance>`, adapter, device,
context and allocator buffers. For each of three distinct nonzero patterns it
records a 4096-byte `clearBuffer` followed by `copyBuffer`, signals a production
`DxvkFence`, submits through the real DXVK submission queue, observes the GPU
timeline, waits for production resource-usage retirement, then compares all 1024
readback words. The CPU first poisons the readback to reject a no-op/stale result.
There is no host emulation of the GPU fill and no raw Vulkan submit path here.

The GPU wait and CPU polling milestones each have five-second bounds. This does
**not** make all DXVK constructors, driver calls or destructors bounded: the
independent runner must apply a whole-process watchdog. Any timeout, nonzero exit,
failed readback or inability to complete normal DXVK destruction fails the gate.
On an uncertain failure the lab retains its owner until terminal process exit;
it does not infer GPU completion from a CPU failure and free live resources.

Success JSON is emitted only after normal production owner teardown returned.
The runner must also require exit code 0 and independently check:

- PE32+/AMD64 and `processBits=64`, with source/build/executable provenance.
- `submitted=gpuTimelineCompleted=resourceTrackingRetired=3`.
- `checkedWords=3072`, `bufferBytesEach=4096`.
- `observedWordSums=[0xA55A13C7*1024, 0x19E27B04*1024, 0xDEAD0123*1024]`.
- `normalOwnerTeardown=true`, no software CPU adapter, no game/D3D9 module.

The buffer lengths are logical payload sizes, **not** measured total allocator
backing or GPU/CPU process memory. A 4096-byte allocation must not be reported as
only 4096 bytes of total memory use. Device-local memory is an allocation property,
not proof of a dedicated physical VRAM residency or game-memory reduction.

## Runner isolation

Launch only inside a fresh artifact directory with a bounded owned process.
Set `DXVK_LOG_PATH` to that directory, `DXVK_SHADER_CACHE=0`, and
`DXVK_CONFIG_FILE` to an explicitly empty lab configuration there (or a known
absent path there). Remove unrelated inherited DXVK override/filter/capture
variables rather than inheriting a user's game configuration. Keep the original
parent environment unchanged. The executable takes no arguments and cannot be
pointed at a player installation, game HWND or game process.

The bootstrap success boundary excludes shader rendering, a complete frame, presentation,
WarVK passes, x86-to-x64 D3D9 state/resource transport, Reset, device loss recovery,
actual gameplay, visual comparison, performance and memory savings. RH2 and RH3
laboratory evidence is separate and does not combine automatically into a working
remote renderer. This target establishes the real existing core as the next host
integration surface without replacing its allocator or lifetime system.

## Resource-subset command host

The same development build also provides `warvk-dxvk-buffer-host`. Unlike the
no-argument bootstrap, it accepts only a fresh protocol nonce and the exact
owned x86 test-parent PID/creation time. It has no player path/HWND argument.
`AutoTest/run_render_host_buffer_gate.py` owns startup and terminal containment.

The host dispatches WRC1 Create/Upload/Verify/Destroy/Finish through the private
resource ledger to `BufferBackend`, using actual DXVK allocation, Context copies,
submission, timeline and usage tracking. Fixed staging/readback storage is reused
only after completion. Bytes are initialized by the x86 test producer; hashes
come from GPU readback and have independent Python goldens. No CPU image relay,
per-resource full CPU mirror, or alternative raw-Vulkan renderer is introduced.

This is a useful integrated resource slice, not a D3D9 bridge or migrated WarVK
pass. Protocol/driver faults retain potentially live owners until exact isolated
process exit. It must not be loaded as a game transport service. The actual
D3D9 factory/resource/draw boundary was designed in
[the migration plan](../../../docs/plan/2026-10-11-x64-d3d9-migration-boundary.md)
and is now implemented only in the separate restricted host below.

## Restricted D3D9 C2 host and original DXVK Presenter

`warvk-d3d9-host-dev.exe` receives the real x86 development COM proxy's
`D9C2` requests and returns `D9R2` replies. `D3D9Backend` uses original shader
tokens through DXSO, plus existing DXVK allocation, Context, submission and
retirement. Old C1/R1 messages and old Draw layouts are rejected rather than
silently upgraded. There is one serialized host owner, no game memory reader,
and no second raw-Vulkan resource-lifetime system.

The shared `d3d9_resource_registry.h` binds exact ID/generation/type identities
to reusable device-connection-scoped logical slots. All six Draw inputs
(VB/IB/texture/VS/PS/declaration) must validate together before any typed backend
slot is consumed. The host retires/destroys through the existing backend before
freeing a logical identity; the proxy frees its identity only after the matching
successful reply. Rejected creation does not burn a generation, and exhausted
generations do not wrap. Capacities remain 64 logical slots, 8 buffers and 4 each
of textures, VS, PS and declarations; a buffer remains at most 256 KiB and a
packet at most 64 KiB. These are logical limits, not total process or GPU memory.

EVENT Query is a real logical resource with a generation and monotonically
increasing Issue revision. Poll must match that revision and the exact GPU serial.
A repeated GPU serial is legitimate when no new GPU work intervenes; a repeated
or stale revision is not. Query create/destroy is CPU bookkeeping, not a GPU
submission. Finish still requires every logical handle to be gone. Public COM
Release reaching zero is separately tested from private bound references and
actual remote destruction; it is not itself permission to release GPU storage.

Initialization explicitly selects offscreen, isolated Presenter, or isolated
Presenter-with-readback. Only the negotiated Presenter profiles support Present,
using the original `Presenter`, `DxvkSwapchainBlitter` and `presentImage` into the
test parent's authenticated HWND. The source handoff materializes deferred clears
through existing DXVK external-resource APIs; it does not add a source-readback
warmup. Diagnostic readback copies the actual acquired swapchain image before
queuePresent, not an image already owned by WSI. It is an image oracle, not proof
of screen scanout; the diagnostic-off profile performs no such image copy.

### Evidence as of 2026-10-22, with failures retained

- The [device-owned COM checkpoint](../../../docs/research/2026-10-22-device-owned-com-lifetime.md)
  records closure of the original single-process reference's 15 retained refs.
  Shared Presenter changes also have a fresh full x86 build and 116/116 configured
  Meson regressions, linked from the
  [Presenter checkpoint](../../../docs/research/2026-10-22-x64-d3d9-presenter-isolation.md).
  Compilation/offline success is not an x64 exit or game acceptance result.
- Presenter r7 passed 37 cases on its own frozen artifacts. Newer C2 r8 and r9
  remain failed despite correct images: r8's 256-frame clear-soak missed the
  5,000 ms primary process-exit deadline, then exited 0 after another 1,328 ms;
  r9 passed that soak but its diagnostic-on eight-frame case missed the same
  deadline, then exited 0 after another 156 ms. A host JSON normal-teardown flag,
  GPU completion or secondary wait cannot certify timely natural process exit.
- [R1/Q1](../../../docs/research/2026-10-22-x64-resource-generation-and-query.md)
  r1 remains failed after its 45-second COM-case watchdog. r2 passes both real
  COM and raw-protocol reuse/Query cases: respectively 96 and 51 Draw/readback
  pairs, 319,488 and 169,728 pixel checks with zero failures, plus rejected-input
  recovery. The added progress/exit markers are not a causal fix for r1's stall.

Use `AutoTest/run_render_host_presenter_gate.py` and
`AutoTest/run_render_host_resource_lifetime_gate.py` only under one explicit
build/GPU lease with fresh output paths. Their actual non-input desktop probes,
exact process handles and original exit deadlines must remain part of acceptance.
The immediate open gate is intermittent process exit/stall, followed by independent
Vulkan/synchronization validation against a wholly passing frozen combination.
Failed fragments are not valid inputs for upgrading the combination to passed.

All API RPCs remain synchronous lab calls. x86 still owns CPU buffer/texture
backing and local index-validation data; no real-game VA/commit reduction has been
measured. Product asynchronous ownership/backpressure, adapter/format negotiation,
full texture/Lock/FFP/stateblock, Reset/Ex, WarVK passes and gameplay are unfinished.
Game.dll processes remain rejected. No deployment, player visual acceptance,
release readiness or full-renderer migration claim follows from this slice.
