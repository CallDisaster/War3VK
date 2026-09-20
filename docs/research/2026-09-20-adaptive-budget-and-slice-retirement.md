# Adaptive memory and snapshot slice retirement — implementation contract

Status: maintenance work, not player/GPU acceptance. Keeps the existing caster,
geometry, index-domain and fail-closed contracts. No forced cache eviction,
copy/compaction, CPU readback, synchronous wait, or raised per-frame work limit.

## Authorities

- https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMemoryBudgetPropertiesEXT.html
  Budget includes allocated memory; usage/budget are estimates and change. Never
  treat total VRAM or another heap's headroom as permission. Use actual allocation
  heap, preserve reserve, check physical usage including allocator backing.
- https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyBuffer.html
  Submitted users must finish. WarVK uses existing DXVK command-list tracking,
  not guessed frame delay, producer completion, or CPU cache ownership alone.

## Scope and ownership

Snapshot slices gain a ref-counted lease from a bounded page-owned table. Cache,
queued CS copy, published caster and each consuming command list keep that lease.
Consumers include surface/volume/point shadows, both outline paths, GPU input
capture (even when later shadow draw is rejected), and the command buffer exposed
to synchronous Shader API callbacks. External asynchronous queues are not granted
a new lifetime contract. CPU-only retained records strip these leases with buffers.
Only after the final reference drops may the allocation owner reuse its interval.
Final release publishes an atomic free state; only the device owner changes the
interval table. A lease holds its page while live, with the cycle explicitly broken
on final release. No per-draw heap allocation or parallel GPU retirement system.
Existing DXVK buffer/allocation tracking and relocation contracts remain in force.
Retirement revision supports only a negative hole-search cache; it never grants
reuse. An unchanged failure can be skipped; allocation/release invalidates it.

The fixed table can saturate: seal the page against all further hole reuse and
fall back to the old append-only/whole-page lifetime. This avoids introducing a
metadata-only caster rejection. Fragmented holes are reused without moving live bytes. No promise to
solve true live-set overflow or to compact unfit noncontiguous holes.

Both pools use one pure budget policy, actual heap data and Win32 VA headroom.
Targets are quotas, not preallocations. Physical headroom is rechecked for growth;
existing live resources are not revoked on pressure. Unsupported budget data
retains conservative legacy limits; invalid VA information cannot authorize growth.
Within one owner frame, a failed growth query suppresses only equal/larger
requests; smaller requests and the next frame can re-query. Existing tails and
retired holes bypass this negative-only throttle. Map/device reset clears it.
This prevents pressure from turning every rejected caster into a driver-budget
query. Typed allocation OOM still follows fail-closed; device loss is rethrown.
Snapshot's existing bounded 512 MiB engineering ceiling remains; Arena's 1152 MiB
total/384 MiB generation work ceiling remains. Adaptive is not unlimited. Future
larger ceilings require separate GPU workload/VA validation.

Arena drops only completed generations' idle tail pages at an owner safe point,
with a demand-history window and spare page. No in-flight or current generation
can be reclaimed. Freed Rc backing may remain pooled by DXVK; physical usage and
logical residency are separate. Tests must cover this distinction.
In particular, the old reuse predicate's serial-zero admission is NOT trimming
permission: trimming also requires a positive retire serial <= completed serial.

## Acceptance

Production components: delayed GPU-like reference release, queued-copy retention,
three streams/UV alias, map reset, bounded metadata, same-class 256-byte anchors,
holes and table exhaustion; budget pressure/recovery/unknowns/overflow/hysteresis.
Wiring: every cached snapshot -> caster path and every draw/copy tracking path.
GPU gates still required: integrity, geometry/shadows, moving camera recovery,
memory trend, GPU incidents, full-frame p95/p99. CPU tests cannot certify these.

## First CPU checkpoint

Production slice table + DXVK Rc and adaptive policy: 893 checks passed (Win32,
O2); wiring guards 5/5. Same-class 24-page negative case now finds reusable
512 KiB holes after simulated consumer completion while all 24 anchors remain.
Queued-copy/caster/GPU-like Rc prevents reuse until last release. This models
completion and does not run Vulkan. First test compile failed because standalone
Rc include lacked util_likely.h; fixed the include, not the production Rc.

Budget quotas: snapshot 1/8 and Arena 3/8 of the exact heap's current process
budget, each within existing engineering ceilings. Actual new backing must also
fit free physical commitment after max(512 MiB, budget/8) reserve and the process
must report >=256 MiB available VA. VA check is a conservative refusal guard,
not a 1:1 mapping model or a guarantee of contiguous allocation. DXVK retained
backing is counted in memoryCommitted; logical memoryAllocated is not used.
Arena target shrink drops at most one retired tail page per generation per
120-frame demand window (or pressure decision); existing live bytes are retained.

## Final offline checkpoint (2026-09-20 20:42 Taipei)

- Source: HEAD `24140e4df4758e143aef2fb022af442f1b59dbb4` plus uncommitted
  maintenance changes. `E:/WarVK-Builds/v1.22-maintenance-20260920-r1/source-adaptive-r9.json`
  pins 2892 source files; SHA-256
  `B767B06C64A39A6B2C16C3306AFBDCAA4E3D5FDA23340E3E949C08BD7197E2B2`.
  Main source and mirror were checked against it with zero drift before final
  documentation-only receipts. Dirty user StormBreaker was not built or changed.
- Explicit CPU targets built BelowNormal/-j2, then `meson test --no-rebuild`:
  99/99. Full static scripts 275/275. Adaptive/slice component: 23,965 checks.
  Snapshot table size 81,960 bytes/page (about 2.50 MiB at 32 pages).
  Claim/release/hole 10,000 iterations: 102 us; dense 2048-slot repeated failed
  search 1000 iterations: 50 us. Zero heap allocation in these measured windows;
  not GPU timings or a claim about all tracker allocations.
- Final exact product incremental build 40/40, target no-work, player-release
  configuration audit, PE32/i386, py_compile and diff whitespace passed.
  Logs under that build root: `adaptive-test-build-r9.log`, `adaptive-meson-r9.log`,
  `adaptive-product-r9.log`, `static-adaptive-r9/`.
- Frozen candidate `adaptive-candidate-028565BC/d3d9.dll`:
  36,065,732 bytes / SHA-256
  `028565BCB33B6EB646E4915115B60CDA3C14C287A4225DAA50F8F6513F9E9BA3`.
  This is unstripped and not a stable release. No deployment or game run.
- Explicit policy delta: old default snapshot resident cap was 384 MiB. Trusted
  adaptive policy now uses up to the existing 512 MiB configurable ceiling;
  explicit user cap still applies and unavailable-budget fallback stays 384 MiB.
  Arena stays 384 MiB/generation and 1152 MiB total. These are bounded pool quotas,
  not a claim that the entire renderer's VRAM is capped at their sum. The 384 MiB
  same-class pinning test independently proves hole reuse without enlarging it.

Remaining acceptance: real GPU completion/lifetime, outline/evidence/Shader API
coexistence, pressure then recovery at 2560x1440, shadow completeness, GPU incidents,
and frame-time distributions. The metadata exhaustion fallback can still seal a
fragmented page; there is no live-byte compaction. Physical allocator cache release
may lag logical shrink; runtime recovery latency is not measured. No FPS, complete
fissure fix, universal OOM prevention or player acceptance is claimed.
