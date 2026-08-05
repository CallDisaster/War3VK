#include "war3_shadow_alpha_test_payload.h"

#include <algorithm>
#include <cstring>

namespace dxvk {
namespace war3 {
namespace shadow {

War3ShadowAlphaTestPayloadCounters g_war3ShadowAlphaTestPayloadCounters = {};

namespace {

inline uint64_t FoldMetadataHash(uint64_t hash, uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
  return hash;
}

inline bool MatchesQuery(const War3ShadowDrawMetadataKey& key,
                         const War3ShadowDrawMetadataQuery& query,
                         bool includeMaterial) {
  return key.instanceIdentity == query.instanceIdentity &&
         key.sceneNode == query.sceneNode &&
         key.renderablePart == query.renderablePart &&
         key.meshPayloadPtr == query.meshPayloadPtr &&
         key.worldObjectEntry == query.worldObjectEntry &&
         key.unitPtr == query.unitPtr && key.jHandle == query.jHandle &&
         key.layerIndex == query.layerIndex &&
         key.producerStage == query.producerStage &&
         key.payloadWord108 == query.payloadWord108 &&
         key.payloadWord11C == query.payloadWord11C &&
         (!includeMaterial ||
          key.materialSignatureHash == query.materialSignatureHash);
}

inline bool MetadataTextureGenerationMatches(
    const War3ShadowDrawMetadata& metadata) {
  return metadata.alpha.diffuseTexture != nullptr &&
         metadata.key.textureIdentity == metadata.alpha.diffuseTexture.ptr() &&
         metadata.key.textureGeneration != 0u &&
         metadata.key.textureGeneration ==
             War3ShadowTextureGeneration(metadata.alpha.diffuseTexture);
}

} // namespace

bool War3ShadowDrawMetadataKey::operator==(
    const War3ShadowDrawMetadataKey& other) const noexcept {
  return instanceIdentity == other.instanceIdentity &&
         sceneNode == other.sceneNode &&
         renderablePart == other.renderablePart &&
         meshPayloadPtr == other.meshPayloadPtr &&
         worldObjectEntry == other.worldObjectEntry && unitPtr == other.unitPtr &&
         jHandle == other.jHandle && layerIndex == other.layerIndex &&
         producerStage == other.producerStage &&
         payloadWord108 == other.payloadWord108 &&
         payloadWord11C == other.payloadWord11C &&
         materialSignatureHash == other.materialSignatureHash &&
         textureIdentity == other.textureIdentity &&
         textureGeneration == other.textureGeneration;
}

uint64_t War3ShadowDrawMetadataKey::stableHash() const noexcept {
  uint64_t hash = 0xcbf29ce484222325ull;
  hash = FoldMetadataHash(hash, reinterpret_cast<uintptr_t>(instanceIdentity));
  hash = FoldMetadataHash(hash, reinterpret_cast<uintptr_t>(sceneNode));
  hash = FoldMetadataHash(hash, reinterpret_cast<uintptr_t>(renderablePart));
  hash = FoldMetadataHash(hash, reinterpret_cast<uintptr_t>(meshPayloadPtr));
  hash = FoldMetadataHash(hash, reinterpret_cast<uintptr_t>(worldObjectEntry));
  hash = FoldMetadataHash(hash, reinterpret_cast<uintptr_t>(unitPtr));
  hash = FoldMetadataHash(hash, jHandle);
  hash = FoldMetadataHash(hash, layerIndex);
  hash = FoldMetadataHash(hash, uint16_t(producerStage));
  hash = FoldMetadataHash(hash, payloadWord108);
  hash = FoldMetadataHash(hash, payloadWord11C);
  hash = FoldMetadataHash(hash, materialSignatureHash);
  hash = FoldMetadataHash(hash, reinterpret_cast<uintptr_t>(textureIdentity));
  hash = FoldMetadataHash(hash, textureGeneration);
  return hash != 0u ? hash : 1u;
}

uint64_t War3ShadowTextureGeneration(const Rc<DxvkImageView>& view) {
  if (view == nullptr)
    return 0u;
  const DxvkDescriptor* descriptor = view->getDescriptor();
  if (descriptor == nullptr)
    return 0u;
  uint64_t bits = 0u;
  const auto handle = descriptor->legacy.image.imageView;
  static_assert(sizeof(handle) <= sizeof(bits));
  std::memcpy(&bits, &handle, sizeof(handle));
  return bits;
}

War3ShadowDrawMetadataFrameStore& War3ShadowDrawMetadataStore() {
  static War3ShadowDrawMetadataFrameStore store;
  return store;
}

void War3ShadowDrawMetadataFrameStore::publish(
    War3ShadowDrawMetadata metadata) {
  if (metadata.frameSerial == 0u)
    return;
  std::lock_guard<std::mutex> lock(m_mutex);
  FrameSlot& slot = m_slots[metadata.frameSerial % m_slots.size()];
  if (slot.frameSerial != metadata.frameSerial) {
    g_war3ShadowAlphaTestPayloadCounters.cacheEvictedCount.fetch_add(
        slot.records.size(), std::memory_order_relaxed);
    slot.records.clear();
    slot.frameSerial = metadata.frameSerial;
    slot.pageGeneration = metadata.frameSerial;
    if (slot.records.capacity() < 256u)
      slot.records.reserve(256u);
  }
  metadata.uvPageGeneration = slot.pageGeneration;
  metadata.alpha.frameSerial = metadata.frameSerial;
  metadata.alpha.uvPageGeneration = slot.pageGeneration;

  auto existing = std::find_if(
      slot.records.begin(), slot.records.end(),
      [&](const War3ShadowDrawMetadata& item) {
        return item.key == metadata.key;
      });
  if (existing != slot.records.end())
    *existing = std::move(metadata);
  else
    slot.records.push_back(std::move(metadata));

  uint64_t total = 0u;
  for (const FrameSlot& frameSlot : m_slots)
    total += frameSlot.records.size();
  g_war3ShadowAlphaTestPayloadCounters.cacheSizeGauge.store(
      total, std::memory_order_relaxed);
}

bool War3ShadowDrawMetadataFrameStore::lookupAlpha(
    const War3ShadowDrawMetadataQuery& query, uint64_t frameSerial,
    War3ShadowDrawMetadata& out, bool noteRejectedLookup) const {
  if (frameSerial == 0u)
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  const FrameSlot& slot = m_slots[frameSerial % m_slots.size()];
  if (slot.frameSerial != frameSerial) {
    if (!noteRejectedLookup)
      return false;
    g_war3ShadowAlphaTestPayloadCounters.metadataRejectedFrameCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }

  const War3ShadowDrawMetadata* match = nullptr;
  for (const War3ShadowDrawMetadata& item : slot.records) {
    if (!item.alphaPayloadComplete ||
        !MatchesQuery(item.key, query, true)) {
      continue;
    }
    if (item.frameSerial != frameSerial ||
        item.uvPageGeneration != slot.pageGeneration ||
        item.alpha.frameSerial != frameSerial ||
        item.alpha.uvPageGeneration != slot.pageGeneration ||
        !MetadataTextureGenerationMatches(item)) {
      if (noteRejectedLookup) {
        g_war3ShadowAlphaTestPayloadCounters
            .metadataRejectedGenerationCount.fetch_add(
                1u, std::memory_order_relaxed);
      }
      continue;
    }
    if (match != nullptr && !(match->key == item.key)) {
      g_war3ShadowAlphaTestPayloadCounters.metadataAmbiguousCount.fetch_add(
          1u, std::memory_order_relaxed);
      return false;
    }
    match = &item;
  }
  if (match == nullptr)
    return false;
  out = *match;
  return true;
}

bool War3ShadowDrawMetadataFrameStore::lookupBlocker(
    const War3ShadowDrawMetadataQuery& query, uint64_t frameSerial,
    War3ShadowMetadataBlockerReason& outReason,
    uint64_t* outKeyHash) const {
  outReason = War3ShadowMetadataBlockerReason::None;
  if (outKeyHash != nullptr)
    *outKeyHash = 0u;
  if (frameSerial == 0u)
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  const FrameSlot& slot = m_slots[frameSerial % m_slots.size()];
  if (slot.frameSerial != frameSerial)
    return false;
  for (const War3ShadowDrawMetadata& item : slot.records) {
    if (!MatchesQuery(item.key, query, false) || !item.hasBlocker())
      continue;
    outReason = item.blockerReason;
    if (outKeyHash != nullptr)
      *outKeyHash = item.key.stableHash();
    return true;
  }
  return false;
}

uint64_t War3ShadowDrawMetadataFrameStore::retire(
    void* renderablePart, void* sceneNode, void* worldObjectEntry,
    int16_t producerStage) {
  std::lock_guard<std::mutex> lock(m_mutex);
  uint64_t removed = 0u;
  for (FrameSlot& slot : m_slots) {
    const auto oldSize = slot.records.size();
    slot.records.erase(
        std::remove_if(
            slot.records.begin(), slot.records.end(),
            [&](const War3ShadowDrawMetadata& item) {
              const bool stageMatch = producerStage >= 0 &&
                                      item.key.producerStage == producerStage;
              const bool identityMatch =
                  (renderablePart != nullptr &&
                   item.key.renderablePart == renderablePart) ||
                  (sceneNode != nullptr && item.key.sceneNode == sceneNode) ||
                  (worldObjectEntry != nullptr &&
                   item.key.worldObjectEntry == worldObjectEntry);
              return stageMatch || identityMatch;
            }),
        slot.records.end());
    removed += oldSize - slot.records.size();
  }
  return removed;
}

void War3ShadowDrawMetadataFrameStore::clear() {
  std::lock_guard<std::mutex> lock(m_mutex);
  for (FrameSlot& slot : m_slots)
    slot = {};
  g_war3ShadowAlphaTestPayloadCounters.cacheSizeGauge.store(
      0u, std::memory_order_relaxed);
}

uint64_t War3ShadowDrawMetadataFrameStore::size() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  uint64_t total = 0u;
  for (const FrameSlot& slot : m_slots)
    total += slot.records.size();
  return total;
}

/// @brief 快照全局计数器；调用方不持有锁。
War3ShadowAlphaTestPayloadCountersSnapshot
ReadWar3ShadowAlphaTestPayloadCountersSnapshot() {
  const auto& src = g_war3ShadowAlphaTestPayloadCounters;
  War3ShadowAlphaTestPayloadCountersSnapshot out = {};
  out.attemptCount = src.attemptCount.load(std::memory_order_relaxed);
  out.hitCount = src.hitCount.load(std::memory_order_relaxed);
  out.missNoUvCount = src.missNoUvCount.load(std::memory_order_relaxed);
  out.missNoDiffuseCount =
      src.missNoDiffuseCount.load(std::memory_order_relaxed);
  out.missStageInvalidCount =
      src.missStageInvalidCount.load(std::memory_order_relaxed);
  out.appliedCount = src.appliedCount.load(std::memory_order_relaxed);
  out.fallbackRejectCount =
      src.fallbackRejectCount.load(std::memory_order_relaxed);
  out.stashCapturedCount =
      src.stashCapturedCount.load(std::memory_order_relaxed);
  out.stashSkipNoSemanticKeyCount =
      src.stashSkipNoSemanticKeyCount.load(std::memory_order_relaxed);
  out.stashSkipNoUvCount =
      src.stashSkipNoUvCount.load(std::memory_order_relaxed);
  out.stashSkipNoDiffuseCount =
      src.stashSkipNoDiffuseCount.load(std::memory_order_relaxed);
  out.stashSkipNoUploadCount =
      src.stashSkipNoUploadCount.load(std::memory_order_relaxed);
  out.cacheEvictedCount =
      src.cacheEvictedCount.load(std::memory_order_relaxed);
  out.cacheSizeGauge = src.cacheSizeGauge.load(std::memory_order_relaxed);
  out.metadataClassifiedCount =
      src.metadataClassifiedCount.load(std::memory_order_relaxed);
  out.metadataCapturedCount =
      src.metadataCapturedCount.load(std::memory_order_relaxed);
  out.metadataAppliedCount =
      src.metadataAppliedCount.load(std::memory_order_relaxed);
  out.metadataRejectedFrameCount =
      src.metadataRejectedFrameCount.load(std::memory_order_relaxed);
  out.metadataRejectedGenerationCount =
      src.metadataRejectedGenerationCount.load(std::memory_order_relaxed);
  out.metadataAmbiguousCount =
      src.metadataAmbiguousCount.load(std::memory_order_relaxed);
  out.metadataRejectedNoMaterialCount =
      src.metadataRejectedNoMaterialCount.load(std::memory_order_relaxed);
  out.metadataRejectedOpaqueCount =
      src.metadataRejectedOpaqueCount.load(std::memory_order_relaxed);
  out.metadataRejectedNoUvCount =
      src.metadataRejectedNoUvCount.load(std::memory_order_relaxed);
  out.metadataRejectedNoDiffuseCount =
      src.metadataRejectedNoDiffuseCount.load(std::memory_order_relaxed);
  out.metadataRejectedUploadCount =
      src.metadataRejectedUploadCount.load(std::memory_order_relaxed);
  out.metadataRejectedDuplicateCount =
      src.metadataRejectedDuplicateCount.load(std::memory_order_relaxed);
  out.blockerKnownRawcodeCount =
      src.blockerKnownRawcodeCount.load(std::memory_order_relaxed);
  out.blockerWidgetIdentityCount =
      src.blockerWidgetIdentityCount.load(std::memory_order_relaxed);
  out.blockerSmallFlatCount =
      src.blockerSmallFlatCount.load(std::memory_order_relaxed);
  out.blockerBelowGroundCount =
      src.blockerBelowGroundCount.load(std::memory_order_relaxed);
  out.blockerUnreadableCount =
      src.blockerUnreadableCount.load(std::memory_order_relaxed);
  out.blockerFinalLeakCount =
      src.blockerFinalLeakCount.load(std::memory_order_relaxed);
  return out;
}

} // namespace shadow
} // namespace war3
} // namespace dxvk
