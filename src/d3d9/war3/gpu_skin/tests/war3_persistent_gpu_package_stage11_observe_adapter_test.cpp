#include "../war3_persistent_gpu_package_stage11_observe_adapter.h"

#include <type_traits>

namespace gpu_skin = dxvk::war3::gpu_skin;
using Adapter = gpu_skin::War3PersistentGpuPackageStage11ObserveAdapter;

namespace {

int Check(bool condition, int code) {
  return condition ? 0 : code;
}

Adapter::ExactSubmittedWitness ValidWitness(bool withSidecar = true) {
  Adapter::ExactSubmittedWitness witness = {};
  witness.frameSerial = 101u;
  witness.exactSubmittedFrameSerial = 101u;
  witness.policyRevision = 7u;
  witness.mapEpoch = 3u;
  witness.deviceEpoch = 5u;
  witness.exactGeometryKeyHash = 11u;
  witness.instanceIdentity = 13u;
  witness.meshPayloadIdentity = 17u;
  witness.renderablePartIdentity = 19u;
  witness.jHandle = 23u;
  witness.layerIndex = 2u;
  witness.payloadWord108 = 29u;
  witness.payloadWord11C = 31u;
  witness.stage = Adapter::kRequiredStage;
  witness.vertexCount = 300u;
  witness.indexCount = 600u;
  witness.indexed = true;
  witness.alphaTestEnabled = true;
  witness.alphaPayloadComplete = true;
  witness.blockerGatePassed = true;
  witness.producerAccepted = true;
  witness.geosetSidecar.geosetDataIdentity = 17u;
  witness.geosetSidecar.contentHash = 37u;
  witness.geosetSidecar.mapEpoch = witness.mapEpoch;
  witness.geosetSidecar.immutableModelGeneration = 41u;
  witness.geosetSidecar.vertexCount = 300u;
  witness.geosetSidecar.found = withSidecar;
  return witness;
}

int TestModesAndHardCeilings() {
  Adapter adapter;
  const auto off = adapter.observe(Adapter::Mode::Off, ValidWitness());
  if (const int rc = Check(
          off.disposition == Adapter::Disposition::ModeOff &&
              off.sourceGeneration == 0u && off.packageGeneration == 0u,
          1))
    return rc;

  const auto consume = adapter.observe(
      Adapter::Mode::Consume, ValidWitness());
  if (const int rc = Check(
          consume.disposition == Adapter::Disposition::ConsumeNotAdmitted &&
              consume.effectiveMode == Adapter::Mode::Off &&
              consume.sourceGeneration == 0u,
          2))
    return rc;

  const auto observed = adapter.observe(
      Adapter::Mode::Observe, ValidWitness());
  if (const int rc = Check(
          observed.disposition ==
                  Adapter::Disposition::RecordedCurrentMapSource &&
              observed.effectiveMode == Adapter::Mode::Observe &&
              observed.packageGeneration == 0u &&
              observed.requestedConsumerMask ==
                  Adapter::kRequestedConsumerMask &&
              observed.eligibleConsumerMask == 0u &&
              observed.wouldUseConsumerMask == 0u &&
              observed.actualConsumerMask == 0u &&
              !observed.packageReady && !observed.fullyEquivalent &&
              !observed.drawMutationAllowed &&
              !observed.gpuBindingAllowed &&
              !observed.commandRecordingAllowed &&
              !observed.packagePublicationAllowed &&
              observed.provesCurrentGameMemory,
          3))
    return rc;
  return 0;
}

int TestExactWitnessAndExplicitSidecar() {
  Adapter adapter;
  auto witness = ValidWitness();
  witness.exactSubmittedFrameSerial += 1u;
  auto evidence = adapter.observe(Adapter::Mode::Observe, witness);
  if (const int rc = Check(
          evidence.disposition ==
                  Adapter::Disposition::InvalidExactSubmittedWitness &&
              evidence.sourceGeneration == 0u,
          10))
    return rc;

  witness = ValidWitness(false);
  evidence = adapter.observe(Adapter::Mode::Observe, witness);
  if (const int rc = Check(
          evidence.disposition == Adapter::Disposition::
              MissingExplicitGeosetDataSidecar &&
              evidence.sourceGeneration != 0u &&
              evidence.immutableModelGeneration == 0u,
          11))
    return rc;

  witness = ValidWitness(false);
  witness.geosetSidecar.lookupFailed = true;
  evidence = adapter.observe(Adapter::Mode::Observe, witness);
  if (const int rc = Check(
          evidence.disposition == Adapter::Disposition::
              ExplicitGeosetDataSidecarLookupFailed &&
              evidence.sourceGeneration != 0u &&
              adapter.diagnostics().
                  explicitGeosetDataSidecarLookupFailed == 1u,
          16))
    return rc;

  witness = ValidWitness();
  witness.geosetSidecar.mapEpoch += 1u;
  evidence = adapter.observe(Adapter::Mode::Observe, witness);
  if (const int rc = Check(
          evidence.disposition == Adapter::Disposition::
              StaleMapEpochExplicitGeosetDataSidecar &&
              !evidence.provesCurrentGameMemory &&
              adapter.diagnostics().
                  staleMapEpochExplicitGeosetDataSidecar == 1u,
          17))
    return rc;

  witness = ValidWitness();
  witness.geosetSidecar.geosetDataIdentity += 1u;
  evidence = adapter.observe(Adapter::Mode::Observe, witness);
  if (const int rc = Check(
          evidence.disposition == Adapter::Disposition::
              InvalidExplicitGeosetDataSidecar &&
              evidence.sourceGeneration != 0u,
          12))
    return rc;

  witness = ValidWitness();
  witness.blockerClassified = true;
  evidence = adapter.observe(Adapter::Mode::Observe, witness);
  if (const int rc = Check(
          evidence.disposition ==
                  Adapter::Disposition::RecordedCurrentMapSource &&
              evidence.blockerClassified &&
              adapter.diagnostics().acceptedBlockerClassified == 1u,
          13))
    return rc;

  witness = ValidWitness();
  witness.instanceIdentity = 0u;
  evidence = adapter.observe(Adapter::Mode::Observe, witness);
  if (const int rc = Check(
          evidence.disposition ==
              Adapter::Disposition::RecordedCurrentMapSource &&
              evidence.provesCurrentGameMemory,
          14))
    return rc;

  witness.jHandle = 0u;
  evidence = adapter.observe(Adapter::Mode::Observe, witness);
  if (const int rc = Check(
          evidence.disposition ==
              Adapter::Disposition::InvalidExactSubmittedWitness,
          15))
    return rc;
  return 0;
}

int TestProcessMonotonicSourceGeneration() {
  Adapter first;
  Adapter second;
  const auto a = first.observe(Adapter::Mode::Observe, ValidWitness());
  auto laterWitness = ValidWitness();
  laterWitness.frameSerial += 1u;
  laterWitness.exactSubmittedFrameSerial += 1u;
  const auto b = second.observe(Adapter::Mode::Observe, laterWitness);
  if (const int rc = Check(
          a.sourceGeneration != 0u &&
              b.sourceGeneration > a.sourceGeneration,
          20))
    return rc;
  return 0;
}

int TestTimingAggregation() {
  Adapter adapter;
  adapter.noteElapsedTicks(101u, 7u);
  adapter.noteElapsedTicks(101u, 11u);
  adapter.noteElapsedTicks(102u, 13u);
  const auto& diagnostics = adapter.diagnostics();
  if (const int rc = Check(
          diagnostics.elapsedTicksTotal == 31u &&
              diagnostics.elapsedTicksMaxCall == 13u &&
              diagnostics.elapsedTicksLastCompletedFrame == 18u &&
              diagnostics.elapsedTicksMaxFrame == 18u &&
              diagnostics.completedTimedFrames == 1u &&
              diagnostics.currentTimedFrameSerial == 102u &&
              diagnostics.currentTimedFrameTicks == 13u &&
              diagnostics.currentTimedFrameCalls == 1u,
          30))
    return rc;
  return 0;
}

}  // namespace

static_assert(std::is_default_constructible_v<Adapter>);
static_assert(Adapter::kRuntimeObserveIntegrated);
static_assert(Adapter::kObserveOnly);
static_assert(!Adapter::kConsumeAdmissionGranted);
static_assert(!Adapter::kBindsGpuResources);
static_assert(!Adapter::kRecordsCommands);
static_assert(!Adapter::kMutatesCanonicalDraw);
static_assert(!Adapter::kPublishesPackage);
static_assert(Adapter::kProvesCurrentGameMemory);
static_assert(
    Adapter::kRequestedConsumerMask ==
    (Adapter::ConsumerCsm0 | Adapter::ConsumerCsm1 |
     Adapter::ConsumerCsm2 | Adapter::ConsumerCsm3));

int main() {
  if (const int rc = TestModesAndHardCeilings())
    return rc;
  if (const int rc = TestExactWitnessAndExplicitSidecar())
    return rc;
  if (const int rc = TestProcessMonotonicSourceGeneration())
    return rc;
  if (const int rc = TestTimingAggregation())
    return rc;
  return 0;
}
