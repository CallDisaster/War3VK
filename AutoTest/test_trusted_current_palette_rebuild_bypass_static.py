from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

install_start = DEVICE.index("outDirectCurrentDrawSample->paletteHash =")
install = DEVICE[install_start : install_start + 1000]
assert "outDirectCurrentDrawSample->paletteProvenance =" in install
assert "directCurrentDrawSample.paletteProvenance" in install

start = DEVICE.index("const auto drawTimeCapturedPaletteProvenance =")
end = DEVICE.index("fallbackAppendTiming.enter(\n      War3FallbackAppendPhase::InputGroupContract)", start)
body = DEVICE[start:end]

# Only the producer-backed same-tag source can bypass the second live rebuild.
assert "PaletteProvenance::TrustedBlendedWriter" in body
assert "drawTimeCapturedPaletteMinFrameTag != 0u" in body
assert "drawTimeCapturedPaletteMinFrameTag ==\n              drawTimeCapturedPaletteMaxFrameTag" in body
assert "QueryCurrentPaletteFrameTag(" in body
assert "currentPaletteFrameTag == drawTimeCapturedPaletteMinFrameTag" in body

attempt = body[body.index("const bool shouldAttempt =") : body.index("if (shouldAttempt)")]
assert "!capturedPaletteCurrentFrameProven" in attempt
assert "kSubmitLiveRebuildEveryFrame" in attempt
assert "kSubmitLiveRebuildLagThreshold" in attempt

# Raw/unknown palettes are not promoted by this optimization, and the normal
# rebuild still uses the exact existing function and fail-soft miss path.
assert "PaletteProvenance::RawGlobalArena" not in attempt
assert "War3TryBuildLiveRuntimeGroupPalette(" in body
assert "NoteSubmitLiveRebuildMiss()" in body

print("trusted current palette rebuild bypass static checks passed")
