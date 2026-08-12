#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
CONTRACT_CPP = ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
HEADER = ROOT / "src/d3d9/war3/render/war3_current_draw_contract.h"


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


contract = CONTRACT_CPP.read_text(encoding="utf-8")
device = DEVICE_CPP.read_text(encoding="utf-8")
header = HEADER.read_text(encoding="utf-8")

decode = function_body(contract, "bool DecodeCurrentDrawGroupSlots(")
resolve_record = function_body(
    contract, "CurrentDrawResolveStatus ResolveCurrentDrawAuthoritativeSampleFromRecord(")
packet = function_body(device, "bool War3TryBuildShadowPacketFromCurrentDrawRecord(")

assert "CurrentDrawGroupSlotSummary summary" in decode
assert "summary.include(groupSlot)" in decode
assert "outStableGroupHash = summary.stableHash(" in decode
assert "outMaxGroupSlot = summary.maxGroupSlot" in decode
assert "ComputeStableGroupContentHash(out.contract, out.groupSlots)" not in resolve_record
assert "uint32_t maxGroupSlot = 0u" in header
assert "directDecodedGroupSlotMaxReady" in packet
assert re.search(
    r"if \(!directDecodedGroupSlotMaxReady\)\s*\{[^}]*for \(uint8_t slot : effectiveGroupSlots\)",
    packet,
    re.S,
)
assert "outDirectCurrentDrawSample->maxGroupSlot" in packet

print("direct group-slot single-pass static checks passed")
