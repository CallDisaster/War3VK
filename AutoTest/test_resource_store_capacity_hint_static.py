from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CACHE_H = (ROOT / "src/d3d9/war3/model/war3_model_resource_cache.h").read_text(
    encoding="utf-8"
)
CACHE_CPP = (
    ROOT / "src/d3d9/war3/model/war3_model_resource_cache.cpp"
).read_text(encoding="utf-8")
STORE_H = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.h"
).read_text(encoding="utf-8")
STORE_CPP = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp"
).read_text(encoding="utf-8")

assert "struct ShadowResourceStoreCapacityHint" in CACHE_H
assert "resourceStoreCapacityHint() const" in CACHE_H

HINT = CACHE_CPP.split(
    "ShadowModelResourceCache::resourceStoreCapacityHint() const", 1
)[1].split("ShadowModelResourceCache::memorySnapshot() const", 1)[0]
assert "std::shared_lock<std::shared_mutex> lock(m_mutex)" in HINT
assert "m_byGeoset.size() + m_byGeosetData.size()" in HINT
assert "std::numeric_limits<size_t>::max" in HINT
assert "m_byModelResource" in HINT and "m_byRuntimeModel" in HINT
assert "runtimeModelPtr == nullptr" in HINT

assert "void reserve(size_t geosetRecordCapacity" in STORE_H
RESERVE = STORE_CPP.split("void ShadowModelResourceStore::reserve", 1)[1].split(
    "void ShadowModelResourceStore::add", 1
)[0]
for container in (
    "m_records",
    "m_byRuntimeGeoset",
    "m_byRuntimeGeosetData",
    "m_byModelResource",
    "m_byRuntimeModel",
):
    assert f"{container}.reserve" in RESERVE

BUILD = STORE_CPP.split("auto buildResourceStore", 1)[1].split(
    "std::shared_ptr<ShadowModelResourceStore> resourcesPtr", 1
)[0]
assert BUILD.index("resourceStoreCapacityHint()") < BUILD.index(
    "forEachGeosetContractSource"
)
assert BUILD.index("freshResources->reserve") < BUILD.index(
    "forEachGeosetContractSource"
)

print("resource-store capacity hint static contract passed")
