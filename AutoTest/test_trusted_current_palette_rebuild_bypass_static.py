from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

install_start = DEVICE.index("outDirectCurrentDrawSample->paletteHash =")
install = DEVICE[install_start : install_start + 1000]
assert "outDirectCurrentDrawSample->paletteProvenance =" in install
assert "directCurrentDrawSample.paletteProvenance" in install

# The provenance tracker is intentionally mutable: the F855 skin-palette
# contract integration reassigns it after a contract-gated rebuild
# (ProducerPartPacket for OwnedPartSnapshot selections, otherwise Unknown).
start = DEVICE.index("auto drawTimeCapturedPaletteProvenance =")
end = DEVICE.index("fallbackAppendTiming.enter(\n      War3FallbackAppendPhase::InputGroupContract)", start)
body = DEVICE[start:end]

# 2026-09-19 二次裁定（路径 i）：检查的**组合规则**已搬进共享头
# （war3/render/war3_palette_object_diagnostics.h 的 NoteCapturedPaletteCurrentFrameEvidence），
# 使宿主测试能执行同一段代码。按位置分别断言：
#   · device 侧：仍把**既有**原值（同一帧标签范围 + 同一处读取）传进去（零新增读取）；
#   · 头侧：组合规则本身仍要求 非零 + min==max + 观测==min。
HEADER = (ROOT / "src/d3d9/war3/render/war3_palette_object_diagnostics.h").read_text(encoding="utf-8")
assert "drawTimeCapturedPaletteMinFrameTag" in body
assert "drawTimeCapturedPaletteMaxFrameTag" in body
assert "QueryCurrentPaletteFrameTag(" in body
assert "currentPaletteFrameTag" in body
_helper = HEADER[HEADER.index("inline void NoteCapturedPaletteCurrentFrameEvidence("):]
_helper = _helper[:_helper.index("inline ExportedNativeFrame ExportedNativeFrameFor")]
assert "minFrameTag != 0u" in _helper
assert "minFrameTag == maxFrameTag" in _helper
assert "observedFrameTag == minFrameTag" in _helper

attempt = body[body.index("const bool shouldAttempt =") : body.index("if (shouldAttempt)")]
assert "!capturedPaletteCurrentFrameProven" in attempt
assert "kSubmitLiveRebuildEveryFrame" in attempt
assert "kSubmitLiveRebuildLagThreshold" in attempt

# Raw/unknown palettes are not promoted by this optimization, and the normal
# rebuild still uses the exact existing function and fail-soft miss path.
assert "PaletteProvenance::RawGlobalArena" not in attempt
assert "War3TryBuildLiveRuntimeGroupPalette(" in body
assert "NoteSubmitLiveRebuildMiss()" in body

# F855 contract integration: when the skin-palette contract is enabled, a
# successful rebuild may only replace the selected palette through the
# contract's CanReplace gate, and the provenance tracker is updated from the
# rebuild selection rather than keeping the old label.
assert "dxvk::war3::render::skin::ContractEnabled()" in body
assert "dxvk::war3::render::skin::CanReplace(selectedPalette, rebuildSelection)" in body
assert "drawTimeCapturedPaletteProvenance =\n" in body

print("trusted current palette rebuild bypass static checks passed")
