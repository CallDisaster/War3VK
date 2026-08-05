#include "war3_persistent_gpu_package_observer.h"

#include <algorithm>
#include <cmath>

namespace dxvk::war3::gpu_skin {

bool War3PersistentGpuPackageObserver::isSupportedMode(Mode mode) noexcept {
  return mode == Mode::Off || mode == Mode::Observe || mode == Mode::Consume;
}

bool War3PersistentGpuPackageObserver::isValidFrameContext(
    const FrameContext& context) noexcept {
  return context.frameSerial != 0u && context.policyRevision != 0u &&
      context.stage == kRequiredStage && context.mapEpoch != 0u &&
      context.deviceEpoch != 0u &&
      context.catalogInstanceGeneration != 0u &&
      context.catalogSnapshotRevision != 0u;
}

bool War3PersistentGpuPackageObserver::isFiniteBounds(
    const Bounds& bounds) noexcept {
  return std::isfinite(bounds.minX) && std::isfinite(bounds.minY) &&
      std::isfinite(bounds.minZ) && std::isfinite(bounds.maxX) &&
      std::isfinite(bounds.maxY) && std::isfinite(bounds.maxZ) &&
      bounds.minX <= bounds.maxX && bounds.minY <= bounds.maxY &&
      bounds.minZ <= bounds.maxZ;
}

bool War3PersistentGpuPackageObserver::exactFrameKey(
    const FrameContext& context, const SealInput& input) noexcept {
  return input.frameSerial == context.frameSerial &&
      input.policyRevision == context.policyRevision &&
      input.stage == context.stage && input.mapEpoch == context.mapEpoch &&
      input.deviceEpoch == context.deviceEpoch &&
      input.catalogInstanceGeneration ==
          context.catalogInstanceGeneration &&
      input.catalogSnapshotRevision == context.catalogSnapshotRevision;
}

bool War3PersistentGpuPackageObserver::exactPackageDecision(
    const SealInput& input) noexcept {
  return input.packageGeneration != 0u &&
      input.immutableModelGeneration != 0u &&
      input.currentDrawSourceGeneration != 0u &&
      input.packageKey.mapEpoch == input.mapEpoch &&
      input.packageKey.deviceEpoch == input.deviceEpoch &&
      input.packageKey.packageGeneration == input.packageGeneration &&
      input.packageKey.immutableModelGeneration ==
          input.immutableModelGeneration &&
      input.packageContentDecision.matches(
          input.packageKey, input.catalogInstanceGeneration,
          input.catalogSnapshotRevision,
          input.frameSerial, input.policyRevision, input.stage,
          input.identityToken, input.sourceToken,
          input.currentDrawSourceGeneration,
          input.materialToken, input.alphaToken,
          input.worldToken, input.boundsToken);
}

bool War3PersistentGpuPackageObserver::isKnownSingleConsumerBit(
    uint32_t consumerBit) noexcept {
  return consumerBit != 0u &&
      (consumerBit & (consumerBit - 1u)) == 0u &&
      (consumerBit & ~kKnownConsumerMask) == 0u;
}

War3PersistentGpuPackageObserver::ProofState
War3PersistentGpuPackageObserver::classify(
    const SealInput& input) noexcept {
  if (!input.identityKnown || !input.identityExact ||
      input.identityToken == 0u)
    return ProofState::IdentityOnly;

  if (!input.sourceKnown || !input.sourceExact ||
      input.sourceToken == 0u || !exactPackageDecision(input))
    return ProofState::ContentPending;

  const bool exactMaterial = input.materialKnown && input.materialExact &&
      input.materialToken != 0u;
  const bool exactAlpha = input.alphaKnown && input.alphaExact &&
      input.alphaToken != 0u;
  const bool exactWorld = input.worldKnown && input.worldExact &&
      input.worldToken != 0u;
  const bool exactBounds = input.boundsKnown && input.boundsExact &&
      input.boundsToken != 0u;
  if (input.dynamic || input.skinned || !input.staticRigidProven ||
      !exactMaterial || !exactAlpha || !exactWorld || !exactBounds)
    return ProofState::PackageInputReady;

  return ProofState::FullyEquivalent;
}

War3PersistentGpuPackageObserver::BeginDecision
War3PersistentGpuPackageObserver::beginFrame(
    const FrameContext& context) noexcept {
  m_context = context;
  m_size = 0u;
  m_budget = std::min(context.observationBudget, kCapacity);
  m_diagnostics = {};
  m_diagnostics.requestedMode = context.requestedMode;
  m_diagnostics.canonicalWorkload = context.canonicalWorkload;

  BeginDecision decision = {};
  decision.requestedMode = context.requestedMode;
  decision.canonicalWorkload = context.canonicalWorkload;

  if (!isSupportedMode(context.requestedMode)) {
    decision.effectiveMode = Mode::Off;
    decision.disposition = Disposition::UnsupportedMode;
  } else if (context.requestedMode == Mode::Consume) {
    // Consume is an explicit capability failure until runtime observation,
    // consumer last-use ownership and correctness gates all pass.
    decision.effectiveMode = Mode::Off;
    decision.disposition = Disposition::ConsumeNotAdmitted;
  } else if (context.requestedMode == Mode::Off) {
    decision.effectiveMode = Mode::Off;
    decision.disposition = Disposition::ModeOff;
  } else if (!isValidFrameContext(context)) {
    decision.effectiveMode = Mode::Off;
    decision.disposition = Disposition::InvalidFrameContext;
  } else {
    decision.effectiveMode = Mode::Observe;
    decision.disposition = Disposition::Recorded;
  }

  m_diagnostics.effectiveMode = decision.effectiveMode;
  m_diagnostics.beginDisposition = decision.disposition;
  return decision;
}

War3PersistentGpuPackageObserver::SealDecision
War3PersistentGpuPackageObserver::makePassthroughDecision(
    const SealInput& input, ProofState state,
    Disposition disposition) const noexcept {
  SealDecision decision = {};
  decision.proofState = state;
  decision.disposition = disposition;
  decision.requestedConsumerMask = input.requestedConsumerMask;
  decision.effectiveConsumerMask = input.requestedConsumerMask;
  return decision;
}

void War3PersistentGpuPackageObserver::countState(
    ProofState state) noexcept {
  switch (state) {
    case ProofState::IdentityOnly:
      m_diagnostics.identityOnly += 1u;
      break;
    case ProofState::ContentPending:
      m_diagnostics.contentPending += 1u;
      break;
    case ProofState::PackageInputReady:
      m_diagnostics.packageInputReady += 1u;
      break;
    case ProofState::FullyEquivalent:
      m_diagnostics.fullyEquivalent += 1u;
      break;
    case ProofState::Rejected:
      m_diagnostics.rejected += 1u;
      break;
  }
}

void War3PersistentGpuPackageObserver::countActualTransition(
    uint32_t previousMask, uint32_t nextMask) noexcept {
  const uint32_t added = nextMask & ~previousMask;
  if (added == 0u)
    return;
  if (previousMask == 0u)
    m_diagnostics.actualConsumerEntries += 1u;
  m_diagnostics.actualConsumerMaskOr |= added;
  for (uint32_t bits = added; bits != 0u; bits &= bits - 1u)
    m_diagnostics.actualConsumerBits += 1u;
}

void War3PersistentGpuPackageObserver::countWouldUseTransition(
    uint32_t previousMask, uint32_t nextMask) noexcept {
  const uint32_t added = nextMask & ~previousMask;
  if (added == 0u)
    return;
  if (previousMask == 0u)
    m_diagnostics.wouldUseEntries += 1u;
  m_diagnostics.wouldUseConsumerMaskOr |= added;
}

War3PersistentGpuPackageObserver::SealDecision
War3PersistentGpuPackageObserver::append(
    const SealInput& input, ProofState state,
    Disposition disposition) noexcept {
  SealDecision decision = makePassthroughDecision(
      input, state, disposition);
  Entry& target = m_entries[m_size];
  const bool packageDecisionBound = exactPackageDecision(input);
  target.frameSerial = input.frameSerial;
  target.policyRevision = input.policyRevision;
  target.stage = input.stage;
  target.mapEpoch = input.mapEpoch;
  target.deviceEpoch = input.deviceEpoch;
  target.catalogInstanceGeneration = input.catalogInstanceGeneration;
  target.catalogSnapshotRevision = input.catalogSnapshotRevision;
  target.packagePublicationRevision = packageDecisionBound
      ? input.packageContentDecision.publicationRevision()
      : 0u;
  target.packageCanonicalDigest = packageDecisionBound
      ? input.packageContentDecision.canonicalDigest()
      : 0u;
  target.packageDecisionReason = input.packageContentDecision.reason();
  target.packageGeneration = input.packageGeneration;
  target.immutableModelGeneration = input.immutableModelGeneration;
  target.currentDrawSourceGeneration =
      input.currentDrawSourceGeneration;
  target.packageKey = input.packageKey;
  target.identityToken = input.identityToken;
  target.sourceToken = input.sourceToken;
  target.materialToken = input.materialToken;
  target.alphaToken = input.alphaToken;
  target.worldToken = input.worldToken;
  target.boundsToken = input.boundsToken;
  target.requestedConsumerMask = input.requestedConsumerMask;
  target.eligibleConsumerMask = state == ProofState::FullyEquivalent
      ? input.requestedConsumerMask
      : 0u;
  target.wouldUseConsumerMask = 0u;
  target.actualConsumerMask = 0u;
  target.proofState = state;
  target.disposition = disposition;
  target.packageDecisionBound = packageDecisionBound;
  if (input.packageContentDecision.ready() && !packageDecisionBound)
    m_diagnostics.packageDecisionBindingMismatch += 1u;

  target.wouldUseConsumerMask =
      target.eligibleConsumerMask & target.actualConsumerMask;

  decision.tableIndex = m_size;
  decision.recorded = true;
  decision.eligibleConsumerMask = target.eligibleConsumerMask;
  decision.wouldUseConsumerMask = target.wouldUseConsumerMask;
  decision.actualConsumerMask = target.actualConsumerMask;
  m_size += 1u;
  m_diagnostics.recorded += 1u;
  countState(state);

  if (target.eligibleConsumerMask != 0u) {
    m_diagnostics.eligibleEntries += 1u;
    m_diagnostics.eligibleConsumerMaskOr |= target.eligibleConsumerMask;
  }
  countWouldUseTransition(0u, target.wouldUseConsumerMask);
  countActualTransition(0u, target.actualConsumerMask);
  return decision;
}

War3PersistentGpuPackageObserver::SealDecision
War3PersistentGpuPackageObserver::seal(const SealInput& input) noexcept {
  m_diagnostics.sealCalls += 1u;

  if (m_diagnostics.requestedMode == Mode::Consume) {
    return makePassthroughDecision(
        input, ProofState::Rejected, Disposition::ConsumeNotAdmitted);
  }
  if (m_diagnostics.beginDisposition == Disposition::UnsupportedMode) {
    return makePassthroughDecision(
        input, ProofState::Rejected, Disposition::UnsupportedMode);
  }
  if (m_diagnostics.beginDisposition == Disposition::InvalidFrameContext) {
    return makePassthroughDecision(
        input, ProofState::Rejected, Disposition::InvalidFrameContext);
  }
  if (m_diagnostics.effectiveMode != Mode::Observe) {
    return makePassthroughDecision(
        input, ProofState::Rejected, Disposition::ModeOff);
  }

  if (m_size >= kCapacity) {
    m_diagnostics.deferredCapacity += 1u;
    return makePassthroughDecision(
        input, ProofState::Rejected, Disposition::DeferredCapacity);
  }
  if (m_size >= m_budget) {
    m_diagnostics.deferredBudget += 1u;
    return makePassthroughDecision(
        input, ProofState::Rejected, Disposition::DeferredBudget);
  }

  const uint32_t requestedMask = input.requestedConsumerMask;
  if (requestedMask == 0u || (requestedMask & ~kKnownConsumerMask) != 0u) {
    return append(
        input, ProofState::Rejected, Disposition::InvalidConsumerMask);
  }
  if (input.frameSerial != m_context.frameSerial) {
    return append(input, ProofState::Rejected, Disposition::FrameMismatch);
  }
  if (input.policyRevision != m_context.policyRevision) {
    return append(input, ProofState::Rejected, Disposition::PolicyMismatch);
  }
  if (input.stage != m_context.stage || input.stage != kRequiredStage) {
    return append(input, ProofState::Rejected, Disposition::StageMismatch);
  }
  if (!exactFrameKey(m_context, input)) {
    return append(
        input, ProofState::Rejected, Disposition::GenerationMismatch);
  }
  if (input.boundsKnown && !isFiniteBounds(input.bounds)) {
    return append(
        input, ProofState::Rejected, Disposition::NonFiniteBounds);
  }

  return append(input, classify(input), Disposition::Recorded);
}

const War3PersistentGpuPackageObserver::Entry*
War3PersistentGpuPackageObserver::entry(size_t index) const noexcept {
  return index < m_size ? &m_entries[index] : nullptr;
}

War3PersistentGpuPackageObserver::ActualNoteResult
War3PersistentGpuPackageObserver::noteActualConsumer(
    const ActualConsumerNote& note) noexcept {
  m_diagnostics.actualNoteCalls += 1u;
  const auto reject = [this](ActualNoteResult result) {
    m_diagnostics.actualNotesRejected += 1u;
    return result;
  };

  if (m_diagnostics.requestedMode == Mode::Consume)
    return reject(ActualNoteResult::ConsumeNotAdmitted);
  if (m_diagnostics.effectiveMode != Mode::Observe)
    return reject(ActualNoteResult::ModeOff);
  if (!isKnownSingleConsumerBit(note.consumerBit))
    return reject(ActualNoteResult::UnknownConsumerBit);
  if (note.tableIndex >= m_size)
    return reject(ActualNoteResult::InvalidEntry);

  if (note.frameSerial != m_context.frameSerial ||
      note.policyRevision != m_context.policyRevision ||
      note.stage != m_context.stage || note.stage != kRequiredStage ||
      note.catalogInstanceGeneration !=
          m_context.catalogInstanceGeneration ||
      note.catalogSnapshotRevision !=
          m_context.catalogSnapshotRevision)
    return reject(ActualNoteResult::ContextMismatch);
  if (note.mapEpoch != m_context.mapEpoch ||
      note.deviceEpoch != m_context.deviceEpoch)
    return reject(ActualNoteResult::GenerationMismatch);

  Entry& target = m_entries[note.tableIndex];
  if (note.packageGeneration != target.packageGeneration ||
      note.immutableModelGeneration != target.immutableModelGeneration ||
      note.currentDrawSourceGeneration !=
          target.currentDrawSourceGeneration)
    return reject(ActualNoteResult::GenerationMismatch);
  if (note.frameSerial != target.frameSerial ||
      note.policyRevision != target.policyRevision ||
      note.stage != target.stage || note.mapEpoch != target.mapEpoch ||
      note.deviceEpoch != target.deviceEpoch ||
      note.catalogInstanceGeneration !=
          target.catalogInstanceGeneration ||
      note.catalogSnapshotRevision != target.catalogSnapshotRevision ||
      !PackageProofCatalog::sameKey(note.packageKey, target.packageKey) ||
      note.identityToken == 0u ||
      note.identityToken != target.identityToken ||
      note.sourceToken != target.sourceToken ||
      note.materialToken != target.materialToken ||
      note.alphaToken != target.alphaToken ||
      note.worldToken != target.worldToken ||
      note.boundsToken != target.boundsToken)
    return reject(ActualNoteResult::EntryMismatch);
  if ((target.requestedConsumerMask & note.consumerBit) == 0u)
    return reject(ActualNoteResult::ConsumerNotRequested);
  if ((target.actualConsumerMask & note.consumerBit) != 0u) {
    m_diagnostics.actualNotesDuplicate += 1u;
    return ActualNoteResult::Duplicate;
  }

  const uint32_t previousActual = target.actualConsumerMask;
  const uint32_t previousWouldUse = target.wouldUseConsumerMask;
  target.actualConsumerMask |= note.consumerBit;
  target.wouldUseConsumerMask =
      target.eligibleConsumerMask & target.actualConsumerMask;
  countActualTransition(previousActual, target.actualConsumerMask);
  countWouldUseTransition(previousWouldUse, target.wouldUseConsumerMask);
  m_diagnostics.actualNotesRecorded += 1u;
  return ActualNoteResult::Recorded;
}

}  // namespace dxvk::war3::gpu_skin
