#include "war3_persistent_gpu_package_recording_authority.h"

#include <atomic>
#include <limits>
#include <new>
#include <utility>

namespace dxvk::war3::gpu_skin {
namespace {

uint64_t acquireInstanceGeneration() noexcept {
  static std::atomic<uint64_t> nextGeneration {1u};
  uint64_t candidate = nextGeneration.load(std::memory_order_relaxed);
  for (;;) {
    if (candidate == 0u ||
        candidate == std::numeric_limits<uint64_t>::max())
      return 0u;
    if (nextGeneration.compare_exchange_weak(
            candidate, candidate + 1u,
            std::memory_order_relaxed,
            std::memory_order_relaxed))
      return candidate;
  }
}

uint64_t mixDigest(uint64_t digest, uint64_t value) noexcept {
  value ^= value >> 30u;
  value *= 0xbf58476d1ce4e5b9ull;
  value ^= value >> 27u;
  value *= 0x94d049bb133111ebull;
  value ^= value >> 31u;
  digest ^= value + 0x9e3779b97f4a7c15ull + (digest << 6u) +
      (digest >> 2u);
  return digest != 0u ? digest : 0xa5a5a5a5a5a5a5a5ull;
}

uint64_t mixContext(
    uint64_t digest,
    const War3PersistentGpuPackageRecordingAuthority::RecordingContext&
        context) noexcept {
  digest = mixDigest(digest, context.mapEpoch);
  digest = mixDigest(digest, context.deviceEpoch);
  digest = mixDigest(digest, context.frameSerial);
  digest = mixDigest(digest, context.policyRevision);
  digest = mixDigest(digest, context.stageGeneration);
  digest = mixDigest(digest, context.recordingOwnerToken);
  digest = mixDigest(digest, context.recordingSessionGeneration);
  digest = mixDigest(digest, context.commandListGeneration);
  digest = mixDigest(digest, context.emitCsSerial);
  digest = mixDigest(digest, context.canonicalBatchToken);
  digest = mixDigest(digest, context.ownerSubmissionSerial);
  return mixDigest(digest, context.stage);
}

}  // namespace

War3PersistentGpuPackageRecordingAuthority::
War3PersistentGpuPackageRecordingAuthority()
: m_instanceGeneration(acquireInstanceGeneration()) {
  if (m_instanceGeneration == 0u)
    m_state = State::Exhausted;
}

uint64_t War3PersistentGpuPackageRecordingAuthority::beginRecordDigest(
    const RecordingContext& context,
    uint32_t expectedRecordCount) noexcept {
  uint64_t digest = mixContext(0x6a09e667f3bcc909ull, context);
  digest = mixDigest(digest, expectedRecordCount);
  return mixDigest(digest, 0xbb67ae8584caa73bull);
}

uint64_t War3PersistentGpuPackageRecordingAuthority::appendRecordDigest(
    uint64_t digest, const RecordInput& input) noexcept {
  digest = mixContext(digest, input.context);
  const CurrentStageSource& source = input.source;
  digest = mixDigest(digest, source.mapEpoch);
  digest = mixDigest(digest, source.deviceEpoch);
  digest = mixDigest(digest, source.frameSerial);
  digest = mixDigest(digest, source.sourceFrameSerial);
  digest = mixDigest(digest, source.evidenceFrameSerial);
  digest = mixDigest(digest, source.policyRevision);
  digest = mixDigest(digest, source.stageGeneration);
  digest = mixDigest(digest, source.currentDrawSourceGeneration);
  digest = mixDigest(digest, source.immutableModelGeneration);
  digest = mixDigest(digest, source.packageGeneration);
  digest = mixDigest(digest, source.packageContentHash);
  digest = mixDigest(digest, source.geosetDataIdentity);
  digest = mixDigest(digest, source.storeInstanceAuthority);
  digest = mixDigest(digest, source.frozenDescriptorIdentity);
  digest = mixDigest(digest, source.cacheSnapshotIdentity);
  digest = mixDigest(digest, source.catalogInstanceGeneration);
  digest = mixDigest(digest, source.catalogSnapshotRevision);
  digest = mixDigest(digest, source.packagePublicationRevision);
  digest = mixDigest(digest, source.packageCanonicalDigest);
  digest = mixDigest(digest, source.identityToken);
  digest = mixDigest(digest, source.sourceToken);
  digest = mixDigest(digest, source.materialToken);
  digest = mixDigest(digest, source.alphaToken);
  digest = mixDigest(digest, source.worldToken);
  digest = mixDigest(digest, source.boundsToken);
  digest = mixDigest(digest, source.packageSchema);
  digest = mixDigest(digest, source.packageLayoutGeneration);
  digest = mixDigest(digest, source.packagePrimitiveOrdinal);
  digest = mixDigest(digest, source.stage);
  digest = mixDigest(digest, source.captureComplete ? 1u : 0u);
  digest = mixDigest(digest, source.exactCurrentAllocation ? 1u : 0u);
  digest = mixDigest(digest, source.fresh ? 1u : 0u);
  digest = mixDigest(digest, source.grace ? 1u : 0u);
  digest = mixDigest(digest, source.rejected ? 1u : 0u);
  digest = mixDigest(digest, input.recordIdentityToken);
  digest = mixDigest(digest, input.commandIdentityToken);
  digest = mixDigest(digest, input.commandPayloadDigest);
  digest = mixDigest(digest, input.ordinal);
  digest = mixDigest(digest, input.primitiveOrdinal);
  return mixDigest(digest, input.consumerMask);
}

bool War3PersistentGpuPackageRecordingAuthority::validContext(
    const RecordingContext& context) noexcept {
  return context.mapEpoch != 0u && context.deviceEpoch != 0u &&
      context.frameSerial != 0u && context.policyRevision != 0u &&
      context.stageGeneration != 0u &&
      context.recordingOwnerToken != 0u &&
      context.recordingSessionGeneration != 0u &&
      context.commandListGeneration != 0u && context.emitCsSerial != 0u &&
      context.canonicalBatchToken != 0u &&
      context.ownerSubmissionSerial != 0u &&
      context.stage == kRequiredStage;
}

bool War3PersistentGpuPackageRecordingAuthority::sameContext(
    const RecordingContext& lhs,
    const RecordingContext& rhs) noexcept {
  return lhs.mapEpoch == rhs.mapEpoch &&
      lhs.deviceEpoch == rhs.deviceEpoch &&
      lhs.frameSerial == rhs.frameSerial &&
      lhs.policyRevision == rhs.policyRevision &&
      lhs.stageGeneration == rhs.stageGeneration &&
      lhs.recordingOwnerToken == rhs.recordingOwnerToken &&
      lhs.recordingSessionGeneration == rhs.recordingSessionGeneration &&
      lhs.commandListGeneration == rhs.commandListGeneration &&
      lhs.emitCsSerial == rhs.emitCsSerial &&
      lhs.canonicalBatchToken == rhs.canonicalBatchToken &&
      lhs.ownerSubmissionSerial == rhs.ownerSubmissionSerial &&
      lhs.stage == rhs.stage;
}

War3PersistentGpuPackageRecordingAuthority::ProofCatalog::Key
War3PersistentGpuPackageRecordingAuthority::packageKey(
    const CurrentStageSource& source) noexcept {
  ProofCatalog::Key key;
  key.schema = source.packageSchema;
  key.mapEpoch = source.mapEpoch;
  key.deviceEpoch = source.deviceEpoch;
  key.packageGeneration = source.packageGeneration;
  key.immutableModelGeneration = source.immutableModelGeneration;
  key.geosetData = source.geosetDataIdentity;
  key.contentHash = source.packageContentHash;
  key.layoutGeneration = source.packageLayoutGeneration;
  key.primitiveOrdinal = source.packagePrimitiveOrdinal;
  return key;
}

bool War3PersistentGpuPackageRecordingAuthority::validCurrentStageSource(
    const CurrentStageSource& source,
    const RecordingContext& context) noexcept {
  if (source.mapEpoch != context.mapEpoch ||
      source.deviceEpoch != context.deviceEpoch ||
      source.frameSerial != context.frameSerial ||
      source.sourceFrameSerial != context.frameSerial ||
      source.evidenceFrameSerial != context.frameSerial ||
      source.policyRevision != context.policyRevision ||
      source.stageGeneration != context.stageGeneration ||
      source.stage != context.stage || source.stage != kRequiredStage ||
      source.currentDrawSourceGeneration == 0u ||
      source.immutableModelGeneration == 0u ||
      source.packageGeneration == 0u ||
      source.packageContentHash == 0u ||
      source.geosetDataIdentity == 0u ||
      source.storeInstanceAuthority == 0u ||
      source.frozenDescriptorIdentity == 0u ||
      source.cacheSnapshotIdentity == 0u ||
      source.catalogSnapshot == nullptr ||
      source.catalogInstanceGeneration == 0u ||
      source.catalogSnapshotRevision == 0u ||
      source.packagePublicationRevision == 0u ||
      source.packageCanonicalDigest == 0u ||
      source.identityToken == 0u || source.sourceToken == 0u ||
      source.materialToken == 0u || source.alphaToken == 0u ||
      source.worldToken == 0u || source.boundsToken == 0u ||
      source.packageSchema == 0u ||
      source.packageLayoutGeneration == 0u ||
      source.packagePrimitiveOrdinal != kGrantedPrimitiveOrdinal ||
      !source.captureComplete || !source.exactCurrentAllocation ||
      !source.fresh || source.grace || source.rejected)
    return false;

  const ProofCatalog::Key key = packageKey(source);
  const ProofCatalog::PackageContentDecision& decision =
      source.packageContentDecision;
  if (source.catalogSnapshot->instanceGeneration() !=
          source.catalogInstanceGeneration ||
      source.catalogSnapshot->revision() !=
          source.catalogSnapshotRevision ||
      !decision.ready() ||
      decision.reason() !=
          ProofCatalog::PackageContentDecision::Reason::Ready ||
      decision.catalogInstanceGeneration() !=
          source.catalogInstanceGeneration ||
      decision.catalogSnapshotRevision() !=
          source.catalogSnapshotRevision ||
      decision.publicationRevision() !=
          source.packagePublicationRevision ||
      decision.canonicalDigest() != source.packageCanonicalDigest ||
      !decision.matches(
          key, source.catalogSnapshot->instanceGeneration(),
          source.catalogSnapshot->revision(), source.frameSerial,
          source.policyRevision, source.stage, source.identityToken,
          source.sourceToken, source.currentDrawSourceGeneration,
          source.materialToken, source.alphaToken, source.worldToken,
          source.boundsToken))
    return false;

  const ProofCatalog::Entry* entry = source.catalogSnapshot->find(key);
  // Raw fields alone never reach this return: the private Ready decision and
  // its exact immutable snapshot are both required above.
  return entry != nullptr &&
      entry->value.state == ProofCatalog::PublicationState::UploadCompleted &&
      entry->value.publicationRevision == source.packagePublicationRevision &&
      entry->value.canonicalDigest == source.packageCanonicalDigest;
}

bool War3PersistentGpuPackageRecordingAuthority::sameCurrentStageSource(
    const CurrentStageSource& lhs,
    const CurrentStageSource& rhs) noexcept {
  return lhs.mapEpoch == rhs.mapEpoch &&
      lhs.deviceEpoch == rhs.deviceEpoch &&
      lhs.frameSerial == rhs.frameSerial &&
      lhs.sourceFrameSerial == rhs.sourceFrameSerial &&
      lhs.evidenceFrameSerial == rhs.evidenceFrameSerial &&
      lhs.policyRevision == rhs.policyRevision &&
      lhs.stageGeneration == rhs.stageGeneration &&
      lhs.currentDrawSourceGeneration ==
          rhs.currentDrawSourceGeneration &&
      lhs.immutableModelGeneration == rhs.immutableModelGeneration &&
      lhs.packageGeneration == rhs.packageGeneration &&
      lhs.packageContentHash == rhs.packageContentHash &&
      lhs.geosetDataIdentity == rhs.geosetDataIdentity &&
      lhs.storeInstanceAuthority == rhs.storeInstanceAuthority &&
      lhs.frozenDescriptorIdentity == rhs.frozenDescriptorIdentity &&
      lhs.cacheSnapshotIdentity == rhs.cacheSnapshotIdentity &&
      lhs.catalogSnapshot.get() == rhs.catalogSnapshot.get() &&
      lhs.packageContentDecision.ready() ==
          rhs.packageContentDecision.ready() &&
      lhs.packageContentDecision.reason() ==
          rhs.packageContentDecision.reason() &&
      lhs.packageContentDecision.catalogInstanceGeneration() ==
          rhs.packageContentDecision.catalogInstanceGeneration() &&
      lhs.packageContentDecision.catalogSnapshotRevision() ==
          rhs.packageContentDecision.catalogSnapshotRevision() &&
      lhs.packageContentDecision.publicationRevision() ==
          rhs.packageContentDecision.publicationRevision() &&
      lhs.packageContentDecision.canonicalDigest() ==
          rhs.packageContentDecision.canonicalDigest() &&
      lhs.catalogInstanceGeneration == rhs.catalogInstanceGeneration &&
      lhs.catalogSnapshotRevision == rhs.catalogSnapshotRevision &&
      lhs.packagePublicationRevision == rhs.packagePublicationRevision &&
      lhs.packageCanonicalDigest == rhs.packageCanonicalDigest &&
      lhs.identityToken == rhs.identityToken &&
      lhs.sourceToken == rhs.sourceToken &&
      lhs.materialToken == rhs.materialToken &&
      lhs.alphaToken == rhs.alphaToken &&
      lhs.worldToken == rhs.worldToken &&
      lhs.boundsToken == rhs.boundsToken &&
      lhs.packageSchema == rhs.packageSchema &&
      lhs.packageLayoutGeneration == rhs.packageLayoutGeneration &&
      lhs.packagePrimitiveOrdinal == rhs.packagePrimitiveOrdinal &&
      lhs.stage == rhs.stage &&
      lhs.captureComplete == rhs.captureComplete &&
      lhs.exactCurrentAllocation == rhs.exactCurrentAllocation &&
      lhs.fresh == rhs.fresh && lhs.grace == rhs.grace &&
      lhs.rejected == rhs.rejected;
}

bool War3PersistentGpuPackageRecordingAuthority::sameRecordInput(
    const RecordInput& lhs, const RecordInput& rhs) noexcept {
  return sameContext(lhs.context, rhs.context) &&
      sameCurrentStageSource(lhs.source, rhs.source) &&
      lhs.recordIdentityToken == rhs.recordIdentityToken &&
      lhs.commandIdentityToken == rhs.commandIdentityToken &&
      lhs.commandPayloadDigest == rhs.commandPayloadDigest &&
      lhs.ordinal == rhs.ordinal &&
      lhs.primitiveOrdinal == rhs.primitiveOrdinal &&
      lhs.consumerMask == rhs.consumerMask;
}

bool War3PersistentGpuPackageRecordingAuthority::validExpectedPlan(
    const CreateInput& input) noexcept {
  if (input.expectedRecordCount == 0u ||
      input.expectedRecordCount > kMaxRecords ||
      input.expectedRecordDigest == 0u ||
      input.expectedRecords.size() != input.expectedRecordCount)
    return false;

  uint64_t digest = beginRecordDigest(
      input.context, input.expectedRecordCount);
  for (uint32_t i = 0u; i < input.expectedRecordCount; i++) {
    const RecordInput& record = input.expectedRecords[i];
    if (!sameContext(record.context, input.context) ||
        !validCurrentStageSource(record.source, input.context) ||
        record.recordIdentityToken == 0u ||
        record.commandIdentityToken == 0u ||
        record.commandPayloadDigest == 0u || record.ordinal != i ||
        record.primitiveOrdinal != kGrantedPrimitiveOrdinal ||
        record.primitiveOrdinal != record.source.packagePrimitiveOrdinal ||
        record.consumerMask == 0u ||
        (record.consumerMask & ~kKnownConsumerMask) != 0u)
      return false;
    digest = appendRecordDigest(digest, record);
  }
  return digest == input.expectedRecordDigest;
}

bool War3PersistentGpuPackageRecordingAuthority::validRetainedPlanLocked(
    const RecordingContext& currentContext) const noexcept {
  if (!sameContext(currentContext, m_context) ||
      m_expectedRecordCount == 0u ||
      m_expectedRecordCount > kMaxRecords ||
      m_expectedRecordDigest == 0u ||
      m_expectedRecords.size() != m_expectedRecordCount)
    return false;

  uint64_t digest = beginRecordDigest(
      currentContext, m_expectedRecordCount);
  for (uint32_t ordinal = 0u; ordinal < m_expectedRecordCount; ++ordinal) {
    const RecordInput& record = m_expectedRecords[ordinal];
    if (!sameContext(record.context, currentContext) ||
        !validCurrentStageSource(record.source, currentContext) ||
        record.recordIdentityToken == 0u ||
        record.commandIdentityToken == 0u ||
        record.commandPayloadDigest == 0u || record.ordinal != ordinal ||
        record.primitiveOrdinal != kGrantedPrimitiveOrdinal ||
        record.primitiveOrdinal != record.source.packagePrimitiveOrdinal ||
        record.consumerMask == 0u ||
        (record.consumerMask & ~kKnownConsumerMask) != 0u)
      return false;
    digest = appendRecordDigest(digest, record);
  }
  return digest == m_expectedRecordDigest;
}

bool War3PersistentGpuPackageRecordingAuthority::exactTicketLocked(
    const TransactionTicket& ticket) const noexcept {
  return ticket.m_instanceGeneration == m_instanceGeneration &&
      ticket.m_transactionGeneration == m_transactionGeneration;
}

bool War3PersistentGpuPackageRecordingAuthority::exactTicketLocked(
    const SealedTicket& ticket) const noexcept {
  return ticket.m_instanceGeneration == m_instanceGeneration &&
      ticket.m_transactionGeneration == m_transactionGeneration;
}

void War3PersistentGpuPackageRecordingAuthority::failCurrentLocked() noexcept {
  if (m_state == State::Recording || m_state == State::Sealed) {
    m_state = State::Aborted;
    m_expectedRecords.clear();
    m_diagnostics.transactionFailures += 1u;
  }
}

War3PersistentGpuPackageRecordingAuthority::CreateDecision
War3PersistentGpuPackageRecordingAuthority::create(
    const CreateInput& input) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_diagnostics.createCalls += 1u;

  CreateDecision decision;
  if (m_state == State::Exhausted || m_instanceGeneration == 0u ||
      m_nextTransactionGeneration == 0u ||
      m_nextTransactionGeneration == std::numeric_limits<uint64_t>::max()) {
    m_state = State::Exhausted;
    decision.result = CreateResult::GenerationExhausted;
  } else if (m_state == State::Recording || m_state == State::Sealed ||
             m_state == State::Emitting) {
    decision.result = CreateResult::Busy;
  } else if (!validContext(input.context)) {
    decision.result = CreateResult::InvalidContext;
  } else if (!validExpectedPlan(input)) {
    decision.result = CreateResult::InvalidExpectedBatch;
  } else if (m_ownerBound &&
             (std::this_thread::get_id() != m_ownerThread ||
              input.context.recordingOwnerToken !=
                  m_boundRecordingOwnerToken)) {
    m_diagnostics.ownershipViolations += 1u;
    decision.result = CreateResult::WrongRecordingOwner;
  } else if (input.context.ownerSubmissionSerial <=
             m_lastOwnerSubmissionSerial) {
    decision.result = CreateResult::StaleOwnerSubmission;
  } else {
    std::vector<RecordInput> frozenExpectedRecords;
    try {
      frozenExpectedRecords = input.expectedRecords;
    } catch (const std::bad_alloc&) {
      decision.result = CreateResult::AllocationFailed;
      m_diagnostics.createRejected += 1u;
      return decision;
    }
    m_state = State::Recording;
    m_transactionGeneration = m_nextTransactionGeneration++;
    m_sealGeneration = 0u;
    m_lastOwnerSubmissionSerial = input.context.ownerSubmissionSerial;
    m_context = input.context;
    if (!m_ownerBound) {
      m_ownerBound = true;
      m_boundRecordingOwnerToken = input.context.recordingOwnerToken;
      m_ownerThread = std::this_thread::get_id();
    }
    m_recordCount = 0u;
    m_expectedRecordCount = input.expectedRecordCount;
    m_expectedRecords = std::move(frozenExpectedRecords);
    m_recordDigest = beginRecordDigest(
        input.context, input.expectedRecordCount);
    m_expectedRecordDigest = input.expectedRecordDigest;
    decision.result = CreateResult::Created;
    decision.ticket = TransactionTicket(
        m_instanceGeneration, m_transactionGeneration);
    m_diagnostics.created += 1u;
    return decision;
  }

  m_diagnostics.createRejected += 1u;
  return decision;
}

War3PersistentGpuPackageRecordingAuthority::RecordResult
War3PersistentGpuPackageRecordingAuthority::record(
    const TransactionTicket& ticket, const RecordInput& input) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_diagnostics.recordCalls += 1u;

  RecordResult result = RecordResult::Recorded;
  if (!ticket.valid()) {
    result = RecordResult::InvalidTicket;
  } else if (!exactTicketLocked(ticket)) {
    m_diagnostics.staleTicketRejects += 1u;
    result = RecordResult::StaleTicket;
  } else if (m_state != State::Recording) {
    // A post-seal mutation attempt invalidates an otherwise sealed batch. An
    // attempt after an already terminal state cannot resurrect it.
    if (m_state == State::Sealed)
      failCurrentLocked();
    result = RecordResult::NotRecording;
  } else if (std::this_thread::get_id() != m_ownerThread) {
    m_diagnostics.ownershipViolations += 1u;
    failCurrentLocked();
    result = RecordResult::WrongOwnerThread;
  } else if (!sameContext(input.context, m_context)) {
    m_diagnostics.contextMismatches += 1u;
    failCurrentLocked();
    result = RecordResult::ContextMismatch;
  } else if (!validCurrentStageSource(input.source, m_context) ||
             input.recordIdentityToken == 0u ||
             input.commandIdentityToken == 0u ||
             input.commandPayloadDigest == 0u ||
             input.primitiveOrdinal != kGrantedPrimitiveOrdinal ||
             input.primitiveOrdinal !=
                 input.source.packagePrimitiveOrdinal ||
             input.consumerMask == 0u ||
             (input.consumerMask & ~kKnownConsumerMask) != 0u) {
    failCurrentLocked();
    result = RecordResult::InvalidCurrentStageSource;
  } else if (m_recordCount >= kMaxRecords ||
             m_recordCount >= m_expectedRecordCount) {
    failCurrentLocked();
    result = RecordResult::RecordCapacityExceeded;
  } else if (input.ordinal != m_recordCount) {
    failCurrentLocked();
    result = RecordResult::OrdinalMismatch;
  } else if (m_recordCount >= m_expectedRecords.size() ||
             !sameRecordInput(input, m_expectedRecords[m_recordCount])) {
    failCurrentLocked();
    result = RecordResult::ExpectedRecordMismatch;
  } else {
    m_recordDigest = appendRecordDigest(m_recordDigest, input);
    m_recordCount += 1u;
    m_diagnostics.recordsAccepted += 1u;
    return RecordResult::Recorded;
  }

  m_diagnostics.recordRejected += 1u;
  return result;
}

War3PersistentGpuPackageRecordingAuthority::SealDecision
War3PersistentGpuPackageRecordingAuthority::seal(
    const TransactionTicket& ticket,
    const RecordingContext& currentContext) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_diagnostics.sealCalls += 1u;

  SealDecision decision;
  if (!ticket.valid()) {
    decision.result = SealResult::InvalidTicket;
  } else if (!exactTicketLocked(ticket)) {
    m_diagnostics.staleTicketRejects += 1u;
    decision.result = SealResult::StaleTicket;
  } else if (m_state != State::Recording) {
    if (m_state == State::Sealed)
      failCurrentLocked();
    decision.result = SealResult::NotRecording;
  } else if (std::this_thread::get_id() != m_ownerThread) {
    m_diagnostics.ownershipViolations += 1u;
    failCurrentLocked();
    decision.result = SealResult::WrongOwnerThread;
  } else if (!sameContext(currentContext, m_context)) {
    m_diagnostics.contextMismatches += 1u;
    failCurrentLocked();
    decision.result = SealResult::ContextMismatch;
  } else if (m_recordCount != m_expectedRecordCount) {
    failCurrentLocked();
    decision.result = SealResult::IncompleteBatch;
  } else if (m_recordDigest != m_expectedRecordDigest) {
    failCurrentLocked();
    decision.result = SealResult::DigestMismatch;
  } else if (!validRetainedPlanLocked(currentContext)) {
    failCurrentLocked();
    decision.result = SealResult::RetainedPlanMismatch;
  } else if (m_nextSealGeneration == 0u ||
             m_nextSealGeneration == std::numeric_limits<uint64_t>::max()) {
    m_state = State::Exhausted;
    m_expectedRecords.clear();
    m_diagnostics.transactionFailures += 1u;
    decision.result = SealResult::GenerationExhausted;
  } else {
    m_state = State::Sealed;
    m_sealGeneration = m_nextSealGeneration++;
    decision.result = SealResult::Sealed;
    decision.ticket = SealedTicket(
        m_instanceGeneration, m_transactionGeneration, m_sealGeneration,
        m_recordCount, m_recordDigest);
    m_diagnostics.sealsAccepted += 1u;
    return decision;
  }

  m_diagnostics.sealRejected += 1u;
  return decision;
}

War3PersistentGpuPackageRecordingAuthority::EmitResult
War3PersistentGpuPackageRecordingAuthority::emitSealed(
    const SealedTicket& ticket,
    const RecordingContext& currentContext,
    EmitCallback callback, void* callbackContext) {
  SealedBatchView view;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_diagnostics.emitCalls += 1u;

    EmitResult rejected = EmitResult::Emitted;
    if (!ticket.valid()) {
      rejected = EmitResult::InvalidTicket;
    } else if (!exactTicketLocked(ticket)) {
      m_diagnostics.staleTicketRejects += 1u;
      rejected = EmitResult::StaleTicket;
    } else if (m_state == State::Emitting) {
      rejected = EmitResult::EmitInProgress;
    } else if (m_state == State::Emitted || m_state == State::Aborted ||
               m_state == State::Exhausted) {
      rejected = EmitResult::AlreadyTerminal;
    } else if (m_state != State::Sealed) {
      rejected = EmitResult::NotSealed;
    } else if (ticket.m_sealGeneration != m_sealGeneration ||
               ticket.m_recordCount != m_recordCount ||
               ticket.m_recordDigest != m_recordDigest) {
      failCurrentLocked();
      rejected = EmitResult::SealedProofMismatch;
    } else if (std::this_thread::get_id() != m_ownerThread) {
      m_diagnostics.ownershipViolations += 1u;
      failCurrentLocked();
      rejected = EmitResult::WrongOwnerThread;
    } else if (!sameContext(currentContext, m_context)) {
      m_diagnostics.contextMismatches += 1u;
      failCurrentLocked();
      rejected = EmitResult::ContextMismatch;
    } else if (!validRetainedPlanLocked(currentContext) ||
               m_recordCount != m_expectedRecordCount ||
               m_expectedRecords.data() == nullptr) {
      failCurrentLocked();
      rejected = EmitResult::ImmutablePlanMismatch;
    } else if (callback == nullptr) {
      failCurrentLocked();
      rejected = EmitResult::MissingCallback;
    } else {
      m_state = State::Emitting;
      m_diagnostics.callbacksStarted += 1u;
      view.context = m_context;
      view.authorityInstanceGeneration = m_instanceGeneration;
      view.transactionGeneration = m_transactionGeneration;
      view.sealGeneration = m_sealGeneration;
      view.recordDigest = m_recordDigest;
      view.recordCount = m_recordCount;
      view.commandPlan = ImmutableCommandPlanView(
          m_expectedRecords.data(), m_recordCount);
      rejected = EmitResult::Emitted;
    }

    if (rejected != EmitResult::Emitted) {
      m_diagnostics.emitRejected += 1u;
      return rejected;
    }
  }

  bool callbackSucceeded = false;
  try {
    callbackSucceeded = callback(callbackContext, view);
  } catch (...) {
    // EmitCs is a terminal transaction boundary. Callback failures, including
    // exceptions, revoke the sealed plan instead of escaping across renderer
    // control flow or terminating through a noexcept function pointer.
    callbackSucceeded = false;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state != State::Emitting) {
    m_state = State::Aborted;
    m_expectedRecords.clear();
    m_diagnostics.callbacksFailed += 1u;
    m_diagnostics.transactionFailures += 1u;
    return EmitResult::CallbackFailed;
  }
  if (!callbackSucceeded) {
    m_state = State::Aborted;
    m_expectedRecords.clear();
    m_diagnostics.callbacksFailed += 1u;
    m_diagnostics.transactionFailures += 1u;
    return EmitResult::CallbackFailed;
  }

  m_state = State::Emitted;
  m_expectedRecords.clear();
  m_diagnostics.callbacksSucceeded += 1u;
  return EmitResult::Emitted;
}

War3PersistentGpuPackageRecordingAuthority::AbortResult
War3PersistentGpuPackageRecordingAuthority::abortLocked(
    uint64_t instanceGeneration,
    uint64_t transactionGeneration) noexcept {
  if (instanceGeneration == 0u || transactionGeneration == 0u)
    return AbortResult::InvalidTicket;
  if (instanceGeneration != m_instanceGeneration ||
      transactionGeneration != m_transactionGeneration) {
    m_diagnostics.staleTicketRejects += 1u;
    return AbortResult::StaleTicket;
  }
  if (m_state == State::Emitting)
    return AbortResult::EmitInProgress;
  if (m_state == State::Emitted || m_state == State::Aborted ||
      m_state == State::Exhausted || m_state == State::Idle)
    return AbortResult::AlreadyTerminal;

  m_state = State::Aborted;
  m_expectedRecords.clear();
  m_diagnostics.transactionFailures += 1u;
  return AbortResult::Aborted;
}

War3PersistentGpuPackageRecordingAuthority::AbortResult
War3PersistentGpuPackageRecordingAuthority::abort(
    const TransactionTicket& ticket) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_diagnostics.abortCalls += 1u;
  const AbortResult result = abortLocked(
      ticket.m_instanceGeneration, ticket.m_transactionGeneration);
  if (result == AbortResult::Aborted)
    m_diagnostics.abortsAccepted += 1u;
  else
    m_diagnostics.abortRejected += 1u;
  return result;
}

War3PersistentGpuPackageRecordingAuthority::AbortResult
War3PersistentGpuPackageRecordingAuthority::abort(
    const SealedTicket& ticket) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_diagnostics.abortCalls += 1u;
  const AbortResult result = abortLocked(
      ticket.m_instanceGeneration, ticket.m_transactionGeneration);
  if (result == AbortResult::Aborted)
    m_diagnostics.abortsAccepted += 1u;
  else
    m_diagnostics.abortRejected += 1u;
  return result;
}

War3PersistentGpuPackageRecordingAuthority::Snapshot
War3PersistentGpuPackageRecordingAuthority::snapshot() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  Snapshot result;
  result.state = m_state;
  result.instanceGeneration = m_instanceGeneration;
  result.transactionGeneration = m_transactionGeneration;
  result.sealGeneration = m_sealGeneration;
  result.boundRecordingOwnerToken = m_boundRecordingOwnerToken;
  result.lastOwnerSubmissionSerial = m_lastOwnerSubmissionSerial;
  result.recordDigest = m_recordDigest;
  result.expectedRecordDigest = m_expectedRecordDigest;
  result.recordCount = m_recordCount;
  result.expectedRecordCount = m_expectedRecordCount;
  return result;
}

War3PersistentGpuPackageRecordingAuthority::Diagnostics
War3PersistentGpuPackageRecordingAuthority::diagnostics() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_diagnostics;
}

}  // namespace dxvk::war3::gpu_skin
