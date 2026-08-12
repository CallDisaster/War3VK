#!/usr/bin/env python3
"""Static contract for immutable geoset summaries consumed by DirectGrouped."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/d3d9/war3/model/war3_model_resource_cache.h").read_text(
    encoding="utf-8"
)
SOURCE = (ROOT / "src/d3d9/war3/model/war3_model_resource_cache.cpp").read_text(
    encoding="utf-8"
)
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


def body(text: str, signature: str) -> str:
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


record = HEADER[HEADER.index("struct ShadowGeosetResourceRecord") :]
record = record[: record.index("};")]
assert "uint32_t maxVertexGroupSlot = 0u" in record
assert "uint64_t vertexGroupSlotWordHash = 0u" in record
assert "uint32_t maxMatrixGroupSize = 0u" in record

refresh = body(SOURCE, "void RefreshGeosetImmutableDerivedValues(")
assert "record.maxVertexGroupSlot = 0u" in refresh
assert "for (uint8_t slot : record.vertexGroupIndices)" in refresh
assert "record.vertexGroupSlotWordHash = bit::fnv1a_init()" in refresh
assert "record.vertexGroupSlotWordHash = bit::fnv1a_iter(" in refresh
assert "record.maxMatrixGroupSize = 0u" in refresh
assert "for (uint32_t groupSize : record.matrixGroupSizes)" in refresh

registration = body(SOURCE, "ShadowModelResourceCache::storeGeosetRecord(")
generation = registration.index("merged.immutableModelGeneration = generation")
derived = registration.index("RefreshGeosetImmutableDerivedValues(merged)")
publish = registration.index("m_byGeosetData[merged.geosetDataPtr]")
assert generation < derived < publish

builder = body(DEVICE, "bool War3TryBuildShadowPacketFromCurrentDrawRecord(")
assert "const uint32_t maxExpectedGroupSize = geo.maxMatrixGroupSize" in builder
assert ": geo.maxVertexGroupSlot" in builder
assert "for (uint32_t groupSize : geo.matrixGroupSizes)" not in builder
assert "for (uint8_t slot : effectiveGroupSlots)" not in builder

print("direct immutable geoset derived-summary static tests passed")
