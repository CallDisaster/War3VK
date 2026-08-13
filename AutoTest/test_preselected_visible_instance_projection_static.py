from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

start = DEVICE.index("// Object-grouped preselection resolves this exact part/layer")
end = DEVICE.index("if (packetBuildTiming != nullptr)\n    packetBuildTiming->enter(War3PacketBuildPhase::RenderableSetup)", start)
body = DEVICE[start:end]

# A preselected value is accepted only for the exact part/layer.
assert "preselectedVisibleRecord->renderablePart == record.renderablePart" in body
assert "preselectedVisibleRecord->layerIndex == record.layerIndex" in body

# A pure contract accepts partial CurrentDraw identity only when at least one
# strong alias exists, every supplied alias agrees, and the visible model owner
# is complete. Shared parts without instance identity retain registry lookup.
assert "War3VisibleInstanceProjectionFacts" in body
assert "War3CanProjectVisibleInstance(" in body
policy = (ROOT / "src/d3d9/war3/render/war3_visible_instance_projection.h").read_text(
    encoding="utf-8"
)
for proof in (
    "facts.recordWorldObjectEntry != nullptr",
    "facts.recordSceneNode != nullptr",
    "facts.recordUnitPtr != nullptr",
    "facts.recordJHandle != 0u",
    "facts.visibleRuntimeModelPtr == nullptr",
    "facts.visibleModelResourcePtr == nullptr",
    "facts.visibleModelKey == 0u",
):
    assert proof in policy

projection = body[body.index("if (visibleProvesInstance) {") :]
fallback = projection.index("if (!instanceHit) {")
assert "instanceRecord.runtimeModelPtr = visibleRecord->runtimeModelPtr" in projection[:fallback]
assert "instanceRegistry.findFirstForDirectPacketView(" in projection[fallback:]

# The same value copy is reused by renderable setup; no second exact-visible
# query runs on the proven path.
visible_fallback = body.index("if (!visibleHit) {")
assert visible_fallback > fallback
assert "visibleRegistry.queryFirstForDirectPacket(" in body[visible_fallback:]

print("preselected visible instance projection static checks passed")
