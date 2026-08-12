# Direct canonical prefilter handoff candidate

## Problem

The object-grouped preselector already rejected exact-owned records, static
world fallbacks, disallowed producer stages, path blockers and unsafe alpha
records. BuildEligible then ran those same canonical checks again for every
selected record before packet construction. Between the two loops only local
sorting and selection occur; none of the render-thread contracts can change.

## Change

- Grouped preselection now marks its output as canonically prefiltered only
  after the complete selection pass succeeds.
- BuildEligible skips the duplicate exact-cache, policy, blocker and alpha
  probes for that selected subset.
- The uncapped/non-grouped fallback retains every historical gate and counter.
- Compact WorkTable Consume remains under its existing sealed-generation gate;
  this change does not enable it or use compact evidence as canonical truth.

## Boundary

No rejected record can enter the handoff: every canonical gate still executes
before `preselectedRecords.push_back`. Caster order, cap selection, completeness
accounting, alpha/blocker policy and packet construction are unchanged. The
candidate is not deployed and requires fixed-scene A/B measurement.
