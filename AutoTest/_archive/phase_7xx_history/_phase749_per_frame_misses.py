"""每 trace frame 打印 publishAttempt/Ready/InvalidSlot delta，
看 FROZEN 8 帧里 publishReady 到底是 0 还是少量非 0。"""

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
    s = raw_hex or ""
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

    parsed = [parse_raw_hex(ev.get("currentDrawRawHex", "")) for ev in events]

    print("  idx skinned combined     attempt+  ready+ invSlot+ noGlobal+ nonWorld+ (marker)")
    prev = None
    prev_combined = None
    frozen_run = 0
    for i, ev in enumerate(events):
        skinned = _ks(ev, "semanticSceneSubmittedSkinned") or 0
        combined = _ks(ev, "semanticSceneSubmittedSkinnedPaletteCombinedHash") or ""
        p = parsed[i]
        if not p:
            continue
        marker = ""
        if prev_combined is not None and combined == prev_combined and combined:
            frozen_run += 1
            marker = f"FROZEN(run={frozen_run})"
        else:
            frozen_run = 0
        if prev is None:
            print(f"  {i:3d} {skinned:3d}    ...{combined[-8:]}")
        else:
            a = p['publishAttemptCount'] - prev['publishAttemptCount']
            r = p['publishReadyCount'] - prev['publishReadyCount']
            isl = p['publishMissInvalidPaletteSlot'] - prev['publishMissInvalidPaletteSlot']
            ng = p['publishMissNoGlobalPalette'] - prev['publishMissNoGlobalPalette']
            nw = p['publishSkippedNonWorldContext'] - prev['publishSkippedNonWorldContext']
            print(f"  {i:3d} {skinned:3d}    ...{combined[-8:]}  {a:4d}    {r:4d}   {isl:4d}     {ng:4d}      {nw:4d}    {marker}")
        prev = p
        prev_combined = combined

    return 0


if __name__ == "__main__":
    sys.exit(main())
