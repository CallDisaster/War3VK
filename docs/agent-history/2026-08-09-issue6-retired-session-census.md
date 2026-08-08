# Issue #6 retired-session census — 2026-08-09

## Investigation result

The map-transition audit did not find a new proven early-release path:

- the active Arena generation is quarantined and signalled after old commands;
- GPU-skin moves an in-flight resource epoch into its bounded retirement owner;
- the receiver drains `std::async`, shuts down the persistent point-shadow
  worker and invalidates CSM/TAA/point publications on the CS thread;
- D3D9 shadow buffers and cache containers move into a dedicated session that
  remains strongly referenced until the Arena completion serial advances.

The remaining diagnosis was not closed, however. Runtime status exposed only
the number of retired sessions. It could not distinguish a normal one-frame
fence delay from a session that retained tens of MiB indefinitely, so an A-to-B
performance loss still had no owner-level evidence.

## Implemented census

Each retired shadow session now seals value-only gauges when ownership moves:

- cached entry count across freeze, persistent, Stage13, draw-time and terrain
  containers;
- allocator chunk bytes;
- cached GPU logical bytes from the recorded ranges;
- CPU-owned Stage13 payload bytes;
- map epoch and retire serial.

The GPU logical figure is explicitly not unique residency: multiple entries
may alias one backing. Allocator bytes are reported separately so the physical
chunk pressure is not hidden by that logical count.

Fence collection now refreshes aggregate outstanding gauges and increments a
cumulative collected-session count. `runtime_status.json` exposes the
outstanding entry/byte totals, oldest retire serial, collected count and last
retired map epoch. This change does not alter resource lifetime, fence order,
Arena capacity or publication policy.

## Verification and physical gate

- 69/69 static test modules passed.
- 17/17 Win32 Meson runnable tests passed.
- Win32 DLL build passed; `ninja -C build32 -n` reported no work.
- `git diff --check` passed.

The candidate is not deployed. A physical cold-B / A-to-B / A-to-B-to-A run
must show outstanding retired sessions, entries and bytes returning to zero
after `completedRetireSerial` reaches the oldest retire serial. If they do but
global residency or CPU time remains elevated, the next audit moves to the
GPU-skin retired-resource census and non-shadow D3D9 allocator owners rather
than changing this fence queue.
