"""分析 Phase 7.55 v4 trace 的 drawTimeVB 命中率 + CombinedHash 冻结。"""
import json
import sys

trace_path = sys.argv[1] if len(sys.argv) > 1 else r"E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_13_19_32_51.jsonl"

frame_events = []
with open(trace_path, encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        try:
            evt = json.loads(line)
        except Exception:
            continue
        if evt.get("type") == "shadowPoseFullTraceFrame":
            frame_events.append(evt)

print(f"Total frame events: {len(frame_events)}")
if not frame_events:
    sys.exit(0)

# 提取 keyStats
def get_path(d, *keys, default=None):
    cur = d
    for k in keys:
        if not isinstance(cur, dict):
            return default
        cur = cur.get(k, default)
        if cur is default:
            return default
    return cur

# Counter 累积值
counter_keys = [
    "drawTimeVBCacheCaptureCount",
    "drawTimeVBCacheConsumeHitCount",
    "drawTimeVBCacheConsumeMissCount",
    "semanticSceneSubmittedSkinned",
    "semanticSceneShadowMapDrawnCasters",
    "runtimeMatrixWriteCount",
]

# 第一个 vs 最后一个看 delta
first = frame_events[0]
last = frame_events[-1]
print("\n=== 累积 counter（last - first）===")
ks_first = first.get("keyStats", {})
ks_last = last.get("keyStats", {})
for key in counter_keys:
    f = ks_first.get(key, 0) or 0
    l = ks_last.get(key, 0) or 0
    print(f"  {key}: {f} -> {l}  (delta={l-f})")

# 看 CombinedHash 冻结
print("\n=== CombinedHash 冻结分析 ===")
combined_hashes = []
for evt in frame_events:
    ks = evt.get("keyStats", {})
    h = ks.get("semanticSceneSubmittedSkinnedPaletteCombinedHash")
    if h is not None:
        combined_hashes.append(h)

if combined_hashes:
    # 统计连续相同的最长 run
    runs = []
    cur_run_start = 0
    for i in range(1, len(combined_hashes)):
        if combined_hashes[i] != combined_hashes[i-1]:
            runs.append((cur_run_start, i - cur_run_start, combined_hashes[i-1]))
            cur_run_start = i
    runs.append((cur_run_start, len(combined_hashes) - cur_run_start, combined_hashes[-1]))

    print(f"  Total samples: {len(combined_hashes)}")
    print(f"  Distinct hash count: {len(set(combined_hashes))}")
    runs.sort(key=lambda x: -x[1])
    print(f"  Top 10 longest frozen runs (start, length, hash):")
    for start, length, h in runs[:10]:
        print(f"    [{start}..{start+length-1}] len={length} hash={h:#x}")
    n_frozen3 = sum(1 for r in runs if r[1] >= 3)
    print(f"  Runs with len>=3: {n_frozen3}")

# 看 semantic scene 是否在 submit
print("\n=== Submit 状态（前 5 帧 / 后 5 帧）===")
for label, evts in [("first 5", frame_events[:5]), ("last 5", frame_events[-5:])]:
    print(f"  {label}:")
    for evt in evts:
        ks = evt.get("keyStats", {})
        print(f"    sub={ks.get('semanticSceneSubmittedSkinned', 0):>6} "
              f"replay={ks.get('semanticSceneReplayDrawsCount', 0):>4} "
              f"map={ks.get('semanticSceneShadowMapExecutedThisFrame', 0)} "
              f"drawn={ks.get('semanticSceneShadowMapDrawnCasters', 0):>4} "
              f"vbCap={ks.get('drawTimeVBCacheCaptureCount', 0):>6} "
              f"vbHit={ks.get('drawTimeVBCacheConsumeHitCount', 0):>6} "
              f"vbMiss={ks.get('drawTimeVBCacheConsumeMissCount', 0):>5}")
