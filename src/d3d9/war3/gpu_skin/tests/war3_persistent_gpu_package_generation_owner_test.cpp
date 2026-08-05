#include "../war3_persistent_gpu_package_owner.h"

#include <type_traits>

namespace gpu_skin = dxvk::war3::gpu_skin;

using Owner = gpu_skin::War3PersistentGpuPackageOwner;
using Mutation = Owner::MutationResult;
using Reclaim = Owner::ReclaimResult;

namespace {

int Check(bool condition, int code) {
  return condition ? 0 : code;
}

}  // namespace

static_assert(std::is_default_constructible_v<Owner>);
static_assert(!std::is_copy_constructible_v<Owner>);
static_assert(!std::is_copy_assignable_v<Owner>);
static_assert(!Owner::kRuntimeObserveEnabled);
static_assert(Owner::kObserveValidationOnly);
static_assert(!Owner::kObserveBindsAtlas);
static_assert(!Owner::kObserveWritesConsumerLastUse);
static_assert(!Owner::kSharedConsumerEnabled);

int main() {
  Owner owner;
  const Owner::GenerationKey key{11u, 22u, 33u};
  const Owner::GenerationKey wrongEpoch{12u, 22u, 33u};
  const Owner::GenerationKey second{11u, 22u, 34u};
  const Owner::FencePoint upload{101u, 7u};
  const Owner::ConsumerLastUse mainUse{
      {202u, 9u}, Owner::ConsumerMain, 90u};
  const Owner::ConsumerLastUse sameSubmitShadow{
      {202u, 9u}, Owner::ConsumerCsm0, 90u};
  const Owner::ConsumerLastUse laterUse{
      {202u, 12u}, Owner::ConsumerPointShadow, 120u};

  if (const int rc = Check(
          owner.registerGeneration({0u, 22u, 33u}) == Mutation::Invalid, 1))
    return rc;
  if (const int rc = Check(
          owner.registerGeneration(key) == Mutation::Accepted, 2))
    return rc;
  if (const int rc = Check(
          owner.registerGeneration(key) == Mutation::Duplicate, 3))
    return rc;
  if (const int rc = Check(
          owner.registerGeneration(wrongEpoch) ==
              Mutation::GenerationMismatch, 4))
    return rc;
  if (const int rc = Check(owner.size() == 1u, 5))
    return rc;

  if (const int rc = Check(
          owner.requestEpochRetirement(key, 130u) ==
              Mutation::UploadProofMissing, 6))
    return rc;
  if (const int rc = Check(
          owner.publishUploadFence(key, {101u, 0u}) == Mutation::Invalid, 7))
    return rc;
  if (const int rc = Check(
          owner.publishUploadFence(key, upload) == Mutation::Accepted, 8))
    return rc;
  if (const int rc = Check(
          owner.publishUploadFence(key, upload) == Mutation::Duplicate, 9))
    return rc;
  if (const int rc = Check(
          owner.publishUploadFence(key, {101u, 8u}) == Mutation::Conflict, 10))
    return rc;
  if (const int rc = Check(
          owner.requestEpochRetirement(key, 130u) ==
              Mutation::ConsumerProofMissing, 11))
    return rc;

  if (const int rc = Check(
          owner.observeConsumerLastUseCandidate(key, mainUse) ==
              Mutation::Accepted, 42))
    return rc;
  const Owner::GenerationRetirement* observedOnly = owner.inspect(key);
  if (const int rc = Check(
          observedOnly != nullptr &&
              observedOnly->consumerLastUse.submitSerial == 0u &&
              observedOnly->consumerLastUse.consumerMask == 0u, 43))
    return rc;

  if (const int rc = Check(
          owner.publishConsumerLastUse(
              key, {{202u, 9u}, 0u, 90u}) == Mutation::Invalid, 12))
    return rc;
  if (const int rc = Check(
          owner.publishConsumerLastUse(
              key, {{202u, 9u}, 1u << 30, 90u}) == Mutation::Invalid, 13))
    return rc;
  if (const int rc = Check(
          owner.publishConsumerLastUse(key, mainUse) == Mutation::Accepted, 14))
    return rc;
  if (const int rc = Check(
          owner.publishConsumerLastUse(key, mainUse) == Mutation::Duplicate, 15))
    return rc;
  if (const int rc = Check(
          owner.publishConsumerLastUse(key, sameSubmitShadow) ==
              Mutation::Accepted, 16))
    return rc;
  if (const int rc = Check(
          owner.publishConsumerLastUse(
              key, {{202u, 10u}, Owner::ConsumerOutline, 89u}) ==
              Mutation::NonMonotonic, 17))
    return rc;
  if (const int rc = Check(
          owner.publishConsumerLastUse(
              key, {{303u, 12u}, Owner::ConsumerOutline, 120u}) ==
              Mutation::Conflict, 18))
    return rc;
  if (const int rc = Check(
          owner.publishConsumerLastUse(key, laterUse) == Mutation::Accepted,
          19))
    return rc;

  const Owner::GenerationRetirement* record = owner.inspect(key);
  if (const int rc = Check(record != nullptr, 20))
    return rc;
  if (const int rc = Check(
          record->uploadFence.fenceIdentity == 101u &&
              record->uploadFence.value == 7u, 21))
    return rc;
  const uint32_t expectedMask = Owner::ConsumerMain | Owner::ConsumerCsm0 |
      Owner::ConsumerPointShadow;
  if (const int rc = Check(
          record->consumerLastUse.consumerMask == expectedMask &&
              record->consumerLastUse.submitSerial == 120u &&
              record->consumerLastUse.fence.value == 12u, 22))
    return rc;

  if (const int rc = Check(
          owner.requestEpochRetirement(key, 0u) == Mutation::Invalid, 23))
    return rc;
  if (const int rc = Check(
          owner.requestEpochRetirement(key, 119u) ==
              Mutation::NonMonotonic, 24))
    return rc;
  if (const int rc = Check(
          owner.requestEpochRetirement(key, 130u) == Mutation::Accepted, 25))
    return rc;
  if (const int rc = Check(
          owner.requestEpochRetirement(key, 130u) == Mutation::Duplicate, 26))
    return rc;
  if (const int rc = Check(
          owner.publishConsumerLastUse(
              key, {{202u, 13u}, Owner::ConsumerOutline, 131u}) ==
              Mutation::AlreadyRetired, 27))
    return rc;

  if (const int rc = Check(
          owner.tryReclaim(
              wrongEpoch, {101u, 7u, true}, {202u, 12u, true}) ==
              Reclaim::RetainedGenerationMismatch, 28))
    return rc;
  if (const int rc = Check(owner.size() == 1u, 29))
    return rc;
  if (const int rc = Check(
          owner.tryReclaim(
              key, {101u, 7u, false}, {202u, 12u, true}) ==
              Reclaim::RetainedObservationFailed, 30))
    return rc;
  if (const int rc = Check(
          owner.tryReclaim(
              key, {101u, 0u, true}, {202u, 12u, true}) ==
              Reclaim::RetainedObservationInvalid, 31))
    return rc;
  if (const int rc = Check(
          owner.tryReclaim(
              key, {404u, 7u, true}, {202u, 12u, true}) ==
              Reclaim::RetainedFenceMismatch, 32))
    return rc;
  if (const int rc = Check(
          owner.tryReclaim(
              key, {101u, 6u, true}, {202u, 12u, true}) ==
              Reclaim::RetainedUploadPending, 33))
    return rc;
  if (const int rc = Check(
          owner.tryReclaim(
              key, {101u, 7u, true}, {202u, 11u, true}) ==
              Reclaim::RetainedConsumerPending, 34))
    return rc;
  if (const int rc = Check(owner.size() == 1u, 35))
    return rc;

  if (const int rc = Check(
          owner.tryReclaim(
              key, {101u, 7u, true}, {202u, 12u, true}) ==
              Reclaim::Reclaimed, 36))
    return rc;
  if (const int rc = Check(owner.size() == 0u, 37))
    return rc;
  if (const int rc = Check(
          owner.tryReclaim(
              key, {101u, 7u, true}, {202u, 12u, true}) ==
              Reclaim::RetainedNotFound, 38))
    return rc;

  if (const int rc = Check(
          owner.registerGeneration(second) == Mutation::Accepted, 39))
    return rc;
  if (const int rc = Check(
          owner.tryReclaim(
              second, {101u, 7u, true}, {202u, 12u, true}) ==
              Reclaim::RetainedEpochActive, 40))
    return rc;
  if (const int rc = Check(owner.size() == 1u, 41))
    return rc;

  return 0;
}
