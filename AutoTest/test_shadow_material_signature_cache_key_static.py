#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


def function(signature: str) -> str:
    start = SOURCE.index(signature)
    brace = SOURCE.index("{", start)
    depth = 0
    for index in range(brace, len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


cache = function("War3BuildShadowMaterialSignatureCached(")
assert "uint64_t provenMapEpoch = 0u" in cache
assert "provenMapEpoch != 0u" in cache
assert "ShadowModelResourceCache::instance().mapEpoch()" in cache

# The canonical resolver does not consume the draw-local layerState view.
# A fallback signature does consume it, so it must retain exact matching.
hash_end = cache.index("static thread_local std::array<CacheEntry, 16384>")
hash_setup = cache[:hash_end]
assert "fnv1a_iter(hash, reinterpret_cast<uintptr_t>(renderable.layerState))" not in hash_setup
assert "entry.signature.layerContractResolved ||" in cache
assert "entry.layerState == renderable.layerState" in cache
assert "entry.layerState = renderable.layerState" in cache
assert "void* meshData = nullptr;" not in cache
assert "entry.meshData" not in cache

packet = function("bool War3TryBuildShadowPacketFromCurrentDrawRecord(")
assert "War3BuildShadowMaterialSignatureCached(\n        renderable, resource.mapEpoch)" in packet

print("shadow material signature cache key static checks passed")
