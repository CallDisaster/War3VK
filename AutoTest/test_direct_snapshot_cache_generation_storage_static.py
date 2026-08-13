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
    compact = "".join(cache.split())
    assert "void reset() noexcept" in cache
    assert "++m_populationGeneration" in cache
    assert "m_populationGeneration == 0u" in cache
    assert "m_entries = {};" in cache
    assert "entry.populationGeneration == m_populationGeneration" in cache
    assert "entry.registryGeneration == currentGeneration" in cache
    assert "m_populationGeneration,observedGeneration" in compact
    assert "uint64_t populationGeneration = 0u" in cache
    assert "uint64_t registryGeneration" in cache

populate = DEVICE.split(
    "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(", 1
)[1].split("uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", 1)[0]
for name in (
    "currentDrawInstanceSnapshotCache",
    "currentDrawShadowObjectSnapshotCache",
    "currentDrawPoseAugmentSnapshotCache",
):
    declaration = populate.index("static thread_local", populate.index(name) - 80)
    reset = populate.index(f"{name}.reset()", declaration)
    use = populate.index(f"&{name}", reset)
    assert declaration < reset < use

print("direct snapshot cache generation storage static checks passed")
