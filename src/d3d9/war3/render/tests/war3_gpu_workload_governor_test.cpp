#include "../war3_gpu_workload_governor.h"

#include <cstdint>
#include <iostream>
#include <limits>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
  if (condition)
    return;
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

using dxvk::war3::render::War3GpuWorkloadConsumer;
using dxvk::war3::render::War3GpuWorkloadCost;
using dxvk::war3::render::War3GpuWorkloadGovernor;
using dxvk::war3::render::War3GpuWorkloadLimits;
using dxvk::war3::render::War3GpuWorkloadRejectReason;
using dxvk::war3::render::War3GpuCanHoldPointShadowLastComplete;
using dxvk::war3::render::War3GpuPointShadowPublicationIdentity;

void TestCheckedArithmetic() {
  uint64_t result = 0u;
  Check(War3GpuWorkloadGovernor::checkedAdd(4u, 5u, result) && result == 9u,
        "checked add accepts finite sum");
  Check(!War3GpuWorkloadGovernor::checkedAdd(
            std::numeric_limits<uint64_t>::max(), 1u, result),
        "checked add rejects overflow");
  Check(War3GpuWorkloadGovernor::checkedMultiply(7u, 9u, result) &&
            result == 63u,
        "checked multiply accepts finite product");
  Check(!War3GpuWorkloadGovernor::checkedMultiply(
            std::numeric_limits<uint64_t>::max(), 2u, result),
        "checked multiply rejects overflow");

  War3GpuWorkloadCost cost = {};
  Check(War3GpuWorkloadGovernor::addRepeatedDraw(cost, 4u, 100u, 300u),
        "repeated draw cost builds");
  Check(cost.draws == 4u && cost.vertices == 400u && cost.indices == 1200u,
        "repeated draw charges all three dimensions");
  Check(!War3GpuWorkloadGovernor::addRepeatedDraw(
            cost, 2u, std::numeric_limits<uint64_t>::max(), 1u) &&
            !cost.valid,
        "draw-cost overflow poisons the complete request");
}

void TestAtomicReservation() {
  War3GpuWorkloadLimits limits = {};
  limits.maxDraws = 10u;
  limits.maxVertices = 1000u;
  limits.maxIndices = 3000u;
  War3GpuWorkloadGovernor governor;
  governor.beginFrame(42u, limits);

  War3GpuWorkloadCost csm = {8u, 800u, 2400u, true};
  Check(governor.tryReserve(War3GpuWorkloadConsumer::DirectionalCsm, 4u,
                            csm),
        "complete CSM reservation fits");
  const auto before = governor.diagnostics().used;

  War3GpuWorkloadCost point = {3u, 100u, 100u, true};
  Check(!governor.tryReserve(War3GpuWorkloadConsumer::PointShadow, 6u,
                             point),
        "whole point batch rejects on draw budget");
  const auto after = governor.diagnostics().used;
  Check(after.draws == before.draws && after.vertices == before.vertices &&
            after.indices == before.indices,
        "rejected multi-face batch consumes no partial budget");
  Check(governor.diagnostics().lastRejectReason ==
            static_cast<uint32_t>(War3GpuWorkloadRejectReason::DrawBudget),
        "draw rejection is diagnosable");
  governor.notePointShadowBudgetFallback(true);
  governor.notePointShadowBudgetFallback(false);
  Check(governor.diagnostics().pointLastCompleteHoldCount == 1u &&
            governor.diagnostics().pointPublicationInvalidatedCount == 1u,
        "point budget fallback outcome is diagnosable");
}

void TestProvisionalReservationRollback() {
  War3GpuWorkloadGovernor governor;
  governor.beginFrame(43u);
  const War3GpuWorkloadCost volume = {4u, 400u, 1200u, true};
  Check(governor.tryReserve(War3GpuWorkloadConsumer::VolumeSun, 4u,
                            volume),
        "provisional volume reservation fits");
  Check(governor.cancelReservation(War3GpuWorkloadConsumer::VolumeSun, 4u,
                                   volume),
        "unrecorded volume reservation rolls back");
  const auto& diagnostics = governor.diagnostics();
  const auto& volumeDiagnostics = diagnostics.consumers[
      static_cast<size_t>(War3GpuWorkloadConsumer::VolumeSun)];
  Check(diagnostics.used.draws == 0u && diagnostics.used.vertices == 0u &&
            diagnostics.used.indices == 0u,
        "rollback restores complete frame budget");
  Check(volumeDiagnostics.acceptedReservations == 0u &&
            volumeDiagnostics.acceptedItems == 0u &&
            volumeDiagnostics.rolledBackReservations == 1u,
        "rollback diagnostics distinguish provisional cancellation");
  Check(!governor.cancelReservation(War3GpuWorkloadConsumer::VolumeSun, 4u,
                                    volume),
        "same reservation cannot roll back twice");
}

void TestIndependentLimitsAndFrameReset() {
  War3GpuWorkloadLimits limits = {};
  limits.maxDraws = 100u;
  limits.maxVertices = 9u;
  limits.maxIndices = 100u;
  War3GpuWorkloadGovernor governor;
  governor.beginFrame(1u, limits);
  Check(!governor.tryReserve(War3GpuWorkloadConsumer::VolumeSun, 2u,
                             {2u, 10u, 0u, true}),
        "vertex budget independently rejects volume-sun");
  Check(governor.diagnostics().used.draws == 0u,
        "vertex rejection is transactional");

  limits.maxVertices = 100u;
  limits.maxIndices = 9u;
  governor.beginFrame(2u, limits);
  Check(!governor.tryReserve(War3GpuWorkloadConsumer::PointShadow, 1u,
                             {1u, 1u, 10u, true}),
        "index budget independently rejects a whole face");
  Check(governor.diagnostics().frameSerial == 2u &&
            governor.diagnostics().used.draws == 0u,
        "new frame clears prior reservations");
}

void TestInvalidAndOverflowRequests() {
  War3GpuWorkloadGovernor governor;
  governor.beginFrame(7u);
  Check(!governor.tryReserve(War3GpuWorkloadConsumer::PointShadow, 0u,
                             {1u, 1u, 1u, true}),
        "zero-item reservation rejects");
  Check(!governor.tryReserve(War3GpuWorkloadConsumer::VolumeSun, 1u,
                             {1u, 1u, 1u, false}),
        "poisoned cost rejects");

  War3GpuWorkloadLimits huge = {};
  huge.maxDraws = std::numeric_limits<uint64_t>::max();
  huge.maxVertices = std::numeric_limits<uint64_t>::max();
  huge.maxIndices = std::numeric_limits<uint64_t>::max();
  governor.beginFrame(8u, huge);
  Check(governor.tryReserve(War3GpuWorkloadConsumer::DirectionalCsm, 1u,
                            {std::numeric_limits<uint64_t>::max(), 1u, 1u,
                             true}),
        "maximum finite first reservation is accepted");
  Check(!governor.tryReserve(War3GpuWorkloadConsumer::PointShadow, 1u,
                             {1u, 1u, 1u, true}),
        "used-cost overflow rejects before commit");
  Check(governor.diagnostics().used.draws ==
            std::numeric_limits<uint64_t>::max(),
        "overflow rejection preserves prior committed usage");
}

War3GpuPointShadowPublicationIdentity MakePointPublicationIdentity() {
  War3GpuPointShadowPublicationIdentity identity = {};
  identity.mapEpoch = 3u;
  identity.deviceEpoch = 5u;
  identity.resourceGeneration = 7u;
  identity.lightGeneration = 11u;
  identity.settingsRevision = 13u;
  identity.resolution = 1024u;
  identity.capacityLights = 2u;
  identity.lightCount = 1u;
  identity.complete = true;
  identity.lights[0] = {17, 100.0f, 200.0f, 300.0f, 512.0f, 0.75f};
  return identity;
}

void TestPointLastCompleteOwnership() {
  const auto published = MakePointPublicationIdentity();
  auto current = published;
  Check(War3GpuCanHoldPointShadowLastComplete(current, published),
        "exact complete point publication may survive budget rejection");

  current.lights[0].positionX = 101.0f;
  Check(!War3GpuCanHoldPointShadowLastComplete(current, published),
        "moved light invalidates last-complete point publication");
  current = published;
  ++current.resourceGeneration;
  Check(!War3GpuCanHoldPointShadowLastComplete(current, published),
        "resource replacement invalidates last-complete point publication");
  current = published;
  ++current.mapEpoch;
  Check(!War3GpuCanHoldPointShadowLastComplete(current, published),
        "map transition invalidates last-complete point publication");
  current = published;
  ++current.lightGeneration;
  Check(!War3GpuCanHoldPointShadowLastComplete(current, published),
        "light generation change invalidates last-complete publication");
  current = published;
  ++current.settingsRevision;
  Check(!War3GpuCanHoldPointShadowLastComplete(current, published),
        "settings change invalidates last-complete point publication");
  current = published;
  current.complete = false;
  Check(!War3GpuCanHoldPointShadowLastComplete(current, published),
        "partial current identity cannot retain a point publication");
  current = published;
  auto partialPublished = published;
  partialPublished.complete = false;
  Check(!War3GpuCanHoldPointShadowLastComplete(current, partialPublished),
        "partial cube is never retained as last-complete");
}

} // namespace

int main() {
  TestCheckedArithmetic();
  TestAtomicReservation();
  TestProvisionalReservationRollback();
  TestIndependentLimitsAndFrameReset();
  TestInvalidAndOverflowRequests();
  TestPointLastCompleteOwnership();
  if (g_failures != 0) {
    std::cerr << g_failures << " workload governor test(s) failed\n";
    return 1;
  }
  return 0;
}
