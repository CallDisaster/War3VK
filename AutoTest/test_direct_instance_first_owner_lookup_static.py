#!/usr/bin/env python3
"""Static contract for the exact-instance-first Direct packet lookup."""

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
first_instance = builder.index("instanceSnapshotCache->find(")
proof = builder.index("const auto instanceProvesGeosetOwner")
owner_lookup = builder.index("resourceCache.findRuntimeModelOwnerBindingIndexed(")
assert first_instance < proof < owner_lookup

proof_body = builder[proof : builder.index("bool ownerSatisfiedByInstance", proof)]
for token in (
    "geosetHit",
    "instanceHit",
    "instanceRecord.runtimeModelPtr != nullptr",
    "instanceRecord.modelResourcePtr == geoset->modelResourcePtr",
    "instanceRecord.modelKey == geoset->modelKey",
):
    assert token in proof_body

owner_guard_start = builder.index("if (!ownerSatisfiedByInstance)", proof)
owner_guard = builder[owner_guard_start:owner_lookup]
assert "!ownerSatisfiedByInstance" in owner_guard
assert "ownerSatisfiedByInstance || instanceProvesGeosetOwner()" in builder
assert "if (!ownerHit && !ownerSatisfiedByInstance)" in builder

# A miss still gets the canonical owner-runtime fallback and no owner result is
# forged from an instance projection.
assert "InstancePostOwnerLookup" in builder
assert builder.count("instanceRegistry.findFirstForDirectPacketView(") == 3
assert "ownerHit = true" not in builder

print("direct instance-first owner lookup static tests passed")
