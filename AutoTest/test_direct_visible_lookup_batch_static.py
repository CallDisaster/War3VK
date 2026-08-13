#!/usr/bin/env python3
"""Static contract for the one-snapshot Direct visible lookup."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/d3d9/war3/render/war3_visible_renderables.h").read_text(
    encoding="utf-8"
)
SOURCE = (ROOT / "src/d3d9/war3/render/war3_visible_renderables.cpp").read_text(
    encoding="utf-8"
)
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[start : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


assert "queryFirstForDirectPacket(const CurrentDrawContractRecord& record" in HEADER

lookup = function_body(
    SOURCE, "bool VisibleRenderableRegistry::queryFirstForDirectPacket("
)
assert lookup.count("snapshotForThread()") == 1

part_pos = lookup.index("snap.byRenderablePartLayer.find")
payload_pos = lookup.index("snap.byPayload.find")
scene_pos = lookup.index("snap.bySceneNode.find")
assert part_pos < payload_pos < scene_pos

match_body = lookup[lookup.index("const auto matchesCurrentDrawSlice") :]
for token in (
    "candidate.layerIndex != record.layerIndex",
    "record.renderablePart != candidate.renderablePart",
    "record.sceneNode != candidate.sceneNode",
):
    assert token in match_body
assert lookup.index("return true;", part_pos) < lookup.index(
    "const auto matchesCurrentDrawSlice"
)

builder = function_body(DEVICE, "bool War3TryBuildShadowPacketFromCurrentDrawRecord(")
visible_start = builder.index('War3SemanticSubmitScope("War3SemanticScene/Direct/VisibleLookup")')
visible_end = builder.index("const void* effectiveRuntimeModelPtr", visible_start)
visible_block = builder[visible_start:visible_end]
assert visible_block.count("queryFirstForDirectPacket") == 1
for old_query in (
    "queryByRenderablePartAndLayer",
    "queryByPayload",
    "queryBySceneNode",
):
    assert old_query not in visible_block

assert not re.search(r"(?:Snapshot|VisibleRenderableRecord)\s*\*\s*queryFirstForDirectPacket", HEADER)

print("direct visible lookup batch static tests passed")
