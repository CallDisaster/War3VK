from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
# M1 迁移：记录级 selector 已迁到语义模块，断言改读新位置（内容与判定顺序不变）。
SEMANTIC = (
    ROOT / "src/d3d9/war3/semantic/war3_device_semantic_predicates.cpp"
).read_text(encoding="utf-8")

selector = SEMANTIC.split("uint64_t War3SemanticDirectRecordSelectionKey(", 1)[1].split(
    "struct War3SemanticUnitValidationCacheEntry", 1
)[0]
assert "const dxvk::war3::render::VisibleRenderableRecord** outVisibleHint" in selector
assert "*outVisibleHint = nullptr;" in selector
query = selector.index("visibleQueryCache->queryPtr(")
gate = selector.index("outVisibleHint != nullptr && visibleQueryCache != nullptr", query)
handoff = selector.index("*outVisibleHint = visible;", gate)
assert query < gate < handoff
assert "*outVisibleHint = *visible" not in selector

populate = DEVICE.split(
    "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(", 1
)[1].split("uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", 1)[0]
declaration = populate.index(
    "const dxvk::war3::render::VisibleRenderableRecord* visibleHint = nullptr"
)
selection = populate.index("record, &visibleHint, &visiblePartLayerQueryCache", declaration)
validate = populate.index("visibleHint->renderablePart == record.renderablePart", selection)
copy = populate.index("preselectedVisibleHints.push_back(*visibleHint)", validate)
next_query_boundary = populate.index("preselectedRecords.push_back", copy)
assert declaration < selection < validate < copy < next_query_boundary

print("direct preselect visible pointer handoff static checks passed")
