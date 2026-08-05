"""Phase 7.47 dt gate probe：分析 full trace 中 dt 分布和 writer 静默帧对齐情况。"""

from __future__ import annotations

import json
import sys
from pathlib import Path


def _get(ev: dict, *names: str):
    # 先 keyStats 后 top-level。
    ks = ev.get("keyStats", {}) or {}
    for n in names:
        if n in ks:
            return ks[n]
        if n in ev:
            return ev[n]
    return None


def main():
    trace_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
        r"E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_13_00_31_14.jsonl")
    if not trace_path.exists():
        print(f"[fatal] trace not found: {trace_path}")
        return 2

    frame_events = []
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
            frame_events.append(obj)

    print(f"[stats] total frame events: {len(frame_events)}")
    if not frame_events:
        return 1

    # 整体占比
    last = frame_events[-1]
    total = _get(last, "spriteUberPreRenderTotalCount") or 0
    zero = _get(last, "spriteUberPreRenderDtZeroCount") or 0
    below = _get(last, "spriteUberPreRenderDtBelowEpsilonCount") or 0
    pos = _get(last, "spriteUberPreRenderDtPositiveCount") or 0
    neg = _get(last, "spriteUberPreRenderDtNegativeCount") or 0
    mw_cnt = _get(last, "runtimeMatrixWriteCount") or 0
    mw_hit = _get(last, "runtimeMatrixWriteFramesWithHitCount") or 0
    mw_empty = _get(last, "runtimeMatrixWriteFramesEmptyCount") or 0
    gpw_cnt = _get(last, "runtimeGroupPaletteWrapperCallCount") or 0
    gpw_hit = _get(last, "runtimeGroupPaletteWrapperFramesWithHitCount") or 0
    gpw_empty = _get(last, "runtimeGroupPaletteWrapperFramesEmptyCount") or 0
    simple_cnt = _get(last, "runtimeSimpleGroupPaletteCallCount") or 0
    simple_hit = _get(last, "runtimeSimpleGroupPaletteFramesWithHitCount") or 0
    simple_empty = _get(last, "runtimeSimpleGroupPaletteFramesEmptyCount") or 0

    print("\n=== Overall dt distribution ===")
    if total > 0:
        print(f"  total        = {total}")
        print(f"  dt == 0      = {zero} ({100*zero/total:.2f}%)  [eval skipped]")
        print(f"  |dt| < eps   = {below} ({100*below/total:.2f}%)  [eval skipped]")
        print(f"  dt > 0       = {pos} ({100*pos/total:.2f}%)  [eval run]")
        print(f"  dt < 0       = {neg} ({100*neg/total:.2f}%)")
        skipped = zero + below
        pct = 100 * skipped / total
        print(f"  -> eval-skipped: {skipped} ({pct:.2f}%)")
    else:
        print("  probe total=0 → NoteSpriteUberPreRenderDtBucket 从未被调用")

    print("\n=== Overall writer per-frameTag hit vs empty ===")
    print(f"  RuntimeMatrixWrite(0x12E600): calls={mw_cnt} frames-with-hit={mw_hit} empty={mw_empty}")
    print(f"  AllocAndFillWrapper(0x12FED0): calls={gpw_cnt} frames-with-hit={gpw_hit} empty={gpw_empty}")
    print(f"  SimpleFallback   (0x12FF90):   calls={simple_cnt} frames-with-hit={simple_hit} empty={simple_empty}")

    # 按帧 delta 分析
    print("\n=== per-trace-frame dt/writer/palette delta (POSE_FROZEN = dynamicPoseSignature unchanged; PALETTE_FROZEN = lastSubmittedPaletteHash unchanged) ===")
    prev = {}
    frozen_windows = []  # list of (start_idx, length, pose_sig, palette_hash)
    cur_start = None
    cur_sig = None
    cur_ph = None
    cur_len = 0
    for i, ev in enumerate(frame_events):
        scene_frame = ev.get("sceneFrameSerial")
        sig = _get(ev, "dynamicPoseSignature") or ""
        ph = _get(ev, "semanticSceneDirectLastSubmittedPaletteHash") or ""
        shadow_exec = _get(ev, "semanticSceneShadowMapExecutedThisFrame")
        reuse = _get(ev, "semanticSceneReceiverReuseShadowMap")

        total_i = _get(ev, "spriteUberPreRenderTotalCount") or 0
        zero_i = _get(ev, "spriteUberPreRenderDtZeroCount") or 0
        pos_i = _get(ev, "spriteUberPreRenderDtPositiveCount") or 0
        mw_hit_i = _get(ev, "runtimeMatrixWriteFramesWithHitCount") or 0
        gpw_hit_i = _get(ev, "runtimeGroupPaletteWrapperFramesWithHitCount") or 0

        if prev:
            d_total = total_i - prev["total"]
            d_zero = zero_i - prev["zero"]
            d_pos = pos_i - prev["pos"]
            d_mw = mw_hit_i - prev["mw_hit"]
            d_gpw = gpw_hit_i - prev["gpw_hit"]

            pose_frozen = sig == prev["sig"] and sig != ""
            palette_frozen = ph == prev["ph"] and ph != ""

            markers = []
            if pose_frozen:
                markers.append("POSE_FROZEN")
            if palette_frozen:
                markers.append("PALETTE_FROZEN")
            if d_pos == 0 and d_total > 0:
                markers.append("NO_DT_POS")
            if d_mw == 0:
                markers.append("NO_MW_HIT")
            if d_gpw == 0:
                markers.append("NO_GPW_HIT")
            m = " ".join(markers)

            # 冻结窗口累积
            if palette_frozen:
                if cur_start is None:
                    cur_start = i - 1
                    cur_len = 1
                    cur_sig = sig
                    cur_ph = ph
                cur_len += 1
            else:
                if cur_start is not None and cur_len >= 3:
                    frozen_windows.append((cur_start, cur_len, cur_sig, cur_ph))
                cur_start = None
                cur_len = 0

            print(f"  [{i:02d}] scene={scene_frame} sig={sig[-8:]} ph={ph[-8:]} "
                  f"dt[tot+{d_total:4d} zero+{d_zero:3d} pos+{d_pos:4d}] "
                  f"mw+{d_mw} gpw+{d_gpw}   {m}")

        prev = {
            "total": total_i, "zero": zero_i, "pos": pos_i,
            "mw_hit": mw_hit_i, "gpw_hit": gpw_hit_i,
            "sig": sig, "ph": ph,
        }

    # 收尾
    if cur_start is not None and cur_len >= 3:
        frozen_windows.append((cur_start, cur_len, cur_sig, cur_ph))

    print("\n=== palette-frozen windows (>=3 consecutive frames with same lastSubmittedPaletteHash) ===")
    if not frozen_windows:
        print("  (none)")
    else:
        for (start, length, sig, ph) in frozen_windows:
            print(f"  start=frame{start} length={length} sig={sig[-8:]} ph={ph[-8:]}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
