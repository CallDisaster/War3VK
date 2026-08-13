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

The first Consume prototype copied the compact position range and a rebased
index range into CPU scratch before the Arena transaction. PostGate attribution
proved that this double copy was not acceptable. The revised candidate retains
the mapped allocations, revalidates both generations, and synchronously freezes
the exact position and raw index subranges into the Arena before returning from
the same D3D draw. Replay uses the corresponding signed vertex offset and the
existing exact-domain validator. It does not retain either mapped pointer after
the draw. Dynamic, skinned, alpha-tested, blend, unreadable or
generation-ambiguous draws retain the existing exact fallback. This is not a
persistent VB/IB cache and does not restore the retired fingerprint path.

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

## CPU A/B correction

A development-only PostGate observer was enabled only in builds configured
with `warvk_shadow_observers_dev=true`; the default Release configuration
cannot enter the observer. It identified that the first Consume implementation
reduced Arena traffic at the cost of CPU copies:

- Off report `war3_perf_report_auto_2026_08_12_06_31_34.html`:
  `ResourceResolve=0.048 ms`, main CPU `10.308 ms`, Arena average/peak
  `79.157/383.976 MiB`, but 19 producer-incomplete frames and 2,564 required
  omissions;
- scratch Consume report `war3_perf_report_auto_2026_08_12_06_28_02.html`:
  `ResourceResolve=4.972 ms`, main CPU `13.448 ms`, Arena average/peak
  `54.389/365.604 MiB`, with no incomplete frame;
- direct position span report
  `war3_perf_report_auto_2026_08_12_06_37_37.html`:
  `ResourceResolve=4.426 ms`;
- direct position plus raw-index span report
  `war3_perf_report_auto_2026_08_12_06_45_11.html`:
  `ResourceResolve=0.882 ms`, main CPU `10.399 ms`, Arena average/peak
  `60.129/366.742 MiB`, with no incomplete frame or required omission.

The final pair had similar aggregate main/process CPU despite differing scene
work, so it proves that the multi-millisecond scratch regression was removed;
it does not yet prove a foreground FPS gain. The remaining approximately
`0.8 ms` ResourceResolve cost is the current exact index-domain scan and is not
authorized for Release without a generation-safe reuse design and a new A/B.

The corrected direct-span/raw-index candidate then completed a 303.844-second
isolated low-view patrol:

- commit: `22fa27e`;
- artifact: `AutoTest/artifacts/life_and_death_tdr/20260812_064731`;
- report: `war3_perf_report_auto_2026_08_12_06_52_36.html`;
- 8,750 observed shadow frames and 368,838 consumed terrain draws;
- 192,253,295,072 bytes avoided;
- producer incomplete, required omission, allocation/fallback/admission/freeze
  failures, budget exceeded, partial publication and replay rejection: zero;
- sampled Arena p50/p95: `26.706/63.929 MiB`; frame-window average/maximum:
  `64.316/369.132 MiB`;
- device lost, new Event 153/4101 and new GPU incident: zero.

The p95 target is met, but the instantaneous peak remains above the desired
128 MiB target. The route therefore remains development-only and needs a
foreground visual gate plus further peak reduction before any Release default
decision.

## Generation-backed exact-domain reuse

The remaining `ResourceResolve` cost was the repeated min/max scan over the
current index span. A development-only cache now reuses only that derived POD
domain. Its key includes map/device epoch, buffer owner and mapped address,
identity/allocation/content generation, span length, index width/count,
base-vertex and position capacity. Every lookup therefore starts after the
current readable span and all generations have been revalidated. The cache
does not retain CPU bytes, a resource owner, a `DxvkBuffer` slice or a Vulkan
binding, and cannot restore the retired cross-frame fingerprint route.

The same development DLL exposes
`DXVK_WAR3_COHERENT_REAL_DOMAIN_CACHE=0/1` only when coherent REAL development
support is compiled. Release cannot reach either the trim route or this cache.
Four 7,200-frame isolated reports formed a B-A-B-A sequence:

| Report | Cache | ResourceResolve | Calls/frame | Incomplete/budget exceeded |
| --- | --- | ---: | ---: | ---: |
| `07_35_17` | on | 0.523 ms | 94.620 | 0 / 0 |
| `07_42_30` | off | 0.742 ms | 90.384 | 0 / 0 |
| `07_46_36` | on | 0.536 ms | 94.553 | 0 / 0 |
| `07_49_55` | off | 0.731 ms | 92.782 | 0 / 0 |

The two on runs save about `0.207 ms/frame` against the two off runs even
though they perform more measured resolve calls. They record roughly 685k
lookups and 145k hits each (about 21%). Enlarging the table from 1,024 to
16,384 entries did not materially change the hit count because most misses
carry a genuinely different content generation. The final source therefore
keeps the 1,024-entry deterministic set-associative table instead of spending
additional TLS memory for false capacity. No TDR, new GPU event, incident,
producer-incomplete frame or budget-exceeded frame occurred in these runs.

This proves an isolated relative CPU win for the generation-backed lookup. It
does not prove foreground FPS or visual acceptance, and does not authorize a
Release default.

The final 1,024-entry build (`917B1503...E44`) was then run independently in
`AutoTest/artifacts/life_and_death_tdr/20260812_075326`; report
`war3_perf_report_auto_2026_08_12_07_55_31.html` measured
`ResourceResolve=0.511 ms/frame` at 96.656 calls/frame, with 148,036 hits from
699,675 lookups. All 7,200 report frames again had zero producer-incomplete or
budget-exceeded frames, and the run created no GPU event or incident. The
stable deployed DLL was restored to SHA-256 `79CA8DB4...B2A4` afterward.

## Comparable ten-minute pressure result

The default Release build was then run three times for ten minutes with the
5x5 low-view path. It produced `79/84/76` incomplete frames, approximately
`9.5k` fallback-byte omissions per run, and a roughly 384 MiB Arena peak. The
coherent REAL candidate ran the same 600-second matrix in artifact
`20260812_083240` and report `08_42_45`:

- 17,327 observed shadow frames;
- 763,816 consumed terrain draws and 398,147,461,408 bytes avoided;
- zero producer-incomplete, required-caster omission, fallback-byte omission,
  budget-exceeded, GPU incident and new driver event;
- Arena average/peak `66.011/369.375 MiB`.

This closes the candidate completeness problem without increasing the Arena
limit, but it does not yet satisfy the performance/default gate. In the same
long matrix, `ShadowCapture` averaged `3.439 ms` versus about `1.706 ms` in the
default run. The remaining cost is dominated by proving a fresh exact index
domain when content generation changes; the generation-backed cache only
helps repeated draws within an unchanged generation. The next step is an
Observe-only comparison of D3D9 supplied index-range hints against the already
computed exact domain. No hint may authorize trimming until under-coverage is
proven to be zero under an explicit contract.

The follow-up Observe build reused that already-computed domain and compared
every eligible draw against D3D9's declared range. Artifact
`20260812_084754`, report `08_58_00`, recorded:

- 3,541,747 comparable indexed terrain draws;
- 3,514,868 exact ranges;
- 26,879 conservative supersets;
- zero under-coverage and zero invalid ranges;
- zero producer-incomplete/budget-exceeded frames, device loss, new GPU event
  or incident.

Microsoft's `IDirect3DDevice9::DrawIndexedPrimitive` contract states that
`MinVertexIndex` is the minimum index relative to `BaseVertexIndex`,
`NumVertices` defines the used range, and indices outside that range are
invalid. This is stronger than an informal performance hint:

- https://learn.microsoft.com/en-us/windows/win32/api/d3d9helper/nf-d3d9helper-idirect3ddevice9-drawindexedprimitive

The sample and API contract justify a separate development candidate that can
use this declared superset for the already restricted terrain/rigid/opaque
cohort. They do not authorize changing general bounds culling, UP/skinned/
alpha routes or the Release default in the observer commit itself.

## D3D9 declared access-domain development candidate

The follow-up candidate turns the documented D3D9 range into a checked compact
position slice only for the existing coherent-REAL terrain/rigid/opaque route.
It validates signed base addition, position capacity and the full UINT16 or
UINT32 raw-index domain. The declared superset is not published as the tighter
actual domain used by general bounds culling.

The same DLL ran B-A-B-A for 120 seconds per run with only
`DXVK_WAR3_COHERENT_REAL_HINT_DOMAIN` changed. `ResourceResolve` measured
`0.087 / 0.700 / 0.084 / 0.685 ms` for on/off/on/off; total `PostGate` measured
`1.250 / 1.867 / 1.232 / 1.803 ms`. The average local saving is approximately
`0.607 ms/frame`. All four runs recorded zero producer-incomplete frames,
device losses, new GPU events or incidents.

A 601.6-second isolated 5x5 low-view gate (`20260812_092422`) also completed
with zero incomplete frames, device loss, new Event 153/4101 or incident. Its
long-run `ResourceResolve` was `0.172 ms/frame`; `FreezeBuffers` is now the
dominant child at `1.284 ms/frame`. The candidate remains development-only and
undeployed. Isolated evidence cannot replace foreground visual review or
authorize a Release default change.
