#pragma once

#include <cstdint>
#include <type_traits>

namespace dxvk::war3::gpu_skin {

// CPU-only evidence adapter at the final accepted Stage11 caster boundary.
// It cannot publish a package, bind GPU storage, record commands, mutate a
// draw, or authorize Consume.  Its sole purpose is to measure how often the
// exact submitted caster can be joined to an explicit immutable geoset
// sidecar before any persistent package runtime is allowed to exist.
class War3PersistentGpuPackageStage11ObserveAdapter final {
public:
  static constexpr bool kRuntimeObserveIntegrated = true;
  static constexpr bool kObserveOnly = true;
  static constexpr bool kConsumeAdmissionGranted = false;
  static constexpr bool kBindsGpuResources = false;
  static constexpr bool kRecordsCommands = false;
  static constexpr bool kMutatesCanonicalDraw = false;
  static constexpr bool kPublishesPackage = false;
  // ShadowModelResourceCache is process-lived and not map-epoch scoped. An
  // address/content join can measure coverage, but it can never prove that the
  // sidecar still describes current Game.dll memory.
  static constexpr bool kProvesCurrentGameMemory = false;
  static constexpr uint32_t kRequiredStage = 11u;

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

  static constexpr uint32_t kRequestedConsumerMask =
      ConsumerCsm0 | ConsumerCsm1 | ConsumerCsm2 | ConsumerCsm3;

  enum class Disposition : uint8_t {
    ModeOff = 0u,
    ConsumeNotAdmitted,
    InvalidExactSubmittedWitness,
    SourceGenerationExhausted,
    ExplicitGeosetDataSidecarLookupFailed,
    MissingExplicitGeosetDataSidecar,
    InvalidExplicitGeosetDataSidecar,
    RecordedContentIdentityOnly,
  };

  struct ExplicitGeosetDataSidecar {
    uintptr_t geosetDataIdentity = 0u;
    uint64_t contentHash = 0u;
    uint64_t immutableModelGeneration = 0u;
    uint32_t vertexCount = 0u;
    bool found = false;
    bool lookupFailed = false;
  };

  struct ExactSubmittedWitness {
    uint64_t frameSerial = 0u;
    uint64_t exactSubmittedFrameSerial = 0u;
    uint64_t policyRevision = 0u;
    uint64_t mapEpoch = 0u;
    uint64_t deviceEpoch = 0u;
    uint64_t exactGeometryKeyHash = 0u;
    uintptr_t instanceIdentity = 0u;
    uintptr_t meshPayloadIdentity = 0u;
    uintptr_t renderablePartIdentity = 0u;
    uint32_t jHandle = 0u;
    uint32_t layerIndex = 0u;
    uint32_t payloadWord108 = 0u;
    uint32_t payloadWord11C = 0u;
    uint32_t stage = 0u;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    bool indexed = false;
    bool alphaTestEnabled = false;
    bool alphaPayloadComplete = false;
    bool blockerGatePassed = false;
    bool blockerClassified = false;
    bool producerAccepted = false;
    ExplicitGeosetDataSidecar geosetSidecar = {};
  };

  struct Evidence {
    Mode requestedMode = Mode::Off;
    Mode effectiveMode = Mode::Off;
    Disposition disposition = Disposition::ModeOff;
    uint64_t frameSerial = 0u;
    uint64_t policyRevision = 0u;
    uint64_t mapEpoch = 0u;
    uint64_t deviceEpoch = 0u;
    uint64_t sourceGeneration = 0u;
    uint64_t packageGeneration = 0u;
    uint64_t immutableModelGeneration = 0u;
    uint64_t immutableContentHash = 0u;
    uint64_t exactGeometryKeyHash = 0u;
    uintptr_t meshPayloadIdentity = 0u;
    uint32_t requestedConsumerMask = 0u;
    uint32_t eligibleConsumerMask = 0u;
    uint32_t wouldUseConsumerMask = 0u;
    uint32_t actualConsumerMask = 0u;
    uint32_t geosetVertexCount = 0u;
    bool blockerClassified = false;
    bool packageReady = false;
    bool fullyEquivalent = false;
    bool drawMutationAllowed = false;
    bool gpuBindingAllowed = false;
    bool commandRecordingAllowed = false;
    bool packagePublicationAllowed = false;
    bool provesCurrentGameMemory = false;
  };

  struct Diagnostics {
    Mode lastRequestedMode = Mode::Off;
    Mode lastEffectiveMode = Mode::Off;
    Disposition lastDisposition = Disposition::ModeOff;
    uint64_t observeCalls = 0u;
    uint64_t modeOff = 0u;
    uint64_t consumeDenied = 0u;
    uint64_t invalidWitness = 0u;
    uint64_t sourceGenerationExhausted = 0u;
    uint64_t explicitGeosetDataSidecarLookupFailed = 0u;
    uint64_t missingExplicitGeosetDataSidecar = 0u;
    uint64_t invalidExplicitGeosetDataSidecar = 0u;
    uint64_t recordedContentIdentityOnly = 0u;
    uint64_t acceptedBlockerClassified = 0u;
    uint64_t lastSourceGeneration = 0u;
    uint64_t elapsedTicksTotal = 0u;
    uint64_t elapsedTicksMaxCall = 0u;
    uint64_t elapsedTicksMaxFrame = 0u;
    uint64_t elapsedTicksLastCompletedFrame = 0u;
    uint64_t completedTimedFrames = 0u;
    uint64_t currentTimedFrameSerial = 0u;
    uint64_t currentTimedFrameTicks = 0u;
    uint64_t currentTimedFrameCalls = 0u;
  };

  Evidence observe(
      Mode requestedMode, const ExactSubmittedWitness& witness) noexcept;
  void noteElapsedTicks(uint64_t frameSerial, uint64_t ticks) noexcept;

  const Evidence& lastEvidence() const noexcept { return m_lastEvidence; }
  const Diagnostics& diagnostics() const noexcept { return m_diagnostics; }

private:
  static bool validExactSubmittedWitness(
      const ExactSubmittedWitness& witness) noexcept;
  static bool validExplicitSidecar(
      const ExactSubmittedWitness& witness) noexcept;
  static uint64_t issueCurrentStageSourceGeneration() noexcept;
  void rollTimedFrame(uint64_t frameSerial) noexcept;
  Evidence finish(Evidence evidence) noexcept;

  // This adapter and its diagnostics are owned by one D3D9 producer thread.
  // No caller may access an instance concurrently.
  Evidence m_lastEvidence = {};
  Diagnostics m_diagnostics = {};
};

static_assert(std::is_standard_layout_v<
    War3PersistentGpuPackageStage11ObserveAdapter::ExplicitGeosetDataSidecar>);
static_assert(std::is_trivially_copyable_v<
    War3PersistentGpuPackageStage11ObserveAdapter::ExplicitGeosetDataSidecar>);
static_assert(std::is_standard_layout_v<
    War3PersistentGpuPackageStage11ObserveAdapter::ExactSubmittedWitness>);
static_assert(std::is_trivially_copyable_v<
    War3PersistentGpuPackageStage11ObserveAdapter::ExactSubmittedWitness>);
static_assert(std::is_standard_layout_v<
    War3PersistentGpuPackageStage11ObserveAdapter::Evidence>);
static_assert(std::is_trivially_copyable_v<
    War3PersistentGpuPackageStage11ObserveAdapter::Evidence>);
static_assert(
    War3PersistentGpuPackageStage11ObserveAdapter::kRuntimeObserveIntegrated);
static_assert(War3PersistentGpuPackageStage11ObserveAdapter::kObserveOnly);
static_assert(
    !War3PersistentGpuPackageStage11ObserveAdapter::kConsumeAdmissionGranted);
static_assert(
    !War3PersistentGpuPackageStage11ObserveAdapter::kBindsGpuResources);
static_assert(
    !War3PersistentGpuPackageStage11ObserveAdapter::kRecordsCommands);
static_assert(
    !War3PersistentGpuPackageStage11ObserveAdapter::kMutatesCanonicalDraw);
static_assert(
    !War3PersistentGpuPackageStage11ObserveAdapter::kPublishesPackage);
static_assert(
    !War3PersistentGpuPackageStage11ObserveAdapter::
        kProvesCurrentGameMemory);

}  // namespace dxvk::war3::gpu_skin
