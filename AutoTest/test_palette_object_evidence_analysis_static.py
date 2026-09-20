"""Contracts for the palette-object/v1 object-level evidence reader.

This is a static analysis test: it never launches, focuses or stops Warcraft III and
never touches the AutoTest MCP control plane. It pins the frozen carrier layout, the
chain rebuild, the missing-chain verdicts, the rule that reaching a palette candidate
is NOT a recovery, and the 2026-09-17 counterexamples that used to pass:

  1. sub-gate on with ZERO events must be "not covered" (never a vacuous all([]));
  2. a chain that is entirely identityWeak = true may not certify a same-object
     recovery chain (stage observation may be reported complete, the chain is not);
  3. deviceEpoch = 0 + epochUnknown = true may not either;
  4. an event session that differs from the root session must be refused;
  5. sequence monotonicity/uniqueness, retention accounting and producer-loss
     accounting are enforced by the reused strict frame-evidence reader;
  6. key.lifecycleIdentity (C++ eight-tuple) has no slot in the frozen wire today, so
     the reader reports the wire gap instead of pretending, while the grouping key
     and the slot switch are already in place for the day C++ adds one.
  7. chainSequence is a **per entry** counter (++entry.chainSequence per emitted event,
     the one-shot TableFull terminal writing 1u), so one chain must carry exactly
     1,2,3,... : an all-equal chain (the 上级's 2026-09-17 counterexample, which used to
     certify with recovered=1 / CLI 0), a late duplicate, a rollback and a hole (1,2,4,
     or a missing head) must all be refused, while the legal 1,2,3,4,5 chain and two
     independent chains that each restart at 1 must still certify.

The export fixture is a real schema-7 frame-evidence root plus the paletteObject
block: a shrunken synthetic envelope cannot prove any root-level invariant and is
now rejected outright.
"""

from __future__ import annotations

import contextlib
import io
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
AUTOTEST = ROOT / "AutoTest"
sys.path.insert(0, str(AUTOTEST))

import analyze_frame_evidence as frame_evidence  # noqa: E402
import analyze_palette_object_evidence as analyzer  # noqa: E402


def key(part=1, model=2, jhandle=3, rawcode=4, generation=5, map_epoch=2000,
        device_epoch=3000, lifecycle=None):
    return {"renderablePart": part, "runtimeModelPtr": model, "jHandle": jhandle,
            "rawcode": rawcode, "sessionGeneration": generation, "mapEpoch": map_epoch,
            "deviceEpoch": device_epoch, "lifecycleIdentity": lifecycle}


def frames(render_frame=100, manifest=11, revision=22, record=33, native=44,
           manifest_unknown=True, native_unknown=True):
    return {"renderFrame": render_frame, "manifestFrameSerial": manifest,
            "manifestPublishRevision": revision, "recordFrameSerial": record,
            "nativeFrameTag": native, "manifestUnknown": manifest_unknown,
            "nativeUnknown": native_unknown}


def event(sequence, object_key, frame_domains, stage, source=0, terminal=0, reason=1,
          hit_key=0, hit_count=0, first_reject_frame=0, delta_frames=0, saw_submit=False,
          saw_draw=False, selection_cleared=False, identity_weak=False, epoch_unknown=False,
          session=1, thread=7, parent=0, owner="0", chain_sequence=0,
          window_segment=None, identity_proof=0, chain_type=None):
    data = [0] * 12
    bits = [0] * 48
    data[0] = object_key["renderablePart"]
    data[1] = object_key["runtimeModelPtr"]
    if object_key["lifecycleIdentity"] is not None:
        # Only meaningful once the reader declares a lifecycleIdentity slot.
        data[2] = object_key["lifecycleIdentity"]
    data[9] = frame_domains["manifestFrameSerial"]
    data[10] = source
    data[11] = hit_key
    # 2026-09-18 阶段 C：v4 的**链型**槽。v1/v2/v3 上 data[4] 是保留零 ——
    # 夹具必须能写出非零值，才能证明旧版本**不会**悄悄接受它。
    if chain_type is not None:
        data[4] = chain_type
    bits[0] = reason
    bits[1] = object_key["jHandle"]
    bits[2] = object_key["rawcode"]
    bits[5] = stage
    # D5（2026-09-17 只读对抗审计）：生产 sink 把**同一个**会话代写进 event.session 与
    # words32[10]/[29] 镜像，读方现在硬校验两者一致。夹具因此从同一个 session 值派生两者
    # （不得各写一份），否则夹具自己就违反被钉死的 wire 契约。
    bits[10] = session & 0xFFFFFFFF
    bits[29] = (session >> 32) & 0xFFFFFFFF
    bits[12] = thread
    bits[14] = 1 if identity_weak else 0
    bits[16] = 1 if epoch_unknown else 0
    bits[18] = terminal
    bits[20] = delta_frames & 0xFFFFFFFF
    bits[33] = delta_frames >> 32
    bits[21] = frame_domains["nativeFrameTag"] & 0xFFFFFFFF
    bits[32] = frame_domains["nativeFrameTag"] >> 32
    bits[22] = hit_count
    bits[23] = chain_sequence
    bits[24] = frame_domains["recordFrameSerial"] & 0xFFFFFFFF
    bits[25] = frame_domains["recordFrameSerial"] >> 32
    bits[26] = frame_domains["manifestPublishRevision"] & 0xFFFFFFFF
    bits[27] = frame_domains["manifestPublishRevision"] >> 32
    bits[28] = ((1 if saw_submit else 0) | (2 if saw_draw else 0) |
                (4 if selection_cleared else 0) |
                (8 if frame_domains["manifestUnknown"] else 0) |
                (16 if frame_domains["nativeUnknown"] else 0))
    bits[30] = first_reject_frame & 0xFFFFFFFF
    bits[31] = first_reject_frame >> 32
    # 版本 2 才启用的两个载体（版本 1 下必须为 0，由独立用例钉死）。
    if window_segment is not None:
        data[3] = window_segment
    bits[15] = identity_proof
    return {"sequence": str(sequence), "session": str(session), "parent": str(parent),
            "qpc": str(sequence * 100), "thread": thread, "kind": analyzer.KIND,
            "owner": owner, "frame": str(frame_domains["renderFrame"]),
            "mapEpoch": str(object_key["mapEpoch"]),
            "deviceEpoch": str(object_key["deviceEpoch"]), "label": analyzer.LABEL,
            "data": [str(value) for value in data], "words32": bits}


def envelope(events, enabled=True, evicted="0", watch_count=0, emitted=None,
             counter_overrides=None, session="1", accepted=None, reserved=None,
             producer_losses=None, trigger_sequence="0", reason=1, state=3,
             post_remaining=0, capacity=4096, format_version=None, block_extra=None):
    """A real schema-7 frame-evidence export root plus the paletteObject block.

    accepted == evicted + exported events and producerLosses >= reserved - accepted
    are the recorder's own retention/booking identities (war3_frame_evidence_core.h:
    Ring::append counts one accepted store per retained event and one evicted event per
    overwritten cell), so this helper derives them instead of inventing numbers. A hole
    in the sequence therefore shows up exactly as it does on the wire: as reserved
    tickets that never landed.
    """
    counters = {name: "0" for name in analyzer.COUNTER_FIELDS}
    # 2026-09-18 更正：写方**只在版本 3** 输出首见计数器，因此 v3 块在线上必然带它们。
    # fixture 必须照生产形状补齐，否则"v3"是个不会被生产出来的形状。
    if (isinstance(format_version, int) and not isinstance(format_version, bool)
            and format_version >= analyzer.PALETTE_OBJECT_FIRST_SIGHT_VERSION):
        for _name in sorted(analyzer.FIRST_SIGHT_COUNTER_FIELDS):
            counters[_name] = "0"
    # 2026-09-18 阶段 C：v4 才登记的**独立结算桶**（ObservationClosed），同理必须补齐。
    if (isinstance(format_version, int) and not isinstance(format_version, bool)
            and format_version >= analyzer.PALETTE_OBJECT_CHAIN_TYPED_VERSION):
        for _name in sorted(analyzer.OBSERVATION_CLOSED_FIELDS):
            counters[_name] = "0"
    counters["emitted"] = str(len(events) if emitted is None else emitted)
    # D3（2026-09-17 只读对抗审计）：终态条数与结算桶是记录器自己的记账，推导而不是
    # 编造。override 仍然可以故意破坏恒等式（那些反例正是要判为未覆盖）。
    terminals = []
    # 2026-09-18 阶段 C：终态表必须**按版本**取。用全局表会让 v4 的 ObservationClosed(7)
    # 在解码处直接 KeyError —— 本用例正是在这里发现该版本无关用法的。
    _terminal_table = analyzer.TERMINALS_BY_VERSION.get(format_version,
                                                        analyzer.TERMINALS)
    for row in events:
        bits = row["words32"]
        if bits[18] != 0 and bits[18] in _terminal_table:
            terminals.append(_terminal_table[bits[18]])
        # 该版本**不认识**的终态：夹具**不**计入任何桶，把"这不该出现"的判定
        # 留给被测对象（否则夹具会先于被测对象抛错，用例就失去了特异性）。
    counters["terminalEmitted"] = str(len(terminals))
    for terminal in terminals:
        # ObservationClosed 有**独立**桶，不得并入 closedUnclosed（那是错误的标签）。
        field = analyzer.TERMINAL_CLOSED_FIELD.get(terminal, "closedObservationClosed")
        counters.setdefault(field, "0")
        counters[field] = str(int(counters[field]) + 1)
    for name, value in (counter_overrides or {}).items():
        counters[name] = str(value)
    evicted_value = int(evicted)
    sequences = [int(row["sequence"]) for row in events]
    if accepted is None:
        accepted = evicted_value + len(events)
    if reserved is None:
        reserved = max([accepted] + sequences)
    holes = reserved - accepted
    if producer_losses is None:
        producer_losses = holes
    return {"schema": 7, "state": state, "reason": reason, "session": session,
            "processId": 4321, "processNonce": "987654321", "qpcFrequency": "10000000",
            "accepted": str(accepted), "evicted": str(evicted),
            "triggerSequence": str(trigger_sequence), "producerLosses": str(producer_losses),
            "reserved": str(reserved), "capacity": capacity,
            "postRemaining": post_remaining,
            "effectiveConfiguration": {"frameEvidence": True, "rawInputs": False,
                                       "paletteObjectEvidence": enabled,
                                       "skinPaletteContract": False,
                                       "localRecorderOwner": False},
            "paletteObject": palette_block(watch_count, counters, format_version, block_extra),
            "captureComplete": False, "rootCauseReady": False,
            "capabilities": {"cpuBoundaryEvents": True, "gpuCompletion": False,
                             "pixelEvidence": False, "videoFrameMapping": False,
                             "actualCasterInputs": False, "exactModuleMapIdentity": False},
            "events": events}


def palette_block(watch_count, counters, format_version=None, block_extra=None):
    """paletteObject 块：无 version = 旧实机产物形状；有 version = 显式版本化形状。"""
    block = {"watchCount": watch_count, "counters": counters}
    if format_version is not None:
        block["version"] = format_version
    for name, value in (block_extra or {}).items():
        block[name] = value
    return block


def segment_chain_events(part=1, segment=1, sequence_base=1, frame_base=500,
                         chain_sequence_base=1, lifecycle=0x4242, device_epoch=3000,
                         identity_proof=1, identity_weak=False, epoch_unknown=False,
                         session=1):
    """版本 2 的一条完整链：分段标签 + 载明的身份证明 + 非零实例生命周期身份。"""
    rows = closed_chain_events(part=part, sequence_base=sequence_base, frame_base=frame_base,
                               chain_sequence_base=chain_sequence_base,
                               identity_weak=identity_weak, epoch_unknown=epoch_unknown,
                               device_epoch=device_epoch, session=session)
    for row in rows:
        row["data"][2] = str(lifecycle)
        row["data"][3] = str(segment)
        row["words32"][15] = identity_proof
    return rows


def closed_chain_events(part=1, identity_weak=False, epoch_unknown=False, device_epoch=3000,
                        sequence_base=1, frame_base=500, session=1, chain_sequence_base=1):
    """Rejected -> ServedCandidate -> Enqueued -> Drawn -> terminal Recovered.

    Frame offsets mirror the frozen recorder arithmetic: renderFrame == firstRejectFrame
    for the Reject, +3 for the served candidate, and deltaFrames == renderFrame -
    firstRejectFrame everywhere the reader checks it.
    """
    object_key = key(part=part, device_epoch=device_epoch)

    def domain(offset):
        return frames(render_frame=frame_base + offset, manifest=11, revision=22, record=33,
                      native=44, manifest_unknown=False, native_unknown=False)

    def stage_event(step, frame_offset, stage, **kwargs):
        row = event(sequence_base + step, object_key, domain(frame_offset), stage=stage,
                    first_reject_frame=frame_base, identity_weak=identity_weak,
                    epoch_unknown=epoch_unknown, session=session, **kwargs)
        row["words32"][23] = chain_sequence_base + step
        return row

    return [
        stage_event(0, 0, 1, reason=1),
        stage_event(1, 3, 2, source=2, hit_key=0xABCD, hit_count=1, delta_frames=3),
        stage_event(2, 4, 3, source=2, hit_count=1, saw_submit=True),
        stage_event(3, 5, 4, source=2, hit_count=1, saw_submit=True, saw_draw=True),
        stage_event(4, 100, 4, source=2, hit_count=1, terminal=1, delta_frames=100,
                    saw_submit=True, saw_draw=True),
    ]

def observation_chain_events(part=7, stages=(2, 3, 4), terminal=7):
    """FirstSight -> [ServedCandidate] -> [Enqueued] -> Drawn -> terminal ObservationClosed.

    2026-09-18 P0-6（**写方/读方同步**）：Astra 裁定 C1 要求 S/E/D 对**两条链各自**记一份 ⇒
    观察链**合法地**可以携带 ServedCandidate（4 个阶段）；该对象从未被服务时是 3 个。
    `stages` 给出链首之外的阶段；delta 只对锚点阶段（FirstSight/ServedCandidate/终态）非零。
    """
    object_key = key(part=part)
    frame_base = 500
    served = 2 in stages

    def domain(offset):
        return frames(render_frame=frame_base + offset, manifest=11, revision=22, record=33,
                      native=44, manifest_unknown=False, native_unknown=False)

    def row(step, offset, stage, **kwargs):
        anchored = stage in (2, 5) or kwargs.get("terminal", 0)
        e = event(step + 1, object_key, domain(offset), stage=stage,
                  first_reject_frame=frame_base,
                  delta_frames=(offset if anchored else 0),
                  window_segment=1, chain_type=1,
                  hit_count=(1 if served else 0), **kwargs)
        e["words32"][23] = step + 1
        return e

    rows = [row(0, 0, 5)]
    offset = 3
    for idx, st in enumerate(stages):
        kw = {"source": 2}
        if st == 2:
            kw["hit_key"] = 0xABCD
        if st == 3:
            kw["saw_submit"] = True
        if st == 4:
            kw["saw_submit"] = True
            kw["saw_draw"] = True
        rows.append(row(idx + 1, offset, st, **kw))
        offset += 1
    rows.append(row(len(rows), offset, 4, source=2, terminal=terminal,
                    saw_submit=True, saw_draw=True))
    return rows


def hit_only_chain_events(part=2, terminal=6):
    object_key = key(part=part)
    first = frames(render_frame=800, manifest_unknown=False, native_unknown=False)
    served = frames(render_frame=801, manifest_unknown=False, native_unknown=False)
    closing = frames(render_frame=900, manifest_unknown=False, native_unknown=False)
    return [
        event(1, object_key, first, stage=1, reason=1, first_reject_frame=800),
        event(2, object_key, served, stage=2, source=3, hit_key=1, hit_count=1,
              first_reject_frame=800, delta_frames=1),
        event(3, object_key, closing, stage=4, source=3, hit_count=1, terminal=terminal,
              first_reject_frame=800, delta_frames=100),
    ]


class LayoutContracts(unittest.TestCase):
    def populated_event(self, unknown=True):
        return event(1, key(), frames(render_frame=500, manifest=11, revision=22, record=33,
                                      native=44, manifest_unknown=unknown,
                                      native_unknown=unknown),
                     stage=2, source=3, terminal=1, reason=2, hit_key=0xABCD, hit_count=4,
                     first_reject_frame=0x100000000, delta_frames=0x200000002,
                     saw_submit=True, saw_draw=True, selection_cleared=True,
                     identity_weak=True, epoch_unknown=True, session=5)

    def test_layout_slot_indices_are_frozen(self):
        encoded = self.populated_event()
        self.assertEqual(encoded["data"][0], "1")
        self.assertEqual(encoded["data"][1], "2")
        self.assertEqual(encoded["data"][9], "11")
        self.assertEqual(encoded["data"][10], "3")
        self.assertEqual(encoded["data"][11], str(0xABCD))
        bits = encoded["words32"]
        self.assertEqual(bits[0], 2)
        self.assertEqual(bits[1], 3)
        self.assertEqual(bits[2], 4)
        self.assertEqual(bits[5], 2)
        self.assertEqual(bits[10], 5)
        self.assertEqual(bits[12], 7)
        self.assertEqual(bits[18], 1)
        self.assertEqual(bits[20], 0x00000002)
        self.assertEqual(bits[33], 0x2)
        self.assertEqual(bits[21], 44)
        self.assertEqual(bits[22], 4)
        self.assertEqual(bits[24], 33)
        self.assertEqual(bits[26], 22)
        self.assertEqual(bits[28], 0x1F)
        known = self.populated_event(unknown=False)
        self.assertEqual(known["words32"][28], 0x07,
                         "unknown frame domains must be the flagged difference, not a zero payload")
        for index in analyzer.RESERVED_DATA:
            self.assertEqual(encoded["data"][index], "0")
        for index in analyzer.RESERVED_BITS:
            self.assertEqual(bits[index], 0)

    def test_field_mapping_round_trips(self):
        decoded = analyzer.decode_event(self.populated_event())
        self.assertEqual(decoded["stage"], "ServedCandidate")
        self.assertEqual(decoded["source"], "PoseKernel")
        self.assertEqual(decoded["terminal"], "Recovered")
        self.assertEqual(decoded["rejectReason"], "R2")
        self.assertEqual(decoded["key"], {"renderablePart": "1", "runtimeModelPtr": "2",
                                          "jHandle": "3", "rawcode": "4",
                                          "sessionGeneration": "5", "mapEpoch": "2000",
                                          "deviceEpoch": "3000", "lifecycleIdentity": "0"})
        self.assertEqual(decoded["frames"], {"renderFrame": "500",
                                             "manifestFrameSerial": "11",
                                             "manifestPublishRevision": "22",
                                             "recordFrameSerial": "33",
                                             "nativeFrameTag": "44",
                                             "manifestUnknown": True,
                                             "nativeUnknown": True})
        self.assertEqual(decoded["hitKey"], str(0xABCD))
        self.assertEqual(decoded["hitCount"], 4)
        self.assertEqual(decoded["deltaFrames"], str(0x200000002))
        self.assertEqual(decoded["firstRejectFrame"], str(0x100000000))
        self.assertTrue(decoded["sawSubmit"])
        self.assertTrue(decoded["sawDraw"])
        self.assertTrue(decoded["selectionClearedByNativeOverride"])
        self.assertTrue(decoded["identityWeak"])
        self.assertTrue(decoded["epochUnknown"])

    def test_unknown_enums_fail_visible(self):
        for mutate in (lambda e: e["words32"].__setitem__(5, 9),
                       lambda e: e["words32"].__setitem__(18, 7),
                       lambda e: e["data"].__setitem__(10, "9"),
                       lambda e: e["words32"].__setitem__(0, 0x7F)):
            encoded = self.populated_event()
            mutate(encoded)
            with self.assertRaises(ValueError):
                analyzer.decode_event(encoded)

    def test_reserved_slots_must_stay_zero(self):
        for mutate in (lambda e: e["data"].__setitem__(4, "1"),
                       lambda e: e["words32"].__setitem__(40, 1),
                       lambda e: e["words32"].__setitem__(14, 3)):
            encoded = self.populated_event()
            mutate(encoded)
            with self.assertRaises(ValueError):
                analyzer.decode_event(encoded)

    def test_thread_witness_and_label_must_match(self):
        encoded = self.populated_event()
        encoded["words32"][12] = 9
        with self.assertRaises(ValueError):
            analyzer.decode_event(encoded)
        encoded = self.populated_event()
        encoded["label"] = "skin-selection/v1"
        with self.assertRaises(ValueError):
            analyzer.decode_event(encoded)
        encoded = self.populated_event()
        encoded["kind"] = 19
        with self.assertRaises(ValueError):
            analyzer.decode_event(encoded)

    # ---- lifecycleIdentity: eighth key field, carried at data[2] ------------------
    def test_lifecycle_identity_is_carried_and_reported(self):
        result = analyzer.analyze(envelope(closed_chain_events()))
        self.assertEqual(analyzer.LIFECYCLE_IDENTITY_SLOT, ("data", 2),
                         "the C++ sink writes data[2]; the reader must declare that slot")
        self.assertEqual(analyzer.CHAIN_IDENTITY_FIELDS[-1], "lifecycleIdentity")
        self.assertNotIn(2, analyzer.RESERVED_DATA,
                         "data[2] is now the lifecycleIdentity carrier, not reserved")
        for reserved in (3, 4, 5, 6, 7, 8):
            self.assertIn(reserved, analyzer.RESERVED_DATA)
        self.assertTrue(result["lifecycleIdentityCarried"])
        self.assertEqual(result["lifecycleIdentitySlot"], ["data", 2])
        self.assertEqual(result["wireGaps"], [])
        self.assertEqual(result["proposedLifecycleIdentitySlot"], ["data", 2])
        chain = result["chains"][0]
        self.assertEqual(chain["lifecycleIdentity"], "0")
        self.assertTrue(chain["lifecycleIdentityCarried"])
        self.assertEqual(len(chain["key"]), 8, "grouping key must stay the eight-tuple")

    def test_cpp_key_and_sink_carry_lifecycle_identity(self):
        header = (ROOT / "src/d3d9/war3/tools/war3_palette_object_evidence.h").read_text(
            encoding="utf-8")
        sink = (ROOT / "src/d3d9/war3/tools/war3_palette_object_evidence_sink.cpp").read_text(
            encoding="utf-8")
        self.assertIn("uint64_t lifecycleIdentity = 0u;", header)
        self.assertIn("a.lifecycleIdentity == b.lifecycleIdentity", header)
        self.assertIn("k.deviceEpoch, k.lifecycleIdentity};", header)
        # 2026-09-17 主线程把编码体抽成公开纯函数 EncodePaletteObjectEvent()（发射器只调用它再 Record）；
        # 本次往返测试落地时发现：本断言原先只查 ENABLED 时的发射器函数体，抽取后必然失败。
        # 断言目标改为**生产转换点**本体，并**追加**发射器仍委托该转换点这一条（收紧，不放松）。
        encoder = sink.split("bool EncodePaletteObjectEvent(", 1)[1]
        self.assertIn("event.data[2] = record.key.lifecycleIdentity;", encoder,
                      "上级 03:58 要求：生命周期身份必须由生产转换点写入声明槽位 data[2]")
        emitter = sink.split("void EmitPaletteObjectEvent(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("EncodePaletteObjectEvent(record, session, event)", emitter,
                      "发射器必须调用唯一的转换点 EncodePaletteObjectEvent()，不得自带第二份映射")

    def test_lifecycle_identity_slot_cannot_be_faked_while_undeclared(self):
        # "保留位不得被用来伪造身份"：把槽位声明退回 None 时，非零 data[2] 必须报错。
        encoded = closed_chain_events()[0]
        encoded["data"][2] = "2730"
        with mock.patch.object(analyzer, "LIFECYCLE_IDENTITY_SLOT", None), \
                mock.patch.object(analyzer, "RESERVED_DATA", (2, 3, 4, 5, 6, 7, 8)):
            with self.assertRaises(ValueError):
                analyzer.decode_event(encoded)

    def test_declared_lifecycle_identity_slot_splits_two_instances(self):
        """Grouping counterexample: two instances differing only in lifecycleIdentity."""
        first = closed_chain_events(part=7, sequence_base=1, frame_base=500,
                                    chain_sequence_base=1)
        second = closed_chain_events(part=7, sequence_base=6, frame_base=700,
                                     chain_sequence_base=1)
        for row in first:
            row["data"][2] = str(0x1111)
        for row in second:
            row["data"][2] = str(0x2222)
        export = envelope(first + second)
        with mock.patch.object(analyzer, "LIFECYCLE_IDENTITY_SLOT", ("data", 2)):
            result = analyzer.analyze(export)
            self.assertTrue(result["lifecycleIdentityCarried"])
            self.assertEqual(result["wireGaps"], [])
            self.assertEqual(result["objectCount"], 2,
                             "two instances must not collapse into one chain")
            self.assertEqual(result["chains"][0]["lifecycleIdentity"], str(0x1111))
            self.assertEqual(result["chains"][1]["lifecycleIdentity"], str(0x2222))
            self.assertEqual(len(result["recovered"]), 2)
            self.assertTrue(all(chain["sameObjectCertified"] for chain in result["chains"]))
        # Undeclared slot: the same distinction is not carried by the wire, and the
        # collapsed group must be refused instead of silently certified.
        for row in first + second:
            row["data"][2] = "0"
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(first + second))


class ChainContracts(unittest.TestCase):
    def test_chain_is_rebuilt_from_sequence(self):
        events = closed_chain_events()
        shuffled = [events[3], events[1], events[4], events[0], events[2]]
        # The export on disk is always emitted in sequence order (Ring::visitFrozen
        # sorts by sequence), so a non-monotonic file is invalid input -- see
        # test_non_monotonic_export_sequence_is_rejected. The chain itself is still
        # rebuilt from the sequence numbers, never from file position.
        ordered = sorted(shuffled, key=lambda row: int(row["sequence"]))
        chain = analyzer.analyze(envelope(ordered))["chains"][0]
        self.assertEqual(chain["stageOrder"], ["Rejected", "ServedCandidate", "Enqueued",
                                               "Drawn", "Drawn"])
        self.assertEqual(chain["sequenceSpan"], [1, 5])
        self.assertEqual(chain["eventCount"], 5)

    def test_closed_chain_is_the_only_recovery(self):
        result = analyzer.analyze(envelope(closed_chain_events()))
        self.assertTrue(result["chainComplete"])
        self.assertFalse(result["chainMissing"])
        self.assertEqual(result["missing"], [])
        chain = result["chains"][0]
        self.assertEqual(chain["conclusion"], "Recovered")
        self.assertTrue(chain["chainComplete"])
        self.assertTrue(chain["sameObjectCertified"])
        self.assertEqual(chain["certificationRefusals"], [])
        self.assertTrue(chain["sawSubmit"])
        self.assertTrue(chain["sawDraw"])
        self.assertEqual(chain["hitCountMax"], 1)
        self.assertEqual(len(result["recovered"]), 1)

    def test_palette_hit_alone_is_unclosed_not_recovered(self):
        result = analyzer.analyze(envelope(hit_only_chain_events()))
        chain = result["chains"][0]
        self.assertEqual(chain["terminal"], "Unclosed")
        self.assertEqual(chain["conclusion"], "Unclosed")
        self.assertFalse(chain["chainComplete"])
        self.assertEqual(result["recovered"], [])
        # The export itself is complete; the object simply never closed as recovered.
        self.assertTrue(result["coverageComplete"])
        self.assertFalse(result["chainMissing"])
        self.assertFalse(result["chainComplete"])
        self.assertNotIn("Recovered", chain["stageOrder"])

    def test_window_expired_and_object_gone_stay_distinct(self):
        expired = analyzer.analyze(envelope([event(1, key(part=9),
                                                    frames(render_frame=800),
                                                    stage=1, reason=1, first_reject_frame=800),
                                             event(2, key(part=9),
                                                   frames(render_frame=900), stage=4,
                                                   terminal=2, first_reject_frame=800,
                                                   delta_frames=100)]))["chains"][0]
        gone = analyzer.analyze(envelope(hit_only_chain_events(part=8, terminal=3)))["chains"][0]
        self.assertEqual(expired["conclusion"], "WindowExpired")
        self.assertEqual(gone["conclusion"], "ObjectGone")
        self.assertNotEqual(expired["conclusion"], gone["conclusion"])

    def test_missing_enqueued_stage_is_not_recovered(self):
        events = closed_chain_events()
        del events[2]
        events[3]["words32"][28] |= 1  # sawSubmit without a live Enqueued event
        result = analyzer.analyze(envelope(events))
        chain = result["chains"][0]
        self.assertEqual(chain["conclusion"], "Uncovered")
        self.assertFalse(chain["covered"])
        self.assertFalse(chain["chainComplete"])
        self.assertIn("submitFlagWithoutEnqueuedEvent", chain["missingStages"])
        self.assertEqual(result["recovered"], [])

    def test_drawn_flag_without_drawn_event_is_missing_chain(self):
        events = closed_chain_events()
        del events[3]
        result = analyzer.analyze(envelope(events))
        chain = result["chains"][0]
        self.assertIn("drawnFlagWithoutDrawnEvent", chain["missingStages"])
        self.assertEqual(chain["conclusion"], "Uncovered")

    def test_order_violation_blocks_recovery(self):
        # The root sequence stays monotonic (a non-monotonic file is rejected earlier);
        # the chain itself records ServedCandidate before Rejected, which is the
        # in-chain order violation the recorder's own MarkStage() guards against.
        events = closed_chain_events()
        events[0]["words32"][5], events[1]["words32"][5] = (
            events[1]["words32"][5], events[0]["words32"][5])
        result = analyzer.analyze(envelope(events))
        chain = result["chains"][0]
        self.assertIn("servedCandidateBeforeRejected", chain["orderIssues"])
        self.assertEqual(chain["conclusion"], "Unclosed")
        self.assertNotEqual(chain["conclusion"], "Recovered")
        self.assertIn("terminalRecoveredNotCertified", chain["orderIssues"])

    def test_truncated_served_candidates_are_missing_chain(self):
        events = closed_chain_events()
        events[2]["words32"][22] = 3  # hitCount proves two Served events were lost
        result = analyzer.analyze(envelope(events))
        chain = result["chains"][0]
        self.assertTrue(any(entry.startswith("servedCandidateEventsTruncated")
                            for entry in chain["missingStages"]))
        self.assertEqual(chain["conclusion"], "Uncovered")

    def test_broken_delta_bookkeeping_is_reported(self):
        events = closed_chain_events()
        events[1]["words32"][20] = 99
        result = analyzer.analyze(envelope(events))
        chain = result["chains"][0]
        self.assertTrue(any(entry.startswith("deltaFramesMismatch") for entry in chain["orderIssues"]))
        self.assertNotEqual(chain["conclusion"], "Recovered")

    # ---- 03:58 counterexamples: weak identity / unknown epoch ---------------------
    def test_identity_weak_chain_is_not_certified(self):
        result = analyzer.analyze(envelope(closed_chain_events(identity_weak=True)))
        chain = result["chains"][0]
        self.assertTrue(chain["stageObservationComplete"],
                        "a weak-identity chain may still report a complete stage observation")
        self.assertFalse(chain["sameObjectCertified"])
        self.assertIn("identityWeak", chain["certificationRefusals"])
        self.assertEqual(chain["conclusion"], "StageCompleteUncertified")
        self.assertNotEqual(chain["conclusion"], "Recovered")
        self.assertFalse(chain["chainComplete"])
        self.assertEqual(result["recovered"], [])
        self.assertEqual(len(result["uncertified"]), 1)
        self.assertFalse(result["chainComplete"])
        self.assertTrue(result["stageObservationComplete"])

    def test_epoch_unknown_chain_is_not_certified(self):
        result = analyzer.analyze(envelope(closed_chain_events(device_epoch=0,
                                                               epoch_unknown=True)))
        chain = result["chains"][0]
        self.assertEqual(chain["key"][6], "0")
        self.assertTrue(chain["epochUnknown"])
        self.assertTrue(chain["stageObservationComplete"])
        self.assertFalse(chain["sameObjectCertified"])
        self.assertIn("epochUnknown", chain["certificationRefusals"])
        self.assertEqual(chain["conclusion"], "StageCompleteUncertified")
        self.assertEqual(result["recovered"], [])
        self.assertFalse(result["chainComplete"])
        self.assertTrue(result["stageObservationComplete"])
        self.assertEqual(result["uncertified"], [chain["key"]])

    def test_weak_and_unknown_together_name_both_refusals(self):
        result = analyzer.analyze(envelope(closed_chain_events(identity_weak=True,
                                                               device_epoch=0,
                                                               epoch_unknown=True)))
        chain = result["chains"][0]
        self.assertEqual(chain["certificationRefusals"][:2], ["identityWeak", "epochUnknown"])
        self.assertFalse(chain["sameObjectCertified"])
        self.assertEqual(result["recovered"], [])

    def test_merged_instances_with_reset_chain_sequence_are_not_certified(self):
        """A wire-level collapse of two instances must not be certified as one object."""
        def build(reset_chain_sequence):
            rows = closed_chain_events(sequence_base=1, frame_base=500, chain_sequence_base=1)
            # Same seven-tuple, but this row belongs to a second instance whose own
            # chainSequence counter restarts at 1 (lifecycleIdentity is the only field
            # the recorder could have used to keep the two entries apart, and it has no
            # wire slot).
            rows[3] = event(4, key(part=1), frames(render_frame=505, manifest_unknown=False,
                                                   native_unknown=False),
                            stage=4, source=2, hit_count=1, first_reject_frame=500,
                            saw_submit=True, saw_draw=True,
                            chain_sequence=1 if reset_chain_sequence else 4)
            return rows

        control = analyzer.analyze(envelope(build(False)))["chains"][0]
        self.assertEqual(control["conclusion"], "Recovered",
                         "the collapse guard must be the only difference")
        self.assertTrue(control["sameObjectCertified"])
        result = analyzer.analyze(envelope(build(True)))
        chain = result["chains"][0]
        # 2026-09-17 上级复验：issue 更名为"不严格递增"，同一回退反例仍必须判出。
        self.assertEqual(chain["orderIssues"][0],
                         "chainSequenceNotStrictlyIncreasing@4")
        self.assertFalse(chain["stageObservationComplete"])
        self.assertFalse(chain["sameObjectCertified"])
        self.assertNotEqual(chain["conclusion"], "Recovered")
        self.assertEqual(result["recovered"], [])

    def test_wire_gap_can_be_promoted_to_certification_refusal(self):
        # 槽位已落地 ⇒ 不再有 wire gap；用 monkeypatch 模拟"未承载"状态验证该开关仍有效。
        with mock.patch.object(analyzer, "REQUIRE_WIRE_LIFECYCLE_IDENTITY", True), \
                mock.patch.object(analyzer, "LIFECYCLE_IDENTITY_SLOT", None), \
                mock.patch.object(analyzer, "RESERVED_DATA", (2, 3, 4, 5, 6, 7, 8)):
            result = analyzer.analyze(envelope(closed_chain_events()))
            chain = result["chains"][0]
            self.assertIn("lifecycleIdentityNotCarried", chain["certificationRefusals"])
            self.assertFalse(chain["sameObjectCertified"])
            self.assertEqual(result["recovered"], [])
            self.assertFalse(result["chainComplete"])


class ChainSequenceStrictnessContracts(unittest.TestCase):
    """2026-09-17 上级复验反例：一条链内的 chainSequence 必须严格递增、唯一、无缺口。

    记录器对**每个条目**的每次发射写 `record.chainSequence = ++entry.chainSequence`
    （war3_palette_object_evidence.h:228/301/343/383/423/455；一次性 TableFull 终态在
    :269 直接写 1u），因此一条链内的序号只能是 1,2,3,...。旧读方只比较
    `chainSequence < previous`，于是"整条链全写 1"曾被认证为
    chainComplete=True / recovered=1 / missing=[] / CLI 0（上级已亲自复验）；
    "链内跳号 1,2,4"同样曾被认证。修复后两者都必须被拒，而合法的 1,2,3,4,5 与
    "不同链各自从 1 开始"必须保持认证（防过度收紧）。
    """

    def sequenced(self, values, **kwargs):
        rows = closed_chain_events(**kwargs)
        self.assertEqual(len(rows), len(values), "fixture shape changed")
        for row, value in zip(rows, values):
            row["words32"][23] = value
        return rows

    def analyzed(self, values, **kwargs):
        payload = envelope(self.sequenced(values, **kwargs))
        return analyzer.analyze(payload), payload

    def test_whole_chain_with_one_repeated_sequence_is_not_certified(self):
        # 修前：chainComplete=True、recovered=1、missing=[]、CLI 0。修后：必须拒绝。
        result, payload = self.analyzed([1, 1, 1, 1, 1])
        self.assertEqual([row["words32"][23] for row in payload["events"]],
                         [1, 1, 1, 1, 1], "the counterexample must be exactly all-equal")
        chain = result["chains"][0]
        self.assertIn("chainSequenceNotStrictlyIncreasing@2", chain["orderIssues"])
        self.assertFalse(chain["stageObservationComplete"],
                         "the issue is at the same level as the stage-order issues")
        self.assertFalse(chain["sameObjectCertified"])
        self.assertFalse(chain["chainComplete"])
        self.assertNotEqual(chain["conclusion"], "Recovered")
        self.assertEqual(result["recovered"], [])
        self.assertFalse(result["chainComplete"])
        self.assertFalse(result["chainMissing"], "the export itself is intact; the chain is not")
        self.assertEqual(result["chains"][0]["certificationRefusals"], ["recoveryNotProven"])

    def test_late_duplicate_is_refused(self):
        result, _ = self.analyzed([1, 2, 3, 3, 5])
        chain = result["chains"][0]
        self.assertIn("chainSequenceNotStrictlyIncreasing@4", chain["orderIssues"])
        self.assertFalse(chain["sameObjectCertified"])
        self.assertEqual(result["recovered"], [])

    def test_rollback_names_the_same_strictness_issue(self):
        result, _ = self.analyzed([1, 2, 3, 2, 5])
        chain = result["chains"][0]
        self.assertIn("chainSequenceNotStrictlyIncreasing@4", chain["orderIssues"])
        self.assertFalse(chain["sameObjectCertified"])
        self.assertEqual(result["recovered"], [])

    def test_chain_sequence_hole_is_a_missing_chain_gap(self):
        # 1,2,4,5,6：记录器发过、导出里缺失的那条事件正好是缺口。
        result, _ = self.analyzed([1, 2, 4, 5, 6])
        chain = result["chains"][0]
        self.assertTrue(any(entry.startswith("chainSequenceGap@3")
                            for entry in chain["missingStages"]), chain["missingStages"])
        self.assertFalse(chain["covered"])
        self.assertEqual(chain["conclusion"], "Uncovered")
        self.assertIn("stageStreamTruncated", chain["certificationRefusals"])
        self.assertFalse(chain["stageObservationComplete"])
        self.assertFalse(chain["sameObjectCertified"])
        self.assertEqual(result["recovered"], [])
        self.assertTrue(result["chainMissing"])

    def test_chain_sequence_missing_head_is_a_hole(self):
        # 3,4,5,6,7 连续但不从 1 开始：链头事件不在导出里，同样不得认证。
        result, _ = self.analyzed([2, 3, 4, 5, 6])
        chain = result["chains"][0]
        self.assertTrue(any(entry.startswith("chainSequenceGap@1(expected 1, saw 2)")
                            for entry in chain["missingStages"]), chain["missingStages"])
        self.assertFalse(chain["covered"])
        self.assertEqual(result["recovered"], [])

    def test_legal_strictly_increasing_chain_is_still_certified(self):
        result, _ = self.analyzed([1, 2, 3, 4, 5])
        chain = result["chains"][0]
        self.assertEqual(chain["orderIssues"], [])
        self.assertEqual(chain["missingStages"], [])
        self.assertEqual(chain["certificationRefusals"], [])
        self.assertTrue(chain["stageObservationComplete"])
        self.assertTrue(chain["sameObjectCertified"])
        self.assertTrue(result["chainComplete"])
        self.assertEqual(result["recovered"], [chain["key"]])
        self.assertEqual(result["missing"], [])

    def test_two_chains_may_each_restart_at_one(self):
        # cross-chain 的合法情况：不同链各自 1..5 不得互相干扰（读方只比链内取值）。
        first = closed_chain_events(part=1, sequence_base=1, frame_base=500,
                                    chain_sequence_base=1)
        second = closed_chain_events(part=2, sequence_base=6, frame_base=700,
                                     chain_sequence_base=1)
        result = analyzer.analyze(envelope(first + second))
        self.assertEqual(result["objectCount"], 2)
        self.assertTrue(result["chainComplete"])
        self.assertTrue(result["coverageComplete"])
        self.assertEqual(len(result["recovered"]), 2)
        for chain in result["chains"]:
            self.assertEqual(chain["orderIssues"], [])
            self.assertEqual(chain["missingStages"], [])
            self.assertTrue(chain["sameObjectCertified"])

    def test_counterexample_cli_exit_codes(self):
        # 修前 duplicate/hole 两条反例的 CLI 都是 0（认证）；修后必须翻转为 2。
        cases = (("duplicate.json", [1, 1, 1, 1, 1], 2),
                 ("hole.json", [1, 2, 4, 5, 6], 2),
                 ("legal.json", [1, 2, 3, 4, 5], 0))
        with tempfile.TemporaryDirectory() as temp_dir:
            for name, values, expected in cases:
                with self.subTest(name=name):
                    path = Path(temp_dir) / name
                    path.write_text(json.dumps(envelope(self.sequenced(values))),
                                    encoding="utf-8")
                    with mock.patch.object(sys, "argv",
                                           ["analyze_palette_object_evidence.py", str(path)]):
                        with contextlib.redirect_stdout(io.StringIO()):
                            self.assertEqual(analyzer.main(), expected)


class CoverageContracts(unittest.TestCase):
    def test_dropped_counters_force_uncovered(self):
        for counter in ("droppedPerFrame", "droppedPerSession", "droppedTableFull",
                        "droppedProbeLimit", "ringEvictedAfterRecord",
                        "droppedPayloadConflict", "droppedTerminalReserve"):
            with self.subTest(counter=counter):
                result = analyzer.analyze(envelope(closed_chain_events(),
                                                   counter_overrides={counter: 1}))
                self.assertTrue(result["chainMissing"])
                self.assertFalse(result["chainComplete"])
                self.assertIn("objectLevelEvidenceDropped", result["missing"])
                self.assertEqual(result["recovered"], [])
                self.assertEqual(result["chains"][0]["conclusion"], "Uncovered")

    def test_dropped_terminal_reserve_is_a_loss_field(self):
        self.assertIn("droppedTerminalReserve", analyzer.LOSS_FIELDS)
        result = analyzer.analyze(envelope(closed_chain_events(),
                                           counter_overrides={"droppedTerminalReserve": 2}))
        self.assertEqual(result["losses"], {"droppedTerminalReserve": 2})
        self.assertTrue(result["chainMissing"])
        self.assertFalse(result["chainComplete"])
        self.assertEqual(result["chains"][0]["conclusion"], "Uncovered")
        self.assertEqual(result["recovered"], [])

    def test_dropped_payload_conflict_is_a_loss_field(self):
        """D6（2026-09-17 上级裁定）：载荷冲突是**损失**，不是"已证明等价的重复"。

        同一键/同一帧/同一阶段但载荷不等价（来源 / hitKey / 清空标志 / 拒绝原因不同）的事件
        不再被压缩成"重复"：记录器把它计入 droppedPayloadConflict。该计数非零说明这条链的
        第二次事实**没有**被记录，因此与其它 dropped* 同级 —— 缺链/未覆盖、不得认证。
        三个方向都在这里钉死：非零 ⇒ 未覆盖；全零 ⇒ 仍可认证；CLI 层非零 ⇒ exit 2。
        """
        self.assertIn("droppedPayloadConflict", analyzer.COUNTER_FIELDS)
        self.assertIn("droppedPayloadConflict", analyzer.LOSS_FIELDS)
        # 方向 1：全零 ⇒ 干净夹具仍然认证，且 losses 里不得出现该字段。
        clean = analyzer.analyze(envelope(closed_chain_events()))
        self.assertEqual(clean["counters"]["droppedPayloadConflict"], 0)
        self.assertNotIn("droppedPayloadConflict", clean["losses"])
        self.assertEqual(clean["losses"], {})
        self.assertTrue(clean["chainComplete"])
        self.assertEqual(len(clean["recovered"]), 1)
        # 方向 2：非零 ⇒ 未覆盖 + losses 具名 + 任何链都不得认证。
        result = analyzer.analyze(envelope(closed_chain_events(),
                                           counter_overrides={"droppedPayloadConflict": 3}))
        self.assertEqual(result["losses"], {"droppedPayloadConflict": 3})
        self.assertIn("objectLevelEvidenceDropped", result["missing"])
        self.assertTrue(result["chainMissing"])
        self.assertFalse(result["chainComplete"])
        self.assertFalse(result["coverageComplete"])
        self.assertEqual(result["recovered"], [])
        self.assertEqual(result["chains"][0]["conclusion"], "Uncovered")
        # 方向 3：CLI 层必须从 0 翻转为 2（"损失必须 fail-visible"）。
        with tempfile.TemporaryDirectory() as temp_dir:
            lossy = Path(temp_dir) / "payload_conflict.json"
            lossy.write_text(json.dumps(envelope(
                closed_chain_events(),
                counter_overrides={"droppedPayloadConflict": 1})), encoding="utf-8")
            with mock.patch.object(sys, "argv",
                                   ["analyze_palette_object_evidence.py", str(lossy)]):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(analyzer.main(), 2)

    def test_missing_payload_conflict_counter_is_rejected(self):
        """根信封 counter 集合是**精确相等**集：没有该计数的导出必须被硬拒绝。

        漏加该字段会让所有真实导出被判字段不匹配（这是显式拒绝，不是静默通过）；
        多了字段同样被既有 test_envelope_fields_and_counter_set_are_strict 拒绝。
        """
        data = envelope(closed_chain_events())
        self.assertIn("droppedPayloadConflict", data["paletteObject"]["counters"])
        del data["paletteObject"]["counters"]["droppedPayloadConflict"]
        with self.assertRaises(ValueError):
            analyzer.analyze(data)

    # ---- 03:58 counterexample 1: sub-gate on with zero events --------------------
    def test_zero_events_with_subgate_on_is_not_covered(self):
        result = analyzer.analyze(envelope([], enabled=True))
        self.assertTrue(result["paletteObjectEvidence"])
        self.assertTrue(result["emptySample"])
        self.assertFalse(result["objectEvidencePresent"])
        self.assertEqual(result["eventCount"], 0)
        self.assertEqual(result["chains"], [])
        self.assertIn("noObjectEvidenceExported", result["missing"])
        self.assertFalse(result["coverageComplete"], "an empty sample can only be uncovered")
        self.assertFalse(result["chainComplete"], "all([]) must not stand in for evidence")
        self.assertFalse(result["stageObservationComplete"])
        self.assertTrue(result["chainMissing"])
        self.assertEqual(result["recovered"], [])

    def test_zero_events_are_not_confused_with_a_disabled_subgate(self):
        empty = analyzer.analyze(envelope([], enabled=True))
        disabled = analyzer.analyze(envelope([], enabled=False))
        self.assertIn("noObjectEvidenceExported", empty["missing"])
        self.assertNotIn("paletteObjectSubGateDisabled", empty["missing"])
        self.assertIn("paletteObjectSubGateDisabled", disabled["missing"])
        self.assertIn("noObjectEvidenceExported", disabled["missing"])

    def test_cli_exit_codes_for_empty_and_uncertified_exports(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            cases = (("empty.json", envelope([], enabled=True), 2),
                     ("weak.json", envelope(closed_chain_events(identity_weak=True)), 2),
                     ("clean.json", envelope(closed_chain_events()), 0))
            for name, payload, expected in cases:
                with self.subTest(name=name):
                    path = Path(temp_dir) / name
                    path.write_text(json.dumps(payload), encoding="utf-8")
                    with mock.patch.object(sys, "argv",
                                           ["analyze_palette_object_evidence.py", str(path)]):
                        with contextlib.redirect_stdout(io.StringIO()) as captured:
                            self.assertEqual(analyzer.main(), expected)
                    self.assertIn("chainComplete", captured.getvalue())

    def test_envelope_eviction_forces_missing_chain(self):
        result = analyzer.analyze(envelope(closed_chain_events(), evicted="1"))
        self.assertTrue(result["chainMissing"])
        self.assertIn("ringEvictionObserved", result["missing"])
        self.assertEqual(result["recovered"], [])

    def test_emitted_counter_mismatch_is_missing_chain(self):
        result = analyzer.analyze(envelope(closed_chain_events(), emitted=9))
        self.assertTrue(result["chainMissing"])
        self.assertTrue(any(entry.startswith("emittedCounterVersusExportedEvents")
                            for entry in result["missing"]))
        self.assertEqual(result["recovered"], [])

    def test_disabled_subgate_cannot_look_complete(self):
        result = analyzer.analyze(envelope([], enabled=False))
        self.assertFalse(result["paletteObjectEvidence"])
        self.assertTrue(result["chainMissing"])
        self.assertIn("paletteObjectSubGateDisabled", result["missing"])

    def test_events_without_subgate_are_rejected(self):
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(closed_chain_events(), enabled=False))

    def test_watch_count_and_duplicate_counter_are_not_loss_evidence(self):
        result = analyzer.analyze(envelope(closed_chain_events(), watch_count=1,
                                           counter_overrides={"droppedDuplicatePerFrame": 4}))
        self.assertFalse(result["chainMissing"])
        self.assertTrue(result["chainComplete"])
        self.assertEqual(result["watchCount"], 1)
        self.assertEqual(len(result["recovered"]), 1)

    def test_envelope_fields_and_counter_set_are_strict(self):
        data = envelope(closed_chain_events())
        data["paletteObject"]["counters"]["invented"] = "0"
        with self.assertRaises(ValueError):
            analyzer.analyze(data)
        data = envelope(closed_chain_events())
        del data["paletteObject"]
        with self.assertRaises(ValueError):
            analyzer.analyze(data)
        data = envelope(closed_chain_events())
        data["schema"] = 6
        with self.assertRaises(ValueError):
            analyzer.analyze(data)

    # ---- 03:58 counterexample 4 + strict root validation -------------------------
    def test_reduced_synthetic_envelope_is_rejected(self):
        shrunken = {"schema": 7, "session": "1", "accepted": "5", "evicted": "0",
                    "effectiveConfiguration": {"frameEvidence": True, "rawInputs": False,
                                               "paletteObjectEvidence": True,
                                               "skinPaletteContract": False,
                                               "localRecorderOwner": False},
                    "paletteObject": {"watchCount": 0,
                                      "counters": {name: "0"
                                                   for name in analyzer.COUNTER_FIELDS}},
                    "events": closed_chain_events()}
        shrunken["paletteObject"]["counters"]["emitted"] = "5"
        with self.assertRaises(ValueError):
            analyzer.analyze(shrunken)
        for field in ("state", "reason", "producerLosses", "reserved", "capacity",
                      "capabilities", "processNonce"):
            with self.subTest(field=field):
                data = envelope(closed_chain_events())
                del data[field]
                with self.assertRaises(ValueError):
                    analyzer.analyze(data)

    def test_event_session_must_match_the_root_session(self):
        events = closed_chain_events()
        for row in events:
            row["session"] = "9"
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(events))

    def test_non_monotonic_export_sequence_is_rejected(self):
        events = closed_chain_events()
        events[0]["sequence"], events[1]["sequence"] = events[1]["sequence"], events[0]["sequence"]
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(events))

    def test_duplicate_export_sequence_is_rejected(self):
        events = closed_chain_events()
        events[2]["sequence"] = events[1]["sequence"]
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(events))

    def test_retention_and_ticket_accounting_are_enforced(self):
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(closed_chain_events(), accepted=99))
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(closed_chain_events(), reserved=7, producer_losses=0))
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(closed_chain_events(), reserved=3))

    def test_export_with_frame_evidence_spans_keeps_the_palette_chain(self):
        """A real export also carries non-palette events: validated, never read as ours."""
        def span(sequence, kind, parent=0):
            row = event(sequence, key(), frames(render_frame=500), stage=1, reason=1,
                        first_reject_frame=500)
            row["kind"] = kind
            row["parent"] = str(parent)
            row["label"] = "FrameSpan/v1"
            row["data"] = ["0"] * 12
            row["words32"] = [0] * 48
            return row

        palette = closed_chain_events(sequence_base=3, chain_sequence_base=1)
        result = analyzer.analyze(envelope([span(1, 1), span(2, 2, parent=1)] + palette,
                                          emitted=len(palette)))
        self.assertEqual(result["eventCount"], len(palette))
        self.assertEqual(result["frameEvidenceGaps"], [])
        self.assertTrue(result["chainComplete"])
        self.assertEqual(len(result["recovered"]), 1)

        # A broken frame-evidence span in the same file is reported, never ignored.
        broken = analyzer.analyze(envelope([span(1, 2, parent=0)] +
                                           closed_chain_events(sequence_base=2),
                                           emitted=5))
        self.assertIn("unmatchedCpuSpans", broken["missing"])
        self.assertIn("unmatchedCpuSpans", broken["frameEvidenceGaps"])
        self.assertTrue(broken["chainMissing"])
        self.assertFalse(broken["chainComplete"])

    def test_producer_loss_forces_not_covered(self):
        result = analyzer.analyze(envelope(closed_chain_events(), reserved=6,
                                           producer_losses=1))
        self.assertIn("producerContentionOrException", result["missing"])
        self.assertIn("producerContentionOrException", result["frameEvidenceGaps"])
        self.assertTrue(result["chainMissing"])
        self.assertFalse(result["chainComplete"])
        self.assertEqual(result["recovered"], [])
        self.assertEqual(result["chains"][0]["conclusion"], "Uncovered")

    def test_post_window_incomplete_forces_not_covered(self):
        result = analyzer.analyze(envelope(closed_chain_events(), post_remaining=4))
        self.assertIn("postWindowNotComplete", result["missing"])
        self.assertTrue(result["chainMissing"])
        self.assertFalse(result["chainComplete"])

    def test_fixture_file_round_trip_and_cli_exit_code(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            clean = Path(temp_dir) / "clean.json"
            clean.write_text(json.dumps(envelope(closed_chain_events())), encoding="utf-8")
            result = analyzer.analyze(analyzer.load(clean))
            self.assertTrue(result["chainComplete"])
            output = Path(temp_dir) / "clean.analysis.json"
            with mock.patch.object(sys, "argv", ["analyze_palette_object_evidence.py",
                                                 str(clean), "--output", str(output)]):
                with contextlib.redirect_stdout(io.StringIO()) as captured:
                    self.assertEqual(analyzer.main(), 0)
            self.assertIn("chainComplete", captured.getvalue())
            self.assertTrue(output.is_file())

            lossy = Path(temp_dir) / "lossy.json"
            lossy.write_text(json.dumps(envelope(closed_chain_events(),
                                                 counter_overrides={"droppedPerFrame": 3})),
                             encoding="utf-8")
            with mock.patch.object(sys, "argv", ["analyze_palette_object_evidence.py",
                                                 str(lossy)]):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(analyzer.main(), 2)

    def test_duplicate_json_key_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            broken = Path(temp_dir) / "broken.json"
            broken.write_text('{"schema":7,"schema":7}', encoding="utf-8")
            with self.assertRaises(ValueError):
                analyzer.load(broken)


# ---------------------------------------------------------------------------
# 2026-09-17 只读对抗审计：D3（头块恒等式）/ D4（认证谓词）/ D5（会话镜像）/
# D8（逐位文档）的反例。每条都在修复前会**误通过**（chainComplete=True、CLI 0），
# 修复后必须被拒绝；反例本身也钉死了"拒绝理由具名且可见"。
# ---------------------------------------------------------------------------
class HeaderIdentityContracts(unittest.TestCase):
    """D3: 头块必须满足记录器自己的结算记账恒等式（不是启发式）。"""

    def assert_not_certified(self, result, marker):
        self.assertTrue(result["chainMissing"])
        self.assertFalse(result["chainComplete"])
        self.assertEqual(result["recovered"], [])
        self.assertTrue(any(entry.startswith(marker) for entry in result["missing"]),
                        "%s must be named in missing (got %s)" % (marker, result["missing"]))

    def test_terminal_emitted_must_equal_the_exported_terminals(self):
        # 反例 1：terminalEmitted="0"，而导出里含一条 Recovered 终态（误通过 ⇒ 现拒绝）。
        # 反例 2：terminalEmitted="9"（多报同样不可信）。
        for value in (0, 9):
            with self.subTest(terminalEmitted=value):
                result = analyzer.analyze(envelope(
                    closed_chain_events(), counter_overrides={"terminalEmitted": value}))
                self.assertEqual(result["exportedTerminalCount"], 1)
                self.assertEqual(result["terminalEmitted"], value)
                self.assert_not_certified(result,
                                          "terminalEmittedCounterVersusExportedTerminals")
                self.assertEqual(result["chains"][0]["conclusion"], "Uncovered")

    def test_closed_buckets_must_sum_to_terminal_emitted(self):
        # 反例：closedRecovered="1" + closedTableFull="6" 却只有 1 条终态。
        result = analyzer.analyze(envelope(
            closed_chain_events(),
            counter_overrides={"closedRecovered": 1, "closedTableFull": 6}))
        self.assert_not_certified(result, "closedCountersVersusTerminalEmitted")
        # 反例：closedObjectGone="5" 却只有 1 条终态。
        result = analyzer.analyze(envelope(
            closed_chain_events(), counter_overrides={"closedObjectGone": 5}))
        self.assert_not_certified(result, "closedCountersVersusTerminalEmitted")

    def test_closed_bucket_must_match_its_own_terminal_kind(self):
        # 总和凑巧对上了（1 == 1），但结算桶写错了**种类**；只有逐种类恒等式能判出。
        result = analyzer.analyze(envelope(
            closed_chain_events(),
            counter_overrides={"closedRecovered": 0, "closedObjectGone": 1}))
        self.assertEqual(result["closedCounterSum"], result["terminalEmitted"])
        self.assert_not_certified(result, "terminalKindCountersVersusExportedTerminals")

    def test_watch_count_is_bounded_by_the_recorder_table(self):
        # 反例：watchCount=2**63 / 10**9（无上界）；1025 也已越界。
        self.assertEqual(analyzer.K_WATCH_CAPACITY, 1024)
        for value in (10 ** 9, 2 ** 63, 1025):
            with self.subTest(watchCount=value):
                with self.assertRaises(ValueError):
                    analyzer.analyze(envelope(closed_chain_events(), watch_count=value))
        # 恰好等于容量仍是合法的（已饱和的）表读数。
        result = analyzer.analyze(envelope(closed_chain_events(), watch_count=1024))
        self.assertEqual(result["watchCapacity"], 1024)
        self.assertTrue(result["chainComplete"])

    def test_header_identities_hold_for_the_clean_fixture(self):
        result = analyzer.analyze(envelope(closed_chain_events()))
        self.assertEqual(result["terminalEmitted"], result["exportedTerminalCount"])
        self.assertEqual(result["closedCounterSum"], result["terminalEmitted"])
        self.assertEqual(result["closedCounters"]["closedRecovered"], 1)


class SameObjectRefusalContracts(unittest.TestCase):
    """D4: 认证谓词不得弱于记录器自身的 Recovered 语义（closedChain）。"""

    def analyzed(self, mutate):
        events = closed_chain_events()
        for row in events:
            mutate(row, events)
        result = analyzer.analyze(envelope(events))
        return result, result["chains"][0]

    def assert_refusal(self, result, chain, refusal):
        self.assertIn(refusal, chain["certificationRefusals"])
        self.assertFalse(chain["sameObjectCertified"])
        self.assertEqual(chain["conclusion"], "StageCompleteUncertified")
        self.assertFalse(chain["chainComplete"])
        self.assertEqual(result["recovered"], [])
        self.assertFalse(result["chainComplete"])

    def test_recovered_terminal_without_a_draw_source_is_refused(self):
        # 反例：终态 source = NoSource(0)（记录器 closedChain 要求 drawSource != None）。
        def mutate(row, rows):
            if row is rows[4]:
                row["data"][10] = "0"
        result, chain = self.analyzed(mutate)
        self.assertEqual(chain["terminalSource"], "NoSource")
        self.assert_refusal(result, chain, "drawSourceMissing")

    def test_recovered_terminal_cleared_by_native_override_is_refused(self):
        # 反例：终态 flags2 含 bit2（selectionClearedByNativeOverride）。
        def mutate(row, rows):
            if row is rows[4]:
                row["words32"][28] |= 4
        result, chain = self.analyzed(mutate)
        self.assertNotEqual(chain["terminalSource"], "NoSource")
        self.assert_refusal(result, chain, "selectionClearedAtDraw")

    def test_live_drawn_without_submit_is_refused(self):
        # 反例：live Drawn 的 sawSubmit=0（记录器要求 sawSubmit；该组合下它只会结算成非恢复）。
        def mutate(row, rows):
            if row is rows[3]:
                row["words32"][28] &= ~1
        result, chain = self.analyzed(mutate)
        self.assert_refusal(result, chain, "liveDrawnWithoutSubmit")

    def test_unknown_native_domain_with_a_value_is_refused(self):
        # 反例：nativeUnknown=1 且 nativeFrameTag=0x1234（"未知不是通配符"）。
        def mutate(row, rows):
            row["words32"][28] |= 16
            row["words32"][21] = 0x1234
        result, chain = self.analyzed(mutate)
        self.assertEqual(chain["unknownFrameDomains"], ["native"])
        self.assert_refusal(result, chain, "unknownFrameDomainWithValue")

    def test_unknown_manifest_domain_with_a_serial_is_refused(self):
        # 反例：manifestUnknown=1 且 manifestFrameSerial=9999。
        def mutate(row, rows):
            row["words32"][28] |= 8
            row["data"][9] = "9999"
        result, chain = self.analyzed(mutate)
        self.assertEqual(chain["unknownFrameDomains"], ["manifest"])
        self.assert_refusal(result, chain, "unknownFrameDomainWithValue")

    def test_clean_chain_still_carries_no_refusal(self):
        result = analyzer.analyze(envelope(closed_chain_events()))
        self.assertEqual(result["chains"][0]["certificationRefusals"], [])
        self.assertEqual(result["chains"][0]["terminalSource"], "ProducerSnapshot")
        self.assertTrue(result["chainComplete"])


class SessionMirrorContracts(unittest.TestCase):
    """D5: event.session 与 words32[10]/[29] 镜像必须是同一代。"""

    def test_session_generation_mirror_mismatch_is_a_hard_error(self):
        # 反例：bits[10]/[29]=12345 而 event.session=7（根会话也是 7）⇒ 曾经 chainComplete=True。
        events = closed_chain_events(session=7)
        for row in events:
            row["words32"][10] = 12345
            row["words32"][29] = 0
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(events, session="7"))

    def test_mirror_must_match_the_event_not_only_the_root_session(self):
        # 根会话与 event.session 一致，因此只有镜像校验能判出 lo/hi 被改坏。
        events = closed_chain_events(session=0x100000007)
        for row in events:
            row["words32"][29] = 0
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(events, session=str(0x100000007)))

    def test_consistent_mirror_is_decoded_and_still_certifies(self):
        events = closed_chain_events(session=0x100000007)
        result = analyzer.analyze(envelope(events, session=str(0x100000007)))
        self.assertEqual(result["chains"][0]["key"][4], str(0x100000007))
        self.assertTrue(result["chainComplete"])


class FrozenLayoutDocumentationContracts(unittest.TestCase):
    """D8: 逐位表必须与 war3_palette_object_evidence_sink.cpp 的实际写入集合一致。"""

    def test_docstring_carrier_table_matches_the_implemented_layout(self):
        doc = analyzer.__doc__
        self.assertIn("data[2]  key.lifecycleIdentity", doc)
        self.assertIn("data[3..8] reserved (0)", doc)
        for stale in ("NOT CARRIED by the frozen wire", "reserved (0): the A-chain-only",
                      "writes only", "is the 上级's call",
                      "The wire, however, has no slot for it"):
            self.assertNotIn(stale, doc, "stale docstring claim: %r" % stale)

    def test_reader_slot_declarations_match_the_sink_write_set(self):
        sink = (ROOT / "src/d3d9/war3/tools/war3_palette_object_evidence_sink.cpp").read_text(
            encoding="utf-8")
        encoder = sink.split("bool EncodePaletteObjectEvent(", 1)[1].split("\n}\n", 1)[0]
        data_written = {int(index) for index in re.findall(r"event\.data\[(\d+)\]\s*=",
                                                           encoder)}
        bits_written = {int(index) for index in re.findall(r"event\.bits\[(\d+)\]\s*=",
                                                           encoder)}
        # 每一个载体槽要么由唯一转换点写入，要么被读方声明为保留零 —— 两个方向都不允许出现
        # 未登记的槽位（这正是 docstring 曾经失真的地方）。
        self.assertEqual(data_written | set(analyzer.reserved_data_indices()), set(range(12)))
        self.assertEqual(bits_written | set(analyzer.reserved_bit_indices()), set(range(48)))
        # 2026-09-17 上级裁定 ⑦⑧（D2 生产写入侧）：转换点现在**确实**写两个版本 2 载体
        # （data[3] = windowSegment、words32[15] = identityProofKind），因此它们从"保留零"集合
        # 移入"已写入"集合。版本 1（默认参数）下读方仍把它们当保留零 —— 上一条 union 断言
        # （写入集合 | 保留集合 == 全槽位）因此必须继续成立，旧合同一位不放宽。
        self.assertIn(3, analyzer.reserved_data_indices(),
                      "版本 1 下 data[3] 必须仍是保留零（旧产物合同不放宽）")
        self.assertNotIn(3, analyzer.reserved_data_indices(2),
                         "版本 2 下 data[3] 是分段载体")
        self.assertIn(15, analyzer.reserved_bit_indices(),
                      "版本 1 下 words32[15] 必须仍是保留零（旧产物合同不放宽）")
        self.assertNotIn(15, analyzer.reserved_bit_indices(2),
                         "版本 2 下 words32[15] 是身份证明载体")
        # 2026-09-18 阶段 C：v4 起 data[4] 是**链型**载体（登记槽），故写入集合法增加 4。
        self.assertEqual(data_written, {0, 1, 2, 3, 4, 9, 10, 11})
        # 加强：data[4] 必须由**记录级 chainType** 写入（调用点不得给值），
        # 否则"登记了槽位"不等于"槽位语义被钉住"。
        assert 'event.data[4] = static_cast<uint32_t>(record.chainType);' in sink, \
            'data[4] must be written from the record-level chainType'
        # 2026-09-18 阶段 D（批次 2）：armed 标志必须与记录器初始化处在**同一同步域**。
        # 裸 bool + 无 acquire/release ⇒ 弱内存序下读者可能看到 armed==true 却看不到初始化。
        # 本断言锁住**三件事**：不退回裸 bool、写用 release、读用 acquire。
        assert "bool g_paletteObjectArmed" not in sink, \
            "g_paletteObjectArmed must not be a plain bool (stage D: single sync domain)"
        assert "std::atomic<bool> g_paletteObjectArmed{false};" in sink, \
            "g_paletteObjectArmed must be std::atomic<bool>"
        assert "g_paletteObjectArmed.store(true, std::memory_order_release);" in sink, \
            "arming must publish with release (so recorder init is visible with it)"
        assert "g_paletteObjectArmed.load(std::memory_order_acquire)" in sink, \
            "readers must acquire the armed flag"
        self.assertEqual(bits_written, {0, 1, 2, 5, 10, 12, 14, 15, 16, 18, 20, 21, 22, 23, 24, 25,
                                        26, 27, 28, 29, 30, 31, 32, 33})
        self.assertEqual(analyzer.LIFECYCLE_IDENTITY_SLOT, ("data", 2))
        self.assertNotIn(2, analyzer.reserved_data_indices())


class AuditCliContracts(unittest.TestCase):
    """审计反例在 CLI 层必须从 0 翻转为拒绝（2 或 ValueError）。"""

    def test_cli_refuses_the_new_counterexamples(self):
        mirror_events = closed_chain_events(session=7)
        for row in mirror_events:
            row["words32"][10] = 12345
            row["words32"][29] = 0
        no_source_events = closed_chain_events()
        no_source_events[4]["data"][10] = "0"
        cases = (("terminalEmitted0.json",
                  envelope(closed_chain_events(), counter_overrides={"terminalEmitted": 0}), 2),
                 ("noDrawSource.json", envelope(no_source_events), 2),
                 ("closedObjectGone5.json",
                  envelope(closed_chain_events(), counter_overrides={"closedObjectGone": 5}), 2))
        with tempfile.TemporaryDirectory() as temp_dir:
            for name, payload, expected in cases:
                with self.subTest(name=name):
                    path = Path(temp_dir) / name
                    path.write_text(json.dumps(payload), encoding="utf-8")
                    with mock.patch.object(sys, "argv",
                                           ["analyze_palette_object_evidence.py", str(path)]):
                        with contextlib.redirect_stdout(io.StringIO()):
                            self.assertEqual(analyzer.main(), expected)
            mirror = Path(temp_dir) / "mirror.json"
            mirror.write_text(json.dumps(envelope(mirror_events, session="7")), encoding="utf-8")
            with mock.patch.object(sys, "argv",
                                   ["analyze_palette_object_evidence.py", str(mirror)]):
                with contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaises(ValueError):
                        analyzer.main()


class FormatVersionContracts(unittest.TestCase):
    """2026-09-17 上级裁定 ⑦：冻结格式必须**显式**版本化。

    导出仍是 schema 7 + paletteObject 块。此前该块是一个未登记的根扩展 ⇒ 通用读方
    （analyze_frame_evidence）对每个真实导出都报 "root fields mismatch"，palette 读方只能
    把块投影掉再委托，history/watcher 入口干脆读不了。这里钉死修后合同：
      * 已登记扩展由**通用读方**自己判定版本；
      * 显式 version=1/2 被接受并如实上报；无 version 的冻结形状按旧合同读取（旧产物仍可读）；
      * 未知版本 / 未知形状 / 非整数版本一律 ValueError（显式拒绝，不是静默忽略）。
    """

    def test_generic_reader_accepts_the_registered_palette_extension(self):
        payload = envelope(closed_chain_events())
        analysis = frame_evidence.analyze(payload)
        self.assertTrue(analysis["schemaValid"])
        self.assertEqual(analysis["schema"], 7)
        self.assertEqual(analysis["extensions"], {"paletteObject": 1})
        result = analyzer.analyze(payload)
        self.assertEqual(result["formatVersion"], 1)
        self.assertEqual(result["extensions"], {"paletteObject": 1})
        self.assertFalse(result["windowSegmentCarried"])
        self.assertIsNone(result["windowSegmentSlot"])
        self.assertIsNone(result["windowSegments"])

    def test_explicit_version_2_is_accepted_and_reported(self):
        payload = envelope(segment_chain_events(segment=1), format_version=2)
        analysis = frame_evidence.analyze(payload)
        self.assertEqual(analysis["extensions"], {"paletteObject": 2})
        result = analyzer.analyze(payload)
        self.assertEqual(result["formatVersion"], 2)
        self.assertTrue(result["windowSegmentCarried"])
        self.assertEqual(result["windowSegmentSlot"], ["data", 3])
        self.assertEqual(result["identityProofSlot"], ["words32", 15])
        self.assertEqual(result["windowSegments"], [1])

    def test_explicit_version_1_matches_the_legacy_contract(self):
        legacy = analyzer.analyze(envelope(closed_chain_events()))
        explicit = analyzer.analyze(envelope(closed_chain_events(), format_version=1))
        self.assertEqual(legacy["formatVersion"], explicit["formatVersion"])
        self.assertEqual(legacy["recovered"], explicit["recovered"])
        self.assertEqual(legacy["chainComplete"], explicit["chainComplete"])
        self.assertTrue(explicit["chainComplete"])
        self.assertFalse(explicit["windowSegmentCarried"])

    def test_event_before_the_chain_head_must_block_completion(self):
        """锁住"链首之前有事件 ⇒ 不得认证"这条判据（2026-09-18 实机取证后的回归锁）。

        实机 `cpu-22056-58874345715-1.json` 里 64 条含首见的链中 **10 条**的实际阶段序列是
        `[Drawn, FirstSight, Enqueued, ...]` —— **链首之前就有 Drawn**。

        根因（`d3d9_device.cpp:23141-23142`）：生产采集点只对"本帧序列号仍然当前"的 draw-time
        缓存条目发 FirstSight/Enqueued，而 Drawn 由阴影 pass 记录 ⇒ **生产采集点的覆盖是阴影 pass
        所绘对象的真子集** ⇒ 对象可能先被绘制、之后的帧才首次进入该生产路径。

        因此 `FirstSight` 的真实语义是「首次进入**这条生产路径**」，不是「对象的首次观察」。
        **解析器判这 10 条 `stageObservationComplete=False` 是正确的** —— 本条用例就是防止
        后来者为了"让它们通过"而放宽 `drawnBeforeEnqueued`。
        """
        key0 = key()
        # 顺序刻意做成 Drawn 在前、Enqueued 在后（且无 FirstSight）——
        # 这正是实机那 10 条链的形状。
        events_ = [event(1, key0, frames(render_frame=100), 4, window_segment=1,
                         identity_weak=True, epoch_unknown=True, chain_sequence=1,
                         saw_draw=True, saw_submit=True, delta_frames=0),
                   event(2, key0, frames(render_frame=101), 3, window_segment=1,
                         identity_weak=True, epoch_unknown=True, chain_sequence=2,
                         saw_draw=True, saw_submit=True, delta_frames=0)]
        result = analyzer.analyze(envelope(events_, format_version=3))
        issues = [i for c in (result["chains"] or []) for i in (c.get("orderIssues") or [])]
        self.assertIn("drawnBeforeEnqueued", issues,
                      "an Enqueued after a Drawn in the same chain is an order violation")
        for c in (result["chains"] or []):
            self.assertFalse(c.get("stageObservationComplete"),
                             "a chain with an event before its head must never be certified")

    def test_first_sight_chain_anchor_delta_spans_multiple_frames(self):
        """P1 行为用例（2026-09-18 对抗性审计指出**单帧夹具永远看不到该冲突**）。

        首见链的 deltaFrames 契约是 本帧 − 链首帧。跨帧时必须 >0 且被判为**合法**；
        修复前读方的"锚点 delta"分支漏了 FirstSight，第 2..N 条会落入 elif 被要求 ==0，
        从而报 frozenZeroDeltaViolated。
        """
        key0 = key()
        # 首见链的锚点帧就是首见帧：两条事件的 firstRejectFrame 都写 100（承载"链首帧"），
        # 因而 delta 必须分别为 100-100=0 与 103-100=3。
        seq = [event(1, key0, frames(render_frame=100), 5, window_segment=1,
                    identity_weak=True, epoch_unknown=True,
                    first_reject_frame=100, delta_frames=0, chain_sequence=1),
               event(2, key0, frames(render_frame=103), 5, window_segment=1,
                    identity_weak=True, epoch_unknown=True,
                    first_reject_frame=100, delta_frames=3, chain_sequence=2)]
        # 夹具自洽性（2026-09-18 新增计数器不变量后暴露）：本用例构造了 **2 条** FirstSight 事件，
        # 因此必须声明相应的发射计数，否则违反「保留量 ≤ 发射量」。1 个条目、发射 2 次
        # 正是**修改前写方**的形状（同一对象跨帧重发）。
        result = analyzer.analyze(envelope(
            seq, format_version=3,
            counter_overrides={"firstSightInserted": 1, "firstSightEmitted": 2}))
        issues = [i for c in (result["chains"] or []) for i in (c.get("orderIssues") or [])]
        self.assertNotIn("frozenZeroDeltaViolated@2", issues,
                         "a re-observed FirstSight at a later frame must follow the anchor delta")
        self.assertFalse([i for i in issues if "deltaFrames" in i],
                         "anchor delta equality must hold: got %s" % issues)

    def test_first_sight_stage_is_rejected_wholesale_under_legacy_version(self):
        """P0 行为用例（2026-09-18 对抗性审计指出此前**只有字符串锚点、零行为用例**）。

        FirstSight 故意取 5；版本 1/2 的形状里它不可解释，读方必须**整份拒绝**，
        而不是加一个软标记后接受。修复前 STAGES 版本无关 ⇒ 只标记不拒绝；
        修复后按版本选表（LEGACY_STAGES）⇒ lookup 的 require 抛 ValueError。
        """
        sample = [event(1, key(), frames(), 5, identity_weak=True, epoch_unknown=True,
                       window_segment=1)]
        for version in (None, 1, 2):
            with self.subTest(version=version):
                with self.assertRaises(ValueError):
                    analyzer.analyze(envelope(sample, format_version=version))
        # 版本 3 下同一形状**合法**（它正是首见链的链首）⇒
        # 证明上面的拒绝是**版本门控**，不是"见到 stage=5 就一律拒绝"。
        result = analyzer.analyze(envelope(
            sample, format_version=3,
            counter_overrides={"firstSightInserted": 1, "firstSightEmitted": 1}))
        self.assertEqual(result["formatVersion"], 3)

    def test_first_sight_terminal_backfill_is_not_double_counted(self):
        """2026-09-18 阶段 C（K1：**我引入的缺陷**）：`FirstSight → CloseWindow` 必须被**接受**。

        终态回填把终态的 stage 写成该条目达到过的最高阶段，因此一条**只走到链首就关窗**的
        **合法**观察链会同时携带「现场 FirstSight（非终态）」与「回填 FirstSight 的终态」两条
        事件。我原先的保留量不变量把两者一起计数 ⇒ 误判为
        "export carries 2 FirstSight events but firstSightEmitted=1"，**整份导出被拒绝**。
        我的 98 个既有用例全都没抓到，因为它们从不构造这个形状。
        本用例是它的**正面见证**：修复前必然失败，修复后必须通过。
        """
        object_key = key()
        sample = [
            event(1, object_key, frames(render_frame=800), stage=5, first_reject_frame=800,
                  window_segment=1),
            event(2, object_key, frames(render_frame=900), stage=5, terminal=2,
                  first_reject_frame=800, delta_frames=100, window_segment=1),
        ]
        result = analyzer.analyze(envelope(
            sample, format_version=3,
            counter_overrides={"firstSightInserted": 1, "firstSightEmitted": 1}))
        self.assertEqual(result["formatVersion"], 3)

    def test_first_sight_terminal_backfill_without_emission_is_refused(self):
        """反向对照：有「回填 FirstSight 的终态」却 `firstSightEmitted=0` ⇒ 必须拒绝。

        证明新增的第二条检查（回填 ≤ 发射）**是活的** —— 我不是把判据放宽成了无检查。
        """
        object_key = key()
        sample = [
            event(1, object_key, frames(render_frame=900), stage=5, terminal=2,
                  first_reject_frame=900, delta_frames=0, window_segment=1),
        ]
        with self.assertRaises(ValueError) as ctx:
            analyzer.analyze(envelope(
                sample, format_version=3,
                counter_overrides={"firstSightInserted": 1, "firstSightEmitted": 0}))
        self.assertIn("backfilled", str(ctx.exception))

    def test_observation_closed_terminal_is_version_gated(self):
        """2026-09-18 阶段 C（探针 #1）：终态 7 (`ObservationClosed`) 必须**只在 v4** 被接受。

        round 6 把终态表改成 `TERMINALS_BY_VERSION`，但当时**没有用例**证明那条能力是活的 ——
        「一律接受 7」或「一律拒绝 7」的实现都能让既有用例通过。本用例同时钉住两侧。
        """
        object_key = key()
        first_sight = {"firstSightInserted": 1, "firstSightEmitted": 1}
        # v1 把 data[3] 当**保留零**，v2/v3/v4 才启用分段 ⇒ 夹具必须按版本给出合法载体，
        # 否则 v1 会因保留槽先被拒，本用例就测不到"终态表"这一条。
        for version, overrides, segment in ((1, {}, 0), (2, {}, 1),
                                           (3, first_sight, 1)):
            with self.subTest(version=version):
                sample = [event(1, object_key, frames(render_frame=900), stage=4,
                                terminal=7, first_reject_frame=900, delta_frames=0,
                                window_segment=segment)]
                # 断言**拒绝理由**：必须来自终态表，而不是顺带的某个计数不符。
                with self.assertRaisesRegex(ValueError, "palette terminal"):
                    analyzer.analyze(envelope(sample, format_version=version,
                                             counter_overrides=overrides))
        # v4 必须**接受**同一形状（否则"版本门控"就退化成"一律拒绝 7"）。
        v4_overrides = dict(first_sight)
        v4_overrides["closedObservationClosed"] = 0
        sample = [event(1, object_key, frames(render_frame=900), stage=4,
                        terminal=7, first_reject_frame=900, delta_frames=0,
                        window_segment=1)]
        result = analyzer.analyze(envelope(sample, format_version=4,
                                          counter_overrides=v4_overrides))
        self.assertEqual(result["formatVersion"], 4)

    def test_observation_chain_must_not_use_window_expired(self):
        """2026-09-18 P0-4（写方/读方同步）：观察链不得以 WindowExpired 结算，且规则按版本门控。

        独立复审指出：读方此前**没有** (Observation, WindowExpired) 规则 ⇒ 写方一旦把结算顺序改回
        旧形状（把 hitCount==0 && !sawServed 排在观察链之前），导出里就会出现「观察链冒充拒绝链的
        窗口过期」而读方无声接受。本用例钉住两侧（v4 必须报出、旧版本不得报出）。

        ⚠️ 实测澄清（我第一版断言写错了）：这些链型规则进入链的 **orderIssues（顾问性）**，
        `analyze()` **不会**因此抛错；真正致命的只有 `require(...)`。所以断言的对象是 orderIssues，
        不是 ValueError。这也意味着「旧标签回来」在导出结果里**可见**，但不会让导出被拒。
        """
        object_key = key()
        first_sight = {"firstSightInserted": 1, "firstSightEmitted": 1}

        def issues_for(stage, terminal, chain_type, version, overrides):
            sample = [event(1, object_key, frames(render_frame=900), stage=stage,
                            terminal=terminal, first_reject_frame=900, delta_frames=0,
                            chain_sequence=1, window_segment=1, chain_type=chain_type)]
            result = analyzer.analyze(envelope(sample, format_version=version,
                                               counter_overrides=overrides))
            chain = (result.get("chains") or [{}])[0]
            return list(chain.get("orderIssues") or [])

        # ① v4 + 观察链 + WindowExpired ⇒ 必须报出该规则
        seen = issues_for(5, 2, 1, 4, dict(first_sight))
        self.assertIn("observationChainMustNotUseWindowExpired", seen,
                      "观察链以 WindowExpired 结算必须被具名报出（旧结算顺序回归时它是唯一的判据）")
        # ② 对照：同一条观察链以 ObservationClosed(7) 结算 ⇒ **不得**出现该规则
        ok_overrides = dict(first_sight)
        ok_overrides["closedObservationClosed"] = 0
        seen_ok = issues_for(5, 7, 1, 4, ok_overrides)
        self.assertNotIn("observationChainMustNotUseWindowExpired", seen_ok,
                         "ObservationClosed 是观察链的合法结算，不得被本规则误报")
        # ③ 版本门控对照：v3 没有链型载体（解码恒给 RejectionRecovery）⇒ 同一数字形状不得被误报
        seen_v3 = issues_for(5, 2, None, 3, dict(first_sight))
        self.assertNotIn("observationChainMustNotUseWindowExpired", seen_v3,
                         "v1/v2/v3 没有链型载体 ⇒ 本规则必须按版本门控，不得误伤旧产物")

    def test_recovered_requires_a_real_reject_fact(self):
        """2026-09-18 P0-6（复审 1266d8db 的独立发现）：读方的 Recovered 谓词必须与写方对齐。

        写方 `closedChain` 要求 `hasRejectFact = (firstReason != NotChecked)` ——
        「Recovered = 拒绝**之后**被接住」，因此必须有真实拒绝事实。
        读方此前解码了 rejectReason 却从不使用 ⇒ 一条把拒绝理由伪造成 NotChecked/Unknown 的链
        仍会被认证为 Recovered、出口码 0（复审实测）。本用例钉住两侧。
        """
        object_key = key()
        first_sight = {"firstSightInserted": 1, "firstSightEmitted": 1}

        def refusals_for(reason_code):
            sample = [event(1, object_key, frames(render_frame=900), stage=4,
                            terminal=1, reason=reason_code, first_reject_frame=900,
                            delta_frames=0, chain_sequence=1, window_segment=1,
                            chain_type=0, saw_submit=True, saw_draw=True)]
            result = analyzer.analyze(envelope(sample, format_version=4,
                                               counter_overrides=dict(first_sight)))
            chain = (result.get("chains") or [{}])[0]
            return list(chain.get("certificationRefusals") or []), result

        # ① 无真实拒绝事实（NotChecked / Unknown）⇒ 必须具名拒绝认证
        # ① 无真实拒绝事实（NotChecked）⇒ 必须具名拒绝认证
        refusals, result = refusals_for(0xFE)
        self.assertIn("recoveredWithoutRejectFact", refusals,
                      "Recovered 但记 NotChecked ⇒ 必须拒绝认证（写方谓词要求 hasRejectFact）")
        self.assertNotIn(object_key, result.get("recovered") or [],
                         "未认证的链不得出现在 recovered 列表里")
        # ② 复审反例 1（**误拒**）：Unknown(0xFF) 是写方认可的**真实**拒绝事实（firstReason != NotChecked）
        #    ⇒ 不得被本规则拒绝。
        refusals_unknown, _ = refusals_for(0xFF)
        self.assertNotIn("recoveredWithoutRejectFact", refusals_unknown,
                         "Unknown(0xFF) 是写方认证的真实拒绝事实；把它当『无事实』会拒绝写方自己的产物")
        # ③ 复审反例 2（**漏报**）：链首 NotChecked + 仅终态塞 R1 的伪造链必须被拒绝
        #    （写方不可能产生该形状：链首恒带条目真实 firstReason）。
        mixed = [event(1, object_key, frames(render_frame=900), stage=5, terminal=0,
                       reason=0xFE, first_reject_frame=900, delta_frames=0,
                       chain_sequence=1, window_segment=1, chain_type=0),
                 event(2, object_key, frames(render_frame=900), stage=4, terminal=1,
                       reason=1, first_reject_frame=900, delta_frames=0,
                       chain_sequence=2, window_segment=1, chain_type=0,
                       saw_submit=True, saw_draw=True)]
        result_mixed = analyzer.analyze(envelope(mixed, format_version=4,
                                                 counter_overrides=dict(first_sight)))
        chain_mixed = (result_mixed.get("chains") or [{}])[0]
        self.assertIn("recoveredWithoutRejectFact",
                      list(chain_mixed.get("certificationRefusals") or []),
                      "只在一条事件里塞真实理由的伪造链必须被拒绝 —— 写方会把 firstReason 盖到**每一条**事件")
        # ④ 对照：整条链都带真实拒绝事实（R1）⇒ 不得被本规则拒绝
        refusals_ok, _ = refusals_for(1)
        self.assertNotIn("recoveredWithoutRejectFact", refusals_ok,
                         "真实拒绝事实不得被本规则误报")
        # ⑤ 复审 0224f6c8 反例 1（**误拒**）：写方 NoteReject 会用**本次调用的 reason** 覆盖该事件的
        #    rejectReason（evidence.h:462），而 hasRejectFact 用条目的 firstReason（:318）⇒ 一条
        #    firstReason=R1 的合法链可以含 NotChecked 事件。判据必须只看**链首**。
        mixed_real_head = [event(1, object_key, frames(render_frame=900), stage=1, terminal=0,
                                reason=1, first_reject_frame=900, delta_frames=0,
                                chain_sequence=1, window_segment=1, chain_type=0),
                           event(2, object_key, frames(render_frame=900), stage=4, terminal=1,
                                reason=0xFE, first_reject_frame=900, delta_frames=0,
                                chain_sequence=2, window_segment=1, chain_type=0,
                                saw_submit=True, saw_draw=True)]
        result_real = analyzer.analyze(envelope(mixed_real_head, format_version=4,
                                                counter_overrides=dict(first_sight)))
        chain_real = (result_real.get("chains") or [{}])[0]
        self.assertNotIn("recoveredWithoutRejectFact",
                         list(chain_real.get("certificationRefusals") or []),
                         "链首带真实拒绝事实、后续事件被 NoteReject 覆盖为 NotChecked 的链**不得**被误拒")

    def test_rejection_chain_must_not_carry_first_sight_version_gated(self):
        """2026-09-18 阶段 C（Q2）：拒绝恢复链不得携带 FirstSight，但规则**必须按版本门控**。

        v4 有链型载体 ⇒ 拒绝恢复链带 FirstSight 必须具名判错；
        v3 **认识** FirstSight 却没有链型载体（恒 RejectionRecovery）⇒
        **不得**把合法的 v3 观察链误判。两侧都钉住，否则"加了门控"与"没加"分不出来。
        """
        object_key = key()
        # v4：拒绝恢复链（chain_type=0）携带 FirstSight ⇒ 必须判错。
        sample = [event(1, object_key, frames(render_frame=900), stage=5, terminal=0,
                        window_segment=1, chain_type=0)]
        result = analyzer.analyze(envelope(sample, format_version=4,
            counter_overrides={"firstSightInserted": 1, "firstSightEmitted": 1}))
        self.assertIn("rejectionRecoveryChainMustNotCarryFirstSightStage",
                      json.dumps(result))
        # v3：同一形状（FirstSight 链首）**不得**被判错 —— v3 没有链型载体，
        # 因此它的 FirstSight 链首是合法的观察链。
        sample3 = [event(1, object_key, frames(render_frame=900), stage=5, terminal=0,
                         window_segment=1)]
        result3 = analyzer.analyze(envelope(sample3, format_version=3,
            counter_overrides={"firstSightInserted": 1, "firstSightEmitted": 1}))
        self.assertNotIn("rejectionRecoveryChainMustNotCarryFirstSightStage",
                         json.dumps(result3))

    def test_observation_chain_must_not_carry_rejected_stage(self):
        """2026-09-18 阶段 C（Q2 裁定）：观察链**不得**出现 Rejected 阶段。

        观察链由 NoteFirstSight 建立、**没有**拒绝事实；出现 Rejected 说明链型与
        **阶段形状**不符（伪造或读方串链）⇒ 必须具名判错。
        这是「Observation 永不使用 Recovered」在**阶段形状**上的镜像（那条拦终态，本条拦阶段）。
        """
        object_key = key()
        sample = [event(1, object_key, frames(render_frame=900), stage=5, terminal=0,
                        window_segment=1, chain_type=1),
                  event(2, object_key, frames(render_frame=901), stage=1,
                        reason=1, window_segment=1, chain_type=1)]
        result = analyzer.analyze(envelope(sample, format_version=4,
            counter_overrides={"firstSightInserted": 1, "firstSightEmitted": 1}))
        self.assertIn("observationChainMustNotCarryRejectedStage", json.dumps(result))

    def test_observation_chain_stage_set_is_a_set_not_a_fixed_count(self):
        """2026-09-18 P0-6（写方/读方同步）：观察链的完整阶段是**集合**判据，不是「恰好 3 个」。

        C1 裁定 S/E/D 对**两条链各自**记一份 ⇒ 「对象既被观察又被服务」时观察链**合法地**携带
        4 个阶段 {FirstSight, ServedCandidate, Enqueued, Drawn}。旧读方硬编码「首见链恰好 3 个」
        ⇒ 会把**合法**导出判成 stageObservationComplete=False（并波及 chainComplete 与退出码）。
        """
        overrides = {"firstSightInserted": 1, "firstSightEmitted": 1,
                     "closedObservationClosed": 1}
        # ① 合法 4 阶段（含 Served）⇒ 必须被接受
        result4 = analyzer.analyze(envelope(observation_chain_events(stages=[2, 3, 4]),
                                            format_version=4,
                                            counter_overrides=dict(overrides)))
        chain4 = result4["chains"][0]
        self.assertTrue(chain4["stageObservationComplete"],
                        "观察链携带 ServedCandidate 是 C1 之后的**合法**形状，不得被判不完整")
        self.assertEqual(chain4["missingStages"], [])
        # ② 合法 3 阶段（该对象从未被服务）⇒ 也必须被接受（确保不是「只认 4」）
        result3 = analyzer.analyze(envelope(observation_chain_events(stages=[3, 4]),
                                            format_version=4,
                                            counter_overrides=dict(overrides)))
        self.assertTrue(result3["chains"][0]["stageObservationComplete"])
        # ③ **不是放宽**：缺 Enqueued（FirstSight + Served + Drawn）仍然必须失败
        resultm = analyzer.analyze(envelope(observation_chain_events(stages=[2, 4]),
                                            format_version=4,
                                            counter_overrides=dict(overrides)))
        self.assertFalse(resultm["chains"][0]["stageObservationComplete"],
                         "缺少 Enqueued 的观察链仍然不完整 —— 新判据不得成为放宽")

    def test_rejection_chain_must_not_use_observation_closed(self):
        """2026-09-18 阶段 C（Q2）：**镜像规则** —— 拒绝恢复链不得使用 ObservationClosed。

        与 test_observation_chain_must_not_use_recovered 合起来把「终态 × 链型」矩阵封口。
        ObservationClosed 的含义是「**观察**已结算」；拒绝恢复链没有观察语义。
        """
        object_key = key()
        # v4：拒绝恢复链（chain_type=0）以 ObservationClosed(7) 收尾 ⇒ 必须具名判错。
        sample = [event(1, object_key, frames(render_frame=900), stage=1, reason=1,
                        window_segment=1, chain_type=0),
                  event(2, object_key, frames(render_frame=901), stage=4, terminal=7,
                        window_segment=1, chain_type=0)]
        result = analyzer.analyze(envelope(sample, format_version=4,
            counter_overrides={"closedObservationClosed": 1}))
        self.assertIn("rejectionRecoveryChainMustNotUseObservationClosed",
                      json.dumps(result))

    def test_observation_chain_must_not_use_recovered(self):
        """2026-09-18 阶段 C（Q2 裁定）：**Observation 永不使用 Recovered**。

        Recovered 的含义是「拒绝之后被接住」，必须有真实拒绝事实；观察链没有拒绝事实，
        因此链型与终态不符时**必须具名判错**，不得静默接受。
        """
        object_key = key()
        sample = [event(1, object_key, frames(render_frame=900), stage=5, terminal=0,
                        window_segment=1, chain_type=1),
                  event(2, object_key, frames(render_frame=901), stage=4, terminal=1,
                        window_segment=1, chain_type=1)]
        result = analyzer.analyze(envelope(sample, format_version=4,
            counter_overrides={"firstSightInserted": 1, "firstSightEmitted": 1,
                               "closedRecovered": 1}))
        self.assertIn("observationChainMustNotUseRecovered", json.dumps(result))

    def test_chain_type_slot_stays_reserved_before_v4(self):
        """2026-09-18 阶段 C（步骤⑦的读方半边）：`data[4]` 的**版本相关性**必须成立。

        v4 把 `data[4]` 从保留零改为链型载体；v1/v2/v3 **必须仍然把它当保留零**，
        否则旧版本产物会开始接受一个它们从未产生过的槽位（版本合同被悄悄放宽）。
        本用例钉住"旧版本仍拒绝"这一半。
        """
        object_key = key()
        for version, segment in ((1, 0), (2, 1), (3, 1)):
            with self.subTest(version=version):
                overrides = ({"firstSightInserted": 1, "firstSightEmitted": 1}
                             if version >= analyzer.PALETTE_OBJECT_FIRST_SIGHT_VERSION else {})
                sample = [event(1, object_key, frames(render_frame=900), stage=4,
                                first_reject_frame=900, delta_frames=0,
                                window_segment=segment, chain_type=1)]
                with self.assertRaisesRegex(ValueError, "reserved"):
                    analyzer.analyze(envelope(sample, format_version=version,
                                             counter_overrides=overrides))
        # v4：同一形状（data[4]=1 = Observation）**必须被接受** ——
        # 否则"版本门控"就退化成"一律拒绝 data[4]"。
        sample = [event(1, object_key, frames(render_frame=900), stage=4,
                        first_reject_frame=900, delta_frames=0,
                        window_segment=1, chain_type=1)]
        result = analyzer.analyze(envelope(sample, format_version=4,
            counter_overrides={"firstSightInserted": 1, "firstSightEmitted": 1,
                               "closedObservationClosed": 0}))
        self.assertEqual(result["formatVersion"], 4)

    def test_explicit_version_3_is_registered_and_accepted_by_both_readers(self):
        """2026-09-18 更正：v3（分段 + 正常观察链）必须是**已登记**版本。

        此前读方只注册 {1,2}，而写方会按 firstSightUsed() 发 v3 ⇒ 实机一旦走首见链，
        两个读方都会拒绝**整份**导出。本用例是那条缺口的端到端见证：v3 必须被接受。
        """
        payload = envelope(segment_chain_events(segment=1), format_version=3)
        analysis = frame_evidence.analyze(payload)
        self.assertEqual(analysis["extensions"], {"paletteObject": 3})
        result = analyzer.analyze(payload)
        self.assertEqual(result["formatVersion"], 3)
        self.assertTrue(result["windowSegmentCarried"])

    def test_unknown_versions_are_rejected_by_both_readers(self):
        # 2026-09-18 更正：3 已是**已登记**版本（v3 = 分段 + 正常观察链），故不在非法列表中。
        # 2026-09-18 阶段 C：4 也已成为**已登记**版本（v4 = 链型 + ObservationClosed），
        # 因此本用例改用**真正未知**的 5。**要求未变**：未知版本必须被两个读方整份拒绝。
        for version in (0, 5, 7, 99, -1, "2", 2.0, True):
            with self.subTest(version=version):
                payload = envelope(closed_chain_events(), format_version=version)
                with self.assertRaises(ValueError):
                    frame_evidence.analyze(payload)
                with self.assertRaises(ValueError):
                    analyzer.analyze(payload)

    def test_unknown_block_shape_is_rejected(self):
        payload = envelope(closed_chain_events(), block_extra={"invented": 1})
        with self.assertRaises(ValueError):
            frame_evidence.analyze(payload)
        with self.assertRaises(ValueError):
            analyzer.analyze(payload)
        payload = envelope(closed_chain_events(), format_version=2,
                           block_extra={"invented": 1})
        with self.assertRaises(ValueError):
            frame_evidence.analyze(payload)
        with self.assertRaises(ValueError):
            analyzer.analyze(payload)

    def test_unversioned_legacy_block_stays_readable_but_reserves_the_v2_slots(self):
        # 旧合同不放宽：没有 version 字段时 data[3]/words32[15] 仍是保留零。
        events = closed_chain_events()
        events[0]["data"][3] = "1"
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(events))
        events = closed_chain_events()
        events[0]["words32"][15] = 1
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(events))
        # 版本 2 声明的槽位集合只在版本 2 生效。
        self.assertIn(3, analyzer.reserved_data_indices(1))
        self.assertNotIn(3, analyzer.reserved_data_indices(2))
        self.assertIn(15, analyzer.reserved_bit_indices(1))
        self.assertNotIn(15, analyzer.reserved_bit_indices(2))

    def test_palette_reader_delegates_the_whole_root_without_projection(self):
        source = (ROOT / "AutoTest/analyze_palette_object_evidence.py").read_text(
            encoding="utf-8")
        self.assertNotIn("if name!='paletteObject'", source,
                         "投影掉 paletteObject 再委托的绕行必须消失")
        self.assertIn("frame_analysis=frame_evidence.analyze(d)", source,
                      "palette 读方必须把**整个根**交给通用读方（统一接入）")

    def test_history_and_watcher_entries_share_the_single_root_reader(self):
        for name in ("analyze_frame_history.py", "frame_history_watch.py"):
            text = (ROOT / "AutoTest" / name).read_text(encoding="utf-8")
            self.assertIn("from analyze_frame_evidence import load,analyze", text, name)
            self.assertNotIn("paletteObject", text,
                             "%s 不得自带一份扩展/根校验（必须走统一入口）" % name)
        # 统一入口现在真的读得了真实形状的 palette 导出（修前是 root fields mismatch）。
        payload = envelope(segment_chain_events(segment=2, sequence_base=1),
                           format_version=2)
        self.assertEqual(frame_evidence.analyze(payload)["extensions"],
                         {"paletteObject": 2})


class WindowSegmentContracts(unittest.TestCase):
    """2026-09-17 上级裁定 ⑧：窗口与 Reset 必须可识别分段。

    清表 ≠ 分段：同会话、同地图、deviceEpoch 未知时，Reset 前后的记录可以八元组完全相同。
    修前读方把两条链合成一条 —— 两个终态 ⇒ 整份导出被拒；一条未闭合 + 一条完整 ⇒ 误判成
    "一条链自身损坏"（objectCount 1 + 一串假 order issue）。版本 2 的分段标签是**记录级**
    载体，必须进入分组键；旧格式没有标签，必须保持 fail-closed，绝不按顺序启发式猜分段。
    """

    def two_windows(self, first_segment, second_segment, **kwargs):
        first = segment_chain_events(part=1, segment=first_segment, sequence_base=1,
                                     frame_base=500, chain_sequence_base=1, **kwargs)
        second = segment_chain_events(part=1, segment=second_segment, sequence_base=6,
                                      frame_base=700, chain_sequence_base=1, **kwargs)
        return envelope(first + second, format_version=2)

    def test_two_windows_with_the_same_key_split_into_two_chains(self):
        result = analyzer.analyze(self.two_windows(1, 2))
        self.assertEqual(result["formatVersion"], 2)
        self.assertEqual(result["objectCount"], 2,
                         "同键不同窗口的两条链不得被合成一条")
        self.assertEqual(result["windowSegments"], [1, 2])
        self.assertEqual([chain["windowSegment"] for chain in result["chains"]], [1, 2])
        self.assertEqual(len(result["recovered"]), 2)
        self.assertTrue(result["coverageComplete"])
        self.assertTrue(result["chainComplete"])
        self.assertEqual(result["missing"], [])
        for chain in result["chains"]:
            self.assertEqual(chain["conclusion"], "Recovered")
            self.assertTrue(chain["sameObjectCertified"])
            self.assertTrue(chain["identityProven"])
            self.assertEqual(chain["certificationRefusals"], [])
            self.assertEqual(chain["orderIssues"], [])
            self.assertEqual(len(chain["key"]), 8, "分组键仍必须如实报告八元组")

    def test_version_1_same_key_two_windows_is_refused_not_guessed(self):
        first = closed_chain_events(part=1, sequence_base=1, frame_base=500)
        second = closed_chain_events(part=1, sequence_base=6, frame_base=700)
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(first + second))
        # 同形状但不同键的对照组必须仍然认证（证明拒绝只来自"同键不分段"）。
        control = analyzer.analyze(envelope(closed_chain_events(part=1, sequence_base=1,
                                                                frame_base=500)
                                            + closed_chain_events(part=9, sequence_base=6,
                                                                  frame_base=700)))
        self.assertEqual(control["objectCount"], 2)
        self.assertTrue(control["chainComplete"])

    def test_version_1_merged_windows_are_misjudged_as_one_corrupt_chain(self):
        first = closed_chain_events(part=1, sequence_base=1, frame_base=500)
        first[-1]["words32"][18] = 0  # 窗口 1 未结算（Reset 清表），只有半条链
        result = analyzer.analyze(envelope(first + closed_chain_events(
            part=1, sequence_base=6, frame_base=700)))
        self.assertEqual(result["objectCount"], 1,
                         "旧合同下这两个窗口**只能**被合并（没有分段量可依据）")
        self.assertFalse(result["chainComplete"])
        self.assertEqual(result["recovered"], [])
        chain = result["chains"][0]
        self.assertEqual(chain["conclusion"], "Uncovered")
        self.assertTrue(any(issue.startswith("chainSequenceNotStrictlyIncreasing@")
                            for issue in chain["orderIssues"]),
                        "合并后的假 order issue 正是修前的误判（分段缺失的可证后果）")

    def test_same_segment_never_splits(self):
        with self.assertRaises(ValueError):
            analyzer.analyze(self.two_windows(1, 1))
        result = analyzer.analyze(envelope(segment_chain_events(segment=1), format_version=2))
        self.assertEqual(result["objectCount"], 1)

    def test_version_2_requires_a_segment_on_every_record(self):
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(segment_chain_events(segment=0), format_version=2))

    def test_segments_must_be_non_decreasing(self):
        with self.assertRaises(ValueError):
            analyzer.analyze(self.two_windows(2, 1))

    def test_group_key_is_the_eight_tuple_plus_the_segment(self):
        decoded = analyzer.decode_event(segment_chain_events(segment=3)[0], 2)
        identity = analyzer.chain_identity(decoded)
        group = analyzer.chain_group_identity(decoded)
        self.assertEqual(len(identity), 8)
        self.assertEqual(group[:8], identity)
        self.assertEqual(group[-1], 3)
        legacy = analyzer.decode_event(closed_chain_events()[0])
        self.assertIsNone(analyzer.chain_group_identity(legacy)[-1])

    def test_segment_is_read_from_the_record_not_from_the_header(self):
        source = (ROOT / "AutoTest/analyze_palette_object_evidence.py").read_text(
            encoding="utf-8")
        self.assertIn("return data[WINDOW_SEGMENT_SLOT[1]]", source,
                      "分段量必须是**记录级**载体（冻结后摘要不再变化的前提）")
        self.assertIn("{event[WINDOW_SEGMENT_FIELD] for event in events}", source,
                      "导出级分段摘要必须由记录自身导出，不得读头块或按顺序推断")
        self.assertIn("grouped.setdefault(chain_group_identity(event),[])", source,
                      "分组必须包含分段量")


class IdentityProofContracts(unittest.TestCase):
    """2026-09-17 上级裁定 ⑦：**字段存在 ≠ 身份已证明**。

    版本 1 如实报告 identityBasis='recorderFlagClaimOnly' + identityProven=False（旧合同判定
    语义不变，但报告不再暗示身份已被证明）。版本 2 要求**载明**登记在案的身份证明种类，
    并且携带的实例生命周期身份非零、identityWeak/epochUnknown 为假；否则具名拒绝
    identityNotProven / identityValueMissing —— identityWeak 的 0 本身永远不足以认证。
    """

    def test_version_1_reports_the_flag_claim_as_unproven(self):
        result = analyzer.analyze(envelope(closed_chain_events()))
        chain = result["chains"][0]
        self.assertTrue(chain["sameObjectCertified"],
                        "旧合同的判定语义不变（版本 1 仍是 flags 合同）")
        self.assertEqual(chain["identityBasis"], "recorderFlagClaimOnly")
        self.assertFalse(chain["identityProven"],
                         "只有 identityWeak=0 时，报告必须明确写出身份**未**被证明")
        self.assertIsNone(chain["windowSegment"])

    def test_version_2_requires_a_carried_proof_kind(self):
        rows = segment_chain_events(identity_proof=0)
        result = analyzer.analyze(envelope(rows, format_version=2))
        chain = result["chains"][0]
        self.assertEqual(chain["identityBasis"], "wireCarriedInstanceLifecycleProof")
        self.assertFalse(chain["identityProven"])
        self.assertIn("identityNotProven", chain["certificationRefusals"])
        self.assertFalse(chain["sameObjectCertified"])
        self.assertEqual(chain["conclusion"], "StageCompleteUncertified")
        self.assertEqual(result["recovered"], [])

    def test_version_2_proof_without_a_non_zero_identity_is_refused(self):
        rows = segment_chain_events(lifecycle=0, identity_proof=1)
        result = analyzer.analyze(envelope(rows, format_version=2))
        chain = result["chains"][0]
        self.assertIn("identityValueMissing", chain["certificationRefusals"])
        self.assertFalse(chain["identityProven"])
        self.assertEqual(result["recovered"], [])

    def test_version_2_with_a_carried_proof_is_certified(self):
        result = analyzer.analyze(envelope(segment_chain_events(lifecycle=0x4242,
                                                                identity_proof=1),
                                           format_version=2))
        chain = result["chains"][0]
        self.assertEqual(chain["certificationRefusals"], [])
        self.assertTrue(chain["identityProven"])
        self.assertTrue(chain["sameObjectCertified"])
        self.assertTrue(result["chainComplete"])
        self.assertEqual(result["lifecycleIdentityCarried"], True)

    def test_version_2_weak_flags_still_block_certification(self):
        result = analyzer.analyze(envelope(
            segment_chain_events(identity_weak=True, identity_proof=1), format_version=2))
        chain = result["chains"][0]
        self.assertIn("identityWeak", chain["certificationRefusals"])
        self.assertFalse(chain["identityProven"])
        self.assertEqual(result["recovered"], [])

    def test_unknown_identity_proof_kind_is_rejected(self):
        with self.assertRaises(ValueError):
            analyzer.analyze(envelope(segment_chain_events(identity_proof=7),
                                      format_version=2))
        self.assertEqual(sorted(analyzer.IDENTITY_PROOF_KINDS), [0, 1])

    def test_version_2_cli_exit_code_follows_the_segmented_verdict(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            cases = (("segmented-certified.json",
                      envelope(segment_chain_events(segment=1), format_version=2), 0),
                     ("segmented-unproven.json",
                      envelope(segment_chain_events(segment=1, identity_proof=0),
                               format_version=2), 2),
                     ("legacy-unsegmented.json",
                      envelope(closed_chain_events()), 0))
            for name, payload, expected in cases:
                with self.subTest(name=name):
                    path = Path(temp_dir) / name
                    path.write_text(json.dumps(payload), encoding="utf-8")
                    with mock.patch.object(sys, "argv",
                                           ["analyze_palette_object_evidence.py", str(path)]):
                        with contextlib.redirect_stdout(io.StringIO()):
                            self.assertEqual(analyzer.main(), expected)

class LiveFoundEvidenceGapOrdering(unittest.TestCase):
    """2026-09-19 **实机发现**的锁定（evidence3 链 key=data[0]="857217852"）。

    实机该链的真实形状：FirstSight → **4 条 live Drawn（sawSubmit=false）** → 首次 Enqueued → … → ObservationClosed。
    读方给出 orderIssues=['drawnBeforeEnqueued'] 与 refusal 'liveDrawnWithoutSubmit'（链级**首次出现**判据，不按尝试分组）。

    为什么这**不是**写方缺陷：`NoteDrawn` 只要求条目存在，而 `sawSubmit` 由 `NoteEnqueued`/`PrepareStage` 置位 ⇒
    **缺提交事实的 live Drawn 是可写出的**（预算丢失场景；同一次实机运行 droppedPerFrame=2756 / droppedPerSession=39300）。
    读方**必须**拒绝认证（fail-visible）。把该形状钉成期望，防止将来把 'drawnBeforeEnqueued' 误判为写方 bug 而放宽判据。
    """

    # 实机链的阶段顺序（含「首条 Drawn 早于首次 Enqueued」这一事实）
    LIVE_SHAPE = [5, 4, 4, 3, 4, 4]
    ORDERED_SHAPE = [5, 3, 4, 4, 4, 4]

    def fixture(self, shape, saw_submit_for_live_draw):
        object_key = key(part=857217)
        base = 1535
        rows = []
        for idx, stage in enumerate(shape):
            is_terminal = idx == len(shape) - 1
            saw_submit = True
            if stage == 4 and idx < shape.index(3):
                saw_submit = saw_submit_for_live_draw   # 首次 Enqueued 之前的 live Drawn
            e = event(idx + 1, object_key,
                      frames(render_frame=base + idx, manifest=11, revision=22, record=33,
                             native=44, manifest_unknown=False, native_unknown=False),
                      stage=stage, first_reject_frame=base, delta_frames=0,
                      window_segment=1, chain_type=1, saw_submit=saw_submit,
                      terminal=(7 if is_terminal else 0),
                      source=(2 if is_terminal else 0))
            e["words32"][23] = idx + 1   # chainSequence 必须严格 1,2,3…
            rows.append(e)
        return rows

    def test_live_draw_before_enqueue_is_flagged_and_refused(self):
        chain = analyzer.analyze(envelope(self.fixture(self.LIVE_SHAPE, False), format_version=4,
                                          counter_overrides={"firstSightEmitted": 1,
                                                             "firstSightInserted": 1}))["chains"][0]
        self.assertIn("drawnBeforeEnqueued", chain["orderIssues"],
                      "the live shape (live Drawn before the first Enqueued) must be flagged")
        self.assertIn("liveDrawnWithoutSubmit", chain["certificationRefusals"],
                      "a live Drawn with sawSubmit=false must refuse certification")
        self.assertFalse(chain["sameObjectCertified"],
                         "an evidence-gap chain must never be certified")

    def test_control_ordered_chain_has_no_order_issue(self):
        chain = analyzer.analyze(envelope(self.fixture(self.ORDERED_SHAPE, True), format_version=4,
                                          counter_overrides={"firstSightEmitted": 1,
                                                             "firstSightInserted": 1}))["chains"][0]
        self.assertNotIn("drawnBeforeEnqueued", chain["orderIssues"],
                         "with Enqueued first the ordering rule must stay silent (control)")
        self.assertNotIn("liveDrawnWithoutSubmit", chain["certificationRefusals"],
                         "control: every live Drawn carries sawSubmit")

if __name__ == "__main__":
    unittest.main()
