"""Phase 7.51 从 currentDrawRawHex 直接读所有 uint64 字段 + 按偏移推断关键值。"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path


def parse_u64s(raw_hex: str):
    s = raw_hex or ""
    if s.startswith("0x"):
        s = s[2:]
    raw = bytes.fromhex(s)
    result = []
    for off in range(0, len(raw) - 7, 8):
        (val,) = struct.unpack_from("<Q", raw, off)
        result.append((off, val))
    return result


def _ks(ev: dict, key: str):
    ks = ev.get("keyStats", {}) or {}
    if key in ks:
        return ks[key]
    if key in ev:
        return ev[key]
    return None


def main():
    trace_path = Path(sys.argv[1])
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
    print(f"events: {len(events)}")

    # 分别分析 first / last 的 currentDraw 全部 u64
    first = events[0]
    last = events[-1]
    first_vals = parse_u64s(first.get("currentDrawRawHex", ""))
    last_vals = parse_u64s(last.get("currentDrawRawHex", ""))

    # 对比 delta 找到真正有活动的字段
    diffs = []
    for (off, a), (off2, b) in zip(first_vals, last_vals):
        d = b - a
        if d != 0 and 0 < d < (1 << 40):
            diffs.append((off, a, b, d))

    print(f"\n non-zero deltas (total trace):")
    for off, a, b, d in diffs:
        print(f"  offset 0x{off:04x} ({off:3d}): first={a:8d}  last={b:8d}  delta={d:8d}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
