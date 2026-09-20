// AUTO-GENERATED legacy reference for the M2-1 live-palette-selection migration.
//
// DO NOT EDIT. This file is the verbatim pre-migration implementation text of the
// 8 symbols that M2-1 moved out of src/d3d9/d3d9_device.cpp into
// src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}.
// Regenerate with: py AutoTest/gen_war3_live_palette_selection_legacy_reference.py
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-M2 work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\\device_pre_m2_1.cpp)
//   SHA-256(pre-M2 work-tree d3d9_device.cpp) = 2736335B42FCF4947B728F2907A04FE9F63EC2E5C8683A887D0785432E499F1B
//   size = 2314463 bytes, captured 2026-09-17T21:11:53 (machine local time)
//   NOTE worktree-vs-git-blob difference: unlike M1 (whose pre-migration text was
//   the committed git blob ae890542d766:src/d3d9/d3d9_device.cpp), the pre-M2 state
//   is the UNCOMMITTED M1 work tree, so git has no blob for it. This provenance is
//   a work-tree file hash, not a git blob hash.
//
// Only two mechanical edits are applied: the body text is re-emitted inside this
// namespace, and the enclosing namespace changes from device.cpp's anonymous
// namespace (namespace dxvk { namespace { ... } }) to
// 'm2_legacy_reference'. Line endings are normalized to LF (device.cpp is CRLF;
// M1's git-blob provenance was likewise LF-normalized). No token of any function
// is rewritten here.
//
// War3GetEnvU32 below is a SUPPORTING verbatim definition: it is NOT one of the
// 8 migrated symbols. M1 already moved it out of device.cpp, so the pre-M2
// device.cpp does not contain it; it is extracted here from the M1 module
// (war3/semantic/war3_device_semantic_predicates.cpp), which is exactly the call
// target the pre-M2 env getters resolved to.
using namespace dxvk;
using namespace dxvk::war3;

namespace m2_legacy_reference {

// --- supporting verbatim definition (NOT one of the 8 migrated
// symbols): the pre-M2 call target of the legacy env getters, from
// war3/semantic/war3_device_semantic_predicates.cpp line 30 ---
uint32_t War3GetEnvU32(const char *name, uint32_t fallback) {
  const std::string v = env::getEnvVar(name);
  if (v.empty())
    return fallback;
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(v.c_str(), &end, 0);
  if (end == v.c_str())
    return fallback;
  return static_cast<uint32_t>(parsed);
}

// --- pre-migration definition: d3d9_device.cpp line 2071 (War3SemanticLivePaletteSafeCopyRuntime) ---
bool War3SemanticLivePaletteSafeCopyRuntime() {
  // Game.dll 的全局 palette 通常每帧只读一次，帧内 region cache 无法摊薄
  // VirtualQuery。ReadProcessMemory 同时完成范围验证与快照复制，避免
  // VirtualQuery 后再直接解引用所留下的 TOCTOU 窗口。保留运行时回退用于
  // 同一 DLL 的 A/B/B/A 验证。
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_LIVE_PALETTE_SAFE_COPY", 1u) != 0u;
  return s_enabled;
}

// --- pre-migration definition: d3d9_device.cpp line 2081 (War3SemanticLivePaletteRefreshRuntime) ---
bool War3SemanticLivePaletteRefreshRuntime() {
  // 每帧从 Hook_RuntimeMatrixWrite 捕获的混合调色板缓存读取完整骨骼矩阵
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH", 1u) != 0u;
  return s_enabled;
}

// --- pre-migration definition: d3d9_device.cpp line 2088 (War3SemanticLivePaletteAllowCModelFallbackRuntime) ---
bool War3SemanticLivePaletteAllowCModelFallbackRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_LIVE_PALETTE_ALLOW_CMODEL_FALLBACK",
                    0u) != 0u;
  return s_enabled;
}

// --- pre-migration definition: d3d9_device.cpp line 2545 (War3SemanticPaletteDiagnosticsRuntime) ---
inline bool War3SemanticPaletteDiagnosticsRuntime() {
  // These historical motion/churn probes do not participate in palette
  // selection, replay validation, or publication. Keep their large lookup
  // tables out of the release hot path unless a targeted capture requests
  // them explicitly.
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS", 0u) != 0u;
  return s_enabled;
}

// --- pre-migration definition: d3d9_device.cpp line 7426 (War3SemanticHashMatrixPalette) ---
uint64_t War3SemanticHashMatrixPalette(const Matrix4* matrices,
                                        uint32_t matrixCount) {
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, matrixCount);
  if (matrices == nullptr || matrixCount == 0u)
    return hash;

  for (uint32_t i = 0u; i < matrixCount; ++i) {
    const Matrix4& matrix = matrices[i];
    for (uint32_t r = 0u; r < 4u; ++r) {
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].x));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].y));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].z));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].w));
    }
  }
  return hash;
}

// --- pre-migration definition: d3d9_device.cpp line 7445 (War3DecodeRuntimePoseMatrix48) ---
Matrix4 War3DecodeRuntimePoseMatrix48(const uint8_t* poseBytes) {
  float pose3x4[12] = {};
  std::memcpy(pose3x4, poseBytes, sizeof(pose3x4));
  return Matrix4(Vector4(pose3x4[0], pose3x4[1], pose3x4[2], 0.0f),
                 Vector4(pose3x4[3], pose3x4[4], pose3x4[5], 0.0f),
                 Vector4(pose3x4[6], pose3x4[7], pose3x4[8], 0.0f),
                 Vector4(pose3x4[9], pose3x4[10], pose3x4[11], 1.0f));
}

// --- pre-migration definition: d3d9_device.cpp line 7454 (War3TryReadRuntimePoseArray) ---
bool War3TryReadRuntimePoseArray(void* runtimeModelPtr,
                                 uint32_t& outPoseCount,
                                 void*& outPoseArrayPtr) {
  outPoseCount = 0u;
  outPoseArrayPtr = nullptr;
  if (runtimeModelPtr == nullptr)
    return false;

  if (!dxvk::war3::SafeReadU32Fast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
          outPoseCount) ||
      outPoseCount == 0u || outPoseCount > 1024u ||
      !dxvk::war3::SafeReadPtrFast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
          outPoseArrayPtr) ||
      outPoseArrayPtr == nullptr ||
      !dxvk::war3::IsReadableRange(
          outPoseArrayPtr, size_t(outPoseCount) * sizeof(float) * 12u)) {
    outPoseCount = 0u;
    outPoseArrayPtr = nullptr;
    return false;
  }
  return true;
}

// --- pre-migration definition: d3d9_device.cpp line 7479 (War3ResolveLivePoseRuntimeAlias) ---
void* War3ResolveLivePoseRuntimeAlias(void* runtimeModelPtr,
                                      uint32_t& outPoseCount,
                                      void*& outPoseArrayPtr) {
  outPoseCount = 0u;
  outPoseArrayPtr = nullptr;
  if (runtimeModelPtr == nullptr)
    return nullptr;

  constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
  const uintptr_t value = reinterpret_cast<uintptr_t>(runtimeModelPtr);
  if (value < 0x10000u)
    return nullptr;

  std::array<void*, 3> candidates = {
      reinterpret_cast<void*>(value + kCModelComplexExtensionOffset),
      runtimeModelPtr,
      value > kCModelComplexExtensionOffset
          ? reinterpret_cast<void*>(value - kCModelComplexExtensionOffset)
          : nullptr};

  for (void* candidate : candidates) {
    uint32_t poseCount = 0u;
    void* poseArrayPtr = nullptr;
    if (!War3TryReadRuntimePoseArray(candidate, poseCount, poseArrayPtr))
      continue;
    outPoseCount = poseCount;
    outPoseArrayPtr = poseArrayPtr;
    return candidate;
  }

  return nullptr;
}


} // namespace m2_legacy_reference
