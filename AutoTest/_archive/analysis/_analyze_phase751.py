"""Phase 7.51 分析：
  - FROZEN 段里 submitLiveRebuildAttempt / Hit / Applied / Miss 的分布
  - paletteSource 分桶（DrawTimeCaptured / SubmitTimeGlobalSlot / 
    SubmitTimeBlendedPaletteCache / SubmitTimePublishedPoseRegistry / 
    SubmitTimeCModelFallback / None）
  - CombinedHash 冻结窗口数
从 currentDrawRawHex 解析也需要的字段。
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path


# CurrentDrawContractDiagnosticsSummary 的 submitLiveRebuild 字段 offset
# 看 war3_current_draw_contract.h 的位置（submitLiveRebuild 紧跟 submitPaletteContentAge*）
# 这里暂不通过 raw hex，直接 keyStats 查


def _ks(ev: dict, key: str):
    ks = ev.get("keyStats", {}) or {}
    if key in ks:
        return ks[key]
    if key in ev:
        return ev[key]
    return None


def main():
    trace_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
        r"E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_13_03_51_53.jsonl")
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
    print(f"[stats] frame events: {len(events)}")

    # 列出最后一帧的 submitLiveRebuild 和 paletteSource 相关字段
    last = events[-1]
    ks = last.get("keyStats", {})
    print("\n=== last frame (cumulative counters) ===")
    keys_of_interest = [
        "publishCallCumulative",
        "publishTrustedHitCumulative",
        "publishRawFallbackCumulative",
        # submit-live-rebuild
        "semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount",
        "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount",
        "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount",
        "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount",
        "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount",
        "semanticSceneSubmittedSkinnedPaletteSourceNoneCount",
        "semanticSceneSubmitted",
        "semanticSceneSubmittedSkinned",
        # renderablePartPaletteBinding counters
        "renderablePartPaletteBindingQueryHitCount",
        "renderablePartPaletteBindingQueryMissCount",
        "renderablePartPaletteSnapshotCapturedCount",
        "renderablePartPaletteSnapshotQueryHitCount",
        "renderablePartPaletteSnapshotQueryMissCount",
        # producer counters
        "runtimeGroupPaletteWrapperCallCount",
        "runtimeSimpleGroupPaletteCallCount",
        "runtimeMatrixWriteCount",
        "runtimeMatrixRangeCopyPalettePublishHitCount",
        "runtimeMatrixRangeCopyPalettePublishMissCount",
    ]
    for k in keys_of_interest:
        v = ks.get(k)
        print(f"  {k:62s} = {v}")

    # 看 submitLiveRebuild 来自 Phase 7.35 的 counter（它们在 CurrentDraw summary 里）
    # 这些字段名：submitLiveRebuildAttemptCount / HitCount / MissCount / AppliedCount
    # 但它们是在 bridge summary 里暴露的，keyStats 里查一下：
    print("\n=== submitLiveRebuild counters (last frame) ===")
    for k in [
        "submitLiveRebuildAttemptCount",
        "submitLiveRebuildHitCount",
        "submitLiveRebuildMissCount",
        "submitLiveRebuildAppliedCount",
    ]:
        v = ks.get(k)
        print(f"  {k:40s} = {v}")

    # 按 CombinedHash 分段：frozen window 数
    segments = []
    cur_start = 0
    cur_combined = _ks(events[0], "semanticSceneSubmittedSkinnedPaletteCombinedHash") or ""
    for i in range(1, len(events)):
        combined = _ks(events[i], "semanticSceneSubmittedSkinnedPaletteCombinedHash") or ""
        if combined != cur_combined:
            segments.append((cur_start, i - cur_start, cur_combined))
            cur_start = i
            cur_combined = combined
    segments.append((cur_start, len(events) - cur_start, cur_combined))

    frozen_segs = [s for s in segments if s[1] >= 3]
    print(f"\n=== CombinedHash frozen windows (>=3 frames) ===")
    print(f"  count: {len(frozen_segs)} / total segments: {len(segments)}")
    if frozen_segs:
        lens = [s[1] for s in frozen_segs]
        print(f"  lengths: min={min(lens)} max={max(lens)} avg={sum(lens)/len(lens):.1f}")

    # 关键对比：FROZEN vs NON-FROZEN 的 submit-live-rebuild 活动
    def delta_counter(a, b, key):
        return (_ks(b, key) or 0) - (_ks(a, key) or 0)

    frozen_attempt = 0
    frozen_hit = 0
    frozen_applied = 0
    frozen_intervals = 0
    nonfrozen_attempt = 0
    nonfrozen_hit = 0
    nonfrozen_applied = 0
    nonfrozen_intervals = 0

    for i in range(1, len(events)):
        prev = events[i - 1]
        cur = events[i]
        c_prev = _ks(prev, "semanticSceneSubmittedSkinnedPaletteCombinedHash") or ""
        c_cur = _ks(cur, "semanticSceneSubmittedSkinnedPaletteCombinedHash") or ""
        att = delta_counter(prev, cur, "submitLiveRebuildAttemptCount")
        hit = delta_counter(prev, cur, "submitLiveRebuildHitCount")
        app = delta_counter(prev, cur, "submitLiveRebuildAppliedCount")
        frozen = (c_prev == c_cur and c_cur != "")
        if frozen:
            frozen_attempt += att
            frozen_hit += hit
            frozen_applied += app
            frozen_intervals += 1
        else:
            nonfrozen_attempt += att
            nonfrozen_hit += hit
            nonfrozen_applied += app
            nonfrozen_intervals += 1

    print("\n=== submit-live-rebuild FROZEN vs NON-FROZEN ===")
    print(f"  FROZEN:    intervals={frozen_intervals}  "
          f"attempt/int={frozen_attempt/max(1,frozen_intervals):.1f}  "
          f"hit/int={frozen_hit/max(1,frozen_intervals):.1f}  "
          f"hit%/attempt={100*frozen_hit/max(1,frozen_attempt):.1f}  "
          f"applied/int={frozen_applied/max(1,frozen_intervals):.1f}")
    print(f"  NON-FRZ:   intervals={nonfrozen_intervals}  "
          f"attempt/int={nonfrozen_attempt/max(1,nonfrozen_intervals):.1f}  "
          f"hit/int={nonfrozen_hit/max(1,nonfrozen_intervals):.1f}  "
          f"hit%/attempt={100*nonfrozen_hit/max(1,nonfrozen_attempt):.1f}  "
          f"applied/int={nonfrozen_applied/max(1,nonfrozen_intervals):.1f}")

    # paletteSource 分桶（全程总和）
    print("\n=== paletteSource bucket distribution (whole trace) ===")
    first = events[0]
    last_ev = events[-1]
    source_keys = [
        ("DrawTimeCaptured",
         "semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount"),
        ("SubmitTimeGlobalSlot",
         "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount"),
        ("SubmitTimeBlendedCache",
         "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount"),
        ("SubmitTimePublishedRegistry",
         "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount"),
        ("SubmitTimeCModelFallback",
         "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount"),
        ("None",
         "semanticSceneSubmittedSkinnedPaletteSourceNoneCount"),
    ]
    total_all = 0
    deltas = []
    for label, key in source_keys:
        d = (_ks(last_ev, key) or 0) - (_ks(first, key) or 0)
        deltas.append((label, d))
        total_all += d
    for label, d in deltas:
        pct = 100 * d / max(1, total_all)
        print(f"  {label:30s} = {d:8d}  ({pct:5.1f}%)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
