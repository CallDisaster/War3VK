from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

populate = DEVICE.split(
    "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(", 1
)[1].split("uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", 1)[0]
record_loop = populate.split("for (size_t buildIndex = 0u;", 1)[1]
manifest = record_loop.index("auto manifestRecord =")
part_key = record_loop.index("computeShadowManifestPartKey(manifestRecord)", manifest)
object_key = record_loop.index("computeShadowManifestObjectKey(manifestRecord)", part_key)
handoff = record_loop.index(
    "shadowEligibleManifestRecords.push_back(std::move(manifestRecord))",
    object_key,
)
previous_lookup = record_loop.index(
    "m_war3SemanticDirectPrevSubmittedPartIdentityKeys.empty()", handoff
)

assert manifest < part_key < object_key < handoff < previous_lookup
assert "shadowEligibleManifestRecords.push_back(manifestRecord)" not in record_loop
assert "if (!eligible.sample.contract.fromGrace)" in record_loop[
    object_key:handoff
]

# A moved record must not be consulted again in the rest of this record's
# identity/append path.
tail = record_loop[handoff:record_loop.index("eligibleRecords.push_back", handoff)]
assert tail.count("manifestRecord") == 1

print("direct manifest move handoff static checks passed")
