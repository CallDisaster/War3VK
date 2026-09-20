// AUTO-GENERATED legacy reference for the M2-3 motion-diagnostics migration.
//
// DO NOT EDIT. This file is the verbatim pre-migration implementation text of
// the three motion/churn diagnostic notes and their two entry structs that M2-3
// moved out of src/d3d9/d3d9_device.cpp into
// src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}:
//   War3SemanticPaletteMotionEntry, War3NoteLivePaletteMotion,
//   War3SemanticHashMotionEntry, War3NoteDrawTimePoseMotion,
//   War3NoteSubmittedPaletteMotion.
// Regenerate with:
//   py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-3
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-M2-3 work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\\device_pre_m2_3.cpp)
//   SHA-256(pre-M2-3 work-tree d3d9_device.cpp) = ECC4B828AF344DCF20368DAC9F6C8344CFCDE4B862779A0190C6390CB3F6F7B3
//   size = 2278494 bytes, captured 2026-09-18T02:18:25 (machine local time)
//   The pre-M2-3 state equals the post-M2-2 work tree (same SHA as the M2-2
//   record's "post-migration d3d9_device.cpp"); like M2-1/M2-2 this is a
//   work-tree file hash, not a git blob hash (M2-1/M2-2 work is uncommitted).
//
// Namespace rewrite rules (the ONLY mechanical edits, besides LF normalization):
//   * This file is included AFTER war3_live_palette_selection_legacy_reference.inc
//     and war3_live_palette_selection_chain_legacy_reference.inc and shares their
//     'm2_legacy_reference' namespace. The three bodies' UNQUALIFIED call to
//     War3SemanticPaletteDiagnosticsRuntime() therefore resolves to the M2-1
//     legacy copy in the same namespace — exactly the call target the pre-M2-3
//     device.cpp text resolved through its using-directive.
//   * War3ShadowCaptureStats is the SHARED real contract type (the same
//     d3d9_war3_scene.h struct on both sides, deliberately not rewritten); the
//     legacy text reaches it through the 'using namespace dxvk;' below.
//   * The two Entry structs are legacy-side copies (verbatim); the module side
//     has its own independently compiled copies with identical layout.
//   * No token of any function or struct is rewritten here.
using namespace dxvk;
using namespace dxvk::war3;

namespace m2_legacy_reference {

// --- pre-migration entry struct: d3d9_device.cpp line 7344 (War3SemanticPaletteMotionEntry) ---
struct War3SemanticPaletteMotionEntry {
  void* runtimeModelPtr = nullptr;
  uint64_t rawHash = 0u;
  uint64_t groupHash = 0u;
  uint64_t frameSerial = 0u;
};

// --- pre-migration definition: d3d9_device.cpp line 7351 (War3NoteLivePaletteMotion) ---
void War3NoteLivePaletteMotion(War3ShadowCaptureStats& stats,
                               void* runtimeModelPtr,
                               uint64_t frameSerial,
                               uint64_t rawHash,
                               uint64_t groupHash) {
  if (!War3SemanticPaletteDiagnosticsRuntime())
    return;
  if (runtimeModelPtr == nullptr || rawHash == 0u || groupHash == 0u)
    return;

  stats.semanticSceneLivePaletteMotionSampleCount++;
  static std::array<War3SemanticPaletteMotionEntry, 512> s_entries = {};
  static uint32_t s_replaceCursor = 0u;

  War3SemanticPaletteMotionEntry* entry = nullptr;
  for (auto& candidate : s_entries) {
    if (candidate.runtimeModelPtr == runtimeModelPtr) {
      entry = &candidate;
      break;
    }
  }

  if (entry == nullptr) {
    for (auto& candidate : s_entries) {
      if (candidate.runtimeModelPtr == nullptr) {
        entry = &candidate;
        break;
      }
    }
  }

  if (entry == nullptr) {
    entry = &s_entries[s_replaceCursor++ % s_entries.size()];
  }

  const bool isNewRuntime = entry->runtimeModelPtr != runtimeModelPtr;
  stats.semanticSceneLivePaletteMotionLastRuntimeModelPtr =
      reinterpret_cast<uintptr_t>(runtimeModelPtr);
  stats.semanticSceneLivePaletteMotionLastPrevRawHash =
      isNewRuntime ? 0u : entry->rawHash;
  stats.semanticSceneLivePaletteMotionLastRawHash = rawHash;
  stats.semanticSceneLivePaletteMotionLastPrevGroupHash =
      isNewRuntime ? 0u : entry->groupHash;
  stats.semanticSceneLivePaletteMotionLastGroupHash = groupHash;

  if (isNewRuntime) {
    stats.semanticSceneLivePaletteMotionNewRuntimeCount++;
  } else {
    if (entry->rawHash != rawHash)
      stats.semanticSceneLivePaletteMotionRawChangedCount++;
    else
      stats.semanticSceneLivePaletteMotionRawStableCount++;

    if (entry->groupHash != groupHash)
      stats.semanticSceneLivePaletteMotionGroupChangedCount++;
    else
      stats.semanticSceneLivePaletteMotionGroupStableCount++;
  }

  entry->runtimeModelPtr = runtimeModelPtr;
  entry->rawHash = rawHash;
  entry->groupHash = groupHash;
  entry->frameSerial = frameSerial;
}

// --- pre-migration entry struct: d3d9_device.cpp line 7416 (War3SemanticHashMotionEntry) ---
struct War3SemanticHashMotionEntry {
  void* runtimeModelPtr = nullptr;
  uint64_t hash = 0u;
  uint64_t frameSerial = 0u;
};

// --- pre-migration definition: d3d9_device.cpp line 7422 (War3NoteDrawTimePoseMotion) ---
void War3NoteDrawTimePoseMotion(War3ShadowCaptureStats& stats,
                                void* runtimeModelPtr,
                                uint64_t frameSerial,
                                uint64_t hash) {
  if (!War3SemanticPaletteDiagnosticsRuntime())
    return;
  if (runtimeModelPtr == nullptr || hash == 0u)
    return;

  static std::array<War3SemanticHashMotionEntry, 512> s_entries = {};
  static uint32_t s_replaceCursor = 0u;

  War3SemanticHashMotionEntry* entry = nullptr;
  for (auto& candidate : s_entries) {
    if (candidate.runtimeModelPtr == runtimeModelPtr) {
      entry = &candidate;
      break;
    }
  }
  if (entry == nullptr) {
    for (auto& candidate : s_entries) {
      if (candidate.runtimeModelPtr == nullptr) {
        entry = &candidate;
        break;
      }
    }
  }
  if (entry == nullptr)
    entry = &s_entries[s_replaceCursor++ % s_entries.size()];

  const bool isNewRuntime = entry->runtimeModelPtr != runtimeModelPtr;
  stats.semanticSceneDrawTimePoseLastRuntimeModelPtr =
      reinterpret_cast<uintptr_t>(runtimeModelPtr);
  stats.semanticSceneDrawTimePoseLastPrevHash =
      isNewRuntime ? 0u : entry->hash;
  stats.semanticSceneDrawTimePoseLastHash = hash;

  if (!isNewRuntime) {
    if (entry->hash != hash)
      stats.semanticSceneDrawTimePoseChangedCount++;
    else
      stats.semanticSceneDrawTimePoseStableCount++;
  }

  entry->runtimeModelPtr = runtimeModelPtr;
  entry->hash = hash;
  entry->frameSerial = frameSerial;
}

// --- pre-migration definition: d3d9_device.cpp line 7471 (War3NoteSubmittedPaletteMotion) ---
void War3NoteSubmittedPaletteMotion(War3ShadowCaptureStats& stats,
                                    void* runtimeModelPtr,
                                    uint64_t frameSerial,
                                    uint64_t hash) {
  if (!War3SemanticPaletteDiagnosticsRuntime())
    return;
  if (runtimeModelPtr == nullptr || hash == 0u)
    return;

  stats.semanticSceneSubmittedPaletteMotionSampleCount++;
  static std::array<War3SemanticHashMotionEntry, 512> s_entries = {};
  static uint32_t s_replaceCursor = 0u;

  War3SemanticHashMotionEntry* entry = nullptr;
  for (auto& candidate : s_entries) {
    if (candidate.runtimeModelPtr == runtimeModelPtr) {
      entry = &candidate;
      break;
    }
  }
  if (entry == nullptr) {
    for (auto& candidate : s_entries) {
      if (candidate.runtimeModelPtr == nullptr) {
        entry = &candidate;
        break;
      }
    }
  }
  if (entry == nullptr)
    entry = &s_entries[s_replaceCursor++ % s_entries.size()];

  const bool isNewRuntime = entry->runtimeModelPtr != runtimeModelPtr;
  stats.semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr =
      reinterpret_cast<uintptr_t>(runtimeModelPtr);
  stats.semanticSceneSubmittedPaletteMotionLastPrevHash =
      isNewRuntime ? 0u : entry->hash;
  stats.semanticSceneSubmittedPaletteMotionLastHash = hash;

  if (isNewRuntime) {
    stats.semanticSceneSubmittedPaletteMotionNewRuntimeCount++;
  } else if (entry->hash != hash) {
    stats.semanticSceneSubmittedPaletteMotionChangedCount++;
  } else {
    stats.semanticSceneSubmittedPaletteMotionStableCount++;
  }

  entry->runtimeModelPtr = runtimeModelPtr;
  entry->hash = hash;
  entry->frameSerial = frameSerial;
}


} // namespace m2_legacy_reference
