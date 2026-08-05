"""对比两份 trace（中心 vs 边界），找出关键差异。"""
import json
import sys

def load_frames(path):
    frames = []
    with open(path, encoding="utf-8") as f:
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
    return frames

def summarize(label, frames):
    print(f"\n=== {label}: {len(frames)} frames ===")
    if not frames:
        return
    # 累计 counter
    keys_to_track = [
        "drawTimeVBCacheCaptureCount",
        "drawTimeVBCacheConsumeHitCount",
        "drawTimeVBCacheConsumeMissCount",
        "drawTimeVBCacheTotalEntered",
        "drawTimeVBCacheRejectInvalidRange",
        "semanticSceneSubmitted",
        "semanticSceneSubmittedSkinned",
        "semanticSceneShadowMapDrawnCasters",
        "semanticSceneShadowMapExecutedThisFrame",
        "semanticSceneReceiverDrawExecutedThisFrame",
        "semanticSceneShadowCastersCount",
        "runtimeMatrixWriteCount",
        "submitPaletteFrameLag0",
        "submitPaletteFrameLag1",
        "submitPaletteFrameLag2",
        "submitPaletteFrameLag3To5",
        "submitPaletteFrameLag6Plus",
        "submitPaletteFrameLagSampleCount",
        "submitPaletteContentAgeLag0",
        "submitPaletteContentAgeLag1",
        "submitPaletteContentAgeLag2",
        "submitPaletteContentAgeLag3To5",
        "submitPaletteContentAgeLag6Plus",
        "submitPaletteContentAgeSampleCount",
    ]
    first = frames[0].get("keyStats", {})
    last = frames[-1].get("keyStats", {})
    for k in keys_to_track:
        f = first.get(k, 0) or 0
        l = last.get(k, 0) or 0
        delta = l - f
        if delta != 0 or k.startswith("submitPaletteFrame") or k.startswith("submitPaletteContent"):
            print(f"  {k}: {f} -> {l}  (Δ={delta})")

    # 每帧 submit/draw 的均值
    sub_total = 0
    sub_skinned_total = 0
    drawn_total = 0
    nonzero_drawn_frames = 0
    nonzero_sub_frames = 0
    map_exec_count = 0
    for evt in frames:
        ks = evt.get("keyStats", {})
        sub = ks.get("semanticSceneSubmitted", 0) or 0
        sub_sk = ks.get("semanticSceneSubmittedSkinned", 0) or 0
        drawn = ks.get("semanticSceneShadowMapDrawnCasters", 0) or 0
        executed = ks.get("semanticSceneShadowMapExecutedThisFrame", 0) or 0
        sub_total += sub
        sub_skinned_total += sub_sk
        drawn_total += drawn
        if drawn > 0: nonzero_drawn_frames += 1
        if sub > 0: nonzero_sub_frames += 1
        if executed: map_exec_count += 1
    n = len(frames)
    print(f"  avg submitted/frame: {sub_total/n:.1f}")
    print(f"  avg submitted skinned/frame: {sub_skinned_total/n:.1f}")
    print(f"  avg shadowMapDrawnCasters/frame: {drawn_total/n:.1f}")
    print(f"  shadowMapExecuted frames: {map_exec_count}/{n} ({100*map_exec_count/n:.1f}%)")
    print(f"  frames with submitted>0: {nonzero_sub_frames}/{n}")
    print(f"  frames with shadowMapDrawn>0: {nonzero_drawn_frames}/{n}")

if __name__ == "__main__":
    a = sys.argv[1] if len(sys.argv) > 1 else r"E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_14_02_54_22.jsonl"
    b = sys.argv[2] if len(sys.argv) > 2 else r"E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_14_02_58_12.jsonl"
    summarize("[CENTER] " + a, load_frames(a))
    summarize("[FAR_FROM_CENTER] " + b, load_frames(b))
