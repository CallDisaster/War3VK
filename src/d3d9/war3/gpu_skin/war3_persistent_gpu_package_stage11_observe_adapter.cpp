#include "war3_persistent_gpu_package_stage11_observe_adapter.h"

#include <algorithm>
#include <atomic>
#include <limits>

namespace dxvk::war3::gpu_skin {

namespace {

// This issuer belongs to the process, not to a device, map, frame, cache, or
// adapter instance.  A device rebuild therefore cannot make an old Stage11
// source-generation value current again.
std::atomic<uint64_t> g_nextCurrentStageSourceGeneration{1u};

}  // namespace

bool War3PersistentGpuPackageStage11ObserveAdapter::
validExactSubmittedWitness(
    const ExactSubmittedWitness& witness) noexcept {
  return witness.frameSerial != 0u &&
      witness.exactSubmittedFrameSerial == witness.frameSerial &&
      witness.policyRevision != 0u && witness.mapEpoch != 0u &&
      witness.deviceEpoch != 0u && witness.stage == kRequiredStage &&
      witness.exactGeometryKeyHash != 0u &&
      (witness.instanceIdentity != 0u || witness.jHandle != 0u) &&
      witness.meshPayloadIdentity != 0u &&
      witness.renderablePartIdentity != 0u && witness.vertexCount != 0u &&
      (!witness.indexed || witness.indexCount != 0u) &&
      (!witness.alphaTestEnabled || witness.alphaPayloadComplete) &&
      witness.blockerGatePassed && witness.producerAccepted;
}

bool War3PersistentGpuPackageStage11ObserveAdapter::validExplicitSidecar(
    const ExactSubmittedWitness& witness) noexcept {
  const ExplicitGeosetDataSidecar& sidecar = witness.geosetSidecar;
  return sidecar.found && sidecar.geosetDataIdentity != 0u &&
      sidecar.geosetDataIdentity == witness.meshPayloadIdentity &&
      sidecar.contentHash != 0u && sidecar.immutableModelGeneration != 0u &&
      sidecar.vertexCount != 0u;
}

uint64_t War3PersistentGpuPackageStage11ObserveAdapter::
issueCurrentStageSourceGeneration() noexcept {
  uint64_t candidate = g_nextCurrentStageSourceGeneration.load(
      std::memory_order_relaxed);
  while (candidate != 0u &&
         candidate != (std::numeric_limits<uint64_t>::max)()) {
    if (g_nextCurrentStageSourceGeneration.compare_exchange_weak(
            candidate, candidate + 1u, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      return candidate;
    }
  }
  return 0u;
}

void War3PersistentGpuPackageStage11ObserveAdapter::rollTimedFrame(
    uint64_t frameSerial) noexcept {
  if (frameSerial == 0u ||
      frameSerial == m_diagnostics.currentTimedFrameSerial) {
    return;
  }
  if (m_diagnostics.currentTimedFrameSerial != 0u) {
    m_diagnostics.elapsedTicksLastCompletedFrame =
        m_diagnostics.currentTimedFrameTicks;
    m_diagnostics.elapsedTicksMaxFrame = (std::max)(
        m_diagnostics.elapsedTicksMaxFrame,
        m_diagnostics.currentTimedFrameTicks);
    m_diagnostics.completedTimedFrames += 1u;
  }
  m_diagnostics.currentTimedFrameSerial = frameSerial;
  m_diagnostics.currentTimedFrameTicks = 0u;
  m_diagnostics.currentTimedFrameCalls = 0u;
}

War3PersistentGpuPackageStage11ObserveAdapter::Evidence
War3PersistentGpuPackageStage11ObserveAdapter::finish(
    Evidence evidence) noexcept {
  m_diagnostics.lastRequestedMode = evidence.requestedMode;
  m_diagnostics.lastEffectiveMode = evidence.effectiveMode;
  m_diagnostics.lastDisposition = evidence.disposition;
  m_lastEvidence = evidence;
  return evidence;
}

War3PersistentGpuPackageStage11ObserveAdapter::Evidence
War3PersistentGpuPackageStage11ObserveAdapter::observe(
    Mode requestedMode, const ExactSubmittedWitness& witness) noexcept {
  m_diagnostics.observeCalls += 1u;

  Evidence evidence = {};
  evidence.requestedMode = requestedMode;
  evidence.frameSerial = witness.frameSerial;
  evidence.policyRevision = witness.policyRevision;
  evidence.mapEpoch = witness.mapEpoch;
  evidence.deviceEpoch = witness.deviceEpoch;
  evidence.exactGeometryKeyHash = witness.exactGeometryKeyHash;
  evidence.meshPayloadIdentity = witness.meshPayloadIdentity;
  evidence.blockerClassified = witness.blockerClassified;

  // These are hard ceilings in the Observe adapter, not values inferred from
  // missing runtime infrastructure.
  evidence.packageGeneration = 0u;
  evidence.requestedConsumerMask = kRequestedConsumerMask;
  evidence.eligibleConsumerMask = 0u;
  evidence.wouldUseConsumerMask = 0u;
  evidence.actualConsumerMask = 0u;
  evidence.packageReady = false;
  evidence.fullyEquivalent = false;
  evidence.drawMutationAllowed = false;
  evidence.gpuBindingAllowed = false;
  evidence.commandRecordingAllowed = false;
  evidence.packagePublicationAllowed = false;
  evidence.provesCurrentGameMemory = false;

  if (requestedMode == Mode::Off) {
    evidence.disposition = Disposition::ModeOff;
    m_diagnostics.modeOff += 1u;
    return finish(evidence);
  }
  if (requestedMode != Mode::Observe) {
    evidence.disposition = Disposition::ConsumeNotAdmitted;
    m_diagnostics.consumeDenied += 1u;
    return finish(evidence);
  }

  evidence.effectiveMode = Mode::Observe;
  if (!validExactSubmittedWitness(witness)) {
    evidence.disposition = Disposition::InvalidExactSubmittedWitness;
    m_diagnostics.invalidWitness += 1u;
    return finish(evidence);
  }

  evidence.sourceGeneration = issueCurrentStageSourceGeneration();
  if (evidence.sourceGeneration == 0u) {
    evidence.disposition = Disposition::SourceGenerationExhausted;
    m_diagnostics.sourceGenerationExhausted += 1u;
    return finish(evidence);
  }
  m_diagnostics.lastSourceGeneration = evidence.sourceGeneration;
  if (witness.blockerClassified)
    m_diagnostics.acceptedBlockerClassified += 1u;

  if (witness.geosetSidecar.lookupFailed) {
    evidence.disposition =
        Disposition::ExplicitGeosetDataSidecarLookupFailed;
    m_diagnostics.explicitGeosetDataSidecarLookupFailed += 1u;
    return finish(evidence);
  }
  if (!witness.geosetSidecar.found) {
    evidence.disposition = Disposition::MissingExplicitGeosetDataSidecar;
    m_diagnostics.missingExplicitGeosetDataSidecar += 1u;
    return finish(evidence);
  }
  if (!validExplicitSidecar(witness)) {
    evidence.disposition = Disposition::InvalidExplicitGeosetDataSidecar;
    m_diagnostics.invalidExplicitGeosetDataSidecar += 1u;
    return finish(evidence);
  }

  evidence.immutableModelGeneration =
      witness.geosetSidecar.immutableModelGeneration;
  evidence.immutableContentHash = witness.geosetSidecar.contentHash;
  evidence.geosetVertexCount = witness.geosetSidecar.vertexCount;
  // This is deliberately weaker than a current-source proof. The process
  // cache may still contain a prior map's record at the same address.
  evidence.disposition = Disposition::RecordedContentIdentityOnly;
  m_diagnostics.recordedContentIdentityOnly += 1u;
  return finish(evidence);
}

void War3PersistentGpuPackageStage11ObserveAdapter::noteElapsedTicks(
    uint64_t frameSerial, uint64_t ticks) noexcept {
  if (frameSerial == 0u)
    return;
  rollTimedFrame(frameSerial);
  m_diagnostics.elapsedTicksTotal += ticks;
  m_diagnostics.elapsedTicksMaxCall =
      (std::max)(m_diagnostics.elapsedTicksMaxCall, ticks);
  m_diagnostics.currentTimedFrameTicks += ticks;
  m_diagnostics.currentTimedFrameCalls += 1u;
}

}  // namespace dxvk::war3::gpu_skin
