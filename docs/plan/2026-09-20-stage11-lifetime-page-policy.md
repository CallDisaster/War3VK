# 2026-09-20 Stage11 lifetime-grouped page policy prototype

Status: memory-b01 offline pure-header prototype and source audit.
Not compiled, not deployed, not wired into `device.cpp`, no default switch
changed. The production integration idea below is **not applied**. Any
improvement discussion here is **synthetic** model reasoning, not player gain
or real census.

## 1. P1b account audit

### 1.1 Fact table: producers, holders and evidence

| Role | Current anchor | Proven | Accounting consequence |
| --- | --- | --- | --- |
| Static candidate | `d3d9_device.cpp` `generationBackedStaticCandidate` (about 42503-42516) | `!isDynamicUnit && !gpuSkin...`; dynamic evidence comes from dynamic pose / unit identity | Only a lifetime hint for a new allocation; not a proof of no future mutation |
| Final static tag | `entry.isStaticGeometry = generationBackedStaticCandidate && !entry.gpuSkinLeaseBacked` (about 43954-43956) | rigid geometry enters long-lived cache; GPU-skin lease does not | Dynamic/static TTLs differ, but the page itself has no lifetime field |
| Capture attempt | `War3DrawTimeSnapshotCaptureAttempt` construction (about 43094-43096) and destructor (lifetime header) | Construction immediately clears `captureComplete`; failure/exception keeps capacity and recomputes `ownedGpuBytes` | failed capture is a retained failure reference, not a free; a current-frame failure is classified as Touched |
| Position allocation | `War3AllocateStage11Snapshot` call (about 43323-43334) | page can be allocated before UV/index finish | a later UV/index failure can still retain the position slice; page selection cannot wait for final commit |
| UV/index allocation | about 43603 and 43752 | streams can land on different pages; UV may alias position | multiple slices on one page are normal; UV alias must not be double-counted |
| Active page owner | `m_war3Stage11SnapshotPages` (`d3d9_device.h` about 2629) | current lookup owner of page objects and `Rc<DxvkBuffer>` | scan covers CPU owner lookup, not complete physical residency |
| Entry page refs | `positionSnapshotPage` / `indexSnapshotPage` / `uvSnapshotPage` (about 2667/2687/2774) | each entry holds `shared_ptr`; a shared UV aliases the same page | `page.use_count()` changes with aliases/entries and is not GPU completion proof |
| Active cache and retired session | `m_war3DrawTimeVBCache` (about 2851-2853), `War3RetiredShadowSession::drawTimeVbCache` (about 2944-2971) | map/device epoch reset moves the cache into a fence-owned session (about 23992) | clearing active lookup does not destroy in-flight backing; retired pages keep page refs |
| Page reclaim | `War3CollectUnusedStage11SnapshotPages` (about 23827-23839) | only `page.use_count() == 1` is erased from the active vector | this is not GPU/CS completion proof; P3 may not turn it into an overwrite lease |
| Retire fence | `War3CollectRetiredShadowSessions` (about 23629-23644) | retired session is released only after `retireSerial <= completedSerial` | whole-page retirement responsibility stays with the owner/fence, not this policy |
| P1b owner classes | sampler about 23922-23932 | Touched / Failed / RecentCache / ColdCache / Retired by entry state | classes are reference states, not per-page short/long/failed byte classes |
| P1b page sweep | `war3_stage11_budget_census.h` `Collector::finish` | endpoint union dedup, `owned[Owner]`, `unreferencedUsed`, `tail` | no lifetime dimension; `unreferencedUsed` is explicitly not a reusable hole |
| Logical owner helper | `war3_draw_time_snapshot_lifetime.h` `War3DrawTimeOwnedSnapshotBytes` | UV alias counted once; logical slice capacity only | not physical VRAM and not GPU residency |
| Extra buffer refs | `positionPinnedAllocation` / `indexPinnedAllocation` / `uvPinnedAllocation` | direct static/upload allocations have page capacity zero | any P3 policy must handle those allocation refs separately |

### 1.2 Answer: P1b cannot distinguish same page short/long/failed refs

1. `Owner` is an entry-state bucket, not a lifetime bucket. A short-lived dynamic
   slice and a long-lived static anchor on the same page fall into the same
   `owned[Touched/RecentCache/...]` total.
2. `staticReferences` counts references, not static bytes. `owned` has no
   short/long/failed dimension, so P1b cannot answer how much of a 16 MiB page
   is anchor bytes, dynamic bytes, or failed-retained bytes.
3. `Failed` appears only for `!captureComplete` when the entry is not touched
   this frame. A current-frame failed capture is first classified as Touched, so
   its failure/revocation label is temporarily lost.
4. `RecentCache` uses a 120-frame protection window, but the census does not
   require `entry.isStaticGeometry`, so dynamic entries can share this bucket.
   It cannot be used to infer dynamic TTL behavior.
5. `notReferencedByCacheBytes` is not a hole and `tail` is not an overwrite
   hole; neither authorizes GPU reuse.
6. `page.use_count()` / `pageReferences` only prove additional CPU owners or
   aliases, not completed CS/GPU use. Keep
   `physicalBackingComplete=false` / `gpuCompletionKnown=false`.

P1b is sufficient for pressure localization and mixed-page suspicion, but not
for approving P3 grouping by itself. The parent should obtain a per-page
lifetime/retention dimension from real census before wiring.

## 2. Lifetime page policy prototype

The new header `src/d3d9/war3/render/war3_stage11_lifetime_page_policy.h`
defines `War3Stage11PageLifetime` (`Unknown`, `ShortLived`, `LongLived`) and
`War3Stage11PageOwnerState` (`Active`, `RetirePending`, `Retired`).
`RetirePending` means CPU cache refs are gone but CS/GPU/backing retirement is
not proven; its tail is never selected. The policy returns only
`ExistingPage`, `NewPage`, or `NoSafeSelection`.

Selection order:

1. Existing `Active` page with the same lifetime and matching epoch, append to
   its tail. Multiple slices on one page are legal; UV alias does not change
   page ownership.
2. Otherwise use an `Unknown` page first; `Unknown` is conservative-compatible
   and must not cause an omitted caster. An `Unknown` request may use a known
   page, marked `conservativeUnknownPage`.
3. Otherwise create a new page for the requested lifetime / `Unknown`, using
   the existing `War3Stage11SnapshotPageCapacity` 16 MiB granularity and shared
   cap. No second allocator is introduced.
4. If creation is blocked by the shared cap, page-vector limit, or the current
   create gate, but an `Active` page tail fits, allow a cross-lifetime
   `borrow` marked `mixedLifetimeBorrow`. This is a correctness-preserving
   fallback against rejecting a legal caster before the shared cap is reached,
   not a default mixing policy.
5. Only if none of the above exist return `NoSafeSelection`; the caller stays
   fail-closed and must not free a live page.

The planner never frees, copies, or overwrites; the caller still performs the
existing `createBuffer` / `War3PublishStage11SnapshotPage` work. Existing-page
plans do not increase `residentBytesAfter`; new-page plans add exactly one page
capacity, so resident accounting is not double-counted.

Failure handling: a failed capture keeps its retained entry/page refs and
provides no new page authorization. `captureAttempt` clears `captureComplete`
before allocation, so wiring must use independent producer evidence
(`isDynamicUnit`, `semanticHasDynamicPose`,
`generationBackedStaticCandidate`), not `entry.captureComplete`. Extra refs
such as `positionPinnedAllocation`, GPU-skin lease, and direct static/upload
backing are outside the Stage11 page view and remain owned by their original
owners/fences.

## 3. Suggested production patch (not applied)

1. In `d3d9_device.h`, add `War3Stage11PageLifetime lifetime` and
   `War3Stage11PageOwnerState ownerState = Active` to `War3Stage11SnapshotPage`;
   include the new header.
2. Add a `requestedLifetime` parameter to `War3AllocateStage11Snapshot` and
   compute it once at the position/UV/index capture call sites from producer
   evidence.
3. In the allocator, build at most 32 `War3Stage11LifetimePageView` entries from
   the current page vector and call `War3PlanStage11LifetimePage`. Existing-page
   results supply page/offset/capacity; new-page results still use the existing
   `createBuffer` and `War3PublishStage11SnapshotPage` path with the selected
   lifetime.
4. Map plan reasons: `CreateGateClosed` to `PageCreateBudget`;
   `SharedCapReached` / `PageVectorLimit` to `ResidentCapacity`;
   `InvalidRequest` to `InvalidRange`. Keep existing failure evidence.
5. Do not use `War3CollectUnusedStage11SnapshotPages` as the only P3 retirement
   proof. P3 must move a page to `RetirePending` when CPU cache refs disappear
   and only to `Retired` after CS/GPU/backing completion evidence.

Predicted cost: O(active pages) integer checks per allocated stream, bounded by
32 pages; no heap, lock, I/O, or byte copy. This is a design prediction, not a
measurement. Falsifiable real-machine fields include
`drawTimeSnapshotPageResidentBytes`, `drawTimeSnapshotPageUsedBytes`,
`drawTimeSnapshotPageCreateCount`, `drawTimeSnapshotPageCapacityRejectCount`,
new `mixedLifetimeBorrow` / `conservativeUnknownPage` / `retirePendingSkipped`
counters, copy bytes, required caster omission, producer incomplete, main
thread p95/p99, CSM render serial, receiver completeness, and shadow recovery
after a low-angle round trip.

## 4. Synthetic counterexample

This is synthetic and not a player workload: 24 rounds with one 256 B long
anchor and one almost-16-MiB short slice can pin 24 pages under mixed-page
append. Lifetime grouping keeps the long anchors on their own page, while
short pages become reclaimable after refs disappear and retirement is proven.
That statement proves only that the offline policy can express the
counterexample, not that a real run improves.

## 5. Next-batch recommendation

1. Take a real per-page lifetime/occupancy census first; if P1b cannot provide
   the dimension, extend P1b instead of enabling P3.
2. If real data supports grouping, apply the Section 3 wiring behind an
   independent default-off switch and let validation compile/run
   `AutoTest/test_stage11_lifetime_page_policy.cpp`.
3. Use only the same candidate, map, 2560x1440 non-interactive isolation and
   zero global input for real measurement; compare resident, create, rejects,
   copy bytes, p95/p99 and shadow recovery.
4. If grouping rejects a legal caster before the shared cap is reached, keep the
   borrow fallback; never omit casters with a reserved quota.
## 6. memory-b07 corrected Python model accounting

Status: bounded offline model only.  This section does not authorize P3, a
production allocation change, a new physical cap, or any claim of physical
savings or fence proof.

The historical six-scenario `_simulate` fixture remains in
`AutoTest/test_stage11_lifetime_page_policy_static.py` as the original baseline.
A separate corrected model now tracks three accounts explicitly:

1. **Active CPU-owned page bytes**: pages with live cache-owner references.
   The shared 384 MiB value is a software-pool admission cap on this active
   account only.
2. **Retired GPU/CS pending backing**: pages whose CPU owner refs are gone but
   whose completion serial or reset/retired hold has not finished.
3. **Truly freed completed backing**: pages erased only after both the CPU
   reference domain and the CS/GPU completion domain released, and any
   reset/retired hold expired.

The corrected model uses a deterministic bounded schedule with CPU refs,
last-use completion serials, whole-page allocation alignment, paired
CPU/GPU retirement, delayed completion, reset/retired hold, repeated return
and unknown-lifetime pressure.  It preserves the same ordered demands and
release events for grouped and mixed-tail strategies.

An independent `LiveByteLedger` oracle records allocated, freed, live and live
peak bytes, and the model verifies that live bytes equal the capacity of all
not-yet-erased pages.  It deliberately can show live pending backing above the
384 MiB software-pool cap because the cap is not a physical residency cap.

The earlier 416 MiB output was a cumulative-allocation artifact: the old model
never erased pages.  The corrected model reports allocated, freed and current
live bytes separately; the 16 MiB active-CPU observation is not a physical or
runtime gain.  No hole reuse, early reclaim, overwrite, production fence proof
or shadow-recovery claim is made.
