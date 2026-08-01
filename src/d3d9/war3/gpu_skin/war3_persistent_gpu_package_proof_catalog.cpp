#include "war3_persistent_gpu_package_proof_catalog.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace dxvk::war3::gpu_skin {

namespace {

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

void HashU64(uint64_t& hash, uint64_t value) noexcept {
  for (uint32_t byte = 0u; byte < 8u; ++byte) {
    hash ^= (value >> (byte * 8u)) & 0xffu;
    hash *= kFnvPrime;
  }
}

void HashU32(uint64_t& hash, uint32_t value) noexcept {
  for (uint32_t byte = 0u; byte < 4u; ++byte) {
    hash ^= (value >> (byte * 8u)) & 0xffu;
    hash *= kFnvPrime;
  }
}

void HashFloat(uint64_t& hash, float value) noexcept {
  uint32_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  HashU32(hash, bits);
}

uint64_t PrimitiveProofAggregate(
    const GpuSkinStaticPrimitiveProof& primitive) noexcept {
  uint64_t hash = kFnvOffset;
  HashU64(hash, primitive.indexContentHash);
  HashU32(hash, primitive.ordinal);
  HashU32(hash, primitive.primitiveTypeOrMaterialSlot);
  HashU32(hash, primitive.firstIndex);
  HashU32(hash, primitive.indexCount);
  HashU32(hash, primitive.minVertex);
  HashU32(hash, primitive.maxVertex);
  return hash;
}

bool SameFence(
    const War3PersistentGpuPackageProofCatalog::ProducerFencePoint& lhs,
    const War3PersistentGpuPackageProofCatalog::ProducerFencePoint& rhs)
    noexcept {
  return lhs.identity == rhs.identity && lhs.value == rhs.value;
}

uint64_t NextCatalogInstanceGeneration() noexcept {
  static std::mutex mutex;
  static uint64_t next = 1u;
  std::lock_guard<std::mutex> lock(mutex);
  if (next == 0u || next == std::numeric_limits<uint64_t>::max())
    return 0u;
  return next++;
}

}  // namespace

const War3PersistentGpuPackageProofCatalog::Entry*
War3PersistentGpuPackageProofCatalog::Snapshot::entry(
    size_t index) const noexcept {
  return index < m_entries.size() ? &m_entries[index] : nullptr;
}

const War3PersistentGpuPackageProofCatalog::Entry*
War3PersistentGpuPackageProofCatalog::Snapshot::find(
    const Key& key) const noexcept {
  const auto found = std::lower_bound(
      m_entries.begin(), m_entries.end(), key,
      [](const Entry& candidate, const Key& expected) {
        return War3PersistentGpuPackageProofCatalog::keyLess(
            candidate.key, expected);
      });
  return found != m_entries.end() &&
          War3PersistentGpuPackageProofCatalog::sameKey(found->key, key)
      ? &*found
      : nullptr;
}

War3PersistentGpuPackageProofCatalog::
War3PersistentGpuPackageProofCatalog() {
  std::vector<Entry> entries;
  m_instanceGeneration = NextCatalogInstanceGeneration();
  // Revision zero means "no snapshot acquired" to renderer-side observers.
  // A valid empty catalog therefore starts at one and safely classifies every
  // lookup as ContentPending rather than disabling Observe altogether.
  m_snapshot = SharedSnapshot(new Snapshot(
      m_instanceGeneration, 1u, std::move(entries)));
}

War3PersistentGpuPackageProofCatalog::SharedSnapshot
War3PersistentGpuPackageProofCatalog::snapshot() const noexcept {
  return std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
}

bool War3PersistentGpuPackageProofCatalog::sameKey(
    const Key& lhs, const Key& rhs) noexcept {
  return lhs.schema == rhs.schema &&
      lhs.mapEpoch == rhs.mapEpoch &&
      lhs.deviceEpoch == rhs.deviceEpoch &&
      lhs.packageGeneration == rhs.packageGeneration &&
      lhs.immutableModelGeneration == rhs.immutableModelGeneration &&
      lhs.geosetData == rhs.geosetData &&
      lhs.contentHash == rhs.contentHash &&
      lhs.layoutGeneration == rhs.layoutGeneration &&
      lhs.primitiveOrdinal == rhs.primitiveOrdinal;
}

bool War3PersistentGpuPackageProofCatalog::keyLess(
    const Key& lhs, const Key& rhs) noexcept {
  if (lhs.schema != rhs.schema)
    return lhs.schema < rhs.schema;
  if (lhs.mapEpoch != rhs.mapEpoch)
    return lhs.mapEpoch < rhs.mapEpoch;
  if (lhs.deviceEpoch != rhs.deviceEpoch)
    return lhs.deviceEpoch < rhs.deviceEpoch;
  if (lhs.packageGeneration != rhs.packageGeneration)
    return lhs.packageGeneration < rhs.packageGeneration;
  if (lhs.immutableModelGeneration != rhs.immutableModelGeneration)
    return lhs.immutableModelGeneration < rhs.immutableModelGeneration;
  if (lhs.geosetData != rhs.geosetData)
    return lhs.geosetData < rhs.geosetData;
  if (lhs.contentHash != rhs.contentHash)
    return lhs.contentHash < rhs.contentHash;
  if (lhs.layoutGeneration != rhs.layoutGeneration)
    return lhs.layoutGeneration < rhs.layoutGeneration;
  return lhs.primitiveOrdinal < rhs.primitiveOrdinal;
}

uint64_t War3PersistentGpuPackageProofCatalog::canonicalDigest(
    const Key& key,
    const GpuSkinStaticPackageProof& proof,
    const GpuSkinStaticPrimitiveProof& primitive) noexcept {
  uint64_t hash = kFnvOffset;
  HashU32(hash, key.schema);
  HashU64(hash, key.mapEpoch);
  HashU64(hash, key.deviceEpoch);
  HashU64(hash, key.packageGeneration);
  HashU64(hash, key.immutableModelGeneration);
  HashU64(hash, uint64_t(key.geosetData));
  HashU64(hash, key.contentHash);
  HashU32(hash, key.layoutGeneration);
  HashU32(hash, key.primitiveOrdinal);

  HashU64(hash, proof.mapEpoch);
  HashU64(hash, proof.deviceEpoch);
  HashU64(hash, proof.packageGeneration);
  HashU64(hash, uint64_t(proof.geosetData));
  HashU64(hash, proof.contentHash);
  HashU64(hash, proof.positionContentHash);
  HashU64(hash, proof.normalContentHash);
  HashU64(hash, proof.vertexGroupContentHash);
  HashU64(hash, proof.uv0ContentHash);
  HashU64(hash, proof.uv1ContentHash);
  HashU64(hash, proof.indexContentHash);
  HashU64(hash, proof.primitiveProofHash);
  HashU64(hash, proof.localBoundsHash);
  HashU32(hash, proof.layoutGeneration);
  HashU32(hash, proof.vertexCount);
  HashU32(hash, proof.indexCount);
  HashU32(hash, proof.uvLayerCount);
  HashU32(hash, proof.primitiveProofCount);
  HashU32(hash, uint32_t(proof.indexType));
  HashFloat(hash, proof.localMinX);
  HashFloat(hash, proof.localMinY);
  HashFloat(hash, proof.localMinZ);
  HashFloat(hash, proof.localMaxX);
  HashFloat(hash, proof.localMaxY);
  HashFloat(hash, proof.localMaxZ);
  HashU32(hash, proof.staticByteOffset);
  HashU32(hash, proof.staticByteLength);
  HashU32(hash, proof.indexByteOffset);
  HashU32(hash, proof.indexByteLength);

  HashU64(hash, primitive.indexContentHash);
  HashU32(hash, primitive.ordinal);
  HashU32(hash, primitive.primitiveTypeOrMaterialSlot);
  HashU32(hash, primitive.firstIndex);
  HashU32(hash, primitive.indexCount);
  HashU32(hash, primitive.minVertex);
  HashU32(hash, primitive.maxVertex);
  return hash != 0u ? hash : kFnvOffset;
}

bool War3PersistentGpuPackageProofCatalog::validKey(
    const Key& key) noexcept {
  return key.schema == kSchemaVersion && key.mapEpoch != 0u &&
      key.deviceEpoch != 0u && key.packageGeneration != 0u &&
      key.immutableModelGeneration != 0u && key.geosetData != 0u &&
      key.contentHash != 0u && key.layoutGeneration != 0u;
}

bool War3PersistentGpuPackageProofCatalog::validFence(
    const ProducerFencePoint& fence) noexcept {
  return fence.identity != 0u && fence.value != 0u;
}

bool War3PersistentGpuPackageProofCatalog::validFenceObservation(
    const ProducerFenceObservation& observation) noexcept {
  return observation.identity != 0u;
}

bool War3PersistentGpuPackageProofCatalog::validPreparedPackage(
    const PreparedPackage& prepared) noexcept {
  const Key& key = prepared.key;
  const GpuSkinStaticPackageProof& proof = prepared.packageProof;
  const GpuSkinStaticPrimitiveProof& primitive = prepared.primitiveProof;
  const uint64_t groupEnd = uint64_t(proof.vertexCount) * 25u;
  const uint64_t texcoordOffset = (groupEnd + 3u) & ~uint64_t(3u);
  const uint64_t expectedStaticBytes = texcoordOffset +
      uint64_t(proof.vertexCount) * 8u * proof.uvLayerCount;
  const uint64_t staticEnd = uint64_t(proof.staticByteOffset) +
      proof.staticByteLength;
  if (!validKey(key) || proof.mapEpoch != key.mapEpoch ||
      proof.deviceEpoch != key.deviceEpoch ||
      proof.packageGeneration != key.packageGeneration ||
      proof.geosetData != key.geosetData ||
      proof.contentHash != key.contentHash ||
      proof.layoutGeneration != key.layoutGeneration ||
      primitive.ordinal != key.primitiveOrdinal ||
      proof.positionContentHash == 0u || proof.normalContentHash == 0u ||
      proof.vertexGroupContentHash == 0u ||
      proof.indexContentHash == 0u || proof.primitiveProofHash == 0u ||
      proof.localBoundsHash == 0u || proof.vertexCount == 0u ||
      proof.indexCount == 0u || proof.primitiveProofCount == 0u ||
      proof.primitiveProofCount != 1u || key.primitiveOrdinal != 0u ||
      primitive.ordinal >= proof.primitiveProofCount ||
      primitive.ordinal != 0u || primitive.firstIndex != 0u ||
      primitive.indexCount != proof.indexCount ||
      primitive.indexContentHash != proof.indexContentHash ||
      PrimitiveProofAggregate(primitive) != proof.primitiveProofHash ||
      primitive.indexContentHash == 0u || primitive.indexCount == 0u ||
      primitive.minVertex > primitive.maxVertex ||
      primitive.maxVertex >= proof.vertexCount ||
      primitive.firstIndex > proof.indexCount ||
      primitive.indexCount > proof.indexCount - primitive.firstIndex ||
      proof.indexType != VK_INDEX_TYPE_UINT16 ||
      expectedStaticBytes == 0u ||
      expectedStaticBytes > std::numeric_limits<uint32_t>::max() ||
      proof.staticByteLength != expectedStaticBytes ||
      proof.indexByteLength == 0u ||
      staticEnd > proof.indexByteOffset ||
      uint64_t(proof.indexByteLength) !=
          uint64_t(proof.indexCount) * sizeof(uint16_t) ||
      proof.uvLayerCount > 2u ||
      (proof.uvLayerCount == 0u &&
          (proof.uv0ContentHash != 0u || proof.uv1ContentHash != 0u)) ||
      (proof.uvLayerCount == 1u &&
          (proof.uv0ContentHash == 0u || proof.uv1ContentHash != 0u)) ||
      (proof.uvLayerCount == 2u &&
          (proof.uv0ContentHash == 0u || proof.uv1ContentHash == 0u)) ||
      !std::isfinite(proof.localMinX) ||
      !std::isfinite(proof.localMinY) ||
      !std::isfinite(proof.localMinZ) ||
      !std::isfinite(proof.localMaxX) ||
      !std::isfinite(proof.localMaxY) ||
      !std::isfinite(proof.localMaxZ) ||
      proof.localMinX > proof.localMaxX ||
      proof.localMinY > proof.localMaxY ||
      proof.localMinZ > proof.localMaxZ)
    return false;
  return true;
}

bool War3PersistentGpuPackageProofCatalog::validExactToken(
    const ExactTokenEvidence& token) noexcept {
  return token.sealedToken != 0u && token.drawToken != 0u &&
      token.sealedToken == token.drawToken;
}

bool War3PersistentGpuPackageProofCatalog::validRoute(
    const DrawEvidence& evidence) noexcept {
  return validExactToken(evidence.identity) &&
      validExactToken(evidence.source) &&
      evidence.currentDrawSourceGeneration != 0u &&
      evidence.identityExact && evidence.sourceExact &&
      evidence.staticRigid && !evidence.dynamic && !evidence.skinned &&
      evidence.fresh && !evidence.grace && !evidence.blocker &&
      !evidence.rejected;
}

bool War3PersistentGpuPackageProofCatalog::sameStreamHashes(
    const StreamHashEvidence& evidence,
    const GpuSkinStaticPackageProof& proof) noexcept {
  return evidence.position == proof.positionContentHash &&
      evidence.normal == proof.normalContentHash &&
      evidence.vertexGroup == proof.vertexGroupContentHash &&
      evidence.uv0 == proof.uv0ContentHash &&
      evidence.uv1 == proof.uv1ContentHash &&
      evidence.index == proof.indexContentHash &&
      evidence.primitiveAggregate == proof.primitiveProofHash &&
      evidence.localBounds == proof.localBoundsHash;
}

bool War3PersistentGpuPackageProofCatalog::samePrimitiveDomain(
    const PrimitiveDomainEvidence& evidence,
    const GpuSkinStaticPackageProof& packageProof,
    const GpuSkinStaticPrimitiveProof& primitiveProof) noexcept {
  return evidence.firstIndex == primitiveProof.firstIndex &&
      evidence.indexCount == primitiveProof.indexCount &&
      evidence.minVertex == primitiveProof.minVertex &&
      evidence.maxVertex == primitiveProof.maxVertex &&
      evidence.wholeIndexCount == packageProof.indexCount &&
      evidence.vertexCount == packageProof.vertexCount &&
      evidence.indexType == packageProof.indexType &&
      evidence.indexCount != 0u &&
      evidence.minVertex <= evidence.maxVertex &&
      evidence.maxVertex < evidence.vertexCount &&
      evidence.firstIndex <= evidence.wholeIndexCount &&
      evidence.indexCount <=
          evidence.wholeIndexCount - evidence.firstIndex;
}

War3PersistentGpuPackageProofCatalog::PackageContentDecision
War3PersistentGpuPackageProofCatalog::reject(
    PackageContentDecision::Reason reason,
    uint64_t catalogSnapshotRevision,
    const Entry* entry) noexcept {
  const Value* value = entry != nullptr ? &entry->value : nullptr;
  return PackageContentDecision(
      false, reason, 0u, catalogSnapshotRevision,
      value != nullptr ? value->publicationRevision : 0u,
      value != nullptr ? value->canonicalDigest : 0u,
      entry != nullptr ? entry->key : Key{},
      0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
}

bool War3PersistentGpuPackageProofCatalog::PackageContentDecision::matches(
    const Key& key, uint64_t catalogInstanceGeneration,
    uint64_t catalogSnapshotRevision,
    uint64_t frameSerial, uint64_t policyRevision, uint32_t stage,
    uint64_t identityToken, uint64_t sourceToken,
    uint64_t currentDrawSourceGeneration,
    uint64_t materialToken, uint64_t alphaToken,
    uint64_t worldToken, uint64_t boundsToken) const noexcept {
  return m_ready && catalogInstanceGeneration != 0u &&
      catalogInstanceGeneration == m_catalogInstanceGeneration &&
      catalogSnapshotRevision != 0u &&
      catalogSnapshotRevision == m_catalogSnapshotRevision &&
      War3PersistentGpuPackageProofCatalog::sameKey(m_key, key) &&
      frameSerial != 0u && frameSerial == m_frameSerial &&
      policyRevision != 0u && policyRevision == m_policyRevision &&
      stage == kRequiredStage && stage == m_stage &&
      identityToken != 0u && identityToken == m_identityToken &&
      sourceToken != 0u && sourceToken == m_sourceToken &&
      currentDrawSourceGeneration != 0u &&
      currentDrawSourceGeneration == m_currentDrawSourceGeneration &&
      materialToken != 0u && materialToken == m_materialToken &&
      alphaToken != 0u && alphaToken == m_alphaToken &&
      worldToken != 0u && worldToken == m_worldToken &&
      boundsToken != 0u && boundsToken == m_boundsToken;
}

bool War3PersistentGpuPackageProofCatalog::bindOrCheckWriter() noexcept {
  const std::thread::id current = std::this_thread::get_id();
  if (!m_writerBound) {
    m_writerThread = current;
    m_writerBound = true;
    return true;
  }
  return m_writerThread == current;
}

void War3PersistentGpuPackageProofCatalog::storeSnapshot(
    std::vector<Entry>&& entries, uint64_t revision) noexcept {
  SharedSnapshot next(new Snapshot(
      m_instanceGeneration, revision, std::move(entries)));
  std::atomic_store_explicit(
      &m_snapshot, std::move(next), std::memory_order_release);
}

War3PersistentGpuPackageProofCatalog::MutationResult
War3PersistentGpuPackageProofCatalog::publishPrepared(
    const PreparedPackage& package) {
  if (!validPreparedPackage(package))
    return MutationResult::Invalid;

  std::lock_guard<std::mutex> lock(m_writerMutex);
  if (!bindOrCheckWriter())
    return MutationResult::WrongWriter;

  const SharedSnapshot current = snapshot();
  if (current == nullptr ||
      current->revision() == std::numeric_limits<uint64_t>::max())
    return MutationResult::Invalid;

  std::vector<Entry> entries = current->m_entries;
  const auto found = std::lower_bound(
      entries.begin(), entries.end(), package.key,
      [](const Entry& candidate, const Key& expected) {
        return keyLess(candidate.key, expected);
      });
  const uint64_t digest = canonicalDigest(
      package.key, package.packageProof, package.primitiveProof);
  if (found != entries.end() && sameKey(found->key, package.key)) {
    if (!SameGpuSkinStaticPackageProof(
            found->value.packageProof, package.packageProof) ||
        !SameGpuSkinStaticPrimitiveProof(
            found->value.primitiveProof, package.primitiveProof) ||
        found->value.canonicalDigest != digest)
      return MutationResult::ImmutableConflict;
    return found->value.state == PublicationState::Invalidated
        ? MutationResult::StateConflict
        : MutationResult::Duplicate;
  }

  const uint64_t revision = current->revision() + 1u;
  Entry inserted;
  inserted.key = package.key;
  inserted.value.packageProof = package.packageProof;
  inserted.value.primitiveProof = package.primitiveProof;
  inserted.value.canonicalDigest = digest;
  inserted.value.publicationRevision = revision;
  inserted.value.state = PublicationState::Prepared;
  entries.insert(found, inserted);
  storeSnapshot(std::move(entries), revision);
  return MutationResult::Accepted;
}

War3PersistentGpuPackageProofCatalog::MutationResult
War3PersistentGpuPackageProofCatalog::publishTransition(
    const Key& key, PublicationState target,
    const ProducerFencePoint* fence,
    const ProducerFenceObservation* observation) {
  if (!validKey(key) ||
      (target == PublicationState::UploadSubmitted &&
       (fence == nullptr || !validFence(*fence))) ||
      (target == PublicationState::UploadCompleted &&
       (observation == nullptr || !validFenceObservation(*observation))))
    return MutationResult::Invalid;

  const SharedSnapshot current = snapshot();
  if (current == nullptr ||
      current->revision() == std::numeric_limits<uint64_t>::max())
    return MutationResult::Invalid;
  std::vector<Entry> entries = current->m_entries;
  const auto found = std::lower_bound(
      entries.begin(), entries.end(), key,
      [](const Entry& candidate, const Key& expected) {
        return keyLess(candidate.key, expected);
      });
  if (found == entries.end() || !sameKey(found->key, key))
    return MutationResult::NotFound;

  Value& value = found->value;
  if (target == PublicationState::Invalidated) {
    if (value.state == PublicationState::Invalidated)
      return MutationResult::Duplicate;
  } else if (target == PublicationState::UploadSubmitted) {
    if (value.state == PublicationState::Invalidated)
      return MutationResult::StateConflict;
    if (value.state == PublicationState::UploadSubmitted ||
        value.state == PublicationState::UploadCompleted)
      return SameFence(value.producerFence, *fence)
          ? MutationResult::Duplicate
          : MutationResult::FenceMismatch;
    if (value.state != PublicationState::Prepared)
      return MutationResult::StateConflict;
    value.producerFence = *fence;
  } else if (target == PublicationState::UploadCompleted) {
    if (value.state != PublicationState::UploadSubmitted &&
        value.state != PublicationState::UploadCompleted)
      return MutationResult::StateConflict;
    if (value.producerFence.identity != observation->identity)
      return MutationResult::FenceMismatch;
    if (!observation->querySucceeded ||
        observation->completedValue < value.producerFence.value)
      return MutationResult::CompletionNotObserved;
    if (value.state == PublicationState::UploadCompleted)
      return MutationResult::Duplicate;
  } else {
    return MutationResult::Invalid;
  }

  const uint64_t revision = current->revision() + 1u;
  value.state = target;
  value.publicationRevision = revision;
  storeSnapshot(std::move(entries), revision);
  return MutationResult::Accepted;
}

War3PersistentGpuPackageProofCatalog::MutationResult
War3PersistentGpuPackageProofCatalog::publishUploadSubmitted(
    const Key& key, const ProducerFencePoint& fence) {
  if (!validKey(key) || !validFence(fence))
    return MutationResult::Invalid;
  std::lock_guard<std::mutex> lock(m_writerMutex);
  if (!bindOrCheckWriter())
    return MutationResult::WrongWriter;
  return publishTransition(
      key, PublicationState::UploadSubmitted, &fence, nullptr);
}

War3PersistentGpuPackageProofCatalog::MutationResult
War3PersistentGpuPackageProofCatalog::publishUploadCompleted(
    const Key& key, const ProducerFenceObservation& observation) {
  if (!validKey(key) || !validFenceObservation(observation))
    return MutationResult::Invalid;
  std::lock_guard<std::mutex> lock(m_writerMutex);
  if (!bindOrCheckWriter())
    return MutationResult::WrongWriter;
  return publishTransition(
      key, PublicationState::UploadCompleted, nullptr, &observation);
}

War3PersistentGpuPackageProofCatalog::MutationResult
War3PersistentGpuPackageProofCatalog::publishInvalidated(const Key& key) {
  if (!validKey(key))
    return MutationResult::Invalid;
  std::lock_guard<std::mutex> lock(m_writerMutex);
  if (!bindOrCheckWriter())
    return MutationResult::WrongWriter;
  return publishTransition(
      key, PublicationState::Invalidated, nullptr, nullptr);
}

War3PersistentGpuPackageProofCatalog::PackageContentDecision
War3PersistentGpuPackageProofCatalog::validateDrawEvidence(
    const SharedSnapshot& catalog,
    const ValidationContext& context,
    const DrawEvidence& evidence) noexcept {
  using Reason = PackageContentDecision::Reason;
  if (catalog == nullptr)
    return reject(Reason::MissingSnapshot);
  if (catalog->instanceGeneration() == 0u ||
      evidence.catalogInstanceGeneration != catalog->instanceGeneration())
    return reject(Reason::CatalogInstanceGenerationMismatch);
  if (context.frameSerial == 0u || context.policyRevision == 0u ||
      context.stage != kRequiredStage ||
      evidence.frameSerial != context.frameSerial ||
      evidence.policyRevision != context.policyRevision ||
      evidence.stage != context.stage)
    return reject(
        Reason::InvalidDrawContext,
        catalog != nullptr ? catalog->revision() : 0u);
  if (!validKey(evidence.key))
    return reject(Reason::InvalidKey, catalog->revision());

  if (evidence.catalogSnapshotRevision == 0u ||
      evidence.catalogSnapshotRevision != catalog->revision())
    return reject(
        Reason::CatalogSnapshotRevisionMismatch, catalog->revision());

  const Entry* entry = catalog->find(evidence.key);
  if (entry == nullptr)
    return reject(Reason::EntryNotFound, catalog->revision());
  const Value& value = entry->value;
  if (evidence.canonicalDigest == 0u ||
      evidence.canonicalDigest != value.canonicalDigest)
    return reject(Reason::DigestMismatch, catalog->revision(), entry);
  if (value.state != PublicationState::UploadCompleted)
    return reject(Reason::UploadNotCompleted, catalog->revision(), entry);
  if (!validFence(value.producerFence))
    return reject(Reason::ProducerFenceMissing, catalog->revision(), entry);

  // Digest equality is never sufficient. These complete comparisons are the
  // authoritative immutable content proof.
  if (!SameGpuSkinStaticPackageProof(
          evidence.packageProof, value.packageProof))
    return reject(
        Reason::PackageProofMismatch, catalog->revision(), entry);
  if (!SameGpuSkinStaticPrimitiveProof(
          evidence.primitiveProof, value.primitiveProof))
    return reject(
        Reason::PrimitiveProofMismatch, catalog->revision(), entry);
  if (!sameStreamHashes(evidence.streamHashes, value.packageProof))
    return reject(Reason::StreamHashMismatch, catalog->revision(), entry);
  if (!samePrimitiveDomain(
          evidence.primitiveDomain, value.packageProof,
          value.primitiveProof))
    return reject(
        Reason::PrimitiveDomainMismatch, catalog->revision(), entry);
  if (!validRoute(evidence))
    return reject(Reason::RouteRejected, catalog->revision(), entry);
  if (!validExactToken(evidence.material) ||
      !validExactToken(evidence.alpha) ||
      !validExactToken(evidence.world) ||
      !validExactToken(evidence.bounds))
    return reject(Reason::ExactTokenMismatch, catalog->revision(), entry);

  return PackageContentDecision(
      true, Reason::Ready, catalog->instanceGeneration(),
      catalog->revision(),
      value.publicationRevision, value.canonicalDigest, entry->key,
      evidence.frameSerial, evidence.policyRevision, evidence.stage,
      evidence.identity.drawToken, evidence.source.drawToken,
      evidence.currentDrawSourceGeneration,
      evidence.material.drawToken, evidence.alpha.drawToken,
      evidence.world.drawToken, evidence.bounds.drawToken);
}

}  // namespace dxvk::war3::gpu_skin
