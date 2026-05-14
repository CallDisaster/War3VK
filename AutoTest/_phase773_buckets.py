"""Phase 7.73：路径阻断器拒绝来源分桶查看。"""
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

    fields = [
        "semanticSceneRejectedPathBlockerCount",
        "semanticSceneRejectedPathBlockerEarlyBypassCount",
        "semanticSceneRejectedPathBlockerEligibilityGateCount",
        "semanticSceneRejectedPathBlockerAppendEntryCount",
        "semanticSceneRejectedPathBlockerAppendEntryByJHandleCount",
        "semanticSceneRejectedPathBlockerAppendVbBlendCount",
        "semanticSceneRejectedPathBlockerFastAppendCount",
        "semanticSceneRejectedPathBlockerDirectGroupedCount",
        "semanticSceneRejectedPathBlockerProducerCount",
        "semanticSceneRejectedPathBlockerStaticSupplementCount",
        "semanticSceneRejectedPathBlockerLegacyCaptureCount",
    ]

    print(f"frames = {len(frames)}")
    for f in fields:
        v = avg(f)
        if v != 0.0:
            print(f"  {f:<70s} = {v:.2f}/frame")
    # Sum of buckets vs total
    total = avg("semanticSceneRejectedPathBlockerCount")
    bucket_sum = sum(avg(f) for f in fields[1:])
    print(f"\ntotal = {total:.2f}")
    print(f"bucket_sum = {bucket_sum:.2f}")
    if abs(total - bucket_sum) > 0.05:
        print(f"WARNING: bucket sum != total (delta={total-bucket_sum:.2f})")


if __name__ == "__main__":
    main(sys.argv[1])
