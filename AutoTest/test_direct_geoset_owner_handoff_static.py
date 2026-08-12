#!/usr/bin/env python3
"""Static contract for allocation-free geoset owner handoff to packets."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
START = DEVICE.index("bool War3TryBuildShadowPacketFromCurrentDrawRecord(")
END = DEVICE.index("uint64_t War3SemanticHashMatrix4", START)
BUILDER = DEVICE[START:END]

cache_store = BUILDER.index(
    "geosetSnapshotCache->store(record.meshPayloadPtr, sharedGeoset)"
)
cache_store_gate = BUILDER.rfind("if (geosetSnapshotCache", 0, cache_store)
geo_ref = BUILDER.index("const auto& geo = *sharedGeoset", cache_store)
owner_move = BUILDER.index(
    "resource.resourceKeepAlive = std::move(sharedGeoset)", geo_ref
)

assert cache_store < geo_ref < owner_move
assert "!geosetSnapshotCacheHit" in BUILDER[cache_store_gate:cache_store]
assert "resource.resourceKeepAlive = sharedGeoset;" not in BUILDER
assert "resource.resourceKeepAlive = std::move(sharedGeoset);" in BUILDER
assert BUILDER[owner_move:].count("sharedGeoset") == 1  # the move expression

print("direct geoset owner handoff static checks passed")
