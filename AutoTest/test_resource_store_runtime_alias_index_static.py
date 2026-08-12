from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp"
).read_text(encoding="utf-8")

ADD = SOURCE.split(
    "void ShadowModelResourceStore::add(ShadowModelResourceRecord record)", 1
)[1].split("void ShadowModelResourceStore::bindRuntimeModelAlias", 1)[0]
ALIAS = SOURCE.split(
    "void ShadowModelResourceStore::bindRuntimeModelAlias", 1
)[1].split("bool ShadowModelResourceStore::findByRuntimeGeoset", 1)[0]

# The exact model/geoset index must continue to prefer a runtime-authoritative
# record, which is what makes a second linear search unnecessary.
assert "!m_records[itExisting->second].prefersRuntimeContract" in ADD
assert "stored.prefersRuntimeContract" in ADD

NON_NULL = ALIAS.split("if (modelResourcePtr != nullptr)", 1)[1].split(
    "for (size_t i = 0; i < m_records.size(); ++i)", 1
)[0]
assert "m_byModelResource.find({modelResourcePtr, geosetIndex})" in NON_NULL
assert "m_byRuntimeModel[{runtimeModelPtr, geosetIndex}] = it->second" in NON_NULL
assert "return;" in NON_NULL
assert "for (" not in NON_NULL
assert NON_NULL.count("m_byModelResource.find") == 1

# Only the intentionally unscoped/null-resource compatibility route may scan
# records.  Non-null model identities must never fall through to O(N) lookup.
assert ALIAS.count("for (size_t i = 0; i < m_records.size(); ++i)") == 2
assert "modelResourcePtr == nullptr ||" not in ALIAS

print("resource-store runtime alias exact-index contract passed")
