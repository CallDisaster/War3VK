from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
CURRENT = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(
    encoding="utf-8"
)
HEADER = (ROOT / "src/d3d9/war3/render/war3_visible_renderables.h").read_text(
    encoding="utf-8"
)
VISIBLE = (ROOT / "src/d3d9/war3/render/war3_visible_renderables.cpp").read_text(
    encoding="utf-8"
)


assert "class VisibleRenderablePartLayerQueryCache" in HEADER
assert "static constexpr size_t kEntryCount = 512u" in HEADER
assert "valid only until the next query or reset" in HEADER
assert "entry.renderablePart == renderablePart" in VISIBLE
assert "entry.layerIndex == layerIndex" in VISIBLE
assert "registry.queryByRenderablePartAndLayer(" in VISIBLE

snapshot = CURRENT[CURRENT.index("void SnapshotPublishedCurrentDrawContracts(") :]
assert "options.visiblePartLayerQueryCache->query(" in snapshot
assert "visibleRegistry.queryByRenderablePartAndLayer(" in snapshot

populate_start = DEVICE.index(
    "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped("
)
populate_end = DEVICE.index(
    "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", populate_start
)
populate = DEVICE[populate_start:populate_end]
reset = populate.index("visiblePartLayerQueryCache.reset()")
snapshot_publish = populate.index(
    "snapshotOptions.visiblePartLayerQueryCache ="
)
selection = populate.index(
    "record, &visibleHint, &visiblePartLayerQueryCache"
)
assert reset < snapshot_publish < selection

builder_start = DEVICE.index("uint64_t War3SemanticDirectRecordSelectionKey(")
builder_end = DEVICE.index("uint64_t War3ProducerClaimObserveObjectKey(", builder_start)
builder = DEVICE[builder_start:builder_end]
assert "visibleQueryCache->queryPtr(" in builder
assert "registry.queryByRenderablePartAndLayer(" in builder

print("direct visible part/layer query cache static checks passed")
