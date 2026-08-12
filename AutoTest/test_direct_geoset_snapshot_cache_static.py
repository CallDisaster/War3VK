#!/usr/bin/env python3
"""Static contract for the scene-local Direct geoset snapshot cache."""

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


cache_start = DEVICE.index("class War3CurrentDrawGeosetSnapshotCache final")
cache_end = DEVICE.index(
    "bool War3TryAttachCurrentDrawVisibleIndexSlice", cache_start
)
cache = DEVICE[cache_start:cache_end]

assert "static constexpr size_t kEntryCount = 128u" in cache
assert "std::array<Entry, kEntryCount> m_entries" in cache
assert "unordered_map" not in cache
assert "thread_local" not in cache
for proof in (
    "entry.snapshot->mapEpoch != m_mapEpoch",
    "entry.snapshot->immutableModelGeneration == 0u",
    "!entry.snapshot->readyForShadowConsumer()",
    "snapshot->mapEpoch != m_mapEpoch",
):
    assert proof in cache

builder = body("bool War3TryBuildShadowPacketFromCurrentDrawRecord(")
local_find = builder.index("geosetSnapshotCache->find")
global_find = builder.index("War3FindDirectPacketGeosetResource")
store = builder.index("geosetSnapshotCache->store")
resource_use = builder.index("const auto& geo = *sharedGeoset")
assert local_find < global_find < store < resource_use
assert "if (sharedGeoset == nullptr)\n      sharedGeoset = War3Find" in builder
assert "geosetSnapshotCacheHit = sharedGeoset != nullptr;" in builder
assert "geosetSnapshotCache != nullptr && !geosetSnapshotCacheHit" in builder

populate = body("uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(")
construct = populate.index("War3CurrentDrawGeosetSnapshotCache")
build_call = populate.index("War3TryBuildShadowPacketFromCurrentDrawRecord(")
cache_arg = populate.index("&currentDrawGeosetSnapshotCache", build_call)
assert construct < build_call < cache_arg

print("direct geoset snapshot cache static tests passed")
