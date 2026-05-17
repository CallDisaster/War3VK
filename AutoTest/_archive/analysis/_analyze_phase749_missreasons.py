"""Phase 7.49 v3：从 currentDrawRawHex 里解析出 publishMiss* counter，
按 CombinedHash FROZEN/NON-FROZEN 段对比 miss 分布。

Struct 布局（CurrentDrawContractDiagnosticsSummary 开头）:
  [0]  u64 publishAttemptCount
  [8]  u64 publishReadyCount
  [16] u64 publishMissNoRenderablePart
  [24] u64 publishMissNoMeshPayload
  [32] u64 publishMissInvalidPaletteSlot
  [40] u64 publishMissInvalidPaletteCount
  [48] u64 publishMissNoGlobalPalette
  [56] u64 publishSkippedNonWorldContext
  [64] u64 publishSkippedSmallViewport
  ...
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path


MISS_FIELDS = [
    ("publishAttemptCount", 0),
    ("publishReadyCount", 8),
    ("publishMissNoRenderablePart", 16),
    ("publishMissNoMeshPayload", 24),
    ("publishMissInvalidPaletteSlot", 32),
    ("publishMissInvalidPaletteCount", 40),
    ("publishMissNoGlobalPalette", 48),
    ("publishSkippedNonWorldContext", 56),
    ("publishSkippedSmallViewport", 64),
]


def parse_raw_hex(raw_hex: str) -> dict[str, int]:
    if not raw_hex:
        return {}
    # raw hex 可能是 "0xXXXX..." 或不带前缀的连续 hex
    s = raw_hex
    if s.startswith("0x"):
        s = s[2:]
    try:
        raw = bytes.fromhex(s)
    except ValueError:
        return {}
    out = {}
    for name, off in MISS_FIELDS:
        if off + 8 <= len(raw):
            (val,) = struct.unpack_from("<Q", raw, off)
            out[name] = val
    return out


def _ks(ev: dict, key: str):
    ks = ev.get("keyStats", {}) or {}
    if key in ks:
        return ks[key]
    if key in ev:
        return ev[key]
    return None


def main():
    trace_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
        r"E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_13_02_23_07.jsonl")
    events = []
    with trace_path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except Exception:
                continue
            if obj.get("type") != "shadowPoseFullTraceFrame":
                continue
            events.append(obj)
    print(f"[stats] events={len(events)}")

    # 解析每帧 miss counter 并存
    parsed = []
    for ev in events:
        raw = ev.get("currentDrawRawHex", "") or ""
        fields = parse_raw_hex(raw)
        parsed.append(fields)

    if not parsed or not parsed[0]:
        print("[fatal] 没能解析 currentDrawRawHex，字段都是空")
        return 2

    # 打印开头两帧的 miss 分布作为 sanity check
    print("\n=== Sanity: first and last event miss counters ===")
    for label, idx in [("first", 0), ("last", len(events) - 1)]:
        p = parsed[idx]
        print(f"  [{label}] {p}")

    # 按 CombinedHash 分段
    segments = []
    cur_start = 0
    cur_combined = _ks(events[0], "semanticSceneSubmittedSkinnedPaletteCombinedHash") or ""
    for i in range(1, len(events)):
        combined = _ks(events[i], "semanticSceneSubmittedSkinnedPaletteCombinedHash") or ""
        if combined != cur_combined:
            segments.append((cur_start, i - cur_start, len(events), cur_combined))
            cur_start = i
            cur_combined = combined
    segments.append((cur_start, len(events) - cur_start, len(events), cur_combined))

    frozen_segs = [s for s in segments if s[1] >= 3]
    nonfrozen_segs = [s for s in segments if s[1] < 3]

    def agg(segs, name):
        delta_per_event = {k: 0 for k, _ in MISS_FIELDS}
        intervals = 0
        for start, length, *_ in segs:
            if length < 2:
                continue
            first = parsed[start]
            last = parsed[start + length - 1]
            if not first or not last:
                continue
            for k, _ in MISS_FIELDS:
                delta_per_event[k] += last.get(k, 0) - first.get(k, 0)
            intervals += length - 1
        print(f"\n=== {name} ({len(segs)} segments, {intervals} intervals) ===")
        if intervals == 0:
            print("  (no intervals)")
            return
        total = delta_per_event["publishAttemptCount"]
        for k, _ in MISS_FIELDS:
            v = delta_per_event[k]
            pct = 100 * v / max(1, total)
            print(f"  {k:45s}: avg/interval={v/intervals:8.1f}  total_delta={v:8d}  {pct:5.1f}%")
        # 推断：
        miss_sum = (delta_per_event["publishMissNoRenderablePart"]
                    + delta_per_event["publishMissNoMeshPayload"]
                    + delta_per_event["publishMissInvalidPaletteSlot"]
                    + delta_per_event["publishMissInvalidPaletteCount"]
                    + delta_per_event["publishMissNoGlobalPalette"]
                    + delta_per_event["publishSkippedNonWorldContext"]
                    + delta_per_event["publishSkippedSmallViewport"])
        valid_publish = delta_per_event["publishReadyCount"]
        print(f"  SUM(miss+skip)={miss_sum}  publishReady={valid_publish}  "
              f"attempt={total}  coverage=(ready+miss+skip)/attempt="
              f"{100*(miss_sum+valid_publish)/max(1,total):.1f}%")

    agg(frozen_segs, "FROZEN segments (CombinedHash unchanged >=3 frames)")
    agg(nonfrozen_segs, "NON-FROZEN segments")

    # 全程 aggregate
    if len(parsed) >= 2 and parsed[0] and parsed[-1]:
        print("\n=== Whole trace total ===")
        total = parsed[-1]["publishAttemptCount"] - parsed[0]["publishAttemptCount"]
        for k, _ in MISS_FIELDS:
            v = parsed[-1].get(k, 0) - parsed[0].get(k, 0)
            pct = 100 * v / max(1, total)
            print(f"  {k:45s}: {v:>8d}  ({pct:5.1f}%)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
