"""Phase 7.70 quick: 直接平均 keyStats 数值（这些字段已经是 per-frame counters，
不是 cumulative）。"""
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
                frames.append(obj.get("keyStats") or {})

    if not frames:
        print("no frames")
        return

    def avg(field: str) -> float:
        vals = [int(f.get(field, 0) or 0) for f in frames]
        return sum(vals) / float(len(vals))

    def total(field: str) -> int:
        return sum(int(f.get(field, 0) or 0) for f in frames)

    print(f"frames                       = {len(frames)}")
    print(f"submitted (avg)              = {avg('semanticSceneSubmitted'):.2f}")
    print(f"shadowMapExec (avg)          = {avg('semanticSceneReceiverDrawExecutedThisFrame'):.2f}")
    print()
    print(f"drawTimeVBCacheTotalEntered (avg) = {avg('drawTimeVBCacheTotalEntered'):.2f}")
    print(f"drawTimeVBCacheCaptureCount (avg) = {avg('drawTimeVBCacheCaptureCount'):.2f}")
    print(f"  Phase 7.70 dedup HIT (avg)        = {avg('drawTimeVBCacheSameFrameDedupHit'):.2f}")
    print(f"  Phase 7.70 dedup MISS (avg)       = {avg('drawTimeVBCacheSameFrameDedupMiss'):.2f}")
    print(f"  Phase 7.70 state refresh (avg)    = {avg('drawTimeVBCacheSameFrameStateRefresh'):.2f}")
    print()
    print(f"PositionCopyCount (avg)      = {avg('drawTimeVBCachePositionCopyCount'):.2f}")
    print(f"PositionCopyBytes (avg)      = {avg('drawTimeVBCachePositionCopyBytes'):.0f} ({avg('drawTimeVBCachePositionCopyBytes')/1024.0:.1f} KB)")
    print(f"IndexCopyCount (avg)         = {avg('drawTimeVBCacheIndexCopyCount'):.2f}")
    print(f"IndexCopyBytes (avg)         = {avg('drawTimeVBCacheIndexCopyBytes'):.0f} ({avg('drawTimeVBCacheIndexCopyBytes')/1024.0:.1f} KB)")
    print(f"PositionAllocCount (avg)     = {avg('drawTimeVBCachePositionAllocCount'):.4f}")
    print(f"IndexAllocCount (avg)        = {avg('drawTimeVBCacheIndexAllocCount'):.4f}")
    print(f"UvCopyCount (avg)            = {avg('drawTimeVBCacheUvCopyCount'):.2f}")
    print(f"UvSharedPosition (avg)       = {avg('drawTimeVBCacheUvSharedPositionCount'):.2f}")
    print()
    print(f"drawTimeSemanticProducerVisibleCandidate (avg) = {avg('drawTimeSemanticProducerVisibleCandidateCount'):.2f}")
    print(f"drawTimeSemanticProducerSubmitted (avg)        = {avg('drawTimeSemanticProducerSubmittedCount'):.2f}")
    print(f"drawTimeSemanticProducerFresh (avg)            = {avg('drawTimeSemanticProducerFreshEntryCount'):.2f}")
    print(f"drawTimeSemanticProducerMissNoFresh (avg)      = {avg('drawTimeSemanticProducerMissNoFreshEntryCount'):.2f}")
    print(f"drawTimeSemanticProducerFallbackCurrentDraw    = {avg('drawTimeSemanticProducerFallbackCurrentDrawCount'):.2f}")
    print()
    print(f"shadowReceiverHoldEmptyReplay (avg) = {avg('semanticSceneShadowMapPreparedDrawCount'):.2f} prepared")
    print(f"semanticSceneShadowVisibilityExecutedThisFrame (avg) = {avg('semanticSceneShadowVisibilityExecutedThisFrame'):.2f}")
    print(f"semanticSceneShadowHistoryValidAfter (avg)           = {avg('semanticSceneShadowHistoryValidAfter'):.2f}")
    print(f"semanticSceneRejectedPathBlockerCount (avg)          = {avg('semanticSceneRejectedPathBlockerCount'):.2f}")


if __name__ == "__main__":
    main(sys.argv[1])
