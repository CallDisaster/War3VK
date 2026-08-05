#pragma once

#include "war3_persistent_gpu_package_proof_catalog.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace dxvk::war3::gpu_skin {

// Value-only foundation for the future renderer-owned package recording path.
// This class deliberately owns no renderer, command list, package store or GPU
// resource. In particular, it is not a delayed "admission bool": the only
// successful emit boundary invokes the supplied callback exactly once while
// the transaction is in its terminal one-shot transition.
class War3PersistentGpuPackageRecordingAuthority final {
public:
  using ProofCatalog = War3PersistentGpuPackageProofCatalog;

  static constexpr uint32_t kRequiredStage = 11u;
  static constexpr uint32_t kMaxRecords = 4096u;
  static constexpr uint32_t kKnownConsumerMask = 0x7fu;
  // P1 grants publication only for a complete one-primitive package.
  static constexpr uint32_t kGrantedPrimitiveOrdinal = 0u;

  struct RecordingContext {
    uint64_t mapEpoch = 0u;
    uint64_t deviceEpoch = 0u;
    uint64_t frameSerial = 0u;
    uint64_t policyRevision = 0u;
    uint64_t stageGeneration = 0u;
    uint64_t recordingOwnerToken = 0u;
    uint64_t recordingSessionGeneration = 0u;
    uint64_t commandListGeneration = 0u;
    uint64_t emitCsSerial = 0u;
    uint64_t canonicalBatchToken = 0u;
    uint64_t ownerSubmissionSerial = 0u;
    uint32_t stage = 0u;
  };

  // Exact source witness captured at the current Stage11 draw boundary. The
  // duplicated frame/stage fields are intentional: a transaction context is
  // not allowed to wash a stale per-record witness into the current frame.
  struct CurrentStageSource {
    uint64_t mapEpoch = 0u;
    uint64_t deviceEpoch = 0u;
    uint64_t frameSerial = 0u;
    uint64_t sourceFrameSerial = 0u;
    uint64_t evidenceFrameSerial = 0u;
    uint64_t policyRevision = 0u;
    uint64_t stageGeneration = 0u;
    uint64_t currentDrawSourceGeneration = 0u;
    uint64_t immutableModelGeneration = 0u;
    uint64_t packageGeneration = 0u;
    uint64_t packageContentHash = 0u;
    uintptr_t geosetDataIdentity = 0u;
    uint64_t storeInstanceAuthority = 0u;
    uintptr_t frozenDescriptorIdentity = 0u;
    uintptr_t cacheSnapshotIdentity = 0u;
    // A caller-filled POD description is not publication authority. Retain
    // both the immutable catalog snapshot and the private-constructor Ready
    // decision minted by P1's validator. Every create/record/seal/EmitCs
    // boundary replays ready()+matches() against the exact tuple below.
    ProofCatalog::SharedSnapshot catalogSnapshot;
    ProofCatalog::PackageContentDecision packageContentDecision;
    uint64_t catalogInstanceGeneration = 0u;
    uint64_t catalogSnapshotRevision = 0u;
    uint64_t packagePublicationRevision = 0u;
    uint64_t packageCanonicalDigest = 0u;
    uint64_t identityToken = 0u;
    uint64_t sourceToken = 0u;
    uint64_t materialToken = 0u;
    uint64_t alphaToken = 0u;
    uint64_t worldToken = 0u;
    uint64_t boundsToken = 0u;
    uint32_t packageSchema = 0u;
    uint32_t packageLayoutGeneration = 0u;
    uint32_t packagePrimitiveOrdinal = 0u;
    uint32_t stage = 0u;
    bool captureComplete = false;
    bool exactCurrentAllocation = false;
    bool fresh = false;
    bool grace = false;
    bool rejected = false;
  };

  struct RecordInput {
    RecordingContext context;
    CurrentStageSource source;
    uint64_t recordIdentityToken = 0u;
    uint64_t commandIdentityToken = 0u;
    uint64_t commandPayloadDigest = 0u;
    uint32_t ordinal = 0u;
    uint32_t primitiveOrdinal = 0u;
    uint32_t consumerMask = 0u;
  };

  class TransactionTicket final {
  public:
    TransactionTicket() = default;
    bool valid() const noexcept {
      return m_instanceGeneration != 0u && m_transactionGeneration != 0u;
    }
    uint64_t instanceGeneration() const noexcept {
      return m_instanceGeneration;
    }
    uint64_t transactionGeneration() const noexcept {
      return m_transactionGeneration;
    }

  private:
    friend class War3PersistentGpuPackageRecordingAuthority;
    TransactionTicket(
        uint64_t instanceGeneration,
        uint64_t transactionGeneration) noexcept
    : m_instanceGeneration(instanceGeneration),
      m_transactionGeneration(transactionGeneration) { }

    uint64_t m_instanceGeneration = 0u;
    uint64_t m_transactionGeneration = 0u;
  };

  class SealedTicket final {
  public:
    SealedTicket() = default;
    bool valid() const noexcept {
      return m_instanceGeneration != 0u && m_transactionGeneration != 0u &&
          m_sealGeneration != 0u && m_recordCount != 0u &&
          m_recordDigest != 0u;
    }
    uint64_t instanceGeneration() const noexcept {
      return m_instanceGeneration;
    }
    uint64_t transactionGeneration() const noexcept {
      return m_transactionGeneration;
    }
    uint64_t sealGeneration() const noexcept { return m_sealGeneration; }
    uint32_t recordCount() const noexcept { return m_recordCount; }
    uint64_t recordDigest() const noexcept { return m_recordDigest; }

  private:
    friend class War3PersistentGpuPackageRecordingAuthority;
    SealedTicket(
        uint64_t instanceGeneration, uint64_t transactionGeneration,
        uint64_t sealGeneration, uint32_t recordCount,
        uint64_t recordDigest) noexcept
    : m_instanceGeneration(instanceGeneration),
      m_transactionGeneration(transactionGeneration),
      m_sealGeneration(sealGeneration), m_recordCount(recordCount),
      m_recordDigest(recordDigest) { }

    uint64_t m_instanceGeneration = 0u;
    uint64_t m_transactionGeneration = 0u;
    uint64_t m_sealGeneration = 0u;
    uint32_t m_recordCount = 0u;
    uint64_t m_recordDigest = 0u;
  };

  enum class State : uint8_t {
    Idle = 0u,
    Recording,
    Sealed,
    Emitting,
    Emitted,
    Aborted,
    Exhausted,
  };

  enum class CreateResult : uint8_t {
    Created = 0u,
    InvalidContext,
    InvalidExpectedBatch,
    Busy,
    WrongRecordingOwner,
    StaleOwnerSubmission,
    AllocationFailed,
    GenerationExhausted,
  };

  struct CreateInput {
    RecordingContext context;
    // Copied by create() before any command is recorded. Digest equality is
    // never sufficient: every accepted record must exactly equal this plan.
    std::vector<RecordInput> expectedRecords;
    uint32_t expectedRecordCount = 0u;
    uint64_t expectedRecordDigest = 0u;
  };

  struct CreateDecision {
    CreateResult result = CreateResult::InvalidContext;
    TransactionTicket ticket;
  };

  enum class RecordResult : uint8_t {
    Recorded = 0u,
    InvalidTicket,
    StaleTicket,
    NotRecording,
    WrongOwnerThread,
    ContextMismatch,
    InvalidCurrentStageSource,
    ExpectedRecordMismatch,
    OrdinalMismatch,
    RecordCapacityExceeded,
  };

  enum class SealResult : uint8_t {
    Sealed = 0u,
    InvalidTicket,
    StaleTicket,
    NotRecording,
    WrongOwnerThread,
    ContextMismatch,
    IncompleteBatch,
    DigestMismatch,
    RetainedPlanMismatch,
    GenerationExhausted,
  };

  struct SealDecision {
    SealResult result = SealResult::InvalidTicket;
    SealedTicket ticket;
  };

  enum class EmitResult : uint8_t {
    Emitted = 0u,
    CallbackFailed,
    InvalidTicket,
    StaleTicket,
    NotSealed,
    AlreadyTerminal,
    EmitInProgress,
    WrongOwnerThread,
    ContextMismatch,
    SealedProofMismatch,
    ImmutablePlanMismatch,
    MissingCallback,
  };

  enum class AbortResult : uint8_t {
    Aborted = 0u,
    InvalidTicket,
    StaleTicket,
    AlreadyTerminal,
    EmitInProgress,
  };

  class ImmutableCommandPlanView final {
  public:
    ImmutableCommandPlanView() = default;
    bool empty() const noexcept { return m_recordCount == 0u; }
    uint32_t size() const noexcept { return m_recordCount; }
    const RecordInput* data() const noexcept { return m_records; }
    const RecordInput* record(uint32_t ordinal) const noexcept {
      return ordinal < m_recordCount ? m_records + ordinal : nullptr;
    }

  private:
    friend class War3PersistentGpuPackageRecordingAuthority;
    ImmutableCommandPlanView(
        const RecordInput* records, uint32_t recordCount) noexcept
    : m_records(records), m_recordCount(recordCount) { }

    const RecordInput* m_records = nullptr;
    uint32_t m_recordCount = 0u;
  };

  struct SealedBatchView {
    RecordingContext context;
    uint64_t authorityInstanceGeneration = 0u;
    uint64_t transactionGeneration = 0u;
    uint64_t sealGeneration = 0u;
    uint64_t recordDigest = 0u;
    uint32_t recordCount = 0u;
    // Valid only for the dynamic extent of EmitCallback. The backing records
    // are authority-owned, immutable and retained until the callback returns;
    // callbackContext must never substitute an external mutable plan.
    ImmutableCommandPlanView commandPlan;
  };

  using EmitCallback = bool (*)(void*, const SealedBatchView&);

  struct Snapshot {
    State state = State::Idle;
    uint64_t instanceGeneration = 0u;
    uint64_t transactionGeneration = 0u;
    uint64_t sealGeneration = 0u;
    uint64_t boundRecordingOwnerToken = 0u;
    uint64_t lastOwnerSubmissionSerial = 0u;
    uint64_t recordDigest = 0u;
    uint64_t expectedRecordDigest = 0u;
    uint32_t recordCount = 0u;
    uint32_t expectedRecordCount = 0u;
  };

  struct Diagnostics {
    uint64_t createCalls = 0u;
    uint64_t created = 0u;
    uint64_t createRejected = 0u;
    uint64_t recordCalls = 0u;
    uint64_t recordsAccepted = 0u;
    uint64_t recordRejected = 0u;
    uint64_t sealCalls = 0u;
    uint64_t sealsAccepted = 0u;
    uint64_t sealRejected = 0u;
    uint64_t emitCalls = 0u;
    uint64_t callbacksStarted = 0u;
    uint64_t callbacksSucceeded = 0u;
    uint64_t callbacksFailed = 0u;
    uint64_t emitRejected = 0u;
    uint64_t abortCalls = 0u;
    uint64_t abortsAccepted = 0u;
    uint64_t abortRejected = 0u;
    uint64_t ownershipViolations = 0u;
    uint64_t contextMismatches = 0u;
    uint64_t staleTicketRejects = 0u;
    uint64_t transactionFailures = 0u;
  };

  War3PersistentGpuPackageRecordingAuthority();
  ~War3PersistentGpuPackageRecordingAuthority() = default;

  War3PersistentGpuPackageRecordingAuthority(
      const War3PersistentGpuPackageRecordingAuthority&) = delete;
  War3PersistentGpuPackageRecordingAuthority& operator=(
      const War3PersistentGpuPackageRecordingAuthority&) = delete;

  // The owner must compute this digest over the complete canonical batch
  // before create(). The authority independently accumulates the same digest
  // as commands are recorded and requires exact equality at seal().
  static uint64_t beginRecordDigest(
      const RecordingContext& context,
      uint32_t expectedRecordCount) noexcept;
  static uint64_t appendRecordDigest(
      uint64_t digest, const RecordInput& input) noexcept;

  CreateDecision create(const CreateInput& input);
  RecordResult record(
      const TransactionTicket& ticket, const RecordInput& input);
  SealDecision seal(
      const TransactionTicket& ticket,
      const RecordingContext& currentContext);
  EmitResult emitSealed(
      const SealedTicket& ticket,
      const RecordingContext& currentContext,
      EmitCallback callback, void* callbackContext);
  AbortResult abort(const TransactionTicket& ticket);
  AbortResult abort(const SealedTicket& ticket);

  Snapshot snapshot() const;
  Diagnostics diagnostics() const;

private:
  static bool validContext(const RecordingContext& context) noexcept;
  static bool sameContext(
      const RecordingContext& lhs,
      const RecordingContext& rhs) noexcept;
  static bool validCurrentStageSource(
      const CurrentStageSource& source,
      const RecordingContext& context) noexcept;
  static ProofCatalog::Key packageKey(
      const CurrentStageSource& source) noexcept;
  static bool sameCurrentStageSource(
      const CurrentStageSource& lhs,
      const CurrentStageSource& rhs) noexcept;
  static bool sameRecordInput(
      const RecordInput& lhs, const RecordInput& rhs) noexcept;
  static bool validExpectedPlan(const CreateInput& input) noexcept;
  bool validRetainedPlanLocked(
      const RecordingContext& currentContext) const noexcept;

  bool exactTicketLocked(const TransactionTicket& ticket) const noexcept;
  bool exactTicketLocked(const SealedTicket& ticket) const noexcept;
  void failCurrentLocked() noexcept;
  AbortResult abortLocked(
      uint64_t instanceGeneration,
      uint64_t transactionGeneration) noexcept;

  mutable std::mutex m_mutex;
  State m_state = State::Idle;
  uint64_t m_instanceGeneration = 0u;
  uint64_t m_nextTransactionGeneration = 1u;
  uint64_t m_transactionGeneration = 0u;
  uint64_t m_nextSealGeneration = 1u;
  uint64_t m_sealGeneration = 0u;
  uint64_t m_boundRecordingOwnerToken = 0u;
  uint64_t m_lastOwnerSubmissionSerial = 0u;
  uint64_t m_recordDigest = 0u;
  uint64_t m_expectedRecordDigest = 0u;
  uint32_t m_recordCount = 0u;
  uint32_t m_expectedRecordCount = 0u;
  std::vector<RecordInput> m_expectedRecords;
  RecordingContext m_context = {};
  bool m_ownerBound = false;
  std::thread::id m_ownerThread;
  Diagnostics m_diagnostics = {};
};

static_assert(std::is_standard_layout_v<
    War3PersistentGpuPackageRecordingAuthority::RecordingContext>);
static_assert(std::is_trivially_copyable_v<
    War3PersistentGpuPackageRecordingAuthority::RecordingContext>);
static_assert(std::is_nothrow_copy_constructible_v<
    War3PersistentGpuPackageRecordingAuthority::CurrentStageSource>);
static_assert(std::is_nothrow_copy_constructible_v<
    War3PersistentGpuPackageRecordingAuthority::RecordInput>);

}  // namespace dxvk::war3::gpu_skin
