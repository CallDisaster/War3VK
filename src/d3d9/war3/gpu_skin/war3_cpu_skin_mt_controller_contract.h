#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace dxvk::war3::gpu_skin {

// Phase 1 is deliberately value-only. None of these constants authorize a
// native hook, a mapped-VB write, parity acceptance, or production routing.
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
inline constexpr uint32_t kCpuSkinMtMxcsrControlMask = 0xffc0u;
inline constexpr uint32_t kCpuSkinMtMxcsrExceptionMask = 0x1f80u;
inline constexpr uint32_t kCpuSkinMtOutputProofVersion = 1u;

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
  ProducerReady,
  CommitClaimed,
  CopyInProgress,
  CopyAwaitingUnlock,
  NativeSelected,
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
  StaleGeneration,
  InvalidTransition,
  KeyMismatch,
  ProducerNotReady,
  CopyAfterUnlock,
  DoubleTerminal,
  LateProducerReady,
  MxcsrMismatch,
  MissingCopyCallback,
};

enum class CpuSkinMtControllerKernelRoute : uint8_t {
  None = 0,
  Copy,
  Native,
  Cancelled,
};

enum class CpuSkinMtControllerKernelReason : uint8_t {
  None = 0,
  ReadyOwnedOutput,
  ProducerNotReady,
  StateLockContended,
  MxcsrMismatch,
  AlreadyDecided,
  WindowClosed,
};

struct CpuSkinMtControllerKernelDecision {
  CpuSkinMtControllerResult result =
      CpuSkinMtControllerResult::InvalidTransition;
  CpuSkinMtControllerKernelRoute route =
      CpuSkinMtControllerKernelRoute::None;
  CpuSkinMtControllerKernelReason reason =
      CpuSkinMtControllerKernelReason::None;
  bool stateLockAcquired = false;
  bool selectedNow = false;
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

struct CpuSkinMtControllerExactKey {
  CpuSkinMtControllerOwnerSessionProof owner;
  CpuSkinMtControllerFlushBindingProof flush;
  CpuSkinMtControllerImmutableSourceProof source;
  CpuSkinMtControllerPaletteProof palette;
  CpuSkinMtControllerOuterBindingProof outer;
  CpuSkinMtControllerDestinationProof destination;
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
bool operator==(const CpuSkinMtControllerExactKey& lhs,
                const CpuSkinMtControllerExactKey& rhs) noexcept;

inline bool operator!=(const CpuSkinMtControllerExactKey& lhs,
                       const CpuSkinMtControllerExactKey& rhs) noexcept {
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
bool CpuSkinMtControllerExactKeyComplete(
    const CpuSkinMtControllerExactKey& key) noexcept;

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

uint32_t CpuSkinMtNormalizeMxcsrControl(uint32_t mxcsr) noexcept;
bool CpuSkinMtHasSafeMxcsrControl(uint32_t mxcsr) noexcept;
CpuSkinMtFormat2EligibilityResult CpuSkinMtValidateFormat2Eligibility(
    const CpuSkinMtFormat2EligibilityInput& input) noexcept;
bool CpuSkinMtEligibilityMatchesExactKey(
    const CpuSkinMtControllerExactKey& key,
    const CpuSkinMtFormat2EligibilityInput& input) noexcept;

struct CpuSkinMtControllerOutputProof {
  CpuSkinMtControllerExactKey exactKey;
  uint64_t resultSerial = 0;
  uint64_t producerGeneration = 0;
  uint64_t outputContentHash = 0;
  uintptr_t ownedBytesIdentity = 0;
  uint32_t outputByteSize = 0;
  uint32_t normalizedMxcsr = 0;
  uint32_t proofVersion = 0;
};

bool operator==(const CpuSkinMtControllerOutputProof& lhs,
                const CpuSkinMtControllerOutputProof& rhs) noexcept;

class CpuSkinMtControllerOwnedOutput final {
public:
  static std::shared_ptr<const CpuSkinMtControllerOwnedOutput> Create(
      const CpuSkinMtControllerExactKey& exactKey,
      const CpuSkinMtFormat2EligibilityInput& eligibility,
      uint64_t resultSerial,
      std::vector<uint8_t> bytes);

  CpuSkinMtControllerOwnedOutput(
      const CpuSkinMtControllerOwnedOutput&) = delete;
  CpuSkinMtControllerOwnedOutput& operator=(
      const CpuSkinMtControllerOwnedOutput&) = delete;

  const CpuSkinMtControllerOutputProof& proof() const noexcept;
  const uint8_t* data() const noexcept;
  uint32_t size() const noexcept;
  bool proofComplete() const noexcept;

private:
  CpuSkinMtControllerOwnedOutput(
      const CpuSkinMtControllerExactKey& exactKey,
      uint32_t normalizedMxcsr,
      uint64_t resultSerial,
      std::vector<uint8_t> bytes) noexcept;

  CpuSkinMtControllerOutputProof m_proof = {};
  std::vector<uint8_t> m_bytes;
};

struct CpuSkinMtControllerOutputView {
  const uint8_t* bytes = nullptr;
  uint32_t byteSize = 0;
  const CpuSkinMtControllerOutputProof* proof = nullptr;
};

using CpuSkinMtControllerCopyCallback = bool (*)(
    void* context,
    const CpuSkinMtControllerOutputView& output,
    const CpuSkinMtControllerDestinationProof& destination) noexcept;

struct CpuSkinMtControllerTerminalLedgerSnapshot {
  uint64_t jobsCreated = 0;
  uint64_t liveJobs = 0;
  uint64_t producerReadyPublications = 0;
  uint64_t copySelections = 0;
  uint64_t nativeSelections = 0;
  uint64_t liveCopyJobs = 0;
  uint64_t liveNativeJobs = 0;
  uint64_t guardedCopies = 0;
  uint64_t guardedCopyBodyFaults = 0;
  uint64_t successfulUnlocks = 0;
  uint64_t failedUnlocks = 0;
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
    std::atomic<uint64_t> jobsCreated{0};
    std::atomic<uint64_t> liveJobs{0};
    std::atomic<uint64_t> producerReadyPublications{0};
    std::atomic<uint64_t> copySelections{0};
    std::atomic<uint64_t> nativeSelections{0};
    std::atomic<uint64_t> liveCopyJobs{0};
    std::atomic<uint64_t> liveNativeJobs{0};
    std::atomic<uint64_t> guardedCopies{0};
    std::atomic<uint64_t> guardedCopyBodyFaults{0};
    std::atomic<uint64_t> successfulUnlocks{0};
    std::atomic<uint64_t> failedUnlocks{0};
    std::atomic<uint64_t> copiedNormal{0};
    std::atomic<uint64_t> originalNormal{0};
    std::atomic<uint64_t> originalFault{0};
    std::atomic<uint64_t> copyFault{0};
    std::atomic<uint64_t> templateMismatch{0};
    std::atomic<uint64_t> resetCancelled{0};
    std::atomic<uint64_t> outerCancelled{0};
    std::atomic<uint64_t> copyCancelled{0};
    std::atomic<uint64_t> nativeCancelled{0};
    std::atomic<uint64_t> unselectedCancelled{0};
    std::atomic<uint64_t> staleGenerationRejects{0};
    std::atomic<uint64_t> invalidTransitionRejects{0};
    std::atomic<uint64_t> keyMismatchRejects{0};
    std::atomic<uint64_t> producerNotReadyRejects{0};
    std::atomic<uint64_t> lateProducerReadyRejects{0};
    std::atomic<uint64_t> copyAfterUnlockRejects{0};
    std::atomic<uint64_t> doubleTerminalRejects{0};
    std::atomic<uint64_t> mxcsrMismatchRejects{0};
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
  CpuSkinMtControllerResult bindOuter(
      uint64_t generation,
      const CpuSkinMtControllerOuterBindingProof& outer) noexcept;
  CpuSkinMtControllerResult awaitKernel(
      uint64_t generation,
      const CpuSkinMtControllerDestinationProof& destination) noexcept;
  CpuSkinMtControllerResult publishProducerReady(
      uint64_t generation,
      std::shared_ptr<const CpuSkinMtControllerOwnedOutput> output) noexcept;
  CpuSkinMtControllerKernelDecision trySelectKernelRoute(
      uint64_t generation,
      uint32_t currentMxcsr) noexcept;
  CpuSkinMtControllerResult copyOutputUnderLease(
      uint64_t generation,
      CpuSkinMtControllerCopyCallback callback,
      void* context) noexcept;
  CpuSkinMtControllerResult completeOriginalNormal(
      uint64_t generation) noexcept;
  CpuSkinMtControllerResult completeOriginalFault(
      uint64_t generation) noexcept;
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
  bool producerReady() const noexcept;
  bool unlockObserved() const noexcept;
  CpuSkinMtControllerExactKey exactKey() const noexcept;
  CpuSkinMtControllerOutputProof outputProof() const noexcept;

private:
  bool isTerminalLocked() const noexcept;
  void materializeAtomicRouteLocked() noexcept;
  CpuSkinMtControllerKernelDecision selectNativeWithoutWaiting(
      CpuSkinMtControllerKernelReason reason,
      bool stateLockAcquired) noexcept;
  CpuSkinMtControllerResult rejectStaleLocked() noexcept;
  CpuSkinMtControllerResult rejectInvalidLocked() noexcept;
  CpuSkinMtControllerResult rejectKeyLocked() noexcept;
  CpuSkinMtControllerResult rejectDoubleTerminalLocked() noexcept;
  CpuSkinMtControllerResult finishLocked(
      CpuSkinMtControllerState state,
      CpuSkinMtControllerTerminal terminal) noexcept;
  CpuSkinMtControllerResult finalizeRouteAfterUnlockLocked() noexcept;

  // copyOutputUnderLease holds this mutex across the actual callback. This is
  // the Phase-1 linearization point: matching Unlock/reset/cancel cannot pass
  // it and therefore can never authorize a byte write that occurs later.
  mutable std::mutex m_mutex;
  CpuSkinMtControllerExactKey m_key = {};
  CpuSkinMtControllerUnlockProof m_unlock = {};
  CpuSkinMtControllerState m_state = CpuSkinMtControllerState::Invalid;
  CpuSkinMtControllerTerminal m_terminal =
      CpuSkinMtControllerTerminal::None;
  uint64_t m_activeGeneration = 0;
  bool m_unlockObserved = false;
  bool m_bodyCompleted = false;
  bool m_bodySucceeded = false;
  CpuSkinMtControllerKernelRoute m_route =
      CpuSkinMtControllerKernelRoute::None;
  std::atomic<CpuSkinMtControllerKernelRoute> m_routeAuthority{
      CpuSkinMtControllerKernelRoute::None};
  std::atomic<uint64_t> m_activeGenerationAtomic{0};
  std::atomic<bool> m_kernelWindowOpen{false};
  std::shared_ptr<const CpuSkinMtControllerOwnedOutput> m_output;
  CpuSkinMtControllerTerminalLedger* m_ledger = nullptr;

  friend class CpuSkinMtControllerJobTestPeer;
};

static_assert(std::is_standard_layout_v<
              CpuSkinMtControllerOwnerSessionProof>);
static_assert(std::is_trivially_copyable_v<
              CpuSkinMtControllerOwnerSessionProof>);
static_assert(std::is_standard_layout_v<
              CpuSkinMtControllerFlushBindingProof>);
static_assert(std::is_trivially_copyable_v<
              CpuSkinMtControllerFlushBindingProof>);
static_assert(std::is_standard_layout_v<
              CpuSkinMtControllerImmutableSourceProof>);
static_assert(std::is_trivially_copyable_v<
              CpuSkinMtControllerImmutableSourceProof>);
static_assert(std::is_standard_layout_v<
              CpuSkinMtControllerPaletteProof>);
static_assert(std::is_trivially_copyable_v<
              CpuSkinMtControllerPaletteProof>);
static_assert(std::is_standard_layout_v<
              CpuSkinMtControllerOuterBindingProof>);
static_assert(std::is_trivially_copyable_v<
              CpuSkinMtControllerOuterBindingProof>);
static_assert(std::is_standard_layout_v<
              CpuSkinMtControllerDestinationProof>);
static_assert(std::is_trivially_copyable_v<
              CpuSkinMtControllerDestinationProof>);
static_assert(std::is_standard_layout_v<
              CpuSkinMtControllerUnlockProof>);
static_assert(std::is_trivially_copyable_v<
              CpuSkinMtControllerUnlockProof>);
static_assert(std::is_standard_layout_v<CpuSkinMtControllerFrozenKey>);
static_assert(std::is_trivially_copyable_v<CpuSkinMtControllerFrozenKey>);
static_assert(std::is_standard_layout_v<CpuSkinMtControllerExactKey>);
static_assert(std::is_trivially_copyable_v<CpuSkinMtControllerExactKey>);
static_assert(std::is_standard_layout_v<CpuSkinMtFormat2EligibilityInput>);
static_assert(std::is_trivially_copyable_v<CpuSkinMtFormat2EligibilityInput>);
static_assert(std::is_standard_layout_v<CpuSkinMtControllerOutputProof>);
static_assert(std::is_trivially_copyable_v<CpuSkinMtControllerOutputProof>);
static_assert(std::is_standard_layout_v<CpuSkinMtControllerKernelDecision>);
static_assert(std::is_trivially_copyable_v<
              CpuSkinMtControllerKernelDecision>);
static_assert(std::atomic<CpuSkinMtControllerKernelRoute>::is_always_lock_free,
              "kernel route CAS must be lock-free");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "render-owner generation and ledger atomics must be lock-free");
static_assert(std::atomic<bool>::is_always_lock_free,
              "kernel window proof must be lock-free");

}  // namespace dxvk::war3::gpu_skin
