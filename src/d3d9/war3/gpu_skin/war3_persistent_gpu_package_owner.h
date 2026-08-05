#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace dxvk::war3::gpu_skin {

// This ledger is a value-only admission boundary for a future shared package
// owner.  It deliberately owns no atlas slice, Vulkan object or renderer
// callback.  The live renderer does not instantiate it yet.
class War3PersistentGpuPackageOwner final {
public:
  static constexpr bool kRuntimeObserveEnabled = false;
  static constexpr bool kObserveValidationOnly = true;
  static constexpr bool kObserveBindsAtlas = false;
  static constexpr bool kObserveWritesConsumerLastUse = false;
  static constexpr bool kSharedConsumerEnabled = false;

  enum ConsumerBits : uint32_t {
    ConsumerMain = 1u << 0,
    ConsumerCsm0 = 1u << 1,
    ConsumerCsm1 = 1u << 2,
    ConsumerCsm2 = 1u << 3,
    ConsumerCsm3 = 1u << 4,
    ConsumerPointShadow = 1u << 5,
    ConsumerOutline = 1u << 6,
  };

  static constexpr uint32_t kKnownConsumerMask =
      ConsumerMain | ConsumerCsm0 | ConsumerCsm1 | ConsumerCsm2 |
      ConsumerCsm3 | ConsumerPointShadow | ConsumerOutline;

  struct GenerationKey {
    uint64_t mapEpoch = 0u;
    uint64_t deviceEpoch = 0u;
    uint64_t packageGeneration = 0u;
  };

  struct FencePoint {
    // Opaque identity supplied by the future renderer-side fence adapter.
    // It must be stable for the lifetime of this generation, but this value-
    // only layer never dereferences it.
    uint64_t fenceIdentity = 0u;
    uint64_t value = 0u;
  };

  struct FenceObservation {
    uint64_t fenceIdentity = 0u;
    uint64_t completedValue = 0u;
    bool querySucceeded = false;
  };

  struct ConsumerLastUse {
    FencePoint fence;
    uint32_t consumerMask = 0u;
    uint64_t submitSerial = 0u;
  };

  struct GenerationRetirement {
    GenerationKey key;
    FencePoint uploadFence;
    ConsumerLastUse consumerLastUse;
    uint64_t retirementSerial = 0u;
    bool retirementRequested = false;
  };

  enum class MutationResult : uint32_t {
    Accepted = 0u,
    Duplicate,
    Invalid,
    NotFound,
    GenerationMismatch,
    Conflict,
    NonMonotonic,
    UploadProofMissing,
    ConsumerProofMissing,
    AlreadyRetired,
  };

  enum class ReclaimResult : uint32_t {
    Reclaimed = 0u,
    RetainedNotFound,
    RetainedGenerationMismatch,
    RetainedEpochActive,
    RetainedUploadProofMissing,
    RetainedConsumerProofMissing,
    RetainedObservationFailed,
    RetainedObservationInvalid,
    RetainedFenceMismatch,
    RetainedUploadPending,
    RetainedConsumerPending,
  };

  War3PersistentGpuPackageOwner() = default;
  War3PersistentGpuPackageOwner(
      const War3PersistentGpuPackageOwner&) = delete;
  War3PersistentGpuPackageOwner& operator=(
      const War3PersistentGpuPackageOwner&) = delete;
  War3PersistentGpuPackageOwner(
      War3PersistentGpuPackageOwner&&) = default;
  War3PersistentGpuPackageOwner& operator=(
      War3PersistentGpuPackageOwner&&) = default;

  static bool validateGenerationKey(const GenerationKey& key) noexcept;
  static bool validateFencePoint(const FencePoint& point) noexcept;
  static bool validateConsumerLastUse(
      const ConsumerLastUse& lastUse) noexcept;
  static bool validateFenceObservation(
      const FenceObservation& observation) noexcept;

  MutationResult registerGeneration(const GenerationKey& key);
  MutationResult publishUploadFence(
      const GenerationKey& key, const FencePoint& uploadFence);
  // Observe may call this classifier to audit the would-be last-use proof.
  // It is const and never publishes or advances a generation.
  MutationResult observeConsumerLastUseCandidate(
      const GenerationKey& key, const ConsumerLastUse& lastUse) const noexcept;
  MutationResult publishConsumerLastUse(
      const GenerationKey& key, const ConsumerLastUse& lastUse);
  MutationResult requestEpochRetirement(
      const GenerationKey& key, uint64_t retirementSerial);

  ReclaimResult tryReclaim(
      const GenerationKey& key,
      const FenceObservation& uploadObservation,
      const FenceObservation& consumerObservation);

  const GenerationRetirement* inspect(
      const GenerationKey& key) const noexcept;
  size_t size() const noexcept { return m_generations.size(); }

private:
  using GenerationMap =
      std::unordered_map<uint64_t, GenerationRetirement>;

  static bool sameKey(
      const GenerationKey& lhs, const GenerationKey& rhs) noexcept;
  GenerationMap::iterator findGeneration(const GenerationKey& key);
  GenerationMap::const_iterator findGeneration(
      const GenerationKey& key) const;

  GenerationMap m_generations;
};

static_assert(!War3PersistentGpuPackageOwner::kRuntimeObserveEnabled);
static_assert(War3PersistentGpuPackageOwner::kObserveValidationOnly);
static_assert(!War3PersistentGpuPackageOwner::kObserveBindsAtlas);
static_assert(!War3PersistentGpuPackageOwner::kObserveWritesConsumerLastUse);
static_assert(!War3PersistentGpuPackageOwner::kSharedConsumerEnabled);

}  // namespace dxvk::war3::gpu_skin
