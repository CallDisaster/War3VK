from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

start = DEVICE.index("// 首选：从 Hook_RuntimeMatrixWrite")
end = DEVICE.index("buildTiming.enter(War3LivePaletteBuildPhase::PoseFallback)", start)
body = DEVICE[start:end]

assert "bool slotFrameTagQueried = false" in body
assert "auto ensureSlotFrameTags = [&]()" in body
assert "if (slotFrameTagQueried)" in body
assert "slotFrameTagQueried = true" in body
assert "QueryBlendedPaletteFrameTagRange(" in body

# The range walk is lazy: only publishing a fallback tag calls it. A producer
# snapshot with its own frame tag returns without invoking the range walk.
publish = body[body.index("auto publishSlotFrameTags = [&]()") :]
assert publish.index("ensureSlotFrameTags();") < publish.index("if (!slotFrameTagReady)")
snapshot = body[body.index("QueryRenderablePartPaletteSnapshot(") :]
snapshot_return = snapshot.index("return true;")
snapshot_success = snapshot[:snapshot_return]
assert "producerPartFrameTag != 0u" in snapshot_success
assert "publishSlotFrameTags();" in snapshot_success
assert snapshot_success.index("producerPartFrameTag != 0u") < snapshot_success.index(
    "publishSlotFrameTags();"
)

# Every later global/blended fallback that can publish a palette still asks
# for the same slot range before returning.
assert body.count("publishSlotFrameTags();") >= 3

print("live palette frame-tag lazy fallback static checks passed")
