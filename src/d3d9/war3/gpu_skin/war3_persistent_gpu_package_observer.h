#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dxvk::war3::gpu_skin {

// Value-only P1 observation table for a future persistent package consumer.
// It owns no renderer or GPU resource, and production code does not instantiate
// it. Observe can classify package-eligible candidates and intersect that
// eligibility with real canonical draw evidence, but it must always pass the
// canonical workload and consumer mask through unchanged.
class War3PersistentGpuPackageObserver final {
public:
  static constexpr bool kRuntimeInstantiated = false;
  static constexpr bool kObserveOnly = true;
  static constexpr bool kObserveBindsAtlas = false;
  static constexpr bool kObserveWritesConsumerLastUse = false;
  static constexpr bool kConsumeAdmissionGranted = false;
  static constexpr uint32_t kRequiredStage = 11u;
  static constexpr size_t kCapacity = 4096u;

  enum class Mode : uint8_t {
    Off = 0u,
    Observe = 1u,
    Consume = 2u,
  };

  enum ConsumerBits : uint32_t {
    ConsumerMain = 1u << 0u,
    ConsumerCsm0 = 1u << 1u,
    ConsumerCsm1 = 1u << 2u,
    ConsumerCsm2 = 1u << 3u,
    ConsumerCsm3 = 1u << 4u,
    ConsumerPointShadow = 1u << 5u,
    ConsumerGeometryOutline = 1u << 6u,
  };

  static constexpr uint32_t kKnownConsumerMask =
      ConsumerMain | ConsumerCsm0 | ConsumerCsm1 | ConsumerCsm2 |
      ConsumerCsm3 | ConsumerPointShadow | ConsumerGeometryOutline;
  // Only Main may already have executed before the future Stage11 seal point.
  // Shadow and outline consumers must use the post-draw note API.
  static constexpr uint32_t kPreSubmittedConsumerMask = ConsumerMain;

  enum class ProofState : uint8_t {
    IdentityOnly = 0u,
    ContentPending,
    PackageInputReady,
    FullyEquivalent,
    Rejected,
  };

  enum class Disposition : uint8_t {
    Recorded = 0u,
    ModeOff,
    UnsupportedMode,
    ConsumeNotAdmitted,
    InvalidFrameContext,
    InvalidConsumerMask,
    FrameMismatch,
    PolicyMismatch,
    StageMismatch,
    GenerationMismatch,
    NonFiniteBounds,
    DeferredBudget,
    DeferredCapacity,
  };

  struct Bounds {
    float minX = 0.0f;
    float minY = 0.0f;
    float minZ = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    float maxZ = 0.0f;
  };

  struct FrameContext {
    Mode requestedMode = Mode::Off;
    uint64_t frameSerial = 0u;
    uint64_t policyRevision = 0u;
    uint32_t stage = 0u;
    uint64_t mapEpoch = 0u;
    uint64_t deviceEpoch = 0u;
    uint64_t packageGeneration = 0u;
    uint64_t sourceGeneration = 0u;
    uint64_t canonicalWorkload = 0u;
    size_t observationBudget = kCapacity;
  };

  struct SealInput {
    uint64_t frameSerial = 0u;
    uint64_t policyRevision = 0u;
    uint32_t stage = 0u;
    uint64_t mapEpoch = 0u;
    uint64_t deviceEpoch = 0u;
    uint64_t packageGeneration = 0u;
    uint64_t sourceGeneration = 0u;

    uint64_t identityToken = 0u;
    uint64_t sourceToken = 0u;
    uint64_t materialToken = 0u;
    uint64_t alphaToken = 0u;
    uint64_t boundsToken = 0u;
    uint32_t requestedConsumerMask = 0u;
    uint32_t preSubmittedConsumerMask = 0u;
    Bounds bounds = {};

    bool identityKnown = false;
    bool identityExact = false;
    bool sourceKnown = false;
    bool sourceExact = false;
    bool packageContentReady = false;
    bool materialKnown = false;
    bool materialExact = false;
    bool alphaKnown = false;
    bool alphaExact = false;
    bool boundsKnown = false;
    bool boundsExact = false;
    bool staticRigidProven = false;
    bool dynamic = false;
    bool skinned = false;
  };

  struct Entry {
    uint64_t frameSerial = 0u;
    uint64_t policyRevision = 0u;
    uint32_t stage = 0u;
    uint64_t mapEpoch = 0u;
    uint64_t deviceEpoch = 0u;
    uint64_t packageGeneration = 0u;
    uint64_t sourceGeneration = 0u;
    uint64_t identityToken = 0u;
    uint64_t sourceToken = 0u;
    uint64_t materialToken = 0u;
    uint64_t alphaToken = 0u;
    uint64_t boundsToken = 0u;
    uint32_t requestedConsumerMask = 0u;
    uint32_t eligibleConsumerMask = 0u;
    uint32_t wouldUseConsumerMask = 0u;
    uint32_t actualConsumerMask = 0u;
    ProofState proofState = ProofState::Rejected;
    Disposition disposition = Disposition::Recorded;
  };

  struct ActualConsumerNote {
    size_t tableIndex = kCapacity;
    uint64_t frameSerial = 0u;
    uint64_t policyRevision = 0u;
    uint32_t stage = 0u;
    uint64_t mapEpoch = 0u;
    uint64_t deviceEpoch = 0u;
    uint64_t packageGeneration = 0u;
    uint64_t sourceGeneration = 0u;
    uint64_t identityToken = 0u;
    uint32_t consumerBit = 0u;
  };

  enum class ActualNoteResult : uint8_t {
    Recorded = 0u,
    Duplicate,
    ModeOff,
    ConsumeNotAdmitted,
    InvalidEntry,
    ContextMismatch,
    GenerationMismatch,
    EntryMismatch,
    UnknownConsumerBit,
    ConsumerNotRequested,
  };

  struct BeginDecision {
    Mode requestedMode = Mode::Off;
    Mode effectiveMode = Mode::Off;
    Disposition disposition = Disposition::ModeOff;
    uint64_t canonicalWorkload = 0u;
  };

  struct SealDecision {
    ProofState proofState = ProofState::Rejected;
    Disposition disposition = Disposition::ModeOff;
    uint32_t requestedConsumerMask = 0u;
    uint32_t effectiveConsumerMask = 0u;
    uint32_t eligibleConsumerMask = 0u;
    uint32_t wouldUseConsumerMask = 0u;
    uint32_t actualConsumerMask = 0u;
    size_t tableIndex = kCapacity;
    bool recorded = false;
    bool preSubmittedAccepted = false;
    bool drawMutationAllowed = false;
    bool atlasBindingAllowed = false;
    bool writesConsumerLastUse = false;
  };

  struct Diagnostics {
    Mode requestedMode = Mode::Off;
    Mode effectiveMode = Mode::Off;
    Disposition beginDisposition = Disposition::ModeOff;
    uint64_t canonicalWorkload = 0u;
    uint64_t sealCalls = 0u;
    uint64_t recorded = 0u;
    uint64_t identityOnly = 0u;
    uint64_t contentPending = 0u;
    uint64_t packageInputReady = 0u;
    uint64_t fullyEquivalent = 0u;
    uint64_t rejected = 0u;
    uint64_t deferredBudget = 0u;
    uint64_t deferredCapacity = 0u;
    uint64_t eligibleEntries = 0u;
    uint32_t eligibleConsumerMaskOr = 0u;
    uint64_t wouldUseEntries = 0u;
    uint32_t wouldUseConsumerMaskOr = 0u;
    uint64_t actualConsumerEntries = 0u;
    uint64_t actualConsumerBits = 0u;
    uint32_t actualConsumerMaskOr = 0u;
    uint64_t invalidPreSubmittedMasks = 0u;
    uint64_t actualNoteCalls = 0u;
    uint64_t actualNotesRecorded = 0u;
    uint64_t actualNotesDuplicate = 0u;
    uint64_t actualNotesRejected = 0u;
  };

  BeginDecision beginFrame(const FrameContext& context) noexcept;
  SealDecision seal(const SealInput& input) noexcept;
  // The future renderer may call this only after the named consumer's real
  // canonical draw has been submitted for this exact sealed entry.
  ActualNoteResult noteActualConsumer(
      const ActualConsumerNote& note) noexcept;

  const Entry* entry(size_t index) const noexcept;
  size_t size() const noexcept { return m_size; }
  size_t capacity() const noexcept { return kCapacity; }
  const Diagnostics& diagnostics() const noexcept { return m_diagnostics; }

  // Observe and Off are identity functions over canonical work.
  uint64_t effectiveCanonicalWorkload() const noexcept {
    return m_context.canonicalWorkload;
  }

private:
  static bool isSupportedMode(Mode mode) noexcept;
  static bool isValidFrameContext(const FrameContext& context) noexcept;
  static bool isFiniteBounds(const Bounds& bounds) noexcept;
  static bool exactFrameKey(
      const FrameContext& context, const SealInput& input) noexcept;
  static bool isKnownSingleConsumerBit(uint32_t consumerBit) noexcept;
  static ProofState classify(const SealInput& input) noexcept;

  SealDecision makePassthroughDecision(
      const SealInput& input, ProofState state,
      Disposition disposition) const noexcept;
  SealDecision append(
      const SealInput& input, ProofState state,
      Disposition disposition) noexcept;
  void countState(ProofState state) noexcept;
  void countWouldUseTransition(
      uint32_t previousMask, uint32_t nextMask) noexcept;
  void countActualTransition(
      uint32_t previousMask, uint32_t nextMask) noexcept;

  std::array<Entry, kCapacity> m_entries = {};
  FrameContext m_context = {};
  Diagnostics m_diagnostics = {};
  size_t m_size = 0u;
  size_t m_budget = 0u;
};

static_assert(std::is_standard_layout_v<
    War3PersistentGpuPackageObserver::Bounds>);
static_assert(std::is_trivially_copyable_v<
    War3PersistentGpuPackageObserver::Bounds>);
static_assert(std::is_standard_layout_v<
    War3PersistentGpuPackageObserver::Entry>);
static_assert(std::is_trivially_copyable_v<
    War3PersistentGpuPackageObserver::Entry>);
static_assert(!War3PersistentGpuPackageObserver::kRuntimeInstantiated);
static_assert(War3PersistentGpuPackageObserver::kObserveOnly);
static_assert(!War3PersistentGpuPackageObserver::kObserveBindsAtlas);
static_assert(
    !War3PersistentGpuPackageObserver::kObserveWritesConsumerLastUse);
static_assert(!War3PersistentGpuPackageObserver::kConsumeAdmissionGranted);

}  // namespace dxvk::war3::gpu_skin
