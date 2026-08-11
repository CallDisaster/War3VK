# Current-draw coherent REAL index trim

## Problem

The 2026-08-12 high-pressure `life_and_death_tdr` Observe run still spent an
average 75.565 MiB and a peak 383.992 MiB of the per-generation shadow Arena.
Nineteen of 5,423 frames were rejected as producer-incomplete, accounting for
2,166 required caster omissions. Every omission was attributed to the fallback
byte budget. The dominant fallback was indexed terrain whose D3D9 draw range
did not provide a trusted exact vertex domain, so the old path froze a much
larger position slice than the indices actually referenced.

The historic cross-frame exact-index trim is not safe for this cohort: an
index range scanned from one CPU generation can be paired later with a mutable
REAL vertex backing from another generation. The new route therefore makes no
cross-frame claim and does not cache a raw `VkBuffer` binding.

## Source contracts

Microsoft documents `StartIndex` as the first index consumed by a D3D9 indexed
draw and `MinVertexIndex`/`NumVertices` as the potentially accessed contiguous
vertex range. It also notes that the range may conservatively cover the whole
buffer, so WarVK must scan the current index bytes before using a smaller exact
domain:

- https://learn.microsoft.com/en-us/windows/win32/direct3d9/rendering-from-vertex-and-index-buffers
- https://learn.microsoft.com/en-us/windows/win32/api/d3d9helper/nf-d3d9helper-idirect3ddevice9-drawindexedprimitive

Khronos defines `vkCmdCopyBuffer` as copying the specified source regions at
GPU command execution time. A mutable capture-time source therefore cannot be
treated as an immutable CPU proof merely because its `VkBuffer` handle is
stable:

- https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html
- https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdCopyBuffer.html

## Implemented boundary

`WARVK_ENABLE_COHERENT_REAL_INDEX_TRIM_DEV=1` exposes a development-only
`Off / Observe / Consume` policy. The default Release configuration is Off and
cannot parse the environment variable into an active route.

The route accepts only the same current D3D9 draw when all of these facts hold:

- indexed terrain caster;
- rigid, opaque and not alpha-tested;
- ordinary dynamic REAL position buffer with a current host-visible mapped
  allocation;
- current host-readable index span covering the complete requested index
  range;
- finite, in-range position/index domain with the current identity,
  allocation and content generations still unchanged.

Consume immediately copies the compact position range and rebased index bytes
into CPU-owned scratch during that same draw. The existing Arena transaction
then copies those immutable bytes into its own mapped snapshot. Dynamic,
skinned, alpha-tested, blend, unreadable or generation-ambiguous draws retain
the existing exact fallback. This is not a persistent VB/IB cache and does not
restore the retired fingerprint path.

## Evidence

Observe DLL before the final report-field rebuild:

- SHA-256: `EDB879976683B35558A1DBFC4591CFF05151EB83C1D82ED6DBFAFA123FBF68E4`
- artifact: `AutoTest/artifacts/life_and_death_tdr/20260812_054201`
- 5,423 frames; 1,005,737 observed draws; 210,318 eligible draws;
  109,636,594,496 bytes potentially avoided;
- Arena average/peak: 75.565 / 383.992 MiB;
- producer-incomplete frames/omissions: 19 / 2,166;
- device lost, new GPU events and new GPU incidents: zero.

Consume short gate using the same implementation:

- artifact: `AutoTest/artifacts/life_and_death_tdr/20260812_054816`
- 4,117 frames; 155,441 consumed draws; 81,035,522,368 bytes avoided;
- Arena average/peak: 54.172 / 354.656 MiB;
- producer-incomplete frames, required omissions, fallback byte-budget
  omissions, replay rejects, partial publications, Arena overflow and partial
  transactions: all zero;
- device lost, new GPU events and new GPU incidents: zero.

Final rebuilt development candidate:

- SHA-256: `D1A14CB05BE7952893742D4C688291851C36CDB43FBEEFC4850C1C3C6A2F5E9C`
- artifact: `AutoTest/artifacts/life_and_death_tdr/20260812_060219`
- 13,780 frames; 507,638 consumed draws; 264,666,924,256 bytes avoided;
- Arena average/peak: 60.879 / 367.789 MiB;
- producer incomplete, required caster omission, fallback byte-budget
  omission, budget exceeded, partial publication, device lost, new GPU events
  and new incidents: all zero.

These are isolated-desktop stability and relative A/B results, not foreground
FPS or player visual acceptance. Release remains Off until a later explicitly
authorized default decision and foreground visual review.
