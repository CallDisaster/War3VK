from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.h"
CONTRACT = ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp"

header = HEADER.read_text(encoding="utf-8")
contract = CONTRACT.read_text(encoding="utf-8")

visitor = header.split("void forEachGeosetContractSource(Fn&& fn) const", 1)[1].split(
    "std::vector<ShadowModelResourceRecord> snapshotModels() const", 1
)[0]
assert "std::shared_lock<std::shared_mutex> lock(m_mutex)" in visitor
assert "fn(*canonical->second, &alias)" in visitor
assert "fn(alias, nullptr)" in visitor
assert "fn(record, nullptr)" in visitor
assert "materializeGeosetAliasRecordLocked" not in visitor
assert "ShadowGeosetResourceRecord result" not in visitor

convert = contract.split("ShadowModelResourceRecord ConvertGeoset(", 1)[1].split(
    "ShadowPoseRecord ConvertPose(", 1
)[0]
for field in (
    "geosetPtr",
    "geosetDataPtr",
    "modelResourcePtr",
    "modelKey",
    "prefersRuntimeContract",
    "geosetIndex",
    "mapEpoch",
):
    assert f"alias->{field}" in convert
for payload in (
    "positions",
    "normals",
    "vertexGroupIndices",
    "primitiveRecords",
    "matrixGroupSizes",
    "matrixIndices",
    "indices",
    "uvLayers",
):
    assert f"src.{payload}" in convert

build = contract.split("auto buildResourceStore =", 1)[1].split(
    "std::shared_ptr<ShadowModelResourceStore> resourcesPtr", 1
)[0]
assert "forEachGeosetContractSource(" in build
assert "ConvertGeoset(geoset, manifest.frameSerial, alias)" in build
assert "snapshotGeosets()" not in build

# Keep the legacy copying snapshot available for non-hot diagnostic callers;
# this optimization changes only ResourceStoreBuild ownership handoff.
assert "snapshotGeosets() const" in header

print("resource store direct geoset source static contract passed")
