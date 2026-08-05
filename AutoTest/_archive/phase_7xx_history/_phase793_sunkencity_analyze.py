"""Phase 7.93: 分析 SunkenCity trace — 为什么 40 个可见对象变成 700 submitted caster。"""
import json
import sys
from pathlib import Path


def main(path: str) -> None:
    p = Path(path)
    frames = []
    with p.open("r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except Exception:
                continue
            if obj.get("type") == "shadowPoseFullTraceFrame":
                frames.append(obj)

    if not frames:
        print("no frames")
        return

    print(f"frames = {len(frames)}")
    print()

    # Per-frame breakdown
    for i, f in enumerate(frames):
        ks = f.get("keyStats", {})
        cadence = f.get("cadence", {})
        submitted = int(cadence.get("submittedDrawCount", 0) or 0)
        skinned = int(cadence.get("submittedSkinnedCount", 0) or 0)
        input_draw = int(cadence.get("inputDrawCount", 0) or 0)
        input_skinned = int(cadence.get("inputSkinnedCount", 0) or 0)
        replay = int(cadence.get("replayDrawsCount", 0) or 0)
        casters = int(cadence.get("shadowCastersCount", 0) or 0)
        shadow_drawn = int(cadence.get("shadowMapDrawnCasters", 0) or 0)
        populate_reason = int(cadence.get("populateReturnReason", 0) or 0)
        
        vb_entered = int(ks.get("drawTimeVBCacheTotalEntered", 0) or 0)
        vb_captured = int(ks.get("drawTimeVBCacheCaptureCount", 0) or 0)
        pos_copy_bytes = int(ks.get("drawTimeVBCachePositionCopyBytes", 0) or 0)
        pos_alloc = int(ks.get("drawTimeVBCachePositionAllocCount", 0) or 0)
        
        producer_visible = int(ks.get("drawTimeSemanticProducerVisibleCandidateCount", 0) or 0)
        producer_submitted = int(ks.get("drawTimeSemanticProducerSubmittedCount", 0) or 0)
        
        bypass_attempt = int(ks.get("semanticSceneDirectDrawTimePrebuildBypassAttemptCount", 0) or 0)
        bypass_hit = int(ks.get("semanticSceneDirectDrawTimePrebuildBypassHitCount", 0) or 0)
        
        print(f"Frame {i:2d}: submitted={submitted:4d} skinned={skinned:3d} "
              f"input={input_draw:4d} replay={replay:4d} casters={casters:4d} "
              f"shadowDrawn={shadow_drawn:5d} | "
              f"vbEntered={vb_entered:4d} vbCaptured={vb_captured:4d} "
              f"posBytes={pos_copy_bytes//1024:4d}KB alloc={pos_alloc:3d} | "
              f"prodVisible={producer_visible:4d} prodSubmit={producer_submitted:4d} | "
              f"bypass={bypass_hit:3d}/{bypass_attempt:3d} reason={populate_reason}")


if __name__ == "__main__":
    main(sys.argv[1])
