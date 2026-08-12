#!/usr/bin/env python3
"""Static contract for the scene-local Direct instance projection cache."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "src/d3d9/war3/model/war3_model_registry.h").read_text(
    encoding="utf-8"
)
SOURCE = (ROOT / "src/d3d9/war3/model/war3_model_registry.cpp").read_text(
    encoding="utf-8"
)


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


view_start = HEADER.index("struct ModelInstanceDirectPacketView")
view_end = HEADER.index("};", view_start)
view = HEADER[view_start:view_end]
for field in (
    "worldObjectEntry",
    "sceneNode",
    "unitPtr",
    "runtimeModelPtr",
    "modelResourcePtr",
    "jHandle",
    "rawcode",
    "modelKey",
):
    assert field in view
for excluded in ("std::vector", "std::string", "lastSeenFrame"):
    assert excluded not in view

lookup = body(SOURCE, "bool ModelInstanceRegistry::findFirstForDirectPacketView(")
assert lookup.count("std::shared_lock<std::shared_mutex> lock(m_mutex)") == 1
assert "m_mutationGeneration.load(std::memory_order_acquire)" in lookup
priority = [
    lookup.index("m_bySceneNode"),
    lookup.index("m_byUnitPtr"),
    lookup.index("m_byWorldObjectEntry"),
    lookup.index("m_byRuntimeModel"),
]
assert priority == sorted(priority)
assert "ProjectModelInstanceDirectPacketView(*record, out)" in lookup

cache_start = DEVICE.index("class War3CurrentDrawInstanceSnapshotCache final")
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
assert "ModelInstanceDirectPacketView instanceRecord" in builder
assert "instanceSnapshotCache->find" in builder
assert "findFirstForDirectPacketView" in builder
assert "instanceRegistry.findFirstForDirectPacket(" not in builder

populate = body(DEVICE, "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(")
construct = populate.index("War3CurrentDrawInstanceSnapshotCache")
call = populate.index("War3TryBuildShadowPacketFromCurrentDrawRecord(")
argument = populate.index("&currentDrawInstanceSnapshotCache", call)
assert construct < call < argument

print("direct instance snapshot cache static tests passed")
