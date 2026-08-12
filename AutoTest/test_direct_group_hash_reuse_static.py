#!/usr/bin/env python3
"""Static contract for reusing the sealed CurrentDraw group hash."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="strict"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


begin = SOURCE.index("const uint64_t sealedCurrentDrawGroupHash")
end = SOURCE.index("// Phase 7.2: stable group hash", begin)
body = SOURCE[begin:end]

require(
    "currentDrawSample != nullptr && currentDrawSample->groupHash != 0u" in body,
    "sealed hash must come from the paired authoritative CurrentDraw sample",
)
require(
    "sealedCurrentDrawGroupHash != 0u" in body
    and "bit::fnv1a_hash(" in body,
    "packet bytes must only be hashed as a fallback when no sealed hash exists",
)
require(
    body.index("sealedCurrentDrawGroupHash != 0u")
    < body.index("bit::fnv1a_hash("),
    "sealed hash selection must precede the byte-scan fallback",
)
require(
    "packet.resource.ownedVertexGroupIndices.data()" in body
    and "packet.resource.ownedVertexGroupIndices.size()" in body,
    "fallback must retain the exact owned group-slot byte range",
)

print("direct group hash reuse static contract passed")
