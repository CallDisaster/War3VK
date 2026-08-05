#pragma once

#include "war3_gpu_skin_resources.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace dxvk::war3::gpu_skin {

// CPU/value-only publication boundary between the future package producer and
// renderer-side proof observers. Snapshots own no GPU object or atlas slice.
// The catalog is deliberately not instantiated by any production owner yet.
class War3PersistentGpuPackageProofCatalog final {
public:
  static constexpr uint32_t kSchemaVersion = 1u;
  static constexpr uint32_t kRequiredStage = 11u;
  static constexpr bool kRuntimeInstantiated = false;
  static constexpr bool kBindsAtlas = false;
  static constexpr bool kConsumeAdmissionGranted = false;
  static constexpr bool kStorePublicationAuthorityIntegrated = false;
  // The current value-only foundation intentionally admits only a complete
  // one-primitive package. Multi-primitive publication requires a future
  // atomic package API and cannot be assembled from independently ready rows.
  static constexpr bool kMultiPrimitivePublicationGranted = false;

  enum class PublicationState : uint32_t {
    Prepared = 0u,
    UploadSubmitted,
    UploadCompleted,
    Invalidated,
  };

  struct Key {
    uint32_t schema = 0u;
    uint64_t mapEpoch = 0u;
    uint64_t deviceEpoch = 0u;
    uint64_t packageGeneration = 0u;
    uint64_t immutableModelGeneration = 0u;
    uintptr_t geosetData = 0u;
    uint64_t contentHash = 0u;
    uint32_t layoutGeneration = 0u;
    uint32_t primitiveOrdinal = 0u;
  };

  struct ProducerFencePoint {
    // Opaque CPU values only. The catalog never dereferences the identity and
    // never queries, waits, signals or owns the producer fence.
    uint64_t identity = 0u;
    uint64_t value = 0u;
  };

  struct ProducerFenceObservation {
    uint64_t identity = 0u;
    uint64_t completedValue = 0u;
    bool querySucceeded = false;
  };

  struct Value {
    GpuSkinStaticPackageProof packageProof;
    GpuSkinStaticPrimitiveProof primitiveProof;
    uint64_t canonicalDigest = 0u;
    // Revision at which this individual entry last changed. This is not the
    // revision of an acquired immutable Snapshot: multiple entries with
    // different publication revisions legitimately coexist in one snapshot.
    uint64_t publicationRevision = 0u;
    ProducerFencePoint producerFence;
    PublicationState state = PublicationState::Prepared;
  };

  struct Entry {
    Key key;
    Value value;
  };

  class Snapshot final {
  public:
    uint64_t instanceGeneration() const noexcept {
      return m_instanceGeneration;
    }
    uint64_t revision() const noexcept { return m_revision; }
    size_t size() const noexcept { return m_entries.size(); }
    const Entry* entry(size_t index) const noexcept;
    const Entry* find(const Key& key) const noexcept;

  private:
    friend class War3PersistentGpuPackageProofCatalog;
    Snapshot(
        uint64_t instanceGeneration, uint64_t revision,
        std::vector<Entry>&& entries)
    : m_instanceGeneration(instanceGeneration), m_revision(revision),
      m_entries(std::move(entries)) { }

    uint64_t m_instanceGeneration = 0u;
    uint64_t m_revision = 0u;
    std::vector<Entry> m_entries;
  };

  using SharedSnapshot = std::shared_ptr<const Snapshot>;

  struct PreparedPackage {
    Key key;
    GpuSkinStaticPackageProof packageProof;
    GpuSkinStaticPrimitiveProof primitiveProof;
  };

  struct StreamHashEvidence {
    uint64_t position = 0u;
    uint64_t normal = 0u;
    uint64_t vertexGroup = 0u;
    uint64_t uv0 = 0u;
    uint64_t uv1 = 0u;
    uint64_t index = 0u;
    uint64_t primitiveAggregate = 0u;
    uint64_t localBounds = 0u;
  };

  struct PrimitiveDomainEvidence {
    uint32_t firstIndex = 0u;
    uint32_t indexCount = 0u;
    uint32_t minVertex = 0u;
    uint32_t maxVertex = 0u;
    uint32_t wholeIndexCount = 0u;
    uint32_t vertexCount = 0u;
    VkIndexType indexType = VK_INDEX_TYPE_MAX_ENUM;
  };

  struct ExactTokenEvidence {
    uint64_t sealedToken = 0u;
    uint64_t drawToken = 0u;
  };

  struct ValidationContext {
    uint64_t frameSerial = 0u;
    uint64_t policyRevision = 0u;
    uint32_t stage = 0u;
  };

  struct DrawEvidence {
    uint64_t frameSerial = 0u;
    uint64_t policyRevision = 0u;
    uint32_t stage = 0u;
    uint64_t catalogInstanceGeneration = 0u;
    // Exact revision of the immutable snapshot used to build this evidence.
    uint64_t catalogSnapshotRevision = 0u;
    uint64_t canonicalDigest = 0u;
    uint64_t currentDrawSourceGeneration = 0u;
    Key key;
    GpuSkinStaticPackageProof packageProof;
    GpuSkinStaticPrimitiveProof primitiveProof;
    StreamHashEvidence streamHashes;
    PrimitiveDomainEvidence primitiveDomain;
    ExactTokenEvidence identity;
    ExactTokenEvidence source;
    ExactTokenEvidence material;
    ExactTokenEvidence alpha;
    ExactTokenEvidence world;
    ExactTokenEvidence bounds;
    bool identityExact = false;
    bool sourceExact = false;
    bool staticRigid = false;
    bool dynamic = false;
    bool skinned = false;
    bool fresh = false;
    bool grace = false;
    bool blocker = false;
    bool rejected = false;
  };

  class PackageContentDecision final {
  public:
    enum class Reason : uint32_t {
      MissingSnapshot = 0u,
      InvalidDrawContext,
      InvalidKey,
      EntryNotFound,
      CatalogInstanceGenerationMismatch,
      CatalogSnapshotRevisionMismatch,
      DigestMismatch,
      UploadNotCompleted,
      ProducerFenceMissing,
      PackageProofMismatch,
      PrimitiveProofMismatch,
      StreamHashMismatch,
      PrimitiveDomainMismatch,
      RouteRejected,
      ExactTokenMismatch,
      Ready,
    };

    PackageContentDecision() = default;
    bool ready() const noexcept { return m_ready; }
    Reason reason() const noexcept { return m_reason; }
    uint64_t catalogSnapshotRevision() const noexcept {
      return m_catalogSnapshotRevision;
    }
    uint64_t catalogInstanceGeneration() const noexcept {
      return m_catalogInstanceGeneration;
    }
    uint64_t publicationRevision() const noexcept {
      return m_publicationRevision;
    }
    uint64_t canonicalDigest() const noexcept { return m_canonicalDigest; }
    // A ready decision is valid only for the exact immutable snapshot and the
    // complete package key that the validator examined. This prevents a valid
    // decision for one primitive from being reused for another sealed row.
    bool matches(
        const Key& key, uint64_t catalogInstanceGeneration,
        uint64_t catalogSnapshotRevision,
        uint64_t frameSerial, uint64_t policyRevision, uint32_t stage,
        uint64_t identityToken, uint64_t sourceToken,
        uint64_t currentDrawSourceGeneration,
        uint64_t materialToken, uint64_t alphaToken,
        uint64_t worldToken, uint64_t boundsToken) const noexcept;

  private:
    friend class War3PersistentGpuPackageProofCatalog;
    PackageContentDecision(
        bool ready, Reason reason, uint64_t catalogInstanceGeneration,
        uint64_t catalogSnapshotRevision,
        uint64_t publicationRevision, uint64_t canonicalDigest,
        const Key& key, uint64_t frameSerial, uint64_t policyRevision,
        uint32_t stage, uint64_t identityToken, uint64_t sourceToken,
        uint64_t currentDrawSourceGeneration,
        uint64_t materialToken, uint64_t alphaToken,
        uint64_t worldToken, uint64_t boundsToken) noexcept
    : m_ready(ready), m_reason(reason),
      m_catalogInstanceGeneration(catalogInstanceGeneration),
      m_catalogSnapshotRevision(catalogSnapshotRevision),
      m_publicationRevision(publicationRevision),
      m_canonicalDigest(canonicalDigest), m_key(key),
      m_frameSerial(frameSerial), m_policyRevision(policyRevision),
      m_stage(stage),
      m_identityToken(identityToken), m_sourceToken(sourceToken),
      m_currentDrawSourceGeneration(currentDrawSourceGeneration),
      m_materialToken(materialToken), m_alphaToken(alphaToken),
      m_worldToken(worldToken), m_boundsToken(boundsToken) { }

    bool m_ready = false;
    Reason m_reason = Reason::MissingSnapshot;
    uint64_t m_catalogInstanceGeneration = 0u;
    uint64_t m_catalogSnapshotRevision = 0u;
    uint64_t m_publicationRevision = 0u;
    uint64_t m_canonicalDigest = 0u;
    Key m_key = {};
    uint64_t m_frameSerial = 0u;
    uint64_t m_policyRevision = 0u;
    uint32_t m_stage = 0u;
    uint64_t m_identityToken = 0u;
    uint64_t m_sourceToken = 0u;
    uint64_t m_currentDrawSourceGeneration = 0u;
    uint64_t m_materialToken = 0u;
    uint64_t m_alphaToken = 0u;
    uint64_t m_worldToken = 0u;
    uint64_t m_boundsToken = 0u;
  };

  enum class MutationResult : uint32_t {
    Accepted = 0u,
    Duplicate,
    Invalid,
    WrongWriter,
    NotFound,
    ImmutableConflict,
    StateConflict,
    FenceMismatch,
    CompletionNotObserved,
  };

  War3PersistentGpuPackageProofCatalog();
  War3PersistentGpuPackageProofCatalog(
      const War3PersistentGpuPackageProofCatalog&) = delete;
  War3PersistentGpuPackageProofCatalog& operator=(
      const War3PersistentGpuPackageProofCatalog&) = delete;

  SharedSnapshot snapshot() const noexcept;

  MutationResult publishPrepared(const PreparedPackage& package);
  MutationResult publishUploadSubmitted(
      const Key& key, const ProducerFencePoint& fence);
  MutationResult publishUploadCompleted(
      const Key& key, const ProducerFenceObservation& observation);
  MutationResult publishInvalidated(const Key& key);

  // This is the only operation capable of constructing a ready decision.
  // Digest equality is an early rejection only; all key, package, primitive,
  // stream, domain, route and exact-token fields are compared afterwards.
  static PackageContentDecision validateDrawEvidence(
      const SharedSnapshot& snapshot,
      const ValidationContext& context,
      const DrawEvidence& evidence) noexcept;

  static bool sameKey(const Key& lhs, const Key& rhs) noexcept;
  static bool keyLess(const Key& lhs, const Key& rhs) noexcept;
  static uint64_t canonicalDigest(
      const Key& key,
      const GpuSkinStaticPackageProof& packageProof,
      const GpuSkinStaticPrimitiveProof& primitiveProof) noexcept;

private:
  static bool validKey(const Key& key) noexcept;
  static bool validFence(const ProducerFencePoint& fence) noexcept;
  static bool validFenceObservation(
      const ProducerFenceObservation& observation) noexcept;
  static bool validPreparedPackage(const PreparedPackage& package) noexcept;
  static bool validExactToken(const ExactTokenEvidence& token) noexcept;
  static bool validRoute(const DrawEvidence& evidence) noexcept;
  static bool sameStreamHashes(
      const StreamHashEvidence& evidence,
      const GpuSkinStaticPackageProof& proof) noexcept;
  static bool samePrimitiveDomain(
      const PrimitiveDomainEvidence& evidence,
      const GpuSkinStaticPackageProof& packageProof,
      const GpuSkinStaticPrimitiveProof& primitiveProof) noexcept;
  static PackageContentDecision reject(
      PackageContentDecision::Reason reason,
      uint64_t catalogSnapshotRevision = 0u,
      const Entry* entry = nullptr) noexcept;

  bool bindOrCheckWriter() noexcept;
  MutationResult publishTransition(
      const Key& key, PublicationState target,
      const ProducerFencePoint* fence,
      const ProducerFenceObservation* observation);
  void storeSnapshot(std::vector<Entry>&& entries, uint64_t revision) noexcept;

  mutable std::mutex m_writerMutex;
  bool m_writerBound = false;
  std::thread::id m_writerThread;
  uint64_t m_instanceGeneration = 0u;
  SharedSnapshot m_snapshot;
};

static_assert(!War3PersistentGpuPackageProofCatalog::kRuntimeInstantiated);
static_assert(!War3PersistentGpuPackageProofCatalog::kBindsAtlas);
static_assert(
    !War3PersistentGpuPackageProofCatalog::kConsumeAdmissionGranted);
static_assert(std::is_standard_layout_v<
    War3PersistentGpuPackageProofCatalog::Key>);
static_assert(std::is_trivially_copyable_v<
    War3PersistentGpuPackageProofCatalog::Key>);
static_assert(std::is_standard_layout_v<
    War3PersistentGpuPackageProofCatalog::Value>);
static_assert(std::is_trivially_copyable_v<
    War3PersistentGpuPackageProofCatalog::Value>);

}  // namespace dxvk::war3::gpu_skin
