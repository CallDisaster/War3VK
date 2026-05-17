"""Phase 7.49 publish-probe 分析：按 CombinedHash FROZEN/NON-FROZEN 窗口拆分，
对比 publish call / trusted hit / record frameTag same-run / live vs record delta。
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


def _ks(ev: dict, *names: str):
    ks = ev.get("keyStats", {}) or {}
    for n in names:
        if n in ks:
            return ks[n]
        if n in ev:
            return ev[n]
    return None


def main():
    if len(sys.argv) < 2:
        print("usage: _analyze_phase749.py <trace.jsonl>")
        return 2
    trace_path = Path(sys.argv[1])
    if not trace_path.exists():
        print(f"[fatal] trace not found: {trace_path}")
        return 2

    events = []
    with trace_path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except Exception:
                continue
            if obj.get("type") != "shadowPoseFullTraceFrame":
                continue
            events.append(obj)

    print(f"[stats] events={len(events)}")
    if len(events) < 3:
        return 1

    # 快速扫：新字段是否在 keyStats 里
    ks0 = events[-1].get("keyStats", {})
    probe_fields = [
        "publishCallCumulative",
        "publishTrustedHitCumulative",
        "publishRawFallbackCumulative",
        "publishRejectedNoTrustedCumulative",
        "publishRecordFrameTagSameRunMax",
        "publishRecordFrameTagCurrentSameRun",
        "publishRecordFrameTagLast",
        "publishLiveGamePaletteFrameTagLast",
        "publishLiveGamePaletteFrameTagMin",
        "publishLiveGamePaletteFrameTagMax",
        "publishRecordFrameTagMin",
        "publishRecordFrameTagMax",
        "publishRecordFrameTagBehindLiveMaxDelta",
        "publishRecordFrameTagEqualsLiveCount",
        "publishRecordFrameTagBehindLiveCount",
        "publishRecordFrameTagAheadLiveCount",
    ]
    print("\n=== probe field presence check (last event) ===")
    all_present = True
    for k in probe_fields:
        v = ks0.get(k)
        if v is None:
            print(f"  MISSING: {k}")
            all_present = False
        else:
            print(f"  {k} = {v}")
    if not all_present:
        print("\n  WARN: 某些字段缺失 —— probe 可能没贯穿完整，但后续分析尽量进行。")

    # 累计 snapshot：最后一帧的累计值
    last_call = _ks(events[-1], "publishCallCumulative") or 0
    last_trusted = _ks(events[-1], "publishTrustedHitCumulative") or 0
    last_raw = _ks(events[-1], "publishRawFallbackCumulative") or 0
    last_rejected = _ks(events[-1], "publishRejectedNoTrustedCumulative") or 0
    first_call = _ks(events[0], "publishCallCumulative") or 0
    first_trusted = _ks(events[0], "publishTrustedHitCumulative") or 0
    first_raw = _ks(events[0], "publishRawFallbackCumulative") or 0
    total_call = last_call - first_call
    total_trusted = last_trusted - first_trusted
    total_raw = last_raw - first_raw
    print(f"\n=== trace window totals ===")
    print(f"  Publish() called:    {total_call}")
    print(f"  Trusted hit:         {total_trusted}  "
          f"({100*total_trusted/max(1,total_call):.1f}%)")
    print(f"  Raw fallback:        {total_raw}  "
          f"({100*total_raw/max(1,total_call):.1f}%)")
    print(f"  Rejected no-trusted: {last_rejected - 0}")

    # 按 CombinedHash 分段
    segments = []
    cur_start = 0
    cur_combined = _ks(events[0], "semanticSceneSubmittedSkinnedPaletteCombinedHash")
    for i in range(1, len(events)):
        combined = _ks(events[i], "semanticSceneSubmittedSkinnedPaletteCombinedHash")
        if combined != cur_combined:
            segments.append((cur_start, i - cur_start, True if i - cur_start >= 3 else False, cur_combined))
            cur_start = i
            cur_combined = combined
    segments.append((cur_start, len(events) - cur_start,
                     True if len(events) - cur_start >= 3 else False,
                     cur_combined))

    # 这里的 frozen 判定：同一 CombinedHash 连续 >=3 trace frame
    # 但我们真正判定要基于：combined 是否在 segment 内持续不变
    frozen_segs = []
    nonfrozen_segs = []
    for s in segments:
        start, length, _, comb = s
        # 在 segment 内部（除第一个 event），combined 都等于 comb，即持续 frozen。
        # 只有 length >=3 才算真正的 frozen run
        if length >= 3:
            frozen_segs.append(s)
        else:
            nonfrozen_segs.append(s)

    def agg(segs, name):
        total_call = 0
        total_trust = 0
        total_raw = 0
        total_events = 0
        total_rec_behind_live_max = 0
        rec_run_max_obs = 0
        for start, length, _, comb in segs:
            if length < 2:
                continue
            first = events[start]
            last = events[start + length - 1]
            dc = (_ks(last, "publishCallCumulative") or 0) - (_ks(first, "publishCallCumulative") or 0)
            dt = (_ks(last, "publishTrustedHitCumulative") or 0) - (_ks(first, "publishTrustedHitCumulative") or 0)
            dr = (_ks(last, "publishRawFallbackCumulative") or 0) - (_ks(first, "publishRawFallbackCumulative") or 0)
            total_call += dc
            total_trust += dt
            total_raw += dr
            total_events += length - 1  # 减去首帧（首帧没参与 segment 内 delta）
            behind = _ks(last, "publishRecordFrameTagBehindLiveMaxDelta") or 0
            if behind > total_rec_behind_live_max:
                total_rec_behind_live_max = behind
            rrm = _ks(last, "publishRecordFrameTagSameRunMax") or 0
            if rrm > rec_run_max_obs:
                rec_run_max_obs = rrm
        print(f"\n=== {name} ({len(segs)} segments, {total_events} intra-segment intervals) ===")
        if total_events > 0:
            print(f"  avg Publish()/interval:      {total_call/total_events:.1f}")
            print(f"  avg TrustedHit/interval:     {total_trust/total_events:.1f}")
            print(f"  avg RawFallback/interval:    {total_raw/total_events:.1f}")
            if total_call > 0:
                print(f"  TrustedHit%:                 {100*total_trust/total_call:.1f}%")
                print(f"  RawFallback%:                {100*total_raw/total_call:.1f}%")
        print(f"  observed max publishRecordFrameTagSameRunMax: {rec_run_max_obs}")
        print(f"  observed max publishRecordFrameTagBehindLiveMaxDelta: {total_rec_behind_live_max}")

    agg(frozen_segs, "FROZEN segments (CombinedHash unchanged >=3 frames)")
    agg(nonfrozen_segs, "NON-FROZEN segments")

    # 整体断言
    print("\n=== VERDICT ===")
    if not frozen_segs:
        print("  AutoTest 场景未复现 FROZEN window — 本轮 probe 只验证字段完整性，")
        print("  真正的诊断需要用户在真卡顿场景录制的 trace。")
        return 0

    last_rec_run = _ks(events[-1], "publishRecordFrameTagSameRunMax") or 0
    last_behind_max = _ks(events[-1], "publishRecordFrameTagBehindLiveMaxDelta") or 0
    print(f"  全程 publishRecordFrameTagSameRunMax = {last_rec_run}")
    print(f"  全程 publishRecordFrameTagBehindLiveMaxDelta = {last_behind_max}")

    # 收集每 FROZEN segment 的 call/trust delta
    print("\n  Per-FROZEN-segment details:")
    for start, length, _, comb in frozen_segs[:20]:
        first = events[start]
        last = events[start + length - 1]
        dc = (_ks(last, "publishCallCumulative") or 0) - (_ks(first, "publishCallCumulative") or 0)
        dt = (_ks(last, "publishTrustedHitCumulative") or 0) - (_ks(first, "publishTrustedHitCumulative") or 0)
        dr = (_ks(last, "publishRawFallbackCumulative") or 0) - (_ks(first, "publishRawFallbackCumulative") or 0)
        ch = str(comb or "")[-8:]
        print(f"    [seg start={start} len={length} comb=...{ch}]  "
              f"publishCall+={dc}  trustedHit+={dt}  rawFallback+={dr}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
