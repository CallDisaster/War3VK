# Native sync-only backbuffer readback: bounded caller experiment

This is an opt-in candidate, not a stable update. User authorized isolated build,
deployment and AutoTest, actual 2560x1440, no foreground input. Restore live 139B
after every process; preserve map 1137 and Game E04D. Foreground concurrent load
is a confounder even for percentages. No global sleep, frame fence or LockRect hack.

## Native proof and limits

IDA Game E04D, base 0x6F000000: ED080 snapshots object+560, calls E65A0(request),
ends scene, and only GetBackBuffer/LockRect when request or the captured +560 is
nonzero. Only the +560 branch consumes pBits; it copies rows and owns +56C.
E65A0 disassembly (102 instructions) never reads its stack argument; its existing
frame accounting, texture cleanup and +560 reset stay unchanged. ED080 disassembly
(202 instructions) uses request only for that call and the readback condition.
Whole loaded function fingerprints, before MinHook: ED080 length 621 SHA1
9900e819c11894e87d225417a9f79a262d129793; E65A0 length 328 SHA1
61922bf50f949c4b2a1b9e85a228653f97fe8f56. Modified routine, unreadable object,
nesting, request other than exactly 1 all remain canonical.

The experiment changes only the caller's request argument from 1 to 0 when the
native screenshot request is zero. Original code still reads the screenshot flag
itself; a newly arriving screenshot still takes its original path. No native field
is written. No LockRect result/pBits/pitch is fabricated. DXVK's resource waits,
EndScene, Present, queue ordering and SyncFrameLatency are untouched. This removes
a *caller-requested per-frame serialization*; it can increase CPU/GPU overlap and
possibly input latency. It cannot be accepted from a shorter wait bucket alone.

Microsoft documents that backbuffer locks can reduce performance and that lock
returns actual CPU-readable data. These contracts prohibit fake successful locks:
- https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dpresentflag
- https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3dsurface9-lockrect

## Frozen progression

1. Build once, Below Normal/-j2, exact DLL, no-work and PE32/i386; offline tests.
2. A: same DLL, OBSERVE=1, ELIDE=0, FRAME_TIMELINE=1. One 60-second fresh process,
   full_default, map camera, pause/background throttle disabled. Ready unchanged.
3. Proceed only if 2000 complete selected Present intervals are strictly inside
   sample QPC; every interval has request=1,capture=0,nested=0,unreadable=0,
   exactly one successful native full 1440p lock, no ledger/identity/restore fault.
4. B: identical contract/DLL except ELIDE=1. Every selected interval must have one
   elision and zero backbuffer locks. Both A and B must retain actual frame images,
   real CSM/receiver workload, successful Present, zero device-lost/dumps/GPU events.
5. If valid, finish B then A (ABBA), independent fresh processes and recovery each.
   Require both B full-cadence results below both A results, mean gain >=10%, no
   disappearance of render population, no greater-than-10% A-to-A drift. Report
   CPU, GPU, Present-tail and p95, not just the removed wait. Failures stop the set;
   no quiet replacements or threshold changes. Inconclusive means inconclusive.

Disabled-profile R12/R13 and failing synthetic API probes remain invalid; they
are not part of this A/B. No further random disable combinations are needed here.
Native screenshot physical regression, input-latency, longer stability, map
transition and player foreground verification remain separate pending gates.
Candidate defaults off and cannot be called stable or permanently deployed.
The existing R11 producer reports MissingRequiredPartCount=2; this is retained
as an unresolved baseline counter, not represented as zero or silently cleared.
ABBA requires it unchanged and caster/geometry averages within 5%, while the
enumerated frame-incomplete/budget/receiver/replay closure counters must stay zero.

## Controls

DXVK_WAR3_NATIVE_FRAME_SYNC_OBSERVE=1 installs only with FRAME_TIMELINE=1 and full
loaded-code fingerprints. DXVK_WAR3_NATIVE_FRAME_SYNC_ELIDE=1 enables only on the
recording owner thread. Census appears per complete frame in mainThreadTimeline.
The normal player launch installs no new hook. The diagnostic runner now freezes
changed game-root logs as well as WarVK/Log, closing the earlier missing log copy.

Evidence: AutoTest/artifacts/native_frame_sync_20260913/. No previous failed data
is overwritten. Development changelog will record valid, invalid and pending gates.

## V2 admission correction before a new set

R14 control failed hook identity: Game loaded at 0x5D120000 rather than its
preferred 0x6F000000. No hook installed, no elision occurred, no B followed;
139B was restored and the invalid run is retained. This is not performance data.
The next source revision normalizes only exact HIGHLOW relocation operands in
a stack copy: ED080 offsets 7,AA,C9,F0 and E65A0 offsets 1D,2F,3A,9C,A8, verified
from the pinned E04D PE relocation directory. Full original SHA1s must still match;
there are no wildcard bytes and no runtime code patch except the usual MinHook
entry after validation. Signed relocation directions use PE32 modulo arithmetic.
New independent ABBA names R15/R16/R17/R18; no threshold change or reuse of R14.

## Completed isolated ABBA (not stable/foreground acceptance)

All four runs used EE90AD4702092215948A474EE6EC437CFAB605143512EF09241CED47B5627010,
34,495,769 bytes, PE32/i386. Exact map/quality, actual 2560x1440, noninteractive
desktop, no global input, one fresh process per run, 60-second sample. Each latest
raw report contains 4000 report frames; the strict ledger selects 2000 contiguous
successful Present intervals wholly inside that run's sample QPC window.

| Run | Policy | Complete Present ms | Reciprocal cadence FPS | Complete Present p95 ms | GPU resource wait ms |
| --- | --- | ---: | ---: | ---: | ---: |
| R15 A1 | Observe/native | 12.942973 | 77.262 | 15.0551 | 4.617039 |
| R16 B1 | Sync-only elision | 7.721212 | 129.513 | 10.0621 | 0 |
| R17 B2 | Sync-only elision | 7.959678 | 125.633 | 9.7688 | 0 |
| R18 A2 | Observe/native | 13.366584 | 74.813 | 15.5948 | 4.777822 |

Mean complete interval 13.154779 -> 7.840445 ms: 40.3985% reduction; reciprocal
mean cadence 76.018 -> 127.544: 67.7810% increase. Both B below both A; A drift
3.2729%, below the frozen 10%. This is not the old report's excluded-Present-tail
FPS metric and not a comparison to the historical native 300 FPS claim.

Every A selected frame has request=1/capture=0 and exactly one successful native
full-surface lock. Every B selected frame has the same input population, one
elision and zero backbuffer locks. No nesting/unreadable/unknown argument appears.
Readback inclusive 4.7666/4.9301 ms disappears; Present inclusive stays
0.3238/0.3423 ms in B versus 0.3633/0.3829 in A: this did not merely move the wait
into the measured Present tail. Whole main-thread OS CPU averages 8.1953 -> 7.6797
ms/frame. The remaining approximately 0.61 ms OutsideScopes is still unknown.

Reported caster averages A1/B1/B2/A2=143.379/143.634/143.606/143.542; geometry
work=39155.261/39167.267/39166.666/39151.805, comfortably within 5%. Selected GPU
pass union report averages 2.425/2.462/2.449/2.494 ms, not reduced to zero. All
enumerated incomplete/budget/receiver/replay gates pass; four CSM cascades and
4096 resolution remain. The inherited MissingRequiredPartCount=2 remains 2 in
all four reports; it has not been fixed or relabeled as zero.

A1 and B1 backbuffer-start.bmp were visually inspected once each: same map camera,
scene, unit/building/terrain/shadow layout, particles and UI; no obvious blank or
missing scene in those two stills. Animation phase differs. They are not exhaustive
visual, temporal, native screenshot or input-latency acceptance.

All four runs restored live 139BBE8CE6F941A176B9F4582E601E2841FC26956CB0DD7884903295F4505FD2,
34,450,973 bytes, original video settings and closed their isolated desktop.
No new game dump or GPU event; owned-process settlement and zero game/compiler
processes passed. R18 completed restoration at 22:40:10 even though the chat turn
was interrupted; continuation verified the existing receipt/live before analysis,
and did not rerun it. No stable or permanent deployment occurred.

### Frozen reports

Under AutoTest/artifacts/frame_timeline_20260913/:

- native_sync_a1_r15_20260913/war3_perf_report_auto_2026_09_13_22_26_37.html:
  7,722,174 bytes / 37026F9018433D63CD8C561A6CC6A4726B592CFCABD6C4A6AF26A07D1E0B2C13
- native_sync_b1_r16_20260913/war3_perf_report_auto_2026_09_13_22_35_26.html:
  7,526,659 bytes / 778EEC1870CC664E42B6773881382C0408F2ED937DE40606F6BA8318FA89EB3B
- native_sync_b2_r17_20260913/war3_perf_report_auto_2026_09_13_22_38_04.html:
  7,528,180 bytes / A082D2008F577F57F6F661768B969B40D341CED89F28FC522A33D0205FA5574B
- native_sync_a2_r18_20260913/war3_perf_report_auto_2026_09_13_22_39_53.html:
  7,731,644 bytes / 7273F91A577EAACD56D49693BA42099ED5C28EAEC1370BACED07A08E8B432B6D

All transaction artifacts, raw reports, identities and analyses are indexed in
AutoTest/artifacts/native_frame_sync_20260913/checkpoint.json. R14 stays invalid.
IDA ED080 and E65A0 comments were appended after an IDB backup, read back and saved;
existing names/comments preserved, no Game.dll binary write.

Next: decouple a production policy from diagnostic recording only after native
screenshot, input-latency and sustained resource/visual gates. Current default is
still off. Concurrent foreground load remains a qualification; these reproducible
isolated results cannot be promised as the player's foreground FPS improvement.
