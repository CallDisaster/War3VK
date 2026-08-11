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
