#include "war3_persistent_gpu_package_owner.h"

namespace dxvk::war3::gpu_skin {

bool War3PersistentGpuPackageOwner::validateGenerationKey(
    const GenerationKey& key) noexcept {
  return key.mapEpoch != 0u && key.deviceEpoch != 0u &&
      key.packageGeneration != 0u;
}

bool War3PersistentGpuPackageOwner::validateFencePoint(
    const FencePoint& point) noexcept {
  return point.fenceIdentity != 0u && point.value != 0u;
}

bool War3PersistentGpuPackageOwner::validateConsumerLastUse(
    const ConsumerLastUse& lastUse) noexcept {
  return validateFencePoint(lastUse.fence) &&
      lastUse.consumerMask != 0u &&
      (lastUse.consumerMask & ~kKnownConsumerMask) == 0u &&
      lastUse.submitSerial != 0u;
}

bool War3PersistentGpuPackageOwner::validateFenceObservation(
    const FenceObservation& observation) noexcept {
  return observation.querySucceeded && observation.fenceIdentity != 0u &&
      observation.completedValue != 0u;
}

bool War3PersistentGpuPackageOwner::sameKey(
    const GenerationKey& lhs, const GenerationKey& rhs) noexcept {
  return lhs.mapEpoch == rhs.mapEpoch &&
      lhs.deviceEpoch == rhs.deviceEpoch &&
      lhs.packageGeneration == rhs.packageGeneration;
}

War3PersistentGpuPackageOwner::GenerationMap::iterator
War3PersistentGpuPackageOwner::findGeneration(const GenerationKey& key) {
  return m_generations.find(key.packageGeneration);
}

War3PersistentGpuPackageOwner::GenerationMap::const_iterator
War3PersistentGpuPackageOwner::findGeneration(
    const GenerationKey& key) const {
  return m_generations.find(key.packageGeneration);
}

War3PersistentGpuPackageOwner::MutationResult
War3PersistentGpuPackageOwner::registerGeneration(
    const GenerationKey& key) {
  if (!validateGenerationKey(key))
    return MutationResult::Invalid;

  const auto found = findGeneration(key);
  if (found != m_generations.end()) {
    return sameKey(found->second.key, key)
        ? MutationResult::Duplicate
        : MutationResult::GenerationMismatch;
  }

  GenerationRetirement retirement;
  retirement.key = key;
  m_generations.emplace(key.packageGeneration, retirement);
  return MutationResult::Accepted;
}

War3PersistentGpuPackageOwner::MutationResult
War3PersistentGpuPackageOwner::publishUploadFence(
    const GenerationKey& key, const FencePoint& uploadFence) {
  if (!validateGenerationKey(key) || !validateFencePoint(uploadFence))
    return MutationResult::Invalid;

  const auto found = findGeneration(key);
  if (found == m_generations.end())
    return MutationResult::NotFound;
  if (!sameKey(found->second.key, key))
    return MutationResult::GenerationMismatch;
  if (found->second.retirementRequested)
    return MutationResult::AlreadyRetired;

  FencePoint& stored = found->second.uploadFence;
  if (!validateFencePoint(stored)) {
    stored = uploadFence;
    return MutationResult::Accepted;
  }
  if (stored.fenceIdentity == uploadFence.fenceIdentity &&
      stored.value == uploadFence.value)
    return MutationResult::Duplicate;
  return MutationResult::Conflict;
}

War3PersistentGpuPackageOwner::MutationResult
War3PersistentGpuPackageOwner::observeConsumerLastUseCandidate(
    const GenerationKey& key, const ConsumerLastUse& lastUse) const noexcept {
  if (!validateGenerationKey(key) || !validateConsumerLastUse(lastUse))
    return MutationResult::Invalid;

  const auto found = findGeneration(key);
  if (found == m_generations.end())
    return MutationResult::NotFound;
  if (!sameKey(found->second.key, key))
    return MutationResult::GenerationMismatch;
  if (found->second.retirementRequested)
    return MutationResult::AlreadyRetired;

  const ConsumerLastUse& stored = found->second.consumerLastUse;
  if (!validateConsumerLastUse(stored))
    return MutationResult::Accepted;
  if (stored.fence.fenceIdentity != lastUse.fence.fenceIdentity)
    return MutationResult::Conflict;

  if (stored.submitSerial == lastUse.submitSerial &&
      stored.fence.value == lastUse.fence.value) {
    const uint32_t combinedMask = stored.consumerMask | lastUse.consumerMask;
    if (combinedMask == stored.consumerMask)
      return MutationResult::Duplicate;
    return MutationResult::Accepted;
  }

  if (lastUse.submitSerial <= stored.submitSerial ||
      lastUse.fence.value <= stored.fence.value)
    return MutationResult::NonMonotonic;

  return MutationResult::Accepted;
}

War3PersistentGpuPackageOwner::MutationResult
War3PersistentGpuPackageOwner::publishConsumerLastUse(
    const GenerationKey& key, const ConsumerLastUse& lastUse) {
  const MutationResult admission =
      observeConsumerLastUseCandidate(key, lastUse);
  if (admission != MutationResult::Accepted)
    return admission;

  const auto found = findGeneration(key);
  ConsumerLastUse& stored = found->second.consumerLastUse;
  if (!validateConsumerLastUse(stored)) {
    stored = lastUse;
    return MutationResult::Accepted;
  }

  if (stored.submitSerial == lastUse.submitSerial &&
      stored.fence.value == lastUse.fence.value) {
    stored.consumerMask |= lastUse.consumerMask;
    return MutationResult::Accepted;
  }

  stored.fence = lastUse.fence;
  stored.consumerMask |= lastUse.consumerMask;
  stored.submitSerial = lastUse.submitSerial;
  return MutationResult::Accepted;
}

War3PersistentGpuPackageOwner::MutationResult
War3PersistentGpuPackageOwner::requestEpochRetirement(
    const GenerationKey& key, uint64_t retirementSerial) {
  if (!validateGenerationKey(key) || retirementSerial == 0u)
    return MutationResult::Invalid;

  const auto found = findGeneration(key);
  if (found == m_generations.end())
    return MutationResult::NotFound;
  if (!sameKey(found->second.key, key))
    return MutationResult::GenerationMismatch;

  GenerationRetirement& retirement = found->second;
  if (retirement.retirementRequested) {
    return retirement.retirementSerial == retirementSerial
        ? MutationResult::Duplicate
        : MutationResult::AlreadyRetired;
  }
  if (!validateFencePoint(retirement.uploadFence))
    return MutationResult::UploadProofMissing;
  if (!validateConsumerLastUse(retirement.consumerLastUse))
    return MutationResult::ConsumerProofMissing;
  if (retirementSerial < retirement.consumerLastUse.submitSerial)
    return MutationResult::NonMonotonic;

  retirement.retirementSerial = retirementSerial;
  retirement.retirementRequested = true;
  return MutationResult::Accepted;
}

War3PersistentGpuPackageOwner::ReclaimResult
War3PersistentGpuPackageOwner::tryReclaim(
    const GenerationKey& key,
    const FenceObservation& uploadObservation,
    const FenceObservation& consumerObservation) {
  if (!validateGenerationKey(key))
    return ReclaimResult::RetainedGenerationMismatch;

  const auto found = findGeneration(key);
  if (found == m_generations.end())
    return ReclaimResult::RetainedNotFound;
  if (!sameKey(found->second.key, key))
    return ReclaimResult::RetainedGenerationMismatch;

  const GenerationRetirement& retirement = found->second;
  if (!retirement.retirementRequested)
    return ReclaimResult::RetainedEpochActive;
  if (!validateFencePoint(retirement.uploadFence))
    return ReclaimResult::RetainedUploadProofMissing;
  if (!validateConsumerLastUse(retirement.consumerLastUse))
    return ReclaimResult::RetainedConsumerProofMissing;
  if (!uploadObservation.querySucceeded ||
      !consumerObservation.querySucceeded)
    return ReclaimResult::RetainedObservationFailed;
  if (!validateFenceObservation(uploadObservation) ||
      !validateFenceObservation(consumerObservation))
    return ReclaimResult::RetainedObservationInvalid;
  if (uploadObservation.fenceIdentity !=
          retirement.uploadFence.fenceIdentity ||
      consumerObservation.fenceIdentity !=
          retirement.consumerLastUse.fence.fenceIdentity)
    return ReclaimResult::RetainedFenceMismatch;
  if (uploadObservation.completedValue < retirement.uploadFence.value)
    return ReclaimResult::RetainedUploadPending;
  if (consumerObservation.completedValue <
      retirement.consumerLastUse.fence.value)
    return ReclaimResult::RetainedConsumerPending;

  m_generations.erase(found);
  return ReclaimResult::Reclaimed;
}

const War3PersistentGpuPackageOwner::GenerationRetirement*
War3PersistentGpuPackageOwner::inspect(
    const GenerationKey& key) const noexcept {
  if (!validateGenerationKey(key))
    return nullptr;
  const auto found = findGeneration(key);
  if (found == m_generations.end() || !sameKey(found->second.key, key))
    return nullptr;
  return &found->second;
}

}  // namespace dxvk::war3::gpu_skin
