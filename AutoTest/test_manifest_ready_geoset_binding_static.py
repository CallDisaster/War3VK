#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.h"
CACHE_CPP = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.cpp"
CONTRACT_CPP = ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp"

header = HEADER.read_text(encoding="utf-8")
cache = CACHE_CPP.read_text(encoding="utf-8")
contract = CONTRACT_CPP.read_text(encoding="utf-8")

binding = header.split("struct ShadowReadyGeosetBinding", 1)[1].split("};", 1)[0]
for field in ("geosetPtr", "geosetDataPtr", "modelResourcePtr",
              "modelKey", "geosetIndex"):
    assert field in binding
assert "std::vector" not in binding
assert "std::is_trivially_copyable_v<ShadowReadyGeosetBinding>" in header

by_ptr = cache.split(
    "ShadowModelResourceCache::findReadyGeosetBindingByPtr(", 1
)[1].split("findReadyGeosetBindingByData(", 1)[0]
assert "m_byGeoset.find" in by_ptr
assert "m_byGeosetData.find" in by_ptr
assert by_ptr.count("readyForShadowConsumer()") >= 2
assert "CopyReadyGeosetBinding" in by_ptr
assert "MergeReadyGeosetAlias" in by_ptr
assert "materializeGeosetAliasRecordLocked" not in by_ptr

by_data = cache.split(
    "ShadowModelResourceCache::findReadyGeosetBindingByData(", 1
)[1].split("findGeosetSnapshotByData(", 1)[0]
assert "m_byGeosetData.find" in by_data
assert "readyForShadowConsumer()" in by_data
assert "CopyReadyGeosetBinding" in by_data
assert "materializeGeosetDataRecordLocked" not in by_data

backfill = contract.split(
    "bool BackfillVisibleUnitGeosetBindingFromCache(", 1
)[1].split("void MaybePublishVisibleUnitGeosetBinding", 1)[0]
assert "ShadowReadyGeosetBinding" in backfill
assert "findReadyGeosetBindingByPtr" in backfill
assert "findReadyGeosetBindingByData" in backfill
assert "ShadowGeosetResourceRecord" not in backfill
assert "findGeosetByPtr" not in backfill
assert "findGeosetByData" not in backfill

print("manifest ready geoset binding static checks passed")
