from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

populate = DEVICE.split(
    "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(", 1
)[1].split("uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", 1)[0]

record = populate.split("struct PreselectedRecord", 1)[1].split("};", 1)[0]
assert "War3CompactWorkItem" not in record
for field in (
    "uint32_t recordIndex",
    "uint32_t visibleHintIndex",
    "uint64_t selectionKey",
    "uint32_t priorityScore",
    "bool previouslySelected",
):
    assert field in record

assert "s_preselectedWorkByRecordIndex" in populate
clear = populate.index("preselectedWorkByRecordIndex.clear()")
mode_gate = populate.index(
    "compactWorkTableMode != War3CompactWorkTableMode::Off", clear
)
resize = populate.index("preselectedWorkByRecordIndex.resize(directRecords.size())", mode_gate)
store = populate.index("preselectedWorkByRecordIndex[recordIndex] = work", resize)
source = populate.index("preselectedRecords[i].recordIndex", store)
append = populate.index("preselectedWorkByRecordIndex[sourceRecordIndex]", source)
fallback = populate.index("m_war3CompactWorkTable.appendInvalid(1u)", append)
assert clear < mode_gate < resize < store < source < append < fallback
assert "preselectedRecords[i].work" not in populate

print("direct preselect compact-work sidecar static checks passed")
