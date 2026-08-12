from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

start = DEVICE.index("// Object-grouped preselection resolves this exact part/layer")
end = DEVICE.index("if (packetBuildTiming != nullptr)\n    packetBuildTiming->enter(War3PacketBuildPhase::RenderableSetup)", start)
body = DEVICE[start:end]

# A preselected value is accepted only for the exact part/layer.
assert "preselectedVisibleRecord->renderablePart == record.renderablePart" in body
assert "preselectedVisibleRecord->layerIndex == record.layerIndex" in body

# Instance projection is restricted to complete current-frame aliases and
# immutable model identity. Any missing or mismatched field falls back to the
# existing generation-checked registry query.
for proof in (
    "visibleRecord.identity.worldObjectEntry == record.worldObjectEntry",
    "visibleRecord.identity.sceneNode == record.sceneNode",
    "visibleRecord.identity.unitPtr == record.unitPtr",
    "visibleRecord.identity.jHandle == record.jHandle",
    "visibleRecord.identity.rawcode == record.rawcode",
    "visibleRecord.runtimeModelPtr != nullptr",
    "visibleRecord.modelResourcePtr != nullptr",
    "visibleRecord.modelKey != 0u",
):
    assert proof in body

projection = body[body.index("if (visibleProvesInstance) {") :]
fallback = projection.index("if (!instanceHit) {")
assert "instanceRecord.runtimeModelPtr = visibleRecord.runtimeModelPtr" in projection[:fallback]
assert "instanceRegistry.findFirstForDirectPacketView(" in projection[fallback:]

# The same value copy is reused by renderable setup; no second exact-visible
# query runs on the proven path.
visible_fallback = body.index("if (!visibleHit) {")
assert visible_fallback > fallback
assert "visibleRegistry.queryFirstForDirectPacket(" in body[visible_fallback:]

print("preselected visible instance projection static checks passed")
