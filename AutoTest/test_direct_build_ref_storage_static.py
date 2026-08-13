from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

populate = DEVICE.split(
    "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(", 1
)[1].split("uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", 1)[0]
storage = populate[populate.index("struct DirectRecordBuildRef") :]

assert "static_assert(sizeof(DirectRecordBuildRef) == 16u)" in storage
assert "static thread_local std::vector<DirectRecordBuildRef> s_recordBuildRefs" in storage
for old in (
    "s_recordIndicesForBuild",
    "s_recordSelectionKeysForBuild",
    "s_recordVisibleHintIndicesForBuild",
):
    assert old not in populate

append = storage.index("recordBuildRefs.push_back(")
assert "preselectedRecords[i].selectionKey" in storage[append : append + 300]
assert "preselectedRecords[i].recordIndex" in storage[append : append + 300]
assert "preselectedRecords[i].visibleHintIndex" in storage[append : append + 300]

fallback = storage.index('enterDirectDetailPhase("SnapshotFallbackCopy")')
loop = storage.index('enterBuildEligiblePhase("RecordLoop")', fallback)
assert "recordBuildRefs.resize(directRecords.size())" in storage[fallback:loop]
assert "recordBuildRefs[i].recordIndex = i" in storage[fallback:loop]

record_loop = storage[loop : storage.index("if (traceBuildEligible)", loop)]
assert "const DirectRecordBuildRef& buildRef = recordBuildRefs[buildIndex]" in record_loop
assert "buildRef.recordIndex" in record_loop
assert "buildRef.selectionKey" in record_loop
assert "buildRef.visibleHintIndex" in record_loop

print("direct build-ref storage static checks passed")
