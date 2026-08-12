#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.h"
CACHE_CPP = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.cpp"
CONTRACT_CPP = ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp"

header = HEADER.read_text(encoding="utf-8")
cache = CACHE_CPP.read_text(encoding="utf-8")
contract = CONTRACT_CPP.read_text(encoding="utf-8")

record = header.split("struct ShadowModelAliasSnapshotRecord", 1)[1].split(
    "};", 1
)[0]
assert "runtimeModelPtr" in record
assert "modelResourcePtr" in record
assert "geosetCount" in record
assert "std::vector" not in record
assert "std::is_trivially_copyable_v<ShadowModelAliasSnapshotRecord>" in header

model_snapshot = cache.split(
    "ShadowModelResourceCache::snapshotModelAliases() const", 1
)[1].split("snapshotRuntimeModelAliases() const", 1)[0]
runtime_snapshot = cache.split(
    "ShadowModelResourceCache::snapshotRuntimeModelAliases() const", 1
)[1].split("size_t ShadowModelResourceCache::geosetRecordCount", 1)[0]
for body, source in ((model_snapshot, "m_byModelResource"),
                     (runtime_snapshot, "m_byRuntimeModel")):
    assert source in body
    assert "std::shared_lock<std::shared_mutex>" in body
    assert "record.runtimeModelPtr" in body
    assert "record.modelResourcePtr" in body
    assert "record.geosetCount" in body
    assert "record.geosetPtrs" not in body
    assert "record.geosetDataPtrs" not in body

resource_build = contract.split("auto buildResourceStore =", 1)[1].split(
    "return freshResources;", 1
)[0]
assert "snapshotModelAliases()" in resource_build
assert "snapshotRuntimeModelAliases()" in resource_build
assert "snapshotModels()" not in resource_build
assert "snapshotRuntimeModels()" not in resource_build

cold_sweep = contract.split("constexpr size_t kColdStartSweepBudget", 1)[1].split(
    "bool TryResolveRuntimeModelSemanticKey", 1
)[0]
assert "snapshotRuntimeModelAliases()" in cold_sweep
assert "snapshotRuntimeModels()" not in cold_sweep

print("resource store scalar alias snapshot static checks passed")
