from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOOK_H = (ROOT / "src/d3d9/war3/model/war3_model_hook.h").read_text(encoding="utf-8")
HOOK_CPP = (ROOT / "src/d3d9/war3/model/war3_model_hook.cpp").read_text(encoding="utf-8")
CURRENT_DRAW = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(encoding="utf-8")
PACK = (ROOT / "src/d3d9/war3/model/war3_palette_pack.h").read_text(encoding="utf-8")

assert "CopyBlendedPaletteBytesBySlotIndexExact" in HOOK_H
validate_start = HOOK_CPP.index("bool ValidateBlendedPaletteBySlotIndexExact")
validate_end = HOOK_CPP.index("bool QueryBlendedPaletteBySlotIndexExact", validate_start)
validate = HOOK_CPP[validate_start:validate_end]
assert "entry.valid" in validate
assert "delta > 2u" in validate
assert "PackWar3PaletteMatrix3x4" not in validate

copy_start = HOOK_CPP.index("bool CopyBlendedPaletteBytesBySlotIndexExact")
copy_end = HOOK_CPP.index("// Phase 7.34", copy_start)
copy_body = HOOK_CPP[copy_start:copy_end]
assert "outPaletteByteCapacity" in copy_body
assert "ValidateBlendedPaletteBySlotIndexExact" in copy_body
assert "PackWar3PaletteMatrix3x4" in copy_body
assert copy_body.index("ValidateBlendedPaletteBySlotIndexExact") < copy_body.index("PackWar3PaletteMatrix3x4")

publish_start = CURRENT_DRAW.index("const size_t snapshotSlot =", CURRENT_DRAW.index("void PublishCurrentDrawContract"))
publish_end = CURRENT_DRAW.index("CurrentDrawFixedPhaseScope globalMapsPhase", publish_start)
publish = CURRENT_DRAW[publish_start:publish_end]
assert "CopyBlendedPaletteBytesBySlotIndexExact" in publish
assert "std::vector<Matrix4> trustedPalette" not in publish
assert "for (uint32_t i = 0u; i < record.capturedPaletteCount" not in publish
assert "snapshot.bytes.data(), snapshot.bytes.size()" in publish
assert "paletteAlreadyStoredInSnapshot" in publish
assert "if (!paletteAlreadyStoredInSnapshot)" in publish
assert "PaletteProvenance::TrustedBlendedWriter" in publish
assert "PaletteProvenance::RawGlobalArena" in publish

assert "std::memcpy" in PACK
assert "3u * sizeof(float)" in PACK
print("current-draw trusted palette direct-pack static checks passed")
