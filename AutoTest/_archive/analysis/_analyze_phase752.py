#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Phase 7.52 trace 分析：对比 CombinedHash 冻结窗口、snapshot hit rate、lease refresh 等。"""
import json
import sys

def parse_hex(s):
    if isinstance(s, int):
        return s
    if isinstance(s, str) and s.startswith('0x'):
        try:
            return int(s, 16)
        except Exception:
            return 0
    return 0

def analyze(path):
    combined_hashes = []
    last_hashes = []
    dyn_sigs = []
    distinct_counts = []
    consec_max_per_frame = []
    snapshot_captured = 0
    snapshot_query_hit = 0
    snapshot_query_miss = 0
    publish_ready = 0
    publish_miss_slot = 0
    publish_attempts = 0
    rebuild_attempt = 0
    rebuild_hit = 0
    rebuild_applied = 0
    range_copy_publish_hit = 0
    range_copy_publish_miss = 0
    shadow_map_executed = []
    receiver_reuse = []
    runtime_matrix_write_count = []
    wrapper_call_count = []
    src_none = 0
    src_draw_time = 0
    src_global_slot = 0
    src_blended_cache = 0
    src_pose_reg = 0
    src_cmodel = 0
    frames = []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except Exception:
                continue
            if obj.get('type') != 'shadowPoseFullTraceFrame':
                continue
            frames.append(obj)
            ks = obj.get('keyStats', {}) or {}
            ch = parse_hex(ks.get('semanticSceneSubmittedSkinnedPaletteCombinedHash', 0))
            lh = parse_hex(ks.get('semanticSceneDirectLastSubmittedPaletteHash', 0))
            ds = parse_hex(ks.get('semanticSceneDynamicPoseSignature', 0))
            dc = ks.get('semanticSceneSubmittedSkinnedPaletteDistinctSampleCount', 0)
            cm = ks.get('semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax', 0)
            combined_hashes.append(ch)
            last_hashes.append(lh)
            dyn_sigs.append(ds)
            distinct_counts.append(dc)
            consec_max_per_frame.append(cm)
            snapshot_captured = max(snapshot_captured, ks.get('renderablePartPaletteSnapshotCapturedCount', 0))
            snapshot_query_hit = max(snapshot_query_hit, ks.get('renderablePartPaletteSnapshotQueryHitCount', 0))
            snapshot_query_miss = max(snapshot_query_miss, ks.get('renderablePartPaletteSnapshotQueryMissCount', 0))
            publish_attempts = max(publish_attempts, ks.get('currentDrawContractPublishAttemptCount', 0))
            publish_ready = max(publish_ready, ks.get('currentDrawContractPublishReadyCount', 0))
            publish_miss_slot = max(publish_miss_slot, ks.get('currentDrawContractPublishMissInvalidPaletteSlot', 0))
            rebuild_attempt = max(rebuild_attempt, ks.get('submitLiveRebuildAttemptCount', 0))
            rebuild_hit = max(rebuild_hit, ks.get('submitLiveRebuildHitCount', 0))
            rebuild_applied = max(rebuild_applied, ks.get('submitLiveRebuildAppliedCount', 0))
            range_copy_publish_hit = max(range_copy_publish_hit, ks.get('runtimeMatrixRangeCopyPalettePublishHitCount', 0))
            range_copy_publish_miss = max(range_copy_publish_miss, ks.get('runtimeMatrixRangeCopyPalettePublishMissCount', 0))
            src_none = max(src_none, ks.get('semanticSceneSubmittedSkinnedPaletteSourceNoneCount', 0))
            src_draw_time = max(src_draw_time, ks.get('semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount', 0))
            src_global_slot = max(src_global_slot, ks.get('semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount', 0))
            src_blended_cache = max(src_blended_cache, ks.get('semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount', 0))
            src_pose_reg = max(src_pose_reg, ks.get('semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedPoseRegistryCount', 0))
            src_cmodel = max(src_cmodel, ks.get('semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount', 0))
            if 'semanticSceneShadowMapExecutedThisFrame' in ks:
                shadow_map_executed.append(ks['semanticSceneShadowMapExecutedThisFrame'])
            if 'semanticSceneReceiverReuseShadowMap' in ks:
                receiver_reuse.append(ks['semanticSceneReceiverReuseShadowMap'])
            if 'runtimeMatrixWriteCount' in ks:
                runtime_matrix_write_count.append(ks['runtimeMatrixWriteCount'])
            if 'runtimeGroupPaletteWrapperCallCount' in ks:
                wrapper_call_count.append(ks['runtimeGroupPaletteWrapperCallCount'])

    total_frames = len(frames)
    print(f"TRACE: {path}")
    print(f"  total trace frames: {total_frames}")
    if total_frames == 0:
        return

    # 找第一次 CombinedHash 非零帧（skinned submit 真正开始的点）
    first_nonzero = None
    for i, h in enumerate(combined_hashes):
        if h != 0:
            first_nonzero = i
            break
    if first_nonzero is None:
        print(f"  *** ALL CombinedHash == 0. skinned caster 从未被提交 ***")
        # 仍输出一些 writer 计数
        if len(runtime_matrix_write_count) >= 2:
            deltas = [runtime_matrix_write_count[i] - runtime_matrix_write_count[i-1]
                      for i in range(1, len(runtime_matrix_write_count))]
            print(f"  RuntimeMatrixWriteCount avg_delta={sum(deltas)/len(deltas):.1f}/trace frame")
        return
    print(f"  first CombinedHash!=0 at frame {first_nonzero}, active frames: {total_frames - first_nonzero}")

    active_combined = combined_hashes[first_nonzero:]

    # CombinedHash 冻结窗口分析
    freeze_windows = []
    cur_start = 0
    cur_hash = active_combined[0]
    for i in range(1, len(active_combined)):
        if active_combined[i] == cur_hash and cur_hash != 0:
            continue
        length = i - cur_start
        if length >= 3 and cur_hash != 0:
            freeze_windows.append((cur_start + first_nonzero, i - 1 + first_nonzero, length, cur_hash))
        cur_start = i
        cur_hash = active_combined[i]
    length = len(active_combined) - cur_start
    if length >= 3 and cur_hash != 0:
        freeze_windows.append((cur_start + first_nonzero, len(active_combined) - 1 + first_nonzero, length, cur_hash))

    print(f"  CombinedHash frozen windows (>=3 frames, active only): {len(freeze_windows)}")
    if freeze_windows:
        lengths = [w[2] for w in freeze_windows]
        print(f"    min={min(lengths)}  max={max(lengths)}  mean={sum(lengths)/len(lengths):.2f}")
        max_window = max(freeze_windows, key=lambda w: w[2])
        print(f"    longest: frame[{max_window[0]}..{max_window[1]}] length={max_window[2]}")

    # LastSubmittedHash 冻结（active 段）
    active_last = last_hashes[first_nonzero:]
    nonzero_last = [h for h in active_last if h != 0]
    if nonzero_last:
        last_freeze = 0
        cur = 1
        for i in range(1, len(active_last)):
            if active_last[i] == active_last[i-1] and active_last[i] != 0:
                cur += 1
            else:
                last_freeze = max(last_freeze, cur)
                cur = 1
        last_freeze = max(last_freeze, cur)
        print(f"  LastSubmittedHash active max same-run: {last_freeze} frames")

    # DistinctSampleCount
    active_distinct = [d for d in distinct_counts[first_nonzero:] if d > 0]
    if active_distinct:
        print(f"  DistinctSampleCount active avg: {sum(active_distinct)/len(active_distinct):.1f}, max: {max(active_distinct)}")

    # Counters
    print(f"  renderablePartPaletteSnapshotCaptured={snapshot_captured}")
    print(f"  renderablePartPaletteSnapshotQuery: hit={snapshot_query_hit} miss={snapshot_query_miss}")
    if snapshot_query_hit + snapshot_query_miss > 0:
        hit_rate = snapshot_query_hit / (snapshot_query_hit + snapshot_query_miss) * 100
        print(f"    snapshot query hit rate: {hit_rate:.1f}%")
    print(f"  Publish: attempt={publish_attempts} ready={publish_ready} missInvalidSlot={publish_miss_slot}")
    if publish_attempts > 0:
        print(f"    publishReady rate: {publish_ready/publish_attempts*100:.1f}%")
        print(f"    publishMissInvalidSlot rate: {publish_miss_slot/publish_attempts*100:.1f}%")
    print(f"  SubmitLiveRebuild: attempt={rebuild_attempt} hit={rebuild_hit} applied={rebuild_applied}")
    if rebuild_attempt > 0:
        print(f"    rebuild hit rate: {rebuild_hit/rebuild_attempt*100:.1f}%")
    print(f"  RuntimeMatrixRangeCopyPublish: hit={range_copy_publish_hit} miss={range_copy_publish_miss}")
    print(f"  PaletteSource dist: None={src_none} DrawTime={src_draw_time} GlobalSlot={src_global_slot} BlendedCache={src_blended_cache} PoseReg={src_pose_reg} CModel={src_cmodel}")

    if shadow_map_executed:
        exec_on = sum(1 for x in shadow_map_executed if x != 0)
        print(f"  ShadowMapExecutedThisFrame: {exec_on}/{len(shadow_map_executed)} frames")
    if receiver_reuse:
        reuse = sum(1 for x in receiver_reuse if x != 0)
        print(f"  ReceiverReuseShadowMap: {reuse}/{len(receiver_reuse)} frames")

    if len(runtime_matrix_write_count) >= 2:
        deltas = [runtime_matrix_write_count[i] - runtime_matrix_write_count[i-1]
                  for i in range(1, len(runtime_matrix_write_count))]
        print(f"  RuntimeMatrixWriteCount avg_delta={sum(deltas)/len(deltas):.1f}/trace frame")

if __name__ == '__main__':
    for p in sys.argv[1:]:
        analyze(p)
        print()
