from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/d3d9/war3/render/war3_visible_renderables.h").read_text(
    encoding="utf-8"
)
VISIBLE = (ROOT / "src/d3d9/war3/render/war3_visible_renderables.cpp").read_text(
    encoding="utf-8"
)
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

assert "const VisibleRenderableRecord* queryPtr(" in HEADER
assert "valid only until the next query or reset" in HEADER

ptr_body = VISIBLE.split(
    "const VisibleRenderableRecord* VisibleRenderablePartLayerQueryCache::queryPtr", 1
)[1].split("bool VisibleRenderablePartLayerQueryCache::query(", 1)[0]
assert "entry.record = {};" in ptr_body
assert "entry.record);" in ptr_body
assert "return entry.found ? &entry.record : nullptr;" in ptr_body
assert "VisibleRenderableRecord record" not in ptr_body

copy_body = VISIBLE.split(
    "bool VisibleRenderablePartLayerQueryCache::query(", 1
)[1].split("bool VisibleRenderableRegistry::queryFirstForDirectPacket", 1)[0]
assert "queryPtr(registry, renderablePart, layerIndex)" in copy_body
assert "out = *record;" in copy_body

# M1 迁移：记录级 selector 已迁到语义模块（内容与判定顺序不变）。
SEMANTIC = (
    ROOT / "src/d3d9/war3/semantic/war3_device_semantic_predicates.cpp"
).read_text(encoding="utf-8")
selection = SEMANTIC.split("uint64_t War3SemanticDirectRecordSelectionKey(", 1)[1].split(
    "struct War3SemanticUnitValidationCacheEntry", 1
)[0]
assert "visibleQueryCache->queryPtr(" in selection
assert "*outVisibleHint = visible;" in selection
assert "visibleQueryCache->query(registry" not in selection

print("visible part/layer query cache value-copy static checks passed")
