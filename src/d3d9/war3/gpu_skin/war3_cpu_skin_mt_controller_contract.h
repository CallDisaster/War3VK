#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace dxvk::war3::gpu_skin {

// Phase 2A remains an isolated value contract. These gates deliberately do
// not authorize a native hook, a mapped-VB write, parity acceptance, or a
// production route.
inline constexpr bool kCpuSkinMtControllerRuntimeIntegrated = false;
inline constexpr bool kCpuSkinMtControllerNativeParityProven = false;
inline constexpr bool kCpuSkinMtControllerConsumeEnabled = false;
inline constexpr bool kCpuSkinMtControllerProductionDefault = false;

inline constexpr uint32_t kCpuSkinMtFormat2SkinMode = 1u;
inline constexpr uint32_t kCpuSkinMtFormat2OutputFormat = 2u;
inline constexpr uint32_t kCpuSkinMtFormat2Fvf = 0x112u;
inline constexpr uint32_t kCpuSkinMtFormat2OutputStride = 32u;
inline constexpr uint32_t kCpuSkinMtFormat2MinVertices = 193u;
inline constexpr uint32_t kCpuSkinMtFormat2MaxVertices = 448u;
inline constexpr uint32_t kCpuSkinMtFormat2MaxPaletteGroups = 64u;
inline constexpr uint32_t kCpuSkinMtPaletteMatrixBytes = 48u;
inline constexpr uint32_t kCpuSkinMtMxcsrStatusMask = 0x003fu;
inline constexpr uint32_t kCpuSkinMtMxcsrControlMask = 0xffc0u;
inline constexpr uint32_t kCpuSkinMtMxcsrExceptionMask = 0x1f80u;
inline constexpr uint32_t kCpuSkinMtProducerResultProofVersion = 2u;
inline constexpr uint32_t kCpuSkinMtRenderCommitEnvelopeVersion = 1u;

enum class CpuSkinMtControllerPath : uint8_t {
  Unknown = 0,
  Common,
  Special,
};

enum class CpuSkinMtControllerState : uint8_t {
  Invalid = 0,
  Frozen,
  Submitted,
  BoundToOuter,
  AwaitingKernel,
  CommitClaimed,
  CopyBodyInProgress,
  CopyAwaitingUnlock,
  NativeBodyInProgress,
  NativeAwaitingUnlock,
  CopiedNormal,
  OriginalNormal,
  OriginalFault,
  CopyFault,
  TemplateMismatch,
  ResetCancelled,
  OuterCancelled,
};

enum class CpuSkinMtControllerTerminal : uint8_t {
  None = 0,
  CopiedNormal,
  OriginalNormal,
  OriginalFault,
  CopyFault,
  TemplateMismatch,
  ResetCancelled,
  OuterCancelled,
};

enum class CpuSkinMtControllerResult : uint8_t {
  Applied = 0,
  AlreadyApplied,
  Published,
  AlreadyPublished,
  Consumed,
  Deferred,
  StaleGeneration,
  InvalidTransition,
  KeyMismatch,
  ProducerNotReady,
  CopyAfterUnlock,
  DoubleTerminal,
  LateProducerReady,
  MxcsrMismatch,
  MxcsrStatusDeltaMismatch,
  MissingCopyCallback,
  InvalidNativeLease,
};

enum class CpuSkinMtControllerKernelRoute : uint8_t {
  None = 0,
  Copy,
  Native,
  Cancelled,
};

enum class CpuSkinMtControllerKernelReason : uint8_t {
  None = 0,
  ReadyProducerResult,
  ProducerNotReady,
  StateLockContended,
  MxcsrMismatch,
  EnvelopeMismatch,
  AlreadyDecided,
  WindowClosed,
};

struct CpuSkinMtControllerOwnerSessionProof {
  uint64_t controllerInstanceGeneration = 0;
  uint64_t producerGeneration = 0;
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
  uint64_t bridgeResetGeneration = 0;
  uint32_t renderThreadId = 0;
};

struct CpuSkinMtControllerFlushBindingProof {
  uint64_t frameTag = 0;
  uint64_t flushEpoch = 0;
  uint64_t batchId = 0;
  uintptr_t renderablePart = 0;
  uintptr_t geosetData = 0;
  uint32_t candidateToken = 0;
  uint32_t flushCandidateOrdinal = 0;
  uint32_t layerIndex = 0;
  CpuSkinMtControllerPath path = CpuSkinMtControllerPath::Unknown;
  bool opaque = false;
};

struct CpuSkinMtControllerImmutableSourceProof {
  uintptr_t snapshotIdentity = 0;
  uint64_t contentHash = 0;
  uint64_t sealedContentToken = 0;
  uint64_t sourceGeneration = 0;
  uint32_t layoutGeneration = 0;
  uint32_t vertexCount = 0;
  uint32_t matrixGroupCount = 0;
  uint32_t uvLayerCount = 0;
  uint32_t maxGroupSlot = 0;
  uintptr_t positions = 0;
  uintptr_t normals = 0;
  uintptr_t groupSlots = 0;
  uintptr_t uv0 = 0;
  uintptr_t extra = 0;
  uintptr_t uv1 = 0;
  uint32_t positionStride = 0;
  uint32_t normalStride = 0;
  uint32_t groupSlotStride = 0;
  uint32_t uv0Stride = 0;
  uint32_t extraStride = 0;
  uint32_t uv1Stride = 0;
  uint32_t positionByteCount = 0;
  uint32_t normalByteCount = 0;
  uint32_t groupSlotByteCount = 0;
  uint32_t uv0ByteCount = 0;
};

struct CpuSkinMtControllerPaletteProof {
  uintptr_t palettePointer = 0;
  uintptr_t immutableBlobIdentity = 0;
  uint64_t paletteGeneration = 0;
  uint64_t paletteWriteSerial = 0;
  uint64_t paletteFrameTag = 0;
  uint64_t contentHash = 0;
  uint64_t sealedContentToken = 0;
  uint32_t groupCount = 0;
  uint32_t byteCount = 0;
  uint32_t floatingPointControl = 0;
};

struct CpuSkinMtControllerOuterBindingProof {
  uint64_t controllerInstanceGeneration = 0;
  uint64_t producerGeneration = 0;
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
  uint64_t bridgeResetGeneration = 0;
  uint64_t frameTag = 0;
  uint64_t flushEpoch = 0;
  uint64_t batchId = 0;
  uint64_t sourceGeneration = 0;
  uint64_t paletteGeneration = 0;
  uint64_t sourceSealedContentToken = 0;
  uint64_t paletteSealedContentToken = 0;
  uint64_t dispatchEpoch = 0;
  uint64_t uploadEpoch = 0;
  uintptr_t gxDeviceD3d = 0;
  uintptr_t renderablePart = 0;
  uintptr_t geosetData = 0;
  uintptr_t positions = 0;
  uintptr_t normals = 0;
  uintptr_t groupSlots = 0;
  uintptr_t uv0 = 0;
  uintptr_t extra = 0;
  uintptr_t uv1 = 0;
  uintptr_t palettePointer = 0;
  uint32_t uploadOrdinal = 0;
  uint32_t candidateToken = 0;
  uint32_t flushCandidateOrdinal = 0;
  uint32_t layerIndex = 0;
  uint32_t vertexCount = 0;
  uint32_t positionStride = 0;
  uint32_t normalStride = 0;
  uint32_t groupSlotStride = 0;
  uint32_t uv0Stride = 0;
  uint32_t extraStride = 0;
  uint32_t uv1Stride = 0;
  uint32_t paletteGroupCount = 0;
  uint32_t skinMode = 0;
  uint32_t outputFormat = 0;
  uint32_t fvf = 0;
  uint32_t outputStride = 0;
};

struct CpuSkinMtControllerDestinationProof {
  uintptr_t commonResource = 0;
  uintptr_t comVertexBuffer = 0;
  uintptr_t nativeD3DDevice = 0;
  uintptr_t mappedAllocation = 0;
  uintptr_t mappedAllocationBase = 0;
  uintptr_t mappedPointer = 0;
  uint64_t resourceGeneration = 0;
  uint64_t realStorageGeneration = 0;
  uint64_t mappingStorageGeneration = 0;
  uint64_t mapAllocationGeneration = 0;
  uint64_t lockSerial = 0;
  uint64_t mappedAllocationByteSize = 0;
  uint64_t dispatchEpoch = 0;
  uint64_t uploadEpoch = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t effectiveFlags = 0;
  uint32_t lockDepth = 0;
  uint32_t uploadOrdinal = 0;
  uint32_t outputFormat = 0;
  uint32_t fvf = 0;
  bool lockSucceeded = false;
  bool lockActive = false;
};

struct CpuSkinMtControllerUnlockProof {
  uintptr_t commonResource = 0;
  uintptr_t comVertexBuffer = 0;
  uintptr_t mappedAllocation = 0;
  uintptr_t mappedPointer = 0;
  uint64_t resourceGeneration = 0;
  uint64_t mapAllocationGeneration = 0;
  uint64_t lockSerial = 0;
  uint64_t unlockSerial = 0;
  int32_t result = 0;
};

struct CpuSkinMtControllerFrozenKey {
  CpuSkinMtControllerOwnerSessionProof owner;
  CpuSkinMtControllerFlushBindingProof flush;
  CpuSkinMtControllerImmutableSourceProof source;
  CpuSkinMtControllerPaletteProof palette;
};

bool operator==(const CpuSkinMtControllerOwnerSessionProof& lhs,
                const CpuSkinMtControllerOwnerSessionProof& rhs) noexcept;
bool operator==(const CpuSkinMtControllerFlushBindingProof& lhs,
                const CpuSkinMtControllerFlushBindingProof& rhs) noexcept;
bool operator==(const CpuSkinMtControllerImmutableSourceProof& lhs,
                const CpuSkinMtControllerImmutableSourceProof& rhs) noexcept;
bool operator==(const CpuSkinMtControllerPaletteProof& lhs,
                const CpuSkinMtControllerPaletteProof& rhs) noexcept;
bool operator==(const CpuSkinMtControllerOuterBindingProof& lhs,
                const CpuSkinMtControllerOuterBindingProof& rhs) noexcept;
bool operator==(const CpuSkinMtControllerDestinationProof& lhs,
                const CpuSkinMtControllerDestinationProof& rhs) noexcept;
bool operator==(const CpuSkinMtControllerUnlockProof& lhs,
                const CpuSkinMtControllerUnlockProof& rhs) noexcept;
bool operator==(const CpuSkinMtControllerFrozenKey& lhs,
                const CpuSkinMtControllerFrozenKey& rhs) noexcept;

inline bool operator!=(const CpuSkinMtControllerFrozenKey& lhs,
                       const CpuSkinMtControllerFrozenKey& rhs) noexcept {
  return !(lhs == rhs);
}

bool CpuSkinMtControllerFrozenKeyValid(
    const CpuSkinMtControllerFrozenKey& key) noexcept;
bool CpuSkinMtControllerOuterMatchesFrozen(
    const CpuSkinMtControllerFrozenKey& frozen,
    const CpuSkinMtControllerOuterBindingProof& outer) noexcept;
bool CpuSkinMtControllerDestinationValid(
    const CpuSkinMtControllerFrozenKey& frozen,
    const CpuSkinMtControllerOuterBindingProof& outer,
    const CpuSkinMtControllerDestinationProof& destination) noexcept;
bool CpuSkinMtControllerUnlockMatchesDestination(
    const CpuSkinMtControllerDestinationProof& destination,
    const CpuSkinMtControllerUnlockProof& unlock) noexcept;

enum CpuSkinMtFormat2EligibilityFailure : uint64_t {
  CpuSkinMtFormat2EligibilityNone = 0,
  CpuSkinMtFormat2EligibilityNotCommon = 1ull << 0,
  CpuSkinMtFormat2EligibilityNotOpaque = 1ull << 1,
  CpuSkinMtFormat2EligibilitySpecialDispatch = 1ull << 2,
  CpuSkinMtFormat2EligibilitySkinMode = 1ull << 3,
  CpuSkinMtFormat2EligibilityOutputFormat = 1ull << 4,
  CpuSkinMtFormat2EligibilityFvf = 1ull << 5,
  CpuSkinMtFormat2EligibilityOutputStride = 1ull << 6,
  CpuSkinMtFormat2EligibilityVertexRange = 1ull << 7,
  CpuSkinMtFormat2EligibilityMissingStream = 1ull << 8,
  CpuSkinMtFormat2EligibilityUnexpectedExtra = 1ull << 9,
  CpuSkinMtFormat2EligibilityUnexpectedUv1 = 1ull << 10,
  CpuSkinMtFormat2EligibilityPositionStride = 1ull << 11,
  CpuSkinMtFormat2EligibilityNormalStride = 1ull << 12,
  CpuSkinMtFormat2EligibilityGroupStride = 1ull << 13,
  CpuSkinMtFormat2EligibilityUv0Stride = 1ull << 14,
  CpuSkinMtFormat2EligibilityUvLayerCount = 1ull << 15,
  CpuSkinMtFormat2EligibilityPaletteRange = 1ull << 16,
  CpuSkinMtFormat2EligibilityGroupSlotRange = 1ull << 17,
  CpuSkinMtFormat2EligibilityPositionCount = 1ull << 18,
  CpuSkinMtFormat2EligibilityNormalCount = 1ull << 19,
  CpuSkinMtFormat2EligibilityGroupCount = 1ull << 20,
  CpuSkinMtFormat2EligibilityUv0Count = 1ull << 21,
  CpuSkinMtFormat2EligibilityPaletteCount = 1ull << 22,
  CpuSkinMtFormat2EligibilityNonFinitePosition = 1ull << 23,
  CpuSkinMtFormat2EligibilityNonFiniteNormal = 1ull << 24,
  CpuSkinMtFormat2EligibilityNonFinitePalette = 1ull << 25,
  CpuSkinMtFormat2EligibilityUnsafeMxcsr = 1ull << 26,
  CpuSkinMtFormat2EligibilityMxcsrMismatch = 1ull << 27,
  CpuSkinMtFormat2EligibilitySourceUnsealed = 1ull << 28,
  CpuSkinMtFormat2EligibilityPaletteUnsealed = 1ull << 29,
};

struct CpuSkinMtFormat2EligibilityInput {
  CpuSkinMtControllerPath path = CpuSkinMtControllerPath::Unknown;
  bool opaque = false;
  bool specialDispatch = false;
  uint32_t skinMode = 0;
  uint32_t outputFormat = 0;
  uint32_t fvf = 0;
  uint32_t outputStride = 0;
  uint32_t vertexCount = 0;
  uint32_t positionStride = 0;
  uint32_t normalStride = 0;
  uint32_t groupSlotStride = 0;
  uint32_t uv0Stride = 0;
  uint32_t uvLayerCount = 0;
  uint32_t paletteGroupCount = 0;
  uintptr_t extra = 0;
  uintptr_t uv1 = 0;
  const float* positions = nullptr;
  size_t positionFloatCount = 0;
  const float* normals = nullptr;
  size_t normalFloatCount = 0;
  const uint8_t* groupSlots = nullptr;
  size_t groupSlotCount = 0;
  const float* uv0 = nullptr;
  size_t uv0FloatCount = 0;
  const float* palette = nullptr;
  size_t paletteFloatCount = 0;
  uint64_t immutableSourceSealedToken = 0;
  uint64_t paletteSealedToken = 0;
  uint32_t frozenMxcsr = 0;
  uint32_t currentMxcsr = 0;
};

struct CpuSkinMtFormat2EligibilityResult {
  uint64_t failureMask = CpuSkinMtFormat2EligibilityNone;
  bool eligible = false;
};

struct CpuSkinMtControllerMxcsrStatusDelta {
  uint32_t normalizedControl = 0;
  uint32_t statusBefore = 0;
  uint32_t statusAfter = 0;
  uint32_t raisedStatus = 0;
  uint32_t clearedStatus = 0;
  bool valid = false;
};

bool operator==(const CpuSkinMtControllerMxcsrStatusDelta& lhs,
                const CpuSkinMtControllerMxcsrStatusDelta& rhs) noexcept;
uint32_t CpuSkinMtNormalizeMxcsrControl(uint32_t mxcsr) noexcept;
bool CpuSkinMtHasSafeMxcsrControl(uint32_t mxcsr) noexcept;
CpuSkinMtControllerMxcsrStatusDelta CpuSkinMtCaptureMxcsrStatusDelta(
    uint32_t before,
    uint32_t after) noexcept;
CpuSkinMtFormat2EligibilityResult CpuSkinMtValidateFormat2Eligibility(
    const CpuSkinMtFormat2EligibilityInput& input) noexcept;
bool CpuSkinMtEligibilityMatchesFrozenKey(
    const CpuSkinMtControllerFrozenKey& key,
    const CpuSkinMtFormat2EligibilityInput& input) noexcept;

struct CpuSkinMtControllerProducerResultProof {
  CpuSkinMtControllerFrozenKey frozenKey;
  CpuSkinMtControllerMxcsrStatusDelta mxcsrStatusDelta;
  uint64_t resultSerial = 0;
  uint64_t producerGeneration = 0;
  uint64_t outputContentHash = 0;
  uintptr_t ownedBytesIdentity = 0;
  uint32_t outputByteSize = 0;
  uint32_t normalizedMxcsr = 0;
  uint32_t proofVersion = 0;
};

bool operator==(const CpuSkinMtControllerProducerResultProof& lhs,
                const CpuSkinMtControllerProducerResultProof& rhs) noexcept;

class CpuSkinMtControllerOwnedProducerResult final {
public:
  static std::shared_ptr<const CpuSkinMtControllerOwnedProducerResult> Create(
      const CpuSkinMtControllerFrozenKey& frozenKey,
      const CpuSkinMtFormat2EligibilityInput& eligibility,
      uint64_t resultSerial,
      std::vector<uint8_t> bytes,
      uint32_t producerMxcsrBefore,
      uint32_t producerMxcsrAfter);

  CpuSkinMtControllerOwnedProducerResult(
      const CpuSkinMtControllerOwnedProducerResult&) = delete;
  CpuSkinMtControllerOwnedProducerResult& operator=(
      const CpuSkinMtControllerOwnedProducerResult&) = delete;

  const CpuSkinMtControllerProducerResultProof& proof() const noexcept;
  const uint8_t* data() const noexcept;
  uint32_t size() const noexcept;
  bool proofComplete() const noexcept;

private:
  CpuSkinMtControllerOwnedProducerResult(
      const CpuSkinMtControllerFrozenKey& frozenKey,
      const CpuSkinMtControllerMxcsrStatusDelta& statusDelta,
      uint64_t resultSerial,
      std::vector<uint8_t> bytes) noexcept;

  CpuSkinMtControllerProducerResultProof m_proof = {};
  std::vector<uint8_t> m_bytes;
};

struct CpuSkinMtControllerRenderCommitEnvelopeProof {
  CpuSkinMtControllerFrozenKey frozenKey;
  CpuSkinMtControllerOuterBindingProof outer;
  CpuSkinMtControllerDestinationProof destination;
  CpuSkinMtControllerProducerResultProof producerResult;
  uint64_t commitSerial = 0;
  uint32_t normalizedOwnerMxcsr = 0;
  uint32_t proofVersion = 0;
};

bool operator==(const CpuSkinMtControllerRenderCommitEnvelopeProof& lhs,
                const CpuSkinMtControllerRenderCommitEnvelopeProof& rhs)
    noexcept;
bool CpuSkinMtControllerRenderCommitEnvelopeComplete(
    const CpuSkinMtControllerRenderCommitEnvelopeProof& proof) noexcept;

class CpuSkinMtControllerRenderCommitEnvelope final {
public:
  CpuSkinMtControllerRenderCommitEnvelope() noexcept = default;
  bool valid() const noexcept;
  const CpuSkinMtControllerRenderCommitEnvelopeProof& proof() const noexcept;

private:
  static CpuSkinMtControllerRenderCommitEnvelope MintAfterLock(
      const CpuSkinMtControllerFrozenKey& frozenKey,
      const CpuSkinMtControllerOuterBindingProof& outer,
      const CpuSkinMtControllerDestinationProof& destination,
      std::shared_ptr<const CpuSkinMtControllerOwnedProducerResult> result,
      uint64_t commitSerial,
      uint32_t ownerMxcsr) noexcept;

  CpuSkinMtControllerRenderCommitEnvelopeProof m_proof = {};
  std::shared_ptr<const CpuSkinMtControllerOwnedProducerResult> m_result;

  friend class CpuSkinMtControllerJob;
};

class CpuSkinMtControllerNativeBodyLease final {
public:
  CpuSkinMtControllerNativeBodyLease() noexcept = default;
  bool valid() const noexcept;
  uint64_t activeGeneration() const noexcept;
  uint64_t lockSerial() const noexcept;

private:
  CpuSkinMtControllerNativeBodyLease(
      uint64_t controllerInstanceGeneration,
      uint64_t producerGeneration,
      uint64_t activeGeneration,
      uint64_t frameTag,
      uint64_t batchId,
      uint64_t lockSerial) noexcept;

  uint64_t m_controllerInstanceGeneration = 0;
  uint64_t m_producerGeneration = 0;
  uint64_t m_activeGeneration = 0;
  uint64_t m_frameTag = 0;
  uint64_t m_batchId = 0;
  uint64_t m_lockSerial = 0;

  friend bool operator==(const CpuSkinMtControllerNativeBodyLease& lhs,
                         const CpuSkinMtControllerNativeBodyLease& rhs)
      noexcept;
  friend class CpuSkinMtControllerJob;
};

bool operator==(const CpuSkinMtControllerNativeBodyLease& lhs,
                const CpuSkinMtControllerNativeBodyLease& rhs) noexcept;

struct CpuSkinMtControllerKernelDecision {
  CpuSkinMtControllerResult result =
      CpuSkinMtControllerResult::InvalidTransition;
  CpuSkinMtControllerKernelRoute route =
      CpuSkinMtControllerKernelRoute::None;
  CpuSkinMtControllerKernelReason reason =
      CpuSkinMtControllerKernelReason::None;
  CpuSkinMtControllerNativeBodyLease nativeBodyLease;
  bool stateLockAcquired = false;
  bool selectedNow = false;
};

struct CpuSkinMtControllerBodyCompletion {
  bool succeeded = false;
  uint32_t mxcsrBefore = 0;
  uint32_t mxcsrAfter = 0;
};

bool operator==(const CpuSkinMtControllerBodyCompletion& lhs,
                const CpuSkinMtControllerBodyCompletion& rhs) noexcept;

struct CpuSkinMtControllerProducerResultView {
  const uint8_t* bytes = nullptr;
  uint32_t byteSize = 0;
  const CpuSkinMtControllerProducerResultProof* producerProof = nullptr;
  const CpuSkinMtControllerRenderCommitEnvelopeProof* commitEnvelope =
      nullptr;
};

using CpuSkinMtControllerCopyCallback = CpuSkinMtControllerBodyCompletion (*)(
    void* context,
    const CpuSkinMtControllerProducerResultView& result,
    const CpuSkinMtControllerDestinationProof& destination) noexcept;

struct CpuSkinMtControllerOuterSettlementSnapshot {
  CpuSkinMtControllerKernelRoute route =
      CpuSkinMtControllerKernelRoute::None;
  CpuSkinMtControllerTerminal deferredTerminal =
      CpuSkinMtControllerTerminal::None;
  CpuSkinMtControllerBodyCompletion bodyCompletion;
  CpuSkinMtControllerMxcsrStatusDelta bodyMxcsrStatusDelta;
  bool settlementOpened = false;
  bool bodyCompleted = false;
  bool unlockObserved = false;
  bool producerResultPublished = false;
  bool producerResultClaimed = false;
  bool producerResultConsumed = false;
  bool nativeBodyLeaseInProgress = false;
  bool cancellationDeferred = false;
  bool settled = false;
};

struct CpuSkinMtControllerTerminalLedgerSnapshot {
  uint64_t jobsCreated = 0;
  uint64_t liveJobs = 0;
  uint64_t producerResultPublications = 0;
  uint64_t liveProducerResults = 0;
  uint64_t producerResultClaims = 0;
  uint64_t liveProducerClaims = 0;
  uint64_t producerResultConsumptions = 0;
  uint64_t producerResultAbandoned = 0;
  uint64_t producerClaimAbandoned = 0;
  uint64_t copySelections = 0;
  uint64_t nativeSelections = 0;
  uint64_t liveCopyJobs = 0;
  uint64_t liveNativeJobs = 0;
  uint64_t nativeBodyLeases = 0;
  uint64_t liveNativeBodyLeases = 0;
  uint64_t nativeBodiesCompleted = 0;
  uint64_t nativeBodyLeasesAbandoned = 0;
  uint64_t outerSettlementsOpened = 0;
  uint64_t liveOuterSettlements = 0;
  uint64_t outerSettlementsCompleted = 0;
  uint64_t guardedCopies = 0;
  uint64_t guardedCopyBodyFaults = 0;
  uint64_t mxcsrStatusDeltasRecorded = 0;
  uint64_t mxcsrStatusDeltaMismatches = 0;
  uint64_t successfulUnlocks = 0;
  uint64_t failedUnlocks = 0;
  uint64_t deferredResetCancellations = 0;
  uint64_t deferredOuterCancellations = 0;
  uint64_t deferredTemplateCancellations = 0;
  uint64_t copiedNormal = 0;
  uint64_t originalNormal = 0;
  uint64_t originalFault = 0;
  uint64_t copyFault = 0;
  uint64_t templateMismatch = 0;
  uint64_t resetCancelled = 0;
  uint64_t outerCancelled = 0;
  uint64_t copyCancelled = 0;
  uint64_t nativeCancelled = 0;
  uint64_t unselectedCancelled = 0;
  uint64_t staleGenerationRejects = 0;
  uint64_t invalidTransitionRejects = 0;
  uint64_t keyMismatchRejects = 0;
  uint64_t producerNotReadyRejects = 0;
  uint64_t lateProducerReadyRejects = 0;
  uint64_t copyAfterUnlockRejects = 0;
  uint64_t doubleTerminalRejects = 0;
  uint64_t mxcsrMismatchRejects = 0;
  uint64_t invalidNativeLeaseRejects = 0;

  uint64_t terminalJobs() const noexcept;
  bool closureHolds() const noexcept;
};

class CpuSkinMtControllerTerminalLedger {
public:
  CpuSkinMtControllerTerminalLedger() = default;
  CpuSkinMtControllerTerminalLedger(
      const CpuSkinMtControllerTerminalLedger&) = delete;
  CpuSkinMtControllerTerminalLedger& operator=(
      const CpuSkinMtControllerTerminalLedger&) = delete;

  CpuSkinMtControllerTerminalLedgerSnapshot snapshot() const noexcept;

private:
  struct AtomicCounters {
#define CPU_SKIN_MT_ATOMIC_COUNTER(name) std::atomic<uint64_t> name{0}
    CPU_SKIN_MT_ATOMIC_COUNTER(jobsCreated);
    CPU_SKIN_MT_ATOMIC_COUNTER(liveJobs);
    CPU_SKIN_MT_ATOMIC_COUNTER(producerResultPublications);
    CPU_SKIN_MT_ATOMIC_COUNTER(liveProducerResults);
    CPU_SKIN_MT_ATOMIC_COUNTER(producerResultClaims);
    CPU_SKIN_MT_ATOMIC_COUNTER(liveProducerClaims);
    CPU_SKIN_MT_ATOMIC_COUNTER(producerResultConsumptions);
    CPU_SKIN_MT_ATOMIC_COUNTER(producerResultAbandoned);
    CPU_SKIN_MT_ATOMIC_COUNTER(producerClaimAbandoned);
    CPU_SKIN_MT_ATOMIC_COUNTER(copySelections);
    CPU_SKIN_MT_ATOMIC_COUNTER(nativeSelections);
    CPU_SKIN_MT_ATOMIC_COUNTER(liveCopyJobs);
    CPU_SKIN_MT_ATOMIC_COUNTER(liveNativeJobs);
    CPU_SKIN_MT_ATOMIC_COUNTER(nativeBodyLeases);
    CPU_SKIN_MT_ATOMIC_COUNTER(liveNativeBodyLeases);
    CPU_SKIN_MT_ATOMIC_COUNTER(nativeBodiesCompleted);
    CPU_SKIN_MT_ATOMIC_COUNTER(nativeBodyLeasesAbandoned);
    CPU_SKIN_MT_ATOMIC_COUNTER(outerSettlementsOpened);
    CPU_SKIN_MT_ATOMIC_COUNTER(liveOuterSettlements);
    CPU_SKIN_MT_ATOMIC_COUNTER(outerSettlementsCompleted);
    CPU_SKIN_MT_ATOMIC_COUNTER(guardedCopies);
    CPU_SKIN_MT_ATOMIC_COUNTER(guardedCopyBodyFaults);
    CPU_SKIN_MT_ATOMIC_COUNTER(mxcsrStatusDeltasRecorded);
    CPU_SKIN_MT_ATOMIC_COUNTER(mxcsrStatusDeltaMismatches);
    CPU_SKIN_MT_ATOMIC_COUNTER(successfulUnlocks);
    CPU_SKIN_MT_ATOMIC_COUNTER(failedUnlocks);
    CPU_SKIN_MT_ATOMIC_COUNTER(deferredResetCancellations);
    CPU_SKIN_MT_ATOMIC_COUNTER(deferredOuterCancellations);
    CPU_SKIN_MT_ATOMIC_COUNTER(deferredTemplateCancellations);
    CPU_SKIN_MT_ATOMIC_COUNTER(copiedNormal);
    CPU_SKIN_MT_ATOMIC_COUNTER(originalNormal);
    CPU_SKIN_MT_ATOMIC_COUNTER(originalFault);
    CPU_SKIN_MT_ATOMIC_COUNTER(copyFault);
    CPU_SKIN_MT_ATOMIC_COUNTER(templateMismatch);
    CPU_SKIN_MT_ATOMIC_COUNTER(resetCancelled);
    CPU_SKIN_MT_ATOMIC_COUNTER(outerCancelled);
    CPU_SKIN_MT_ATOMIC_COUNTER(copyCancelled);
    CPU_SKIN_MT_ATOMIC_COUNTER(nativeCancelled);
    CPU_SKIN_MT_ATOMIC_COUNTER(unselectedCancelled);
    CPU_SKIN_MT_ATOMIC_COUNTER(staleGenerationRejects);
    CPU_SKIN_MT_ATOMIC_COUNTER(invalidTransitionRejects);
    CPU_SKIN_MT_ATOMIC_COUNTER(keyMismatchRejects);
    CPU_SKIN_MT_ATOMIC_COUNTER(producerNotReadyRejects);
    CPU_SKIN_MT_ATOMIC_COUNTER(lateProducerReadyRejects);
    CPU_SKIN_MT_ATOMIC_COUNTER(copyAfterUnlockRejects);
    CPU_SKIN_MT_ATOMIC_COUNTER(doubleTerminalRejects);
    CPU_SKIN_MT_ATOMIC_COUNTER(mxcsrMismatchRejects);
    CPU_SKIN_MT_ATOMIC_COUNTER(invalidNativeLeaseRejects);
#undef CPU_SKIN_MT_ATOMIC_COUNTER
  };

  AtomicCounters m_counters;

  friend class CpuSkinMtControllerJob;
};

class CpuSkinMtControllerJob {
public:
  CpuSkinMtControllerJob(
      const CpuSkinMtControllerFrozenKey& frozen,
      uint64_t activeGeneration,
      CpuSkinMtControllerTerminalLedger& ledger) noexcept;
  ~CpuSkinMtControllerJob() noexcept;

  CpuSkinMtControllerJob(const CpuSkinMtControllerJob&) = delete;
  CpuSkinMtControllerJob& operator=(
      const CpuSkinMtControllerJob&) = delete;

  CpuSkinMtControllerResult submit(uint64_t generation) noexcept;
  CpuSkinMtControllerResult publishProducerResult(
      uint64_t generation,
      std::shared_ptr<const CpuSkinMtControllerOwnedProducerResult> result)
      noexcept;
  CpuSkinMtControllerResult bindOuter(
      uint64_t generation,
      const CpuSkinMtControllerOuterBindingProof& outer) noexcept;
  CpuSkinMtControllerResult awaitKernel(
      uint64_t generation,
      const CpuSkinMtControllerDestinationProof& destination) noexcept;
  CpuSkinMtControllerKernelDecision trySelectKernelRoute(
      uint64_t generation,
      uint32_t currentMxcsr) noexcept;
  CpuSkinMtControllerResult copyProducerResultUnderLease(
      uint64_t generation,
      CpuSkinMtControllerCopyCallback callback,
      void* context) noexcept;
  CpuSkinMtControllerResult completeNativeBody(
      uint64_t generation,
      const CpuSkinMtControllerNativeBodyLease& lease,
      const CpuSkinMtControllerBodyCompletion& completion) noexcept;
  CpuSkinMtControllerResult noteUnlock(
      uint64_t generation,
      const CpuSkinMtControllerUnlockProof& unlock) noexcept;
  CpuSkinMtControllerResult cancelTemplateMismatch(
      uint64_t generation) noexcept;
  CpuSkinMtControllerResult cancelOuter(uint64_t generation) noexcept;
  CpuSkinMtControllerResult cancelForReset(
      uint64_t generation,
      uint64_t nextGeneration) noexcept;

  CpuSkinMtControllerState state() const noexcept;
  CpuSkinMtControllerTerminal terminal() const noexcept;
  uint64_t activeGeneration() const noexcept;
  bool producerResultPublished() const noexcept;
  bool producerResultConsumed() const noexcept;
  bool unlockObserved() const noexcept;
  CpuSkinMtControllerProducerResultProof producerResultProof() const noexcept;
  CpuSkinMtControllerRenderCommitEnvelopeProof renderCommitEnvelopeProof()
      const noexcept;
  CpuSkinMtControllerOuterSettlementSnapshot outerSettlement() const noexcept;

private:
  bool isTerminalLocked() const noexcept;
  void materializeAtomicRouteLocked() noexcept;
  CpuSkinMtControllerNativeBodyLease makeNativeBodyLease() const noexcept;
  bool nativeBodyLeaseMatchesLocked(
      const CpuSkinMtControllerNativeBodyLease& lease) const noexcept;
  CpuSkinMtControllerKernelDecision selectNativeWithoutWaiting(
      CpuSkinMtControllerKernelReason reason,
      bool stateLockAcquired) noexcept;
  CpuSkinMtControllerResult rejectStaleLocked() noexcept;
  CpuSkinMtControllerResult rejectInvalidLocked() noexcept;
  CpuSkinMtControllerResult rejectKeyLocked() noexcept;
  CpuSkinMtControllerResult rejectDoubleTerminalLocked() noexcept;
  void settleProducerConsumedLocked() noexcept;
  void settleProducerAbandonedLocked() noexcept;
  bool shouldDeferCancellationLocked() const noexcept;
  CpuSkinMtControllerResult queueDeferredCancellationLocked(
      CpuSkinMtControllerTerminal terminal,
      uint64_t nextGeneration) noexcept;
  CpuSkinMtControllerResult finishLocked(
      CpuSkinMtControllerState state,
      CpuSkinMtControllerTerminal terminal) noexcept;
  CpuSkinMtControllerResult finalizeRouteAfterUnlockLocked() noexcept;

  // The copy callback runs while m_mutex is held. A native route is protected
  // differently: the route CAS itself issues the exact NativeBodyLease, so a
  // concurrent reset/cancel can only queue a deferred terminal until both the
  // body and exact Unlock have settled.
  mutable std::mutex m_mutex;
  CpuSkinMtControllerFrozenKey m_frozen = {};
  CpuSkinMtControllerOuterBindingProof m_outer = {};
  CpuSkinMtControllerDestinationProof m_destination = {};
  CpuSkinMtControllerUnlockProof m_unlock = {};
  CpuSkinMtControllerBodyCompletion m_bodyCompletion = {};
  CpuSkinMtControllerMxcsrStatusDelta m_bodyMxcsrStatusDelta = {};
  CpuSkinMtControllerRenderCommitEnvelope m_commitEnvelope;
  CpuSkinMtControllerState m_state = CpuSkinMtControllerState::Invalid;
  CpuSkinMtControllerTerminal m_terminal =
      CpuSkinMtControllerTerminal::None;
  CpuSkinMtControllerTerminal m_deferredTerminal =
      CpuSkinMtControllerTerminal::None;
  uint64_t m_pendingResetGeneration = 0;
  uint64_t m_activeGeneration = 0;
  bool m_unlockObserved = false;
  bool m_bodyCompleted = false;
  bool m_bodySucceeded = false;
  bool m_producerPublicationSettled = false;
  bool m_producerClaimed = false;
  bool m_producerConsumed = false;
  bool m_outerSettlementOpen = false;
  bool m_nativeBodyLeaseLive = false;
  CpuSkinMtControllerKernelRoute m_route =
      CpuSkinMtControllerKernelRoute::None;
  std::atomic<CpuSkinMtControllerKernelRoute> m_routeAuthority{
      CpuSkinMtControllerKernelRoute::None};
  std::atomic<uint64_t> m_activeGenerationAtomic{0};
  std::atomic<bool> m_kernelWindowOpen{false};
  std::shared_ptr<const CpuSkinMtControllerOwnedProducerResult>
      m_producerResult;
  CpuSkinMtControllerTerminalLedger* m_ledger = nullptr;

  friend class CpuSkinMtControllerJobTestPeer;
};

#define CPU_SKIN_MT_ASSERT_POD(type)           \
  static_assert(std::is_standard_layout_v<type>); \
  static_assert(std::is_trivially_copyable_v<type>)
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerOwnerSessionProof);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerFlushBindingProof);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerImmutableSourceProof);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerPaletteProof);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerOuterBindingProof);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerDestinationProof);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerUnlockProof);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerFrozenKey);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtFormat2EligibilityInput);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerMxcsrStatusDelta);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerProducerResultProof);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerRenderCommitEnvelopeProof);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerNativeBodyLease);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerKernelDecision);
CPU_SKIN_MT_ASSERT_POD(CpuSkinMtControllerBodyCompletion);
#undef CPU_SKIN_MT_ASSERT_POD

static_assert(std::atomic<CpuSkinMtControllerKernelRoute>::is_always_lock_free,
              "kernel route CAS must be lock-free");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "render-owner generation and ledger atomics must be lock-free");
static_assert(std::atomic<bool>::is_always_lock_free,
              "kernel window proof must be lock-free");

}  // namespace dxvk::war3::gpu_skin
