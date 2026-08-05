// war3_model_registry.cpp - War3 濡€崇€风挧鍕爱/鐎圭偘绶?婵寧鈧浇绻嶇悰灞炬妤犮劍鐏︾€圭偟骞?

#include "war3_model_registry.h"

#include "../core/war3_internal_test_config.h"
#include "../game/war3_unit.h"
#include "../core/war3_memory.h"
#include "../render/war3_render_objects.h"
#include "../../util/util_env.h"

#include <cassert>
#include <unordered_set>

namespace dxvk {
namespace war3 {
namespace model {

namespace {
uint64_t Fnv1a64(const void *data, size_t size,
                 uint64_t seed = 1469598103934665603ull) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  uint64_t hash = seed;
  for (size_t i = 0; i < size; ++i) {
    hash ^= uint64_t(bytes[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

uint64_t CombineHash(uint64_t a, uint64_t b) {
  uint64_t x = a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2));
  return x ? x : 0xA5A5A5A5A5A5A5A5ull;
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
      SafeReadPtrFast(candidate, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
                      ownedHandlePtr) &&
      ownedHandlePtr != nullptr;
  const bool hasRuntimeGeosets =
      SafeReadU32Fast(candidate, dxvk::war3::CModelOffsets::RuntimeGeosetCount,
                      runtimeGeosetCount) &&
      runtimeGeosetCount < 4096u &&
      SafeReadPtrFast(candidate, dxvk::war3::CModelOffsets::RuntimeGeosets,
                      runtimeGeosets) &&
      runtimeGeosets != nullptr;
  const bool hasFinalPoseArray =
      SafeReadU32Fast(candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
                      finalPoseMatrixCount) &&
      finalPoseMatrixCount <= 256u &&
      SafeReadPtrFast(candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
                      finalPoseMatrixArray) &&
      finalPoseMatrixArray != nullptr;
  return hasOwnedHandle || hasRuntimeGeosets || hasFinalPoseArray;
}

bool IsObviouslyInvalidRuntimeModelForInstance(void* runtimeModelPtr,
                                               void* worldObjectEntry,
                                               void* sceneNode,
                                               void* spritePtr) {
  if (runtimeModelPtr == nullptr)
    return true;
  return runtimeModelPtr == worldObjectEntry || runtimeModelPtr == sceneNode ||
         runtimeModelPtr == spritePtr;
}

void* TryReadRuntimeModelFromSprite(void* spritePtr) {
  void* runtimeModelPtr = nullptr;
  if (!spritePtr)
    return nullptr;
  if (!SafeReadPtrFast(spritePtr, 0x20, runtimeModelPtr) ||
      !LooksLikeRuntimeModelPtr(runtimeModelPtr)) {
    return nullptr;
  }
  return runtimeModelPtr;
}

bool HasRuntimeOwnerIdentity(const ModelInstanceRecord& record) {
  return record.worldObjectEntry != nullptr || record.sceneNode != nullptr ||
         record.unitPtr != nullptr || record.jHandle != 0u ||
         record.rawcode != 0u;
}

bool HasRuntimeSourceObject(const ModelInstanceRecord& record) {
  return record.sourceObjectPtr != nullptr ||
         record.sourceSpriteObjectPtr != nullptr;
}

struct ModelInstanceTrackingClassification {
  bool runtimeBound = false;
  bool completeIdentity = false;
};

ModelInstanceTrackingClassification ClassifyModelInstanceTracking(
    const ModelInstanceRecord& record) noexcept {
  ModelInstanceTrackingClassification classification = {};
  classification.runtimeBound =
      record.runtimeModelPtr != nullptr && record.spritePtr != nullptr;
  classification.completeIdentity =
      classification.runtimeBound &&
      (record.sceneNode != nullptr || record.unitPtr != nullptr ||
       record.jHandle != 0u || record.rawcode != 0u);
  return classification;
}

struct PoseTrackingClassification {
  bool readyPose = false;
  bool spriteFramePose = false;
  bool matrixPalette = false;
};

PoseTrackingClassification ClassifyPoseTracking(
    const PoseRecord& record) noexcept {
  PoseTrackingClassification classification = {};
  classification.spriteFramePose =
      record.lastSpriteFramePoseFrame != 0u;
  classification.matrixPalette =
      record.lastMatrixPaletteFrame != 0u && record.matrixCount != 0u &&
      !record.matrixPalette.empty();
  classification.readyPose =
      classification.spriteFramePose || record.lastRootPoseFrame != 0u ||
      classification.matrixPalette;
  return classification;
}

void ReplaceTrackingClassificationCount(uint64_t& count, bool oldValue,
                                        bool newValue) noexcept {
  if (oldValue == newValue)
    return;
  if (oldValue) {
    assert(count != 0u);
    if (count != 0u)
      --count;
    return;
  }
  ++count;
}

bool RegistryTrackingHealthVerifierEnabled() {
  static const bool enabled =
      env::getEnvVar("DXVK_WAR3_REGISTRY_HEALTH_VERIFY") == "1" ||
      env::getEnvVar("DXVK_WAR3_REGISTRY_HEALTH_VERIFY_ASSERT") == "1";
  return enabled;
}

bool RegistryTrackingHealthVerifierAssertEnabled() {
  static const bool enabled =
      env::getEnvVar("DXVK_WAR3_REGISTRY_HEALTH_VERIFY_ASSERT") == "1";
  return enabled;
}

void MaybeAssertRegistryTrackingHealth(uint32_t mismatchMask) {
  if (mismatchMask == 0u ||
      !RegistryTrackingHealthVerifierAssertEnabled()) {
    return;
  }
  assert(mismatchMask == 0u &&
         "War3 registry tracking-health aggregate mismatch");
}

bool HasSameRuntimeOwnerIdentity(const ModelInstanceRecord& a,
                                 const ModelInstanceRecord& b) {
  return a.worldObjectEntry == b.worldObjectEntry &&
         a.sceneNode == b.sceneNode && a.unitPtr == b.unitPtr &&
         a.spritePtr == b.spritePtr && a.jHandle == b.jHandle &&
         a.rawcode == b.rawcode;
}

bool HasSameModelInstancePayloadExact(const ModelInstanceRecord& a,
                                      const ModelInstanceRecord& b) {
  return a.worldObjectEntry == b.worldObjectEntry &&
         a.sceneNode == b.sceneNode && a.unitPtr == b.unitPtr &&
         a.spritePtr == b.spritePtr &&
         a.runtimeModelPtr == b.runtimeModelPtr &&
         a.sourceObjectPtr == b.sourceObjectPtr &&
         a.sourceSpriteObjectPtr == b.sourceSpriteObjectPtr &&
         a.runtimeCreatorModelDataPtr == b.runtimeCreatorModelDataPtr &&
         a.runtimeCreatorHandlePtr == b.runtimeCreatorHandlePtr &&
         a.modelResourcePtr == b.modelResourcePtr &&
         a.jHandle == b.jHandle && a.rawcode == b.rawcode &&
         a.runtimeCreatorCallerRva == b.runtimeCreatorCallerRva &&
         a.runtimeResolveCallerRva == b.runtimeResolveCallerRva &&
         a.modelKey == b.modelKey &&
         a.firstSeenFrame == b.firstSeenFrame;
}

bool HasExactRuntimeOwnerProof(const ModelInstanceRecord& owner,
                               const ModelInstanceRecord& source) {
  return owner.runtimeModelPtr == source.runtimeModelPtr &&
         HasSameRuntimeOwnerIdentity(owner, source) &&
         owner.modelKey == source.modelKey &&
         owner.modelResourcePtr == source.modelResourcePtr &&
         owner.lastSceneCollectorBatchFrame ==
             source.lastSceneCollectorBatchFrame;
}

void CollectRuntimeModelTreeForOwner(void* rootRuntimeModelPtr,
                                     std::vector<void*>& out) {
  out.clear();
  if (!LooksLikeRuntimeModelPtr(rootRuntimeModelPtr))
    return;

  std::vector<void*> pending;
  pending.reserve(32u);
  pending.push_back(rootRuntimeModelPtr);

  std::unordered_set<void*> visitedRuntimeModels;
  visitedRuntimeModels.reserve(64u);
  std::unordered_set<void*> visitedLinkNodes;
  visitedLinkNodes.reserve(128u);

  size_t cursor = 0u;
  constexpr size_t kMaxRuntimeModels = 256u;
  constexpr size_t kMaxLinkNodes = 1024u;
  while (cursor < pending.size() && pending.size() <= kMaxRuntimeModels) {
    void* currentRuntimeModel = pending[cursor++];
    if (currentRuntimeModel == nullptr ||
        !visitedRuntimeModels.insert(currentRuntimeModel).second) {
      continue;
    }
    out.push_back(currentRuntimeModel);

    uint32_t childGroupCount = 0u;
    void* childGroupArray = nullptr;
    if (!SafeReadU32Fast(currentRuntimeModel,
                         dxvk::war3::CModelOffsets::ChildBucketCount,
                         childGroupCount) ||
        !SafeReadPtrFast(currentRuntimeModel,
                         dxvk::war3::CModelOffsets::ChildBucketArray,
                         childGroupArray) ||
        childGroupCount == 0u || childGroupArray == nullptr ||
        !IsReadableRange(childGroupArray, size_t(childGroupCount) * 12u)) {
      continue;
    }

    const auto* childGroups =
        reinterpret_cast<const uint8_t*>(childGroupArray);
    for (uint32_t i = 0u; i < childGroupCount; ++i) {
      void* linkNode = nullptr;
      SafeReadPtrFast(childGroups + size_t(i) * 12u, 8u, linkNode);
      size_t traversed = 0u;
      while (linkNode != nullptr && traversed < kMaxLinkNodes &&
             visitedLinkNodes.insert(linkNode).second) {
        ++traversed;
        void* childRuntimeModel = nullptr;
        void* nextLinkNode = nullptr;
        SafeReadPtrFast(linkNode, 8u, childRuntimeModel);
        SafeReadPtrFast(linkNode, 4u, nextLinkNode);
        if (LooksLikeRuntimeModelPtr(childRuntimeModel))
          pending.push_back(childRuntimeModel);
        linkNode = nextLinkNode;
      }
    }
  }
}

void MergeModelResourceRecord(ModelResourceRecord &dst,
                              const ModelResourceRecord &src) {
  if (src.spritePtr)
    dst.spritePtr = src.spritePtr;
  if (src.runtimeModelPtr)
    dst.runtimeModelPtr = src.runtimeModelPtr;
  if (src.modelResourcePtr)
    dst.modelResourcePtr = src.modelResourcePtr;
  if (!src.modelPath.empty())
    dst.modelPath = src.modelPath;
  if (src.modelType != 0)
    dst.modelType = src.modelType;
  if (src.flags != 0)
    dst.flags = src.flags;
  if (src.modelKey != 0)
    dst.modelKey = src.modelKey;
  if (dst.firstSeenFrame == 0 ||
      (src.firstSeenFrame != 0 && src.firstSeenFrame < dst.firstSeenFrame)) {
    dst.firstSeenFrame = src.firstSeenFrame;
  }
  if (src.lastSeenFrame > dst.lastSeenFrame)
    dst.lastSeenFrame = src.lastSeenFrame;
}

void MergeModelInstanceRecord(ModelInstanceRecord &dst,
                              const ModelInstanceRecord &src) {
  if (src.worldObjectEntry)
    dst.worldObjectEntry = src.worldObjectEntry;
  if (src.sceneNode)
    dst.sceneNode = src.sceneNode;
  if (src.unitPtr)
    dst.unitPtr = src.unitPtr;
  if (src.spritePtr)
    dst.spritePtr = src.spritePtr;
  if (src.runtimeModelPtr)
    dst.runtimeModelPtr = src.runtimeModelPtr;
  if (src.sourceObjectPtr)
    dst.sourceObjectPtr = src.sourceObjectPtr;
  if (src.sourceSpriteObjectPtr)
    dst.sourceSpriteObjectPtr = src.sourceSpriteObjectPtr;
  if (src.runtimeCreatorModelDataPtr)
    dst.runtimeCreatorModelDataPtr = src.runtimeCreatorModelDataPtr;
  if (src.runtimeCreatorHandlePtr)
    dst.runtimeCreatorHandlePtr = src.runtimeCreatorHandlePtr;
  if (src.modelResourcePtr)
    dst.modelResourcePtr = src.modelResourcePtr;
  if (src.jHandle != 0)
    dst.jHandle = src.jHandle;
  if (src.rawcode != 0)
    dst.rawcode = src.rawcode;
  if (src.runtimeCreatorCallerRva != 0u)
    dst.runtimeCreatorCallerRva = src.runtimeCreatorCallerRva;
  if (src.runtimeResolveCallerRva != 0u)
    dst.runtimeResolveCallerRva = src.runtimeResolveCallerRva;
  if (src.modelKey != 0)
    dst.modelKey = src.modelKey;
  if (dst.firstSeenFrame == 0 ||
      (src.firstSeenFrame != 0 && src.firstSeenFrame < dst.firstSeenFrame)) {
    dst.firstSeenFrame = src.firstSeenFrame;
  }
  if (src.lastSeenFrame > dst.lastSeenFrame)
    dst.lastSeenFrame = src.lastSeenFrame;
  if (src.lastSceneCollectorBatchFrame >
      dst.lastSceneCollectorBatchFrame) {
    dst.lastSceneCollectorBatchFrame = src.lastSceneCollectorBatchFrame;
  }
}

void MergePoseRecord(PoseRecord &dst, const PoseRecord &src) {
  if (src.runtimeModelPtr)
    dst.runtimeModelPtr = src.runtimeModelPtr;
  if (src.sceneNode)
    dst.sceneNode = src.sceneNode;
  if (src.unitPtr)
    dst.unitPtr = src.unitPtr;
  if (src.spritePtr)
    dst.spritePtr = src.spritePtr;
  if (src.sequenceId != 0)
    dst.sequenceId = src.sequenceId;
  if (src.sequenceTime != 0.0f)
    dst.sequenceTime = src.sequenceTime;
  if (src.scale != 1.0f || dst.scale == 0.0f)
    dst.scale = src.scale;
  if (src.yaw != 0.0f)
    dst.yaw = src.yaw;
  if (src.pitch != 0.0f)
    dst.pitch = src.pitch;
  if (src.roll != 0.0f)
    dst.roll = src.roll;
  if (src.height != 0.0f)
    dst.height = src.height;
  if (src.hasWorldTransform) {
    dst.hasWorldTransform = true;
    dst.worldTransform = src.worldTransform;
  }
  if (src.hasSpriteFrameTransform) {
    dst.hasSpriteFrameTransform = true;
    dst.spriteFrameTransform = src.spriteFrameTransform;
  }
  if (src.spriteFrameDt != 0.0f)
    dst.spriteFrameDt = src.spriteFrameDt;
  bool replacedMatrixPalette = false;
  if (src.matrixCount != 0) {
    const bool shouldReplacePalette =
        dst.matrixCount == 0 ||
        src.lastMatrixPaletteFrame > dst.lastMatrixPaletteFrame ||
        (src.lastMatrixPaletteFrame == dst.lastMatrixPaletteFrame &&
         src.matrixCount >= dst.matrixCount);
    if (shouldReplacePalette) {
      dst.matrixCount = src.matrixCount;
      dst.matrixHash = src.matrixHash;
      dst.matrixPalette = src.matrixPalette;
      replacedMatrixPalette = true;
    }
  }
  if (src.lastRootPoseFrame != 0)
    dst.lastRootPoseFrame = src.lastRootPoseFrame;
  if (src.lastSpriteFramePoseFrame != 0)
    dst.lastSpriteFramePoseFrame = src.lastSpriteFramePoseFrame;
  if (src.lastMatrixPaletteFrame != 0 &&
      (src.matrixCount == 0 || replacedMatrixPalette))
    dst.lastMatrixPaletteFrame = src.lastMatrixPaletteFrame;
  if (src.spriteFrameSampleCount != 0)
    dst.spriteFrameSampleCount = src.spriteFrameSampleCount;
  if (dst.firstSeenFrame == 0 ||
      (src.firstSeenFrame != 0 && src.firstSeenFrame < dst.firstSeenFrame)) {
    dst.firstSeenFrame = src.firstSeenFrame;
  }
  if (src.lastSeenFrame > dst.lastSeenFrame)
    dst.lastSeenFrame = src.lastSeenFrame;
}

void MergeAttachmentRigidRecord(AttachmentRigidRecord& dst,
                                const AttachmentRigidRecord& src) {
  if (src.rootRuntimeModelPtr)
    dst.rootRuntimeModelPtr = src.rootRuntimeModelPtr;
  if (src.ownerRuntimeModelPtr)
    dst.ownerRuntimeModelPtr = src.ownerRuntimeModelPtr;
  if (src.childRuntimeModelPtr)
    dst.childRuntimeModelPtr = src.childRuntimeModelPtr;
  if (src.childSpritePtr)
    dst.childSpritePtr = src.childSpritePtr;
  if (src.sourceObjectPtr)
    dst.sourceObjectPtr = src.sourceObjectPtr;
  if (src.sourceSpriteObjectPtr)
    dst.sourceSpriteObjectPtr = src.sourceSpriteObjectPtr;
  if (src.worldObjectEntry)
    dst.worldObjectEntry = src.worldObjectEntry;
  if (src.sceneNode)
    dst.sceneNode = src.sceneNode;
  if (src.unitPtr)
    dst.unitPtr = src.unitPtr;
  if (src.jHandle != 0u)
    dst.jHandle = src.jHandle;
  if (src.rawcode != 0u)
    dst.rawcode = src.rawcode;
  dst.slotIndex = src.slotIndex;
  dst.sourceRecordIndex = src.sourceRecordIndex;
  dst.childTag = src.childTag;
  dst.localPointX = src.localPointX;
  dst.localPointY = src.localPointY;
  dst.localPointZ = src.localPointZ;
  if (dst.firstSeenFrame == 0 ||
      (src.firstSeenFrame != 0 && src.firstSeenFrame < dst.firstSeenFrame)) {
    dst.firstSeenFrame = src.firstSeenFrame;
  }
  if (src.lastSeenFrame > dst.lastSeenFrame)
    dst.lastSeenFrame = src.lastSeenFrame;
}

class RegistryMutationGenerationGuard {
public:
  explicit RegistryMutationGenerationGuard(
      std::atomic<uint64_t>& generation) noexcept
  : m_generation(generation) {
    // Odd means a writer is active. All registry writers already serialize on
    // their unique_lock, so this is a single-writer seqlock.
    m_generation.fetch_add(1u, std::memory_order_acq_rel);
  }

  ~RegistryMutationGenerationGuard() {
    // Publish every map write before making the next even generation visible.
    m_generation.fetch_add(1u, std::memory_order_release);
  }

  RegistryMutationGenerationGuard(
      const RegistryMutationGenerationGuard&) = delete;
  RegistryMutationGenerationGuard& operator=(
      const RegistryMutationGenerationGuard&) = delete;

private:
  std::atomic<uint64_t>& m_generation;
};
} // namespace

ModelRegistry &ModelRegistry::instance() {
  static ModelRegistry s_instance;
  return s_instance;
}

uint64_t ModelRegistry::HashModelKey(const std::string &path, uint32_t modelType,
                                     uint32_t flags) {
  uint64_t hash = Fnv1a64(path.data(), path.size());
  hash = CombineHash(hash, uint64_t(modelType));
  hash = CombineHash(hash, uint64_t(flags));
  return hash;
}

uint64_t ModelRegistry::HashRuntimeModelKey(void *modelResourcePtr,
                                            void *runtimeModelPtr,
                                            uint32_t modelType,
                                            uint32_t flags) {
  uint64_t hash = 1469598103934665603ull;
  hash = CombineHash(hash, reinterpret_cast<uintptr_t>(modelResourcePtr));
  hash = CombineHash(hash, reinterpret_cast<uintptr_t>(runtimeModelPtr));
  hash = CombineHash(hash, uint64_t(modelType));
  hash = CombineHash(hash, uint64_t(flags));
  return hash;
}

void ModelRegistry::beginFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_frameNumber.fetch_add(1u, std::memory_order_relaxed);
}

void ModelRegistry::endFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticRegistryEndFrameSweeps)
    return;
  for (auto &it : m_bySprite)
    it.second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  for (auto &it : m_byRuntimeModel)
    it.second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  for (auto &it : m_byPath)
    it.second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
}

void ModelRegistry::recordSpriteModelPath(void *spritePtr, const char *modelPath,
                                          uint32_t modelType, uint32_t flags) {
  if (!spritePtr || !modelPath || !modelPath[0])
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  ModelResourceRecord record = {};
  auto it = m_bySprite.find(spritePtr);
  if (it != m_bySprite.end())
    record = it->second;
  record.spritePtr = spritePtr;
  record.modelPath = modelPath;
  record.modelType = modelType;
  record.flags = flags;
  record.modelKey =
      record.modelResourcePtr != nullptr || record.runtimeModelPtr != nullptr
          ? HashRuntimeModelKey(record.modelResourcePtr, record.runtimeModelPtr,
                                record.modelType, record.flags)
          : HashModelKey(record.modelPath, record.modelType, record.flags);
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  m_bySprite[spritePtr] = record;
  if (record.runtimeModelPtr)
    m_byRuntimeModel[record.runtimeModelPtr] = record;
  m_byPath[record.modelPath] = record;
}

void ModelRegistry::recordRuntimeModelBinding(void *spritePtr,
                                              void *runtimeModelPtr,
                                              void *modelResourcePtr,
                                              uint32_t modelType,
                                              uint32_t flags) {
  if (!spritePtr || !runtimeModelPtr)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  ModelResourceRecord record = {};
  auto itSprite = m_bySprite.find(spritePtr);
  if (itSprite != m_bySprite.end())
    MergeModelResourceRecord(record, itSprite->second);
  auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
  if (itRuntime != m_byRuntimeModel.end())
    MergeModelResourceRecord(record, itRuntime->second);

  record.spritePtr = spritePtr;
  record.runtimeModelPtr = runtimeModelPtr;
  if (modelResourcePtr)
    record.modelResourcePtr = modelResourcePtr;
  if (modelType != 0)
    record.modelType = modelType;
  if (flags != 0)
    record.flags = flags;
  record.modelKey = HashRuntimeModelKey(record.modelResourcePtr,
                                        record.runtimeModelPtr,
                                        record.modelType, record.flags);
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);

  m_bySprite[spritePtr] = record;
  m_byRuntimeModel[runtimeModelPtr] = record;
  if (!record.modelPath.empty())
    m_byPath[record.modelPath] = record;
}

bool ModelRegistry::findBySprite(void *spritePtr, ModelResourceRecord &out) const {
  if (!spritePtr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_bySprite.find(spritePtr);
  if (it == m_bySprite.end())
    return false;
  out = it->second;
  return true;
}

bool ModelRegistry::findByRuntimeModel(void *runtimeModelPtr,
                                       ModelResourceRecord &out) const {
  if (!runtimeModelPtr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end())
    return false;
  out = it->second;
  return true;
}

bool ModelRegistry::findByPath(const std::string &modelPath,
                               ModelResourceRecord &out) const {
  if (modelPath.empty())
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byPath.find(modelPath);
  if (it == m_byPath.end())
    return false;
  out = it->second;
  return true;
}

std::vector<ModelResourceRecord> ModelRegistry::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::vector<ModelResourceRecord> out;
  out.reserve(m_bySprite.size());
  for (const auto &it : m_bySprite)
    out.push_back(it.second);
  return out;
}

size_t ModelRegistry::recordCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_byRuntimeModel.size();
}

uint64_t ModelRegistry::frameNumber() const {
  // Phase 7.83锛歮_frameNumber 宸叉敼 atomic锛屼笉鍐嶉渶瑕侀攣銆?
  return m_frameNumber.load(std::memory_order_relaxed);
}

ModelInstanceRegistry &ModelInstanceRegistry::instance() {
  static ModelInstanceRegistry s_instance;
  return s_instance;
}

void ModelInstanceRegistry::beginFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  RegistryMutationGenerationGuard mutation(m_mutationGeneration);
  m_frameNumber.fetch_add(1u, std::memory_order_relaxed);
}

void ModelInstanceRegistry::endFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticRegistryEndFrameSweeps)
    return;
  RegistryMutationGenerationGuard mutation(m_mutationGeneration);
  for (auto &it : m_byWorldObjectEntry)
    it.second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  for (auto &it : m_bySceneNode)
    it.second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  for (auto &it : m_byUnitPtr)
    it.second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  for (auto &it : m_bySpritePtr)
    it.second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  for (auto &it : m_byRuntimeModel)
    it.second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  for (auto &it : m_runtimeOwnerByRuntimeModel)
    it.second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  for (auto& it : m_bySourceObject)
    it.second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  for (auto& it : m_bySourceSpriteObject)
    it.second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  for (auto &it : m_byHandle)
    it.second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
}

void ModelInstanceRegistry::storeRuntimeModelRecordLocked(
    const ModelInstanceRecord& record) {
  assert(record.runtimeModelPtr != nullptr);
  if (record.runtimeModelPtr == nullptr)
    return;

  auto it = m_byRuntimeModel.find(record.runtimeModelPtr);
  if (it == m_byRuntimeModel.end()) {
    const auto inserted =
        m_byRuntimeModel.emplace(record.runtimeModelPtr, record);
    const auto current =
        ClassifyModelInstanceTracking(inserted.first->second);
    ReplaceTrackingClassificationCount(
        m_trackingRuntimeBoundCount, false, current.runtimeBound);
    ReplaceTrackingClassificationCount(
        m_trackingCompleteIdentityCount, false,
        current.completeIdentity);
    return;
  }

  const auto previous = ClassifyModelInstanceTracking(it->second);
  it->second = record;
  const auto current = ClassifyModelInstanceTracking(it->second);
  ReplaceTrackingClassificationCount(
      m_trackingRuntimeBoundCount, previous.runtimeBound,
      current.runtimeBound);
  ReplaceTrackingClassificationCount(
      m_trackingCompleteIdentityCount, previous.completeIdentity,
      current.completeIdentity);
}

void ModelInstanceRegistry::storeRecord(const ModelInstanceRecord &record) {
  ModelInstanceRecord merged = {};
  if (record.worldObjectEntry) {
    auto it = m_byWorldObjectEntry.find(record.worldObjectEntry);
    if (it != m_byWorldObjectEntry.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  if (record.sceneNode) {
    auto it = m_bySceneNode.find(record.sceneNode);
    if (it != m_bySceneNode.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  if (record.unitPtr) {
    auto it = m_byUnitPtr.find(record.unitPtr);
    if (it != m_byUnitPtr.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  if (record.spritePtr) {
    auto it = m_bySpritePtr.find(record.spritePtr);
    if (it != m_bySpritePtr.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  if (record.runtimeModelPtr) {
    auto it = m_byRuntimeModel.find(record.runtimeModelPtr);
    if (it != m_byRuntimeModel.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  if (record.sourceObjectPtr) {
    auto it = m_bySourceObject.find(record.sourceObjectPtr);
    if (it != m_bySourceObject.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  if (record.sourceSpriteObjectPtr) {
    auto it = m_bySourceSpriteObject.find(record.sourceSpriteObjectPtr);
    if (it != m_bySourceSpriteObject.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  if (record.jHandle != 0) {
    auto it = m_byHandle.find(record.jHandle);
    if (it != m_byHandle.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  MergeModelInstanceRecord(merged, record);

  if (!merged.worldObjectEntry && !merged.sceneNode && !merged.unitPtr &&
      !merged.spritePtr && !merged.runtimeModelPtr &&
      !merged.sourceObjectPtr && !merged.sourceSpriteObjectPtr &&
      merged.jHandle == 0u) {
    return;
  }

  // Publish invalidation before touching any lookup-visible map.  A cache hit
  // that observed the previous generation is linearized before this mutation;
  // every later lookup will take the locked miss path.
  RegistryMutationGenerationGuard mutation(m_mutationGeneration);
  if (merged.worldObjectEntry)
    m_byWorldObjectEntry[merged.worldObjectEntry] = merged;
  if (merged.sceneNode)
    m_bySceneNode[merged.sceneNode] = merged;
  if (merged.unitPtr)
    m_byUnitPtr[merged.unitPtr] = merged;
  if (merged.spritePtr)
    m_bySpritePtr[merged.spritePtr] = merged;
  if (merged.runtimeModelPtr)
    storeRuntimeModelRecordLocked(merged);
  if (merged.sourceObjectPtr)
    m_bySourceObject[merged.sourceObjectPtr] = merged;
  if (merged.sourceSpriteObjectPtr)
    m_bySourceSpriteObject[merged.sourceSpriteObjectPtr] = merged;
  if (merged.jHandle != 0)
    m_byHandle[merged.jHandle] = merged;
}

void ModelInstanceRegistry::propagateRuntimeOwnerIdentityLocked(
    void* rootRuntimeModelPtr, const ModelInstanceRecord& ownerRecord) {
  if (!LooksLikeRuntimeModelPtr(rootRuntimeModelPtr) ||
      !HasRuntimeOwnerIdentity(ownerRecord)) {
    return;
  }

  const uint64_t currentFrame =
      m_frameNumber.load(std::memory_order_relaxed);
  const auto itExisting = m_runtimeOwnerByRuntimeModel.find(rootRuntimeModelPtr);
  if (itExisting != m_runtimeOwnerByRuntimeModel.end() &&
      itExisting->second.lastSeenFrame == currentFrame &&
      HasExactRuntimeOwnerProof(itExisting->second, ownerRecord)) {
    return;
  }

  std::vector<void*> runtimeModels;
  CollectRuntimeModelTreeForOwner(rootRuntimeModelPtr, runtimeModels);
  if (runtimeModels.empty())
    runtimeModels.push_back(rootRuntimeModelPtr);

  RegistryMutationGenerationGuard mutation(m_mutationGeneration);
  for (void* runtimeModelPtr : runtimeModels) {
    if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
      continue;

    ModelInstanceRecord merged = {};
    auto itOwner = m_runtimeOwnerByRuntimeModel.find(runtimeModelPtr);
    if (itOwner != m_runtimeOwnerByRuntimeModel.end())
      MergeModelInstanceRecord(merged, itOwner->second);
    auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
    if (itRuntime != m_byRuntimeModel.end())
      MergeModelInstanceRecord(merged, itRuntime->second);
    MergeModelInstanceRecord(merged, ownerRecord);
    merged.runtimeModelPtr = runtimeModelPtr;
    if (merged.firstSeenFrame == 0)
      merged.firstSeenFrame = currentFrame;
    merged.lastSeenFrame = currentFrame;
    m_runtimeOwnerByRuntimeModel[runtimeModelPtr] = merged;
  }
}

void ModelInstanceRegistry::propagateRuntimeSourceObjectLocked(
    void* rootRuntimeModelPtr, const ModelInstanceRecord& sourceRecord) {
  if (!LooksLikeRuntimeModelPtr(rootRuntimeModelPtr) ||
      !HasRuntimeSourceObject(sourceRecord)) {
    return;
  }

  std::vector<void*> runtimeModels;
  CollectRuntimeModelTreeForOwner(rootRuntimeModelPtr, runtimeModels);
  if (runtimeModels.empty())
    runtimeModels.push_back(rootRuntimeModelPtr);

  RegistryMutationGenerationGuard mutation(m_mutationGeneration);
  for (void* runtimeModelPtr : runtimeModels) {
    if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
      continue;

    ModelInstanceRecord merged = {};
    auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
    if (itRuntime != m_byRuntimeModel.end())
      merged = itRuntime->second;

    merged.runtimeModelPtr = runtimeModelPtr;
    if (sourceRecord.sourceObjectPtr != nullptr)
      merged.sourceObjectPtr = sourceRecord.sourceObjectPtr;
    if (sourceRecord.sourceSpriteObjectPtr != nullptr)
      merged.sourceSpriteObjectPtr = sourceRecord.sourceSpriteObjectPtr;
    if (merged.firstSeenFrame == 0)
      merged.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
    merged.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
    storeRuntimeModelRecordLocked(merged);
  }
}

bool ModelInstanceRegistry::trySkipExactSameFrameRenderObjectLocked(
    const render::RenderObjectInfo& info, uint64_t currentFrame) {
  auto& stats = m_sameFrameIdentityDedupStats;
  ++stats.attempts;
  const auto fail = [](uint64_t& counter) {
    ++counter;
    return false;
  };

  // This helper is deliberately specific to the RenderQueue single-feed path.
  // Both keys are supplied by mapSceneNode after SceneCollector ran for the
  // group; incomplete/native-only paths retain the historical merge.
  if (info.worldObjectEntry == nullptr || info.sceneNode == nullptr)
    return fail(stats.missIncomplete);

  const auto itScene = m_bySceneNode.find(info.sceneNode);
  if (itScene == m_bySceneNode.end())
    return fail(stats.missMissingAlias);

  const ModelInstanceRecord& canonical = itScene->second;
  if (canonical.lastSeenFrame != currentFrame)
    return fail(stats.missCrossFrame);
  if (canonical.lastSceneCollectorBatchFrame != currentFrame)
    return fail(stats.missNoBatchProof);

  // Only fields consumed by noteInstanceIdentityLocked participate.  Unknown
  // (null/zero) input cannot erase an already complete canonical record.
  if (canonical.worldObjectEntry != info.worldObjectEntry ||
      canonical.sceneNode != info.sceneNode ||
      // The historical path assigns unitPtr even when the single-feed input
      // is null, which deliberately defeats the owner-tree same-frame
      // shortcut and gives newly attached runtime children one more
      // propagation pass.  Only an exact value may skip that behavior.
      canonical.unitPtr != info.unitPtr ||
      (info.jHandle != 0u && canonical.jHandle != info.jHandle) ||
      (info.rawcode != 0u && canonical.rawcode != info.rawcode)) {
    return fail(stats.missInputMismatch);
  }

  if (canonical.firstSeenFrame == 0u ||
      canonical.worldObjectEntry == nullptr || canonical.sceneNode == nullptr ||
      canonical.unitPtr == nullptr || canonical.spritePtr == nullptr ||
      canonical.runtimeModelPtr == nullptr || canonical.modelKey == 0u ||
      canonical.modelResourcePtr == nullptr) {
    return fail(stats.missIncomplete);
  }

  enum : uint32_t {
    kAliasClean = 0u,
    kAliasMissing = 1u,
    kAliasCrossFrame = 2u,
    kAliasNoBatchProof = 3u,
    kAliasConflict = 4u,
  };
  const auto validateAlias = [&](const auto& map,
                                 const auto& key) -> uint32_t {
    const auto it = map.find(key);
    if (it == map.end())
      return kAliasMissing;
    const ModelInstanceRecord& alias = it->second;
    if (alias.lastSeenFrame != currentFrame)
      return kAliasCrossFrame;
    if (alias.lastSceneCollectorBatchFrame != currentFrame)
      return kAliasNoBatchProof;
    if (!HasSameModelInstancePayloadExact(canonical, alias))
      return kAliasConflict;
    return kAliasClean;
  };
  const auto failAlias = [&](uint32_t result) {
    if (result == kAliasMissing)
      return fail(stats.missMissingAlias);
    if (result == kAliasCrossFrame)
      return fail(stats.missCrossFrame);
    if (result == kAliasNoBatchProof)
      return fail(stats.missNoBatchProof);
    return fail(stats.missAliasConflict);
  };

  uint32_t aliasResult =
      validateAlias(m_byWorldObjectEntry, canonical.worldObjectEntry);
  if (aliasResult != kAliasClean)
    return failAlias(aliasResult);
  aliasResult = validateAlias(m_byUnitPtr, canonical.unitPtr);
  if (aliasResult != kAliasClean)
    return failAlias(aliasResult);
  aliasResult = validateAlias(m_bySpritePtr, canonical.spritePtr);
  if (aliasResult != kAliasClean)
    return failAlias(aliasResult);
  aliasResult = validateAlias(m_byRuntimeModel, canonical.runtimeModelPtr);
  if (aliasResult != kAliasClean)
    return failAlias(aliasResult);
  if (canonical.sourceObjectPtr != nullptr) {
    aliasResult = validateAlias(m_bySourceObject, canonical.sourceObjectPtr);
    if (aliasResult != kAliasClean)
      return failAlias(aliasResult);
  }
  if (canonical.sourceSpriteObjectPtr != nullptr) {
    aliasResult =
        validateAlias(m_bySourceSpriteObject, canonical.sourceSpriteObjectPtr);
    if (aliasResult != kAliasClean)
      return failAlias(aliasResult);
  }
  if (canonical.jHandle != 0u) {
    aliasResult = validateAlias(m_byHandle, canonical.jHandle);
    if (aliasResult != kAliasClean)
      return failAlias(aliasResult);
  }

  const auto itOwner =
      m_runtimeOwnerByRuntimeModel.find(canonical.runtimeModelPtr);
  if (itOwner == m_runtimeOwnerByRuntimeModel.end() ||
      itOwner->second.lastSeenFrame != currentFrame ||
      itOwner->second.lastSceneCollectorBatchFrame != currentFrame ||
      !HasExactRuntimeOwnerProof(itOwner->second, canonical)) {
    return fail(stats.missRuntimeOwner);
  }

  ++stats.hits;
  return true;
}

void ModelInstanceRegistry::noteRenderObject(const render::RenderObjectInfo &info) {
  if (!info.worldObjectEntry && !info.sceneNode && !info.unitPtr &&
      !info.hasValidHandle())
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  const uint64_t currentFrame =
      m_frameNumber.load(std::memory_order_relaxed);
  if (currentFrame != 0u && m_lastSceneCollectorBatchFrame == currentFrame &&
      trySkipExactSameFrameRenderObjectLocked(info, currentFrame)) {
    return;
  }
  noteInstanceIdentityLocked(info.worldObjectEntry, info.sceneNode, info.unitPtr,
                             nullptr, info.jHandle, info.rawcode, false);
}

void ModelInstanceRegistry::noteRenderObjectsBatch(
    const std::vector<const render::RenderObjectInfo *> &infos) {
  if (infos.empty())
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  for (const auto *info : infos) {
    if (info == nullptr)
      continue;
    if (!info->worldObjectEntry && !info->sceneNode && !info->unitPtr &&
        !info->hasValidHandle()) {
      continue;
    }
    noteInstanceIdentityLocked(info->worldObjectEntry, info->sceneNode,
                               info->unitPtr, nullptr, info->jHandle,
                               info->rawcode, true);
  }
}

void ModelInstanceRegistry::noteInstanceIdentity(void *worldObjectEntry,
                                                 void *sceneNode,
                                                 void *unitPtr,
                                                 void *spritePtr,
                                                 uint32_t jHandle,
                                                 uint32_t rawcode) {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  noteInstanceIdentityLocked(worldObjectEntry, sceneNode, unitPtr, spritePtr,
                             jHandle, rawcode, false);
}

void ModelInstanceRegistry::noteInstanceIdentityLocked(void *worldObjectEntry,
                                                       void *sceneNode,
                                                       void *unitPtr,
                                                       void *spritePtr,
                                                       uint32_t jHandle,
                                                       uint32_t rawcode,
                                                       bool sceneCollectorBatch) {
  if (!worldObjectEntry && !sceneNode && !unitPtr && !spritePtr &&
      jHandle == 0u && rawcode == 0u) {
    return;
  }

  const uint64_t currentFrame =
      m_frameNumber.load(std::memory_order_relaxed);
  ModelInstanceRecord record = {};
  if (sceneNode) {
    auto it = m_bySceneNode.find(sceneNode);
    if (it != m_bySceneNode.end())
      MergeModelInstanceRecord(record, it->second);
  }
  if (unitPtr) {
    auto it = m_byUnitPtr.find(unitPtr);
    if (it != m_byUnitPtr.end())
      MergeModelInstanceRecord(record, it->second);
  }
  if (spritePtr) {
    auto it = m_bySpritePtr.find(spritePtr);
    if (it != m_bySpritePtr.end())
      MergeModelInstanceRecord(record, it->second);
  }
  if (jHandle != 0u) {
    auto it = m_byHandle.find(jHandle);
    if (it != m_byHandle.end())
      MergeModelInstanceRecord(record, it->second);
  }

  record.worldObjectEntry = worldObjectEntry;
  record.sceneNode = sceneNode;
  record.unitPtr = unitPtr;
  if (spritePtr)
    record.spritePtr = spritePtr;
  if (record.unitPtr && record.spritePtr == nullptr) {
    game::UnitWrapper unit(record.unitPtr);
    if (unit.IsValid())
      record.spritePtr = unit.GetSprite();
  }
  if (jHandle != 0u)
    record.jHandle = jHandle;
  if (rawcode != 0u)
    record.rawcode = rawcode;
  if (record.spritePtr && record.runtimeModelPtr == nullptr) {
    record.runtimeModelPtr = TryReadRuntimeModelFromSprite(record.spritePtr);
    if (IsObviouslyInvalidRuntimeModelForInstance(
            record.runtimeModelPtr, record.worldObjectEntry, record.sceneNode,
            record.spritePtr)) {
      record.runtimeModelPtr = nullptr;
    }
    if (record.runtimeModelPtr) {
      ModelRegistry::instance().recordRuntimeModelBinding(
          record.spritePtr, record.runtimeModelPtr, nullptr, 0u, 0u);
    }
  }
  if (record.spritePtr &&
      (record.modelKey == 0 || record.modelResourcePtr == nullptr ||
       record.runtimeModelPtr == nullptr)) {
    ModelResourceRecord modelRecord = {};
    if (ModelRegistry::instance().findBySprite(record.spritePtr, modelRecord) ||
        ModelRegistry::instance().findByRuntimeModel(record.runtimeModelPtr,
                                                     modelRecord)) {
      record.modelKey = modelRecord.modelKey;
      if (modelRecord.runtimeModelPtr)
        record.runtimeModelPtr = modelRecord.runtimeModelPtr;
      record.modelResourcePtr = modelRecord.modelResourcePtr;
    }
  }
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = currentFrame;
  record.lastSeenFrame = currentFrame;
  if (sceneCollectorBatch) {
    record.lastSceneCollectorBatchFrame = currentFrame;
    m_lastSceneCollectorBatchFrame = currentFrame;
    ++m_sameFrameIdentityDedupStats.batchMarked;
  }
  storeRecord(record);
  if (record.runtimeModelPtr != nullptr && HasRuntimeOwnerIdentity(record))
    propagateRuntimeOwnerIdentityLocked(record.runtimeModelPtr, record);
}

void ModelInstanceRegistry::bindSpriteToInstance(void *unitPtr, void *spritePtr) {
  if (!unitPtr || !spritePtr)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  ModelInstanceRecord record = {};
  const auto itUnit = m_byUnitPtr.find(unitPtr);
  if (itUnit != m_byUnitPtr.end())
    record = itUnit->second;
  record.unitPtr = unitPtr;
  record.spritePtr = spritePtr;
  record.runtimeModelPtr = TryReadRuntimeModelFromSprite(spritePtr);
  if (IsObviouslyInvalidRuntimeModelForInstance(
          record.runtimeModelPtr, record.worldObjectEntry, record.sceneNode,
          record.spritePtr)) {
    record.runtimeModelPtr = nullptr;
  }
  if (record.runtimeModelPtr) {
    ModelRegistry::instance().recordRuntimeModelBinding(
        spritePtr, record.runtimeModelPtr, nullptr, 0u, 0u);
  }
  ModelResourceRecord modelRecord = {};
  if (ModelRegistry::instance().findBySprite(spritePtr, modelRecord) ||
      ModelRegistry::instance().findByRuntimeModel(record.runtimeModelPtr,
                                                   modelRecord)) {
    record.modelKey = modelRecord.modelKey;
    if (modelRecord.runtimeModelPtr)
      record.runtimeModelPtr = modelRecord.runtimeModelPtr;
    record.modelResourcePtr = modelRecord.modelResourcePtr;
  }
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
  if (record.runtimeModelPtr != nullptr)
    propagateRuntimeOwnerIdentityLocked(record.runtimeModelPtr, record);
}

void ModelInstanceRegistry::bindRuntimeModelToSprite(void *spritePtr,
                                                     void *runtimeModelPtr,
                                                     uint64_t modelKey,
                                                     void *modelResourcePtr,
                                                     bool propagateOwnerIdentity) {
  if (!spritePtr || !runtimeModelPtr)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  ModelInstanceRecord record = {};
  auto itSprite = m_bySpritePtr.find(spritePtr);
  if (itSprite != m_bySpritePtr.end())
    record = itSprite->second;
  auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
  if (itRuntime != m_byRuntimeModel.end())
    MergeModelInstanceRecord(record, itRuntime->second);

  record.spritePtr = spritePtr;
  record.runtimeModelPtr = runtimeModelPtr;
  if (modelKey != 0)
    record.modelKey = modelKey;
  if (modelResourcePtr)
    record.modelResourcePtr = modelResourcePtr;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
  if (propagateOwnerIdentity && record.runtimeModelPtr != nullptr)
    propagateRuntimeOwnerIdentityLocked(record.runtimeModelPtr, record);
}

void ModelInstanceRegistry::noteRuntimeCreationProvenance(void *runtimeModelPtr,
                                                          void *modelDataPtr,
                                                          uint32_t callerRva) {
  if (!runtimeModelPtr)
    return;
  if (modelDataPtr == nullptr && callerRva == 0u)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  ModelInstanceRecord record = {};
  auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
  if (itRuntime != m_byRuntimeModel.end())
    MergeModelInstanceRecord(record, itRuntime->second);

  record.runtimeModelPtr = runtimeModelPtr;
  if (modelDataPtr != nullptr)
    record.runtimeCreatorModelDataPtr = modelDataPtr;
  if (callerRva != 0u)
    record.runtimeCreatorCallerRva = callerRva;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
}

void ModelInstanceRegistry::noteRuntimeResolveProvenance(void *runtimeModelPtr,
                                                         void *creatorHandlePtr,
                                                         uint32_t callerRva) {
  if (!runtimeModelPtr)
    return;
  if (creatorHandlePtr == nullptr && callerRva == 0u)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  ModelInstanceRecord record = {};
  auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
  if (itRuntime != m_byRuntimeModel.end())
    MergeModelInstanceRecord(record, itRuntime->second);

  record.runtimeModelPtr = runtimeModelPtr;
  if (creatorHandlePtr != nullptr)
    record.runtimeCreatorHandlePtr = creatorHandlePtr;
  if (callerRva != 0u)
    record.runtimeResolveCallerRva = callerRva;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
}

void ModelInstanceRegistry::noteRuntimeSourceObject(void *runtimeModelPtr,
                                                    void *sourceObjectPtr,
                                                    void *sourceSpriteObjectPtr,
                                                    void *spritePtr) {
  if (!runtimeModelPtr)
    return;
  if (!sourceObjectPtr && !sourceSpriteObjectPtr)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  ModelInstanceRecord record = {};
  auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
  if (itRuntime != m_byRuntimeModel.end())
    MergeModelInstanceRecord(record, itRuntime->second);
  if (spritePtr) {
    auto itSprite = m_bySpritePtr.find(spritePtr);
    if (itSprite != m_bySpritePtr.end())
      MergeModelInstanceRecord(record, itSprite->second);
  }

  record.runtimeModelPtr = runtimeModelPtr;
  if (spritePtr)
    record.spritePtr = spritePtr;
  if (sourceObjectPtr)
    record.sourceObjectPtr = sourceObjectPtr;
  if (sourceSpriteObjectPtr)
    record.sourceSpriteObjectPtr = sourceSpriteObjectPtr;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
  if (record.runtimeModelPtr != nullptr)
    propagateRuntimeSourceObjectLocked(record.runtimeModelPtr, record);
  if (record.runtimeModelPtr != nullptr && HasRuntimeOwnerIdentity(record))
    propagateRuntimeOwnerIdentityLocked(record.runtimeModelPtr, record);
}

void ModelInstanceRegistry::noteRuntimeOwnerIdentity(void *runtimeModelPtr,
                                                     void *worldObjectEntry,
                                                     void *sceneNode,
                                                     void *unitPtr,
                                                     void *spritePtr,
                                                     uint32_t jHandle,
                                                     uint32_t rawcode) {
  if (!runtimeModelPtr)
    return;
  if (!worldObjectEntry && !sceneNode && !unitPtr && !spritePtr &&
      jHandle == 0u && rawcode == 0u) {
    return;
  }

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  ModelInstanceRecord record = {};
  auto itOwner = m_runtimeOwnerByRuntimeModel.find(runtimeModelPtr);
  if (itOwner != m_runtimeOwnerByRuntimeModel.end())
    MergeModelInstanceRecord(record, itOwner->second);
  auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
  if (itRuntime != m_byRuntimeModel.end())
    MergeModelInstanceRecord(record, itRuntime->second);
  if (worldObjectEntry) {
    auto it = m_byWorldObjectEntry.find(worldObjectEntry);
    if (it != m_byWorldObjectEntry.end())
      MergeModelInstanceRecord(record, it->second);
  }
  if (sceneNode) {
    auto it = m_bySceneNode.find(sceneNode);
    if (it != m_bySceneNode.end())
      MergeModelInstanceRecord(record, it->second);
  }
  if (unitPtr) {
    auto it = m_byUnitPtr.find(unitPtr);
    if (it != m_byUnitPtr.end())
      MergeModelInstanceRecord(record, it->second);
  }
  if (spritePtr) {
    auto it = m_bySpritePtr.find(spritePtr);
    if (it != m_bySpritePtr.end())
      MergeModelInstanceRecord(record, it->second);
  }
  if (jHandle != 0u) {
    auto it = m_byHandle.find(jHandle);
    if (it != m_byHandle.end())
      MergeModelInstanceRecord(record, it->second);
  }

  record.runtimeModelPtr = runtimeModelPtr;
  if (worldObjectEntry)
    record.worldObjectEntry = worldObjectEntry;
  if (sceneNode)
    record.sceneNode = sceneNode;
  if (unitPtr)
    record.unitPtr = unitPtr;
  if (spritePtr)
    record.spritePtr = spritePtr;
  if (jHandle != 0u)
    record.jHandle = jHandle;
  if (rawcode != 0u)
    record.rawcode = rawcode;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);

  storeRecord(record);
  propagateRuntimeOwnerIdentityLocked(runtimeModelPtr, record);
}

void ModelInstanceRegistry::bindModelToInstance(void *sceneNode,
                                                uint64_t modelKey) {
  if (!sceneNode || modelKey == 0)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  RegistryMutationGenerationGuard mutation(m_mutationGeneration);
  ModelInstanceRecord &record = m_bySceneNode[sceneNode];
  record.sceneNode = sceneNode;
  record.modelKey = modelKey;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  if (record.spritePtr)
    m_bySpritePtr[record.spritePtr] = record;
  if (record.runtimeModelPtr)
    storeRuntimeModelRecordLocked(record);
}

bool ModelInstanceRegistry::findByWorldObjectEntry(void *worldObjectEntry,
                                                   ModelInstanceRecord &out) const {
  if (!worldObjectEntry)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byWorldObjectEntry.find(worldObjectEntry);
  if (it == m_byWorldObjectEntry.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findBySceneNode(void *sceneNode,
                                            ModelInstanceRecord &out) const {
  if (!sceneNode)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_bySceneNode.find(sceneNode);
  if (it == m_bySceneNode.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findByUnitPtr(void *unitPtr,
                                          ModelInstanceRecord &out) const {
  if (!unitPtr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byUnitPtr.find(unitPtr);
  if (it == m_byUnitPtr.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findBySpritePtr(void *spritePtr,
                                            ModelInstanceRecord &out) const {
  if (!spritePtr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_bySpritePtr.find(spritePtr);
  if (it == m_bySpritePtr.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findByRuntimeModel(void *runtimeModelPtr,
                                               ModelInstanceRecord &out) const {
  if (!runtimeModelPtr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findBySourceObject(void* sourceObjectPtr,
                                               ModelInstanceRecord& out) const {
  out = {};
  if (!sourceObjectPtr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_bySourceObject.find(sourceObjectPtr);
  if (it == m_bySourceObject.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findBySourceSpriteObject(
    void* sourceSpriteObjectPtr, ModelInstanceRecord& out) const {
  out = {};
  if (!sourceSpriteObjectPtr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_bySourceSpriteObject.find(sourceSpriteObjectPtr);
  if (it == m_bySourceSpriteObject.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findOwnerByRuntimeModel(void *runtimeModelPtr,
                                                    ModelInstanceRecord &out) const {
  out = {};
  if (!runtimeModelPtr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_runtimeOwnerByRuntimeModel.find(runtimeModelPtr);
  if (it == m_runtimeOwnerByRuntimeModel.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findByHandle(uint32_t jHandle,
                                          ModelInstanceRecord &out) const {
  if (jHandle == 0)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byHandle.find(jHandle);
  if (it == m_byHandle.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findFirstForAugment(
    void* worldObjectEntry, void* sceneNode, void* primaryUnitPtr,
    void* secondaryUnitPtr, uint32_t jHandle,
    ModelInstanceRecord& out) const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);

  auto findPointer = [&](const auto& map, void* key) -> bool {
    if (key == nullptr)
      return false;
    const auto it = map.find(key);
    if (it == map.end())
      return false;
    out = it->second;
    return true;
  };

  if (findPointer(m_byWorldObjectEntry, worldObjectEntry) ||
      findPointer(m_bySceneNode, sceneNode) ||
      findPointer(m_byUnitPtr, primaryUnitPtr) ||
      (secondaryUnitPtr != primaryUnitPtr &&
       findPointer(m_byUnitPtr, secondaryUnitPtr))) {
    return true;
  }

  if (jHandle == 0u)
    return false;
  const auto handleIt = m_byHandle.find(jHandle);
  if (handleIt == m_byHandle.end())
    return false;
  out = handleIt->second;
  return true;
}

bool ModelInstanceRegistry::findFirstForAugmentView(
    void* worldObjectEntry, void* sceneNode, void* primaryUnitPtr,
    void* secondaryUnitPtr, uint32_t jHandle,
    ModelInstanceAugmentView& out,
    uint64_t* mutationGenerationOut) const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  if (mutationGenerationOut != nullptr) {
    *mutationGenerationOut =
        m_mutationGeneration.load(std::memory_order_acquire);
  }

  const ModelInstanceRecord* record = nullptr;
  const auto findPointer = [&](const auto& map, const auto& key) -> bool {
    const auto it = map.find(key);
    if (it == map.end())
      return false;
    record = &it->second;
    return true;
  };

  if (worldObjectEntry != nullptr)
    findPointer(m_byWorldObjectEntry, worldObjectEntry);
  if (record == nullptr && sceneNode != nullptr)
    findPointer(m_bySceneNode, sceneNode);
  if (record == nullptr && primaryUnitPtr != nullptr)
    findPointer(m_byUnitPtr, primaryUnitPtr);
  if (record == nullptr && secondaryUnitPtr != nullptr &&
      secondaryUnitPtr != primaryUnitPtr) {
    findPointer(m_byUnitPtr, secondaryUnitPtr);
  }
  if (record == nullptr && jHandle != 0u)
    findPointer(m_byHandle, jHandle);

  if (record == nullptr)
    return false;

  out.worldObjectEntry = record->worldObjectEntry;
  out.sceneNode = record->sceneNode;
  out.runtimeModelPtr = record->runtimeModelPtr;
  out.modelResourcePtr = record->modelResourcePtr;
  out.jHandle = record->jHandle;
  out.rawcode = record->rawcode;
  out.modelKey = record->modelKey;
  return true;
}

std::vector<ModelInstanceRecord> ModelInstanceRegistry::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::vector<ModelInstanceRecord> out;
  out.reserve(m_bySceneNode.size());
  for (const auto &it : m_bySceneNode)
    out.push_back(it.second);
  return out;
}

size_t ModelInstanceRegistry::recordCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_byRuntimeModel.size();
}

size_t ModelInstanceRegistry::runtimeBoundCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return size_t(m_trackingRuntimeBoundCount);
}

size_t ModelInstanceRegistry::runtimeCreationProvenanceCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto &it : m_byRuntimeModel) {
    const auto &record = it.second;
    if (record.runtimeCreatorCallerRva != 0u ||
        record.runtimeCreatorModelDataPtr != nullptr) {
      ++count;
    }
  }
  return count;
}

size_t ModelInstanceRegistry::runtimeResolveProvenanceCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto& it : m_byRuntimeModel) {
    const auto& record = it.second;
    if (record.runtimeCreatorHandlePtr != nullptr ||
        record.runtimeResolveCallerRva != 0u) {
      ++count;
    }
  }
  return count;
}

size_t ModelInstanceRegistry::runtimeSourceObjectCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto &it : m_byRuntimeModel) {
    const auto &record = it.second;
    if (record.sourceObjectPtr != nullptr ||
        record.sourceSpriteObjectPtr != nullptr) {
      ++count;
    }
  }
  return count;
}

size_t ModelInstanceRegistry::runtimeOwnerIdentityCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto &it : m_runtimeOwnerByRuntimeModel) {
    const auto &record = it.second;
    if (record.worldObjectEntry != nullptr || record.sceneNode != nullptr ||
        record.unitPtr != nullptr || record.jHandle != 0u ||
        record.rawcode != 0u) {
      ++count;
    }
  }
  return count;
}

size_t ModelInstanceRegistry::completeIdentityCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return size_t(m_trackingCompleteIdentityCount);
}

ModelInstanceTrackingHealthSnapshot
ModelInstanceRegistry::trackingHealthSnapshot() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return trackingHealthSnapshotLocked(
      RegistryTrackingHealthVerifierEnabled());
}

ModelInstanceTrackingHealthSnapshot
ModelInstanceRegistry::debugVerifyTrackingHealthAggregate() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return trackingHealthSnapshotLocked(true);
}

ModelInstanceTrackingHealthSnapshot
ModelInstanceRegistry::trackingHealthSnapshotLocked(
    bool runVerifier) const {
  ModelInstanceTrackingHealthSnapshot snapshot = {};
  snapshot.frameNumber = m_frameNumber.load(std::memory_order_relaxed);
  snapshot.recordCount = uint64_t(m_byRuntimeModel.size());
  snapshot.runtimeBoundCount = m_trackingRuntimeBoundCount;
  snapshot.completeIdentityCount = m_trackingCompleteIdentityCount;
  snapshot.aggregateReadPasses = 1u;

  if (runVerifier) {
    uint64_t scannedRuntimeBoundCount = 0u;
    uint64_t scannedCompleteIdentityCount = 0u;
    snapshot.verifierScanPasses = 1u;
    for (const auto& it : m_byRuntimeModel) {
      ++snapshot.verifierRecordsScanned;
      const auto classification =
          ClassifyModelInstanceTracking(it.second);
      if (classification.runtimeBound)
        ++scannedRuntimeBoundCount;
      if (classification.completeIdentity)
        ++scannedCompleteIdentityCount;
    }

    if (snapshot.recordCount != snapshot.verifierRecordsScanned) {
      snapshot.verifierMismatchMask |=
          ModelInstanceTrackingHealthMismatchRecordCount;
    }
    if (snapshot.runtimeBoundCount != scannedRuntimeBoundCount) {
      snapshot.verifierMismatchMask |=
          ModelInstanceTrackingHealthMismatchRuntimeBoundCount;
    }
    if (snapshot.completeIdentityCount !=
        scannedCompleteIdentityCount) {
      snapshot.verifierMismatchMask |=
          ModelInstanceTrackingHealthMismatchCompleteIdentityCount;
    }
    snapshot.verifierMismatchCount =
        snapshot.verifierMismatchMask != 0u ? 1u : 0u;
    MaybeAssertRegistryTrackingHealth(snapshot.verifierMismatchMask);
  }
  return snapshot;
}

ModelIdentitySameFrameDedupStats
ModelInstanceRegistry::sameFrameIdentityDedupStats() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_sameFrameIdentityDedupStats;
}

uint64_t ModelInstanceRegistry::frameNumber() const {
  // Phase 7.83锛歮_frameNumber 宸叉敼 atomic锛屼笉鍐嶉渶瑕侀攣銆?
  return m_frameNumber.load(std::memory_order_relaxed);
}

uint64_t ModelInstanceRegistry::mutationGeneration() const {
  return m_mutationGeneration.load(std::memory_order_acquire);
}

PoseRegistry &PoseRegistry::instance() {
  static PoseRegistry s_instance;
  return s_instance;
}

void PoseRegistry::beginFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_frameNumber.fetch_add(1u, std::memory_order_relaxed);
}

void PoseRegistry::endFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
}

void PoseRegistry::storeRuntimeModelRecordLocked(const PoseRecord& record) {
  assert(record.runtimeModelPtr != nullptr);
  if (record.runtimeModelPtr == nullptr)
    return;

  auto it = m_byRuntimeModel.find(record.runtimeModelPtr);
  if (it == m_byRuntimeModel.end()) {
    const auto inserted =
        m_byRuntimeModel.emplace(record.runtimeModelPtr, record);
    const auto current = ClassifyPoseTracking(inserted.first->second);
    ReplaceTrackingClassificationCount(
        m_trackingReadyPoseCount, false, current.readyPose);
    ReplaceTrackingClassificationCount(
        m_trackingSpriteFramePoseCount, false,
        current.spriteFramePose);
    ReplaceTrackingClassificationCount(
        m_trackingMatrixPaletteCount, false, current.matrixPalette);
    return;
  }

  const auto previous = ClassifyPoseTracking(it->second);
  it->second = record;
  const auto current = ClassifyPoseTracking(it->second);
  ReplaceTrackingClassificationCount(
      m_trackingReadyPoseCount, previous.readyPose, current.readyPose);
  ReplaceTrackingClassificationCount(
      m_trackingSpriteFramePoseCount, previous.spriteFramePose,
      current.spriteFramePose);
  ReplaceTrackingClassificationCount(
      m_trackingMatrixPaletteCount, previous.matrixPalette,
      current.matrixPalette);
}

void PoseRegistry::storeRecord(const PoseRecord &record) {
  PoseRecord merged = {};
  if (record.runtimeModelPtr) {
    auto it = m_byRuntimeModel.find(record.runtimeModelPtr);
    if (it != m_byRuntimeModel.end())
      MergePoseRecord(merged, it->second);
  }
  if (record.sceneNode) {
    auto it = m_bySceneNode.find(record.sceneNode);
    if (it != m_bySceneNode.end())
      MergePoseRecord(merged, it->second);
  }
  if (record.unitPtr) {
    auto it = m_byUnitPtr.find(record.unitPtr);
    if (it != m_byUnitPtr.end())
      MergePoseRecord(merged, it->second);
  }
  MergePoseRecord(merged, record);

  if (merged.runtimeModelPtr)
    storeRuntimeModelRecordLocked(merged);
  if (merged.sceneNode)
    m_bySceneNode[merged.sceneNode] = merged;
  if (merged.unitPtr)
    m_byUnitPtr[merged.unitPtr] = merged;
}

void PoseRegistry::recordPose(void *runtimeModelPtr, void *sceneNode,
                              void *unitPtr, uint32_t sequenceId,
                              float sequenceTime, float scale, float yaw,
                              float pitch, float roll, float height,
                              bool hasWorldTransform,
                              const Matrix4 *worldTransform) {
  if (!runtimeModelPtr && !sceneNode && !unitPtr)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  PoseRecord record = {};
  record.runtimeModelPtr = runtimeModelPtr;
  record.sceneNode = sceneNode;
  record.unitPtr = unitPtr;
  record.sequenceId = sequenceId;
  record.sequenceTime = sequenceTime;
  record.scale = scale;
  record.yaw = yaw;
  record.pitch = pitch;
  record.roll = roll;
  record.height = height;
  if (hasWorldTransform && worldTransform != nullptr) {
    record.hasWorldTransform = true;
    record.worldTransform = *worldTransform;
  }
  record.lastRootPoseFrame = m_frameNumber.load(std::memory_order_relaxed);
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
}

void PoseRegistry::recordSpriteFramePose(void *runtimeModelPtr, void *spritePtr,
                                         void *sceneNode, void *unitPtr,
                                         float dt, uint32_t sequenceId,
                                         float sequenceTime, float scale,
                                         float yaw, float pitch, float roll,
                                         float height,
                                         bool hasWorldTransform,
                                         const Matrix4 *worldTransform) {
  if (!runtimeModelPtr && !spritePtr && !sceneNode && !unitPtr)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  PoseRecord record = {};
  record.runtimeModelPtr = runtimeModelPtr;
  record.spritePtr = spritePtr;
  record.sceneNode = sceneNode;
  record.unitPtr = unitPtr;
  record.sequenceId = sequenceId;
  record.sequenceTime = sequenceTime;
  record.scale = scale;
  record.yaw = yaw;
  record.pitch = pitch;
  record.roll = roll;
  record.height = height;
  record.spriteFrameDt = dt;
  if (hasWorldTransform && worldTransform != nullptr) {
    record.hasSpriteFrameTransform = true;
    record.spriteFrameTransform = *worldTransform;
  }
  record.lastSpriteFramePoseFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.spriteFrameSampleCount = 1;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
}

void PoseRegistry::recordMatrixPalette(void* runtimeModelPtr, void* sceneNode,
                                       void* unitPtr, const Matrix4* matrices,
                                       uint32_t matrixCount) {
  if ((!runtimeModelPtr && !sceneNode && !unitPtr) || matrices == nullptr ||
      matrixCount == 0)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  PoseRecord record = {};
  record.runtimeModelPtr = runtimeModelPtr;
  record.sceneNode = sceneNode;
  record.unitPtr = unitPtr;
  record.matrixCount = matrixCount;
  record.matrixPalette.assign(matrices, matrices + matrixCount);
  record.matrixHash = Fnv1a64(record.matrixPalette.data(),
                             record.matrixPalette.size() * sizeof(Matrix4));
  record.lastMatrixPaletteFrame = m_frameNumber.load(std::memory_order_relaxed);
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
}

bool PoseRegistry::findByRuntimeModel(void *runtimeModelPtr,
                                      PoseRecord &out) const {
  if (!runtimeModelPtr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end())
    return false;
  out = it->second;
  return true;
}

bool PoseRegistry::findBySceneNode(void *sceneNode, PoseRecord &out) const {
  if (!sceneNode)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_bySceneNode.find(sceneNode);
  if (it == m_bySceneNode.end())
    return false;
  out = it->second;
  return true;
}

bool PoseRegistry::findByUnitPtr(void *unitPtr, PoseRecord &out) const {
  if (!unitPtr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byUnitPtr.find(unitPtr);
  if (it == m_byUnitPtr.end())
    return false;
  out = it->second;
  return true;
}

// Project the fields the per-draw shadow augment path actually consumes,
// skipping the matrixPalette vector so no heap allocation/copy happens.
static inline void ProjectPoseAugment(const PoseRecord &rec,
                                      PoseAugmentView &out) {
  out.hasWorldTransform = rec.hasWorldTransform;
  out.worldTransform = rec.worldTransform;
  out.hasSpriteFrameTransform = rec.hasSpriteFrameTransform;
  out.spriteFrameTransform = rec.spriteFrameTransform;
  out.scale = rec.scale;
  out.height = rec.height;
  out.matrixCount = rec.matrixCount;
  out.matrixHash = rec.matrixHash;
  out.lastRootPoseFrame = rec.lastRootPoseFrame;
  out.lastSpriteFramePoseFrame = rec.lastSpriteFramePoseFrame;
  out.lastMatrixPaletteFrame = rec.lastMatrixPaletteFrame;
}

bool PoseRegistry::findByRuntimeModelAugment(void *runtimeModelPtr,
                                             PoseAugmentView &out) const {
  if (!runtimeModelPtr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end())
    return false;
  ProjectPoseAugment(it->second, out);
  return true;
}

bool PoseRegistry::findBySceneNodeAugment(void *sceneNode,
                                          PoseAugmentView &out) const {
  if (!sceneNode)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_bySceneNode.find(sceneNode);
  if (it == m_bySceneNode.end())
    return false;
  ProjectPoseAugment(it->second, out);
  return true;
}

bool PoseRegistry::findByUnitPtrAugment(void *unitPtr,
                                        PoseAugmentView &out) const {
  if (!unitPtr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byUnitPtr.find(unitPtr);
  if (it == m_byUnitPtr.end())
    return false;
  ProjectPoseAugment(it->second, out);
  return true;
}

std::vector<PoseRecord> PoseRegistry::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::vector<PoseRecord> out;
  out.reserve(m_byRuntimeModel.size() + m_bySceneNode.size());
  for (const auto& it : m_byRuntimeModel)
    out.push_back(it.second);
  for (const auto& it : m_bySceneNode) {
    if (it.second.runtimeModelPtr == nullptr)
      out.push_back(it.second);
  }
  return out;
}

size_t PoseRegistry::recordCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_byRuntimeModel.size();
}

size_t PoseRegistry::readyPoseCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return size_t(m_trackingReadyPoseCount);
}

size_t PoseRegistry::spriteFramePoseCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return size_t(m_trackingSpriteFramePoseCount);
}

size_t PoseRegistry::matrixPaletteCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return size_t(m_trackingMatrixPaletteCount);
}

PoseTrackingHealthSnapshot PoseRegistry::trackingHealthSnapshot() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return trackingHealthSnapshotLocked(
      RegistryTrackingHealthVerifierEnabled());
}

PoseTrackingHealthSnapshot
PoseRegistry::debugVerifyTrackingHealthAggregate() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return trackingHealthSnapshotLocked(true);
}

PoseTrackingHealthSnapshot PoseRegistry::trackingHealthSnapshotLocked(
    bool runVerifier) const {
  PoseTrackingHealthSnapshot snapshot = {};
  snapshot.frameNumber = m_frameNumber.load(std::memory_order_relaxed);
  snapshot.recordCount = uint64_t(m_byRuntimeModel.size());
  snapshot.readyPoseCount = m_trackingReadyPoseCount;
  snapshot.spriteFramePoseCount = m_trackingSpriteFramePoseCount;
  snapshot.matrixPaletteCount = m_trackingMatrixPaletteCount;
  snapshot.aggregateReadPasses = 1u;

  if (runVerifier) {
    uint64_t scannedReadyPoseCount = 0u;
    uint64_t scannedSpriteFramePoseCount = 0u;
    uint64_t scannedMatrixPaletteCount = 0u;
    snapshot.verifierScanPasses = 1u;
    for (const auto& it : m_byRuntimeModel) {
      ++snapshot.verifierRecordsScanned;
      const auto classification = ClassifyPoseTracking(it.second);
      if (classification.readyPose)
        ++scannedReadyPoseCount;
      if (classification.spriteFramePose)
        ++scannedSpriteFramePoseCount;
      if (classification.matrixPalette)
        ++scannedMatrixPaletteCount;
    }

    if (snapshot.recordCount != snapshot.verifierRecordsScanned) {
      snapshot.verifierMismatchMask |=
          PoseTrackingHealthMismatchRecordCount;
    }
    if (snapshot.readyPoseCount != scannedReadyPoseCount) {
      snapshot.verifierMismatchMask |=
          PoseTrackingHealthMismatchReadyPoseCount;
    }
    if (snapshot.spriteFramePoseCount !=
        scannedSpriteFramePoseCount) {
      snapshot.verifierMismatchMask |=
          PoseTrackingHealthMismatchSpriteFramePoseCount;
    }
    if (snapshot.matrixPaletteCount !=
        scannedMatrixPaletteCount) {
      snapshot.verifierMismatchMask |=
          PoseTrackingHealthMismatchMatrixPaletteCount;
    }
    snapshot.verifierMismatchCount =
        snapshot.verifierMismatchMask != 0u ? 1u : 0u;
    MaybeAssertRegistryTrackingHealth(snapshot.verifierMismatchMask);
  }
  return snapshot;
}

uint64_t PoseRegistry::frameNumber() const {
  // Phase 7.83锛歮_frameNumber 宸叉敼 atomic锛屼笉鍐嶉渶瑕侀攣銆?
  return m_frameNumber.load(std::memory_order_relaxed);
}

AttachmentRigidRegistry& AttachmentRigidRegistry::instance() {
  static AttachmentRigidRegistry s_instance;
  return s_instance;
}

void AttachmentRigidRegistry::beginFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_frameNumber.fetch_add(1u, std::memory_order_relaxed);
}

void AttachmentRigidRegistry::endFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
}

void AttachmentRigidRegistry::storeRecord(
    const AttachmentRigidRecord& record) {
  AttachmentRigidRecord merged = {};
  if (record.childRuntimeModelPtr != nullptr) {
    auto it = m_byChildRuntimeModel.find(record.childRuntimeModelPtr);
    if (it != m_byChildRuntimeModel.end())
      MergeAttachmentRigidRecord(merged, it->second);
  }
  if (record.ownerRuntimeModelPtr != nullptr) {
    auto it = m_byOwnerRuntimeModel.find(record.ownerRuntimeModelPtr);
    if (it != m_byOwnerRuntimeModel.end())
      MergeAttachmentRigidRecord(merged, it->second);
  }
  if (record.rootRuntimeModelPtr != nullptr) {
    auto it = m_byRootRuntimeModel.find(record.rootRuntimeModelPtr);
    if (it != m_byRootRuntimeModel.end())
      MergeAttachmentRigidRecord(merged, it->second);
  }
  if (record.worldObjectEntry != nullptr) {
    auto it = m_byWorldObjectEntry.find(record.worldObjectEntry);
    if (it != m_byWorldObjectEntry.end())
      MergeAttachmentRigidRecord(merged, it->second);
  }
  if (record.sceneNode != nullptr) {
    auto it = m_bySceneNode.find(record.sceneNode);
    if (it != m_bySceneNode.end())
      MergeAttachmentRigidRecord(merged, it->second);
  }
  if (record.unitPtr != nullptr) {
    auto it = m_byUnitPtr.find(record.unitPtr);
    if (it != m_byUnitPtr.end())
      MergeAttachmentRigidRecord(merged, it->second);
  }
  MergeAttachmentRigidRecord(merged, record);

  if (merged.childRuntimeModelPtr != nullptr)
    m_byChildRuntimeModel[merged.childRuntimeModelPtr] = merged;
  if (merged.ownerRuntimeModelPtr != nullptr)
    m_byOwnerRuntimeModel[merged.ownerRuntimeModelPtr] = merged;
  if (merged.rootRuntimeModelPtr != nullptr)
    m_byRootRuntimeModel[merged.rootRuntimeModelPtr] = merged;
  if (merged.worldObjectEntry != nullptr)
    m_byWorldObjectEntry[merged.worldObjectEntry] = merged;
  if (merged.sceneNode != nullptr)
    m_bySceneNode[merged.sceneNode] = merged;
  if (merged.unitPtr != nullptr)
    m_byUnitPtr[merged.unitPtr] = merged;
  if (merged.jHandle != 0u)
    m_byHandle[merged.jHandle] = merged;
}

void AttachmentRigidRegistry::noteAttachmentRigid(
    void* rootRuntimeModelPtr, void* ownerRuntimeModelPtr,
    void* childRuntimeModelPtr, void* childSpritePtr, void* worldObjectEntry,
    void* sceneNode, void* unitPtr, void* sourceObjectPtr,
    void* sourceSpriteObjectPtr, uint32_t jHandle, uint32_t rawcode,
    uint32_t slotIndex,
    uint32_t sourceRecordIndex, uint32_t childTag, float localPointX,
    float localPointY, float localPointZ) {
  if (rootRuntimeModelPtr == nullptr || childRuntimeModelPtr == nullptr)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  AttachmentRigidRecord record = {};
  record.rootRuntimeModelPtr = rootRuntimeModelPtr;
  record.ownerRuntimeModelPtr = ownerRuntimeModelPtr;
  record.childRuntimeModelPtr = childRuntimeModelPtr;
  record.childSpritePtr = childSpritePtr;
  record.sourceObjectPtr = sourceObjectPtr;
  record.sourceSpriteObjectPtr = sourceSpriteObjectPtr;
  record.worldObjectEntry = worldObjectEntry;
  record.sceneNode = sceneNode;
  record.unitPtr = unitPtr;
  record.jHandle = jHandle;
  record.rawcode = rawcode;
  record.slotIndex = slotIndex;
  record.sourceRecordIndex = sourceRecordIndex;
  record.childTag = childTag;
  record.localPointX = localPointX;
  record.localPointY = localPointY;
  record.localPointZ = localPointZ;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
}

void AttachmentRigidRegistry::noteRuntimeIdentity(
    void* runtimeModelPtr, void* worldObjectEntry, void* sceneNode,
    void* unitPtr, void* sourceObjectPtr, void* sourceSpriteObjectPtr,
    uint32_t jHandle, uint32_t rawcode) {
  if (runtimeModelPtr == nullptr)
    return;
  if (worldObjectEntry == nullptr && sceneNode == nullptr && unitPtr == nullptr &&
      sourceObjectPtr == nullptr && sourceSpriteObjectPtr == nullptr &&
      jHandle == 0u && rawcode == 0u) {
    return;
  }

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  AttachmentRigidRecord record = {};
  auto itChild = m_byChildRuntimeModel.find(runtimeModelPtr);
  if (itChild != m_byChildRuntimeModel.end())
    MergeAttachmentRigidRecord(record, itChild->second);
  auto itOwner = m_byOwnerRuntimeModel.find(runtimeModelPtr);
  if (itOwner != m_byOwnerRuntimeModel.end())
    MergeAttachmentRigidRecord(record, itOwner->second);
  auto itRoot = m_byRootRuntimeModel.find(runtimeModelPtr);
  if (itRoot != m_byRootRuntimeModel.end())
    MergeAttachmentRigidRecord(record, itRoot->second);

  if (record.childRuntimeModelPtr == nullptr &&
      record.ownerRuntimeModelPtr == nullptr &&
      record.rootRuntimeModelPtr == nullptr) {
    return;
  }

  if (worldObjectEntry != nullptr)
    record.worldObjectEntry = worldObjectEntry;
  if (sceneNode != nullptr)
    record.sceneNode = sceneNode;
  if (unitPtr != nullptr)
    record.unitPtr = unitPtr;
  if (sourceObjectPtr != nullptr)
    record.sourceObjectPtr = sourceObjectPtr;
  if (sourceSpriteObjectPtr != nullptr)
    record.sourceSpriteObjectPtr = sourceSpriteObjectPtr;
  if (jHandle != 0u)
    record.jHandle = jHandle;
  if (rawcode != 0u)
    record.rawcode = rawcode;
  if (record.firstSeenFrame == 0u)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
}

bool AttachmentRigidRegistry::promoteAttachmentChildRuntime(
    void* parentRuntimeModelPtr, void* childRuntimeModelPtr,
    void* childSpritePtr, AttachmentRigidRecord* outRecord,
    void** outPreviousChildRuntimeModelPtr) {
  if (parentRuntimeModelPtr == nullptr || childRuntimeModelPtr == nullptr)
    return false;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  AttachmentRigidRecord record = {};
  bool found = false;
  auto itOwner = m_byOwnerRuntimeModel.find(parentRuntimeModelPtr);
  if (itOwner != m_byOwnerRuntimeModel.end()) {
    MergeAttachmentRigidRecord(record, itOwner->second);
    found = true;
  }
  auto itRoot = m_byRootRuntimeModel.find(parentRuntimeModelPtr);
  if (itRoot != m_byRootRuntimeModel.end()) {
    MergeAttachmentRigidRecord(record, itRoot->second);
    found = true;
  }
  if (!found)
    return false;

  void* previousChildRuntimeModelPtr = record.childRuntimeModelPtr;
  if (outPreviousChildRuntimeModelPtr != nullptr)
    *outPreviousChildRuntimeModelPtr = previousChildRuntimeModelPtr;
  record.childRuntimeModelPtr = childRuntimeModelPtr;
  if (childSpritePtr != nullptr)
    record.childSpritePtr = childSpritePtr;
  if (record.firstSeenFrame == 0u)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);

  if (previousChildRuntimeModelPtr != nullptr &&
      previousChildRuntimeModelPtr != childRuntimeModelPtr) {
    m_byChildRuntimeModel.erase(previousChildRuntimeModelPtr);
  }

  storeRecord(record);
  if (outRecord != nullptr)
    *outRecord = record;
  return true;
}

bool AttachmentRigidRegistry::findByChildRuntimeModel(
    void* childRuntimeModelPtr, AttachmentRigidRecord& out) const {
  out = {};
  if (childRuntimeModelPtr == nullptr)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byChildRuntimeModel.find(childRuntimeModelPtr);
  if (it == m_byChildRuntimeModel.end())
    return false;
  out = it->second;
  return true;
}

bool AttachmentRigidRegistry::findByOwnerRuntimeModel(
    void* ownerRuntimeModelPtr, AttachmentRigidRecord& out) const {
  out = {};
  if (ownerRuntimeModelPtr == nullptr)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byOwnerRuntimeModel.find(ownerRuntimeModelPtr);
  if (it == m_byOwnerRuntimeModel.end())
    return false;
  out = it->second;
  return true;
}

bool AttachmentRigidRegistry::findByRootRuntimeModel(
    void* rootRuntimeModelPtr, AttachmentRigidRecord& out) const {
  out = {};
  if (rootRuntimeModelPtr == nullptr)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byRootRuntimeModel.find(rootRuntimeModelPtr);
  if (it == m_byRootRuntimeModel.end())
    return false;
  out = it->second;
  return true;
}

bool AttachmentRigidRegistry::findByAnyRuntimeModel(
    void* runtimeModelPtr, AttachmentRigidRecord& out) const {
  out = {};
  if (runtimeModelPtr == nullptr)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto itChild = m_byChildRuntimeModel.find(runtimeModelPtr);
  if (itChild != m_byChildRuntimeModel.end()) {
    out = itChild->second;
    return true;
  }
  auto itOwner = m_byOwnerRuntimeModel.find(runtimeModelPtr);
  if (itOwner != m_byOwnerRuntimeModel.end()) {
    out = itOwner->second;
    return true;
  }
  auto itRoot = m_byRootRuntimeModel.find(runtimeModelPtr);
  if (itRoot != m_byRootRuntimeModel.end()) {
    out = itRoot->second;
    return true;
  }
  return false;
}

bool AttachmentRigidRegistry::findByWorldObjectEntry(
    void* worldObjectEntry, AttachmentRigidRecord& out) const {
  out = {};
  if (worldObjectEntry == nullptr)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byWorldObjectEntry.find(worldObjectEntry);
  if (it == m_byWorldObjectEntry.end())
    return false;
  out = it->second;
  return true;
}

bool AttachmentRigidRegistry::findBySceneNode(void* sceneNode,
                                              AttachmentRigidRecord& out) const {
  out = {};
  if (sceneNode == nullptr)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_bySceneNode.find(sceneNode);
  if (it == m_bySceneNode.end())
    return false;
  out = it->second;
  return true;
}

bool AttachmentRigidRegistry::findByUnitPtr(void* unitPtr,
                                            AttachmentRigidRecord& out) const {
  out = {};
  if (unitPtr == nullptr)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byUnitPtr.find(unitPtr);
  if (it == m_byUnitPtr.end())
    return false;
  out = it->second;
  return true;
}

bool AttachmentRigidRegistry::findByHandle(uint32_t jHandle,
                                           AttachmentRigidRecord& out) const {
  out = {};
  if (jHandle == 0u)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto it = m_byHandle.find(jHandle);
  if (it == m_byHandle.end())
    return false;
  out = it->second;
  return true;
}

std::vector<AttachmentRigidRecord> AttachmentRigidRegistry::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::vector<AttachmentRigidRecord> out;
  out.reserve(m_byChildRuntimeModel.size() + m_bySceneNode.size());
  for (const auto& it : m_byChildRuntimeModel)
    out.push_back(it.second);
  for (const auto& it : m_bySceneNode) {
    if (it.second.childRuntimeModelPtr == nullptr)
      out.push_back(it.second);
  }
  return out;
}

size_t AttachmentRigidRegistry::recordCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_byChildRuntimeModel.size();
}

uint64_t AttachmentRigidRegistry::frameNumber() const {
  // Phase 7.83锛歮_frameNumber 宸叉敼 atomic锛屼笉鍐嶉渶瑕侀攣銆?
  return m_frameNumber.load(std::memory_order_relaxed);
}

} // namespace model
} // namespace war3
} // namespace dxvk
