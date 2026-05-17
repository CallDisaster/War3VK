"""Dump one frame's keyStats for debugging."""
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

# 找一个有提交的帧
target = None
for evt in frame_events:
    ks = evt.get("keyStats", {})
    if ks.get("semanticSceneSubmittedSkinned", 0) > 0:
        target = evt
        break

if target is None:
    print("No frame with submitted > 0")
    target = frame_events[10] if len(frame_events) > 10 else frame_events[-1]

print("=== Sample frame keyStats ===")
ks = target.get("keyStats", {})
for k, v in sorted(ks.items()):
    if isinstance(v, (int, float, str, bool)) and ("draw" in k.lower() or "vb" in k.lower() or "capture" in k.lower() or "skinned" in k.lower() or "shadow" in k.lower() or "submit" in k.lower() or "semantic" in k.lower()):
        print(f"  {k}: {v}")

print("\n=== Cadence ===")
cad = target.get("cadence", {})
for k, v in sorted(cad.items()):
    if isinstance(v, (int, float, str, bool)):
        print(f"  {k}: {v}")
