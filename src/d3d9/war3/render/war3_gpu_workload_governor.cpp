#include "war3_gpu_workload_governor.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace dxvk::war3::render {

namespace {

bool AddCost(const War3GpuWorkloadCost& lhs,
             const War3GpuWorkloadCost& rhs,
             War3GpuWorkloadCost& result) noexcept {
  result = {};
  if (!lhs.valid || !rhs.valid ||
      !War3GpuWorkloadGovernor::checkedAdd(lhs.draws, rhs.draws,
                                           result.draws) ||
      !War3GpuWorkloadGovernor::checkedAdd(lhs.vertices, rhs.vertices,
                                           result.vertices) ||
      !War3GpuWorkloadGovernor::checkedAdd(lhs.indices, rhs.indices,
                                           result.indices)) {
    result = {};
    result.valid = false;
    return false;
  }
  return true;
}

bool ExactF32(float lhs, float rhs) noexcept {
  uint32_t lhsBits = 0u;
  uint32_t rhsBits = 0u;
  static_assert(sizeof(lhsBits) == sizeof(lhs));
  std::memcpy(&lhsBits, &lhs, sizeof(lhsBits));
  std::memcpy(&rhsBits, &rhs, sizeof(rhsBits));
  return lhsBits == rhsBits;
}

bool ValidPointLightIdentity(
    const War3GpuPointShadowLightIdentity& light) noexcept {
  return light.id != 0 && std::isfinite(light.positionX) &&
      std::isfinite(light.positionY) && std::isfinite(light.positionZ) &&
      std::isfinite(light.range) && light.range > 0.0f &&
      std::isfinite(light.shadowIntensity);
}

bool ExactPointLightIdentity(
    const War3GpuPointShadowLightIdentity& lhs,
    const War3GpuPointShadowLightIdentity& rhs) noexcept {
  return lhs.id == rhs.id && ExactF32(lhs.positionX, rhs.positionX) &&
      ExactF32(lhs.positionY, rhs.positionY) &&
      ExactF32(lhs.positionZ, rhs.positionZ) &&
      ExactF32(lhs.range, rhs.range) &&
      ExactF32(lhs.shadowIntensity, rhs.shadowIntensity);
}

} // namespace

bool War3GpuCanHoldPointShadowLastComplete(
    const War3GpuPointShadowPublicationIdentity& current,
    const War3GpuPointShadowPublicationIdentity& published) noexcept {
  const auto valid = [](const War3GpuPointShadowPublicationIdentity& value) {
    return value.complete && value.mapEpoch != 0u &&
        value.deviceEpoch != 0u && value.resourceGeneration != 0u &&
        value.lightGeneration != 0u && value.settingsRevision != 0u &&
        value.resolution != 0u && value.capacityLights != 0u &&
        value.lightCount != 0u &&
        value.lightCount <= War3GpuPointShadowPublicationIdentity::kMaxLights &&
        value.lightCount <= value.capacityLights;
  };
  if (!valid(current) || !valid(published) ||
      current.mapEpoch != published.mapEpoch ||
      current.deviceEpoch != published.deviceEpoch ||
      current.resourceGeneration != published.resourceGeneration ||
      current.lightGeneration != published.lightGeneration ||
      current.settingsRevision != published.settingsRevision ||
      current.resolution != published.resolution ||
      current.capacityLights != published.capacityLights ||
      current.lightCount != published.lightCount)
    return false;

  for (uint32_t index = 0u; index < current.lightCount; ++index) {
    if (!ValidPointLightIdentity(current.lights[index]) ||
        !ValidPointLightIdentity(published.lights[index]) ||
        !ExactPointLightIdentity(current.lights[index], published.lights[index]))
      return false;
  }
  return true;
}

War3GpuWorkloadGovernor::War3GpuWorkloadGovernor(
    const War3GpuWorkloadLimits& limits) noexcept {
  beginFrame(0u, limits);
}

void War3GpuWorkloadGovernor::beginFrame(
    uint64_t frameSerial, const War3GpuWorkloadLimits& limits) noexcept {
  m_diagnostics = {};
  m_diagnostics.frameSerial = frameSerial;
  m_diagnostics.limits = limits;
}

size_t War3GpuWorkloadGovernor::consumerIndex(
    War3GpuWorkloadConsumer consumer) noexcept {
  return static_cast<size_t>(consumer);
}

void War3GpuWorkloadGovernor::reject(
    War3GpuWorkloadConsumer consumer,
    War3GpuWorkloadRejectReason reason) noexcept {
  const size_t index = consumerIndex(consumer);
  if (index < m_diagnostics.consumers.size())
    ++m_diagnostics.consumers[index].rejectedReservations;
  m_diagnostics.lastRejectedConsumer = static_cast<uint32_t>(consumer);
  m_diagnostics.lastRejectReason = static_cast<uint32_t>(reason);
  if (reason == War3GpuWorkloadRejectReason::ArithmeticOverflow)
    ++m_diagnostics.arithmeticOverflowRejectCount;
}

bool War3GpuWorkloadGovernor::tryReserve(
    War3GpuWorkloadConsumer consumer, uint64_t itemCount,
    const War3GpuWorkloadCost& cost) noexcept {
  const size_t index = consumerIndex(consumer);
  if (index >= m_diagnostics.consumers.size()) {
    reject(consumer, War3GpuWorkloadRejectReason::InvalidRequest);
    return false;
  }

  auto& consumerDiagnostics = m_diagnostics.consumers[index];
  War3GpuWorkloadCost requested = {};
  if (!AddCost(consumerDiagnostics.requested, cost, requested)) {
    consumerDiagnostics.requested.valid = false;
    reject(consumer, War3GpuWorkloadRejectReason::ArithmeticOverflow);
    return false;
  }
  consumerDiagnostics.requested = requested;
  if (!checkedAdd(consumerDiagnostics.requestedItems, itemCount,
                  consumerDiagnostics.requestedItems)) {
    reject(consumer, War3GpuWorkloadRejectReason::ArithmeticOverflow);
    return false;
  }

  if (m_diagnostics.frameSerial == 0u ||
      !m_diagnostics.limits.valid()) {
    reject(consumer, War3GpuWorkloadRejectReason::InvalidFrame);
    return false;
  }
  if (!cost.valid || itemCount == 0u) {
    reject(consumer, War3GpuWorkloadRejectReason::InvalidRequest);
    return false;
  }

  War3GpuWorkloadCost candidate = {};
  if (!AddCost(m_diagnostics.used, cost, candidate)) {
    reject(consumer, War3GpuWorkloadRejectReason::ArithmeticOverflow);
    return false;
  }
  if (candidate.draws > m_diagnostics.limits.maxDraws) {
    reject(consumer, War3GpuWorkloadRejectReason::DrawBudget);
    return false;
  }
  if (candidate.vertices > m_diagnostics.limits.maxVertices) {
    reject(consumer, War3GpuWorkloadRejectReason::VertexBudget);
    return false;
  }
  if (candidate.indices > m_diagnostics.limits.maxIndices) {
    reject(consumer, War3GpuWorkloadRejectReason::IndexBudget);
    return false;
  }

  War3GpuWorkloadCost accepted = {};
  if (!AddCost(consumerDiagnostics.accepted, cost, accepted)) {
    reject(consumer, War3GpuWorkloadRejectReason::ArithmeticOverflow);
    return false;
  }
  uint64_t acceptedItems = 0u;
  if (!checkedAdd(consumerDiagnostics.acceptedItems, itemCount,
                  acceptedItems)) {
    reject(consumer, War3GpuWorkloadRejectReason::ArithmeticOverflow);
    return false;
  }

  // Commit only after every dimension and diagnostic counter is proven.
  m_diagnostics.used = candidate;
  consumerDiagnostics.accepted = accepted;
  consumerDiagnostics.acceptedItems = acceptedItems;
  ++consumerDiagnostics.acceptedReservations;
  return true;
}

void War3GpuWorkloadGovernor::notePointShadowBudgetFallback(
    bool heldLastComplete) noexcept {
  if (heldLastComplete)
    ++m_diagnostics.pointLastCompleteHoldCount;
  else
    ++m_diagnostics.pointPublicationInvalidatedCount;
}

bool War3GpuWorkloadGovernor::checkedAdd(uint64_t lhs, uint64_t rhs,
                                         uint64_t& result) noexcept {
  if (rhs > std::numeric_limits<uint64_t>::max() - lhs)
    return false;
  result = lhs + rhs;
  return true;
}

bool War3GpuWorkloadGovernor::checkedMultiply(
    uint64_t lhs, uint64_t rhs, uint64_t& result) noexcept {
  if (lhs != 0u && rhs > std::numeric_limits<uint64_t>::max() / lhs)
    return false;
  result = lhs * rhs;
  return true;
}

bool War3GpuWorkloadGovernor::addRepeatedDraw(
    War3GpuWorkloadCost& total, uint64_t repeatCount,
    uint64_t vertexCount, uint64_t indexCount) noexcept {
  if (!total.valid || repeatCount == 0u) {
    total.valid = false;
    return false;
  }

  War3GpuWorkloadCost delta = {};
  if (!checkedMultiply(1u, repeatCount, delta.draws) ||
      !checkedMultiply(vertexCount, repeatCount, delta.vertices) ||
      !checkedMultiply(indexCount, repeatCount, delta.indices)) {
    total.valid = false;
    return false;
  }

  War3GpuWorkloadCost sum = {};
  if (!AddCost(total, delta, sum)) {
    total.valid = false;
    return false;
  }
  total = sum;
  return true;
}

} // namespace dxvk::war3::render
