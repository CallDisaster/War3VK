# Shadow PostGate low-overhead attribution

## Result

The low-overhead `life_and_death_tdr` run disabled Compact WorkTable,
BuildEligible/Direct phase breakdown, Stage11 allocation observation and both
culling observers. It retained only the development coherent REAL trim route.
The resulting report was
`E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_08_12_06_21_50.html`.

`CaptureLiveState/ResourceStoreBuild` averaged `0.017 ms/frame` and
`ManifestCopy` averaged `0.030 ms/frame`. Neither reaches the project's
`0.15 ms/frame` implementation gate. `DirectGrouped` averaged `0.690 ms`, but
the existing WorkTable/Claim observers still lack sealed source identities and
therefore cannot safely Enforce.

The dominant WarVK producer cost was instead `ShadowCapture/PostGate`. A
development-only recursive observer attributed the first trim candidate to
`ResourceResolve`, not `FreezeBuffers`, Arena allocation, persistent lookup or
bounds. The A/B and the corrected direct-span implementation are documented in
`docs/research/2026-08-12-coherent-real-index-trim.md`.

## Boundary

- The PostGate recursive observer remains unreachable in the default Release
  build and cannot enable a Consume rendering route.
- ResourceStore/Manifest optimization is stopped for now because its measured
  average cost is below the benefit gate.
- Producer Claim/WorkTable Enforce remains Off because zero-mismatch identity
  evidence does not exist.
- The direct-span coherent REAL candidate is still development-only and needs
  a longer stability gate plus foreground visual confirmation.

## Exact-domain cache follow-up

A complete-key POD cache removed repeated exact index-domain scans without
retaining resource ownership or physical bindings. Development-only B-A-B-A
reports measured `ResourceResolve` at `0.523 / 0.742 / 0.536 / 0.731 ms` for
on/off/on/off, respectively. The average improvement is about `0.207 ms/frame`
and all four reports kept producer-incomplete and budget-exceeded frames at
zero. A larger table did not improve the roughly 21% hit rate, so the final
candidate retains the original bounded 1,024-entry table. Release remains Off
pending foreground visual review.

The final 1,024-entry binary repeated the result at `0.511 ms/frame` over
7,200 frames (`07_55_31`) with zero incomplete/budget frames, GPU incidents or
new driver events. AutoTest restored the stable deployed DLL after the gate.
