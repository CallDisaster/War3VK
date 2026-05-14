"""Phase 7.70: 检查同帧 capture dedup 是否生效，与 Phase 7.69 基线 trace 对账。"""
import json
import sys
from pathlib import Path
from collections import defaultdict


def summarize(trace_path: Path):
    frames = []
    producer_zero = 0
    submitted_min = None
    pos_copy_count_per_frame = []
    pos_copy_bytes_per_frame = []
    idx_copy_count_per_frame = []
    dedup_hit_per_frame = []
    dedup_miss_per_frame = []
    state_refresh_per_frame = []
    total_entered_per_frame = []
    capture_count_per_frame = []
    submitted_per_frame = []
    fresh_per_frame = []
    candidates_per_frame = []
    receiver_exec = 0
    history_valid = 0

    last_pos_copy_count = 0
    last_pos_copy_bytes = 0
    last_idx_copy_count = 0
    last_dedup_hit = 0
    last_dedup_miss = 0
    last_state_refresh = 0
    last_total_entered = 0
    last_capture_count = 0
    last_submitted = 0
    last_fresh = 0
    last_candidates = 0

    with trace_path.open("r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except Exception:
                continue
            if obj.get("type") != "shadowPoseFullTraceFrame":
                continue
            ks = obj.get("keyStats") or {}
            if not isinstance(ks, dict):
                continue
            sub = int(ks.get("semanticSceneSubmitted", 0) or 0)
            submitted_per_frame.append(sub)
            if sub == 0:
                producer_zero += 1
            if submitted_min is None or sub < submitted_min:
                submitted_min = sub

            cur_pos_copy_count = int(ks.get("drawTimeVBCachePositionCopyCount", 0) or 0)
            cur_pos_copy_bytes = int(ks.get("drawTimeVBCachePositionCopyBytes", 0) or 0)
            cur_idx_copy_count = int(ks.get("drawTimeVBCacheIndexCopyCount", 0) or 0)
            cur_dedup_hit = int(ks.get("drawTimeVBCacheSameFrameDedupHit", 0) or 0)
            cur_dedup_miss = int(ks.get("drawTimeVBCacheSameFrameDedupMiss", 0) or 0)
            cur_state_refresh = int(ks.get("drawTimeVBCacheSameFrameStateRefresh", 0) or 0)
            cur_total_entered = int(ks.get("drawTimeVBCacheTotalEntered", 0) or 0)
            cur_capture = int(ks.get("drawTimeVBCacheCaptureCount", 0) or 0)
            cur_submitted = int(ks.get("drawTimeSemanticProducerSubmittedCount", 0) or 0)
            cur_fresh = int(ks.get("drawTimeSemanticProducerFreshEntryCount", 0) or 0)
            cur_candidates = int(ks.get("drawTimeSemanticProducerVisibleCandidateCount", 0) or 0)

            # they're cumulative since process start; per-frame delta is what we want
            pos_copy_count_per_frame.append(max(0, cur_pos_copy_count - last_pos_copy_count))
            pos_copy_bytes_per_frame.append(max(0, cur_pos_copy_bytes - last_pos_copy_bytes))
            idx_copy_count_per_frame.append(max(0, cur_idx_copy_count - last_idx_copy_count))
            dedup_hit_per_frame.append(max(0, cur_dedup_hit - last_dedup_hit))
            dedup_miss_per_frame.append(max(0, cur_dedup_miss - last_dedup_miss))
            state_refresh_per_frame.append(max(0, cur_state_refresh - last_state_refresh))
            total_entered_per_frame.append(max(0, cur_total_entered - last_total_entered))
            capture_count_per_frame.append(max(0, cur_capture - last_capture_count))
            submitted_delta = max(0, cur_submitted - last_submitted)
            fresh_delta = max(0, cur_fresh - last_fresh)
            candidates_delta = max(0, cur_candidates - last_candidates)

            last_pos_copy_count = cur_pos_copy_count
            last_pos_copy_bytes = cur_pos_copy_bytes
            last_idx_copy_count = cur_idx_copy_count
            last_dedup_hit = cur_dedup_hit
            last_dedup_miss = cur_dedup_miss
            last_state_refresh = cur_state_refresh
            last_total_entered = cur_total_entered
            last_capture_count = cur_capture
            last_submitted = cur_submitted
            last_fresh = cur_fresh
            last_candidates = cur_candidates

            if int(ks.get("semanticSceneShadowMapExecutedThisFrame", 0) or 0):
                receiver_exec += 1
            if int(ks.get("semanticSceneShadowHistoryValidAfter", 0) or 0):
                history_valid += 1
            frames.append(ks)

    n = max(1, len(frames))

    # drop first sample because all "current" deltas are uninitialized baseline
    def avg(lst):
        if len(lst) <= 1:
            return 0.0
        return sum(lst[1:]) / float(len(lst) - 1)

    print(f"frames                        = {len(frames)}")
    print(f"submitted=0                   = {producer_zero} / {len(frames)}")
    print(f"submitted_min                 = {submitted_min}")
    print(f"shadowMapExecuted             = {receiver_exec} / {len(frames)}")
    print(f"shadowHistoryValid            = {history_valid} / {len(frames)}")
    print()
    print(f"avg pos copy commands / frame = {avg(pos_copy_count_per_frame):.2f}")
    print(f"avg pos copy bytes / frame    = {avg(pos_copy_bytes_per_frame):.0f} ({avg(pos_copy_bytes_per_frame)/1024.0:.1f} KB)")
    print(f"avg idx copy commands / frame = {avg(idx_copy_count_per_frame):.2f}")
    print(f"avg total entered / frame     = {avg(total_entered_per_frame):.2f}")
    print(f"avg capture (slow path) / fr  = {avg(capture_count_per_frame):.2f}")
    print(f"avg dedup HIT / frame         = {avg(dedup_hit_per_frame):.2f}")
    print(f"avg dedup MISS / frame        = {avg(dedup_miss_per_frame):.2f}")
    print(f"avg state refresh / frame     = {avg(state_refresh_per_frame):.2f}")
    print(f"avg producer submitted / fr   = {avg(submitted_per_frame):.2f}")
    print(f"avg producer fresh / frame    = {avg(fresh_per_frame):.2f}" if fresh_per_frame else "")
    print(f"avg producer candidates / fr  = {avg(candidates_per_frame):.2f}" if candidates_per_frame else "")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: _phase770_dedup_check.py <trace.jsonl>")
        sys.exit(1)
    summarize(Path(sys.argv[1]))
