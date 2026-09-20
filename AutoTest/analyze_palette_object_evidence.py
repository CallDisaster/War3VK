"""Strict palette-object/v1 reader: proves chain shape and coverage, never a recovery.

The object-level palette recorder (src/d3d9/war3/tools/war3_palette_object_evidence.h)
reuses the frozen evidence wire (Kind::ShadowState = 12 + label "palette-object/v1").
This reader decodes that carrier out of a schema-7 export envelope and answers only
four questions:

  1. are the recorded fields the fields we froze (fail-visible on any unknown enum or
     any non-zero reserved slot)?
  2. is each object's chain complete in the export (Rejected -> ServedCandidate ->
     Enqueued -> Drawn -> terminal), rebuilt from the event sequence?
  3. does the export prove the round was fully covered, or are dropped / evicted
     counters / truncated streams forcing a "missing chain / not covered" verdict?
  4. is the chain allowed to certify a **same-object** recovery at all (2026-09-17
     上级 03:58 反例修复)?

A served palette candidate is a CPU-side observation. Reaching it is NOT a shadow
recovery, and a Drawn stage is a recorded draw command, not a GPU submission or a
pixel. "Recovered" is emitted only for a chain that carries all four stages, the
recorder's own terminal Recovered verdict, its sawSubmit + sawDraw flags, an
internally consistent hitCount/deltaFrames, an export with zero loss counters, **and**
an identity that is allowed to name one object instance.

Verdict vocabulary (2026-09-17 反例修复)

  * Empty sample can only be "not covered": zero palette events adds
    'noObjectEvidenceExported' to missing and forces coverageComplete = chainComplete
    = False. A vacuous all([]) may never pass.
  * Weak identity / unknown epoch may NOT certify a same-object recovery chain:
    a chain with identityWeak = true or epochUnknown = true may still report
    stageObservationComplete = true (the stage observation is complete), but
    sameObjectCertified is false, certificationRefusals names 'identityWeak' /
    'epochUnknown', conclusion is 'StageCompleteUncertified', and the chain is NOT
    listed in recovered / chainComplete.
  * Same-object certification mirrors the recorder's own Recovered predicate
    (war3_palette_object_evidence.h:176-179 requires
    sawServed && sawSubmit && sawDraw && !orderViolation && !drawSelectionCleared &&
    drawSource != PaletteObjectSource::None), so a self-contradicting chain is refused
    with a named certificationRefusals entry instead of being certified:
    (war3_palette_object_evidence.h:309-311 additionally requires
    hasRejectFact = (firstReason != NotChecked) -- 'Recovered' means 'caught *after a rejection*',
    so a chain whose HEAD event carries NotChecked is refused with recoveredWithoutRejectFact;
    Unknown(0xFF) **is** a real reject fact for the writer and must NOT be refused; that predicate
    was decoded but unused before 2026-09-18 P0-6),
    recoveredWithoutRejectFact; that predicate was decoded but unused before 2026-09-18 P0-6),
    drawSourceMissing (a Recovered terminal whose carried draw source is NoSource),
    selectionClearedAtDraw (that terminal still carries
    selectionClearedByNativeOverride), liveDrawnWithoutSubmit (a live Drawn event with
    sawSubmit = false -- the recorder can only settle such an entry as a non-recovery),
    unknownFrameDomainWithValue (a frame domain flagged unknown while its value is
    non-zero; the production filler MakePaletteObjectFrames() writes 0 + unknown).
  * Chain-sequence strictness (2026-09-17 上级复验反例): chainSequence is a **per
    recorder entry** counter -- every emission writes ++entry.chainSequence
    (war3_palette_object_evidence.h:228/301/343/383/423/455; the one-shot TableFull
    terminal writes 1u at :269) -- so the values inside ONE chain must be **strictly
    increasing and unique**: exactly 1, 2, 3, ... in sequence order. Equal or backwards
    values are an order issue (chainSequenceNotStrictlyIncreasing@<sequence>) at the same
    level as the stage-order checks, and a hole (1,2,4, or a missing head such as
    3,4,5,6,7) is a truncated stage stream (chainSequenceGap@<sequence>) that forces
    "not covered". Duplicates and holes therefore can never certify. Different chains may
    each restart at their own 1: every entry owns its counter, so only the values inside
    one chain are compared (cross-chain restarts stay legal).
  * Header-block accounting is checked, not assumed (2026-09-17 只读对抗审计 D3/D5):
    emitted == exported palette events; terminalEmitted == exported terminal events;
    the seven settled buckets sum to terminalEmitted (the seventh, closedObservationClosed,
    exists only from v4 on -- see OBSERVATION_CLOSED_FIELDS -- and is never required for
    v1/v2/v3); and each closed* bucket equals the number of exported terminals of its
    own kind; watchCount <= kWatchCapacity(1024).
    Any mismatch forces "not covered"/"not certified" (watchCount is a hard ValueError),
    and event.session must equal the words32[10]/[29] mirror -- a mismatch is a hard
    ValueError, so a decoupled mirror cannot certify anything.
  * Strict root validation is **reused** from analyze_frame_evidence.analyze() -- not
    merely its load(). The palette export IS the schema-7 frame-evidence root plus the
    "paletteObject" header block (that reader's root field set is an exact-equality
    check and does not know the palette block, so the block is projected away first).
    Reused checks: root/event session agreement, strictly increasing (monotonic and
    unique) event sequences, reservation vs retention accounting
    (accepted == evicted + exported events), producer loss accounting
    (producerLosses >= reserved - accepted), event field/payload shape. Any violation
    raises ValueError (explicit refusal) and never passes silently. Every non-claim
    gap that reader reports (producer contention, post window not complete, capacity
    freeze, truncated labels, unmatched spans, ...) is folded into missing, so it
    forces "not covered".
  * The frozen CLI exit code is 0 only for a certified chainComplete export; every
    other reading (including "stage observation complete but uncertified") exits 2, and
    a refused export raises ValueError, so the CLI exits 1 with the reason on stderr.

Explicit format version (2026-09-17 上级裁定 ⑦; the block's version is read by
analyze_frame_evidence.extension_version(), which owns the root format registry):

  * The export root stays schema 7 and carries the "paletteObject" block. That block used
    to be an **unversioned** extension, so this reader had to project it away before
    delegating ("root fields mismatch" from the generic reader) and the history/watcher
    entries could not read a palette export at all. The block is now read as a registered
    extension and this reader delegates the **whole root** to analyze_frame_evidence.
  * Version 1 (PALETTE_OBJECT_LEGACY_VERSION) is the legacy contract: either an explicit
    "version": 1 or -- for real artifacts already on disk -- a block with no version field
    at all whose field set is the frozen {watchCount, counters}. data[3..8] and
    words32[15] stay reserved-zero and every old-format artifact stays readable under the
    old contract (identical grouping and identical certification outcome).
  * Version 2 (PALETTE_OBJECT_SEGMENTED_VERSION) is the segmented contract and requires an
    explicit "version": 2. It turns two previously reserved slots into carriers:
    data[3] = windowSegment (u64, the recorder's window/Reset ordinal, >= 1) and
    words32[15] = identityProofKind (registered kinds only). An unknown, non-integer or
    structurally inconsistent version is refused with ValueError -- never ignored.
  * Identity is explicit instead of implied: carrying a field is NOT a proof. Every chain
    reports identityBasis + identityProven. Under version 1 the basis is
    'recorderFlagClaimOnly' and identityProven is False: sameObjectCertified keeps the
    legacy flag-based meaning, and the report never claims the instance was proven. Under
    version 2 a chain may only certify when a registered identityProofKind is carried
    AND the carried identity value is non-zero AND identityWeak/epochUnknown are false;
    otherwise it is refused with 'identityNotProven' / 'identityValueMissing'. So the 0 of
    identityWeak alone can never certify a same-object recovery in the versioned format.

Window / Reset segmentation (2026-09-17 上级裁定 ⑧):

  * Clearing the observation table on ResetForSessionTransition is not segmentation. Two
    complete chains that share the eight-tuple key but sit in different windows were
    grouped into ONE chain by this reader: with two settled terminals the whole export was
    refused ("one object may carry at most one terminal event") and, with one open chain,
    the two windows were merged and misjudged as one object with a corrupt stage stream.
  * The eight-tuple cannot distinguish them (that is the finding, not a bug in the key):
    the recorder must therefore carry an **identifiable** segment label on every record,
    and the group key must include it. Under version 2 the label is data[3] (write-once
    per emitted event, so a frozen export's segmentation summary is a pure function of the
    frozen events and cannot change after the freeze).
  * Legacy artifacts carry no label. This reader refuses to guess: it never splits a chain
    on an ordering heuristic (a chainSequence restart is a corruption witness, not a
    window boundary), so a legacy export that really spans a Reset stays fail-closed.
  * A segment label identifies which recorder window a record was emitted in; it is not a
    proof that a device Reset happened, and it is not GPU or pixel evidence.

Frozen carrier (data[12] / words32[48]; every u64 may be split into lo/hi words).
This table is the **implemented** encoding, not a proposal: it is the write set of
EncodePaletteObjectEvent() in src/d3d9/war3/tools/war3_palette_object_evidence_sink.cpp
(the single conversion point the emitter delegates to), and
test_palette_object_evidence_analysis_static.py re-derives the sink's written slot set
from that function and requires it to be exactly {documented slots} | RESERVED_DATA
(resp. RESERVED_BITS), so this reader cannot drift from the C++ conversion point.

  event.kind = 12, event.label = "palette-object/v1"
  event.session          -> key.sessionGeneration (mirrored at words32[10]/[29]; the two
                            encodings must agree, a mismatch is a hard error -- see D5)
  event.frame            -> frames.renderFrame      event.mapEpoch -> key.mapEpoch
  event.deviceEpoch      -> key.deviceEpoch         event.thread   == words32[12]
  data[0]  key.renderablePart         data[1]  key.runtimeModelPtr
  data[2]  key.lifecycleIdentity      data[3..8] reserved (0)
  data[9]  frames.manifestFrameSerial data[10] source (hitKind)   data[11] hitKey
  words32[0] rejectReason   [1] key.jHandle   [2] key.rawcode   [3..4] reserved (0)
  words32[5] stage   [6..9] reserved (0)   [10] sessionGeneration lo32
  words32[11] reserved (0)   [12] recording thread   [13] reserved (0)
  words32[14] bit0 identityWeak (other bits 0)   [15] reserved (0)
  words32[16] bit0 epochUnknown (other bits 0)   [17] reserved (0)
  words32[18] terminalKind   [19] reserved (0)
  words32[20] deltaFrames lo32   [21] nativeFrameTag lo32
  words32[22] hitCount   [23] chainSequence (1,2,3,... inside one chain; strictly
              increasing and unique -- see chain-sequence strictness above)
  words32[24..25] recordFrameSerial (lo, hi)   [26..27] manifestPublishRevision (lo, hi)
  words32[28] flags2: bit0 sawSubmit, bit1 sawDraw, bit2 selectionClearedByNativeOverride,
              bit3 manifestUnknown, bit4 nativeUnknown (bits 5..31 must be 0)
  words32[29] sessionGeneration hi32   [30..31] firstRejectFrame (lo, hi)
  words32[32] nativeFrameTag hi32      [33] deltaFrames hi32
  words32[34..47] reserved (0)

key.lifecycleIdentity -- carried at data[2] (slot added later at the 上级's request)

  History: the C++ key has always been the eight-tuple
  (war3_palette_object_evidence.h:27-38: renderablePart, runtimeModelPtr, jHandle,
  rawcode, sessionGeneration, mapEpoch, deviceEpoch, **lifecycleIdentity** at line 35)
  and SameKey/HashKey compare/hash all eight fields
  (war3_palette_object_evidence.h:438-455), but the frozen wire originally reserved
  data[2..8] and carried no lifecycle identity at all. After the 2026-09-17 上级 03:58
  requirement that the identity must be carried through the wire, the C++ sink writes
  key.lifecycleIdentity into data[2] and this reader declares that slot.

    * LIFECYCLE_IDENTITY_SLOT = ('data', 2): key['lifecycleIdentity'] is decoded,
      chains report lifecycleIdentityCarried = true, and two instances that differ
      only in lifecycleIdentity split into two chains (data[2]).
    * A non-zero data[2] while the slot is undeclared (the historical wire) is still a
      hard error, so the reserved carrier cannot be used to fake an identity.
    * With the slot declared, that index is decoded and no longer reserved-zero;
      data[3..8] remain reserved-zero (RESERVED_DATA).
    * REQUIRE_WIRE_LIFECYCLE_IDENTITY can still promote "slot not carried" itself into
      a certification refusal (default False: identityWeak already carries that
      meaning for every current capture point, see
      war3_palette_object_capture.h:148-166, which always writes
      lifecycleIdentity = 0 + identityWeak = true).

Header block read by this reader (session-scope accounting, nothing else):
  "paletteObject": {"watchCount": int, "counters": {18 canonical u64 decimal strings}}
  LOSS_FIELDS (all of them, including droppedTerminalReserve and droppedPayloadConflict)
  force "not covered": a non-zero droppedPayloadConflict means a same-key/same-frame/same-stage
  event carried a **different payload** and was therefore not recorded as a proven-equivalent
  duplicate, so the export cannot prove which fact is missing.
  The blocked identities are the recorder's own accounting (CloseWindow F2 at
  war3_palette_object_evidence.h:202-209, NoteObjectGone F4 at :420-422 and the
  TableFull F4 at :245-247 increment exactly one settled bucket per terminal that was
  really emitted), so a header block that violates them cannot prove which events the
  export is missing -- it is a "not covered" verdict, never a silent pass.

This reader does not modify test_recorder_event_wire_golden.py and does not certify
GPU work, pixels, or a graphics root cause.
"""
import argparse
import json
from pathlib import Path

import analyze_frame_evidence as frame_evidence
from analyze_frame_evidence import load, require, u64, uint

LABEL='palette-object/v1'
KIND=12
EVENT_FIELDS={'sequence','session','parent','qpc','thread','kind','owner','frame',
              'mapEpoch','deviceEpoch','label','data','words32'}
# The palette export is exactly the schema-7 frame-evidence root plus the palette
# header block. The previous "set(d) >= ENVELOPE_FIELDS" shape silently accepted a
# shrunken synthetic envelope that could not prove any root-level invariant.
ENVELOPE_FIELDS=(frozenset(frame_evidence.ROOT_FIELDS)
                 |{'reserved','effectiveConfiguration','paletteObject'})
COUNTER_FIELDS={'emitted','terminalEmitted','droppedPerFrame','droppedPerSession',
                'droppedTableFull','droppedProbeLimit','droppedDuplicatePerFrame',
                'droppedPayloadConflict','droppedTerminalReserve','ringEvictedAfterRecord',
                'closedRecovered','closedWindowExpired',
                'closedObjectGone','closedTableFull','closedEventLost','closedUnclosed',
                'weakIdentityRecords','epochUnknownRecords'}
# 2026-09-18 批次 3 更正（**该规则已被阶段 C 取代，见下方 OBSERVATION_CLOSED_FIELDS 同段风格**）：
# 当时写方**只在版本 3**（firstSightUsed()）输出这两个首见计数器，因此 counters 校验按版本：
# v1/v2 是精确集合、v3 才允许追加。**阶段 C 后写方恒定 v4** ⇒ v4 的 counters **恒含**这两个键；
# v1/v2/v3 的产物不会产生它们，仍必须是精确集合（不得追加）。
FIRST_SIGHT_COUNTER_FIELDS=frozenset({'firstSightInserted','firstSightEmitted'})
# 2026-09-18 阶段 C：v4 才登记的**新结算桶**。不得把它并进全局集合 ——
# v1/v2/v3 产物不会产生 ObservationClosed，也不该携带该计数器。
OBSERVATION_CLOSED_FIELDS=frozenset({'closedObservationClosed'})
# Missing-chain fields: any non-zero value means events may have been lost, so no chain
# in this export can be certified. droppedTerminalReserve (the 512 terminal reserve ran
# out and a terminal could not be written) is part of the verdict, not a side note.
# 2026-09-17 D6（上级裁定）：droppedPayloadConflict 是**同键同帧同阶段但载荷冲突**的计数
# （war3_palette_object_evidence.h 的四处同帧去重都先比较载荷；来源/hitKey/清空标志/拒绝原因
# 不同时不再压缩成"已证明等价的重复"）。它意味着这条链的第二次事实**没有**被记录 ——
# 与其它 dropped* 同级：非零 ⇒ 缺链/未覆盖（fail-visible），绝不允许静默通过。
LOSS_FIELDS=('droppedPerFrame','droppedPerSession','droppedTableFull','droppedProbeLimit',
             'droppedPayloadConflict','ringEvictedAfterRecord','droppedTerminalReserve')
RESERVED_DATA=(3,4,5,6,7,8)
# 2026-09-18 阶段 C：v4 把 data[4] 从**保留零**改为**链型**载体（0=RejectionRecovery，1=Observation）。
# 版本相关性必须显式登记：v1/v2/v3 的 data[4] 仍是保留零（旧产物合同一位不放宽）。
CHAIN_TYPE_SLOT=('data',4)
CHAIN_TYPES={0:'RejectionRecovery',1:'Observation'}
RESERVED_BITS=(3,4,6,7,8,9,11,13,15,17,19,34,35,36,37,38,39,40,41,42,43,44,45,46,47)

# ---- 显式格式版本 + 版本 2 的分段/身份证明载体（2026-09-17 上级裁定 ⑦⑧）-------------
# 版本登记表只有一份，在 analyze_frame_evidence（通用读方 = 根格式的单一来源，也是
# history/watcher 唯一引入的读方）。这里只引用，不复制常量。
PALETTE_OBJECT_LEGACY_VERSION=frame_evidence.PALETTE_OBJECT_LEGACY_VERSION
PALETTE_OBJECT_SEGMENTED_VERSION=frame_evidence.PALETTE_OBJECT_SEGMENTED_VERSION
PALETTE_OBJECT_VERSION_FIELD=frame_evidence.PALETTE_OBJECT_VERSION_FIELD
# 版本 2 才启用的两个载体槽位；版本 1（含所有旧实机产物）下它们仍是必须为 0 的保留槽。
WINDOW_SEGMENT_FIELD='windowSegment'
WINDOW_SEGMENT_SLOT=('data',3)
IDENTITY_PROOF_FIELD='identityProofKind'
IDENTITY_PROOF_SLOT=('words32',15)
IDENTITY_PROOF_KINDS={0:'NoIdentityProof',1:'InstanceLifecycleIdentityProof'}
IDENTITY_BASIS_FLAGS_ONLY='recorderFlagClaimOnly'
IDENTITY_BASIS_WIRE_PROOF='wireCarriedInstanceLifecycleProof'
REFUSAL_IDENTITY_NOT_PROVEN='identityNotProven'
REFUSAL_IDENTITY_VALUE_MISSING='identityValueMissing'
# ---- 2026-09-17 只读对抗审计 D3：头块恒等式（记录器自己的记账，不是启发式）---------
# 记录器每**真的发出**一条终态就恰好递增一个结算桶（war3_palette_object_evidence.h
# CloseWindow F2 / NoteObjectGone F4 / TableFull F4），所以这三个恒等式必须成立：
#   terminalEmitted == 导出终态条数；sum(closed*) == terminalEmitted；
#   每个 closed* == 对应终态种类的导出条数。
# 任一不符都说明导出与记账脱节 —— 该导出无法证明少了哪些事件，任何链都不得认证。
# watchCount 是 kWatchCapacity(1024) 定长表的读数，超过上界是结构上不可能的。
K_WATCH_CAPACITY=1024
# 2026-09-18 阶段 C：`closedObservationClosed` **不并入**本元组 —— 本元组会被
# 无版本区分地用于一致性取值，而 v1/v2/v3 的 counters 合法地不含该键（会 KeyError）。
# v4 是否携带该计数器由 `OBSERVATION_CLOSED_FIELDS` 在**版本维**上校验。
CLOSED_FIELDS=('closedRecovered','closedWindowExpired','closedObjectGone','closedTableFull',
               'closedEventLost','closedUnclosed','closedObservationClosed')
TERMINAL_CLOSED_FIELD={'Recovered':'closedRecovered','WindowExpired':'closedWindowExpired',
                       'ObjectGone':'closedObjectGone','TableFull':'closedTableFull',
                       'EventLost':'closedEventLost','Unclosed':'closedUnclosed',
                       'ObservationClosed':'closedObservationClosed'}
# 未知帧域**不是通配符**：生产填充 MakePaletteObjectFrames()（war3_palette_object_capture.h
# :176-188）在域未知时写 0，所以"标记未知却带着值"是自相矛盾，必须拒绝认证（D4）。
UNKNOWN_DOMAIN_FIELDS=(('manifest',('manifestFrameSerial','manifestPublishRevision')),
                       ('native',('nativeFrameTag',)))

# ---- key.lifecycleIdentity: eighth C++ key field, carried at data[2] ---------------
LIFECYCLE_IDENTITY_FIELD='lifecycleIdentity'
LIFECYCLE_IDENTITY_ABSENT='<not-carried>'
CHAIN_IDENTITY_FIELDS=('renderablePart','runtimeModelPtr','jHandle','rawcode',
                       'sessionGeneration','mapEpoch','deviceEpoch',
                       LIFECYCLE_IDENTITY_FIELD)
# ---- 2026-09-17 上级 03:58 要求"必须贯通"后落地 --------------------------------------
# 发射器（war3_palette_object_evidence_sink.cpp）现在写 data[2] = key.lifecycleIdentity；
# 因此读方声明该槽位（原先的 None 只适用于"C++ 尚未补槽位"的过渡状态）。
# data[3..8] 仍是必须为 0 的保留位（RESERVED_DATA）。
LIFECYCLE_IDENTITY_SLOT=('data',2)
PROPOSED_LIFECYCLE_IDENTITY_SLOT=('data',2)
ALTERNATE_LIFECYCLE_IDENTITY_SLOT=('words32',34,35)
WIRE_GAP_LIFECYCLE_IDENTITY='lifecycleIdentityNotCarriedRequiresCppSlot'
# Optional one-line tightening: True makes "the wire does not carry lifecycleIdentity"
# itself a same-object certification refusal.
REQUIRE_WIRE_LIFECYCLE_IDENTITY=False

# 2026-09-18 独立复审批次 3：版本 3 = **正常观察链**（新增阶段 FirstSight=5）。
# 语义：该对象作为 CPU caster 候选**首次进入观察**；它**不是**拒绝，也**不是**「已恢复」。
# FirstSight 故意取 5：版本 1/2 的形状里它不可解释，读方必须**拒绝**而不是静默忽略。
PALETTE_OBJECT_FIRST_SIGHT_VERSION=3
STAGES={1:'Rejected',2:'ServedCandidate',3:'Enqueued',4:'Drawn',5:'FirstSight'}
# 2026-09-18 对抗性审计 P0：上面的注释说「版本 1/2 里 FirstSight 不可解释、读方必须拒绝」，
# 但 STAGES 是**版本无关**的，于是 v1/v2 载荷出现 stage=5 时只是被软标记而不是**整份拒绝**。
# 这里补回版本门控：v1/v2 只认识 1..4 ⇒ lookup() 的 require 会让整份导出以 ValueError 被拒。
LEGACY_STAGES={1:'Rejected',2:'ServedCandidate',3:'Enqueued',4:'Drawn'}
SOURCES={0:'NoSource',1:'ArenaSlot',2:'ProducerSnapshot',3:'PoseKernel',4:'DrawTimeCaptured',
         5:'PublishedRegistry',6:'OwnedPartSnapshot',7:'Unknown'}
# C++ PaletteObjectTerminal: None=0, Recovered=1, WindowExpired=2, ObjectGone=3,
# TableFull=4, EventLost=5, Unclosed=6 (frozen by the implemented recorder header).
TERMINALS={0:'NoTerminal',1:'Recovered',2:'WindowExpired',3:'ObjectGone',4:'TableFull',
           5:'EventLost',6:'Unclosed'}
# 2026-09-18 阶段 C：终态必须**按版本**校验。
# 裁定明确要求「不得只把 7 加进全局 TERMINALS」—— 若那样做，v1/v2/v3 产物会开始**接受**
# 一个它们从未产生过的终态，读方就丢掉了「这个版本不该出现这个值」这一判据。
# v4 引入 ObservationClosed=7：Observation 链「观察已结算」，**不表示**阶段齐全、
# 不表示身份已证明、不表示 palette 已消费、更不表示画面已恢复。
TERMINALS_OBSERVATION_CLOSED=7
TERMINALS_V4=dict(TERMINALS);TERMINALS_V4[TERMINALS_OBSERVATION_CLOSED]='ObservationClosed'
TERMINALS_BY_VERSION={1:TERMINALS,2:TERMINALS,3:TERMINALS,4:TERMINALS_V4}
PALETTE_OBJECT_CHAIN_TYPED_VERSION=4
REASONS={0:'R0',1:'R1',2:'R2',3:'R3',0xFE:'NotChecked',0xFF:'Unknown'}

def lookup(table,value,what):
    require(value in table,'unknown %s %d'%(what,value));return table[value]

def join64(lo,hi):
    return (uint(hi,2**32-1)<<32)|uint(lo,2**32-1)

def lifecycle_identity_slot():
    """Return the declared ('data',index) / ('words32',lo,hi) slot, or None."""
    slot=LIFECYCLE_IDENTITY_SLOT
    if slot is None:return None
    require(type(slot) is tuple and slot and slot[0] in ('data','words32'),
            'invalid lifecycleIdentity slot declaration')
    if slot[0]=='data':
        require(len(slot)==2 and type(slot[1]) is int and 0<=slot[1]<12,
                'invalid lifecycleIdentity data slot')
    else:
        require(len(slot)==3 and all(type(index) is int and 0<=index<48 for index in slot[1:]),
                'invalid lifecycleIdentity words32 slot')
    return slot

def lifecycle_identity_carried():
    return lifecycle_identity_slot() is not None

def reserved_data_indices(version=PALETTE_OBJECT_LEGACY_VERSION):
    """该**显式格式版本**下必须为 0 的 data 槽。

    版本 1（默认）与旧实机产物完全一致；版本 2 把 data[3] 变成 windowSegment 载体，
    因此它不再出现在保留集合里（其余 data 槽仍必须为 0）。
    """
    indices=RESERVED_DATA
    slot=lifecycle_identity_slot()
    if slot is not None and slot[0]=='data':
        indices=tuple(index for index in indices if index!=slot[1])
    if version>=PALETTE_OBJECT_SEGMENTED_VERSION:
        indices=tuple(index for index in indices if index!=WINDOW_SEGMENT_SLOT[1])
    if version>=PALETTE_OBJECT_CHAIN_TYPED_VERSION:
        # 阶段 C：v4 的 data[4] 是**链型**载体，不再是保留零。
        indices=tuple(index for index in indices if index!=CHAIN_TYPE_SLOT[1])
    return indices

def reserved_bit_indices(version=PALETTE_OBJECT_LEGACY_VERSION):
    """该**显式格式版本**下必须为 0 的 words32 槽（版本 2 启用 words32[15]）。"""
    indices=RESERVED_BITS
    slot=lifecycle_identity_slot()
    if slot is not None and slot[0]=='words32':
        indices=tuple(index for index in indices if index not in slot[1:])
    if version>=PALETTE_OBJECT_SEGMENTED_VERSION:
        indices=tuple(index for index in indices if index!=IDENTITY_PROOF_SLOT[1])
    return indices

def decode_window_segment(data,version):
    """版本 2 的窗口/Reset 分段标签；版本 1 不携带（返回 None）。

    data 已由调用点逐槽 u64() 校验为 int，这里直接取值（不得二次 u64()）。
    """
    if version<PALETTE_OBJECT_SEGMENTED_VERSION:
        return None
    return data[WINDOW_SEGMENT_SLOT[1]]

def decode_identity_proof_kind(bits,version):
    """版本 2 载明的身份证明种类；版本 1 不携带（返回 0 = NoIdentityProof）。"""
    if version<PALETTE_OBJECT_SEGMENTED_VERSION:
        return 0
    return uint(bits[IDENTITY_PROOF_SLOT[1]],2**32-1)

def decode_lifecycle_identity(data,bits):
    slot=lifecycle_identity_slot()
    if slot is None:return None
    if slot[0]=='data':return str(data[slot[1]])
    return str(join64(bits[slot[1]],bits[slot[2]]))

def unknown_frame_domain_conflicts(event):
    """Frame domains flagged unknown that still carry a non-zero value (D4).

    The production filler writes 0 whenever a domain is unknown, so "unknown" must not
    smuggle a value: a flagged-unknown domain with a non-zero payload is self
    contradictory and may not certify a same-object recovery.
    """
    frames=event['frames']
    conflicts=[]
    for domain,fields in UNKNOWN_DOMAIN_FIELDS:
        if not frames[domain+'Unknown']:continue
        if any(int(frames[field])!=0 for field in fields):
            conflicts.append(domain)
    return conflicts

def decode_event(e,version=PALETTE_OBJECT_LEGACY_VERSION):
    """按**显式格式版本**解码一条 palette 事件（版本 1 = 旧合同，默认值保持旧行为）。"""
    require(type(e) is dict and set(e)==EVENT_FIELDS,'palette event fields mismatch')
    require(e['kind']==KIND,'palette event kind must be Kind::ShadowState(12)')
    require(e['label']==LABEL,'palette event label mismatch')
    require(type(e['data']) is list and len(e['data'])==12,'palette event data length')
    data=[u64(v) for v in e['data']]
    require(type(e['words32']) is list and len(e['words32'])==48,'palette event words32 length')
    bits=[uint(v,2**32-1) for v in e['words32']]
    for index in reserved_data_indices(version):
        require(data[index]==0,'palette carrier data[%d] is reserved and must be zero'%index)
    for index in reserved_bit_indices(version):
        require(bits[index]==0,'palette carrier words32[%d] is reserved and must be zero'%index)
    window_segment=decode_window_segment(data,version)
    if window_segment is not None:
        require(window_segment>=1,
                'palette window segment must be >= 1 (got %d)'%window_segment)
    identity_proof_kind=decode_identity_proof_kind(bits,version)
    if version>=PALETTE_OBJECT_SEGMENTED_VERSION:
        lookup(IDENTITY_PROOF_KINDS,identity_proof_kind,'palette identity proof kind')
    require(bits[14]&~1==0 and bits[16]&~1==0,'palette flag word has undefined bits set')
    require(bits[28]&~0x1F==0,'palette flag2 word has undefined bits set')
    require(bits[12]==uint(e['thread'],2**32-1),'palette recording thread witness mismatch')
    session_generation=join64(bits[10],bits[29])
    # 2026-09-17 只读对抗审计 D5：sink 把 key.sessionGeneration 镜像到 words32[10]/[29]，
    # 并与 event.session 同源。两者不一致说明载体不是读方假定的那份冻结 wire，
    # 因此是硬错误（与 thread 见证同级的结构校验），绝不允许静默通过。
    require(session_generation==u64(e['session']),
            'palette session generation mirror mismatch: words32[10]/[29]=%d event.session=%s'
            %(session_generation,e['session']))
    return {
        'sequence':u64(e['sequence']),'session':u64(e['session']),'thread':uint(e['thread'],2**32-1),
        # 按版本选表：v1/v2 不认识 stage=5（见 LEGACY_STAGES 上方注释）。
        'stage':lookup(
            STAGES if version>=PALETTE_OBJECT_FIRST_SIGHT_VERSION else LEGACY_STAGES,
            bits[5],'palette stage'),
        'source':lookup(SOURCES,data[10],'palette source'),
        # 阶段 C：链型。v4 从 data[4] 解码（未知值 ⇒ 整份拒绝）；
        # v1/v2/v3 **没有**链型载体 —— 那些版本里只存在一类链（拒绝恢复），这不是猜测而是
        # 它们的合同：data[4] 在那三个版本上被校验为保留零（见 reserved_data_indices）。
        'chainType':(lookup(CHAIN_TYPES,data[4],'palette chain type')
                     if version>=PALETTE_OBJECT_CHAIN_TYPED_VERSION else 'RejectionRecovery'),
        # 按版本选终态表（v1/v2/v3 不认识 7 ⇒ 遇到即整份拒绝；v4 才认识 ObservationClosed）。
        'terminal':lookup(TERMINALS_BY_VERSION.get(version,TERMINALS),bits[18],'palette terminal'),
        'rejectReason':lookup(REASONS,bits[0],'palette reject reason'),
        'key':{'renderablePart':str(data[0]),'runtimeModelPtr':str(data[1]),
               'jHandle':str(bits[1]),'rawcode':str(bits[2]),
               'sessionGeneration':str(session_generation),
               'mapEpoch':e['mapEpoch'],'deviceEpoch':e['deviceEpoch'],
               LIFECYCLE_IDENTITY_FIELD:decode_lifecycle_identity(data,bits)},
        'frames':{'renderFrame':e['frame'],'manifestFrameSerial':str(data[9]),
                  'manifestPublishRevision':str(join64(bits[26],bits[27])),
                  'recordFrameSerial':str(join64(bits[24],bits[25])),
                  'nativeFrameTag':str(join64(bits[21],bits[32])),
                  'manifestUnknown':bool(bits[28]&8),'nativeUnknown':bool(bits[28]&16)},
        'hitKey':str(data[11]),'hitCount':bits[22],'chainSequence':bits[23],
        'firstRejectFrame':str(join64(bits[30],bits[31])),
        'deltaFrames':str(join64(bits[20],bits[33])),
        'sawSubmit':bool(bits[28]&1),'sawDraw':bool(bits[28]&2),
        'selectionClearedByNativeOverride':bool(bits[28]&4),
        'identityWeak':bool(bits[14]&1),'epochUnknown':bool(bits[16]&1),
        WINDOW_SEGMENT_FIELD:window_segment,
        IDENTITY_PROOF_FIELD:identity_proof_kind,
        IDENTITY_PROOF_FIELD+'Name':lookup(IDENTITY_PROOF_KINDS,identity_proof_kind,
                                           'palette identity proof kind'),
    }

def chain_identity(event):
    """Eight-tuple grouping key: dropping lifecycleIdentity would merge two instances."""
    key=event['key']
    return tuple(LIFECYCLE_IDENTITY_ABSENT if key[field] is None else key[field]
                 for field in CHAIN_IDENTITY_FIELDS)

def chain_group_identity(event):
    """**实际的离线分组键** = 八元组 + 分段标签。

    2026-09-17 上级裁定 ⑧：同会话、同地图、deviceEpoch 未知时，Reset 前后的两条记录可以
    八元组完全相同（清表 ≠ 分段）。只按八元组分组会把它们合成一条链（两个终态 ⇒ 整份导出
    被拒；一个终态 ⇒ 误判成"一条链自身损坏"）。因此版本 2 的分段标签必须进入分组键；
    版本 1 没有标签（None），分组与旧合同逐字节一致 —— 绝不按顺序启发式猜分段。
    """
    # 2026-09-18 阶段 C（Q2）：链型必须进入分组键。同一对象现在可以**同时**持有观察链与
    # 拒绝恢复链两条独立条目；只按八元组 + 分段分组会把它们并成一条链（两个终态 ⇒ 整份
    # 导出被拒，或一个终态 ⇒ 误判成「链自身损坏」）。
    # 位置：插在**分段标签之前**，以保证调用方的 `item[0][-1]` 仍指分段标签（排序语义不变）。
    # v1/v2/v3 的 chainType 恒为 RejectionRecovery ⇒ 旧合同的分组逐字节不变。
    return chain_identity(event)+(event["chainType"],event[WINDOW_SEGMENT_FIELD])


def stage_index(events,stage,live_only=False):
    for index,event in enumerate(events):
        if event['stage']!=stage:continue
        if live_only and event['terminal']!='NoTerminal':continue
        return index
    return None

def judge_chain(key,events,lossy,lifecycle_carried,
                version=PALETTE_OBJECT_LEGACY_VERSION):
    order=[event['sequence'] for event in events]
    issues=[];truncation=[]
    stages=sorted({event['stage'] for event in events})
    terminal_events=[event for event in events if event['terminal']!='NoTerminal']
    require(len(terminal_events)<=1,'one object may carry at most one terminal event')
    terminal=terminal_events[0]['terminal'] if terminal_events else 'NoTerminal'
    # 2026-09-18 阶段 C（Q2 裁定）：**Observation 永不使用 Recovered**。
    # Recovered 的含义是「拒绝之后被接住」，它**必须**有真实拒绝事实。
    # 观察链没有拒绝事实 ⇒ 它出现 Recovered 就是**链型与终态不符** ⇒ 具名判错，
    # 不得静默接受（也不得把它当作一条"普通链"绕过）。
    # v1/v2/v3 没有链型载体（解码处恒给 RejectionRecovery）⇒ 本规则天然不触及旧产物。
    if terminal=="Recovered" and any(event.get("chainType")=="Observation" for event in events):
        issues.append("observationChainMustNotUseRecovered")
    # 2026-09-18 阶段 C（Q2 裁定）：**镜像规则** —— 拒绝恢复链不得使用 ObservationClosed。
    #
    # 与上一条合起来把「终态 × 链型」矩阵封口：
    #   · Observation        + Recovered         ⇒ 拒绝（上一条）
    #   · RejectionRecovery  + ObservationClosed ⇒ 拒绝（本条）
    # ObservationClosed 的含义是「**观察**已结算」；拒绝恢复链没有观察语义，
    # 出现它说明链型与终态不符（伪造或串链）。
    #
    # ⚠️ 按版本门控：v1/v2/v3 的终态表**不认识** 7（解码处即拒绝），本规则对它们无意义；
    # 显式门控是为了避免"版本无关用法"（本项目已撞过三次的缺陷类）。
    if (version>=PALETTE_OBJECT_CHAIN_TYPED_VERSION
            and terminal=="ObservationClosed"
            and any(event.get("chainType")=="RejectionRecovery" for event in events)):
        issues.append("rejectionRecoveryChainMustNotUseObservationClosed")
    # 2026-09-18 阶段 C（Q2 裁定）：观察链**不得**出现 Rejected 阶段。
    # 观察链由 NoteFirstSight 建立，其条目**没有**拒绝事实（两条链各自独立条目）。
    # 出现 Rejected 说明**链型与阶段形状不符**（伪造、或读方把两条链串成一条）
    # ⇒ 具名判错，不得当作一条"带拒绝的观察链"接受。
    # 与上一条的关系：Recovered 拦的是**终态**，本条拦的是**阶段形状**；两者互补。
    if (any(event.get("chainType")=="Observation" for event in events)
            and any(event["stage"]=="Rejected" for event in events)):
        issues.append("observationChainMustNotCarryRejectedStage")
    # 2026-09-18 P0-4（写方/读方同步）：**观察链不得以 WindowExpired 结算**。
    # 裁定：「仅有链首后关闭，也应使用 ObservationClosed，同时诚实报告没有完整绘制证据」。
    # WindowExpired 的含义是「真的发生过拒绝、窗口内从未被接住、也没有服务事实」——
    # 那是**拒绝恢复链**的语义。观察链出现它，说明写方的结算顺序退回了旧的错误形状
    #（把 hitCount==0 && !sawServed 排在观察链之前）；此前读方**对此毫无判据**，
    # 于是「旧标签回来」在导出层面完全不可见（独立复审 C 项实测）。
    # 与上一条互补：上一条拦「观察链冒充已恢复」，本条拦「观察链冒充拒绝链的窗口过期」。
    # 按版本门控（v1/v2/v3 无链型载体，解码恒给 RejectionRecovery），避免版本无关用法。
    if (version>=PALETTE_OBJECT_CHAIN_TYPED_VERSION
            and terminal=="WindowExpired"
            and any(event.get("chainType")=="Observation" for event in events)):
        issues.append("observationChainMustNotUseWindowExpired")
    # 2026-09-18 阶段 C（Q2 裁定）：**对称规则** —— 拒绝恢复链不得携带 FirstSight 阶段。
    # 拒绝恢复链由 NoteReject 建立；FirstSight 是**观察链**的链首。一条链不该同时有两类链首。
    #
    # ⚠️ **必须按版本门控**：v3 **认识** FirstSight 却**没有链型载体**（解码恒给
    # RejectionRecovery）⇒ 不加门控就会把合法的 v3 观察链误判为"拒绝恢复链带 FirstSight"。
    # 这正是本项目反复出现的"版本无关用法"缺陷类，故在此显式登记。
    if (version>=PALETTE_OBJECT_CHAIN_TYPED_VERSION
            and any(event.get("chainType")=="RejectionRecovery" for event in events)
            and any(event["stage"]=="FirstSight" for event in events)):
        issues.append("rejectionRecoveryChainMustNotCarryFirstSightStage")
    i_firstsight=stage_index(events,'FirstSight')
    i_reject=stage_index(events,'Rejected')
    i_served=stage_index(events,'ServedCandidate')
    i_enqueued=stage_index(events,'Enqueued')
    i_drawn=stage_index(events,'Drawn',live_only=True)
# 2026-09-18 批次 3：首见阶段只在版本 3 下可解释；旧版本出现它必须**具名判错**，
# 不得因为 STAGES 里认得这个数字就默认接受（那正是"放宽解析器直到它通过"）。
    if i_firstsight is not None and version<PALETTE_OBJECT_FIRST_SIGHT_VERSION:
        issues.append('firstSightStageUnderLegacyVersion')
    served_count=sum(1 for event in events if event['stage']=='ServedCandidate')
    hit_max=max(event['hitCount'] for event in events)
    # A stage that never happened is not a missing chain; only the recorder's own
    # flags / counters can prove that an event existed and was lost from the export.
    # 2026-09-18 批次 3：版本 3 的**正常观察链**以 FirstSight 为链首，该链**不存在**拒绝
    # 事实，因此"缺 Rejected"在此时不是截断。版本 1/2 无 FirstSight 阶段，规则一字不变。
    # 2026-09-18 对抗性审计 P0：豁免必须**按版本**。首见链只在版本 3 下存在；
    # v1/v2 出现 FirstSight 已在解码处被整份拒绝（LEGACY_STAGES），这里是纵深防御。
    if i_reject is None and i_firstsight is None:
        truncation.append('rejectedStageMissingFromStream')
    if i_firstsight is not None and version<PALETTE_OBJECT_FIRST_SIGHT_VERSION:
        raise ValueError('firstSight stage under legacy version %d' % version)
    if i_served is None and hit_max>0:truncation.append('servedCandidateStageMissingFromStream')
    # 两条链互斥：同一对象不得同时出现拒绝链首与首见链首（那是两个不同的事实）。
    if i_reject is not None and i_firstsight is not None:
        issues.append('bothRejectedAndFirstSightHead')
    if (i_firstsight is not None and i_served is not None
            and not i_firstsight<i_served):
        issues.append('servedCandidateBeforeFirstSight')
    if i_reject is not None and i_served is not None and not i_reject<i_served:
        issues.append('servedCandidateBeforeRejected')
    # 正常链 = FirstSight -> Enqueued（**无** ServedCandidate）；拒绝恢复链才需要
    # ServedCandidate 在前。故此处只约束"若两者都在则必须有序"，不要求 Served 必须存在。
    if i_served is not None and i_enqueued is not None and not i_served<i_enqueued:
        issues.append('enqueuedBeforeServedCandidate')
    if i_enqueued is not None and i_drawn is not None and not i_enqueued<i_drawn:
        issues.append('drawnBeforeEnqueued')
    saw_submit=any(event['sawSubmit'] for event in events)
    saw_draw=any(event['sawDraw'] for event in events)
    # 2026-09-17 只读对抗审计 D4：认证谓词不得弱于记录器自身的 Recovered 语义
    # （war3_palette_object_evidence.h:176-179 的 closedChain）。终态记录携带
    # drawSource / selectionClearedByNativeOverride，live Drawn 携带 sawSubmit，
    # 因此这些自相矛盾可以作为具名 refusal 判出，而不是被静默认证。
    terminal_event=terminal_events[0] if terminal_events else None
    draw_events=[event for event in events if event['stage']=='Drawn']
    live_draws=[event for event in draw_events if event['terminal']=='NoTerminal']
    draw_source_missing=(terminal=='Recovered' and terminal_event is not None
                         and terminal_event['source']=='NoSource')
    selection_cleared_at_draw=(terminal=='Recovered' and terminal_event is not None
                               and terminal_event['selectionClearedByNativeOverride'])
    live_drawn_without_submit=any(not event['sawSubmit'] for event in live_draws)
    unknown_frame_domains=sorted({domain for event in events
                                  for domain in unknown_frame_domain_conflicts(event)})
    if saw_draw and i_drawn is None:truncation.append('drawnFlagWithoutDrawnEvent')
    if saw_submit and i_enqueued is None:truncation.append('submitFlagWithoutEnqueuedEvent')
    if hit_max!=served_count:
        truncation.append('servedCandidateEventsTruncated(%d events, %d served)'
                          %(served_count,hit_max))
    # chainSequence is one counter per recorder entry: every emission writes
    # `record.chainSequence = ++entry.chainSequence`
    # (war3_palette_object_evidence.h:228/301/343/383/423/455, and the one-shot TableFull
    # terminal writes 1u at :269), so inside ONE chain the reader must see exactly
    # 1, 2, 3, ... in sequence order. 2026-09-17 上级复验反例：整条链都写 1 曾被认证。
    #   * equal or backwards (当前值 <= 前一个值，即 current < expected) means the wire
    #     duplicated / reordered one entry's events, or collapsed two entries the recorder
    #     kept apart (lifecycleIdentity is the only field that can keep two instances
    #     apart, so a collapse shows up exactly here) -- the group is not one object
    #     instance. Same level as the stage-order issues above, so it blocks
    #     stageObservationComplete and sameObjectCertified.
    #   * a hole (1,2,4 -- including a missing head such as 3,4,5,6,7) means an event the
    #     recorder really emitted for this entry is absent from the export: the stage
    #     stream is truncated (缺口), which forces "not covered" instead of a silent pass.
    # Cross-chain restarts stay legal: every entry owns its own counter, so two different
    # chains independently start at 1; only the values inside one chain are compared.
    expected=1
    for event in events:
        current=event['chainSequence']
        if current<expected:
            issues.append('chainSequenceNotStrictlyIncreasing@%d'%event['sequence'])
            break
        if current!=expected:
            truncation.append('chainSequenceGap@%d(expected %d, saw %d)'
                              %(event['sequence'],expected,current))
            break
        expected+=1
    first_reject=int(events[0]['firstRejectFrame'])
    if any(int(event['firstRejectFrame'])!=first_reject for event in events):
        issues.append('firstRejectFrameChangedInsideChain')
    for event in events:
        frame=int(event['frames']['renderFrame'])
        delta=int(event['deltaFrames'])
        # 2026-09-18 对抗性审计 P1：分支原先漏了 FirstSight。
        # 写方对首见链的契约是 delta = 本帧 − **链首帧**（war3_palette_object_evidence.h:403-407
        # 注释明写「firstRejectFrame 槽位承载的是链首帧，deltaFrames 因而 = 本帧 - 链首帧」）。
        # 但本分支只认 Rejected/ServedCandidate/终态，首见链的 FirstSight 于是落入 elif 被要求
        # delta==0 —— 首次那条恰好为 0 而蒙混过关，**第 2..N 次必然 >0** ⇒ `frozenZeroDeltaViolated`。
        # 注意这里**不是放宽**：仍然是严格的 delta==frame-锚点 等式，只是把首见链的锚点事件纳入。
        if (event['stage'] in ('Rejected','ServedCandidate','FirstSight')
                or event['terminal']!='NoTerminal'):
            if frame>=first_reject and delta!=frame-first_reject:
                issues.append('deltaFramesMismatch@%d'%event['sequence'])
        elif delta!=0:
            issues.append('frozenZeroDeltaViolated@%d'%event['sequence'])
    identity_weak=any(event['identityWeak'] for event in events)
    epoch_unknown=any(event['epochUnknown'] for event in events)
    covered=not lossy and not truncation
    # 2026-09-18 对抗性审计 P1：原先硬编码 len(stages)==4，那是**拒绝恢复链**的形状
    # {Rejected, ServedCandidate, Enqueued, Drawn}。而版本 3 的**正常观察链**按设计**永不发**
    # ServedCandidate（d3d9_device.cpp:23524-23527 明确说发 NoteServed 等于冒充 ServedCandidate），
    # 只有 {FirstSight, Enqueued, Drawn} = 3 个阶段 ⇒ 该判据对正常链**结构性不可达**。
    # 故按链型取要求数：首见链 3、拒绝恢复链 4。这不是放宽 —— 两侧都是完整阶段集。
    # 2026-09-18 P0-6（**写方/读方同步**；C1 之后）：观察链的合法阶段集是
    # {FirstSight} ∪ {可选 ServedCandidate} ∪ {Enqueued, Drawn} —— Astra 裁定 C1 要求 S/E/D 对
    # **两条链各自**记一份，因此「该对象既被观察又被服务」时观察链**合法地**携带 4 个阶段。
    # 旧判据硬编码「首见链恰好 3 个」⇒ 会把**合法**的双链导出判成 stageObservationComplete=False。
    # 新判据按**集合**：必须含三个承重阶段 {FirstSight, Enqueued, Drawn}，且所有阶段都取自合法四元组
    # （不得出现陌生阶段）。**这不是放宽**：缺 Enqueued/Drawn 仍然失败，陌生阶段仍然失败；
    # 拒绝恢复链仍要求完整的 {Rejected, ServedCandidate, Enqueued, Drawn}。
    if i_firstsight is not None:
        required_stages={'FirstSight','Enqueued','Drawn'}
        legal_stages=required_stages|{'ServedCandidate'}
    else:
        required_stages={'Rejected','ServedCandidate','Enqueued','Drawn'}
        legal_stages=set(required_stages)
    _stages=set(stages)
    stage_observation_complete=(not truncation and not issues
                               and required_stages<=_stages
                               and _stages<=legal_stages
                               and saw_submit and saw_draw and terminal!='NoTerminal')
    refusals=[]
    if identity_weak:refusals.append('identityWeak')
    if epoch_unknown:refusals.append('epochUnknown')
    # 2026-09-17 上级裁定 ⑦：**字段存在 ≠ 身份已证明**。版本 2 要求这条链**载明**登记在案的
    # 身份证明种类，并且携带的身份值非零、两个弱标记为假；只把 identityWeak 写成 0 不足以认证
    # 同对象恢复。版本 1 是旧合同：它只报告 identityBasis='recorderFlagClaimOnly' +
    # identityProven=False（明确写出"这只是记录器自己的声明，不是证明"），判定语义不变。
    proof_kinds=sorted({event[IDENTITY_PROOF_FIELD] for event in events})
    identity_value=key[-1]
    identity_value_carried=(identity_value not in (None,LIFECYCLE_IDENTITY_ABSENT,'0'))
    segmented=(version>=PALETTE_OBJECT_SEGMENTED_VERSION)
    if segmented:
        if any(kind!=1 for kind in proof_kinds):
            refusals.append(REFUSAL_IDENTITY_NOT_PROVEN)
        elif not identity_value_carried:
            refusals.append(REFUSAL_IDENTITY_VALUE_MISSING)
    identity_proven=bool(segmented and proof_kinds==[1] and identity_value_carried and
                         not identity_weak and not epoch_unknown)
    identity_basis=(IDENTITY_BASIS_WIRE_PROOF if segmented else IDENTITY_BASIS_FLAGS_ONLY)
    if REQUIRE_WIRE_LIFECYCLE_IDENTITY and not lifecycle_carried:
        refusals.append('lifecycleIdentityNotCarried')
    if draw_source_missing:refusals.append('drawSourceMissing')
    if selection_cleared_at_draw:refusals.append('selectionClearedAtDraw')
    if live_drawn_without_submit:refusals.append('liveDrawnWithoutSubmit')
    if unknown_frame_domains:refusals.append('unknownFrameDomainWithValue')
    # 2026-09-18 P0-6（复审 1266d8db 的独立发现）：**读方的 Recovered 谓词弱于写方**。
    # 写方（war3_palette_object_evidence.h:309-311）的 closedChain 要求：
    #     hasRejectFact && sawServed && sawSubmit && sawDraw && !orderViolation &&
    #     !drawSelectionCleared && drawSource != None
    # 其中 hasRejectFact = (firstReason != NotChecked) —— 语义是「Recovered = **拒绝之后**被接住」，
    # 因此**必须有真实拒绝事实**。读方此前解码了 rejectReason（:456）却**从不使用** ⇒ 一条把拒绝理由
    # 伪造成 NotChecked/Unknown 的链仍会被认证为 Recovered，且出口码为 0（复审实测）。
    # ⇒ 补一条具名 refusal，把两侧谓词对齐。这与 drawSourceMissing / selectionClearedAtDraw 同级，
    # 2026-09-18 P0-6（复审 0224f6c8 的反例 1）：判据必须用**链首**事件的理由，不能扫全链。
    # 写方 `hasRejectFact = (e.firstReason != NotChecked)`（evidence.h:318）用**条目**的 firstReason；
    # 而 `NoteReject` 在发事件时用**本次调用的 reason** 覆盖该事件的 rejectReason（evidence.h:462）——
    # 所以一条 firstReason=R1 的合法链，其**后续**事件可以携带 NotChecked（同键再调 NoteReject(NotChecked)），
    # 「任何事件带 NotChecked 就拒绝」会**误拒写方自己认证过**的 Recovered 链（复审实测）。
    # 链首事件由建立该链的那次 NoteReject 发出 ⇒ 它的 rejectReason 就是 firstReason，正是写方谓词的镜像；
    # 同时仍能拦住伪造链（链首 NotChecked + 终态塞 R1 —— 写方不可能产生该形状）。
    if terminal=='Recovered' and events and events[0]['rejectReason']=='NotChecked':
        refusals.append('recoveredWithoutRejectFact')
        refusals.append('recoveredWithoutRejectFact')
    if not covered:refusals.append('chainNotCovered')
    if truncation:refusals.append('stageStreamTruncated')
    if issues or not saw_submit or not saw_draw or terminal!='Recovered':
        refusals.append('recoveryNotProven')
    same_object_certified=bool(stage_observation_complete and terminal=='Recovered'
                               and not refusals)
    if terminal=='NoTerminal':
        conclusion='Unclosed';issues.append('terminalMissing')
    elif same_object_certified:
        conclusion='Recovered'
    elif terminal=='Recovered' and stage_observation_complete:
        # 阶段观察完整，但同对象恢复链未认证（弱身份 / 未知代际 / 根级缺口）。
        conclusion='StageCompleteUncertified';issues.append('sameObjectRecoveryNotCertified')
    elif terminal=='Recovered':
        conclusion='Unclosed';issues.append('terminalRecoveredNotCertified')
        if not saw_submit:issues.append('recoveredWithoutSubmitEvidence')
        if not saw_draw:issues.append('recoveredWithoutDrawEvidence')
    else:
        conclusion=terminal
    if not covered:conclusion='Uncovered'
    return {'key':key,'windowSegment':events[0][WINDOW_SEGMENT_FIELD],
            'identityBasis':identity_basis,'identityProven':identity_proven,
            'identityProofKinds':proof_kinds,
            'sequenceSpan':[order[0],order[-1]],'eventCount':len(events),
            'stages':stages,'stageOrder':[event['stage'] for event in events],
            'terminal':terminal,'servedCandidates':served_count,'hitCountMax':hit_max,
            'sawSubmit':saw_submit,'sawDraw':saw_draw,
            'terminalSource':None if terminal_event is None else terminal_event['source'],
            'drawSources':[event['source'] for event in draw_events],
            'unknownFrameDomains':unknown_frame_domains,
            'identityWeak':identity_weak,'epochUnknown':epoch_unknown,
            'lifecycleIdentity':(None if key[-1]==LIFECYCLE_IDENTITY_ABSENT else key[-1]),
            'lifecycleIdentityCarried':lifecycle_carried,
            'covered':covered,'conclusion':conclusion,
            'stageObservationComplete':stage_observation_complete,
            'sameObjectCertified':same_object_certified,
            'certificationRefusals':refusals,
            'missingStages':truncation,'orderIssues':issues,
            'chainComplete':covered and same_object_certified}

def analyze(d):
    require(type(d) is dict,'root must be object')
    require(type(d.get('schema')) is int and d['schema']>=7,'palette reader needs schema >= 7')
    require(set(d)==ENVELOPE_FIELDS,'palette envelope fields mismatch: %s'
            %sorted(set(d)^ENVELOPE_FIELDS))
    config=d['effectiveConfiguration']
    require(type(config) is dict and type(config.get('paletteObjectEvidence')) is bool,
            'effectiveConfiguration.paletteObjectEvidence missing')
    # Strict root validation is REUSED **on the whole root**, not re-implemented and no
    # longer projected: analyze_frame_evidence owns the explicit extension registry and now
    # knows the paletteObject block (unknown version / unknown block shape => ValueError),
    # so it enforces session/sequence/producer-loss/retention/payload invariants on the very
    # object this reader reads. The same single entry point is what the history/watcher
    # readers (analyze_frame_history / frame_history_watch) call.
    frame_analysis=frame_evidence.analyze(d)
    version=frame_analysis['extensions'][frame_evidence.PALETTE_OBJECT_EXTENSION]
    palette=d['paletteObject']
    # 版本化块必须与该**显式版本**的登记字段集一致；无 version 字段的块只接受登记在案的
    # 冻结形状（旧的实机产物），否则就是未知形状 ⇒ 显式拒绝。
    _declared=PALETTE_OBJECT_VERSION_FIELD in (palette if type(palette) is dict else {})
    _expected=((frame_evidence.PALETTE_OBJECT_BLOCK_FIELDS[version] if _declared
                else frame_evidence.PALETTE_OBJECT_LEGACY_BLOCK_FIELDS))
    require(type(palette) is dict and set(palette)==_expected,
            'paletteObject block fields mismatch for extension version %d: %s'
            %(version,sorted(set(palette)^_expected) if type(palette) is dict else sorted(_expected)))
    require(type(palette['watchCount']) is int and palette['watchCount']>=0,
            'paletteObject.watchCount must be a non-negative int')
    # D3：kWatchCapacity(1024) 是记录器定长表的上界，超过它的读数在结构上不可能。
    require(palette['watchCount']<=K_WATCH_CAPACITY,
            'paletteObject.watchCount %d exceeds kWatchCapacity(%d)'
            %(palette['watchCount'],K_WATCH_CAPACITY))
    expected_counters=(
        COUNTER_FIELDS|FIRST_SIGHT_COUNTER_FIELDS|OBSERVATION_CLOSED_FIELDS
        if version>=PALETTE_OBJECT_CHAIN_TYPED_VERSION else
        (COUNTER_FIELDS|FIRST_SIGHT_COUNTER_FIELDS
         if version>=PALETTE_OBJECT_FIRST_SIGHT_VERSION else COUNTER_FIELDS))
    require(type(palette['counters']) is dict and set(palette['counters'])==expected_counters,
            'paletteObject counter fields mismatch (version %d)'%version)
    counters={name:u64(palette['counters'][name]) for name in sorted(expected_counters)}
    evicted=u64(d['evicted'])
    require(type(d['events']) is list,'events must be a list')
    events=[decode_event(e,version) for e in d['events'] if e.get('label')==LABEL]
    # 2026-09-17 上级裁定 ⑧：分段标签必须**非递减**（一个分段块不得在更晚的分段之后再现）；
    # 分段来自每条记录自己携带的 data[3]，不是从事件顺序猜出来的，也不是头块状态。
    if version>=PALETTE_OBJECT_SEGMENTED_VERSION:
        segments=[event[WINDOW_SEGMENT_FIELD] for event in events]
        require(all(earlier<=later for earlier,later in zip(segments,segments[1:])),
                'palette window segments must be non-decreasing across the export (got %s)'
                %segments)
    require(not events or config['paletteObjectEvidence'],
            'palette-object events exist while the sub-gate reads disabled')
    # 2026-09-18 对抗性审计：读方原先只要求 v3 块**存在**两个首见计数器，从不与事件比对，
    # 于是 `firstSightInserted=0, firstSightEmitted=10**9` 也会被接受（审计探针 B）。
    # 这里加两条**单向不变量**（对修改前/后的写方都成立，故不是迁就实现）：
    #   ① 有发射必然有插入：一条链首事件必然对应一次「建条目」。
    #   ② 导出里保留的 FirstSight 事件数不可能多于发射次数：丢失只会减少保留量。
    if version>=PALETTE_OBJECT_FIRST_SIGHT_VERSION:
        # 2026-09-18 阶段 C（K1：**我自己在 round 256 引入的缺陷**，复核 Astra 指出）：
        # 终态**回填**会把终态的 stage 写成该条目达到过的最高阶段。一条只走到链首就关窗的
        # **合法**观察链，在 CloseWindow 时会产生
        # `stage=FirstSight, terminal=ObservationClosed/WindowExpired` 的**终态**事件。
        # 原实现把它一并计入 `first_sight_events` ⇒ 合法导出被误判为
        # 'export carries 2 FirstSight events but firstSightEmitted=1'，**整份导出被拒绝**。
        # 我的 98 个测试没有抓到，因为它们从不构造 'FirstSight→CloseWindow' 这个形状。
        # 正确语义：`firstSightEmitted` 计的是**现场（非终态）**FirstSight 事件的发射次数，
        # 因此保留量不变量必须**排除终态回填**；回填另作一致性检查（见下）。
        live_first_sight=sum(1 for event in events
                             if event['stage']=='FirstSight'
                             and event['terminal']=='NoTerminal')
        backfilled_first_sight=sum(1 for event in events
                                   if event['stage']=='FirstSight'
                                   and event['terminal']!='NoTerminal')
        inserted=counters['firstSightInserted']
        emitted=counters['firstSightEmitted']
        require(emitted==0 or inserted>=1,
                'firstSightEmitted=%d while firstSightInserted=0: an emission requires an entry'
                %emitted)
        # ① 现场保留量 ≤ 发射次数（丢失只会减少保留量）。这一条现在是**正确的**单向不变量。
        require(live_first_sight<=emitted,
                'export carries %d live FirstSight events but firstSightEmitted=%d;'
                ' retention can never exceed emission'%(live_first_sight,emitted))
        # ② 终态回填的阶段摘要同样不可能多于发射次数 —— 否则就是在宣告一条**从未发射过**
        #    链首的链。该检查比原先的弱，因此不会误拒合法导出，但仍能杀死
        #    'firstSightEmitted=0 而导出里有 FirstSight 终态' 这类伪造。
        require(backfilled_first_sight<=emitted,
                'export carries %d terminal events backfilled with the FirstSight stage but'
                ' firstSightEmitted=%d; a chain cannot summarise a stage it never emitted'
                %(backfilled_first_sight,emitted))
    carried=lifecycle_identity_carried()
    slot=lifecycle_identity_slot()
    losses={name:counters[name] for name in LOSS_FIELDS if counters[name]}
    exported_mismatch=counters['emitted']!=len(events)
    # D3：头块恒等式。导出终态条数由逐事件解码得出（不是由计数推导），因此两者
    # 必须相等；每个结算桶还必须等于对应终态种类的导出条数（只对总和无从发现的
    # "种类串位"也由此判出）。
    exported_terminals=[event['terminal'] for event in events
                        if event['terminal']!='NoTerminal']
    # 2026-09-18 阶段 C：本求和必须**只对实际存在的**桶取值。
    # `closedObservationClosed` 只在 v4 登记，v1/v2/v3 产物合法地不含它 ——
    # 直接 `counters[name]` 会让所有旧版本导出在此 KeyError（那是版本无关用法的老毛病）。
    closed_sum=sum(counters[name] for name in CLOSED_FIELDS if name in counters if name in counters)
    closed_kind_mismatch={}
    for kind in sorted(set(exported_terminals)):
        # 终态→桶的映射必须是**全表**（含 v4 的 ObservationClosed）；
        # 但取桶值时按存在性判断，避免旧版本产物在此失败。
        field=TERMINAL_CLOSED_FIELD[kind]
        if field not in counters:
            closed_kind_mismatch[kind]='%s absent from a version-%d counter set'%(
                field,version)
            continue
        exported_count=exported_terminals.count(kind)
        if counters[field]!=exported_count:
            closed_kind_mismatch[kind]='%s=%d exportedTerminals=%d'%(
                field,counters[field],exported_count)
    missing=[]
    if not config['paletteObjectEvidence']:missing.append('paletteObjectSubGateDisabled')
    # An empty sample is "not covered": it may never satisfy a vacuous all([]).
    if not events:missing.append('noObjectEvidenceExported')
    if losses:missing.append('objectLevelEvidenceDropped')
    if evicted:missing.append('ringEvictionObserved')
    if exported_mismatch:
        missing.append('emittedCounterVersusExportedEvents(%d recorded, %d exported)'
                       %(counters['emitted'],len(events)))
    if counters['terminalEmitted']!=len(exported_terminals):
        missing.append('terminalEmittedCounterVersusExportedTerminals(%d recorded, %d exported)'
                       %(counters['terminalEmitted'],len(exported_terminals)))
    if closed_sum!=counters['terminalEmitted']:
        missing.append('closedCountersVersusTerminalEmitted(sum=%d, terminalEmitted=%d)'
                       %(closed_sum,counters['terminalEmitted']))
    if closed_kind_mismatch:
        missing.append('terminalKindCountersVersusExportedTerminals(%s)'
                       %'; '.join('%s: %s'%(kind,value)
                                  for kind,value in sorted(closed_kind_mismatch.items())))
    # Root-level gaps reported by the reused strict reader (producer contention,
    # post window not complete, capacity freeze, truncated labels, unmatched spans,
    # ...). Permanent capability non-claims are not gaps.
    frame_gaps=[entry for entry in frame_analysis['missing']
                if entry not in frame_evidence.CAPABILITIES]
    missing.extend(frame_gaps)
    # Any of the loss shapes makes every chain uncoverable: the dropped events
    # cannot be attributed back to one object key.
    lossy=bool(missing) and config['paletteObjectEvidence']
    grouped={}
    for event in events:grouped.setdefault(chain_group_identity(event),[]).append(event)
    chains=[judge_chain(group[0:len(CHAIN_IDENTITY_FIELDS)],
                        sorted(rows,key=lambda row:row['sequence']),lossy,carried,version)
            for group,rows in sorted(grouped.items(),key=lambda item:(item[0][-1] is None,
                                                                     item[0][-1],item[0]))]
    recovered=[chain['key'] for chain in chains
               if chain['conclusion']=='Recovered' and chain['sameObjectCertified']]
    uncertified=[chain['key'] for chain in chains
                 if chain['conclusion']=='StageCompleteUncertified']
    uncovered=[chain['key'] for chain in chains if chain['conclusion']=='Uncovered']
    # A zero-chain export can never be "complete": bool(chains) is explicit, all([])
    # is not allowed to stand in for evidence.
    coverage_complete=(bool(chains) and not missing
                       and not any(not chain['covered'] for chain in chains))
    chain_complete=coverage_complete and all(chain['chainComplete'] for chain in chains)
    stage_observation_complete=(bool(chains) and not missing
                                and all(chain['stageObservationComplete'] for chain in chains))
    return {'schemaValid':True,'label':LABEL,'kind':KIND,
            'formatVersion':version,
            'extensions':frame_analysis['extensions'],
            'schema':frame_analysis.get('schema'),
            'windowSegmentCarried':version>=PALETTE_OBJECT_SEGMENTED_VERSION,
            'windowSegmentSlot':
                (None if version<PALETTE_OBJECT_SEGMENTED_VERSION else list(WINDOW_SEGMENT_SLOT)),
            'windowSegments':(None if version<PALETTE_OBJECT_SEGMENTED_VERSION
                              else sorted({event[WINDOW_SEGMENT_FIELD] for event in events})),
            'identityProofSlot':
                (None if version<PALETTE_OBJECT_SEGMENTED_VERSION else list(IDENTITY_PROOF_SLOT)),
            'registeredIdentityProofKinds':sorted(IDENTITY_PROOF_KINDS),
            'paletteObjectEvidence':config['paletteObjectEvidence'],
            'watchCount':palette['watchCount'],'counters':counters,'losses':losses,
            'watchCapacity':K_WATCH_CAPACITY,
            'terminalEmitted':counters['terminalEmitted'],
            'exportedTerminalCount':len(exported_terminals),
            'closedCounterSum':closed_sum,
            'closedCounters':{name:counters[name] for name in CLOSED_FIELDS if name in counters},
            'eventCount':len(events),'objectCount':len(chains),'objectEvidencePresent':bool(events),
            'emptySample':not events,
            'coverageComplete':coverage_complete,'chainComplete':chain_complete,
            'stageObservationComplete':stage_observation_complete,
            'chainMissing':not coverage_complete,
            'lifecycleIdentityCarried':carried,
            'lifecycleIdentitySlot':None if slot is None else list(slot),
            'proposedLifecycleIdentitySlot':list(PROPOSED_LIFECYCLE_IDENTITY_SLOT),
            'alternateLifecycleIdentitySlot':list(ALTERNATE_LIFECYCLE_IDENTITY_SLOT),
            'wireGaps':[] if carried else [WIRE_GAP_LIFECYCLE_IDENTITY],
            'frameEvidenceMissing':frame_analysis['missing'],
            'frameEvidenceGaps':sorted(set(frame_gaps)),
            'producerLosses':d['producerLosses'],
            'missing':sorted(set(missing)),'chains':chains,'recovered':recovered,
            'uncertified':uncertified,'uncovered':uncovered,
            'note':'CPU boundary records only: a served palette candidate is not a shadow '
                   'recovery, and a Drawn stage is a recorded draw command, not a GPU '
                   'submission, GPU execution or pixel evidence. A chain is only '
                   'certified when the export is fully covered AND the identity is '
                   'strong (identityWeak/epochUnknown refuse same-object certification). '
                   'identityProven is true only when the versioned wire carries a '
                   'registered identity proof: carrying a field (or writing '
                   'identityWeak = 0) is NOT a proof. windowSegment identifies which '
                   'recorder window a record was emitted in; it is not evidence that a '
                   'device Reset happened, and legacy (version 1) artifacts carry no '
                   'segment at all, so a chain that really spans a Reset is refused '
                   'instead of being guessed.'}

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('input',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    result=analyze(load(args.input))
    if args.output:
        with args.output.open('x',encoding='utf-8') as stream:json.dump(result,stream,indent=2)
    print(json.dumps(result,indent=2))
    return 0 if result['chainComplete'] and not result['chainMissing'] else 2

if __name__=='__main__':
    raise SystemExit(main())
