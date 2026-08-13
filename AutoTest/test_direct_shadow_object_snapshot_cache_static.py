#!/usr/bin/env python3
"""Static contract for scene-local Direct shadow-object value caching."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
HEADER = (
    ROOT / "src/d3d9/war3/render/war3_shadow_object_registry.h"
).read_text(encoding="utf-8")
SOURCE = (
    ROOT / "src/d3d9/war3/render/war3_shadow_object_registry.cpp"
).read_text(encoding="utf-8")


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


assert "uint64_t* mutationGenerationOut = nullptr" in HEADER
lookup = body(SOURCE, "bool ShadowObjectRegistry::findFirstForDirectPacketView(")
assert lookup.count("std::shared_lock<std::shared_mutex> lock(m_mutex)") == 1
assert "m_mutationGeneration.load(std::memory_order_acquire)" in lookup
priority = [
    lookup.index("m_byWorldObjectEntry"),
    lookup.index("m_bySceneNode"),
    lookup.index("m_byHandle"),
    lookup.index("m_byRuntimeModel, primaryRuntimeModelPtr"),
    lookup.index("m_byRuntimeModel, secondaryRuntimeModelPtr"),
]
assert priority == sorted(priority)
assert "ProjectShadowObjectAugmentView(*record, out)" in lookup

cache_start = DEVICE.index("class War3CurrentDrawShadowObjectSnapshotCache final")
cache_end = DEVICE.index("bool War3TryAttachCurrentDrawVisibleIndexSlice", cache_start)
cache = DEVICE[cache_start:cache_end]
assert "static constexpr size_t kEntryCount = 128u" in cache
assert "std::array<Entry, kEntryCount> m_entries" in cache
assert "unordered_map" not in cache
assert "thread_local" not in cache
assert "registry.mutationGeneration()" in cache
assert cache.count("registry.mutationGeneration()") >= 3
assert "(currentGeneration & 1u) == 0u" in cache
assert "observedGeneration == publishedGeneration" in cache
assert "registry.mutationGeneration() == currentGeneration" in cache

builder = body(DEVICE, "bool War3TryBuildShadowPacketFromCurrentDrawRecord(")
assert "shadowObjectSnapshotCache->find" in builder
assert "shadowRegistry.findFirstForDirectPacketView" in builder

populate = body(DEVICE, "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(")
construct = populate.index("War3CurrentDrawShadowObjectSnapshotCache")
call = populate.index("War3TryBuildShadowPacketFromCurrentDrawRecord(")
argument = populate.index("&currentDrawShadowObjectSnapshotCache", call)
assert construct < call < argument

print("direct shadow-object snapshot cache static tests passed")
