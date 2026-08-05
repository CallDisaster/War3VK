"""Phase 7.48 后续：在 CombinedHash frozen 窗口内，看 writer/producer 是否也静默。

目标：判断冻结是 "submit 端复用缓存"（producer 仍跑）还是
      "producer 真的不跑" (dt gate 或别的机制导致引擎不生产 palette)
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
    trace_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
        r"E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_13_01_42_25.jsonl")
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

    # 按 CombinedHash 分段
    segments = []  # list of (start, length, frozen_bool)
    cur_start = 0
    cur_combined = _ks(events[0], "semanticSceneSubmittedSkinnedPaletteCombinedHash")
    cur_frozen = False
    for i, ev in enumerate(events[1:], start=1):
        combined = _ks(ev, "semanticSceneSubmittedSkinnedPaletteCombinedHash")
        if combined == cur_combined:
            cur_frozen = True
        else:
            segments.append((cur_start, i - cur_start, cur_frozen, cur_combined))
            cur_start = i
            cur_combined = combined
            cur_frozen = False
    segments.append((cur_start, len(events) - cur_start, cur_frozen, cur_combined))

    print("\n=== per-segment writer & palette-capture delta ===")
    print("  # note: runtimeMatrixWrite*Count / paletteCapture* are cumulative counters")
    print("          so we diff between first and last event of each segment.")
    print()
    header = ("SEG  #events  frozen?  combined_hash      "
              "mwCall  mwWrHits  mwEmpty  "
              "gpwCall  gpwWrHits  gpwEmpty  "
              "simpleCall  simpleWrHits  simpleEmpty")
    print(header)

    # 需要访问的 counter（cumulative 字段，在 keyStats 里）
    count_keys = [
        "runtimeMatrixWriteCount",
        "runtimeMatrixWriteFramesWithHitCount",
        "runtimeMatrixWriteFramesEmptyCount",
        "runtimeGroupPaletteWrapperCallCount",
        "runtimeGroupPaletteWrapperFramesWithHitCount",
        "runtimeGroupPaletteWrapperFramesEmptyCount",
        "runtimeSimpleGroupPaletteCallCount",
        "runtimeSimpleGroupPaletteFramesWithHitCount",
        "runtimeSimpleGroupPaletteFramesEmptyCount",
    ]

    for seg_idx, (start, length, frozen, combined) in enumerate(segments):
        first_ev = events[start]
        last_ev = events[start + length - 1]
        delta = {}
        for k in count_keys:
            a = _ks(first_ev, k)
            b = _ks(last_ev, k)
            if isinstance(a, int) and isinstance(b, int):
                delta[k] = b - a
            else:
                delta[k] = "?"
        ch = str(combined or "")[-10:]
        marker = "[FROZEN]" if frozen else ""
        # filter: 只关心 length >= 3 的 frozen 或任何 non-zero length
        if length < 2:
            continue
        print(f"  {seg_idx:3d}  {length:3d}     {str(frozen):5s}    ...{ch:>10s}  "
              f"{delta.get('runtimeMatrixWriteCount', '?'):>6}  "
              f"{delta.get('runtimeMatrixWriteFramesWithHitCount', '?'):>7}  "
              f"{delta.get('runtimeMatrixWriteFramesEmptyCount', '?'):>7}  "
              f"{delta.get('runtimeGroupPaletteWrapperCallCount', '?'):>6}  "
              f"{delta.get('runtimeGroupPaletteWrapperFramesWithHitCount', '?'):>7}  "
              f"{delta.get('runtimeGroupPaletteWrapperFramesEmptyCount', '?'):>7}  "
              f"{delta.get('runtimeSimpleGroupPaletteCallCount', '?'):>6}  "
              f"{delta.get('runtimeSimpleGroupPaletteFramesWithHitCount', '?'):>6}  "
              f"{delta.get('runtimeSimpleGroupPaletteFramesEmptyCount', '?'):>7}")

    # 对比 frozen vs non-frozen 平均 writer 活动
    print("\n=== aggregate: frozen vs non-frozen writer activity ===")
    frozen_mw_total = 0
    frozen_mw_hits = 0
    frozen_events = 0
    nonfrozen_mw_total = 0
    nonfrozen_mw_hits = 0
    nonfrozen_events = 0

    # 以每对相邻 event 之间的 delta 作为单位
    for i in range(1, len(events)):
        prev = events[i - 1]
        cur = events[i]
        mw_delta = (_ks(cur, "runtimeMatrixWriteCount") or 0) - (_ks(prev, "runtimeMatrixWriteCount") or 0)
        mw_hit_delta = (_ks(cur, "runtimeMatrixWriteFramesWithHitCount") or 0) - (_ks(prev, "runtimeMatrixWriteFramesWithHitCount") or 0)
        gpw_hit_delta = (_ks(cur, "runtimeGroupPaletteWrapperFramesWithHitCount") or 0) - (_ks(prev, "runtimeGroupPaletteWrapperFramesWithHitCount") or 0)
        prev_combined = _ks(prev, "semanticSceneSubmittedSkinnedPaletteCombinedHash")
        cur_combined = _ks(cur, "semanticSceneSubmittedSkinnedPaletteCombinedHash")
        frozen = prev_combined == cur_combined and cur_combined not in (None, "", "0", 0)
        if frozen:
            frozen_mw_total += mw_delta
            frozen_mw_hits += mw_hit_delta
            frozen_events += 1
        else:
            nonfrozen_mw_total += mw_delta
            nonfrozen_mw_hits += mw_hit_delta
            nonfrozen_events += 1

    print(f"  FROZEN:    events={frozen_events}  avgMwCall={frozen_mw_total/max(1,frozen_events):.1f}  avgMwWrHits={frozen_mw_hits/max(1,frozen_events):.2f}")
    print(f"  NON-FROZEN:events={nonfrozen_events}  avgMwCall={nonfrozen_mw_total/max(1,nonfrozen_events):.1f}  avgMwWrHits={nonfrozen_mw_hits/max(1,nonfrozen_events):.2f}")

    if frozen_mw_hits / max(1, frozen_events) < 0.5 and nonfrozen_mw_hits / max(1, nonfrozen_events) >= 0.9:
        print("\n  -> PRODUCER 在 frozen 窗口里明显不 fire (mwWrHits avg < 0.5)")
        print("     根因是 Codex 原提出的 dt gate 类型的 'producer 早退' 机制")
    elif frozen_mw_hits / max(1, frozen_events) >= 0.9:
        print("\n  -> PRODUCER 在 frozen 窗口里仍然 fire 每次都命中")
        print("     根因是 submit 端 palette 仲裁/缓存在冻结窗口里吃旧数据")
    else:
        print("\n  -> 比例介于中间，需要看分布")
    return 0


if __name__ == "__main__":
    sys.exit(main())
