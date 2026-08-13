from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


def cache_body(start: str, end: str) -> str:
    return DEVICE.split(start, 1)[1].split(end, 1)[0]


caches = (
    cache_body(
        "class War3CurrentDrawInstanceSnapshotCache final",
        "// PoseAugmentView is the allocation-free subset",
    ),
    cache_body(
        "class War3CurrentDrawPoseAugmentSnapshotCache final",
        "// ShadowObject identity/pose metadata",
    ),
    cache_body(
        "class War3CurrentDrawShadowObjectSnapshotCache final",
        "bool War3TryAttachCurrentDrawVisibleIndexSlice",
    ),
)

for cache in caches:
    assert "const auto cachedValue = entry.value" not in cache
    assert "const bool cachedFound = entry.found" not in cache
    generation = cache.index("registry.mutationGeneration() == currentGeneration")
    copy = cache.index("out = entry.value", generation)
    result = cache.index("return entry.found", copy)
    assert generation < copy < result

print("direct snapshot cache single-copy static checks passed")
