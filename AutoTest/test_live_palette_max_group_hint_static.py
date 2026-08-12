from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

definition = DEVICE[DEVICE.index("bool War3TryBuildLiveRuntimeGroupPalette(\n", 200000) :]
definition = definition[: definition.index("auto resolvePaletteSlotIndex =")]

assert "uint32_t provenMaxVertexGroupSlot = 0xFFFFFFFFu" in definition
assert "if (provenMaxVertexGroupSlot < 256u)" in definition
assert "outMaxVertexGroupSlot = provenMaxVertexGroupSlot" in definition
scan = definition.index("for (const uint8_t groupSlot : vertexGroups)")
assert definition.index("} else {") < scan

# Submit and lease refresh may skip the O(vertexCount) scan only when the
# packet's sealed maximum fits its existing palette domain. Unknown callers
# continue to pass the sentinel and execute the exact scan.
assert "packet.maxVertexGroupSlot < drawTimeCapturedPaletteCount" in DEVICE
assert DEVICE.count("leased.packet.hasRuntimeGroupPalette &&") >= 2
assert DEVICE.count("leased.packet.maxVertexGroupSlot <") >= 2
assert DEVICE.count("? leased.packet.maxVertexGroupSlot") >= 2

print("live palette max-group hint static checks passed")
