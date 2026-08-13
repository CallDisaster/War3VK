# Current-draw coherent UP index trimming

## Scope

This note defines a development-only observation and A/B boundary for reducing
WarVK Shadow Arena traffic. It does not authorize cross-frame VB/IB reuse and
does not change the Release path by default.

The old exact-index trim proved an index domain from one CPU generation while
the position bytes later consumed by replay could come from a different REAL
buffer generation. That combination was disabled after a device-reset A/B.
The replacement considered here is narrower: position, blend/UV and index data
must all be the bytes copied for the same `UploadPerDrawData` call and pinned by
the same `DxvkResourceAllocation`. The proof and the copied snapshot exist only
for that draw.

## Primary contracts

- Microsoft documents that `DrawIndexedPrimitiveUP` completes access to the
  supplied vertex data before returning, so the application does not have to
  retain its original pointer afterwards. WarVK therefore must own a copy for
  deferred replay; it cannot retain the caller pointer.
  <https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-drawindexedprimitiveup>
- Vulkan requires every `vkCmdCopyBuffer` source and destination range to lie
  inside the named buffers and forbids overlapping ranges in the same memory.
  <https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdCopyBuffer.html>

## WarVK mapping

`UploadPerDrawData` already copies all SYSTEMMEM/UP streams needed by one draw
into one owned allocation, changes the draw arguments to address those packed
slices, and retains exact CPU pointers plus `DxvkBufferSlice` objects. A trim is
coherent only when:

1. position and index are both current per-draw uploads;
2. their `Rc<DxvkResourceAllocation>` owners are identical;
3. CPU byte ranges cover the exact index count and the complete packed vertex
   capacity;
4. the exact index domain, signed base vertex, blend stream and mandatory alpha
   UV stream all fit that packed capacity;
5. rebased indices and compact vertex streams are copied immediately into
   WarVK-owned current-frame snapshot storage before later command submission.

The build option `warvk_coherent_up_index_trim_dev` defaults to false. In an
explicit development build, environment mode `1` observes without mutating;
mode `2` may consume only this coherent current-draw subset. The Release DLL is
unaffected regardless of environment variables.

## 2026-08-12 isolated Observe result

The development observer was run for 120 seconds in the life-and-death test
scene on the non-interactive desktop. The foreground desktop handle remained
unchanged, the scenario completed, and no device loss, Arena overflow or
producer-incomplete frame was reported. All five coherent-trim counters stayed
at zero for every captured status sample:

- observed: 0;
- eligible: 0;
- would-save bytes: 0;
- consumed: 0;
- consumed bytes saved: 0.

Evidence is stored under
`AutoTest/artifacts/life_and_death_tdr/20260812_041826`. The map's dominant
terrain fallback does not upload position and index through the same current
UP allocation, so this route has no opportunity here and must not be enabled
for Consume. The next experiment must retain the already-owned current
position allocation independently of the index source; it must not infer a
cross-frame immutable terrain generation.

## Current-UP position replay candidate

The follow-up candidate keeps the exact `DxvkResourceAllocation` that
`UploadPerDrawData` created for the current main draw and lets the shadow draw
reference that same position range. It does not retain the Warcraft caller's
pointer, invent an immutable generation, or reuse anything in a later frame.
The replay binding records the pinned allocation and the consumer command list
tracks it until its completion fence retires.

The production eligibility helper requires the current UP marker, identical
virtual buffer owner, a non-null pinned allocation, and a replay byte range
fully contained in the uploaded slice. The initial scope is position only;
blend, UV and index streams continue through their existing exact paths. This
keeps mixed-stream ownership explicit and avoids changing skinned geometry.

The additional build option `warvk_current_up_shadow_replay_dev` defaults to
false. Mode 1 is Observe and mode 2 is an isolated Consume candidate. Neither
mode is reachable in the default Release build. A Consume decision removes
only the proven position bytes from fallback admission and Arena reservation;
all remaining streams still form their normal transaction.

Vulkan permits binding buffers backed by host-visible memory; correctness is
defined by the buffer usage, range and synchronization contracts rather than a
requirement that vertex data reside in device-local memory. WarVK already uses
this allocation for the main draw. See the Vulkan vertex-input chapter and
buffer binding command reference:

- <https://docs.vulkan.org/spec/latest/chapters/fxvertex.html>
- <https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdBindVertexBuffers.html>

### Isolated Observe result

The 120-second isolated run under
`AutoTest/artifacts/life_and_death_tdr/20260812_044653` completed without a GPU
event, incident, Arena overflow, busy reuse or frame-incomplete signal. It
observed 817,360 position-freeze draws, but accepted zero current-UP position
ranges and therefore reported zero avoidable bytes. The deployed baseline DLL
was restored to SHA-256
`79CA8DB4C73E47357E586CA3B6BE74F267F378AC13E2272F1D4F4722CDD8B2A4`.

This is a useful negative result: the high-pressure terrain producer uses the
ordinary dynamic REAL-buffer route rather than the per-draw UP allocation. A
current-UP Consume run has no work to remove and is rejected. Directly pinning
the ordinary dynamic buffer would not be equivalent because subsequent draws
can replace its contents before shadow replay. That route remains forbidden
without an upper-layer immutable tile generation or an earlier cull decision.
