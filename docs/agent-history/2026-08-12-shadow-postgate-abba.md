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
