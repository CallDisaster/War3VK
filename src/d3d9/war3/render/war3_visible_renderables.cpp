#include "war3_visible_renderables.h"

#include "war3_shadow_lifecycle.h"
#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_scene.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../core/war3_semantic_shadow_gate.h"
#include "../game/war3_unit.h"
#include "../model/war3_model_resource_cache.h"
#include "../model/war3_model_registry.h"
#include "war3_current_draw_contract.h"
#include "war3_render_state.h"
#include "war3_render_objects.h"
#include "war3_render_queue_tracker.h"
#include "war3_shadow_object_registry.h"
#include "war3_shadow_runtime_bridge.h"
#include "../../util/util_bit.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <unordered_set>

namespace dxvk::war3::render {

namespace {

constexpr size_t kRenderBatchElementStride = 20;
constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;

uint64_t VisibleRenderablePartLayerKey(void* renderablePart,
                                       uint32_t layerIndex) {
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(
      hash, uint64_t(reinterpret_cast<uintptr_t>(renderablePart)));
  hash = bit::fnv1a_iter(hash, layerIndex);
  return hash;
}

constexpr uint32_t kInvalidSemanticMergeRecordIndex = 0xFFFFFFFFu;

struct VisibleSemanticMergeIndexConfig {
  // The verifier maintains and probes the index, but does not select the
  // production path unless the independent enabled flag is set.
  bool enabled = false;
  bool verify = false;
  bool assertOnMismatch = false;
};

const VisibleSemanticMergeIndexConfig&
GetVisibleSemanticMergeIndexConfig() {
  static const VisibleSemanticMergeIndexConfig s_config = []() {
    const auto readEnabled = [](const char* name) {
      const char* raw = std::getenv(name);
      return raw != nullptr && raw[0] == '1';
    };
    VisibleSemanticMergeIndexConfig config = {};
    config.enabled =
        readEnabled("DXVK_WAR3_VISIBLE_SEMANTIC_MERGE_INDEX");
    config.assertOnMismatch =
        readEnabled("DXVK_WAR3_VISIBLE_SEMANTIC_MERGE_INDEX_VERIFY_ASSERT");
    config.verify =
        readEnabled("DXVK_WAR3_VISIBLE_SEMANTIC_MERGE_INDEX_VERIFY") ||
        config.assertOnMismatch;
    return config;
  }();
  return s_config;
}

bool War3VisibleSemanticMergeIndexEnabled() {
  return GetVisibleSemanticMergeIndexConfig().enabled;
}

bool War3VisibleSemanticMergeIndexVerifierAssertEnabled() {
  return GetVisibleSemanticMergeIndexConfig().assertOnMismatch;
}

bool War3VisibleSemanticMergeIndexVerifierEnabled() {
  return GetVisibleSemanticMergeIndexConfig().verify;
}

bool War3VisibleSemanticMergeIndexMaintained() {
  const auto& config = GetVisibleSemanticMergeIndexConfig();
  return config.enabled || config.verify;
}

template <typename Fn>
void ForEachDistinctSemanticMergePointer(const VisibleRenderableRecord& record,
                                         Fn&& fn) {
  if (record.renderablePart != nullptr)
    fn(record.renderablePart);
  if (record.payload != nullptr && record.payload != record.renderablePart)
    fn(record.payload);
}

void IndexSemanticMergeRecord(VisibleRenderableRegistry::Snapshot& snap,
                              uint32_t index) {
  if (!War3VisibleSemanticMergeIndexMaintained() ||
      index >= snap.records.size()) {
    return;
  }

  ForEachDistinctSemanticMergePointer(
      snap.records[index],
      [&](void* pointer) { snap.semanticMergeByPointer.emplace(pointer, index); });
}

void RefreshSemanticMergeRecordIndex(
    VisibleRenderableRegistry::Snapshot& snap, uint32_t index,
    void* oldRenderablePart, void* oldPayload) {
  if (!War3VisibleSemanticMergeIndexMaintained() ||
      index >= snap.records.size()) {
    return;
  }

  const VisibleRenderableRecord& record = snap.records[index];
  if (record.renderablePart == oldRenderablePart &&
      record.payload == oldPayload) {
    return;
  }

  const auto oldHadPointer = [&](void* pointer) {
    return pointer != nullptr &&
           (pointer == oldRenderablePart || pointer == oldPayload);
  };
  const auto newHasPointer = [&](void* pointer) {
    return pointer != nullptr &&
           (pointer == record.renderablePart || pointer == record.payload);
  };
  const auto erasePointerIndex = [&](void* pointer) {
    if (pointer == nullptr || newHasPointer(pointer))
      return;
    const auto range = snap.semanticMergeByPointer.equal_range(pointer);
    for (auto it = range.first; it != range.second;) {
      if (it->second == index)
        it = snap.semanticMergeByPointer.erase(it);
      else
        ++it;
    }
  };

  erasePointerIndex(oldRenderablePart);
  if (oldPayload != oldRenderablePart)
    erasePointerIndex(oldPayload);

  ForEachDistinctSemanticMergePointer(record, [&](void* pointer) {
    if (!oldHadPointer(pointer))
      snap.semanticMergeByPointer.emplace(pointer, index);
  });
}

void RebuildSemanticMergePointerIndex(
    VisibleRenderableRegistry::Snapshot& snap) {
  snap.semanticMergeByPointer.clear();
  if (!War3VisibleSemanticMergeIndexMaintained())
    return;

  snap.semanticMergeByPointer.reserve(snap.records.size() * 2u);
  for (uint32_t index = 0u; index < snap.records.size(); ++index) {
    ForEachDistinctSemanticMergePointer(
        snap.records[index],
        [&](void* pointer) {
          snap.semanticMergeByPointer.emplace(pointer, index);
        });
  }
}

bool VerifySemanticMergePointerIndex(
    const VisibleRenderableRegistry::Snapshot& snap) {
  using Entry = std::pair<uintptr_t, uint32_t>;
  static thread_local std::vector<Entry> s_expected;
  static thread_local std::vector<Entry> s_actual;
  s_expected.clear();
  s_actual.clear();
  s_expected.reserve(snap.records.size() * 2u);
  s_actual.reserve(snap.semanticMergeByPointer.size());

  for (uint32_t index = 0u; index < snap.records.size(); ++index) {
    ForEachDistinctSemanticMergePointer(
        snap.records[index], [&](void* pointer) {
          s_expected.emplace_back(
              reinterpret_cast<uintptr_t>(pointer), index);
        });
  }
  for (const auto& [pointer, index] : snap.semanticMergeByPointer) {
    s_actual.emplace_back(reinterpret_cast<uintptr_t>(pointer), index);
  }

  std::sort(s_expected.begin(), s_expected.end());
  std::sort(s_actual.begin(), s_actual.end());
  return s_expected == s_actual;
}

uint64_t HashTaggedPtr(uint32_t tag, const void* value) {
  if (value == nullptr)
    return 0u;
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, tag);
  hash = bit::fnv1a_iter(
      hash, uint64_t(reinterpret_cast<uintptr_t>(value)));
  return hash;
}

uint64_t HashTaggedU32(uint32_t tag, uint32_t value) {
  if (value == 0u)
    return 0u;
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, tag);
  hash = bit::fnv1a_iter(hash, value);
  return hash;
}

uint64_t ShadowManifestObjectKey(
    const CurrentDrawContractRecord& record) {
  return VisibleRenderableRegistry::computeShadowManifestObjectKey(record);
}

uint64_t ShadowManifestPartKey(
    const CurrentDrawContractRecord& record, uint64_t objectKey) {
  if (objectKey == 0u)
    return 0u;

  uint64_t hash = bit::fnv1a_init();
  // Phase 7.31 Phase 5：destructible 专项。
  // 背景：大门（可破坏物）在 closed/opening/opened 状态之间切换时，
  // renderablePart 指针不变，但 slice 的逻辑语义已经变了——这时 lease
  // 会把 closed 状态下的 packet 继续垫给 opening 的 live packet，
  // 肉眼看到的就是"大门阴影在 closed/open 之间剧烈闪烁"。
  //
  // Iter C 曾经试过给所有对象都在 part key 里混入 payload11C，hot_shadow_poll
  // 指标很好，但 run_quick_autotest benchmark（14K+ skinned）FPS 从 100+
  // 崩到 3.7，因为全局 manifest 里 part 数量乘了 2-9 倍，触发了
  // noteShadowManifestPartGoodPacket 的 O(N²) 扫描。
  //
  // 本轮做受限版本：**只对 Destructible 把 payload11C 进 key**。
  // - Destructible 在场景里数量有限（几十到几百个，远小于 skinned 上万）；
  // - Unit/其他对象继续走原 key 路径，保持 benchmark FPS 不退化；
  // - 用户反馈"大门闪得特别厉害"正是 destructible 专属症状，对症下药。
  hash = bit::fnv1a_iter(hash, 0x72110100u);
  hash = bit::fnv1a_iter(hash, objectKey);
  hash = bit::fnv1a_iter(hash, record.layerIndex);
  hash = bit::fnv1a_iter(hash, record.payloadWord108);
  if (record.objectKind == dxvk::war3::render::ObjectKind::Destructible) {
    hash = bit::fnv1a_iter(hash, record.payloadWord11C);
  }
  return hash;
}

uint64_t ShadowManifestPartAnchorKey(
    const CurrentDrawContractRecord& record, uint64_t objectKey) {
  if (objectKey == 0u)
    return 0u;

  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, 0x72110200u);
  hash = bit::fnv1a_iter(hash, objectKey);
  hash = bit::fnv1a_iter(hash, record.payloadWord108);
  return hash;
}

uint64_t ShadowManifestSliceKey(
    const CurrentDrawContractRecord& record) {
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, record.layerIndex);
  hash = bit::fnv1a_iter(hash, record.payloadWord108);
  return hash;
}

void HashBytesFnv64Append(uint64_t& hash, const void* data, size_t size) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0u; i < size; ++i) {
    hash ^= uint64_t(bytes[i]);
    hash *= 1099511628211ull;
  }
}

// Phase 7.26: CModel pose 探测本身涉及可变长度的矩阵 hash（最多 256 * 48B = 12KB
// FNV-1a 扫描），在默认 release 配置下属于"不产生行为只产生诊断"的开销。
// 将其收口到运行时开关后，只有显式开启 pose restore 或 pose 诊断时才执行。
bool War3SemanticShadowManifestCModelPoseProbeAllowed() {
  static const bool s_enabled = []() {
    auto readU32 = [](const char* name, uint32_t fallback) {
      const char* raw = std::getenv(name);
      if (raw == nullptr || raw[0] == '\0')
        return fallback;
      try {
        return uint32_t(std::strtoul(raw, nullptr, 0));
      } catch (...) {
        return fallback;
      }
    };
    // pose restore 开启时必须允许探测，否则 restore 路径没有判定依据。
    if (readU32("DXVK_WAR3_SEMANTIC_MANIFEST_CMODEL_POSE_RESTORE", 0u) != 0u)
      return true;
    // 单独的诊断开关允许不启用 restore 行为也持续观察 pose 状态。
    if (readU32("DXVK_WAR3_SEMANTIC_SHADOW_MANIFEST_CMODEL_POSE_DIAG", 0u) != 0u)
      return true;
    return false;
  }();
  return s_enabled;
}

bool War3SemanticShadowManifestPoseGenerationVerifierAssertEnabled() {
  static const bool s_enabled = []() {
    const char* raw = std::getenv(
        "DXVK_WAR3_SEMANTIC_MANIFEST_POSE_GENERATION_VERIFY_ASSERT");
    return raw != nullptr && raw[0] == '1';
  }();
  return s_enabled;
}

bool War3SemanticShadowManifestPoseGenerationVerifierEnabled() {
  static const bool s_enabled = []() {
    const char* raw = std::getenv(
        "DXVK_WAR3_SEMANTIC_MANIFEST_POSE_GENERATION_VERIFY");
    return (raw != nullptr && raw[0] == '1') ||
           War3SemanticShadowManifestPoseGenerationVerifierAssertEnabled();
  }();
  return s_enabled;
}

bool TryProbeLiveCModelPose(void* runtimeModelPtr,
                            uint32_t& outMatrixCount,
                            uint64_t& outMatrixHash) {
  outMatrixCount = 0u;
  outMatrixHash = 0u;
  if (runtimeModelPtr == nullptr)
    return false;

  uint32_t matrixCount = 0u;
  void* matrixBase = nullptr;
  if (!dxvk::war3::SafeReadU32Fast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
          matrixCount) ||
      !dxvk::war3::SafeReadPtrFast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
          matrixBase) ||
      matrixBase == nullptr || matrixCount == 0u) {
    return false;
  }

  matrixCount = (std::min)(matrixCount, 256u);
  const size_t byteCount = size_t(matrixCount) * 48u;
  if (!dxvk::war3::IsReadableRange(matrixBase, byteCount))
    return false;

  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, matrixCount);
  HashBytesFnv64Append(hash, matrixBase, byteCount);
  outMatrixCount = matrixCount;
  outMatrixHash = hash;
  return true;
}

void* TryCanonicalRuntimeModelPtr(void* candidate);

void* ResolveRuntimeModelForCurrentDrawRecord(
    const VisibleRenderableRegistry::Snapshot& snap,
    const CurrentDrawContractRecord& record) {
  auto runtimeFromIndex = [&](uint32_t index) -> void* {
    if (index >= snap.records.size())
      return nullptr;
    void* runtimeModelPtr = snap.records[index].runtimeModelPtr;
    if (runtimeModelPtr == nullptr)
      return nullptr;
    return TryCanonicalRuntimeModelPtr(runtimeModelPtr);
  };

  if (record.renderablePart != nullptr) {
    const uint64_t partLayerKey =
        VisibleRenderablePartLayerKey(record.renderablePart,
                                      record.layerIndex);
    const auto partLayerIt = snap.byRenderablePartLayer.find(partLayerKey);
    if (partLayerIt != snap.byRenderablePartLayer.end()) {
      if (void* runtimeModelPtr = runtimeFromIndex(partLayerIt->second))
        return runtimeModelPtr;
    }

    const auto countIt =
        snap.renderablePartRecordCount.find(record.renderablePart);
    if (countIt != snap.renderablePartRecordCount.end() &&
        countIt->second == 1u) {
      const auto partIt = snap.byRenderablePart.find(record.renderablePart);
      if (partIt != snap.byRenderablePart.end()) {
        if (void* runtimeModelPtr = runtimeFromIndex(partIt->second))
          return runtimeModelPtr;
      }
    }
  }

  if (record.sceneNode != nullptr) {
    const auto sceneIt = snap.bySceneNode.find(record.sceneNode);
    if (sceneIt != snap.bySceneNode.end()) {
      if (void* runtimeModelPtr = runtimeFromIndex(sceneIt->second))
        return runtimeModelPtr;
    }
  }

  for (const auto& visible : snap.records) {
    if (record.renderablePart != nullptr &&
        visible.renderablePart == record.renderablePart &&
        visible.layerIndex == record.layerIndex) {
      if (void* runtimeModelPtr =
              TryCanonicalRuntimeModelPtr(visible.runtimeModelPtr))
        return runtimeModelPtr;
    }
    if (record.sceneNode != nullptr &&
        (visible.sceneNode == record.sceneNode ||
         visible.identity.sceneNode == record.sceneNode)) {
      if (void* runtimeModelPtr =
              TryCanonicalRuntimeModelPtr(visible.runtimeModelPtr))
        return runtimeModelPtr;
    }
    const bool identityMatch =
        (record.unitPtr != nullptr &&
         visible.identity.unitPtr == record.unitPtr) ||
        (record.jHandle != 0u && visible.identity.jHandle == record.jHandle) ||
        (record.worldObjectEntry != nullptr &&
         visible.identity.worldObjectEntry == record.worldObjectEntry) ||
        (record.sceneNode != nullptr &&
         (visible.sceneNode == record.sceneNode ||
          visible.identity.sceneNode == record.sceneNode));
    if (identityMatch) {
      if (void* runtimeModelPtr =
              TryCanonicalRuntimeModelPtr(visible.runtimeModelPtr))
        return runtimeModelPtr;
    }
  }

  model::ModelInstanceRecord instanceRecord = {};
  auto& instanceRegistry = model::ModelInstanceRegistry::instance();
  if ((record.unitPtr != nullptr &&
       instanceRegistry.findByUnitPtr(record.unitPtr, instanceRecord)) ||
      (record.worldObjectEntry != nullptr &&
       instanceRegistry.findByWorldObjectEntry(record.worldObjectEntry,
                                               instanceRecord)) ||
      (record.sceneNode != nullptr &&
       instanceRegistry.findBySceneNode(record.sceneNode, instanceRecord)) ||
      (record.jHandle != 0u &&
       instanceRegistry.findByHandle(record.jHandle, instanceRecord))) {
    if (void* runtimeModelPtr =
            TryCanonicalRuntimeModelPtr(instanceRecord.runtimeModelPtr))
      return runtimeModelPtr;
  }

  ShadowObjectRecord shadowRecord = {};
  auto& shadowRegistry = ShadowObjectRegistry::instance();
  if ((record.unitPtr != nullptr &&
       shadowRegistry.findByUnitPtr(record.unitPtr, shadowRecord)) ||
      (record.worldObjectEntry != nullptr &&
       shadowRegistry.findByWorldObjectEntry(record.worldObjectEntry,
                                             shadowRecord)) ||
      (record.sceneNode != nullptr &&
       shadowRegistry.findBySceneNode(record.sceneNode, shadowRecord)) ||
      (record.jHandle != 0u &&
       shadowRegistry.findByHandle(record.jHandle, shadowRecord))) {
    if (void* runtimeModelPtr =
            TryCanonicalRuntimeModelPtr(shadowRecord.runtimeModelPtr))
      return runtimeModelPtr;
  }

  return nullptr;
}

struct RenderablePartBasicFields {
  void *sceneNode = nullptr;
  void *meshData = nullptr;
};

bool TryReadRenderablePartPtr(void *renderablePart, size_t offset, void *&outPtr) {
  outPtr = nullptr;
  if (!renderablePart)
    return false;

  const uintptr_t base = reinterpret_cast<uintptr_t>(renderablePart);
  if (base < 0x10000u || base + offset < base)
    return false;

  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigTrustVisibleRenderablePartPointers) {
    outPtr = *reinterpret_cast<void **>(
        reinterpret_cast<std::uint8_t *>(renderablePart) + offset);
    return outPtr != nullptr;
  }

  return dxvk::war3::SafeReadPtrFast(renderablePart, offset, outPtr) &&
         outPtr != nullptr;
}

bool TryReadSceneNodeFromRenderablePart(void *renderablePart, void *&outSceneNode) {
  outSceneNode = nullptr;
  if (!renderablePart)
    return false;
  return TryReadRenderablePartPtr(renderablePart,
                                  RenderablePartFieldOffsets::SceneNode,
                                  outSceneNode);
}

bool TryReadMeshDataFromRenderablePart(void *renderablePart, void *&outMeshData) {
  outMeshData = nullptr;
  if (!renderablePart)
    return false;
  return TryReadRenderablePartPtr(renderablePart,
                                  RenderablePartFieldOffsets::MeshData,
                                  outMeshData);
}

bool TryReadU32Fast(const void *base, size_t offset, uint32_t &out) {
  out = 0u;
  return base != nullptr && dxvk::war3::SafeReadU32Fast(base, offset, out);
}

bool TryReadPtrFast(const void *base, size_t offset, void *&outPtr) {
  outPtr = nullptr;
  return base != nullptr && dxvk::war3::SafeReadPtrFast(base, offset, outPtr) &&
         outPtr != nullptr;
}

bool LooksLikeRuntimeModelPtr(void* candidate) {
  if (candidate == nullptr)
    return false;

  const uintptr_t candidateValue = reinterpret_cast<uintptr_t>(candidate);
  if (candidateValue < 0x10000u)
    return false;

  void* ownedHandlePtr = nullptr;
  uint32_t runtimeGeosetCount = 0u;
  void* runtimeGeosets = nullptr;
  uint32_t finalPoseMatrixCount = 0u;
  void* finalPoseMatrixArray = nullptr;
  const bool hasOwnedHandle =
      dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
          ownedHandlePtr) &&
      ownedHandlePtr != nullptr &&
      model::ShadowModelResourceCache::instance().resolveDirectModelResourcePtr(
          ownedHandlePtr) != nullptr;
  const bool hasRuntimeGeosets =
      dxvk::war3::SafeReadU32Fast(candidate,
                                  dxvk::war3::CModelOffsets::RuntimeGeosetCount,
                                  runtimeGeosetCount) &&
      runtimeGeosetCount > 0u &&
      runtimeGeosetCount < 4096u &&
      dxvk::war3::SafeReadPtrFast(candidate,
                                  dxvk::war3::CModelOffsets::RuntimeGeosets,
                                  runtimeGeosets) &&
      runtimeGeosets != nullptr &&
      dxvk::war3::IsReadableRange(
          runtimeGeosets,
          size_t(runtimeGeosetCount > 4u ? 4u : runtimeGeosetCount) *
              sizeof(void*));
  const bool hasFinalPoseArray =
      dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
          finalPoseMatrixCount) &&
      finalPoseMatrixCount > 0u &&
      finalPoseMatrixCount <= 256u &&
      dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
          finalPoseMatrixArray) &&
      finalPoseMatrixArray != nullptr &&
      dxvk::war3::IsReadableRange(finalPoseMatrixArray,
                                  size_t(sizeof(float)) * 16u);
  return hasRuntimeGeosets || (hasOwnedHandle && hasFinalPoseArray);
}

template <typename Fn>
void ForEachRuntimeModelAlias(void* runtimeModelPtr, Fn&& fn) {
  if (runtimeModelPtr == nullptr)
    return;

  const uintptr_t value = reinterpret_cast<uintptr_t>(runtimeModelPtr);
  if (value < 0x10000u)
    return;

  fn(runtimeModelPtr);
  if (value <= (~uintptr_t(0u)) - kCModelComplexExtensionOffset)
    fn(reinterpret_cast<void*>(value + kCModelComplexExtensionOffset));
  if (value > kCModelComplexExtensionOffset)
    fn(reinterpret_cast<void*>(value - kCModelComplexExtensionOffset));
}

void* TryCanonicalRuntimeModelPtr(void* candidate) {
  void* resolved = nullptr;
  ForEachRuntimeModelAlias(candidate, [&](void* alias) {
    if (resolved == nullptr && LooksLikeRuntimeModelPtr(alias))
      resolved = alias;
  });
  return resolved;
}

void *TryReadRuntimeModelFromSprite(void *spritePtr) {
  void *runtimeModelPtr = nullptr;
  if (!spritePtr)
    return nullptr;
  if (!dxvk::war3::SafeReadPtrFast(spritePtr, dxvk::war3::CSpriteOffsets::Model,
                                   runtimeModelPtr))
    return nullptr;
  return TryCanonicalRuntimeModelPtr(runtimeModelPtr);
}

bool IsObviouslyInvalidRuntimeModelForRenderable(
    void* runtimeModelPtr, void* worldObjectEntry, void* sceneNode,
    void* spritePtr, void* renderablePart, void* meshData) {
  if (runtimeModelPtr == nullptr)
    return true;
  if (TryCanonicalRuntimeModelPtr(runtimeModelPtr) != nullptr)
    return false;
  return runtimeModelPtr == worldObjectEntry || runtimeModelPtr == sceneNode ||
         runtimeModelPtr == spritePtr || runtimeModelPtr == renderablePart ||
         runtimeModelPtr == meshData;
}

void SanitizeRuntimeModelPtrForRecord(VisibleRenderableRecord& record) {
  void* spritePtr = nullptr;
  if (record.identity.unitPtr != nullptr) {
    game::UnitWrapper unit(record.identity.unitPtr);
    if (unit.IsValid())
      spritePtr = unit.GetSprite();
  }

  if (!IsObviouslyInvalidRuntimeModelForRenderable(
          record.runtimeModelPtr, record.identity.worldObjectEntry,
          record.sceneNode, spritePtr, record.renderablePart, record.meshData)) {
    return;
  }

  record.runtimeModelPtr = nullptr;
}

void BackfillIdentityFromRuntimeModel(VisibleRenderableRecord& record) {
  if (record.runtimeModelPtr == nullptr)
    return;

  if (void* canonicalRuntimeModel = TryCanonicalRuntimeModelPtr(
          record.runtimeModelPtr)) {
    record.runtimeModelPtr = canonicalRuntimeModel;
  } else {
    return;
  }

  auto& instanceRegistry = model::ModelInstanceRegistry::instance();
  auto& shadowRegistry = ShadowObjectRegistry::instance();

  model::ModelInstanceRecord instanceRecord = {};
  bool hasInstanceRecord = false;
  ForEachRuntimeModelAlias(record.runtimeModelPtr, [&](void* alias) {
    if (hasInstanceRecord)
      return;
    hasInstanceRecord =
        instanceRegistry.findByRuntimeModel(alias, instanceRecord) ||
        instanceRegistry.findOwnerByRuntimeModel(alias, instanceRecord);
  });
  if (hasInstanceRecord) {
    if (record.identity.worldObjectEntry == nullptr)
      record.identity.worldObjectEntry = instanceRecord.worldObjectEntry;
    if (record.sceneNode == nullptr)
      record.sceneNode = instanceRecord.sceneNode;
    if (record.identity.sceneNode == nullptr)
      record.identity.sceneNode = instanceRecord.sceneNode;
    if (record.identity.unitPtr == nullptr)
      record.identity.unitPtr = instanceRecord.unitPtr;
    if (record.identity.jHandle == 0u)
      record.identity.jHandle = instanceRecord.jHandle;
    if (record.identity.rawcode == 0u)
      record.identity.rawcode = instanceRecord.rawcode;
    if (record.modelResourcePtr == nullptr)
      record.modelResourcePtr = instanceRecord.modelResourcePtr;
    if (record.modelKey == 0u)
      record.modelKey = instanceRecord.modelKey;
    if (record.identity.kind == ObjectKind::Unknown &&
        (instanceRecord.unitPtr != nullptr || instanceRecord.jHandle != 0u ||
         instanceRecord.rawcode != 0u)) {
      record.identity.kind = ObjectKind::Unit;
    }
  }

  ShadowObjectRecord shadowRecord = {};
  bool hasShadowRecord = false;
  ForEachRuntimeModelAlias(record.runtimeModelPtr, [&](void* alias) {
    if (!hasShadowRecord)
      hasShadowRecord = shadowRegistry.findByRuntimeModel(alias, shadowRecord);
  });
  if (!hasShadowRecord)
    return;

  if (record.identity.worldObjectEntry == nullptr)
    record.identity.worldObjectEntry = shadowRecord.worldObjectEntry;
  if (record.sceneNode == nullptr)
    record.sceneNode = shadowRecord.sceneNode;
  if (record.identity.sceneNode == nullptr)
    record.identity.sceneNode = shadowRecord.sceneNode;
  if (record.identity.unitPtr == nullptr)
    record.identity.unitPtr = shadowRecord.unitPtr;
  if (record.identity.jHandle == 0u)
    record.identity.jHandle = shadowRecord.jHandle;
  if (record.identity.rawcode == 0u)
    record.identity.rawcode = shadowRecord.rawcode;
  if (record.modelResourcePtr == nullptr)
    record.modelResourcePtr = shadowRecord.modelResourcePtr;
  if (record.modelKey == 0u)
    record.modelKey = shadowRecord.modelKey;
  if (record.identity.kind == ObjectKind::Unknown &&
      shadowRecord.kind != ObjectKind::Unknown) {
    record.identity.kind = shadowRecord.kind;
  }
}

void* TryReadRuntimeModelFromPointerWindow(void* owner, size_t maxOffset) {
  if (owner == nullptr)
    return nullptr;

  constexpr size_t kPointerStride = sizeof(void*);
  for (size_t offset = 0u; offset <= maxOffset; offset += kPointerStride) {
    void* candidate = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(owner, offset, candidate) ||
        candidate == nullptr) {
      continue;
    }
    if (void* runtimeModelPtr = TryCanonicalRuntimeModelPtr(candidate))
      return runtimeModelPtr;
  }

  return nullptr;
}

void* TryReadRuntimeModelFromMeshData(void* meshData) {
  if (!meshData)
    return nullptr;

  void* poseOrTransformCtx = nullptr;
  if (dxvk::war3::SafeReadPtrFast(
          meshData, dxvk::war3::MeshDataOffsets::TransformOrPoseCtx,
          poseOrTransformCtx) &&
      poseOrTransformCtx != nullptr) {
    if (void* runtimeModelPtr = TryCanonicalRuntimeModelPtr(poseOrTransformCtx))
      return runtimeModelPtr;

    // Render-dispatch MeshData usually points at a small pose/transform context
    // rather than CModel directly.  Keep this to a tight pointer window so the
    // visible end-frame hydrate can recover the owning CModel without bringing
    // back the old hot-path registry scans.
    if (void* runtimeModelPtr =
            TryReadRuntimeModelFromPointerWindow(poseOrTransformCtx, 0x60u)) {
      return runtimeModelPtr;
    }
  }

  // Some prepared mesh variants keep the runtime model near the MeshData tail.
  // This is deliberately small and guarded by LooksLikeRuntimeModelPtr: meshData
  // itself is a shared resource key and must never become object identity.
  constexpr size_t kMeshDataRuntimeCandidateOffsets[] = {
      0xE8u, 0xECu, 0xF0u, 0xF4u, 0xF8u, 0xFCu, 0x100u, 0x104u,
      0x108u, 0x10Cu, 0x110u, 0x114u, 0x118u, 0x11Cu, 0x120u};
  for (size_t offset : kMeshDataRuntimeCandidateOffsets) {
    void* candidate = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(meshData, offset, candidate) ||
        candidate == nullptr) {
      continue;
    }
    if (void* runtimeModelPtr = TryCanonicalRuntimeModelPtr(candidate))
      return runtimeModelPtr;
  }

  return nullptr;
}

void* TryReadDirectModelResourceFromRuntimeModel(void* runtimeModelPtr) {
  void* ownedHandlePtr = nullptr;
  if (!runtimeModelPtr)
    return nullptr;
  if (!dxvk::war3::SafeReadPtrFast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
          ownedHandlePtr)) {
    return nullptr;
  }

  auto& resourceCache = model::ShadowModelResourceCache::instance();
  return resourceCache.resolveDirectModelResourcePtr(ownedHandlePtr);
}

bool TryResolveRenderablePartFromTransparentPayload(
    void *payload, uint32_t transparentType,
    const RenderObjectIdentitySnapshot &identity, void *&outRenderablePart,
    void *&outSceneNode, void *&outMeshData) {
  outRenderablePart = nullptr;
  outSceneNode = nullptr;
  outMeshData = nullptr;
  if (!payload)
    return false;

  void *sceneCandidate = nullptr;
  void *meshCandidate = nullptr;
  if (!TryReadSceneNodeFromRenderablePart(payload, sceneCandidate) ||
      !TryReadMeshDataFromRenderablePart(payload, meshCandidate)) {
    return false;
  }

  const bool looksLikeDirectPart = transparentType == 0u;
  const bool matchesIdentityScene =
      identity.sceneNode != nullptr && sceneCandidate == identity.sceneNode;
  if (!looksLikeDirectPart && !matchesIdentityScene)
    return false;

  outRenderablePart = payload;
  outSceneNode = sceneCandidate;
  outMeshData = meshCandidate;
  return true;
}

bool TryReadMeshIndexFromMeshData(void *meshData, uint32_t &outMeshIndex) {
  // 仅对 canonical MeshData 结构成立；若 `renderablePart + 0x0C`
  // 实际上是 CGeosetData*，则 0x108 更像 layout/material slot，
  // 不应继续当作 runtime geoset index 解释。
  outMeshIndex = kInvalidVisibleMeshIndex;
  if (!meshData)
    return false;
  return TryReadU32Fast(meshData, dxvk::war3::MeshDataOffsets::MeshIndex,
                        outMeshIndex);
}

bool LooksLikeGeosetDataPtr(void* candidate) {
  if (candidate == nullptr)
    return false;

  uint32_t vertexCount = 0;
  uint32_t primitiveCount = 0;
  uint32_t matrixGroupCount = 0;
  uint32_t matrixIndexCount = 0;
  void* positions = nullptr;
  void* primitiveRecords = nullptr;
  void* matrixGroupSizes = nullptr;
  void* matrixIndices = nullptr;

  if (!TryReadU32Fast(candidate, dxvk::war3::CGeosetDataOffsets::VertexCount,
                      vertexCount) ||
      !TryReadPtrFast(candidate,
                      dxvk::war3::CGeosetDataOffsets::VertexPositions,
                      positions) ||
      !TryReadU32Fast(candidate,
                      dxvk::war3::CGeosetDataOffsets::PrimitiveRecordCount,
                      primitiveCount) ||
      !TryReadPtrFast(candidate,
                      dxvk::war3::CGeosetDataOffsets::PrimitiveRecords,
                      primitiveRecords) ||
      !TryReadU32Fast(candidate,
                      dxvk::war3::CGeosetDataOffsets::MatrixGroupCount,
                      matrixGroupCount) ||
      !TryReadPtrFast(candidate,
                      dxvk::war3::CGeosetDataOffsets::MatrixGroupSizes,
                      matrixGroupSizes) ||
      !TryReadU32Fast(candidate,
                      dxvk::war3::CGeosetDataOffsets::MatrixIndexCount,
                      matrixIndexCount) ||
      !TryReadPtrFast(candidate,
                      dxvk::war3::CGeosetDataOffsets::MatrixIndices,
                      matrixIndices)) {
    return false;
  }

  if (vertexCount == 0u || vertexCount > (1u << 20) ||
      primitiveCount == 0u || primitiveCount > (1u << 16) ||
      matrixGroupCount == 0u || matrixGroupCount > 4096u ||
      matrixIndexCount == 0u || matrixIndexCount > (1u << 16)) {
    return false;
  }

  return dxvk::war3::IsReadableRange(
             positions, std::min<size_t>(size_t(vertexCount) * 3u * sizeof(float),
                                         64u)) &&
         dxvk::war3::IsReadableRange(
             primitiveRecords,
             std::min<size_t>(
                 size_t(primitiveCount) *
                    sizeof(dxvk::war3::GeosetPrimitiveRecord),
                 64u)) &&
         dxvk::war3::IsReadableRange(
             matrixGroupSizes,
             std::min<size_t>(size_t(matrixGroupCount) * sizeof(uint32_t), 64u)) &&
         dxvk::war3::IsReadableRange(
             matrixIndices,
             std::min<size_t>(size_t(matrixIndexCount) * sizeof(uint32_t), 64u));
}

bool TryResolveGeosetFromArray(void *ownerPtr, size_t countOffset,
                               size_t arrayOffset, uint32_t geosetIndex,
                               void *&outGeosetPtr, void *&outGeosetDataPtr) {
  outGeosetPtr = nullptr;
  outGeosetDataPtr = nullptr;
  if (ownerPtr == nullptr || geosetIndex == kInvalidVisibleMeshIndex)
    return false;

  uint32_t geosetCount = 0;
  void *geosetArray = nullptr;
  if (!TryReadU32Fast(ownerPtr, countOffset, geosetCount) ||
      !TryReadPtrFast(ownerPtr, arrayOffset, geosetArray) ||
      geosetCount == 0 || geosetIndex >= geosetCount ||
      !dxvk::war3::IsReadableRange(geosetArray,
                                   size_t(geosetCount) * sizeof(void *))) {
    return false;
  }

  auto **entries = reinterpret_cast<void **>(geosetArray);
  outGeosetPtr = entries[geosetIndex];
  if (outGeosetPtr == nullptr)
    return false;

  TryReadPtrFast(outGeosetPtr, dxvk::war3::CGeosetOffsets::GeosetData,
                 outGeosetDataPtr);
  return true;
}

void ResolveGeosetMetadata(VisibleRenderableRecord &record) {
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  if (record.modelResourcePtr != nullptr) {
    record.modelResourcePtr =
        resourceCache.resolveDirectModelResourcePtr(record.modelResourcePtr);
  }

  record.meshIndex = kInvalidVisibleMeshIndex;
  record.geosetIndex = kInvalidVisibleMeshIndex;
  record.runtimeGeosetPtr = nullptr;
  record.runtimeGeosetDataPtr = nullptr;

  model::ShadowGeosetResourceRecord directGeosetRecord = {};
  if (record.meshData != nullptr &&
      resourceCache.findGeosetByData(record.meshData, directGeosetRecord)) {
    record.runtimeGeosetDataPtr = record.meshData;
    if (directGeosetRecord.geosetPtr != nullptr)
      record.runtimeGeosetPtr = directGeosetRecord.geosetPtr;
    if (record.modelResourcePtr == nullptr &&
        directGeosetRecord.modelResourcePtr != nullptr) {
      record.modelResourcePtr = directGeosetRecord.modelResourcePtr;
    }
    if (record.modelKey == 0u && directGeosetRecord.modelKey != 0u)
      record.modelKey = directGeosetRecord.modelKey;
    if (directGeosetRecord.geosetIndex != model::kInvalidShadowGeosetIndex) {
      record.geosetIndex = directGeosetRecord.geosetIndex;
      record.meshIndex = directGeosetRecord.geosetIndex;
    }
  } else if (record.meshData != nullptr && LooksLikeGeosetDataPtr(record.meshData)) {
    // 新确认的 native 链路里，某些 batch item + 0x0C 直接就是 CGeosetData*，
    // 不能再一概当 MeshData* 解释，否则会把 layout slot 误读成 mesh index。
    record.runtimeGeosetDataPtr = record.meshData;
  }

  if (record.runtimeGeosetDataPtr == nullptr && record.meshData != nullptr)
    TryReadMeshIndexFromMeshData(record.meshData, record.meshIndex);

  if (record.meshIndex == kInvalidVisibleMeshIndex)
    record.geosetIndex = kInvalidVisibleMeshIndex;
  else
    record.geosetIndex = record.meshIndex;

  if (record.runtimeGeosetDataPtr == nullptr &&
      record.meshIndex != kInvalidVisibleMeshIndex) {
    TryResolveGeosetFromArray(record.runtimeModelPtr,
                              dxvk::war3::CModelOffsets::RuntimeGeosetCount,
                              dxvk::war3::CModelOffsets::RuntimeGeosets,
                              record.meshIndex, record.runtimeGeosetPtr,
                              record.runtimeGeosetDataPtr);
  }

  if (record.runtimeGeosetPtr != nullptr || record.runtimeGeosetDataPtr != nullptr) {
    resourceCache.noteRuntimeGeosetBinding(
        record.runtimeModelPtr, record.meshIndex, record.runtimeGeosetPtr,
        record.runtimeGeosetDataPtr, record.modelResourcePtr, record.modelKey);
  }

  model::ShadowGeosetResourceRecord geosetRecord = {};
  if (record.runtimeGeosetPtr != nullptr &&
      resourceCache.findGeosetByPtr(record.runtimeGeosetPtr, geosetRecord)) {
    if (geosetRecord.geosetIndex != model::kInvalidShadowGeosetIndex)
      record.geosetIndex = geosetRecord.geosetIndex;
    if (record.runtimeGeosetDataPtr == nullptr)
      record.runtimeGeosetDataPtr = geosetRecord.geosetDataPtr;
    if (record.modelResourcePtr == nullptr)
      record.modelResourcePtr = geosetRecord.modelResourcePtr;
    if (record.modelKey == 0u)
      record.modelKey = geosetRecord.modelKey;
    return;
  }

  if (record.runtimeGeosetDataPtr != nullptr &&
      resourceCache.findGeosetByData(record.runtimeGeosetDataPtr, geosetRecord)) {
    if (geosetRecord.geosetIndex != model::kInvalidShadowGeosetIndex)
      record.geosetIndex = geosetRecord.geosetIndex;
    if (record.runtimeGeosetPtr == nullptr)
      record.runtimeGeosetPtr = geosetRecord.geosetPtr;
    if (record.modelResourcePtr == nullptr)
      record.modelResourcePtr = geosetRecord.modelResourcePtr;
    if (record.modelKey == 0u)
      record.modelKey = geosetRecord.modelKey;
    return;
  }

  if (record.runtimeModelPtr != nullptr &&
      resourceCache.findRuntimeModelGeoset(record.runtimeModelPtr,
                                           record.meshIndex, geosetRecord)) {
    if (record.runtimeGeosetPtr == nullptr)
      record.runtimeGeosetPtr = geosetRecord.geosetPtr;
    if (record.runtimeGeosetDataPtr == nullptr)
      record.runtimeGeosetDataPtr = geosetRecord.geosetDataPtr;
    if (geosetRecord.geosetIndex != model::kInvalidShadowGeosetIndex)
      record.geosetIndex = geosetRecord.geosetIndex;
    if (record.modelResourcePtr == nullptr)
      record.modelResourcePtr = geosetRecord.modelResourcePtr;
    if (record.modelKey == 0u)
      record.modelKey = geosetRecord.modelKey;
    return;
  }

  if (record.modelResourcePtr == nullptr)
    return;

  if (record.meshIndex == kInvalidVisibleMeshIndex) {
    model::ShadowModelResourceRecord modelRecord = {};
    if (resourceCache.findModelResource(record.modelResourcePtr, modelRecord) &&
        modelRecord.geosetCount == 1u) {
      record.meshIndex = 0u;
      record.geosetIndex = 0u;
    }
  }

  if (record.meshIndex == kInvalidVisibleMeshIndex)
    return;

  if (resourceCache.findModelGeoset(record.modelResourcePtr, record.meshIndex,
                                    geosetRecord)) {
    if (record.runtimeGeosetPtr == nullptr)
      record.runtimeGeosetPtr = geosetRecord.geosetPtr;
    if (record.runtimeGeosetDataPtr == nullptr)
      record.runtimeGeosetDataPtr = geosetRecord.geosetDataPtr;
    if (geosetRecord.geosetIndex != model::kInvalidShadowGeosetIndex)
      record.geosetIndex = geosetRecord.geosetIndex;
    return;
  }

  void *resourceGeosetPtr = nullptr;
  void *resourceGeosetDataPtr = nullptr;
  if (TryResolveGeosetFromArray(record.modelResourcePtr,
                                dxvk::war3::CModelDataOffsets::GeosetCount,
                                dxvk::war3::CModelDataOffsets::Geosets,
                                record.meshIndex, resourceGeosetPtr,
                                resourceGeosetDataPtr)) {
    if (record.runtimeGeosetPtr == nullptr)
      record.runtimeGeosetPtr = resourceGeosetPtr;
    if (record.runtimeGeosetDataPtr == nullptr)
      record.runtimeGeosetDataPtr = resourceGeosetDataPtr;
  }
}

bool ResolveRuntimeOwnerFromGeosetBinding(VisibleRenderableRecord& record) {
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  model::ShadowRuntimeModelOwnerBinding runtimeOwner = {};
  if (!resourceCache.findRuntimeModelOwnerBinding(
          record.runtimeGeosetPtr, record.runtimeGeosetDataPtr,
          record.geosetIndex, record.modelResourcePtr, runtimeOwner)) {
    return false;
  }

  bool changed = false;
  if (record.runtimeModelPtr == nullptr &&
      runtimeOwner.runtimeModelPtr != nullptr) {
    record.runtimeModelPtr = runtimeOwner.runtimeModelPtr;
    changed = true;
  }
  if (record.modelResourcePtr == nullptr &&
      runtimeOwner.modelResourcePtr != nullptr) {
    record.modelResourcePtr = runtimeOwner.modelResourcePtr;
    changed = true;
  }
  if (record.modelKey == 0u && runtimeOwner.modelKey != 0u) {
    record.modelKey = runtimeOwner.modelKey;
    changed = true;
  }

  return changed;
}

void ResolveModelMetadata(const RenderObjectIdentitySnapshot &identity,
                          void *sceneNode, void* renderablePart, void* meshData,
                          void *&outRuntimeModelPtr,
                          void *&outModelResourcePtr, uint64_t &outModelKey) {
  outRuntimeModelPtr = nullptr;
  outModelResourcePtr = nullptr;
  outModelKey = 0;

  model::ModelInstanceRecord instanceRecord = {};
  bool hasInstanceRecord = false;
  auto &instanceRegistry = model::ModelInstanceRegistry::instance();
  if (identity.worldObjectEntry != nullptr)
    hasInstanceRecord = instanceRegistry.findByWorldObjectEntry(
        identity.worldObjectEntry, instanceRecord);
  if (!hasInstanceRecord && sceneNode != nullptr)
    hasInstanceRecord = instanceRegistry.findBySceneNode(sceneNode, instanceRecord);
  if (!hasInstanceRecord && identity.jHandle != 0u)
    hasInstanceRecord = instanceRegistry.findByHandle(identity.jHandle, instanceRecord);
  if (!hasInstanceRecord && identity.unitPtr != nullptr)
    hasInstanceRecord = instanceRegistry.findByUnitPtr(identity.unitPtr, instanceRecord);

  if (hasInstanceRecord) {
    outRuntimeModelPtr = instanceRecord.runtimeModelPtr;
    outModelResourcePtr = instanceRecord.modelResourcePtr;
    outModelKey = instanceRecord.modelKey;
  }

  ShadowObjectRecord shadowRecord = {};
  bool hasShadowRecord = false;
  auto &shadowRegistry = ShadowObjectRegistry::instance();
  if (identity.worldObjectEntry != nullptr)
    hasShadowRecord = shadowRegistry.findByWorldObjectEntry(
        identity.worldObjectEntry, shadowRecord);
  if (!hasShadowRecord && sceneNode != nullptr)
    hasShadowRecord = shadowRegistry.findBySceneNode(sceneNode, shadowRecord);
  if (!hasShadowRecord && identity.jHandle != 0u)
    hasShadowRecord = shadowRegistry.findByHandle(identity.jHandle, shadowRecord);
  if (!hasShadowRecord && identity.unitPtr != nullptr)
    hasShadowRecord = shadowRegistry.findByUnitPtr(identity.unitPtr, shadowRecord);

  if (hasShadowRecord) {
    if (outRuntimeModelPtr == nullptr)
      outRuntimeModelPtr = shadowRecord.runtimeModelPtr;
    if (outModelResourcePtr == nullptr)
      outModelResourcePtr = shadowRecord.modelResourcePtr;
    if (outModelKey == 0)
      outModelKey = shadowRecord.modelKey;
  }

  if (outRuntimeModelPtr != nullptr) {
    if (!hasInstanceRecord)
      hasInstanceRecord =
          instanceRegistry.findByRuntimeModel(outRuntimeModelPtr, instanceRecord);
    if (hasInstanceRecord) {
      if (outModelResourcePtr == nullptr)
        outModelResourcePtr = instanceRecord.modelResourcePtr;
      if (outModelKey == 0u)
        outModelKey = instanceRecord.modelKey;
    }

    if (!hasShadowRecord)
      hasShadowRecord =
          shadowRegistry.findByRuntimeModel(outRuntimeModelPtr, shadowRecord);
    if (hasShadowRecord) {
      if (outModelResourcePtr == nullptr)
        outModelResourcePtr = shadowRecord.modelResourcePtr;
      if (outModelKey == 0u)
        outModelKey = shadowRecord.modelKey;
    }
  }

  const RenderObjectInfo* renderObject = nullptr;
  auto& renderRegistry = RenderObjectRegistry::instance();
  if (sceneNode != nullptr)
    renderObject = renderRegistry.findBySceneNode(sceneNode);
  if (renderObject == nullptr && identity.worldObjectEntry != nullptr)
    renderObject = renderRegistry.findByEntry(identity.worldObjectEntry);
  if (renderObject == nullptr && identity.jHandle != 0u)
    renderObject = renderRegistry.findByHandle(identity.jHandle);

  void *spritePtr = nullptr;
  auto trySpriteLikeRuntimeModel = [&](void* spriteLikePtr) {
    if (spriteLikePtr == nullptr || outRuntimeModelPtr != nullptr)
      return;

    void* runtimeModelCandidate = TryReadRuntimeModelFromSprite(spriteLikePtr);
    if (runtimeModelCandidate == nullptr)
      return;

    outRuntimeModelPtr = runtimeModelCandidate;
    if (spritePtr == nullptr)
      spritePtr = spriteLikePtr;
  };

  auto tryDirectRuntimeModel = [&](void* candidatePtr) {
    if (candidatePtr == nullptr || outRuntimeModelPtr != nullptr)
      return;
    candidatePtr = TryCanonicalRuntimeModelPtr(candidatePtr);
    if (candidatePtr == nullptr)
      return;
    outRuntimeModelPtr = candidatePtr;
  };

  // 这批 anonymous visible subpart 在 renderable 侧经常只有
  // `worldObjectEntry/sceneNode`，没有稳定 `unit->sprite`。当前 live 证据
  // 显示 sceneNode 本身可直接是 CModel；worldObjectEntry 也可能长得像
  // CModel，但把它当 direct runtime 会扩大误命中面，导致错误 caster 共享。
  // 因此 direct runtime 只信 sceneNode，worldObjectEntry 继续走 sprite-like
  // host 试读路径。
  tryDirectRuntimeModel(sceneNode);
  trySpriteLikeRuntimeModel(identity.worldObjectEntry);
  trySpriteLikeRuntimeModel(sceneNode);

  void* resolvedUnitPtr =
      identity.unitPtr != nullptr ? identity.unitPtr
                                  : renderObject != nullptr
                                        ? renderObject->unitPtr
                                        : nullptr;
  if (resolvedUnitPtr != nullptr) {
    game::UnitWrapper unit(resolvedUnitPtr);
    if (unit.IsValid())
      spritePtr = unit.GetSprite();
  }

  if (!hasInstanceRecord && spritePtr != nullptr)
    hasInstanceRecord =
        instanceRegistry.findBySpritePtr(spritePtr, instanceRecord);
  if (!hasShadowRecord && spritePtr != nullptr)
    hasShadowRecord = shadowRegistry.findBySpritePtr(spritePtr, shadowRecord);

  if (hasInstanceRecord) {
    if (outRuntimeModelPtr == nullptr)
      outRuntimeModelPtr = instanceRecord.runtimeModelPtr;
    if (outModelResourcePtr == nullptr)
      outModelResourcePtr = instanceRecord.modelResourcePtr;
    if (outModelKey == 0u)
      outModelKey = instanceRecord.modelKey;
  }
  if (hasShadowRecord) {
    if (outRuntimeModelPtr == nullptr)
      outRuntimeModelPtr = shadowRecord.runtimeModelPtr;
    if (outModelResourcePtr == nullptr)
      outModelResourcePtr = shadowRecord.modelResourcePtr;
    if (outModelKey == 0u)
      outModelKey = shadowRecord.modelKey;
  }

  auto sanitizeRuntimeModelPtr = [&]() {
    if (!IsObviouslyInvalidRuntimeModelForRenderable(
            outRuntimeModelPtr, identity.worldObjectEntry, sceneNode, spritePtr,
            renderablePart, meshData)) {
      return;
    }
    outRuntimeModelPtr = nullptr;
  };

  sanitizeRuntimeModelPtr();

  if (outRuntimeModelPtr == nullptr && spritePtr != nullptr)
    outRuntimeModelPtr = TryReadRuntimeModelFromSprite(spritePtr);
  sanitizeRuntimeModelPtr();

  if (outRuntimeModelPtr == nullptr && meshData != nullptr)
    outRuntimeModelPtr = TryReadRuntimeModelFromMeshData(meshData);
  sanitizeRuntimeModelPtr();

  if (outModelResourcePtr == nullptr && outRuntimeModelPtr != nullptr)
    outModelResourcePtr =
        TryReadDirectModelResourceFromRuntimeModel(outRuntimeModelPtr);

  model::ShadowRuntimeModelOwnerBinding runtimeResourceRecord = {};
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  auto backfillRuntimeResourceCache = [&]() {
    if (outModelResourcePtr != nullptr) {
      outModelResourcePtr =
          resourceCache.resolveDirectModelResourcePtr(outModelResourcePtr);
    }

    if (outRuntimeModelPtr != nullptr) {
      model::ShadowRuntimeModelOwnerBinding existingRuntimeRecord = {};
      const bool hasExistingRuntimeRecord =
          resourceCache.findRuntimeModelBinding(outRuntimeModelPtr,
                                                existingRuntimeRecord);
      const bool needsRuntimeBackfill =
          !hasExistingRuntimeRecord ||
          (existingRuntimeRecord.modelResourcePtr == nullptr &&
           outModelResourcePtr != nullptr) ||
          (existingRuntimeRecord.modelKey == 0u && outModelKey != 0u);
      if (needsRuntimeBackfill) {
        resourceCache.noteRuntimeModelBinding(outRuntimeModelPtr,
                                              outModelResourcePtr, outModelKey);
      }
    }

    if (outModelResourcePtr != nullptr) {
      model::ShadowRuntimeModelOwnerBinding existingModelRecord = {};
      const bool hasExistingModelRecord =
          resourceCache.findModelBinding(outModelResourcePtr,
                                         existingModelRecord);
      const bool needsModelBackfill =
          !hasExistingModelRecord ||
          (existingModelRecord.modelKey == 0u && outModelKey != 0u);
      if (needsModelBackfill) {
        resourceCache.noteModelResourceBinding(outModelResourcePtr, outModelKey);
      }
    }
  };

  if (outRuntimeModelPtr != nullptr &&
      resourceCache.findRuntimeModelBinding(outRuntimeModelPtr,
                                            runtimeResourceRecord)) {
    if (outModelResourcePtr == nullptr)
      outModelResourcePtr = runtimeResourceRecord.modelResourcePtr;
    if (outModelKey == 0u)
      outModelKey = runtimeResourceRecord.modelKey;
  }

  model::ModelResourceRecord modelRecord = {};
  auto &modelRegistry = model::ModelRegistry::instance();
  if (outRuntimeModelPtr != nullptr &&
      modelRegistry.findByRuntimeModel(outRuntimeModelPtr, modelRecord)) {
    if (outModelResourcePtr == nullptr)
      outModelResourcePtr = modelRecord.modelResourcePtr;
    if (outModelKey == 0)
      outModelKey = modelRecord.modelKey;
    if (outRuntimeModelPtr == nullptr)
      outRuntimeModelPtr = modelRecord.runtimeModelPtr;
    backfillRuntimeResourceCache();
    return;
  }

  if (spritePtr != nullptr && modelRegistry.findBySprite(spritePtr, modelRecord)) {
    if (outRuntimeModelPtr == nullptr)
      outRuntimeModelPtr = modelRecord.runtimeModelPtr;
    if (outModelResourcePtr == nullptr)
      outModelResourcePtr = modelRecord.modelResourcePtr;
    if (outModelKey == 0)
      outModelKey = modelRecord.modelKey;
  }

  if (outModelResourcePtr != nullptr) {
    outModelResourcePtr =
        resourceCache.resolveDirectModelResourcePtr(outModelResourcePtr);
  }

  const bool needsRuntimeBridgeAugment =
      sceneNode != nullptr &&
      (outRuntimeModelPtr == nullptr || outModelResourcePtr == nullptr ||
       outModelKey == 0u);
  if (needsRuntimeBridgeAugment) {
    dxvk::War3ShadowSemanticContext semantic = {};
    semantic.renderablePart = renderablePart;
    semantic.sceneNode = sceneNode;
    semantic.worldObjectEntry = identity.worldObjectEntry;
    semantic.runtimeModelPtr = outRuntimeModelPtr;
    semantic.modelResourcePtr = outModelResourcePtr;
    semantic.jHandle = identity.jHandle;
    semantic.rawcode = identity.rawcode;
    semantic.modelKey = outModelKey;
    semantic.objectKind = identity.kind;
    if (render::AugmentShadowSemanticContext(semantic, nullptr)) {
      if (outRuntimeModelPtr == nullptr)
        outRuntimeModelPtr = semantic.runtimeModelPtr;
      if (outModelResourcePtr == nullptr)
        outModelResourcePtr = semantic.modelResourcePtr;
      if (outModelKey == 0u)
        outModelKey = semantic.modelKey;

      if (outModelResourcePtr != nullptr) {
        outModelResourcePtr =
            resourceCache.resolveDirectModelResourcePtr(outModelResourcePtr);
      }
    }
  }

  sanitizeRuntimeModelPtr();

  backfillRuntimeResourceCache();
}

const VisibleRenderableRecord *
FindPriorRecord(const VisibleRenderableRegistry::Snapshot &prior,
                const VisibleRenderableRecord &record) {
  auto findByIndex = [&](const auto &map, const auto &key)
      -> const VisibleRenderableRecord * {
    const auto it = map.find(key);
    if (it == map.end() || it->second >= prior.records.size())
      return nullptr;
    return &prior.records[it->second];
  };

  if (record.identity.worldObjectEntry != nullptr) {
    if (const auto *match =
            findByIndex(prior.byWorldObjectEntry, record.identity.worldObjectEntry))
      return match;
  }
  if (record.sceneNode != nullptr) {
    if (const auto *match = findByIndex(prior.bySceneNode, record.sceneNode))
      return match;
  }
  if (record.identity.jHandle != 0u) {
    if (const auto *match = findByIndex(prior.byHandle, record.identity.jHandle))
      return match;
  }
  if (record.runtimeModelPtr != nullptr) {
    if (const auto *match =
            findByIndex(prior.byRuntimeModel, record.runtimeModelPtr))
      return match;
  }
  if (record.renderablePart != nullptr) {
    if (const auto *match =
            findByIndex(prior.byRenderablePart, record.renderablePart))
      return match;
  }
  if (record.payload != nullptr) {
    if (const auto *match = findByIndex(prior.byPayload, record.payload))
      return match;
  }

  return nullptr;
}

const VisibleRenderableRecord*
FindSnapshotRecord(const VisibleRenderableRegistry::Snapshot& snap,
                   const VisibleRenderableRecord& record) {
  auto findByIndex = [&](const auto& map, const auto& key)
      -> const VisibleRenderableRecord* {
    const auto it = map.find(key);
    if (it == map.end() || it->second >= snap.records.size())
      return nullptr;
    return &snap.records[it->second];
  };

  if (record.identity.worldObjectEntry != nullptr) {
    if (const auto* match =
            findByIndex(snap.byWorldObjectEntry, record.identity.worldObjectEntry)) {
      return match;
    }
  }
  if (record.sceneNode != nullptr) {
    if (const auto* match = findByIndex(snap.bySceneNode, record.sceneNode))
      return match;
  }
  if (record.identity.jHandle != 0u) {
    if (const auto* match = findByIndex(snap.byHandle, record.identity.jHandle))
      return match;
  }
  if (record.runtimeModelPtr != nullptr) {
    if (const auto* match = findByIndex(snap.byRuntimeModel, record.runtimeModelPtr))
      return match;
  }
  if (record.renderablePart != nullptr) {
    if (const auto* match =
            findByIndex(snap.byRenderablePart, record.renderablePart)) {
      return match;
    }
  }
  if (record.payload != nullptr) {
    if (const auto* match = findByIndex(snap.byPayload, record.payload))
      return match;
  }

  return nullptr;
}

void MergeIdentityFromPrior(const VisibleRenderableRecord &prior,
                            VisibleRenderableRecord &record) {
  if (record.identity.worldObjectEntry == nullptr)
    record.identity.worldObjectEntry = prior.identity.worldObjectEntry;
  if (record.identity.sceneNode == nullptr)
    record.identity.sceneNode = prior.identity.sceneNode;
  if (record.identity.unitPtr == nullptr)
    record.identity.unitPtr = prior.identity.unitPtr;
  if (record.identity.agentPtr == nullptr)
    record.identity.agentPtr = prior.identity.agentPtr;
  if (record.identity.handleId == 0u)
    record.identity.handleId = prior.identity.handleId;
  if (record.identity.jHandle == 0u)
    record.identity.jHandle = prior.identity.jHandle;
  if (record.identity.rawcode == 0u)
    record.identity.rawcode = prior.identity.rawcode;
  if (record.identity.agentType == 0u)
    record.identity.agentType = prior.identity.agentType;
  if (record.identity.flags5C == 0u)
    record.identity.flags5C = prior.identity.flags5C;
  if (record.identity.kind == ObjectKind::Unknown &&
      prior.identity.kind != ObjectKind::Unknown) {
    record.identity.kind = prior.identity.kind;
  }
  if (record.identity.groupIdx < 0 && prior.identity.groupIdx >= 0)
    record.identity.groupIdx = prior.identity.groupIdx;

  if (record.sceneNode == nullptr)
    record.sceneNode = prior.sceneNode;
  if (record.runtimeModelPtr == nullptr)
    record.runtimeModelPtr = prior.runtimeModelPtr;
  if (record.modelResourcePtr == nullptr)
    record.modelResourcePtr = prior.modelResourcePtr;
  if (record.modelKey == 0u)
    record.modelKey = prior.modelKey;
  record.pathBlocker =
      record.pathBlocker || prior.pathBlocker ||
      dxvk::war3::internal::IsPathBlockerFourCc(record.identity.rawcode);
  // Do not copy per-renderable geoset/slice data from a sibling or prior record.
  // Multiple units and multiple submeshes can share sceneNode/modelResource keys,
  // and copying the previous geoset here produced the "one caster fragment on
  // every caster" artifact.  Current-frame meshData must resolve its own geoset.
}

void FinalizeVisibleRecord(VisibleRenderableRegistry::Snapshot &snap,
                           const VisibleRenderableRegistry::Snapshot &prior,
                           VisibleRenderableRecord &record) {
  if (record.payload == nullptr)
    record.payload = record.renderablePart;

  if (record.sceneNode == nullptr && record.identity.sceneNode != nullptr)
    record.sceneNode = record.identity.sceneNode;
  if (record.sceneNode == nullptr && record.renderablePart != nullptr)
    TryReadSceneNodeFromRenderablePart(record.renderablePart, record.sceneNode);
  if (record.identity.sceneNode == nullptr && record.sceneNode != nullptr)
    record.identity.sceneNode = record.sceneNode;

  if (record.meshData == nullptr && record.renderablePart != nullptr)
    TryReadMeshDataFromRenderablePart(record.renderablePart, record.meshData);

  const bool identityMissingClassification =
      record.identity.kind == ObjectKind::Unknown &&
      (record.identity.worldObjectEntry != nullptr || record.sceneNode != nullptr ||
       record.identity.sceneNode != nullptr);
  const bool identityMissingRawcode =
      record.identity.rawcode == 0u &&
      (record.identity.jHandle != 0u || record.identity.unitPtr != nullptr ||
       record.identity.worldObjectEntry != nullptr);
  const bool needsIdentityResolve =
      !record.identity.HasStableIdentity() ||
      (record.sceneNode == nullptr && record.identity.sceneNode == nullptr) ||
      identityMissingClassification || identityMissingRawcode;
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleFinalizeIdentityResolve) {
    if (needsIdentityResolve) {
    RenderObjectIdentitySnapshot resolvedIdentity = {};
    if (TryResolveRenderObjectIdentity(record.identity.worldObjectEntry,
                                       record.sceneNode, resolvedIdentity)) {
      record.identity = resolvedIdentity;
      if (record.sceneNode == nullptr)
        record.sceneNode = resolvedIdentity.sceneNode;
    }
    }
  }

  if ((record.identity.kind == ObjectKind::Unknown ||
       record.identity.rawcode == 0u ||
       !record.identity.HasStableIdentity())) {
    if constexpr (!dxvk::war3::internal::
                      kWar3RuntimeConfigDisableSemanticVisibleFinalizeSiblingRecovery) {
      if (const auto *priorRecord = FindPriorRecord(prior, record))
        MergeIdentityFromPrior(*priorRecord, record);
    }
  }
  SanitizeRuntimeModelPtrForRecord(record);

  if (record.runtimeModelPtr == nullptr && record.modelResourcePtr == nullptr &&
      record.modelKey == 0u && record.sceneNode != nullptr) {
    const auto it = snap.modelMetadataBySceneNode.find(record.sceneNode);
    if (it != snap.modelMetadataBySceneNode.end()) {
      record.runtimeModelPtr = it->second.runtimeModelPtr;
      record.modelResourcePtr = it->second.modelResourcePtr;
      record.modelKey = it->second.modelKey;
    }
  }

  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleFinalizeModelMetadata) {
    if (record.runtimeModelPtr == nullptr || record.modelResourcePtr == nullptr ||
        record.modelKey == 0u) {
      ResolveModelMetadata(record.identity, record.sceneNode, record.renderablePart,
                           record.meshData,
                           record.runtimeModelPtr, record.modelResourcePtr,
                           record.modelKey);
      if (record.sceneNode != nullptr) {
        snap.modelMetadataBySceneNode[record.sceneNode] = {
            record.runtimeModelPtr, record.modelResourcePtr, record.modelKey};
      }
    }
  }

  if (record.identity.kind == ObjectKind::Unknown ||
      record.identity.rawcode == 0u || record.identity.jHandle == 0u) {
    if constexpr (!dxvk::war3::internal::
                      kWar3RuntimeConfigDisableSemanticVisibleFinalizeSiblingRecovery) {
      if (const auto *priorRecord = FindPriorRecord(prior, record))
        MergeIdentityFromPrior(*priorRecord, record);
    }
  }
  SanitizeRuntimeModelPtrForRecord(record);

  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleFinalizeGeosetMetadata) {
    ResolveGeosetMetadata(record);
    if (ResolveRuntimeOwnerFromGeosetBinding(record)) {
      SanitizeRuntimeModelPtrForRecord(record);
      ResolveGeosetMetadata(record);
    }
  }

  const bool needsSiblingRecovery =
      !record.identity.HasStableIdentity() ||
      record.runtimeModelPtr == nullptr ||
      record.modelResourcePtr == nullptr || record.modelKey == 0u ||
      record.runtimeGeosetPtr == nullptr ||
      record.runtimeGeosetDataPtr == nullptr;
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleFinalizeSiblingRecovery) {
    if (needsSiblingRecovery) {
      if (const auto* siblingRecord = FindSnapshotRecord(snap, record))
        MergeIdentityFromPrior(*siblingRecord, record);
      if (const auto* priorRecord = FindPriorRecord(prior, record))
        MergeIdentityFromPrior(*priorRecord, record);
    }
  }
  SanitizeRuntimeModelPtrForRecord(record);

  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleFinalizeModelMetadata) {
    if (record.runtimeModelPtr == nullptr || record.modelResourcePtr == nullptr ||
        record.modelKey == 0u) {
      ResolveModelMetadata(record.identity, record.sceneNode, record.renderablePart,
                           record.meshData,
                           record.runtimeModelPtr, record.modelResourcePtr,
                           record.modelKey);
      if (record.sceneNode != nullptr) {
        snap.modelMetadataBySceneNode[record.sceneNode] = {
            record.runtimeModelPtr, record.modelResourcePtr, record.modelKey};
      }
    }
  }

  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleFinalizeGeosetMetadata) {
    ResolveGeosetMetadata(record);
    if (ResolveRuntimeOwnerFromGeosetBinding(record)) {
      SanitizeRuntimeModelPtrForRecord(record);
      ResolveGeosetMetadata(record);
    }
  }
}

} // namespace

uint64_t VisibleRenderableRegistry::computeShadowManifestObjectKey(
    const CurrentDrawContractRecord& record) {
  if (uint64_t key = HashTaggedU32(0x72110002u, record.jHandle))
    return key;
  if (uint64_t key = HashTaggedPtr(0x72110001u, record.unitPtr))
    return key;
  if (uint64_t key = HashTaggedPtr(0x72110003u, record.worldObjectEntry))
    return key;
  return HashTaggedPtr(0x72110004u, record.sceneNode);
}

uint64_t VisibleRenderableRegistry::computeShadowManifestPartKey(
    const CurrentDrawContractRecord& record) {
  return ShadowManifestPartKey(record, computeShadowManifestObjectKey(record));
}

VisibleRenderableRegistry &VisibleRenderableRegistry::instance() {
  static VisibleRenderableRegistry *s_instance = new VisibleRenderableRegistry();
  return *s_instance;
}

void VisibleRenderableRegistry::appendRecord(Snapshot &snap,
                                             VisibleRenderableRecord &record) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigLightweightSemanticVisibleRenderableWrites) {
    const Snapshot &prior = readSnapshot();
    FinalizeVisibleRecord(snap, prior, record);
  } else if (record.payload == nullptr) {
    record.payload = record.renderablePart;
  }

  record.pathBlocker =
      record.pathBlocker ||
      dxvk::war3::internal::IsPathBlockerFourCc(record.identity.rawcode);

  const uint32_t index = static_cast<uint32_t>(snap.records.size());
  snap.records.emplace_back(record);
  IndexSemanticMergeRecord(snap, index);

  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDeferSemanticVisibleIndexBuild ||
                dxvk::war3::internal::
                    kWar3RuntimeConfigMaintainSemanticVisibleHotLookupIndexes) {
    if (record.payload != nullptr)
      snap.byPayload[record.payload] = index;
    if (record.renderablePart != nullptr) {
      snap.byRenderablePart[record.renderablePart] = index;
      snap.renderablePartRecordCount[record.renderablePart]++;
      const uint64_t partLayerKey =
          VisibleRenderablePartLayerKey(record.renderablePart,
                                        record.layerIndex);
      const auto existing = snap.byRenderablePartLayer.find(partLayerKey);
      if (existing == snap.byRenderablePartLayer.end() ||
          existing->second >= snap.records.size() ||
          snap.records[existing->second].queueKind !=
              VisibleRenderableQueueKind::MainQueue) {
        snap.byRenderablePartLayer[partLayerKey] = index;
      }
    }
    if (record.identity.worldObjectEntry != nullptr)
      snap.byWorldObjectEntry[record.identity.worldObjectEntry] = index;
    if (record.identity.jHandle != 0u)
      snap.byHandle[record.identity.jHandle] = index;
    if (record.sceneNode != nullptr)
      snap.bySceneNode[record.sceneNode] = index;
    if (record.meshData != nullptr)
      snap.byMeshData[record.meshData] = index;
    if (record.runtimeModelPtr != nullptr)
      snap.byRuntimeModel[record.runtimeModelPtr] = index;
    if (record.runtimeGeosetPtr != nullptr)
      snap.byRuntimeGeoset[record.runtimeGeosetPtr] = index;
    if (record.runtimeGeosetDataPtr != nullptr)
      snap.byRuntimeGeosetData[record.runtimeGeosetDataPtr] = index;
  }
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDeferSemanticVisibleIndexBuild &&
                !dxvk::war3::internal::
                    kWar3RuntimeConfigMaintainSemanticVisibleHotLookupIndexes &&
                !dxvk::war3::internal::
                    kWar3RuntimeConfigBuildSemanticVisibleIndexesAtEndFrame) {
    if (record.renderablePart != nullptr) {
      snap.byRenderablePart[record.renderablePart] = index;
      snap.renderablePartRecordCount[record.renderablePart]++;
      const uint64_t partLayerKey =
          VisibleRenderablePartLayerKey(record.renderablePart,
                                        record.layerIndex);
      const auto existing = snap.byRenderablePartLayer.find(partLayerKey);
      if (existing == snap.byRenderablePartLayer.end() ||
          existing->second >= snap.records.size() ||
          snap.records[existing->second].queueKind !=
              VisibleRenderableQueueKind::MainQueue) {
        snap.byRenderablePartLayer[partLayerKey] = index;
      }
    }
  }

  if (record.queueKind == VisibleRenderableQueueKind::Transparent)
    ++snap.transparentCount;
  else
    ++snap.mainQueueCount;
}

namespace {

void RebuildVisibleSnapshotIndexes(VisibleRenderableRegistry::Snapshot &snap) {
  const size_t recordCount = snap.records.size();

  snap.byPayload.clear();
  snap.byRenderablePart.clear();
  snap.byRenderablePartLayer.clear();
  snap.renderablePartRecordCount.clear();
  snap.byWorldObjectEntry.clear();
  snap.byHandle.clear();
  snap.bySceneNode.clear();
  snap.byMeshData.clear();
  snap.byRuntimeModel.clear();
  snap.byRuntimeGeoset.clear();
  snap.byRuntimeGeosetData.clear();
  snap.mainQueueCount = 0;
  snap.transparentCount = 0;

  snap.byPayload.reserve(recordCount);
  snap.byRenderablePart.reserve(recordCount);
  snap.byRenderablePartLayer.reserve(recordCount);
  snap.renderablePartRecordCount.reserve(recordCount);
  snap.byWorldObjectEntry.reserve(recordCount);
  snap.byHandle.reserve(recordCount);
  snap.bySceneNode.reserve(recordCount);
  snap.byMeshData.reserve(recordCount);
  snap.byRuntimeModel.reserve(recordCount);
  snap.byRuntimeGeoset.reserve(recordCount);
  snap.byRuntimeGeosetData.reserve(recordCount);

  for (uint32_t index = 0; index < snap.records.size(); ++index) {
    const VisibleRenderableRecord &record = snap.records[index];
    if (record.payload != nullptr)
      snap.byPayload[record.payload] = index;
    if (record.renderablePart != nullptr) {
      snap.byRenderablePart[record.renderablePart] = index;
      snap.renderablePartRecordCount[record.renderablePart]++;
      const uint64_t partLayerKey =
          VisibleRenderablePartLayerKey(record.renderablePart,
                                        record.layerIndex);
      const auto existing = snap.byRenderablePartLayer.find(partLayerKey);
      if (existing == snap.byRenderablePartLayer.end() ||
          existing->second >= snap.records.size() ||
          snap.records[existing->second].queueKind !=
              VisibleRenderableQueueKind::MainQueue) {
        snap.byRenderablePartLayer[partLayerKey] = index;
      }
    }
    if (record.identity.worldObjectEntry != nullptr)
      snap.byWorldObjectEntry[record.identity.worldObjectEntry] = index;
    if (record.identity.jHandle != 0u)
      snap.byHandle[record.identity.jHandle] = index;
    if (record.sceneNode != nullptr)
      snap.bySceneNode[record.sceneNode] = index;
    if (record.meshData != nullptr)
      snap.byMeshData[record.meshData] = index;
    if (record.runtimeModelPtr != nullptr)
      snap.byRuntimeModel[record.runtimeModelPtr] = index;
    if (record.runtimeGeosetPtr != nullptr)
      snap.byRuntimeGeoset[record.runtimeGeosetPtr] = index;
    if (record.runtimeGeosetDataPtr != nullptr)
      snap.byRuntimeGeosetData[record.runtimeGeosetDataPtr] = index;

    if (record.queueKind == VisibleRenderableQueueKind::Transparent)
      ++snap.transparentCount;
    else
      ++snap.mainQueueCount;
  }

  RebuildSemanticMergePointerIndex(snap);
}

void HydrateVisibleSnapshotBasicFields(VisibleRenderableRegistry::Snapshot &snap) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticVisibleEndFrameBasicHydrate) {
    return;
  }

  if (snap.records.empty())
    return;

  bool changed = false;
  // Phase 7.79：thread_local 复用 partCache 避免每帧 alloc/free。
  static thread_local std::unordered_map<void *, RenderablePartBasicFields>
      s_partCache;
  s_partCache.clear();
  s_partCache.reserve(snap.records.size());
  auto &partCache = s_partCache;

  for (uint32_t index = 0u; index < snap.records.size(); ++index) {
    VisibleRenderableRecord& record = snap.records[index];
    void* const oldRenderablePart = record.renderablePart;
    void* const oldPayload = record.payload;
    if (record.payload == nullptr)
      record.payload = record.renderablePart;

    if (record.sceneNode == nullptr && record.identity.sceneNode != nullptr)
      record.sceneNode = record.identity.sceneNode;

    if (record.renderablePart != nullptr &&
        (record.sceneNode == nullptr || record.meshData == nullptr)) {
      auto it = partCache.find(record.renderablePart);
      if (it == partCache.end()) {
        RenderablePartBasicFields fields = {};
        TryReadSceneNodeFromRenderablePart(record.renderablePart, fields.sceneNode);
        TryReadMeshDataFromRenderablePart(record.renderablePart, fields.meshData);
        it = partCache.emplace(record.renderablePart, fields).first;
      }

      if (record.sceneNode == nullptr && it->second.sceneNode != nullptr) {
        record.sceneNode = it->second.sceneNode;
        changed = true;
      }
      if (record.meshData == nullptr && it->second.meshData != nullptr) {
        record.meshData = it->second.meshData;
        changed = true;
      }
    }

    if (record.identity.sceneNode == nullptr && record.sceneNode != nullptr) {
      record.identity.sceneNode = record.sceneNode;
      changed = true;
    }

    RefreshSemanticMergeRecordIndex(snap, index, oldRenderablePart, oldPayload);
  }

  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticVisibleEndFrameUnitGeosetHydrate) {
    constexpr size_t kMaxVisibleGeosetHydrateRecords = 384u;
    size_t hydrated = 0u;
    for (VisibleRenderableRecord& record : snap.records) {
      if (hydrated >= kMaxVisibleGeosetHydrateRecords)
        break;
      if (record.queueKind != VisibleRenderableQueueKind::MainQueue)
        continue;
      if (record.meshData == nullptr && record.renderablePart == nullptr)
        continue;

      const bool needsUnitIdentityBackfill =
          record.identity.kind != ObjectKind::Unit &&
          record.identity.unitPtr == nullptr &&
          record.identity.jHandle == 0u &&
          record.identity.rawcode == 0u &&
          record.sceneNode != nullptr;
      if (needsUnitIdentityBackfill &&
          (record.runtimeModelPtr == nullptr ||
           record.modelResourcePtr == nullptr || record.modelKey == 0u)) {
        ResolveModelMetadata(record.identity, record.sceneNode,
                             record.renderablePart, record.meshData,
                             record.runtimeModelPtr, record.modelResourcePtr,
                             record.modelKey);
        SanitizeRuntimeModelPtrForRecord(record);
      }
      if (record.identity.kind == ObjectKind::Unknown ||
          record.identity.unitPtr == nullptr ||
          record.identity.jHandle == 0u ||
          record.identity.rawcode == 0u) {
        BackfillIdentityFromRuntimeModel(record);
      }

      const bool unitLike =
          record.identity.kind == ObjectKind::Unit ||
          record.identity.unitPtr != nullptr ||
          (record.identity.kind == ObjectKind::Unknown &&
           record.identity.rawcode != 0u);
      if (!unitLike)
        continue;

      ++hydrated;
      const void* beforeRuntime = record.runtimeModelPtr;
      const void* beforeModel = record.modelResourcePtr;
      const void* beforeGeoset = record.runtimeGeosetPtr;
      const void* beforeGeosetData = record.runtimeGeosetDataPtr;
      const uint32_t beforeGeosetIndex = record.geosetIndex;
      const bool missingGeosetMetadata =
          record.runtimeGeosetPtr == nullptr ||
          record.runtimeGeosetDataPtr == nullptr ||
          record.geosetIndex == model::kInvalidShadowGeosetIndex;

      if (record.runtimeModelPtr == nullptr ||
          record.modelResourcePtr == nullptr || record.modelKey == 0u) {
        ResolveModelMetadata(record.identity, record.sceneNode,
                             record.renderablePart, record.meshData,
                             record.runtimeModelPtr, record.modelResourcePtr,
                             record.modelKey);
        SanitizeRuntimeModelPtrForRecord(record);
      }

      if (missingGeosetMetadata) {
        ResolveGeosetMetadata(record);
        if (ResolveRuntimeOwnerFromGeosetBinding(record)) {
          SanitizeRuntimeModelPtrForRecord(record);
          ResolveGeosetMetadata(record);
        }
      }

      changed |= beforeRuntime != record.runtimeModelPtr ||
                 beforeModel != record.modelResourcePtr ||
                 beforeGeoset != record.runtimeGeosetPtr ||
                 beforeGeosetData != record.runtimeGeosetDataPtr ||
                 beforeGeosetIndex != record.geosetIndex;
    }
  }

  if ((changed || dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) &&
      (dxvk::war3::internal::
           kWar3RuntimeConfigBuildSemanticVisibleIndexesAtEndFrame ||
       dxvk::war3::internal::
           kWar3RuntimeConfigSemanticVisibleEndFrameUnitGeosetHydrate))
    RebuildVisibleSnapshotIndexes(snap);
}

bool ShouldHydrateStaticSemanticRecord(const VisibleRenderableRecord& record) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate) {
    return false;
  }

  if (!dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() ||
      dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly) {
    return false;
  }

  return record.identity.kind == ObjectKind::Building ||
         record.identity.kind == ObjectKind::Destructible;
}

void HydrateVisibleSnapshotStaticSemanticFields(
    VisibleRenderableRegistry::Snapshot& snap) {
  if (snap.records.empty())
    return;

  constexpr size_t kMaxStaticHydrateRecords = 64u;
  size_t hydrated = 0u;
  bool changed = false;

  for (uint32_t index = 0u; index < snap.records.size(); ++index) {
    VisibleRenderableRecord& record = snap.records[index];
    if (!ShouldHydrateStaticSemanticRecord(record))
      continue;
    if (hydrated >= kMaxStaticHydrateRecords)
      break;
    ++hydrated;
    void* const oldRenderablePart = record.renderablePart;
    void* const oldPayload = record.payload;

    if (record.payload == nullptr)
      record.payload = record.renderablePart;
    if (record.sceneNode == nullptr && record.identity.sceneNode != nullptr)
      record.sceneNode = record.identity.sceneNode;
    if (record.renderablePart != nullptr) {
      if (record.sceneNode == nullptr)
        changed |= TryReadSceneNodeFromRenderablePart(record.renderablePart,
                                                     record.sceneNode);
      if (record.meshData == nullptr)
        changed |=
            TryReadMeshDataFromRenderablePart(record.renderablePart,
                                             record.meshData);
    }
    if (record.identity.sceneNode == nullptr && record.sceneNode != nullptr) {
      record.identity.sceneNode = record.sceneNode;
      changed = true;
    }

    const bool hadGeoset = record.HasResolvedGeoset();
    ResolveGeosetMetadata(record);
    changed |= !hadGeoset && record.HasResolvedGeoset();
    RefreshSemanticMergeRecordIndex(snap, index, oldRenderablePart, oldPayload);
  }

  if ((changed || dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) &&
      dxvk::war3::internal::
          kWar3RuntimeConfigBuildSemanticVisibleIndexesAtEndFrame) {
    RebuildVisibleSnapshotIndexes(snap);
  }
}

} // namespace

VisibleRenderableRegistry::Snapshot &VisibleRenderableRegistry::writeSnapshot() {
  return m_snapshots[m_writeIndex];
}

const VisibleRenderableRegistry::Snapshot &
VisibleRenderableRegistry::readSnapshot() const {
  const uint32_t index = m_publishedIndex.load(std::memory_order_acquire);
  return m_snapshots[index];
}

const VisibleRenderableRegistry::Snapshot &
VisibleRenderableRegistry::snapshotForThread() const {
  if (std::this_thread::get_id() == m_renderThreadId)
    return m_snapshots[m_writeIndex];
  return readSnapshot();
}

void VisibleRenderableRegistry::beginFrame() {
  m_renderThreadId = std::this_thread::get_id();
  const uint32_t published = m_publishedIndex.load(std::memory_order_relaxed);
  m_writeIndex = (published + 1u) % kSnapshotCount;

  Snapshot &snap = m_snapshots[m_writeIndex];
  const size_t reserveRecord = snap.lastRecordCount;
  snap.records.clear();
  snap.mainQueueCount = 0;
  snap.transparentCount = 0;
  snap.mainQueueRangeCallCount = 0u;
  snap.mainQueueRangeRecordCount = 0u;
  snap.semanticCandidateCallCount = 0u;
  snap.semanticCandidateMergedCount = 0u;
  snap.semanticCandidateAppendedCount = 0u;
  snap.semanticMergeFallbackCallCount = 0u;
  snap.semanticMergeIndexLookupCount = 0u;
  snap.semanticMergeIndexHitCount = 0u;
  snap.semanticMergeIndexCandidateVisitCount = 0u;
  snap.semanticMergeLegacyScanCallCount = 0u;
  snap.semanticMergeLegacyScanRecordVisitCount = 0u;
  snap.semanticMergeVerifierCallCount = 0u;
  snap.semanticMergeVerifierLegacyScanRecordVisitCount = 0u;
  snap.semanticMergeVerifierMismatchCount = 0u;
  snap.semanticMergeVerifierSelectionMismatchCount = 0u;
  snap.semanticMergeVerifierAuxIndexCheckCount = 0u;
  snap.semanticMergeVerifierAuxIndexMismatchCount = 0u;
  snap.transparentEntryCallCount = 0u;
  if (reserveRecord)
    snap.records.reserve(reserveRecord);

  const size_t reserveSemanticMergePointers =
      snap.lastSemanticMergePointerCount;
  snap.semanticMergeByPointer.clear();
  if (War3VisibleSemanticMergeIndexMaintained() &&
      reserveSemanticMergePointers != 0u) {
    snap.semanticMergeByPointer.reserve(reserveSemanticMergePointers);
  }

  constexpr bool kMaintainIndexes =
      !dxvk::war3::internal::kWar3RuntimeConfigDeferSemanticVisibleIndexBuild ||
      dxvk::war3::internal::
          kWar3RuntimeConfigMaintainSemanticVisibleHotLookupIndexes ||
      dxvk::war3::internal::kWar3RuntimeConfigBuildSemanticVisibleIndexesAtEndFrame;

  if constexpr (kMaintainIndexes) {
    const size_t reservePayload = snap.lastPayloadCount;
    const size_t reserveRenderablePart = snap.lastRenderablePartCount;
    const size_t reserveRenderablePartLayer =
        snap.lastRenderablePartLayerCount;
    const size_t reserveRenderablePartRecord =
        snap.lastRenderablePartRecordCount;
    const size_t reserveWorldObject = snap.lastWorldObjectCount;
    const size_t reserveHandle = snap.lastHandleCount;
    const size_t reserveSceneNode = snap.lastSceneNodeCount;
    const size_t reserveMeshData = snap.lastMeshDataCount;
    const size_t reserveRuntimeModel = snap.lastRuntimeModelCount;
    const size_t reserveRuntimeGeoset = snap.lastRuntimeGeosetCount;
    const size_t reserveRuntimeGeosetData = snap.lastRuntimeGeosetDataCount;
    const size_t reserveModelMetadata = snap.lastModelMetadataCount;

    snap.byPayload.clear();
    snap.byRenderablePart.clear();
    snap.byRenderablePartLayer.clear();
    snap.renderablePartRecordCount.clear();
    snap.byWorldObjectEntry.clear();
    snap.byHandle.clear();
    snap.bySceneNode.clear();
    snap.byMeshData.clear();
    snap.byRuntimeModel.clear();
    snap.byRuntimeGeoset.clear();
    snap.byRuntimeGeosetData.clear();
    snap.modelMetadataBySceneNode.clear();

    if (reservePayload)
      snap.byPayload.reserve(reservePayload);
    if (reserveRenderablePart)
      snap.byRenderablePart.reserve(reserveRenderablePart);
    if (reserveRenderablePartLayer)
      snap.byRenderablePartLayer.reserve(reserveRenderablePartLayer);
    if (reserveRenderablePartRecord)
      snap.renderablePartRecordCount.reserve(reserveRenderablePartRecord);
    if (reserveWorldObject)
      snap.byWorldObjectEntry.reserve(reserveWorldObject);
    if (reserveHandle)
      snap.byHandle.reserve(reserveHandle);
    if (reserveSceneNode)
      snap.bySceneNode.reserve(reserveSceneNode);
    if (reserveMeshData)
      snap.byMeshData.reserve(reserveMeshData);
    if (reserveRuntimeModel)
      snap.byRuntimeModel.reserve(reserveRuntimeModel);
    if (reserveRuntimeGeoset)
      snap.byRuntimeGeoset.reserve(reserveRuntimeGeoset);
    if (reserveRuntimeGeosetData)
      snap.byRuntimeGeosetData.reserve(reserveRuntimeGeosetData);
    if (reserveModelMetadata)
      snap.modelMetadataBySceneNode.reserve(reserveModelMetadata);
  } else {
    // 当前数据层性能模式不维护全量多索引表，避免每帧
    // clear/reserve 多张 unordered_map。保留 renderablePart/layer 热索引，
    // 因为 direct shadow path 会高频使用它来避免 slice 串线。
    const size_t reserveRenderablePart = snap.lastRenderablePartCount;
    const size_t reserveRenderablePartLayer =
        snap.lastRenderablePartLayerCount;
    const size_t reserveRenderablePartRecord =
        snap.lastRenderablePartRecordCount;
    if (!snap.byPayload.empty())
      snap.byPayload.clear();
    if (!snap.byRenderablePart.empty())
      snap.byRenderablePart.clear();
    if (!snap.byRenderablePartLayer.empty())
      snap.byRenderablePartLayer.clear();
    if (!snap.renderablePartRecordCount.empty())
      snap.renderablePartRecordCount.clear();
    if (reserveRenderablePart)
      snap.byRenderablePart.reserve(reserveRenderablePart);
    if (reserveRenderablePartLayer)
      snap.byRenderablePartLayer.reserve(reserveRenderablePartLayer);
    if (reserveRenderablePartRecord)
      snap.renderablePartRecordCount.reserve(reserveRenderablePartRecord);
    if (!snap.byWorldObjectEntry.empty())
      snap.byWorldObjectEntry.clear();
    if (!snap.byHandle.empty())
      snap.byHandle.clear();
    if (!snap.bySceneNode.empty())
      snap.bySceneNode.clear();
    if (!snap.byMeshData.empty())
      snap.byMeshData.clear();
    if (!snap.byRuntimeModel.empty())
      snap.byRuntimeModel.clear();
    if (!snap.byRuntimeGeoset.empty())
      snap.byRuntimeGeoset.clear();
    if (!snap.byRuntimeGeosetData.empty())
      snap.byRuntimeGeosetData.clear();
    if (!snap.modelMetadataBySceneNode.empty())
      snap.modelMetadataBySceneNode.clear();
  }

  m_frameNumber.fetch_add(1, std::memory_order_relaxed);
  m_recordCapReached.store(false, std::memory_order_relaxed);
}

void VisibleRenderableRegistry::endFrame() {
  Snapshot &snap = m_snapshots[m_writeIndex];

  // Phase 7.96：桥/斜坡/升降机等地形结构附近 War3 引擎会 dispatch 大量对象，
  // hydrate 里的 BackfillIdentityFromRuntimeModel 对每条 record 做多次
  // registry 查找，在高压场景下 registry 本身有几千条 entries，导致 O(N*M)
  // 行为耗时 30ms+。draw-time producer 不依赖 hydrate 结果，因此安全跳过。
  constexpr size_t kHydrateMaxRecords = 128u;
  if (snap.records.size() <= kHydrateMaxRecords) {
    HydrateVisibleSnapshotBasicFields(snap);
    HydrateVisibleSnapshotStaticSemanticFields(snap);
  }

  if (War3VisibleSemanticMergeIndexVerifierEnabled()) {
    ++snap.semanticMergeVerifierAuxIndexCheckCount;
    const bool auxIndexMismatch =
        !VerifySemanticMergePointerIndex(snap);
    if (auxIndexMismatch) {
      ++snap.semanticMergeVerifierAuxIndexMismatchCount;
      ++snap.semanticMergeVerifierMismatchCount;
      if (War3VisibleSemanticMergeIndexVerifierAssertEnabled()) {
        assert(!auxIndexMismatch &&
               "War3 visible semantic merge auxiliary index mismatch");
      }
    }
  }

  snap.lastRecordCount = snap.records.size();
  snap.lastPayloadCount = snap.byPayload.size();
  snap.lastRenderablePartCount = snap.byRenderablePart.size();
  snap.lastRenderablePartLayerCount = snap.byRenderablePartLayer.size();
  snap.lastRenderablePartRecordCount = snap.renderablePartRecordCount.size();
  snap.lastSemanticMergePointerCount = snap.semanticMergeByPointer.size();
  snap.lastWorldObjectCount = snap.byWorldObjectEntry.size();
  snap.lastHandleCount = snap.byHandle.size();
  snap.lastSceneNodeCount = snap.bySceneNode.size();
  snap.lastMeshDataCount = snap.byMeshData.size();
  snap.lastRuntimeModelCount = snap.byRuntimeModel.size();
  snap.lastRuntimeGeosetCount = snap.byRuntimeGeoset.size();
  snap.lastRuntimeGeosetDataCount = snap.byRuntimeGeosetData.size();
  snap.lastModelMetadataCount = snap.modelMetadataBySceneNode.size();
  m_publishedIndex.store(m_writeIndex, std::memory_order_release);

  // This is diagnostic output, not part of the registry publication
  // contract.  Print() flushes a console synchronously and can stall the
  // render thread for tens of milliseconds on an isolated desktop.  Keep the
  // old cadence when render logging is explicitly requested, but make the
  // production path completely silent.
  if (dxvk::war3dbg::RenderLogEnabled()) {
    static std::atomic<uint32_t> s_logCount{0};
    const uint32_t logCount =
        s_logCount.fetch_add(1, std::memory_order_relaxed);
    if ((snap.mainQueueCount != 0 || snap.transparentCount != 0) &&
        (logCount < 12u || (logCount % 120u) == 0u)) {
      dxvk::war3dbg::Print(
        "DXVK VisibleManifest: frame=%llu total=%zu main=%zu transparent=%zu "
        "payload=%zu part=%zu entry=%zu handle=%zu scene=%zu mesh=%zu "
        "runtime=%zu rtGeo=%zu rtGeoData=%zu ranges=%llu rangeRecords=%llu "
        "semanticCalls=%llu semanticMerged=%llu semanticAppended=%llu "
        "semanticFallback=%llu mergeIndexLookups=%llu mergeIndexHits=%llu "
        "mergeIndexVisits=%llu mergeScans=%llu mergeScanRecords=%llu "
        "mergeVerify=%llu mergeAuxChecks=%llu mergeMismatch=%llu "
        "mergeIndexEntries=%zu "
        "transparentCalls=%llu\n",
        static_cast<unsigned long long>(
            m_frameNumber.load(std::memory_order_relaxed)),
        snap.records.size(), snap.mainQueueCount, snap.transparentCount,
        snap.byPayload.size(), snap.byRenderablePart.size(),
        snap.byWorldObjectEntry.size(), snap.byHandle.size(),
        snap.bySceneNode.size(), snap.byMeshData.size(),
        snap.byRuntimeModel.size(), snap.byRuntimeGeoset.size(),
        snap.byRuntimeGeosetData.size(),
        static_cast<unsigned long long>(snap.mainQueueRangeCallCount),
        static_cast<unsigned long long>(snap.mainQueueRangeRecordCount),
        static_cast<unsigned long long>(snap.semanticCandidateCallCount),
        static_cast<unsigned long long>(snap.semanticCandidateMergedCount),
        static_cast<unsigned long long>(snap.semanticCandidateAppendedCount),
        static_cast<unsigned long long>(snap.semanticMergeFallbackCallCount),
        static_cast<unsigned long long>(snap.semanticMergeIndexLookupCount),
        static_cast<unsigned long long>(snap.semanticMergeIndexHitCount),
        static_cast<unsigned long long>(
            snap.semanticMergeIndexCandidateVisitCount),
        static_cast<unsigned long long>(snap.semanticMergeLegacyScanCallCount),
        static_cast<unsigned long long>(
            snap.semanticMergeLegacyScanRecordVisitCount),
        static_cast<unsigned long long>(snap.semanticMergeVerifierCallCount),
        static_cast<unsigned long long>(
            snap.semanticMergeVerifierAuxIndexCheckCount),
        static_cast<unsigned long long>(
            snap.semanticMergeVerifierMismatchCount),
        snap.semanticMergeByPointer.size(),
        static_cast<unsigned long long>(snap.transparentEntryCallCount));
    }
  }
}

void VisibleRenderableRegistry::registerMainQueueRange(
    void *batchArray, uint32_t before, uint32_t after,
    const RenderObjectIdentitySnapshot &identity) {
  if constexpr (!dxvk::war3::internal::kNativeVisibleRenderableRegistryEnabled) {
    return;
  }
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleRenderableWrites) {
    return;
  }

  if (!batchArray || after <= before)
    return;

  // Phase 7.96：cap 已触发后，后续调用在入口直接 return，避免函数调用累积开销。
  if (m_recordCapReached.load(std::memory_order_relaxed))
    return;

  m_renderThreadId = std::this_thread::get_id();
  Snapshot &snap = writeSnapshot();

  // Phase 7.96：record 数量超过阈值时拒绝新增，从源头控制 endFrame 成本。
  if (snap.records.size() >=
      dxvk::war3::internal::kWar3RuntimeConfigSemanticVisibleEndFrameMaxRecords) {
    m_recordCapReached.store(true, std::memory_order_relaxed);
    return;
  }

  // Phase 7.96：限制单次调用写入的 record 数量，避免单次 batch 写入几千条。
  const uint32_t maxRemaining = static_cast<uint32_t>(
      dxvk::war3::internal::kWar3RuntimeConfigSemanticVisibleEndFrameMaxRecords -
      snap.records.size());
  const uint32_t effectiveAfter =
      (after - before > maxRemaining) ? (before + maxRemaining) : after;

  snap.mainQueueRangeCallCount++;
  snap.mainQueueRangeRecordCount += uint64_t(effectiveAfter - before);
  snap.records.reserve(snap.records.size() + (effectiveAfter - before));
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDeferSemanticVisibleIndexBuild ||
                dxvk::war3::internal::
                    kWar3RuntimeConfigMaintainSemanticVisibleHotLookupIndexes) {
    snap.byPayload.reserve(snap.byPayload.size() + (effectiveAfter - before));
    snap.byRenderablePart.reserve(snap.byRenderablePart.size() + (effectiveAfter - before));
    snap.byWorldObjectEntry.reserve(snap.byWorldObjectEntry.size() + (effectiveAfter - before));
    snap.byHandle.reserve(snap.byHandle.size() + (effectiveAfter - before));
    snap.bySceneNode.reserve(snap.bySceneNode.size() + (effectiveAfter - before));
    snap.byMeshData.reserve(snap.byMeshData.size() + (effectiveAfter - before));
    snap.byRuntimeModel.reserve(snap.byRuntimeModel.size() + (effectiveAfter - before));
    snap.byRuntimeGeoset.reserve(snap.byRuntimeGeoset.size() + (effectiveAfter - before));
    snap.byRuntimeGeosetData.reserve(
        snap.byRuntimeGeosetData.size() + (effectiveAfter - before));
  }

  auto *base = reinterpret_cast<std::uint8_t *>(batchArray);
  for (uint32_t i = before; i < effectiveAfter; ++i) {
    auto *element = base + i * kRenderBatchElementStride;

    VisibleRenderableRecord record = {};
    record.queueKind = VisibleRenderableQueueKind::MainQueue;
    record.stage = static_cast<int16_t>(War3RenderState::GetStage());
    record.payload = *reinterpret_cast<void **>(
        element + RenderBatchElementOffsets::BatchEntry);
    record.renderablePart = *reinterpret_cast<void **>(
        element + RenderBatchElementOffsets::BatchEntry);
    record.flags = *reinterpret_cast<uint32_t *>(
        element + RenderBatchElementOffsets::Flags);
    record.layerIndex = *reinterpret_cast<uint32_t *>(
        element + RenderBatchElementOffsets::LayerIndex);
    record.subIndex = *reinterpret_cast<uint32_t *>(
        element + RenderBatchElementOffsets::LayerCounter);
    record.layerState = *reinterpret_cast<void **>(
        element + RenderBatchElementOffsets::LayerState);
    record.queueSlot = i;
    record.identity = identity;
    if (!record.identity.HasStableIdentity() ||
        record.identity.kind == ObjectKind::Unknown ||
        record.identity.rawcode == 0u || record.identity.jHandle == 0u) {
      RenderObjectIdentitySnapshot cachedIdentity = {};
      if (RenderQueueTracker::instance().GetCachedObjectIdentity(
              record.payload, cachedIdentity)) {
        if (record.identity.worldObjectEntry == nullptr)
          record.identity.worldObjectEntry = cachedIdentity.worldObjectEntry;
        if (record.identity.sceneNode == nullptr)
          record.identity.sceneNode = cachedIdentity.sceneNode;
        if (record.identity.unitPtr == nullptr)
          record.identity.unitPtr = cachedIdentity.unitPtr;
        if (record.identity.agentPtr == nullptr)
          record.identity.agentPtr = cachedIdentity.agentPtr;
        if (record.identity.handleId == 0u)
          record.identity.handleId = cachedIdentity.handleId;
        if (record.identity.jHandle == 0u)
          record.identity.jHandle = cachedIdentity.jHandle;
        if (record.identity.rawcode == 0u)
          record.identity.rawcode = cachedIdentity.rawcode;
        if (record.identity.agentType == 0u)
          record.identity.agentType = cachedIdentity.agentType;
        if (record.identity.flags5C == 0u)
          record.identity.flags5C = cachedIdentity.flags5C;
        if (record.identity.kind == ObjectKind::Unknown)
          record.identity.kind = cachedIdentity.kind;
        if (record.identity.groupIdx < 0)
          record.identity.groupIdx = cachedIdentity.groupIdx;
        if (record.sceneNode == nullptr)
          record.sceneNode = cachedIdentity.sceneNode;
      }
    }
    appendRecord(snap, record);
  }
}

bool VisibleRenderableRegistry::registerSemanticCandidate(
    const VisibleRenderableRecord &candidate) {
  if constexpr (!dxvk::war3::internal::kNativeVisibleRenderableRegistryEnabled) {
    return false;
  }
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleRenderableWrites) {
    return false;
  }

  if (!candidate.HasStableIdentity() && candidate.sceneNode == nullptr &&
      candidate.renderablePart == nullptr && candidate.meshData == nullptr &&
      candidate.runtimeGeosetPtr == nullptr &&
      candidate.runtimeGeosetDataPtr == nullptr) {
    return false;
  }

  m_renderThreadId = std::this_thread::get_id();
  Snapshot &snap = writeSnapshot();

  // Phase 7.96：cap 已触发后直接 return。
  if (m_recordCapReached.load(std::memory_order_relaxed))
    return false;

  // Phase 7.96：record 数量超过阈值时拒绝新增，从源头控制 endFrame 成本。
  if (snap.records.size() >=
      dxvk::war3::internal::kWar3RuntimeConfigSemanticVisibleEndFrameMaxRecords) {
    m_recordCapReached.store(true, std::memory_order_relaxed);
    return false;
  }

  snap.semanticCandidateCallCount++;
  auto canMergeVisibleSubpart =
      [](const VisibleRenderableRecord& existing,
         const VisibleRenderableRecord& incoming) {
        const bool sameRenderablePart =
            incoming.renderablePart != nullptr &&
            existing.renderablePart == incoming.renderablePart;
        const bool samePayload =
            incoming.payload != nullptr && existing.payload == incoming.payload;
        if (!sameRenderablePart && !samePayload)
          return true;

        // A single renderable part can emit several visible layers/subparts.
        // Merging those records collapses the current primitive slice and makes
        // the semantic shadow path fall back to the whole CGeosetData, including
        // hidden construction/material variants. Only merge when the visible
        // layer identity is compatible.
        if (existing.layerIndex != incoming.layerIndex ||
            existing.subIndex != incoming.subIndex) {
          return false;
        }
        if (existing.layerState != nullptr && incoming.layerState != nullptr &&
            existing.layerState != incoming.layerState) {
          return false;
        }
        return true;
      };
  auto mergeCandidateFields = [&](VisibleRenderableRecord& existing) {
    if (!canMergeVisibleSubpart(existing, candidate))
      return false;
    if (existing.payload == nullptr && candidate.payload != nullptr) {
      existing.payload = candidate.payload;
    }
    if (existing.renderablePart == nullptr && candidate.renderablePart != nullptr) {
      existing.renderablePart = candidate.renderablePart;
    }
    if (existing.sceneNode == nullptr && candidate.sceneNode != nullptr) {
      existing.sceneNode = candidate.sceneNode;
    }
    if (existing.meshData == nullptr && candidate.meshData != nullptr) {
      existing.meshData = candidate.meshData;
    }
    if (existing.layerState == nullptr && candidate.layerState != nullptr) {
      existing.layerState = candidate.layerState;
    }
    if (existing.runtimeModelPtr == nullptr && candidate.runtimeModelPtr != nullptr) {
      existing.runtimeModelPtr = candidate.runtimeModelPtr;
    }
    if (existing.modelResourcePtr == nullptr &&
        candidate.modelResourcePtr != nullptr) {
      existing.modelResourcePtr = candidate.modelResourcePtr;
    }
    if (existing.runtimeGeosetPtr == nullptr &&
        candidate.runtimeGeosetPtr != nullptr) {
      existing.runtimeGeosetPtr = candidate.runtimeGeosetPtr;
    }
    if (existing.runtimeGeosetDataPtr == nullptr &&
        candidate.runtimeGeosetDataPtr != nullptr) {
      existing.runtimeGeosetDataPtr = candidate.runtimeGeosetDataPtr;
    }
    if (existing.modelKey == 0u && candidate.modelKey != 0u) {
      existing.modelKey = candidate.modelKey;
    }
    if (existing.meshIndex == kInvalidVisibleMeshIndex &&
        candidate.meshIndex != kInvalidVisibleMeshIndex) {
      existing.meshIndex = candidate.meshIndex;
    }
    if (existing.geosetIndex == kInvalidVisibleMeshIndex &&
        candidate.geosetIndex != kInvalidVisibleMeshIndex) {
      existing.geosetIndex = candidate.geosetIndex;
    }
    if (existing.layerIndex == 0u && candidate.layerIndex != 0u)
      existing.layerIndex = candidate.layerIndex;
    if (existing.subIndex == 0u && candidate.subIndex != 0u)
      existing.subIndex = candidate.subIndex;
    if (existing.identity.worldObjectEntry == nullptr)
      existing.identity.worldObjectEntry = candidate.identity.worldObjectEntry;
    if (existing.identity.sceneNode == nullptr)
      existing.identity.sceneNode = candidate.identity.sceneNode;
    if (existing.identity.unitPtr == nullptr)
      existing.identity.unitPtr = candidate.identity.unitPtr;
    if (existing.identity.agentPtr == nullptr)
      existing.identity.agentPtr = candidate.identity.agentPtr;
    if (existing.identity.handleId == 0u)
      existing.identity.handleId = candidate.identity.handleId;
    if (existing.identity.jHandle == 0u)
      existing.identity.jHandle = candidate.identity.jHandle;
    if (existing.identity.rawcode == 0u)
      existing.identity.rawcode = candidate.identity.rawcode;
    if (existing.identity.agentType == 0u)
      existing.identity.agentType = candidate.identity.agentType;
    if (existing.identity.flags5C == 0u)
      existing.identity.flags5C = candidate.identity.flags5C;
    if (existing.identity.kind == ObjectKind::Unknown)
      existing.identity.kind = candidate.identity.kind;
    if (existing.identity.groupIdx < 0)
      existing.identity.groupIdx = candidate.identity.groupIdx;
    existing.pathBlocker =
        existing.pathBlocker || candidate.pathBlocker ||
        dxvk::war3::internal::IsPathBlockerFourCc(existing.identity.rawcode);
    return true;
  };

  auto mergeCandidateIntoExisting = [&](uint32_t index) {
    if (index >= snap.records.size())
      return false;
    VisibleRenderableRecord& existing = snap.records[index];
    void* const oldRenderablePart = existing.renderablePart;
    void* const oldPayload = existing.payload;
    if (!mergeCandidateFields(existing))
      return false;

    RefreshSemanticMergeRecordIndex(
        snap, index, oldRenderablePart, oldPayload);

    if constexpr (!dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild ||
                  dxvk::war3::internal::
                      kWar3RuntimeConfigMaintainSemanticVisibleHotLookupIndexes) {
      if (existing.payload != nullptr)
        snap.byPayload[existing.payload] = index;
      if (existing.renderablePart != nullptr)
        snap.byRenderablePart[existing.renderablePart] = index;
      if (existing.identity.worldObjectEntry != nullptr)
        snap.byWorldObjectEntry[existing.identity.worldObjectEntry] = index;
      if (existing.identity.jHandle != 0u)
        snap.byHandle[existing.identity.jHandle] = index;
      if (existing.sceneNode != nullptr)
        snap.bySceneNode[existing.sceneNode] = index;
      if (existing.meshData != nullptr)
        snap.byMeshData[existing.meshData] = index;
      if (existing.runtimeModelPtr != nullptr)
        snap.byRuntimeModel[existing.runtimeModelPtr] = index;
      if (existing.runtimeGeosetPtr != nullptr)
        snap.byRuntimeGeoset[existing.runtimeGeosetPtr] = index;
      if (existing.runtimeGeosetDataPtr != nullptr)
        snap.byRuntimeGeosetData[existing.runtimeGeosetDataPtr] = index;
    }
    return true;
  };
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDeferSemanticVisibleIndexBuild &&
                !dxvk::war3::internal::
                    kWar3RuntimeConfigMaintainSemanticVisibleHotLookupIndexes) {
    auto tryMergeIndexedCandidate = [&](uint32_t index) {
      if (index < snap.records.size() && mergeCandidateIntoExisting(index)) {
        snap.semanticCandidateMergedCount++;
        return true;
      }
      return false;
    };
    if (candidate.renderablePart != nullptr) {
      const uint64_t partLayerKey =
          VisibleRenderablePartLayerKey(candidate.renderablePart,
                                        candidate.layerIndex);
      const auto layerIt = snap.byRenderablePartLayer.find(partLayerKey);
      if (layerIt != snap.byRenderablePartLayer.end() &&
          tryMergeIndexedCandidate(layerIt->second)) {
        return true;
      }
      const auto partIt = snap.byRenderablePart.find(candidate.renderablePart);
      if (partIt != snap.byRenderablePart.end() &&
          tryMergeIndexedCandidate(partIt->second)) {
        return true;
      }
    }

    ++snap.semanticMergeFallbackCallCount;
    const bool partPayloadFallback =
        candidate.renderablePart != nullptr || candidate.payload != nullptr;
    const bool useSemanticMergeIndex =
        partPayloadFallback && War3VisibleSemanticMergeIndexEnabled();
    const bool verifySemanticMergeIndex =
        partPayloadFallback &&
        War3VisibleSemanticMergeIndexVerifierEnabled();

    auto selectLegacyFallback = [&](uint64_t& recordVisitCount) {
      for (uint32_t index = 0u; index < snap.records.size(); ++index) {
        const auto& existing = snap.records[index];
        if (candidate.renderablePart != nullptr &&
            existing.renderablePart == candidate.renderablePart) {
          if (canMergeVisibleSubpart(existing, candidate)) {
            recordVisitCount = uint64_t(index) + 1u;
            return index;
          }
          continue;
        }
        if (candidate.payload != nullptr &&
            existing.payload == candidate.payload) {
          if (canMergeVisibleSubpart(existing, candidate)) {
            recordVisitCount = uint64_t(index) + 1u;
            return index;
          }
          continue;
        }
        if (candidate.renderablePart == nullptr &&
            candidate.payload == nullptr &&
            candidate.runtimeGeosetPtr != nullptr &&
            existing.runtimeGeosetPtr == candidate.runtimeGeosetPtr) {
          if (canMergeVisibleSubpart(existing, candidate)) {
            recordVisitCount = uint64_t(index) + 1u;
            return index;
          }
          continue;
        }
        if (candidate.renderablePart == nullptr &&
            candidate.payload == nullptr &&
            candidate.runtimeGeosetDataPtr != nullptr &&
            existing.runtimeGeosetDataPtr ==
                candidate.runtimeGeosetDataPtr) {
          if (canMergeVisibleSubpart(existing, candidate)) {
            recordVisitCount = uint64_t(index) + 1u;
            return index;
          }
          continue;
        }
      }
      recordVisitCount = snap.records.size();
      return kInvalidSemanticMergeRecordIndex;
    };

    auto selectIndexedPartPayloadFallback = [&]() {
      ++snap.semanticMergeIndexLookupCount;
      uint32_t bestIndex = kInvalidSemanticMergeRecordIndex;

      auto visitPointer = [&](void* pointer) {
        if (pointer == nullptr)
          return;
        const auto range = snap.semanticMergeByPointer.equal_range(pointer);
        for (auto it = range.first; it != range.second; ++it) {
          ++snap.semanticMergeIndexCandidateVisitCount;
          const uint32_t index = it->second;
          if (index >= snap.records.size() || index >= bestIndex)
            continue;

          // semanticMergeByPointer is deliberately an untagged union of
          // renderablePart/payload keys. Re-check the original field-specific
          // predicates before canMergeVisibleSubpart: a cross-field pointer
          // coincidence is not a legacy fallback match.
          const auto& existing = snap.records[index];
          const bool exactPartMatch =
              candidate.renderablePart != nullptr &&
              existing.renderablePart == candidate.renderablePart;
          const bool exactPayloadMatch =
              candidate.payload != nullptr &&
              existing.payload == candidate.payload;
          if (!exactPartMatch && !exactPayloadMatch)
            continue;
          if (!canMergeVisibleSubpart(existing, candidate))
            continue;
          bestIndex = index;
        }
      };

      visitPointer(candidate.renderablePart);
      if (candidate.payload != candidate.renderablePart)
        visitPointer(candidate.payload);
      if (bestIndex != kInvalidSemanticMergeRecordIndex)
        ++snap.semanticMergeIndexHitCount;
      return bestIndex;
    };

    uint32_t legacySelection = kInvalidSemanticMergeRecordIndex;
    uint32_t indexedSelection = kInvalidSemanticMergeRecordIndex;
    uint64_t legacyRecordVisits = 0u;
    if (!useSemanticMergeIndex || verifySemanticMergeIndex) {
      legacySelection = selectLegacyFallback(legacyRecordVisits);
      if (!useSemanticMergeIndex) {
        ++snap.semanticMergeLegacyScanCallCount;
        snap.semanticMergeLegacyScanRecordVisitCount += legacyRecordVisits;
      }
      if (verifySemanticMergeIndex) {
        ++snap.semanticMergeVerifierCallCount;
        snap.semanticMergeVerifierLegacyScanRecordVisitCount +=
            legacyRecordVisits;
      }
    }
    if (useSemanticMergeIndex || verifySemanticMergeIndex)
      indexedSelection = selectIndexedPartPayloadFallback();

    const uint32_t productionSelection =
        useSemanticMergeIndex ? indexedSelection : legacySelection;

    // Both selectors feed the same merge/append code below. Matching selected
    // indices therefore also preserves the resulting record, ordering, counts
    // and any deterministic content hash without cloning the snapshot.
    if (verifySemanticMergeIndex &&
        legacySelection != indexedSelection) {
      ++snap.semanticMergeVerifierSelectionMismatchCount;
      ++snap.semanticMergeVerifierMismatchCount;
      if (War3VisibleSemanticMergeIndexVerifierAssertEnabled()) {
        assert(legacySelection == indexedSelection &&
               "War3 visible semantic merge index selection mismatch");
      }
    }

    if (productionSelection != kInvalidSemanticMergeRecordIndex) {
      const bool merged =
          mergeCandidateIntoExisting(productionSelection);
      if (merged) {
        ++snap.semanticCandidateMergedCount;
      } else {
        // Both selectors only return bounds-checked, compatible records.
        // Preserve fail-safe append behavior if an invariant is ever violated.
        VisibleRenderableRecord record = candidate;
        record.queueKind = VisibleRenderableQueueKind::MainQueue;
        appendRecord(snap, record);
        ++snap.semanticCandidateAppendedCount;
      }
    } else {
      VisibleRenderableRecord record = candidate;
      record.queueKind = VisibleRenderableQueueKind::MainQueue;
      appendRecord(snap, record);
      ++snap.semanticCandidateAppendedCount;
    }
    return true;
  } else {
    if (candidate.renderablePart != nullptr &&
        snap.byRenderablePart.find(candidate.renderablePart) !=
            snap.byRenderablePart.end()) {
      if (mergeCandidateIntoExisting(
              snap.byRenderablePart[candidate.renderablePart])) {
        snap.semanticCandidateMergedCount++;
        return true;
      }
    }
    if (candidate.payload != nullptr &&
        snap.byPayload.find(candidate.payload) != snap.byPayload.end()) {
      if (mergeCandidateIntoExisting(snap.byPayload[candidate.payload])) {
        snap.semanticCandidateMergedCount++;
        return true;
      }
    }
    if (candidate.renderablePart == nullptr && candidate.payload == nullptr &&
        candidate.runtimeGeosetPtr != nullptr &&
        snap.byRuntimeGeoset.find(candidate.runtimeGeosetPtr) !=
            snap.byRuntimeGeoset.end()) {
      if (mergeCandidateIntoExisting(
              snap.byRuntimeGeoset[candidate.runtimeGeosetPtr])) {
        snap.semanticCandidateMergedCount++;
        return true;
      }
    }
    if (candidate.renderablePart == nullptr && candidate.payload == nullptr &&
        candidate.runtimeGeosetDataPtr != nullptr &&
        snap.byRuntimeGeosetData.find(candidate.runtimeGeosetDataPtr) !=
            snap.byRuntimeGeosetData.end()) {
      if (mergeCandidateIntoExisting(
              snap.byRuntimeGeosetData[candidate.runtimeGeosetDataPtr])) {
        snap.semanticCandidateMergedCount++;
        return true;
      }
    }
  }

  VisibleRenderableRecord record = candidate;
  record.queueKind = VisibleRenderableQueueKind::MainQueue;
  appendRecord(snap, record);
  snap.semanticCandidateAppendedCount++;
  return true;
}

void VisibleRenderableRegistry::registerTransparentEntry(
    void *payload, uint32_t transparentType, uint32_t queueSlot,
    uint32_t sortKey, float distanceSq,
    const RenderObjectIdentitySnapshot &identity) {
  if constexpr (!dxvk::war3::internal::kNativeVisibleRenderableRegistryEnabled) {
    return;
  }
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleRenderableWrites) {
    return;
  }

  if (payload == nullptr && !identity.HasContext())
    return;

  // Phase 7.96：cap 已触发后直接 return。
  if (m_recordCapReached.load(std::memory_order_relaxed))
    return;

  m_renderThreadId = std::this_thread::get_id();
  Snapshot &snap = writeSnapshot();

  // Phase 7.96：record 数量超过阈值时拒绝新增，从源头控制 endFrame 成本。
  if (snap.records.size() >=
      dxvk::war3::internal::kWar3RuntimeConfigSemanticVisibleEndFrameMaxRecords) {
    m_recordCapReached.store(true, std::memory_order_relaxed);
    return;
  }

  snap.transparentEntryCallCount++;
  snap.records.reserve(snap.records.size() + 1u);
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
    snap.byPayload.reserve(snap.byPayload.size() + 1u);
    snap.byRenderablePart.reserve(snap.byRenderablePart.size() + 1u);
    snap.byWorldObjectEntry.reserve(snap.byWorldObjectEntry.size() + 1u);
    snap.byHandle.reserve(snap.byHandle.size() + 1u);
    snap.bySceneNode.reserve(snap.bySceneNode.size() + 1u);
    snap.byMeshData.reserve(snap.byMeshData.size() + 1u);
    snap.byRuntimeModel.reserve(snap.byRuntimeModel.size() + 1u);
    snap.byRuntimeGeoset.reserve(snap.byRuntimeGeoset.size() + 1u);
    snap.byRuntimeGeosetData.reserve(snap.byRuntimeGeosetData.size() + 1u);
  }

  VisibleRenderableRecord record = {};
  record.queueKind = VisibleRenderableQueueKind::Transparent;
  record.stage = static_cast<int16_t>(War3RenderState::GetStage());
  record.payload = payload;
  record.queueSlot = queueSlot;
  record.transparentType = transparentType;
  record.transparentSortKey = sortKey;
  record.transparentDistanceSq = distanceSq;
  record.identity = identity;

  TryResolveRenderablePartFromTransparentPayload(payload, transparentType,
                                                 identity, record.renderablePart,
                                                 record.sceneNode,
                                                 record.meshData);
  appendRecord(snap, record);
}

bool VisibleRenderableRegistry::queryByPayload(void *payload,
                                               VisibleRenderableRecord &out) const {
  out = {};
  if (!payload)
    return false;

  const Snapshot &snap = snapshotForThread();
  const auto it = snap.byPayload.find(payload);
  if (it == snap.byPayload.end() || it->second >= snap.records.size()) {
    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& record : snap.records) {
        if (record.payload == payload) {
          out = record;
          return true;
        }
      }
    }
    return false;
  }

  out = snap.records[it->second];
  return true;
}

bool VisibleRenderableRegistry::queryByRenderablePart(
    void *renderablePart, VisibleRenderableRecord &out) const {
  out = {};
  if (!renderablePart)
    return false;

  const Snapshot &snap = snapshotForThread();
  const auto it = snap.byRenderablePart.find(renderablePart);
  if (it == snap.byRenderablePart.end() || it->second >= snap.records.size()) {
    // Phase 7.26：deferred-index 模式下若 record-count 索引里也没有对应项，
    // 说明本帧 snapshot 根本不包含该 renderablePart，没必要再做 O(records)
    // 全量扫描。
    const auto countIt = snap.renderablePartRecordCount.find(renderablePart);
    if (countIt == snap.renderablePartRecordCount.end())
      return false;

    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& record : snap.records) {
        if (record.renderablePart == renderablePart) {
          out = record;
          return true;
        }
      }
    }
    return false;
  }

  out = snap.records[it->second];
  return true;
}

bool VisibleRenderableRegistry::queryByRenderablePartAndLayer(
    void *renderablePart, uint32_t layerIndex,
    VisibleRenderableRecord &out) const {
  out = {};
  if (!renderablePart)
    return false;

  const Snapshot &snap = snapshotForThread();
  const uint64_t partLayerKey =
      VisibleRenderablePartLayerKey(renderablePart, layerIndex);
  const auto layerIt = snap.byRenderablePartLayer.find(partLayerKey);
  if (layerIt != snap.byRenderablePartLayer.end() &&
      layerIt->second < snap.records.size()) {
    out = snap.records[layerIt->second];
    m_shadowManifestVisibleLookupPartLayerHitCount.fetch_add(
        1u, std::memory_order_relaxed);
    return true;
  }

  const auto countIt = snap.renderablePartRecordCount.find(renderablePart);
  const bool mayUseSoleFallback =
      countIt != snap.renderablePartRecordCount.end() && countIt->second == 1u;
  const auto partIt = snap.byRenderablePart.find(renderablePart);
  if (mayUseSoleFallback && partIt != snap.byRenderablePart.end() &&
      partIt->second < snap.records.size()) {
    out = snap.records[partIt->second];
    m_shadowManifestVisibleLookupSingleFallbackCount.fetch_add(
        1u, std::memory_order_relaxed);
    return true;
  }

  // Phase 7.26：当索引表不存在该 renderablePart 时，直接认定为 miss。
  // 原本在 deferred-index 模式下会对整个 snapshot 做线性扫描，实测
  // 热路径每帧可产生 1e4 级别全扫描。这些 miss 对 core gate 决策没有
  // 贡献，shadow submission 已经不依赖 visible lookup 去决定 existence，
  // 所以提前短路是安全的。
  if (partIt == snap.byRenderablePart.end() &&
      countIt == snap.renderablePartRecordCount.end()) {
    m_shadowManifestVisibleLookupMissCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }

  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
    const VisibleRenderableRecord *transparentFallback = nullptr;
    const VisibleRenderableRecord *solePartRecord = nullptr;
    uint32_t partRecordCount = 0u;
    for (const auto &record : snap.records) {
      if (record.renderablePart != renderablePart)
        continue;
      ++partRecordCount;
      solePartRecord = &record;
      if (record.layerIndex == layerIndex) {
        if (record.queueKind == VisibleRenderableQueueKind::MainQueue) {
          out = record;
          m_shadowManifestVisibleLookupPartLayerHitCount.fetch_add(
              1u, std::memory_order_relaxed);
          return true;
        }
        if (transparentFallback == nullptr)
          transparentFallback = &record;
      }
    }

    if (transparentFallback != nullptr) {
      out = *transparentFallback;
      m_shadowManifestVisibleLookupPartLayerHitCount.fetch_add(
          1u, std::memory_order_relaxed);
      return true;
    }

    if (partRecordCount == 1u && solePartRecord != nullptr) {
      out = *solePartRecord;
      m_shadowManifestVisibleLookupSingleFallbackCount.fetch_add(
          1u, std::memory_order_relaxed);
      return true;
    }
  }
  m_shadowManifestVisibleLookupMissCount.fetch_add(
      1u, std::memory_order_relaxed);
  return false;
}

void VisibleRenderablePartLayerQueryCache::reset() noexcept {
  ++m_generation;
  if (m_generation == 0u) {
    m_entries = {};
    ++m_generation;
  }
}

bool VisibleRenderablePartLayerQueryCache::query(
    const VisibleRenderableRegistry& registry,
    void* renderablePart, uint32_t layerIndex,
    VisibleRenderableRecord& out) noexcept {
  out = {};
  if (renderablePart == nullptr)
    return false;

  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(
      hash, uint64_t(reinterpret_cast<uintptr_t>(renderablePart)));
  hash = bit::fnv1a_iter(hash, layerIndex);
  Entry& entry = m_entries[size_t(hash) & (kEntryCount - 1u)];
  if (entry.generation == m_generation &&
      entry.renderablePart == renderablePart &&
      entry.layerIndex == layerIndex) {
    if (entry.found)
      out = entry.record;
    return entry.found;
  }

  VisibleRenderableRecord record = {};
  const bool found = registry.queryByRenderablePartAndLayer(
      renderablePart, layerIndex, record);
  entry.renderablePart = renderablePart;
  entry.layerIndex = layerIndex;
  entry.generation = m_generation;
  entry.found = found;
  entry.record = found ? record : VisibleRenderableRecord{};
  if (found)
    out = record;
  return found;
}

bool VisibleRenderableRegistry::queryFirstForDirectPacket(
    const CurrentDrawContractRecord& record,
    VisibleRenderableRecord& out) const {
  out = {};
  const Snapshot& snap = snapshotForThread();

  // Preserve the canonical lookup order. An exact part/layer match is already
  // the strongest identity available here; only the weaker payload/scene
  // fallbacks need the current-draw slice compatibility gate.
  if (record.renderablePart != nullptr) {
    const uint64_t partLayerKey =
        VisibleRenderablePartLayerKey(record.renderablePart,
                                      record.layerIndex);
    const auto layerIt = snap.byRenderablePartLayer.find(partLayerKey);
    if (layerIt != snap.byRenderablePartLayer.end() &&
        layerIt->second < snap.records.size()) {
      out = snap.records[layerIt->second];
      m_shadowManifestVisibleLookupPartLayerHitCount.fetch_add(
          1u, std::memory_order_relaxed);
      return true;
    }

    const auto countIt =
        snap.renderablePartRecordCount.find(record.renderablePart);
    const auto partIt = snap.byRenderablePart.find(record.renderablePart);
    if (countIt != snap.renderablePartRecordCount.end() &&
        countIt->second == 1u && partIt != snap.byRenderablePart.end() &&
        partIt->second < snap.records.size()) {
      out = snap.records[partIt->second];
      m_shadowManifestVisibleLookupSingleFallbackCount.fetch_add(
          1u, std::memory_order_relaxed);
      return true;
    }

    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      const VisibleRenderableRecord* transparentFallback = nullptr;
      const VisibleRenderableRecord* solePartRecord = nullptr;
      uint32_t partRecordCount = 0u;
      for (const auto& candidate : snap.records) {
        if (candidate.renderablePart != record.renderablePart)
          continue;
        ++partRecordCount;
        solePartRecord = &candidate;
        if (candidate.layerIndex == record.layerIndex) {
          if (candidate.queueKind == VisibleRenderableQueueKind::MainQueue) {
            out = candidate;
            m_shadowManifestVisibleLookupPartLayerHitCount.fetch_add(
                1u, std::memory_order_relaxed);
            return true;
          }
          if (transparentFallback == nullptr)
            transparentFallback = &candidate;
        }
      }

      if (transparentFallback != nullptr) {
        out = *transparentFallback;
        m_shadowManifestVisibleLookupPartLayerHitCount.fetch_add(
            1u, std::memory_order_relaxed);
        return true;
      }
      if (partRecordCount == 1u && solePartRecord != nullptr) {
        out = *solePartRecord;
        m_shadowManifestVisibleLookupSingleFallbackCount.fetch_add(
            1u, std::memory_order_relaxed);
        return true;
      }
    }
  }

  const auto matchesCurrentDrawSlice =
      [&record](const VisibleRenderableRecord& candidate) {
        if (candidate.layerIndex != record.layerIndex)
          return false;
        if (record.renderablePart != nullptr &&
            candidate.renderablePart != nullptr &&
            record.renderablePart != candidate.renderablePart)
          return false;
        if (record.sceneNode != nullptr && candidate.sceneNode != nullptr &&
            record.sceneNode != candidate.sceneNode)
          return false;
        return true;
      };

  if (record.renderablePart != nullptr) {
    const auto payloadIt = snap.byPayload.find(record.renderablePart);
    if (payloadIt != snap.byPayload.end() &&
        payloadIt->second < snap.records.size()) {
      const auto& candidate = snap.records[payloadIt->second];
      if (matchesCurrentDrawSlice(candidate)) {
        out = candidate;
        return true;
      }
    } else if constexpr (dxvk::war3::internal::
                             kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& candidate : snap.records) {
        if (candidate.payload == record.renderablePart &&
            matchesCurrentDrawSlice(candidate)) {
          out = candidate;
          return true;
        }
      }
    }
  }

  if (record.sceneNode != nullptr) {
    const auto sceneIt = snap.bySceneNode.find(record.sceneNode);
    if (sceneIt != snap.bySceneNode.end() &&
        sceneIt->second < snap.records.size()) {
      const auto& candidate = snap.records[sceneIt->second];
      if (matchesCurrentDrawSlice(candidate)) {
        out = candidate;
        return true;
      }
    } else if constexpr (dxvk::war3::internal::
                             kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& candidate : snap.records) {
        if ((candidate.sceneNode == record.sceneNode ||
             candidate.identity.sceneNode == record.sceneNode) &&
            matchesCurrentDrawSlice(candidate)) {
          out = candidate;
          return true;
        }
      }
    }
  }

  m_shadowManifestVisibleLookupMissCount.fetch_add(
      1u, std::memory_order_relaxed);
  return false;
}

bool VisibleRenderableRegistry::queryByWorldObjectEntry(
    void *worldObjectEntry, VisibleRenderableRecord &out) const {
  out = {};
  if (!worldObjectEntry)
    return false;

  const Snapshot &snap = snapshotForThread();
  const auto it = snap.byWorldObjectEntry.find(worldObjectEntry);
  if (it == snap.byWorldObjectEntry.end() || it->second >= snap.records.size()) {
    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& record : snap.records) {
        if (record.identity.worldObjectEntry == worldObjectEntry) {
          out = record;
          return true;
        }
      }
    }
    return false;
  }

  out = snap.records[it->second];
  return true;
}

bool VisibleRenderableRegistry::queryByHandle(uint32_t jHandle,
                                              VisibleRenderableRecord &out) const {
  out = {};
  if (jHandle == 0u)
    return false;

  const Snapshot &snap = snapshotForThread();
  const auto it = snap.byHandle.find(jHandle);
  if (it == snap.byHandle.end() || it->second >= snap.records.size()) {
    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& record : snap.records) {
        if (record.identity.jHandle == jHandle) {
          out = record;
          return true;
        }
      }
    }
    return false;
  }

  out = snap.records[it->second];
  return true;
}

bool VisibleRenderableRegistry::queryBySceneNode(void *sceneNode,
                                                 VisibleRenderableRecord &out) const {
  out = {};
  if (!sceneNode)
    return false;

  const Snapshot &snap = snapshotForThread();
  const auto it = snap.bySceneNode.find(sceneNode);
  if (it == snap.bySceneNode.end() || it->second >= snap.records.size()) {
    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& record : snap.records) {
        if (record.sceneNode == sceneNode ||
            record.identity.sceneNode == sceneNode) {
          out = record;
          return true;
        }
      }
    }
    return false;
  }

  out = snap.records[it->second];
  return true;
}

bool VisibleRenderableRegistry::queryByRuntimeModel(
    void *runtimeModelPtr, VisibleRenderableRecord &out) const {
  out = {};
  if (!runtimeModelPtr)
    return false;

  const Snapshot &snap = snapshotForThread();
  const auto it = snap.byRuntimeModel.find(runtimeModelPtr);
  if (it == snap.byRuntimeModel.end() || it->second >= snap.records.size()) {
    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& record : snap.records) {
        if (record.runtimeModelPtr == runtimeModelPtr) {
          out = record;
          return true;
        }
      }
    }
    return false;
  }

  out = snap.records[it->second];
  return true;
}

void VisibleRenderableRegistry::refreshShadowManifestFromCurrentDraw(
    const std::vector<CurrentDrawContractRecord>& records,
    uint64_t frameNumber) {
  static const std::vector<CurrentDrawContractRecord> s_emptyRecords;
  refreshShadowManifestFromCurrentDraw(records, s_emptyRecords, frameNumber);
}

void VisibleRenderableRegistry::refreshShadowManifestFromCurrentDraw(
    const std::vector<CurrentDrawContractRecord>& firstRecords,
    const std::vector<CurrentDrawContractRecord>& secondRecords,
    uint64_t frameNumber) {
  if (m_shadowManifestMapEpoch == 0u) {
    clearShadowManifest();
    return;
  }
  const uint64_t frame =
      frameNumber != 0u ? frameNumber : m_frameNumber.load(std::memory_order_relaxed);
  ShadowManifestSummary summary = {};
  summary.mapEpoch = m_shadowManifestMapEpoch;
  summary.frameNumber = frame;
  summary.poseFreshGenerationVerifierMismatchCount =
      m_shadowManifestSummary.poseFreshGenerationVerifierMismatchCount;

  // Multi-slice membership is diagnostic-only, but the old pair of local
  // unordered containers allocated buckets and nodes on every manifest
  // refresh. Keep a render-thread scratch of exact anchor/slice pairs instead:
  // sorting and scanning the scalar values preserves the count while making
  // steady-state refresh allocation-free.
  static thread_local std::vector<std::pair<uint64_t, uint64_t>>
      s_sliceKeysByPartAnchor;
  auto& sliceKeysByPartAnchor = s_sliceKeysByPartAnchor;
  sliceKeysByPartAnchor.clear();
  bool hasPoseFreshObject = false;
  const Snapshot& visibleSnapshot = snapshotForThread();
  (void)visibleSnapshot;  // 保留 snapshotForThread() 调用以维持内部快照确认
  const size_t recordCount = firstRecords.size() + secondRecords.size();
  sliceKeysByPartAnchor.reserve(recordCount);
  const auto forEachRecord = [&](const auto& callback) {
    for (const auto& record : firstRecords)
      callback(record);
    for (const auto& record : secondRecords)
      callback(record);
  };

  // Generation 0 is reserved for "never marked".  A refresh receives a new
  // generation even when frameNumber is unchanged, so repeated same-frame
  // populates preserve the old per-call set semantics exactly.  On uint32 wrap
  // clear all persistent marks before restarting at 1; this prevents entries
  // stamped during the previous generation epoch from becoming false hits.
  uint32_t refreshGeneration = m_shadowManifestRefreshGeneration + 1u;
  if (refreshGeneration == 0u) {
    for (auto& [_, entry] : m_shadowManifestObjects)
      entry.poseFreshGeneration = 0u;
    refreshGeneration = 1u;
  }
  m_shadowManifestRefreshGeneration = refreshGeneration;

  // Explicitly enabled verifier only: reconstruct the retired temporary set
  // as ground truth.  The default path neither constructs nor allocates it.
  std::unique_ptr<std::unordered_set<uint64_t>> poseFreshVerifierObjects;
  if (War3SemanticShadowManifestPoseGenerationVerifierEnabled()) {
    poseFreshVerifierObjects =
        std::make_unique<std::unordered_set<uint64_t>>();
    poseFreshVerifierObjects->reserve(recordCount);
    forEachRecord([&](const CurrentDrawContractRecord& record) {
      if (record.fromGrace)
        return;
      const uint64_t objectKey = ShadowManifestObjectKey(record);
      if (objectKey != 0u)
        poseFreshVerifierObjects->insert(objectKey);
    });
  }

  // Phase 7.26：runtimeModelPtr 只在 pose restore/pose 诊断开启时才会被后续
  // 读取。refresh 阶段的 ResolveRuntimeModelForCurrentDrawRecord 是
  // O(snapshot.records) 扫描，在 dynamic_shadow_pressure 下占 Populate 主要
  // CPU。默认 release 下跳过这一步，manifest entry 的 runtimeModelPtr
  // 保持 nullptr，并由 cModelPoseNoRuntimeCount 显式反映该状态。
  const bool allowResolveRuntimeModel =
      War3SemanticShadowManifestCModelPoseProbeAllowed();
  auto resolveRuntimeModelSafely =
      [&](const CurrentDrawContractRecord& record) -> void* {
    if (!allowResolveRuntimeModel)
      return nullptr;
    return ResolveRuntimeModelForCurrentDrawRecord(visibleSnapshot, record);
  };

  forEachRecord([&](const CurrentDrawContractRecord& record) {
    // Grace can fill a one-frame producer hole, but it must never become a
    // fresh Manifest observation or extend structure/pose/slice lifetime.
    if (record.fromGrace)
      return;
    const uint64_t objectKey = ShadowManifestObjectKey(record);
    if (objectKey == 0u)
      return;
    hasPoseFreshObject = true;

    auto objectIt = m_shadowManifestObjects.find(objectKey);
    if (objectIt == m_shadowManifestObjects.end()) {
      ShadowManifestObjectEntry entry = {};
      entry.mapEpoch = m_shadowManifestMapEpoch;
      entry.key = objectKey;
      entry.firstSeenFrame = frame;
      entry.lastSeenFrame = frame;
      entry.observedFrameCount = 1u;
      entry.poseFreshGeneration = refreshGeneration;
      objectIt = m_shadowManifestObjects.emplace(objectKey, entry).first;
      ++summary.newObjectCount;
    } else {
      auto& entry = objectIt->second;
      if (entry.mapEpoch != m_shadowManifestMapEpoch) {
        entry = {};
        entry.mapEpoch = m_shadowManifestMapEpoch;
        entry.key = objectKey;
        entry.firstSeenFrame = frame;
      }
      if (entry.lastSeenFrame != frame)
        ++entry.observedFrameCount;
      entry.lastSeenFrame = frame;
      entry.poseFreshGeneration = refreshGeneration;
    }

    const uint64_t partKey = ShadowManifestPartKey(record, objectKey);
    if (partKey == 0u)
      return;

    auto partIt = m_shadowManifestParts.find(partKey);
    if (partIt == m_shadowManifestParts.end()) {
      ShadowManifestPartEntry entry = {};
      entry.mapEpoch = m_shadowManifestMapEpoch;
      entry.key = partKey;
      entry.objectKey = objectKey;
      entry.firstSeenFrame = frame;
      entry.lastSeenFrame = frame;
      entry.lastPoseFrame = frame;
      entry.lastSliceFrame = frame;
      entry.lastGoodPacketFrame = 0u;
      entry.renderablePart = record.renderablePart;
      entry.sceneNode = record.sceneNode;
      entry.runtimeModelPtr = resolveRuntimeModelSafely(record);
      entry.layerIndex = record.layerIndex;
      entry.payloadWord108 = record.payloadWord108;
      entry.lastPayloadWord11C = record.payloadWord11C;
      entry.observedFrameCount = 1u;
      entry.producerStage =
          record.producerStage >= 0 ? record.producerStage : record.stage;
      entry.producerGroup = record.producerGroup;
      entry.sourceKind = record.sourceKind;
      entry.producerFreshThisFrame = record.producerFreshThisFrame;
      entry.visibleFrameSerial = record.visibleFrameSerial;
      entry.stagePolicyRevision = record.stagePolicyRevision;
      entry.fromGrace = record.fromGrace;
      entry.graceAge = record.graceAge;
      entry.alphaPayloadComplete = record.alphaPayloadComplete;
      partIt = m_shadowManifestParts.emplace(partKey, entry).first;
      // Phase 7.31 Iteration G：不维护 objectKey→partKey 反向索引；
      // object entry 的 refresh generation 在统一 part sweep 中传播。
    } else {
      auto& entry = partIt->second;
      if (entry.mapEpoch != m_shadowManifestMapEpoch) {
        entry = {};
        entry.mapEpoch = m_shadowManifestMapEpoch;
        entry.key = partKey;
        entry.objectKey = objectKey;
        entry.firstSeenFrame = frame;
      }
      if (entry.lastSeenFrame != frame)
        ++entry.observedFrameCount;
      if (entry.lastPayloadWord11C != record.payloadWord11C)
        ++summary.payload11CChurnCount;
      if (entry.renderablePart != nullptr && record.renderablePart != nullptr &&
          entry.renderablePart != record.renderablePart) {
        ++summary.renderablePartChurnCount;
        ++entry.renderablePartChurnCount;
      }
      entry.lastSeenFrame = frame;
      entry.lastPoseFrame = frame;
      entry.lastSliceFrame = frame;
      entry.renderablePart = record.renderablePart;
      entry.sceneNode = record.sceneNode;
      if (void* runtimeModelPtr = resolveRuntimeModelSafely(record))
        entry.runtimeModelPtr = runtimeModelPtr;
      entry.layerIndex = record.layerIndex;
      entry.payloadWord108 = record.payloadWord108;
      entry.lastPayloadWord11C = record.payloadWord11C;
      entry.producerStage =
          record.producerStage >= 0 ? record.producerStage : record.stage;
      entry.producerGroup = record.producerGroup;
      entry.sourceKind = record.sourceKind;
      entry.producerFreshThisFrame = record.producerFreshThisFrame;
      entry.visibleFrameSerial = record.visibleFrameSerial;
      entry.stagePolicyRevision = record.stagePolicyRevision;
      entry.fromGrace = record.fromGrace;
      entry.graceAge = record.graceAge;
      entry.alphaPayloadComplete = record.alphaPayloadComplete;
    }

    const uint64_t partAnchorKey =
        ShadowManifestPartAnchorKey(record, objectKey);
    if (partAnchorKey != 0u) {
      const uint64_t sliceKey = ShadowManifestSliceKey(record);
      sliceKeysByPartAnchor.emplace_back(partAnchorKey, sliceKey);
    }
  });

  std::sort(sliceKeysByPartAnchor.begin(), sliceKeysByPartAnchor.end());
  uint64_t multiSlicePartCount = 0u;
  for (size_t begin = 0u; begin < sliceKeysByPartAnchor.size();) {
    size_t end = begin + 1u;
    bool hasDifferentSlice = false;
    while (end < sliceKeysByPartAnchor.size() &&
           sliceKeysByPartAnchor[end].first ==
               sliceKeysByPartAnchor[begin].first) {
      hasDifferentSlice = hasDifferentSlice ||
          sliceKeysByPartAnchor[end].second !=
              sliceKeysByPartAnchor[begin].second;
      ++end;
    }
    multiSlicePartCount += hasDifferentSlice ? 1u : 0u;
    begin = end;
  }

  for (auto it = m_shadowManifestObjects.begin();
       it != m_shadowManifestObjects.end();) {
    const uint64_t lastSeen = it->second.lastSeenFrame;
    if (frame > lastSeen &&
        frame - lastSeen > kShadowManifestStructureTtlFrames) {
      it = m_shadowManifestObjects.erase(it);
      ++summary.expiredObjectCount;
    } else {
      ++summary.objectCount;
      if (it->second.observedFrameCount >= 2u)
        ++summary.stableObjectCount;
      ++it;
    }
  }

  const auto isPoseFreshObject = [&](uint64_t objectKey) {
    const auto objectIt = m_shadowManifestObjects.find(objectKey);
    return objectIt != m_shadowManifestObjects.end() &&
           objectIt->second.poseFreshGeneration == refreshGeneration;
  };

  if (poseFreshVerifierObjects != nullptr) {
    uint64_t refreshMismatchCount = 0u;
    for (const auto& [_, entry] : m_shadowManifestParts) {
      const bool expectedFresh =
          poseFreshVerifierObjects->find(entry.objectKey) !=
          poseFreshVerifierObjects->end();
      if (isPoseFreshObject(entry.objectKey) != expectedFresh)
        ++refreshMismatchCount;
    }
    summary.poseFreshGenerationVerifierMismatchCount += refreshMismatchCount;
    if (refreshMismatchCount != 0u &&
        War3SemanticShadowManifestPoseGenerationVerifierAssertEnabled()) {
      assert(refreshMismatchCount == 0u &&
             "War3 shadow manifest pose generation mismatch");
    }
  }

  for (auto it = m_shadowManifestParts.begin();
       it != m_shadowManifestParts.end();) {
    auto& entry = it->second;

    // Phase 7.31 Iteration G：pose-bearing record 是 object 级 freshness 信号。
    // 在过期及 pose-age 判断前传播给 sibling，并合入已有 part sweep，
    // 避免每帧第二次全表扫描。
    if (hasPoseFreshObject && isPoseFreshObject(entry.objectKey))
      entry.lastPoseFrame = (std::max)(entry.lastPoseFrame, frame);

    const uint64_t lastSeen = entry.lastSeenFrame;
    if (frame > lastSeen &&
        frame - lastSeen > kShadowManifestStructureTtlFrames) {
      it = m_shadowManifestParts.erase(it);
      ++summary.expiredPartCount;
    } else {
      ++summary.partCount;
      if (entry.lastSeenFrame == frame) {
        ++summary.freshPartCount;
      } else {
        const uint64_t packetAge =
            frame > entry.lastGoodPacketFrame
                ? frame - entry.lastGoodPacketFrame
                : 0u;
        if (entry.lastGoodPacketFrame != 0u &&
            packetAge <= kShadowManifestLastGoodPacketTtlFrames)
          ++summary.leaseablePartCount;
      }

      const uint64_t poseAge =
          frame > entry.lastPoseFrame ? frame - entry.lastPoseFrame : 0u;
      if (poseAge > kShadowManifestSkinnedPoseTtlFrames) {
        ++summary.poseStalePartCount;
        if (entry.runtimeModelPtr == nullptr) {
          ++summary.cModelPoseNoRuntimeCount;
        } else if (!War3SemanticShadowManifestCModelPoseProbeAllowed()) {
          // Phase 7.26：默认 release 配置下不做每帧 pose hash，避免对每个 stale
          // part 做一次 12KB 级别的内存扫描。只有显式开启 restore 或诊断才进入。
          ++summary.cModelPoseNoRuntimeCount;
        } else {
          uint32_t matrixCount = 0u;
          uint64_t matrixHash = 0u;
          if (TryProbeLiveCModelPose(entry.runtimeModelPtr, matrixCount,
                                     matrixHash)) {
            ++summary.cModelPoseHitCount;
            summary.cModelPoseLastRuntimeModelPtr =
                uint64_t(reinterpret_cast<uintptr_t>(entry.runtimeModelPtr));
            summary.cModelPoseLastMatrixCount = matrixCount;
            summary.cModelPoseLastMatrixHash = matrixHash;
          } else {
            ++summary.cModelPoseMissCount;
          }
        }
      }

      const uint64_t sliceAge =
          frame > entry.lastSliceFrame ? frame - entry.lastSliceFrame : 0u;
      if (sliceAge > kShadowManifestSliceTtlFrames)
        ++summary.sliceStalePartCount;
      ++it;
    }
  }

  summary.multiSlicePartCount = multiSlicePartCount;
  summary.visibleLookupPartLayerHitCount =
      m_shadowManifestVisibleLookupPartLayerHitCount.load(
          std::memory_order_relaxed);
  summary.visibleLookupSingleFallbackCount =
      m_shadowManifestVisibleLookupSingleFallbackCount.load(
          std::memory_order_relaxed);
  summary.visibleLookupMissCount =
      m_shadowManifestVisibleLookupMissCount.load(std::memory_order_relaxed);
  m_shadowManifestSummary = summary;
}

void VisibleRenderableRegistry::noteShadowManifestPartGoodPacket(
    uint64_t partKey, uint64_t frameNumber) {
  if (partKey == 0u)
    return;

  auto it = m_shadowManifestParts.find(partKey);
  if (it == m_shadowManifestParts.end())
    return;

  const uint64_t frame =
      frameNumber != 0u ? frameNumber : m_frameNumber.load(std::memory_order_relaxed);
  auto& entry = it->second;
  if (entry.mapEpoch == 0u || entry.mapEpoch != m_shadowManifestMapEpoch)
    return;
  entry.lastGoodPacketFrame = frame;
  entry.lastPoseFrame = (std::max)(entry.lastPoseFrame, frame);

  // Phase 7.31 Iteration G：sibling propagation 默认关闭。
  // benchmark 下每帧 20K+ 次调用是 Populate 瓶颈之一；Iter B 关闭 stale
  // restore 后，sibling pose-fresh 保守一点只是让 lease 更不容易命中，
  // 这是可接受的（live pose 本来就 fresh）。
  // 保留 env var 开关便于 A/B 复核。
  static const bool s_siblingPropEnabled =
      []() {
        const char* env = std::getenv(
            "DXVK_WAR3_SEMANTIC_MANIFEST_SIBLING_POSE_PROPAGATION");
        return env != nullptr && env[0] == '1';
      }();
  if (!s_siblingPropEnabled)
    return;
  // Pose/palette evidence is object-wide for War3 skinned geosets. Propagating
  // only the frame number lets sibling parts satisfy pose freshness.
  // 注：此处回退到线性扫描；benchmark 下仍有性能代价，但 env=0 时已早退。
  const uint64_t objectKey = entry.objectKey;
  if (objectKey == 0u)
    return;
  for (auto& [otherKey, otherEntry] : m_shadowManifestParts) {
    if (otherKey == partKey || otherEntry.objectKey != objectKey)
      continue;
    otherEntry.lastPoseFrame = (std::max)(otherEntry.lastPoseFrame, frame);
  }
}

VisibleRenderableRegistry::ShadowManifestPartLeaseInfo
VisibleRenderableRegistry::queryShadowManifestPartLeaseInfo(
    uint64_t partKey, uint64_t frameNumber) const {
  ShadowManifestPartLeaseInfo info = {};
  info.partKey = partKey;
  if (partKey == 0u)
    return info;

  const auto it = m_shadowManifestParts.find(partKey);
  if (it == m_shadowManifestParts.end())
    return info;

  const uint64_t frame =
      frameNumber != 0u ? frameNumber : m_frameNumber.load(std::memory_order_relaxed);
  const auto& entry = it->second;
  if (entry.mapEpoch == 0u ||
      entry.mapEpoch != m_shadowManifestMapEpoch)
    return info;
  info.mapEpoch = entry.mapEpoch;
  info.found = true;
  info.objectKey = entry.objectKey;
  info.lastSeenFrame = entry.lastSeenFrame;
  info.lastPoseFrame = entry.lastPoseFrame;
  info.lastSliceFrame = entry.lastSliceFrame;
  info.lastGoodPacketFrame = entry.lastGoodPacketFrame;
  info.runtimeModelPtr = entry.runtimeModelPtr;
  info.observedFrameCount = entry.observedFrameCount;
  info.producerStage = entry.producerStage;
  info.producerGroup = entry.producerGroup;
  info.sourceKind = entry.sourceKind;
  info.producerFreshThisFrame = entry.producerFreshThisFrame;
  info.visibleFrameSerial = entry.visibleFrameSerial;
  info.stagePolicyRevision = entry.stagePolicyRevision;
  info.fromGrace = entry.fromGrace;
  info.graceAge = entry.graceAge;
  info.alphaPayloadComplete = entry.alphaPayloadComplete;
  info.poseAgeFrames =
      frame > entry.lastPoseFrame ? frame - entry.lastPoseFrame : 0u;
  info.sliceAgeFrames =
      frame > entry.lastSliceFrame ? frame - entry.lastSliceFrame : 0u;
  info.packetAgeFrames =
      entry.lastGoodPacketFrame != 0u && frame > entry.lastGoodPacketFrame
          ? frame - entry.lastGoodPacketFrame
          : 0u;
  const uint64_t structureAge =
      frame > entry.lastSeenFrame ? frame - entry.lastSeenFrame : 0u;
  info.structureLive = structureAge <= kShadowManifestStructureTtlFrames;
  info.poseFresh = info.poseAgeFrames <= kShadowManifestSkinnedPoseTtlFrames;
  info.sliceFresh = info.sliceAgeFrames <= kShadowManifestSliceTtlFrames;
  info.packetFresh =
      entry.lastGoodPacketFrame != 0u &&
      info.packetAgeFrames <= kShadowManifestLastGoodPacketTtlFrames;
  if (!info.poseFresh && entry.runtimeModelPtr != nullptr &&
      War3SemanticShadowManifestCModelPoseProbeAllowed()) {
    uint32_t matrixCount = 0u;
    uint64_t matrixHash = 0u;
    if (TryProbeLiveCModelPose(entry.runtimeModelPtr, matrixCount,
                               matrixHash)) {
      info.cModelPoseFresh = true;
      info.cModelPoseMatrixCount = matrixCount;
      info.cModelPoseMatrixHash = matrixHash;
    }
  }
  info.leaseable =
      info.structureLive && info.poseFresh && info.sliceFresh && info.packetFresh;
  return info;
}

VisibleRenderableRegistry::ShadowManifestRetireResult
VisibleRenderableRegistry::retireShadowManifest(
    const ShadowCasterTombstone& tombstone) {
  ShadowManifestRetireResult result = {};
  std::array<uint64_t, 4u> objectKeys = {};
  size_t objectKeyCount = 0u;
  const auto appendObjectKey =
      [&](const CurrentDrawContractRecord& record) {
        const uint64_t key = computeShadowManifestObjectKey(record);
        if (key == 0u)
          return;
        for (size_t i = 0u; i < objectKeyCount; ++i) {
          if (objectKeys[i] == key)
            return;
        }
        objectKeys[objectKeyCount++] = key;
      };

  CurrentDrawContractRecord identityRecord = {};
  if (tombstone.identity.jHandle != 0u) {
    identityRecord.jHandle = tombstone.identity.jHandle;
    appendObjectKey(identityRecord);
    identityRecord = {};
  }
  if (tombstone.identity.unitPtr != nullptr) {
    identityRecord.unitPtr = tombstone.identity.unitPtr;
    appendObjectKey(identityRecord);
    identityRecord = {};
  }
  if (tombstone.identity.worldObjectEntry != nullptr) {
    identityRecord.worldObjectEntry =
        tombstone.identity.worldObjectEntry;
    appendObjectKey(identityRecord);
    identityRecord = {};
  }
  if (tombstone.identity.sceneNode != nullptr) {
    identityRecord.sceneNode = tombstone.identity.sceneNode;
    appendObjectKey(identityRecord);
  }

  const auto matchesObjectKey = [&](uint64_t key) {
    return std::find(
               objectKeys.begin(),
               objectKeys.begin() + objectKeyCount,
               key) != objectKeys.begin() + objectKeyCount;
  };

  for (auto it = m_shadowManifestObjects.begin();
       it != m_shadowManifestObjects.end();) {
    if (matchesObjectKey(it->first)) {
      it = m_shadowManifestObjects.erase(it);
      result.objectCount++;
    } else {
      ++it;
    }
  }
  for (auto it = m_shadowManifestParts.begin();
       it != m_shadowManifestParts.end();) {
    const ShadowManifestPartEntry& part = it->second;
    const bool stageMatch =
        tombstone.reason == ShadowCasterTombstoneReason::StageDisabled &&
        tombstone.identity.producerStage >= 0 &&
        part.producerStage == tombstone.identity.producerStage;
    const bool directPointerMatch =
        (tombstone.identity.renderablePart != nullptr &&
         part.renderablePart == tombstone.identity.renderablePart) ||
        (tombstone.identity.sceneNode != nullptr &&
         part.sceneNode == tombstone.identity.sceneNode);
    if (stageMatch || matchesObjectKey(part.objectKey) ||
        directPointerMatch) {
      it = m_shadowManifestParts.erase(it);
      result.partCount++;
    } else {
      ++it;
    }
  }
  if (result.partCount != 0u) {
    std::unordered_set<uint64_t> liveObjectKeys;
    liveObjectKeys.reserve(m_shadowManifestParts.size());
    for (const auto& [_, part] : m_shadowManifestParts)
      liveObjectKeys.insert(part.objectKey);
    for (auto it = m_shadowManifestObjects.begin();
         it != m_shadowManifestObjects.end();) {
      if (liveObjectKeys.find(it->first) == liveObjectKeys.end()) {
        it = m_shadowManifestObjects.erase(it);
        result.objectCount++;
      } else {
        ++it;
      }
    }
  }
  return result;
}

VisibleRenderableRegistry::ShadowManifestRetireResult
VisibleRenderableRegistry::clearShadowManifest() {
  ShadowManifestRetireResult result = {};
  result.objectCount = m_shadowManifestObjects.size();
  result.partCount = m_shadowManifestParts.size();
  m_shadowManifestObjects.clear();
  m_shadowManifestParts.clear();
  m_shadowManifestSummary = {};
  return result;
}

void VisibleRenderableRegistry::resetShadowManifestMapEpoch(
    uint64_t mapEpoch) {
  if (mapEpoch == 0u || mapEpoch == m_shadowManifestMapEpoch)
    return;
  clearShadowManifest();
  m_shadowManifestMapEpoch = mapEpoch;
  m_shadowManifestSummary.mapEpoch = mapEpoch;
  m_shadowManifestRefreshGeneration = 0u;
}

VisibleRenderableRegistry::ShadowManifestSummary
VisibleRenderableRegistry::queryShadowManifestSummary() const {
  ShadowManifestSummary summary = m_shadowManifestSummary;
  summary.visibleLookupPartLayerHitCount =
      m_shadowManifestVisibleLookupPartLayerHitCount.load(
          std::memory_order_relaxed);
  summary.visibleLookupSingleFallbackCount =
      m_shadowManifestVisibleLookupSingleFallbackCount.load(
          std::memory_order_relaxed);
  summary.visibleLookupMissCount =
      m_shadowManifestVisibleLookupMissCount.load(std::memory_order_relaxed);
  return summary;
}

const std::vector<VisibleRenderableRecord>&
VisibleRenderableRegistry::getAllVisibleView() const {
  return snapshotForThread().records;
}

std::vector<VisibleRenderableRecord> VisibleRenderableRegistry::getAllVisible() const {
  return getAllVisibleView();
}

size_t VisibleRenderableRegistry::getVisibleCount() const {
  return snapshotForThread().records.size();
}

size_t VisibleRenderableRegistry::getMainQueueCount() const {
  return snapshotForThread().mainQueueCount;
}

size_t VisibleRenderableRegistry::getTransparentCount() const {
  return snapshotForThread().transparentCount;
}

VisibleRenderableRegistry::DebugSummary
VisibleRenderableRegistry::queryDebugSummary() const {
  const Snapshot& snap = snapshotForThread();
  DebugSummary summary = {};
  summary.frameNumber = m_frameNumber.load(std::memory_order_relaxed);
  summary.visibleCount = snap.records.size();
  summary.mainQueueCount = snap.mainQueueCount;
  summary.transparentCount = snap.transparentCount;
  summary.mainQueueRangeCallCount = snap.mainQueueRangeCallCount;
  summary.mainQueueRangeRecordCount = snap.mainQueueRangeRecordCount;
  summary.semanticCandidateCallCount = snap.semanticCandidateCallCount;
  summary.semanticCandidateMergedCount = snap.semanticCandidateMergedCount;
  summary.semanticCandidateAppendedCount = snap.semanticCandidateAppendedCount;
  summary.semanticMergeFallbackCallCount =
      snap.semanticMergeFallbackCallCount;
  summary.semanticMergeIndexLookupCount =
      snap.semanticMergeIndexLookupCount;
  summary.semanticMergeIndexHitCount = snap.semanticMergeIndexHitCount;
  summary.semanticMergeIndexCandidateVisitCount =
      snap.semanticMergeIndexCandidateVisitCount;
  summary.semanticMergeLegacyScanCallCount =
      snap.semanticMergeLegacyScanCallCount;
  summary.semanticMergeLegacyScanRecordVisitCount =
      snap.semanticMergeLegacyScanRecordVisitCount;
  summary.semanticMergeVerifierCallCount =
      snap.semanticMergeVerifierCallCount;
  summary.semanticMergeVerifierLegacyScanRecordVisitCount =
      snap.semanticMergeVerifierLegacyScanRecordVisitCount;
  summary.semanticMergeVerifierMismatchCount =
      snap.semanticMergeVerifierMismatchCount;
  summary.semanticMergeVerifierSelectionMismatchCount =
      snap.semanticMergeVerifierSelectionMismatchCount;
  summary.semanticMergeVerifierAuxIndexCheckCount =
      snap.semanticMergeVerifierAuxIndexCheckCount;
  summary.semanticMergeVerifierAuxIndexMismatchCount =
      snap.semanticMergeVerifierAuxIndexMismatchCount;
  summary.semanticMergeIndexEntryCount = snap.semanticMergeByPointer.size();
  summary.transparentEntryCallCount = snap.transparentEntryCallCount;
  return summary;
}

} // namespace dxvk::war3::render
