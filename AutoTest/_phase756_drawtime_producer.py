"""Analyze draw-time semantic producer traces for the long-term pose path."""

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


def longest_zero_runs(values):
    runs = []
    start = None
    for i, value in enumerate(values):
        if value == 0:
            if start is None:
                start = i
        elif start is not None:
            runs.append((start, i - start))
            start = None
    if start is not None:
        runs.append((start, len(values) - start))
    return sorted(runs, key=lambda x: (-x[1], x[0]))


def main():
    path = sys.argv[1]
    frames = load_frames(path)
    if not frames:
        print("No shadowPoseFullTraceFrame events found.")
        return

    submitted = []
    producer_submitted = []
    producer_visible = []
    producer_fresh = []
    producer_miss = []
    producer_fallback = []
    reasons = []

    for evt in frames:
        ks = evt.get("keyStats", {})
        cad = evt.get("cadence", {})
        submitted.append(ks.get("semanticSceneSubmitted", 0) or 0)
        producer_submitted.append(
            ks.get("drawTimeSemanticProducerSubmittedCount", 0) or 0
        )
        producer_visible.append(
            ks.get("drawTimeSemanticProducerVisibleCandidateCount", 0) or 0
        )
        producer_fresh.append(
            ks.get("drawTimeSemanticProducerFreshEntryCount", 0) or 0
        )
        producer_miss.append(
            ks.get("drawTimeSemanticProducerMissNoFreshEntryCount", 0) or 0
        )
        producer_fallback.append(
            ks.get("drawTimeSemanticProducerFallbackCurrentDrawCount", 0) or 0
        )
        reasons.append(cad.get("populateReturnReason", -1))

    zero_runs = longest_zero_runs(submitted)
    producer_reason_frames = sum(1 for r in reasons if r == 10)
    zero_frames = sum(1 for s in submitted if s == 0)

    print(f"Trace: {path}")
    print(f"Total frames: {len(frames)}")
    print(f"Frames with submitted=0: {zero_frames}/{len(frames)} ({100.0 * zero_frames / len(frames):.1f}%)")
    print(f"Frames with populateReturnReason=10 (draw-time producer): {producer_reason_frames}/{len(frames)}")
    print(f"Average producer visible candidates/frame: {sum(producer_visible) / len(frames):.1f}")
    print(f"Average producer fresh entries/frame: {sum(producer_fresh) / len(frames):.1f}")
    print(f"Average producer submitted/frame: {sum(producer_submitted) / len(frames):.1f}")
    print(f"Average producer miss-no-fresh/frame: {sum(producer_miss) / len(frames):.1f}")
    print(f"Frames with producer fallback to current-draw: {sum(1 for x in producer_fallback if x != 0)}/{len(frames)}")

    print("\nTop 10 zero-submit runs:")
    for start, length in zero_runs[:10]:
        print(f"  [{start}..{start + length - 1}] len={length}")

    print("\nSample frames:")
    sample_indices = []
    if zero_runs:
        sample_indices.extend(range(max(0, zero_runs[0][0] - 1), min(len(frames), zero_runs[0][0] + 3)))
    else:
        sample_indices.extend(range(min(5, len(frames))))
    seen = set()
    for i in sample_indices:
        if i in seen:
            continue
        seen.add(i)
        print(
            f"  Frame {i}: submitted={submitted[i]} "
            f"producerSubmitted={producer_submitted[i]} "
            f"visible={producer_visible[i]} fresh={producer_fresh[i]} "
            f"miss={producer_miss[i]} fallback={producer_fallback[i]} "
            f"reason={reasons[i]}"
        )


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: py AutoTest\\_phase756_drawtime_producer.py <trace.jsonl>")
        sys.exit(1)
    main()
