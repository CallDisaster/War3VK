"""Phase 7.48 per-frame palette aggregator 分析：解决解释 A vs B 的二分。"""

from __future__ import annotations

import json
import sys
from pathlib import Path


def main():
    trace_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
        r"E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_13_01_34_35.jsonl")
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
    print(f"[stats] trace frame events: {len(events)}")
    if not events:
        return 1

    prev_combined = None
    prev_last = None
    prev_first = None
    prev_distinct = None

    print("\n=== per-trace-frame aggregator (core decision data) ===")
    print("  scene  submitted  skinned  distinct consecutive-same-max  "
          "firstH  combinedH  lastSubmittedH  [markers]")

    combined_frozen_runs = []
    cur_combined = None
    cur_run_start = None
    cur_run_len = 0

    last_frozen_runs = []
    cur_last = None
    cur_last_start = None
    cur_last_len = 0

    for i, ev in enumerate(events):
        ks = ev.get("keyStats", {}) or {}
        scene_frame = ev.get("sceneFrameSerial")
        submitted = ks.get("semanticSceneSubmitted")
        skinned = ks.get("semanticSceneSubmittedSkinned")
        distinct = ks.get("semanticSceneSubmittedSkinnedPaletteDistinctSampleCount")
        max_same = ks.get("semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax")
        first = ks.get("semanticSceneSubmittedSkinnedPaletteFirstSubmittedHash", "")
        combined = ks.get("semanticSceneSubmittedSkinnedPaletteCombinedHash", "")
        last_h = ks.get("semanticSceneDirectLastSubmittedPaletteHash", "")
        zero = ks.get("semanticSceneSubmittedSkinnedPaletteZeroHashCount")

        markers = []
        if isinstance(last_h, str) and last_h == prev_last and last_h:
            markers.append("LAST_FROZEN")
        if isinstance(combined, str) and combined == prev_combined and combined:
            markers.append("COMBINED_FROZEN")
        if isinstance(first, str) and first == prev_first and first:
            markers.append("FIRST_FROZEN")
        if skinned and distinct == 1:
            markers.append("SINGLE_HASH_ENTIRE_FRAME")
        if skinned and max_same == skinned:
            markers.append("ALL_SAME_HASH")

        # combined-frozen 累积
        if markers and "COMBINED_FROZEN" in markers:
            if cur_run_start is None:
                cur_run_start = i - 1
                cur_run_len = 1
                cur_combined = combined
            cur_run_len += 1
        else:
            if cur_run_start is not None and cur_run_len >= 3:
                combined_frozen_runs.append((cur_run_start, cur_run_len, cur_combined))
            cur_run_start = None
            cur_run_len = 0

        if markers and "LAST_FROZEN" in markers:
            if cur_last_start is None:
                cur_last_start = i - 1
                cur_last_len = 1
                cur_last = last_h
            cur_last_len += 1
        else:
            if cur_last_start is not None and cur_last_len >= 3:
                last_frozen_runs.append((cur_last_start, cur_last_len, cur_last))
            cur_last_start = None
            cur_last_len = 0

        first_short = first[-8:] if isinstance(first, str) else str(first)
        combined_short = combined[-8:] if isinstance(combined, str) else str(combined)
        last_short = last_h[-8:] if isinstance(last_h, str) else str(last_h)
        print(f"  [{i:02d}] scene={scene_frame} sub={submitted} skn={skinned} "
              f"distinct={distinct} maxSame={max_same} zero={zero} "
              f"first={first_short} combined={combined_short} last={last_short} "
              f"{' '.join(markers)}")
        prev_combined = combined
        prev_last = last_h
        prev_first = first
        prev_distinct = distinct

    if cur_run_start is not None and cur_run_len >= 3:
        combined_frozen_runs.append((cur_run_start, cur_run_len, cur_combined))
    if cur_last_start is not None and cur_last_len >= 3:
        last_frozen_runs.append((cur_last_start, cur_last_len, cur_last))

    print("\n=== CombinedHash frozen runs (>=3 consecutive trace frames) ===")
    if not combined_frozen_runs:
        print("  NONE  -> CombinedHash 每帧都在变：整帧所有 skinned palette 聚合起来没有跨帧锁死")
    else:
        for r in combined_frozen_runs:
            print(f"  start=trace_frame{r[0]} length={r[1]} combined=...{r[2][-10:]}")

    print("\n=== LastSubmittedHash frozen runs (>=3 consecutive trace frames) ===")
    if not last_frozen_runs:
        print("  NONE")
    else:
        for r in last_frozen_runs:
            print(f"  start=trace_frame{r[0]} length={r[1]} last=...{r[2][-10:]}")

    print("\n=== VERDICT ===")
    if combined_frozen_runs:
        print("  解释 B（真冻结）成立迹象：CombinedHash 跨多 trace frame 不变 → "
              "submit 端整帧所有 skinned 都在吃同一批 palette。")
        print("  下一步：查 PublishCurrentDrawContract 仲裁 + Phase 7.46 snapshot "
              "在冻结窗口内的 hit / provenance 分布。")
    elif last_frozen_runs:
        print("  解释 A（指标错觉）成立：LastSubmittedHash 不稳，但 CombinedHash "
              "每帧都在变 → 冻结只是因为 lastSubmittedHash 记的是最后一个 caster，"
              "单英雄地图里它恰好常相同。用户视频的阴影停顿不是这个 counter 指示的。")
        print("  下一步：需要跑用户实机场景（多英雄/真卡顿地图）的 15s trace 再看。")
    else:
        print("  两个 frozen run 都不存在 → 本轮 AutoTest 没复现冻结窗口。")

    return 0


if __name__ == "__main__":
    sys.exit(main())
