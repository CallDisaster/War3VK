#!/usr/bin/env python3
"""Static contract for bypassing redundant RenderObject direct lookups."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


def body(signature: str) -> str:
    start = DEVICE.index(signature)
    brace = DEVICE.index("{", start)
    depth = 0
    for pos in range(brace, len(DEVICE)):
        if DEVICE[pos] == "{":
            depth += 1
        elif DEVICE[pos] == "}":
            depth -= 1
            if depth == 0:
                return DEVICE[start : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


builder = body("bool War3TryBuildShadowPacketFromCurrentDrawRecord(")
visible = builder.index("visibleRegistry.queryFirstForDirectPacket")
gate = builder.index("const bool currentDrawObjectIdentityComplete")
kind_gate = builder.index("const bool currentDrawHasUnitKind")
render = builder.index("renderRegistry.findFirstForDirectPacket")
assert visible < kind_gate < gate < render

gate_body = builder[gate:render]
for proof in (
    "record.worldObjectEntry != nullptr",
    "record.sceneNode != nullptr",
    "record.unitPtr != nullptr",
    "record.jHandle != 0u",
    "record.rawcode != 0u",
    "currentDrawHasUnitKind",
    "visibleHit",
    "visibleRecord.identity.groupIdx >= 0",
    "visibleRecord.identity.flags5C != 0u",
):
    assert proof in gate_body

kind_gate_body = builder[kind_gate:gate]
assert "ObjectKind::Unit" in kind_gate_body
assert "ObjectKind::Building" in kind_gate_body

lookup_guard = builder[builder.rfind("if (", gate, render):render]
assert "!currentDrawObjectIdentityComplete" in lookup_guard

# The bypass may omit only RenderObjectRegistry. Exact visible identity,
# ShadowObject fallback and map-epoch unit flags remain in the packet path.
assert "shadowObjectSnapshotCache->find" in builder[render:]
assert "War3TryReadUnitFlags5CCached" in builder[render:]
assert "visibleRecord.identity.groupIdx" in builder[render:]
assert "? visibleRecord.identity.flags5C" in builder[render:]

print("direct complete-identity RenderObject bypass static tests passed")
