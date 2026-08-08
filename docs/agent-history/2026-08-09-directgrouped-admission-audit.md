# DirectGrouped admission audit (2026-08-09)

## Evidence

The latest available foreground report is
`E:\Work\War3\WarVK\Log\war3_perf_report_2026_08_08_22_51_32.html`
(DLL `BCD5F8A5...E839`, 3600 frames). Its corrected profiler reports:

- `DirectGrouped`: 1.522 ms/frame average, 4.473 ms p95.
- `BuildEligible`: 1.199 ms/frame average and 78.8% of DirectGrouped.
- 273,668 CurrentDraw packets were built.
- The workload series proves 396,028 draw-time producer candidates were all
  fresh, claimed and submitted.
- Total semantic submissions were 396,536, so all other semantic producers
  together added only 508 submissions in the same window.
- The report accumulated 194,446 late DirectGrouped append failures, but the
  aggregate report omitted the exact-owner/producer ledger required to
  classify those failures.

This proves a large duplicate-control-plane opportunity, but it does not yet
prove that a weaker pre-build identity may safely suppress work. The exact
producer currently owns a collision-resistant key containing map epoch,
instance, mesh payload, renderable part, JASS handle, layer and payload words.
DirectGrouped often cannot construct that exact key until after packet build.

## Change in this checkpoint

No caster selection or render behavior changes. The performance monitor now
aggregates and exports the existing draw-time producer counters into both
`shadowBudgetSummary` and `shadowRuntimeV2Summary`:

- visible candidates and fresh entries;
- claimed and submitted entries;
- missing-fresh and CurrentDraw fallback counts;
- exact-owner DirectGrouped suppressions;
- lifecycle merges.

The next physical performance report can therefore distinguish a late
exact-owner suppression from a genuine geometry, alpha, blocker or resource
failure. A Producer Claim Ledger may proceed only in Observe mode until its
pre-build logical key has zero false positives and false negatives against the
existing post-build exact-owner decision.

## Boundary

- This checkpoint is diagnostics-only and is not a performance optimization.
- It is not deployed and has no physical map result.
- Claim Ledger Consume, Compact WorkTable Consume and all persistent-package
  Consume paths remain disabled.
