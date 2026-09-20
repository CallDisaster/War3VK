// AUTO-GENERATED legacy reference for the M2-4 palette compose-policy migration.
//
// DO NOT EDIT. This file is the verbatim pre-migration text of the eight
// palette-side symbols (nine definitions: War3SemanticPaletteLooksModelLocal
// has two overloads) that M2-4 moved out of src/d3d9/d3d9_device.cpp into
// src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}:
//   War3SemanticPaletteInPlaceAppendRuntime, War3SemanticDrawTimePoseRuntime,
//   War3SemanticBoundsRadiusForObjectKind, War3SemanticTranslationDistanceSq,
//   War3SemanticTranslationFinite, War3SemanticPaletteStorageReadable,
//   War3SemanticPaletteLooksModelLocal (pointer + vector overloads),
//   War3SemanticHashMatrix4.
// Regenerate with:
//   py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-4
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-M2-4 work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\device_pre_m2_4.cpp)
//   SHA-256(pre-M2-4 work-tree d3d9_device.cpp) = 7391F3071F7D2CF32583C57FF1D914FC59AA765A5EDD5749F5D3A8FCBD8A0844
//   size = 2254826 bytes, captured 2026-09-18T03:34:55 (machine local time)
//   The pre-M2-4 state equals the post-M2-5 work tree (same SHA as the M2-5
//   record's "post-migration d3d9_device.cpp"); like M1/M2-1/M2-2/M2-3/M2-5 this
//   is a work-tree file hash, not a git blob hash (the work is uncommitted).
//
// Namespace rewrite rules (the ONLY mechanical edits, besides LF normalization):
//   * This file is included AFTER the M2-1/M2-2/M2-3/M2-5 .inc files and shares
//     their 'm2_legacy_reference' namespace. The bodies' UNQUALIFIED calls
//     (War3SemanticTranslationFinite, War3SemanticTranslationDistanceSq,
//     War3SemanticBoundsRadiusForObjectKind, War3SemanticPaletteStorageReadable,
//     War3SemanticPaletteLooksModelLocal, War3GetEnvU32) therefore resolve to the
//     legacy copies in this same namespace - exactly the call targets the
//     pre-M2-4 device.cpp text resolved through its using-directive. The .inc
//     therefore emits the definitions in dependency order.
//   * dxvk::war3::render::ObjectKind and dxvk::war3::IsReadableRange are the
//     SHARED real contract primitives (same headers on both sides, deliberately
//     not rewritten).
//   * EXACTLY ZERO lines are deleted or rewritten relative to the device.cpp
//     text: only the enclosing namespace changes and line endings are LF-
//     normalized. The generator fail-closes unless every module .cpp body
//     between its BEGIN/END markers is byte-identical to this file's bodies.
//   * Signature-only differences that are NOT part of the compared body: the env
//     getters drop 'inline' (declared in the module header) and the pointer
//     overload of War3SemanticPaletteLooksModelLocal keeps its
//     'checkReadable = true' default only on the module header declaration
//     (M2-2 precedent). No token inside any function body was edited.
using namespace dxvk;
using namespace dxvk::war3;

namespace m2_legacy_reference {

// --- pre-migration definition: d3d9_device.cpp line 1302 (War3SemanticPaletteInPlaceAppendRuntime, verbatim; body byte-identical to the module) ---
inline bool War3SemanticPaletteInPlaceAppendRuntime() {
  // Construct the compact live palette directly in its scene slot. Keep a
  // runtime A/B gate so isolated tests can compare the two byte-equivalent
  // paths without changing the fixed 256-matrix shader upload layout.
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_PALETTE_IN_PLACE_APPEND", 1u) != 0u;
  return s_enabled;
}

// --- pre-migration definition: d3d9_device.cpp line 2079 (War3SemanticDrawTimePoseRuntime, verbatim; body byte-identical to the module) ---
bool War3SemanticDrawTimePoseRuntime() {
  // 1.27a 默认关闭：实机报告证实 D3DRS_VERTEXBLEND 恒为 DISABLE，
  // draw-time D3D palette 路径 100% NoVertexBlend 拒绝（Published=0）。
  // 默认开时每帧数百次空跑会直接抬高 capture 热路径与 Untracked。
  // 诊断时再用 DXVK_WAR3_SEMANTIC_DRAW_TIME_POSE=1。
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_DRAW_TIME_POSE", 0u) != 0u;
  return s_enabled;
}

// --- pre-migration definition: d3d9_device.cpp line 7144 (War3SemanticBoundsRadiusForObjectKind, verbatim; body byte-identical to the module) ---
float War3SemanticBoundsRadiusForObjectKind(uint8_t objectKind) {
  using dxvk::war3::render::ObjectKind;
  switch (static_cast<ObjectKind>(objectKind)) {
  case ObjectKind::Unit:
    return 260.0f;
  case ObjectKind::Building:
    return 900.0f;
  case ObjectKind::Destructible:
    return 750.0f;
  case ObjectKind::Item:
    return 220.0f;
  case ObjectKind::Effect:
    return 900.0f;
  default:
    return 0.0f;
  }
}

// --- pre-migration definition: d3d9_device.cpp line 7177 (War3SemanticTranslationDistanceSq, verbatim; body byte-identical to the module) ---
float War3SemanticTranslationDistanceSq(const Matrix4& a, const Matrix4& b) {
  const float dx = a[3].x - b[3].x;
  const float dy = a[3].y - b[3].y;
  const float dz = a[3].z - b[3].z;
  return dx * dx + dy * dy + dz * dz;
}

// --- pre-migration definition: d3d9_device.cpp line 7184 (War3SemanticTranslationFinite, verbatim; body byte-identical to the module) ---
bool War3SemanticTranslationFinite(const Matrix4& m) {
  return std::isfinite(m[3].x) && std::isfinite(m[3].y) &&
         std::isfinite(m[3].z);
}

// --- pre-migration definition: d3d9_device.cpp line 7189 (War3SemanticPaletteStorageReadable, verbatim; body byte-identical to the module) ---
bool War3SemanticPaletteStorageReadable(const std::vector<Matrix4>& palette) {
  if (palette.empty())
    return false;
  if (palette.size() > 256u)
    return false;
  return dxvk::war3::IsReadableRange(
      palette.data(), palette.size() * sizeof(Matrix4));
}

// --- pre-migration definition: d3d9_device.cpp line 7209 (War3SemanticPaletteLooksModelLocal[pointer], verbatim; body byte-identical to the module) ---
bool War3SemanticPaletteLooksModelLocal(
    const Matrix4* palette,
    uint32_t paletteCount,
    const Matrix4& worldTransform,
    uint8_t objectKind,
    bool checkReadable = true) {
  if (palette == nullptr || paletteCount == 0u ||
      (checkReadable &&
       !dxvk::war3::IsReadableRange(palette,
                                    size_t(paletteCount) * sizeof(Matrix4))) ||
      !War3SemanticTranslationFinite(worldTransform))
    return false;

  const float worldMagSq = worldTransform[3].x * worldTransform[3].x +
                           worldTransform[3].y * worldTransform[3].y +
                           worldTransform[3].z * worldTransform[3].z;
  if (!(worldMagSq > 16.0f))
    return false;

  float guardRadius = War3SemanticBoundsRadiusForObjectKind(objectKind);
  if (!(guardRadius > 0.0f))
    guardRadius = 260.0f;
  guardRadius = std::max(384.0f, guardRadius * 1.5f);
  const float thresholdSq = guardRadius * guardRadius;

  float closestSq = std::numeric_limits<float>::max();
  float closestPaletteMagSq = std::numeric_limits<float>::max();
  const uint32_t sampleCount =
      std::min<uint32_t>(paletteCount, 4u);
  for (uint32_t i = 0u; i < sampleCount; ++i) {
    if (!War3SemanticTranslationFinite(palette[i]))
      return false;
    const float px = palette[i][3].x;
    const float py = palette[i][3].y;
    const float pz = palette[i][3].z;
    closestPaletteMagSq =
        std::min(closestPaletteMagSq, px * px + py * py + pz * pz);
    closestSq = std::min(
        closestSq,
        War3SemanticTranslationDistanceSq(palette[i], worldTransform));
  }

  // The shadow caster shader expects world-space fixed-function matrices
  // because it evaluates `in_pos * paletteMatrix` directly. CModel's live
  // final-pose array can be model-local on the semantic direct-read path, while
  // CModel+0x64 carries the runtime world transform. If sampled palette
  // translations are far from the runtime world origin, treat the palette as
  // model-local and compose it to the same world-space contract the old D3D
  // fixed-function path provided.
  const float localMagLimit = std::max(1024.0f, guardRadius * 2.0f);
  if (closestPaletteMagSq > localMagLimit * localMagLimit)
    return false;

  return closestSq > thresholdSq;
}

// --- pre-migration definition: d3d9_device.cpp line 7265 (War3SemanticPaletteLooksModelLocal[vector], verbatim; body byte-identical to the module) ---
bool War3SemanticPaletteLooksModelLocal(
    const std::vector<Matrix4>& palette,
    const Matrix4& worldTransform,
    uint8_t objectKind) {
  if (!War3SemanticPaletteStorageReadable(palette))
    return false;
  return War3SemanticPaletteLooksModelLocal(
      palette.data(), uint32_t(palette.size()), worldTransform, objectKind,
      false);
}

// --- pre-migration definition: d3d9_device.cpp line 7292 (War3SemanticHashMatrix4, verbatim; body byte-identical to the module) ---
uint64_t War3SemanticHashMatrix4(const Matrix4& matrix) {
  uint64_t hash = bit::fnv1a_init();
  for (uint32_t r = 0u; r < 4u; ++r) {
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].x));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].y));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].z));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].w));
  }
  return hash;
}


} // namespace m2_legacy_reference
