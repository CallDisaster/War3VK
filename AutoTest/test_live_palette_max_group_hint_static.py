from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
# M2-2：War3TryBuildLiveRuntimeGroupPalette 定义已迁往
# src/d3d9/war3/semantic/war3_live_palette_selection.cpp（逐字节）；
# 函数体断言跟随符号改锚到模块；device.cpp 侧的调用点断言保留在 DEVICE。
MODULE = (ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.cpp").read_text(
    encoding="utf-8"
)
MODULE_H = (ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.h").read_text(
    encoding="utf-8"
)

definition = MODULE[MODULE.index("bool War3TryBuildLiveRuntimeGroupPalette(\n") :]
definition = definition[: definition.index("auto resolvePaletteSlotIndex =")]

# 默认实参（sentinel）按迁移铁律集中在模块头声明；在模块头断言同一文本，
# 不削弱原断言（原断言锚在 device.cpp 的定义签名上，该签名当时携带默认实参）。
declaration = MODULE_H[MODULE_H.index("bool War3TryBuildLiveRuntimeGroupPalette(\n") :]
assert "uint32_t provenMaxVertexGroupSlot = 0xFFFFFFFFu" in declaration
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
