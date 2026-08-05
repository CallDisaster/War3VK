"""分析 submit=0 的帧分布，找出卡顿模式。"""
import json
import sys

trace = sys.argv[1] if len(sys.argv) > 1 else r"E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_14_02_58_12.jsonl"

frames = []
with open(trace, encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        try:
            evt = json.loads(line)
        except Exception:
            continue
        if evt.get("type") == "shadowPoseFullTraceFrame":
            frames.append(evt)

print(f"Total frames: {len(frames)}")

# 标记每帧 submit==0 与否
submit_marks = []
for evt in frames:
    ks = evt.get("keyStats", {})
    sub = ks.get("semanticSceneSubmitted", 0) or 0
    submit_marks.append(sub)

# 找连续 0 区间
runs = []
cur_zero_start = None
for i, s in enumerate(submit_marks):
    if s == 0:
        if cur_zero_start is None:
            cur_zero_start = i
    else:
        if cur_zero_start is not None:
            runs.append((cur_zero_start, i - cur_zero_start))
            cur_zero_start = None
if cur_zero_start is not None:
    runs.append((cur_zero_start, len(submit_marks) - cur_zero_start))

runs.sort(key=lambda r: -r[1])
print("\nTop 20 longest zero-submit runs (start_frame, length):")
for start, length in runs[:20]:
    print(f"  [{start}..{start+length-1}] len={length}")

zero_count = sum(1 for s in submit_marks if s == 0)
print(f"\nZero-submit frames: {zero_count}/{len(frames)} ({100*zero_count/len(frames):.1f}%)")

# 看看 submit=0 帧的其他特征
print("\nFirst 5 zero-submit frames detail:")
for i, s in enumerate(submit_marks):
    if s == 0:
        evt = frames[i]
        ks = evt.get("keyStats", {})
        cad = evt.get("cadence", {})
        print(f"  Frame {i}: submitted={s}, "
              f"replayDraws={ks.get('semanticSceneReplayDrawsCount', 0)}, "
              f"shadowMapExec={ks.get('semanticSceneShadowMapExecutedThisFrame', 0)}, "
              f"populateReason={cad.get('populateReturnReason', '?')}, "
              f"inputDraws={cad.get('inputDrawCount', '?')}, "
              f"inputSkinned={cad.get('inputSkinnedCount', '?')}")
        if i > 5: break

# 对比 submit > 0 帧
print("\nFirst 5 submit>0 frames detail:")
shown = 0
for i, s in enumerate(submit_marks):
    if s > 0 and shown < 5:
        evt = frames[i]
        ks = evt.get("keyStats", {})
        cad = evt.get("cadence", {})
        print(f"  Frame {i}: submitted={s}, "
              f"replayDraws={ks.get('semanticSceneReplayDrawsCount', 0)}, "
              f"shadowMapExec={ks.get('semanticSceneShadowMapExecutedThisFrame', 0)}, "
              f"populateReason={cad.get('populateReturnReason', '?')}, "
              f"inputDraws={cad.get('inputDrawCount', '?')}, "
              f"inputSkinned={cad.get('inputSkinnedCount', '?')}")
        shown += 1
