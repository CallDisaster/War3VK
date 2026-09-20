// AUTO-GENERATED legacy reference for the A9 dead-code-ruling migration.
//
// DO NOT EDIT. This file is the verbatim pre-migration text of the dead-code
// function War3SemanticBuildWorldPaletteIfNeeded that the 2026-09-18 dead-code
// ruling moved (NOT deleted) out of src/d3d9/d3d9_device.cpp (:7161-7175, where
// it was marked [[maybe_unused]] with zero callers) into
// src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}.
// Regenerate with:
//   py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --a9
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-A9 work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\device_pre_a9.cpp)
//   SHA-256(pre-A9 work-tree d3d9_device.cpp) = FB4D2FF61DC75EC1A8DAFEC2DCFD2A1CE51F101B16399BE1383F4B035555716C
//   size = 2248823 bytes, captured 2026-09-18T04:49:48 (machine local time)
//   The pre-A9 state equals the post-A10 work tree (same SHA as the A10 step's
//   post-migration d3d9_device.cpp); like M1/M2-x this is a work-tree file hash,
//   not a git blob hash (the work is uncommitted).
//
// Namespace rewrite rules (the ONLY mechanical edits, besides LF normalization):
//   * This file is included AFTER the M2-1/M2-2/M2-3/M2-5/M2-4/M2-5B .inc files
//     and shares their 'm2_legacy_reference' namespace. The body's UNQUALIFIED
//     call to War3SemanticPaletteLooksModelLocal (the A4 compose-policy)
//     therefore resolves to the M2-4 legacy copy in this same namespace - exactly
//     the call target the pre-A9 device.cpp text resolved through its
//     using-directive. The A9 diff battery consequently covers A9's own logic AND
//     the A4 contract in its call chain (A4 -> A3/A6/A7/A8 legacy copies).
//   * dxvk::war3::render::ObjectKind and dxvk::war3::IsReadableRange are the
//     SHARED real contract primitives (same headers on both sides, deliberately
//     not rewritten).
//   * EXACTLY ZERO lines are deleted or rewritten relative to the device.cpp
//     text: only the enclosing namespace changes and line endings are LF-
//     normalized. The generator fail-closes unless the module .cpp body between
//     its A9 BEGIN/END markers is byte-identical to this file's body.
using namespace dxvk;
using namespace dxvk::war3;

namespace m2_legacy_reference {

// --- pre-migration definition: d3d9_device.cpp line 7161 (War3SemanticBuildWorldPaletteIfNeeded, verbatim; body byte-identical to the module) ---
[[maybe_unused]] void War3SemanticBuildWorldPaletteIfNeeded(
    const std::vector<Matrix4>& sourcePalette,
    const Matrix4& worldTransform,
    uint8_t objectKind,
    std::vector<Matrix4>& outPalette) {
  outPalette.clear();
  if (!War3SemanticPaletteLooksModelLocal(sourcePalette, worldTransform,
                                          objectKind)) {
    return;
  }

  outPalette.reserve(sourcePalette.size());
  for (const Matrix4& localMatrix : sourcePalette)
    outPalette.push_back(worldTransform * localMatrix);
}


} // namespace m2_legacy_reference
