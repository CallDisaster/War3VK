from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.h"
CACHE = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.cpp"
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
CONTRACT = ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp"
VISIBLE = ROOT / "src/d3d9/war3/render/war3_visible_renderables.cpp"
CORE = ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp"


header = HEADER.read_text(encoding="utf-8")
cache = CACHE.read_text(encoding="utf-8")
device = DEVICE.read_text(encoding="utf-8")
contract = CONTRACT.read_text(encoding="utf-8")
visible = VISIBLE.read_text(encoding="utf-8")
core = CORE.read_text(encoding="utf-8")

binding = header.split("struct ShadowRuntimeModelOwnerBinding", 1)[1].split(
    "static_assert", 1
)[0]
for scalar in ("runtimeModelPtr", "modelResourcePtr", "modelKey", "geosetCount"):
    assert scalar in binding
for payload in ("geosetPtrs", "geosetDataPtrs", "std::vector"):
    assert payload not in binding
assert "std::is_trivially_copyable_v<ShadowRuntimeModelOwnerBinding>" in header

indexed = cache.split(
    "bool ShadowModelResourceCache::findRuntimeModelOwnerBindingIndexed(", 1
)[1].split("bool ShadowModelResourceCache::findModelResource(", 1)[0]
assert "out = itRuntime->second" not in indexed
for scalar in ("runtimeModelPtr", "modelResourcePtr", "modelKey", "geosetCount"):
    assert f"out.{scalar} = record.{scalar}" in indexed

scored = cache.split(
    "bool ShadowModelResourceCache::findRuntimeModelOwnerBinding(", 1
)[1].split(
    "bool ShadowModelResourceCache::findRuntimeModelOwnerBindingIndexed(", 1
)[0]
assert "ScoreRuntimeOwnerCandidate(" in scored
assert "bestScore" in scored and "ambiguous" in scored
assert "out = itRuntime->second" not in scored

for lookup, next_lookup in (
    ("findModelBinding(", "findRuntimeModelBinding("),
    ("findRuntimeModelBinding(", "findModelResource("),
):
    body = cache.split(f"bool ShadowModelResourceCache::{lookup}", 1)[1].split(
        f"bool ShadowModelResourceCache::{next_lookup}", 1
    )[0]
    assert "ShadowModelResourceRecord& record" in body
    assert "out = it" not in body
    for scalar in ("runtimeModelPtr", "modelResourcePtr", "modelKey", "geosetCount"):
        assert f"out.{scalar} = record.{scalar}" in body

builder = device.split(
    "bool War3TryBuildShadowPacketFromCurrentDrawRecord(", 1
)[1].split("bool War3TryResolveDirectRenderable", 1)[0]
initial_owner = builder.split("War3PacketBuildPhase::OwnerInstanceLookup", 1)[1].split(
    "War3PacketBuildPhase::GeosetFallbacks", 1
)[0]
assert "ShadowRuntimeModelOwnerBinding ownerBinding" in initial_owner
assert "findRuntimeModelOwnerBindingIndexed(" in initial_owner
assert "ShadowModelResourceRecord ownerRecord" not in initial_owner
assert "findRuntimeModelResource(" not in initial_owner

# Full pointer arrays remain available only inside the uncommon geoset-miss
# recovery block; this preserves its exact lookup order without taxing hits.
fallback = builder.split("War3PacketBuildPhase::GeosetFallbacks", 1)[1].split(
    "War3PacketBuildPhase::RegistryLookups", 1
)[0]
assert "ShadowModelResourceRecord ownerRecord" in fallback
assert "findRuntimeModelResource(ownerBinding.runtimeModelPtr" in fallback

hydrate = contract.split("void HydrateManifestRuntimeOwnersFromIndexedCache(", 1)[1].split(
    "ShadowModelResourceRecord ConvertGeoset(", 1
)[0]
assert "ShadowRuntimeModelOwnerBinding owner" in hydrate
assert "findRuntimeModelOwnerBindingIndexed(" in hydrate
assert "ShadowModelResourceRecord owner" not in hydrate

visible_owner = visible.split("bool ResolveRuntimeOwnerFromGeosetBinding(", 1)[1].split(
    "void ResolveModelMetadata(", 1
)[0]
assert "ShadowRuntimeModelOwnerBinding runtimeOwner" in visible_owner
assert "findRuntimeModelOwnerBinding(" in visible_owner
assert "ShadowModelResourceRecord runtimeOwner" not in visible_owner

metadata = visible.split("void ResolveModelMetadata(", 1)[1].split(
    "void ResolveUnitMetadata(", 1
)[0]
assert metadata.count("ShadowRuntimeModelOwnerBinding") >= 3
assert "findRuntimeModelBinding(" in metadata
assert "findModelBinding(" in metadata

# Renderer fallbacks that need only a runtime/model identity use the scalar
# owner contract. The two owner-resource recovery paths that inspect geoset
# arrays deliberately keep their full-record lookup.
assert core.count("findRuntimeModelOwnerBinding(") >= 6
assert core.count("findRuntimeModelOwner(") == 2

print("direct owner scalar binding static contract passed")
