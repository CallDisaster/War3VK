#pragma once

#include "war3_gpu_skin_types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dxvk::war3::gpu_skin {

// Skipping the complete +0x68 upload cannot preserve the native VB Lock/SEH
// contract. P4 may only reopen through the post-Lock CPU skin-kernel hook.
inline constexpr bool kWholeNativeUploadBypassEnabled = false;
inline constexpr uintptr_t kNativeSkinCopyKernelRva = 0x0EDDC0u;
inline constexpr uint64_t kNativeBeginTimingSamplePeriod = 127u;
// Every Nth strict outside/no-upload DIP keeps the complete legacy observer
// transaction as a live control for the lighter pre-observer reader cover.
inline constexpr uint64_t kNativeOutsideDipReaderEvidencePeriod = 127u;
inline constexpr uint64_t kNativeOutsidePoisonAuthorityEvidencePeriod = 127u;
// Production-light raw-QPC timing has an independent, coprime population.
// The non-zero phase prevents cold-start alignment with the existing 127th
// evidence sample while retaining an exact 1/256 predicate. Outer and kernel
// hooks own separate TLS ordinals: each supports its own inclusive closure,
// but their sampled totals must never be subtracted from one another.
inline constexpr uint64_t kNativeProductionTimingSamplePeriod = 256u;
inline constexpr uint64_t kNativeProductionTimingSamplePhase = 0xa5u;
inline constexpr uint32_t kNativeDispatchCpuOnlySealViewCapacity = 512u;
inline constexpr size_t kNativeOutsideDipFastRejectReasonCount = 8u;
inline constexpr size_t kNativeOutsideDipFastLocalRejectReasonCount = 7u;
inline constexpr uint32_t kNativeOutsideDipFastEarlySamplePhase = 0x39u;
inline constexpr uint32_t kNativeOutsideDipFastLateSamplePhase = 0xd7u;
inline constexpr size_t kNativeOutsideUploadAdmissionClassCount = 6u;
inline constexpr size_t kNativeOutsidePoisonShadowMatrixSize = 9u;
inline constexpr size_t kNativeOutsidePoisonO1ShadowMatrixSize = 9u;
inline constexpr uint64_t kNativeOutsidePoisonO1ShadowAuthority = 0u;

// Diagnostics-only terminal reasons for an O0 poison-scan/vertex-Lock shadow
// probe that could not produce an independently comparable three-state
// result. Outer cancellation and reset abortion are disjoint terminal classes
// and therefore deliberately absent from this list.
enum class NativeOutsidePoisonShadowUnprovableReason : uint8_t {
  NoLock = 0,
  MultipleLocks,
  OwnerOrLifo,
  Reentry,
  ResetOrRetirement,
  PoisonMutation,
  FormatOrFvf,
  LockDescriptor,
  ResourceIdentity,
  StorageIdentity,
  Range,
  KernelNotNormal,
  Count,
};

inline constexpr size_t kNativeOutsidePoisonShadowUnprovableReasonCount =
    static_cast<size_t>(
        NativeOutsidePoisonShadowUnprovableReason::Count);
inline constexpr std::array<
    const char*, kNativeOutsidePoisonShadowUnprovableReasonCount>
    kNativeOutsidePoisonShadowUnprovableReasonNames = {{
        "noLock",
        "multipleLocks",
        "ownerOrLifo",
        "reentry",
        "resetOrRetirement",
        "poisonMutation",
        "formatOrFvf",
        "lockDescriptor",
        "resourceIdentity",
        "storageIdentity",
        "range",
        "kernelNotNormal",
    }};
static_assert(
    kNativeOutsidePoisonShadowUnprovableReasonNames.size() ==
        kNativeOutsidePoisonShadowUnprovableReasonCount);

// O1a remains report-only. The first twelve values retain O0's parser order,
// but O1a derives every verdict independently and never lifts an O0 failure.
// Appended reasons describe the independent 8-byte kernel-state, exact Unlock,
// and outer normal-return contract needed by a future poison settlement.
enum class NativeOutsidePoisonO1ShadowUnprovableReason : uint8_t {
  NoLock = 0,
  MultipleLocks,
  OwnerOrLifo,
  Reentry,
  ResetOrRetirement,
  PoisonMutation,
  FormatOrFvf,
  LockDescriptor,
  ResourceIdentity,
  StorageIdentity,
  Range,
  KernelNotNormal,
  KernelNotObserved,
  MultipleKernels,
  KernelStateRead,
  KernelMode,
  KernelFormat,
  KernelMappedDst,
  UnlockNotObserved,
  MultipleUnlocks,
  UnlockBeforeFreeze,
  UnlockFailed,
  UnlockIdentity,
  UnlockGeneration,
  OuterResult,
  Count,
};

inline constexpr size_t kNativeOutsidePoisonO1ShadowUnprovableReasonCount =
    static_cast<size_t>(
        NativeOutsidePoisonO1ShadowUnprovableReason::Count);
inline constexpr std::array<
    const char*, kNativeOutsidePoisonO1ShadowUnprovableReasonCount>
    kNativeOutsidePoisonO1ShadowUnprovableReasonNames = {{
        "noLock",
        "multipleLocks",
        "ownerOrLifo",
        "reentry",
        "resetOrRetirement",
        "poisonMutation",
        "formatOrFvf",
        "lockDescriptor",
        "resourceIdentity",
        "storageIdentity",
        "range",
        "kernelNotNormal",
        "kernelNotObserved",
        "multipleKernels",
        "kernelStateRead",
        "kernelMode",
        "kernelFormat",
        "kernelMappedDst",
        "unlockNotObserved",
        "multipleUnlocks",
        "unlockBeforeFreeze",
        "unlockFailed",
        "unlockIdentity",
        "unlockGeneration",
        "outerResult",
    }};
static_assert(
    kNativeOutsidePoisonO1ShadowUnprovableReasonNames.size() ==
        kNativeOutsidePoisonO1ShadowUnprovableReasonCount);

// O1a-v2 scans only after a successful vertex Lock has published its active
// lock and mapped pointer. These four lanes keep first-cause accounting exact
// without granting any runtime authority.
enum class NativeOutsidePoisonO1LockLane : uint8_t {
  BufferNoOverwrite = 0,
  BufferDiscard,
  DirectNoOverwrite,
  DirectDiscard,
  Count,
};

inline constexpr size_t kNativeOutsidePoisonO1LockLaneCount =
    static_cast<size_t>(NativeOutsidePoisonO1LockLane::Count);
inline constexpr std::array<
    const char*, kNativeOutsidePoisonO1LockLaneCount>
    kNativeOutsidePoisonO1LockLaneNames = {{
        "bufferNoOverwrite",
        "bufferDiscard",
        "directNoOverwrite",
        "directDiscard",
    }};

// Mutually exclusive first cause for a logical scanner R verdict. Storage
// generations are deliberately absent: they are observation aids, not part of
// SameCpuOverwriteTarget's logical content identity.
enum class NativeOutsidePoisonO1ScanFailureReason : uint8_t {
  LedgerIncomplete = 0,
  CurrentIncomplete,
  PoisonIncomplete,
  PartialIdentity,
  Device,
  Layout,
  ResourceGeneration,
  Range,
  Count,
};

inline constexpr size_t kNativeOutsidePoisonO1ScanFailureReasonCount =
    static_cast<size_t>(NativeOutsidePoisonO1ScanFailureReason::Count);
inline constexpr size_t kNativeOutsidePoisonO1LaneFailureCount =
    kNativeOutsidePoisonO1LockLaneCount *
    kNativeOutsidePoisonO1ScanFailureReasonCount;
inline constexpr std::array<
    const char*, kNativeOutsidePoisonO1ScanFailureReasonCount>
    kNativeOutsidePoisonO1ScanFailureReasonNames = {{
        "ledgerIncomplete",
        "currentIncomplete",
        "poisonIncomplete",
        "partialIdentity",
        "device",
        "layout",
        "resourceGeneration",
        "range",
    }};

enum class NativeOutsidePoisonO1UnlockDriftComponent : uint8_t {
  Resource = 0,
  Real,
  Mapping,
  MapAllocation,
  Count,
};

inline constexpr size_t kNativeOutsidePoisonO1UnlockDriftComponentCount =
    static_cast<size_t>(NativeOutsidePoisonO1UnlockDriftComponent::Count);
inline constexpr std::array<
    const char*, kNativeOutsidePoisonO1UnlockDriftComponentCount>
    kNativeOutsidePoisonO1UnlockDriftComponentNames = {{
        "resource",
        "real",
        "mapping",
        "mapAllocation",
    }};

static_assert(kNativeOutsidePoisonO1LockLaneNames.size() ==
              kNativeOutsidePoisonO1LockLaneCount);
static_assert(kNativeOutsidePoisonO1ScanFailureReasonNames.size() ==
              kNativeOutsidePoisonO1ScanFailureReasonCount);
static_assert(kNativeOutsidePoisonO1UnlockDriftComponentNames.size() ==
              kNativeOutsidePoisonO1UnlockDriftComponentCount);

enum class NativeProductionTimingStage : uint8_t {
  OuterAdmissionAccepted = 0,
  OuterAdmissionRejected,
  OuterFastInclusive,
  OuterFastBody,
  OuterFastComplete,
  OuterFastCancel,
  KernelInclusive,
  KernelEvaluate,
  KernelOriginal,
  KernelNotify,
  // Appended v2 stages. Keep every v1 numeric value stable for parsers that
  // consume a mixed deployment while diagnostics are being rolled forward.
  OuterFallbackInclusive,
  OuterFallbackBegin,
  OuterFallbackBody,
  OuterFallbackComplete,
  // Appended v3 event-graph stages. These are independent 1/256 physical
  // event populations and must not be subtracted from stable-id callback
  // samples. Multi-stage Semantic and DIP samples publish in one writer
  // generation so their call/tick closure can be evaluated atomically.
  FlushRoot,
  DispatchSemanticLookup,
  DispatchBeginRoot,
  DispatchEndRoot,
  SemanticInclusive,
  SemanticOriginal,
  DipDeviceRootOutside,
  DipDeviceRootNoUpload,
  DipDeviceRootCorrelated,
  DipBridgeOutside,
  DipBridgeNoUpload,
  DipBridgeCorrelated,
  DipResolveOutside,
  DipResolveNoUpload,
  DipResolveCorrelated,
  // Appended v4 outer-admission aliases. The bucket carried by the timing
  // entry selects one accepted class or one exact first-reject reason; these
  // aliases partition existing roots and must never be added to them.
  OuterAdmissionAcceptedClass,
  OuterFastCompleteClass,
  OuterFallbackReason,
  // Appended v5 independent exact-negative dispatch-seal route. These stages
  // do not participate in the six-class outside aliases above.
  OuterDispatchSealAdmission,
  OuterDispatchSealInclusive,
  OuterDispatchSealBody,
  OuterDispatchSealComplete,
  OuterDispatchSealCancel,
};

enum class NativeOutsideUploadAdmissionClass : uint8_t {
  NoPoisonFlush = 0,
  NoPoisonIndependent,
  PoisonFlush,
  PoisonIndependent,
  NoPoisonSemantic,
  PoisonSemantic,
  Count,
};

enum class NativeOutsideUploadCoverKind : uint8_t {
  Flush = 0,
  Semantic,
  Independent,
  Count,
};

// Exact first failing predicate in TryBeginNativeOutsideUploadFastPath. Keep
// Unknown at zero as a hard-zero corruption sentinel. New reasons may only be
// appended so report parsers can consume mixed deployments safely.
enum class NativeOutsideUploadRejectReason : uint8_t {
  Unknown = 0,
  ActiveFastMarker,
  ModeNotBypass,
  FullDiagnostics,
  IngressClosed,
  BypassDisabled,
  WrongThread,
  NullDevice,
  DispatchOwned,
  SemanticScopeHazard,
  DispatchOverflow,
  SemanticOverflow,
  NestedUpload,
  GenericUploadInFlight,
  FastRejectUploadInFlight,
  EvidenceUploadInFlight,
  RetirementPending,
  RetirementQueueFault,
  ResetGenerationMismatch,
  EvidenceCohort,
  PoisonReadFailure,
  PoisonOverlap,
  PoisonPostScanRevalidation,
  IndependentPinRevalidation,
  Count,
};

inline constexpr size_t kNativeOutsideUploadRejectReasonCount =
    static_cast<size_t>(NativeOutsideUploadRejectReason::Count);
static_assert(
    kNativeOutsideUploadAdmissionClassCount ==
        static_cast<size_t>(NativeOutsideUploadAdmissionClass::Count));
static_assert(kNativeOutsideUploadRejectReasonCount == 24u);

inline constexpr std::array<
    const char*, kNativeOutsideUploadAdmissionClassCount>
    kNativeOutsideUploadAdmissionClassNames = {{
        "noPoisonFlush",
        "noPoisonIndependent",
        "poisonFlush",
        "poisonIndependent",
        "noPoisonSemantic",
        "poisonSemantic",
    }};
inline constexpr std::array<const char*, kNativeOutsideUploadRejectReasonCount>
    kNativeOutsideUploadRejectReasonNames = {{
        "unknown",
        "activeFastMarker",
        "modeNotBypass",
        "fullDiagnostics",
        "ingressClosed",
        "bypassDisabled",
        "wrongThread",
        "nullDevice",
        "dispatchOwned",
        "semanticScopeHazard",
        "dispatchOverflow",
        "semanticOverflow",
        "nestedUpload",
        "genericUploadInFlight",
        "fastRejectUploadInFlight",
        "evidenceUploadInFlight",
        "retirementPending",
        "retirementQueueFault",
        "resetGenerationMismatch",
        "evidenceCohort",
        "poisonReadFailure",
        "poisonOverlap",
        "poisonPostScanRevalidation",
        "independentPinRevalidation",
    }};
static_assert(kNativeOutsideUploadAdmissionClassNames.size() ==
              kNativeOutsideUploadAdmissionClassCount);
static_assert(kNativeOutsideUploadRejectReasonNames.size() ==
              kNativeOutsideUploadRejectReasonCount);

enum class NativeBridgeCallbackKind : uint8_t {
  Flush = 0,
  DispatchBegin,
  DispatchEnd,
  Preflight,
  CpuRewrite,
  Upload,
  Dip,
  Fanout,
  Count,
};

inline constexpr size_t kNativeBridgeCallbackKindCount =
    static_cast<size_t>(NativeBridgeCallbackKind::Count);

struct NativeSampledRawTiming {
  uint64_t calls = 0;
  uint64_t ticks = 0;
  uint64_t maxTicks = 0;
};

struct NativeProductionTimingEntry {
  NativeProductionTimingStage stage =
      NativeProductionTimingStage::OuterAdmissionRejected;
  uint64_t ticks = 0;
  // Only the three appended alias stages consume this field. Existing stage
  // aggregate initializers remain source-compatible and default to bucket 0.
  uint8_t bucket = 0;
};

struct NativeOutsideUploadAdmissionProbe {
  NativeOutsideUploadAdmissionClass acceptedClass =
      NativeOutsideUploadAdmissionClass::Count;
  NativeOutsideUploadRejectReason rejectReason =
      NativeOutsideUploadRejectReason::Unknown;
};

// Hook-local identity for the exact production-light outside population.
// Tracked means TryBegin observed raw dispatchDepth == 0 and therefore
// published one accepted/reject attempt. Untracked means a dispatch owner was
// present at that same entry boundary; only a later generic-upload terminal can
// decide whether that call ultimately contributed to uploadsOutsideDispatch.
// None keeps full/off modes exactly zero. The token is passed by value through
// NativeUploadCall/Observation so nested or re-entrant uploads cannot overwrite
// one another through shared TLS.
enum class NativeOutsideUploadPopulationClass : uint8_t {
  None = 0,
  Tracked,
  Untracked,
};

struct NativeOutsideUploadPopulationToken {
  NativeOutsideUploadPopulationClass populationClass =
      NativeOutsideUploadPopulationClass::None;
  bool lifecycleAlreadyExcluded = false;
};

enum class NativeDispatchPath : uint8_t {
  Unknown = 0,
  Common,
  Special,
  Transparent,
  MultiPass,
};

// Report-only origin of a physical DIP relative to the native dispatch
// contract. This never authorizes a takeover or changes suppression.
enum class NativeDipScope : uint8_t {
  Unknown = 0,
  ActiveUpload,
  OutsideDispatch,
  DispatchNoUpload,
  ScopeHazard,
};

// Mutually exclusive reason why the fail-closed poison ledger selected a
// range. ExactRangeOverlap is the only interval-proven case; the others are
// conservative whole-resource fallbacks.
enum class NativePoisonHitKind : uint8_t {
  None = 0,
  IncompleteIdentity,
  LayoutMismatch,
  InexactRange,
  ExactRangeOverlap,
};

enum NativeDispatchSemanticFailure : uint32_t {
  NativeDispatchSemanticFailureNone = 0u,
  NativeDispatchSemanticFailureQueryMiss = 1u << 0,
  NativeDispatchSemanticFailureConflict = 1u << 1,
  NativeDispatchSemanticFailureUnknown = 1u << 2,
  NativeDispatchSemanticFailureLayerMismatch = 1u << 3,
};

enum NativeFingerprintFailure : uint32_t {
  NativeFingerprintFailureNone = 0,
  NativeFingerprintFailureAddressBook = 1u << 0,
  NativeFingerprintFailurePeImage = 1u << 1,
  NativeFingerprintFailureVersion = 1u << 2,
  NativeFingerprintFailureFileHash = 1u << 3,
  NativeFingerprintFailureOpcodes = 1u << 4,
  NativeFingerprintFailureVtable = 1u << 5,
};

enum NativePeFingerprintBit : uint32_t {
  NativePeFingerprintDosReadable = 1u << 0,
  NativePeFingerprintDosSignature = 1u << 1,
  NativePeFingerprintPeOffset = 1u << 2,
  NativePeFingerprintNtReadable = 1u << 3,
  NativePeFingerprintNtSignature = 1u << 4,
  NativePeFingerprintMachine = 1u << 5,
  NativePeFingerprintOptionalMagic = 1u << 6,
  NativePeFingerprintImageBase = 1u << 7,
  NativePeFingerprintTimestamp = 1u << 8,
  NativePeFingerprintImageSize = 1u << 9,
  NativePeFingerprintChecksum = 1u << 10,
  NativePeFingerprintImageBasePreferred = 1u << 11,
  NativePeFingerprintImageBaseRuntimeRelocated = 1u << 12,
};

inline constexpr uint32_t kNativePeFingerprintRequiredMask =
    NativePeFingerprintDosReadable |
    NativePeFingerprintDosSignature |
    NativePeFingerprintPeOffset |
    NativePeFingerprintNtReadable |
    NativePeFingerprintNtSignature |
    NativePeFingerprintMachine |
    NativePeFingerprintOptionalMagic |
    NativePeFingerprintImageBase |
    NativePeFingerprintTimestamp |
    NativePeFingerprintImageSize |
    NativePeFingerprintChecksum;

enum NativeOpcodeFingerprintFailure : uint32_t {
  NativeOpcodeFailureNone = 0,
  NativeOpcodeFailureApplyPrefix = 1u << 0,
  NativeOpcodeFailureApplyTail = 1u << 1,
  NativeOpcodeFailureUploadPrefix = 1u << 2,
  NativeOpcodeFailureUploadEh4 = 1u << 3,
  NativeOpcodeFailureUploadTail = 1u << 4,
  NativeOpcodeFailureKernelPrefix = 1u << 5,
  NativeOpcodeFailureKernelTail = 1u << 6,
  NativeOpcodeFailureLockVertexRingCall = 1u << 7,
  NativeOpcodeFailureSkinCopyKernelCall = 1u << 8,
  NativeOpcodeFailureFlushPrefix = 1u << 9,
  NativeOpcodeFailureDispatchPrefix = 1u << 10,
};

enum NativePreflightBit : uint64_t {
  NativePreflightExactFingerprint = 1ull << 0,
  NativePreflightCommonDispatch = 1ull << 1,
  NativePreflightStage11 = 1ull << 2,
  NativePreflightWorldObjectsTag = 1ull << 3,
  NativePreflightSinglePassCaller = 1ull << 4,
  NativePreflightSkinMode1 = 1ull << 5,
  NativePreflightSupportedFormat = 1ull << 6,
  NativePreflightVertexCountMatch = 1ull << 7,
  NativePreflightPositionSourceMatch = 1ull << 8,
  NativePreflightPositionStride12 = 1ull << 9,
  NativePreflightNormalSourceMatch = 1ull << 10,
  NativePreflightNormalStride12 = 1ull << 11,
  NativePreflightGroupSlotsMatch = 1ull << 12,
  NativePreflightGroupStride1 = 1ull << 13,
  NativePreflightPalettePresent = 1ull << 14,
  NativePreflightPaletteCountValid = 1ull << 15,
  NativePreflightPaletteReadable = 1ull << 16,
  NativePreflightUvLayoutSupported = 1ull << 17,
  NativePreflightOriginalExecuted = 1ull << 18,
  NativePreflightOriginalSucceeded = 1ull << 19,
  NativePreflightNativeRingReady = 1ull << 20,
  NativePreflightResourceGenerationReady = 1ull << 21,
  NativePreflightStaticResourceReady = 1ull << 22,
  NativePreflightPaletteCopied = 1ull << 23,
  NativePreflightGpuOutputReady = 1ull << 24,
  NativePreflightDipEpochMatch = 1ull << 25,
  NativePreflightDipStreamMatch = 1ull << 26,
  NativePreflightDipBaseVertexMatch = 1ull << 27,
  NativePreflightNoUnsupportedDrawPath = 1ull << 28,
  NativePreflightNativeBuffersInitialized = 1ull << 29,
  NativePreflightNativeRingReplayable = 1ull << 30,
  NativePreflightBatchSubmitted = 1ull << 31,
  NativePreflightExactGpuJob = 1ull << 32,
  NativePreflightAllConsumersLeased = 1ull << 33,
  NativePreflightHostDrawSafe = 1ull << 34,
  NativePreflightIndexPathPredicted = 1ull << 35,
  NativePreflightStaticInputsMatch = 1ull << 36,
  NativePreflightOuterUploadInFlight = 1ull << 37,
  NativePreflightKernelMappedDestination = 1ull << 38,
  NativePreflightKernelStateExact = 1ull << 39,
  NativePreflightCpuBaselineExact = 1ull << 40,
  NativePreflightBypassKeyNotFused = 1ull << 41,
  NativePreflightIndexPathActualProof = 1ull << 42,
  NativePreflightOriginalKernelReturnedNormally = 1ull << 43,
};

// manager 侧预授权的兼容名称。它只表示预测；只有 bridge 可以发布
// ActualProof。
inline constexpr uint64_t NativePreflightIndexPathSafe =
    NativePreflightIndexPathPredicted;

// 24/32 位最初只服务 compute output/job。VS-B1 不得改变 ABI-9 位布局，
// 因而把同一位解释为“当前执行路线的 GPU consumer capability / exact work
// contract 已闭合”。compute 仍使用旧名称，input-only 路线使用下列别名。
inline constexpr uint64_t NativePreflightGpuConsumerCapabilityReady =
    NativePreflightGpuOutputReady;
inline constexpr uint64_t NativePreflightExactGpuWorkContract =
    NativePreflightExactGpuJob;

inline constexpr uint64_t kNativeUploadInputRequiredPreflight =
    NativePreflightExactFingerprint |
    NativePreflightCommonDispatch |
    NativePreflightStage11 |
    NativePreflightWorldObjectsTag |
    NativePreflightSinglePassCaller |
    NativePreflightSkinMode1 |
    NativePreflightSupportedFormat |
    NativePreflightVertexCountMatch |
    NativePreflightPositionSourceMatch |
    NativePreflightPositionStride12 |
    NativePreflightNormalSourceMatch |
    NativePreflightNormalStride12 |
    NativePreflightGroupSlotsMatch |
    NativePreflightGroupStride1 |
    NativePreflightPalettePresent |
    NativePreflightPaletteCountValid |
    NativePreflightPaletteReadable |
    NativePreflightUvLayoutSupported;

inline constexpr size_t kNativeUploadInputPreflightBitCount = 18u;
static_assert(kNativeUploadInputRequiredPreflight ==
                   ((uint64_t{1} << kNativeUploadInputPreflightBitCount) - 1u),
               "input preflight diagnostics require contiguous low bits");

// 这五个位可以只从 hook 的 TLS dispatch/semantic scope 取得，不必读取
// Game.dll 持有的 vertex、geoset 或 GX device 内存。Bypass 用它们作为产品
// T0 门；Observe/Dual 则保留完整证据路径来验证合同。
inline constexpr uint64_t kNativeUploadScopeRequiredPreflight =
    NativePreflightExactFingerprint |
    NativePreflightCommonDispatch |
    NativePreflightStage11 |
    NativePreflightWorldObjectsTag |
    NativePreflightSinglePassCaller;

inline constexpr uint64_t kNativeObservationRequiredPreflight =
    kNativeUploadInputRequiredPreflight |
    NativePreflightOriginalExecuted |
    NativePreflightOriginalKernelReturnedNormally |
    NativePreflightOriginalSucceeded |
    NativePreflightNativeRingReady;

inline constexpr uint64_t kP4BypassRequiredPreflight =
    kNativeUploadInputRequiredPreflight |
    NativePreflightNativeBuffersInitialized |
    NativePreflightNativeRingReplayable |
    NativePreflightResourceGenerationReady |
    NativePreflightStaticResourceReady |
    NativePreflightPaletteCopied |
    NativePreflightGpuConsumerCapabilityReady |
    NativePreflightBatchSubmitted |
    NativePreflightExactGpuWorkContract |
    NativePreflightStaticInputsMatch |
    NativePreflightAllConsumersLeased |
    NativePreflightHostDrawSafe |
    NativePreflightIndexPathPredicted |
    NativePreflightNoUnsupportedDrawPath |
    NativePreflightOuterUploadInFlight |
    NativePreflightKernelMappedDestination |
    NativePreflightKernelStateExact |
    NativePreflightCpuBaselineExact |
    NativePreflightBypassKeyNotFused;

inline constexpr uint64_t kFutureTakeoverRequiredPreflight =
    kP4BypassRequiredPreflight |
    NativePreflightIndexPathActualProof |
    NativePreflightDipEpochMatch |
    NativePreflightDipStreamMatch |
    NativePreflightDipBaseVertexMatch;

enum class NativeBypassFailureReason : uint8_t {
  None = 0,
  ModeNotBypass,
  MissingInFlightUpload,
  NullMappedDestination,
  ThreadMismatch,
  SelfMismatch,
  EpochMismatch,
  DuplicateKernelCall,
  InputPreflightFailed,
  ManagerRejected,
  AuthorizationMismatch,
  NativeStateStale,
  DispatchScopeFailClosed,
  BridgeResetPending,
  OriginalUploadFailed,
  PostSkipMismatch,
  PoisonLedgerOverflow,
};

// The kernel detour must never encode an irreversible decision as bool false.
// Only the two CallOriginal outcomes permit the hook layer to enter the native
// trampoline.
enum class NativeKernelDetourOutcome : uint8_t {
  CallOriginal = 0,
  BypassCommitted,
  IrreversibleNoCpuRescue,
  // CPU-only passthrough with no in-flight observation. The hook still calls
  // the original trampoline exactly once (including NULL under caller SEH).
  // O0/O1a and a provisional production O1 token may still consume the real
  // normal-return endpoint; this value only suppresses the generic upload
  // observation notification.
  CallOriginalNoNotify,
};

enum class NativePoisonLedgerResetReason : uint8_t {
  Explicit = 0,
  MapEpoch,
  DeviceEpoch,
  BridgeDisabled,
};

enum class NativeBridgeResetStatus : uint8_t {
  Completed = 0,
  DeferredNoObservedRenderThread,
  DeferredWrongThread,
  DeferredActiveTransactions,
  DeferredOwnerEpoch,
  DeferredPoisonRetirement,
  DeferredRetirementQueueFault,
};

struct NativeBridgeResetResult {
  NativeBridgeResetStatus status =
      NativeBridgeResetStatus::DeferredNoObservedRenderThread;
  NativePoisonLedgerResetReason reason =
      NativePoisonLedgerResetReason::Explicit;
  uint64_t requestedGeneration = 0;
  uint64_t completedGeneration = 0;
  uint32_t poisonRangesCleared = 0;
  uint32_t poisonRangesOutstanding = 0;
  uint32_t retirementEventsPending = 0;
  bool currentThreadIsObservedRenderThread = false;
  bool currentThreadTlsQuiescent = false;
  bool ownerEpochRetired = false;
  bool resourceRetirementObserved = false;
  bool retirementQueueFaulted = false;
};

struct NativeBridgeQuiescenceSnapshot {
  uint64_t observedRenderThreadId = 0;
  uint64_t requestedResetGeneration = 0;
  uint64_t completedResetGeneration = 0;
  uint64_t ownerRetiredGeneration = 0;
  uint64_t pendingKernelAuthorizations = 0;
  uint64_t poisonRangesOutstanding = 0;
  uint64_t retirementEventsPending = 0;
  uint32_t activeCallbackPins = 0;
  uint32_t activeFlushTransactions = 0;
  uint32_t activeDispatchTransactions = 0;
  uint32_t activeSemanticTransactions = 0;
  uint32_t activeUploadTransactions = 0;
  // Aggregate of full DIP observers and strict outside-DIP lightweight reader
  // covers. Both are drain-visible and prevent reset/uninstall completion.
  uint32_t activeDipObserverTransactions = 0;
  bool resetPending = false;
  bool currentThreadIsObservedRenderThread = false;
  bool currentThreadTlsQuiescent = false;
  bool safetyObservationEnabled = false;
  bool transactionIngressEnabled = false;
  bool callbackIngressEnabled = false;
  bool dipObserverIngressEnabled = false;
  bool newBypassBlocked = false;
  bool retirementQueueFaulted = false;
  // Production-light render-thread telemetry may be newer than the relaxed
  // process-wide atomics while this is non-zero. Only its owning render thread
  // may merge and clear the pending delta.
  uint32_t telemetryDeltaPending = 0;
  bool telemetryDeltaFaulted = false;
};

enum NativeIndexTicketStage : uint32_t {
  NativeIndexTicketStageNone = 0u,
  NativeIndexTicketStageExpectedProof = 1u << 0,
  NativeIndexTicketStageLockAttempt = 1u << 1,
  NativeIndexTicketStageLockExact = 1u << 2,
  NativeIndexTicketStageContentsExact = 1u << 3,
  NativeIndexTicketStageUnlockExact = 1u << 4,
  NativeIndexTicketStageSetIndicesExact = 1u << 5,
  NativeIndexTicketStageActualProof = 1u << 6,
  NativeIndexTicketStageDipConsumed = 1u << 7,
};

enum NativeIndexTicketFailure : uint64_t {
  NativeIndexTicketFailureNone = 0u,
  NativeIndexTicketFailureExpectedProofMissing = 1ull << 0,
  NativeIndexTicketFailureExpectedProofStale = 1ull << 1,
  NativeIndexTicketFailureThreadMismatch = 1ull << 2,
  NativeIndexTicketFailureUploadEpochMismatch = 1ull << 3,
  NativeIndexTicketFailureTicketGenerationMismatch = 1ull << 4,
  NativeIndexTicketFailureResourceGenerationMissing = 1ull << 5,
  NativeIndexTicketFailureResourceGenerationMismatch = 1ull << 6,
  NativeIndexTicketFailureCommonResourceMissing = 1ull << 7,
  NativeIndexTicketFailureComIndexBufferMissing = 1ull << 8,
  NativeIndexTicketFailureNativeComIdentityMismatch = 1ull << 9,
  NativeIndexTicketFailureResourceIdentityMismatch = 1ull << 10,
  NativeIndexTicketFailureIndexFormatMismatch = 1ull << 11,
  NativeIndexTicketFailureLockFailed = 1ull << 12,
  NativeIndexTicketFailureMappedPointerMissing = 1ull << 13,
  NativeIndexTicketFailureLockOffsetMismatch = 1ull << 14,
  NativeIndexTicketFailureLockSizeMismatch = 1ull << 15,
  NativeIndexTicketFailureLockFlagsMismatch = 1ull << 16,
  NativeIndexTicketFailureRingStateMismatch = 1ull << 17,
  NativeIndexTicketFailureOutOfOrder = 1ull << 18,
  NativeIndexTicketFailureMappedRangeUnreadable = 1ull << 19,
  NativeIndexTicketFailureIndexCountMismatch = 1ull << 20,
  NativeIndexTicketFailureIndexRangeMismatch = 1ull << 21,
  NativeIndexTicketFailureIndexHashMismatch = 1ull << 22,
  NativeIndexTicketFailureUnlockProofMissing = 1ull << 23,
  NativeIndexTicketFailureUnlockFailed = 1ull << 24,
  NativeIndexTicketFailureSetIndicesBeforeUnlock = 1ull << 25,
  NativeIndexTicketFailureSetIndicesFailed = 1ull << 26,
  NativeIndexTicketFailureActualProofMissing = 1ull << 27,
  NativeIndexTicketFailureDipIdentityMismatch = 1ull << 28,
  NativeIndexTicketFailureDipSignatureMismatch = 1ull << 29,
  NativeIndexTicketFailureTicketLeak = 1ull << 30,
  NativeIndexTicketFailureScopeHazard = 1ull << 31,
  NativeIndexTicketFailureSnapshotAllocationFailed = 1ull << 32,
  NativeIndexTicketFailureContentsMismatch = 1ull << 33,
};

inline constexpr size_t kNativeIndexTicketFailureBitCount = 34u;

/** @brief Stable D3D9 common-resource and COM VB identity. */
struct NativeVertexResourceIdentity {
  uintptr_t commonResource = 0;
  uintptr_t comVertexBuffer = 0;
  uint64_t resourceGeneration = 0;
};

inline constexpr uint32_t kNativeVertexMapModeBuffer = 0u;
inline constexpr uint32_t kNativeVertexMapModeDirect = 1u;

/**
 * @brief Storage generations and live-lock state captured by D3D9.
 *
 * real/mapping generations are diagnostics only because DXVK may replace
 * those backings without changing the app-visible CPU overwrite target.
 * O1a uses mapAllocationGeneration only as a hard Lock-to-Unlock identity;
 * it never compares that generation with an older poison range to authorize
 * or clear anything.
 */
struct NativeVertexStorageDiagnostics {
  uint64_t realStorageGeneration = 0;
  uint64_t mappingStorageGeneration = 0;
  uint64_t mapAllocationGeneration = 0;
  uint32_t mapMode = 0;
  uint32_t activeLockFlags = 0;
  bool activeLock = false;
};

/**
 * @brief Independent evidence from one successful D3D9 vertex LockBuffer.
 *
 * The payload alone is not authority. O0/O1a consume it strictly as report-only
 * evidence; a separately armed production O1 token may consume the same exact
 * endpoint together with normal kernel return, successful Unlock, stable poison
 * state, and successful outer settlement. It never replaces those remaining
 * gates. requestedFlags is the application value at LockBuffer entry;
 * effectiveFlags is the value after the established D3D9 normalization.
 */
struct NativeOutsidePoisonVertexLockEvidence {
  NativeVertexResourceIdentity resource;
  NativeVertexStorageDiagnostics storage;
  uintptr_t nativeD3DDevice = 0;
  uintptr_t realBuffer = 0;
  uintptr_t mappingBuffer = 0;
  uintptr_t mappedAllocation = 0;
  uintptr_t mappedAllocationBase = 0;
  uintptr_t mappedPointer = 0;
  uint32_t descType = 0;
  uint32_t descPool = 0;
  uint32_t descUsage = 0;
  uint32_t descSize = 0;
  uint32_t descFvf = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t requestedFlags = 0;
  uint32_t effectiveFlags = 0;
  uint32_t lockDepth = 0;
  int32_t result = 0;
};

// 生产 O1 的“实际 Lock 不相交”轻量端点。它只用于 sidecar 关闭且
// 已经确认与 poison 区间不相交的上传；若任何字段不完整，调用方必须
// 回到 NativeOutsidePoisonVertexLockEvidence 的完整证据路径。
struct NativeOutsidePoisonVertexLockFastInput {
  NativeVertexResourceIdentity resource;
  NativeVertexStorageDiagnostics storage;
  uintptr_t nativeD3DDevice = 0;
  uint32_t descType = 0;
  uint32_t descPool = 0;
  uint32_t descUsage = 0;
  uint32_t descSize = 0;
  uint32_t descFvf = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t requestedFlags = 0;
  uint32_t effectiveFlags = 0;
  uint32_t lockDepth = 0;
  int32_t result = 0;
};

// 与轻量 Lock 端点配对的 Unlock 最小身份。它不读取 backing/storage
// 指针，只验证同一应用锁、同一资源和成功的 Unlock；轻量端点只可能
// 提交 NoOverlap，绝不清除 poison 字节。
struct NativeOutsidePoisonVertexUnlockFastInput {
  NativeVertexResourceIdentity resource;
  uintptr_t nativeD3DDevice = 0;
  uintptr_t activeLockOwnerIdentity = 0;
  uint32_t activeLockOffset = 0;
  uint32_t activeLockSize = 0;
  uint32_t activeLockFlags = 0;
  uint32_t entryLockDepth = 0;
  uint32_t completionLockDepth = 0;
  int32_t result = 0;
};

/**
 * @brief Exact endpoint spanning one real D3D9 vertex UnlockBuffer call.
 *
 * Entry fields are captured while the successful Lock's active-lock record is
 * still live. Completion fields are captured only after the real Unlock result
 * is known. O1a compares both endpoints with its retained Lock evidence and
 * remains report-only. A separately armed production O1 token may consume this
 * endpoint only as one member of its complete fail-closed settlement proof.
 */
struct NativeOutsidePoisonVertexUnlockEvidence {
  NativeVertexResourceIdentity resource;
  NativeVertexStorageDiagnostics entryStorage;
  NativeVertexStorageDiagnostics completionStorage;
  uintptr_t nativeD3DDevice = 0;
  uintptr_t realBuffer = 0;
  uintptr_t mappingBuffer = 0;
  uintptr_t mappedAllocation = 0;
  uintptr_t mappedAllocationBase = 0;
  uintptr_t completionRealBuffer = 0;
  uintptr_t completionMappingBuffer = 0;
  uintptr_t completionMappedAllocation = 0;
  uintptr_t completionMappedAllocationBase = 0;
  uint64_t completionResourceGeneration = 0;
  uintptr_t activeLockOwnerIdentity = 0;
  uint32_t activeLockOffset = 0;
  uint32_t activeLockSize = 0;
  uint32_t activeLockFlags = 0;
  uint32_t entryLockDepth = 0;
  uint32_t completionLockDepth = 0;
  bool activeLockObserved = false;
  int32_t result = 0;
};

/** @brief Host-proven native output identity and immutable vertex layout. */
struct NativeVertexOutputProof {
  NativeVertexResourceIdentity resource;
  uintptr_t nativeD3DDevice = 0;
  uint32_t outputFormat = 0;
  uint32_t vertexStride = 0;
  uint32_t fvf = 0;
};

/** @brief Pre-Unlock proof for one normal-return CPU vertex rewrite. */
struct NativeCpuRewriteOutputProof {
  NativeVertexOutputProof vertexOutputProof;
  uint32_t baseVertex = 0;
  uint32_t vertexCount = 0;
  uint32_t byteOffset = 0;
  uint32_t byteLength = 0;
};

/** @brief Stable D3D9 common-resource and COM IB identity for one map epoch. */
struct NativeIndexResourceIdentity {
  uintptr_t commonResource = 0;
  uintptr_t comIndexBuffer = 0;
  uint64_t resourceGeneration = 0;
  uint32_t indexFormat = 0;
};

/** @brief Raw COM Lock result observed before native index memcpy executes. */
struct NativeIndexLockInput {
  NativeIndexResourceIdentity resource;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t flags = 0;
  void* mappedPointer = nullptr;
  int32_t result = 0;
};

/** @brief Lock context replayed synchronously immediately before Unlock. */
struct NativeIndexUnlockInput {
  NativeIndexResourceIdentity resource;
  uint64_t ticketGeneration = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t flags = 0;
  const void* mappedPointer = nullptr;
};

/** @brief Result of Unlock or SetIndices for an existing ticket generation. */
struct NativeIndexOperationInput {
  NativeIndexResourceIdentity resource;
  uint64_t ticketGeneration = 0;
  int32_t result = 0;
};

/**
 * @brief Persistent proof state for one native index upload.
 * @note This structure intentionally contains no mapped pointer.
 */
struct NativeIndexTicketObservation {
  uint64_t uploadEpoch = 0;
  uint64_t ticketGeneration = 0;
  uint64_t resourceGeneration = 0;
  uint64_t expectedIndexHash = 0;
  uint64_t actualIndexHash = 0;
  uint64_t failureMask = NativeIndexTicketFailureNone;
  uintptr_t commonResource = 0;
  uintptr_t comIndexBuffer = 0;
  uint32_t expectedIndexCount = 0;
  uint32_t actualIndexCount = 0;
  uint32_t expectedMinIndex = 0;
  uint32_t expectedMaxIndex = 0;
  uint32_t actualMinIndex = 0;
  uint32_t actualMaxIndex = 0;
  uint32_t predictedStartIndex = 0;
  uint32_t expectedLockOffset = 0;
  uint32_t expectedLockSize = 0;
  uint32_t expectedLockFlags = 0;
  uint32_t observedLockOffset = 0;
  uint32_t observedLockSize = 0;
  uint32_t observedLockFlags = 0;
  uint32_t indexFormat = 0;
  uint32_t stageMask = NativeIndexTicketStageNone;
  bool exact = false;
  bool suppressed = false;
  bool leaked = false;
};

struct NativeBridgeAddresses {
  uintptr_t flushSortedItemsRva = 0;
  uintptr_t dispatchCommonRva = 0;
  uintptr_t applyDrawStateAndSamplerPairRva = 0;
  uintptr_t gxDeviceD3dDynamicVertexUploadRva = 0;
  uintptr_t gxDeviceD3dSkinCopyKernelRva = 0;
};

struct NativeBridgeFingerprint {
  uint32_t failureMask = NativeFingerprintFailureNone;
  uint32_t peValidMask = 0;
  uint32_t peFailureMask = 0;
  uint32_t opcodeFailureMask = NativeOpcodeFailureNone;
  uintptr_t loadedImageBase = 0;
  uint32_t observedImageBase = 0;
  uint32_t observedPeOffset = 0;
  uint32_t observedNtSignature = 0;
  uint32_t peTimestamp = 0;
  uint32_t imageSize = 0;
  uint32_t imageChecksum = 0;
  uint16_t observedMachine = 0;
  uint16_t observedOptionalMagic = 0;
  uint32_t fileVersion[4] = {};
  uint64_t fileSize = 0;
  uint8_t md5[16] = {};
  uint8_t sha1[20] = {};
  bool exactMatch = false;
};

struct NativeEpochKey {
  uint64_t renderThreadId = 0;
  uint64_t flushEpoch = 0;
  uint64_t dispatchEpoch = 0;
  uint64_t uploadEpoch = 0;
  uint32_t uploadOrdinal = 0;
  uint32_t dipOrdinal = 0;
};

struct NativeFlushObservation {
  GpuSkinMode mode = GpuSkinMode::Disabled;
  uint64_t renderThreadId = 0;
  uint64_t flushEpoch = 0;
  uintptr_t opaqueArray = 0;
  uintptr_t transparentArray = 0;
  uint32_t opaqueCount = 0;
  uint32_t transparentCount = 0;
};

// Compact manager-to-bridge publication key for the authoritative candidate
// set of one fully assembled flush. The synchronous sidecar is independent of
// callback ABI 9; the bridge copies at most the fixed capacity into render TLS.
struct NativeDispatchCpuOnlySealCandidate {
  uintptr_t renderablePart = 0;
  uint32_t layerIndex = 0;
};

struct NativeDispatchObservation {
  NativeEpochKey epoch;
  NativeDispatchPath path = NativeDispatchPath::Unknown;
  uintptr_t sceneNode = 0;
  uintptr_t renderablePart = 0;
  uint32_t layerIndex = 0;
  uint32_t nestingDepth = 0;
  int32_t stage = -1;
  int32_t batchTag = -1;
  bool failClosed = false;
};

struct NativeDispatchSummary {
  NativeDispatchObservation dispatch;
  uint32_t semanticScopeCount = 0;
  uint32_t uploadCount = 0;
  uint32_t dipCount = 0;
};

// Exact 0x6F0EEA50 stack contract after the +0x68 vtable dispatch.
struct NativeUploadCall {
  uintptr_t gxDeviceD3d = 0;
  uint32_t vertexCount = 0;
  uintptr_t positions = 0;
  uint32_t positionStride = 0;
  uintptr_t normals = 0;
  uint32_t normalStride = 0;
  uintptr_t extra = 0;
  uint32_t extraStride = 0;
  uintptr_t groupSlots = 0;
  uint32_t groupSlotStride = 0;
  uintptr_t uv0 = 0;
  uint32_t uv0Stride = 0;
  uintptr_t uv1 = 0;
  uint32_t uv1Stride = 0;
  NativeOutsideUploadPopulationToken outsideAdmissionPopulation;
};

struct NativeUploadObservation {
  NativeEpochKey epoch;
  GpuSkinMode mode = GpuSkinMode::Disabled;
  NativeDispatchPath path = NativeDispatchPath::Unknown;
  uintptr_t sceneNode = 0;
  uintptr_t renderablePart = 0;
  uintptr_t geosetData = 0;
  uintptr_t layerDispatch = 0;
  uintptr_t layerState = 0;
  uintptr_t semanticCaller = 0;
  uintptr_t gxDeviceD3d = 0;
  uintptr_t nativeD3DDevice = 0;
  uintptr_t nativeVertexBuffer = 0;
  uintptr_t nativeIndexBuffer = 0;
  NativeVertexOutputProof vertexOutputProof;
  NativeCpuRewriteOutputProof cpuRewriteOutputProof;
  uintptr_t mappedDst = 0;
  uintptr_t positions = 0;
  uintptr_t normals = 0;
  uintptr_t extra = 0;
  uintptr_t groupSlots = 0;
  uintptr_t uv0 = 0;
  uintptr_t uv1 = 0;
  uintptr_t palette = 0;
  uint32_t vertexCount = 0;
  uint32_t geosetVertexCount = 0;
  uint32_t positionStride = 0;
  uint32_t normalStride = 0;
  uint32_t extraStride = 0;
  uint32_t groupSlotStride = 0;
  uint32_t uv0Stride = 0;
  uint32_t uv1Stride = 0;
  uint32_t paletteGroupCount = 0;
  uint32_t skinMode = 0;
  uint32_t outputFormat = 0;
  uint32_t fvf = 0;
  uint32_t outputStride = 0;
  uint32_t submittedVertexCountBefore = 0;
  uint32_t ringBaseVertexBefore = 0;
  uint32_t ringNextVertexBefore = 0;
  uint32_t ringBaseVertexAfter = 0;
  uint32_t ringNextVertexAfter = 0;
  uint32_t indexRingBaseBefore = 0;
  uint32_t indexRingNextBefore = 0;
  uint32_t predictedIndexRingBase = 0;
  uint32_t predictedIndexRingNext = 0;
  uint32_t priorIndexBaseVertex = 0;
  uint32_t fanoutCount = 0;
  uint32_t bypassToken = 0;
  uint32_t expectedIndexCount = 0;
  int32_t stage = -1;
  int32_t batchTag = -1;
  int32_t originalResult = 0;
  uint32_t postSkipMismatchMask = 0;
  uint64_t observedPreflight = 0;
  uint64_t requiredPreflight = kNativeObservationRequiredPreflight;
  uint64_t fuseKey = 0;
  uint64_t cpuSkinBytesSkipped = 0;
  NativeIndexTicketObservation indexTicket;
  NativeBypassFailureReason bypassFailure = NativeBypassFailureReason::None;
  bool originalUploadExecuted = false;
  bool cpuSkinKernelCalled = false;
  // Selection means the detour chose the trampoline. It is not evidence that
  // the caller-owned SEH did not handle a fault inside that trampoline.
  bool cpuSkinOriginalTrampolineSelected = false;
  bool cpuSkinOriginalReturnedNormally = false;
  bool cpuSkinOriginalRangeExact = false;
  bool cpuRewriteOutputProofExact = false;
  bool cpuSkinKernelBypassed = false;
  bool kernelAuthorizationPending = false;
  bool completionPublished = false;
  bool postSkipMismatch = false;
  bool nativeDeviceStateReadable = false;
  bool bypassAuthorized = false;
  bool nativeObservationEligible = false;
  bool takeoverEligible = false;
  bool scopeFailClosed = false;
  // Production bypass rejected this upload before kernel-time proof.  The
  // original Game.dll upload still runs, but the bridge deliberately avoids
  // in-flight state, GX refreshes and range proofs for this non-candidate.
  bool productionFastRejected = false;
  // A fast-rejected original upload can still overwrite an older bypassed
  // slice in Game.dll's cyclic dynamic VB. Admit only that predicted overlap
  // to the normal-return retirement path; it is never bypass-authorizable.
  bool poisonRetirementOnly = false;
  NativeOutsideUploadPopulationToken outsideAdmissionPopulation;
};

struct NativeBypassAuthorization {
  uint64_t fuseKey = 0;
  uint32_t token = 0;
  uint32_t expectedIndexCount = 0;
  uint32_t requiredConsumerBits = 0;
  uint32_t predictedStartIndex = 0;
  uint64_t approvedPreflight = 0;
  NativeVertexOutputProof vertexOutputProof;
};

enum NativeDipFlag : uint32_t {
  NativeDipFlagNone = 0,
  NativeDipFlagDebugSkip = 1u << 0,
  NativeDipFlagAutoInstancing = 1u << 1,
  NativeDipFlagIndexSplit = 1u << 2,
  NativeDipFlagRecursive = 1u << 3,
  NativeDipFlagNonMainPass = 1u << 4,
};

struct NativeDipInput {
  NativeVertexResourceIdentity stream0Resource;
  NativeIndexResourceIdentity indexResource;
  uintptr_t nativeD3DDevice = 0;
  uint64_t indexTicketGeneration = 0;
  uint32_t primitiveType = 0;
  int32_t baseVertexIndex = 0;
  uint32_t minVertexIndex = 0;
  uint32_t numVertices = 0;
  uint32_t startIndex = 0;
  uint32_t primitiveCount = 0;
  uint32_t vertexStride = 0;
  uint32_t fvf = 0;
  uint32_t outputFormat = 0;
  uint32_t flags = NativeDipFlagNone;
};

struct NativeDipDiagnosticInput {
  NativeVertexStorageDiagnostics stream0Storage;
  NativeDispatchPath dispatchPath = NativeDispatchPath::Unknown;
  NativeDipScope scope = NativeDipScope::Unknown;
  int32_t stage = -1;
  int32_t batchTag = -1;
};

inline constexpr size_t kNativePoisonDiagnosticSampleCapacity = 32u;

struct NativePoisonDiagnosticSample {
  uint64_t sequence = 0;
  NativeEpochKey currentEpoch;
  NativeEpochKey poisonEpoch;
  uint64_t poisonFuseKey = 0;
  NativeVertexStorageDiagnostics currentStorage;
  NativeVertexStorageDiagnostics poisonStorage;
  NativeDispatchPath dispatchPath = NativeDispatchPath::Unknown;
  NativeDipScope scope = NativeDipScope::Unknown;
  NativePoisonHitKind hitKind = NativePoisonHitKind::None;
  int32_t stage = -1;
  int32_t batchTag = -1;
  int32_t baseVertexIndex = 0;
  uint32_t minVertexIndex = 0;
  uint32_t numVertices = 0;
  uint32_t startIndex = 0;
  uint32_t primitiveCount = 0;
  uint32_t primitiveType = 0;
  uint32_t poisonBaseVertex = 0;
  uint32_t poisonVertexCount = 0;
  uint32_t originExpectedStartIndex = 0;
  uint32_t originExpectedIndexCount = 0;
  bool poisonRealStorageMixed = false;
  bool poisonMappingStorageMixed = false;
  bool poisonMapAllocationMixed = false;
  bool poisonMapModeMixed = false;
  bool poisonIndexSignatureMixed = false;
};

struct NativePoisonDiagnosticSnapshot {
  uint64_t poisonOnlyHits = 0;
  uint64_t scopeActiveUpload = 0;
  uint64_t scopeOutsideDispatch = 0;
  uint64_t scopeDispatchNoUpload = 0;
  uint64_t scopeHazard = 0;
  uint64_t scopeUnknown = 0;
  uint64_t hitIncompleteIdentity = 0;
  uint64_t hitLayoutMismatch = 0;
  uint64_t hitInexactRange = 0;
  uint64_t hitExactRangeOverlap = 0;
  uint64_t hitUnknown = 0;
  uint64_t realStorageSame = 0;
  uint64_t realStorageDifferent = 0;
  uint64_t realStorageMixed = 0;
  uint64_t realStorageUnknown = 0;
  uint64_t mappingStorageSame = 0;
  uint64_t mappingStorageDifferent = 0;
  uint64_t mappingStorageMixed = 0;
  uint64_t mappingStorageUnknown = 0;
  uint64_t mapAllocationSame = 0;
  uint64_t mapAllocationDifferent = 0;
  uint64_t mapAllocationMixed = 0;
  uint64_t mapAllocationUnknown = 0;
  uint64_t indexSignatureSame = 0;
  uint64_t indexSignatureDifferent = 0;
  uint64_t indexSignatureMixed = 0;
  uint64_t indexSignatureUnknown = 0;
  uint64_t sampleOverflow = 0;
  uint32_t sampleCount = 0;
  std::array<NativePoisonDiagnosticSample,
             kNativePoisonDiagnosticSampleCapacity> samples{};
};

struct NativeDipObservation {
  NativeEpochKey epoch;
  NativeDipInput dip;
  uint64_t sourceUploadFuseKey = 0;
  uint32_t uploadFanoutOrdinal = 0;
  uint64_t observedPreflight = 0;
  NativeIndexTicketObservation indexTicket;
  bool sourceUploadKernelBypassed = false;
  bool sourceUploadPostSkipMismatch = false;
  bool correlated = false;
  bool exactNativeMatch = false;
  bool takeoverEligible = false;
  // A poisoned native range may only be consumed through an exact upper-layer
  // GPU lease. The bridge never assumes that a manager or lease exists.
  bool nativeRangePoisoned = false;
  // Sampled before poison handling promotes sourceUploadKernelBypassed. This
  // distinguishes a DIP already owned by a bypassed upload from a later DIP
  // that only intersects a still-poisoned native range.
  bool nativePoisonHadBypassedSource = false;
  bool requiresSuppression = false;
};

struct NativeUploadFanoutObservation {
  NativeEpochKey epoch;
  NativeDispatchPath path = NativeDispatchPath::Unknown;
  uint64_t sourceUploadFuseKey = 0;
  uint32_t dipCount = 0;
  NativeIndexTicketObservation indexTicket;
  bool sourceUploadKernelBypassed = false;
  bool sourceUploadPostSkipMismatch = false;
};

// B1 在确认单 DIP fan-out 且 Main/Shadow consumer 都已消费后，允许把对应
// 的 native poison 区间提前退休。请求只接受当前 TLS 中仍处于 fan-out 收口
// 的同一上传；任何身份、索引、epoch 或 consumer 证明漂移都会保留 poison。
bool RetireNativeBypassPoisonAfterExactConsumer(
    const NativeUploadObservation& observation,
    bool exactSingleDip, bool consumerSettlementExact) noexcept;

struct NativeBridgeCounters {
  uint64_t hotPathTimingFrequency = 0;
  uint64_t beginUploadTimingCalls = 0;
  uint64_t beginUploadTimingTicks = 0;
  uint64_t beginUploadTimingMaxTicks = 0;
  uint64_t beginSampleCommonCalls = 0;
  uint64_t beginSampleCommonTicks = 0;
  uint64_t beginSampleCommonMaxTicks = 0;
  uint64_t beginSampleStateCalls = 0;
  uint64_t beginSampleStateTicks = 0;
  uint64_t beginSampleStateMaxTicks = 0;
  uint64_t beginSampleExactCalls = 0;
  uint64_t beginSampleExactTicks = 0;
  uint64_t beginSampleExactMaxTicks = 0;
  uint64_t beginSampleScopeRouteCalls = 0;
  uint64_t beginSampleScopeRouteTicks = 0;
  uint64_t beginSampleScopeRouteMaxTicks = 0;
  uint64_t beginSampleStateRejectRouteCalls = 0;
  uint64_t beginSampleStateRejectRouteTicks = 0;
  uint64_t beginSampleStateRejectRouteMaxTicks = 0;
  uint64_t beginSampleSkinRouteCalls = 0;
  uint64_t beginSampleSkinRouteTicks = 0;
  uint64_t beginSampleSkinRouteMaxTicks = 0;
  uint64_t beginSampleSmallRouteCalls = 0;
  uint64_t beginSampleSmallRouteTicks = 0;
  uint64_t beginSampleSmallRouteMaxTicks = 0;
  uint64_t beginSampleCandidateRouteCalls = 0;
  uint64_t beginSampleCandidateRouteTicks = 0;
  uint64_t beginSampleCandidateRouteMaxTicks = 0;
  uint64_t t2SampleGeoSnapCalls = 0;
  uint64_t t2SampleGeoSnapTicks = 0;
  uint64_t t2SampleGeoSnapMaxTicks = 0;
  uint64_t t2SampleGeoHeaderCalls = 0;
  uint64_t t2SampleGeoHeaderTicks = 0;
  uint64_t t2SampleGeoHeaderMaxTicks = 0;
  uint64_t t2SamplePositionProofCalls = 0;
  uint64_t t2SamplePositionProofTicks = 0;
  uint64_t t2SamplePositionProofMaxTicks = 0;
  uint64_t t2SampleNormalProofCalls = 0;
  uint64_t t2SampleNormalProofTicks = 0;
  uint64_t t2SampleNormalProofMaxTicks = 0;
  uint64_t t2SampleGroupProofCalls = 0;
  uint64_t t2SampleGroupProofTicks = 0;
  uint64_t t2SampleGroupProofMaxTicks = 0;
  uint64_t t2SamplePaletteProofCalls = 0;
  uint64_t t2SamplePaletteProofTicks = 0;
  uint64_t t2SamplePaletteProofMaxTicks = 0;
  uint64_t evaluateKernelTimingCalls = 0;
  uint64_t evaluateKernelTimingTicks = 0;
  uint64_t evaluateKernelTimingMaxTicks = 0;
  uint64_t completeUploadTimingCalls = 0;
  uint64_t completeUploadTimingTicks = 0;
  uint64_t completeUploadTimingMaxTicks = 0;
  uint64_t notifyNormalTimingCalls = 0;
  uint64_t notifyNormalTimingTicks = 0;
  uint64_t notifyNormalTimingMaxTicks = 0;
  uint64_t dipTimingCalls = 0;
  uint64_t dipTimingTicks = 0;
  uint64_t dipTimingMaxTicks = 0;
  uint64_t outerUploadTimingCalls = 0;
  uint64_t outerUploadTimingTicks = 0;
  uint64_t outerUploadTimingMaxTicks = 0;
  uint64_t originalKernelTimingCalls = 0;
  uint64_t originalKernelTimingTicks = 0;
  uint64_t originalKernelTimingMaxTicks = 0;
  uint64_t flushNotifications = 0;
  uint64_t dispatchScopes = 0;
  uint64_t dispatchScopeEnds = 0;
  uint64_t commonDispatchScopes = 0;
  uint64_t specialDispatchScopes = 0;
  // Production-light Bypass may defer the manager's ABI-9 dispatch callback
  // until the first in-scope Common upload. Native dispatch epochs, ordinals
  // and physical end bookkeeping remain eager and are counted by
  // dispatchScopes/dispatchScopeEnds.
  uint64_t managerDispatchEagerScopes = 0;
  uint64_t managerDispatchLazyScopes = 0;
  uint64_t managerDispatchNeverScopes = 0;
  uint64_t managerDispatchEvidenceEagerScopes = 0;
  uint64_t managerDispatchBeginCallbacks = 0;
  uint64_t managerDispatchEndCallbacks = 0;
  uint64_t managerDispatchEagerBegins = 0;
  uint64_t managerDispatchLazyAdmissionAttempts = 0;
  uint64_t managerDispatchLazyAdmissions = 0;
  uint64_t managerDispatchEagerAdmissionFailures = 0;
  uint64_t managerDispatchLazyAdmissionFailures = 0;
  uint64_t managerDispatchNeverSafetyFailures = 0;
  uint64_t managerDispatchIssuedEnds = 0;
  uint64_t managerDispatchNoUploadEnds = 0;
  uint64_t managerDispatchNeverEnds = 0;
  uint64_t managerDispatchFailedEnds = 0;
  uint64_t managerDispatchSkippedUploads = 0;
  uint64_t managerDispatchSkippedDips = 0;
  uint64_t managerDispatchSkippedFanouts = 0;
  uint64_t semanticScopes = 0;
  uint64_t uploads = 0;
  uint64_t uploadsOutsideDispatch = 0;
  uint64_t nativeEligibleUploads = 0;
  uint64_t productionFastRejectScope = 0;
  uint64_t productionFastRejectState = 0;
  uint64_t productionFastRejectSkinFormat = 0;
  uint64_t productionFastRejectInput = 0;
  uint64_t productionFastRejectSmall = 0;
  uint64_t productionFastRejectUnknownState = 0;
  uint64_t productionCandidates = 0;
  uint64_t productionPoisonRetirementOnly = 0;
  uint64_t productionOutsideCallbacksSkipped = 0;
  uint64_t productionOutsideNativeFastPath = 0;
  // Production-light, dispatch-free and poison-free uploads may bypass all
  // manager/O1 admission while still executing the original outer upload and
  // native CPU kernel. These counters form an exact receipt ledger only; they
  // never grant bypass authority or clear poison.
  uint64_t productionOutsideNoPoisonDirectAttempts = 0;
  uint64_t productionOutsideNoPoisonDirectKernelCalls = 0;
  uint64_t productionOutsideNoPoisonDirectNormalReturns = 0;
  uint64_t productionOutsideNoPoisonDirectKernelNoNormalReturns = 0;
  uint64_t productionOutsideNoPoisonDirectCompleted = 0;
  uint64_t productionOutsideNoPoisonDirectConflicts = 0;
  uint64_t productionOutsideNoPoisonDirectCancellations = 0;
  uint64_t productionOutsideNoPoisonDirectActive = 0;
  uint64_t productionOutsideNoPoisonDirectResetCompletedWhileActive = 0;
  uint64_t productionOutsideNoPoisonDirectLatePoison = 0;
  // Production-light outside admission may inspect the live native vertex
  // ring only while poison exists. These remain atomic-only diagnostics so
  // the established telemetryBatchedAdds closure is unchanged. Scan attempts
  // close exactly into no-overlap admissions + overlap rejects + unreadable or
  // asynchronously invalidated proof rejects.
  uint64_t productionOutsidePoisonScanAttempts = 0;
  uint64_t productionOutsidePoisonNoOverlapAdmissions = 0;
  uint64_t productionOutsidePoisonOverlapRejects = 0;
  uint64_t productionOutsidePoisonReadFailRejects = 0;
  // Frozen once by InitializeNativeBridge. Invalid explicit environment input
  // fails to policy none because these sidecars are report-only. The policy
  // closure proves every disabled side remained numerically cold.
  uint32_t productionOutsidePoisonSidecarPolicy = 0;
  bool productionOutsidePoisonSidecarPolicyExplicit = false;
  bool productionOutsidePoisonSidecarPolicyInvalid = false;
  bool productionOutsidePoisonSidecarPolicyClosureClean = false;
  // O0 report-only comparison between the legacy Game.dll SafeCopy scan
  // (row: no-overlap/overlap/read-failure) and an independently observed
  // successful D3D9 vertex Lock (column: same order). Structural failures are
  // outside the matrix and close through exactly one unprovable reason.
  uint64_t productionOutsidePoisonShadowAttempts = 0;
  uint64_t productionOutsidePoisonShadowCreated = 0;
  uint64_t productionOutsidePoisonShadowOverflow = 0;
  uint64_t productionOutsidePoisonShadowActive = 0;
  uint64_t productionOutsidePoisonShadowLockNotifications = 0;
  uint64_t productionOutsidePoisonShadowSettled = 0;
  uint64_t productionOutsidePoisonShadowCancelled = 0;
  uint64_t productionOutsidePoisonShadowResetAborted = 0;
  uint64_t productionOutsidePoisonShadowComparable = 0;
  uint64_t productionOutsidePoisonShadowUnprovable = 0;
  std::array<uint64_t, kNativeOutsidePoisonShadowMatrixSize>
      productionOutsidePoisonShadowMatrix{};
  std::array<
      uint64_t, kNativeOutsidePoisonShadowUnprovableReasonCount>
      productionOutsidePoisonShadowUnprovableReasons{};
  uint64_t productionOutsidePoisonShadowMatrixTotal = 0;
  uint64_t productionOutsidePoisonShadowOffDiagonal = 0;
  uint64_t productionOutsidePoisonShadowLegacyMissedOverlap = 0;
  bool productionOutsidePoisonShadowAttemptClosureClean = false;
  bool productionOutsidePoisonShadowLifetimeClosureClean = false;
  bool productionOutsidePoisonShadowSettlementClosureClean = false;
  // O1a-v2 reuses the O0 owner/LIFO cookie, but its logical N/O/R verdict is
  // frozen independently inside the successful-Lock notification. The legacy
  // matrix below is logical-only and must never be treated as authority-ready.
  // wouldClear never mutates poison and authority stays 0.
  uint64_t productionOutsidePoisonO1ShadowAttempts = 0;
  uint64_t productionOutsidePoisonO1ShadowCreated = 0;
  uint64_t productionOutsidePoisonO1ShadowOverflow = 0;
  uint64_t productionOutsidePoisonO1ShadowActive = 0;
  uint64_t productionOutsidePoisonO1ShadowLockNotifications = 0;
  uint64_t productionOutsidePoisonO1ShadowKernelNotifications = 0;
  uint64_t productionOutsidePoisonO1ShadowUnlockNotifications = 0;
  uint64_t productionOutsidePoisonO1ShadowFrozen = 0;
  uint64_t productionOutsidePoisonO1ShadowSettled = 0;
  uint64_t productionOutsidePoisonO1ShadowCancelled = 0;
  uint64_t productionOutsidePoisonO1ShadowResetAborted = 0;
  // comparable is a Lock-time logical publication; unprovable is a later
  // physical-settlement rejection. They are orthogonal, not a partition.
  uint64_t productionOutsidePoisonO1ShadowComparable = 0;
  uint64_t productionOutsidePoisonO1ShadowUnprovable = 0;
  uint64_t productionOutsidePoisonO1ShadowComparisonMissing = 0;
  uint64_t productionOutsidePoisonO1ShadowWouldClear = 0;
  // Joint attribution for the only expected legacy O -> logical N path. The
  // old exact overlap is frozen by the SafeCopy scan; DIRECT DISCARD must then
  // retire that same immutable range in one closed ledger transition, and the
  // successful Lock must consume the exact new map allocation. All fields are
  // diagnostics-only and authority remains zero.
  uint64_t productionOutsidePoisonO1ShadowOldOverlapFrozen = 0;
  uint64_t productionOutsidePoisonO1ShadowDirectDiscardNotifications = 0;
  uint64_t
      productionOutsidePoisonO1ShadowDirectDiscardOldOverlapRetired = 0;
  uint64_t
      productionOutsidePoisonO1ShadowOldOverlapToNoOverlapDiscardExact = 0;
  uint64_t productionOutsidePoisonO1ShadowOldOverlapToNoOverlapOther = 0;
  bool productionOutsidePoisonO1ShadowDiscardJointClosureClean = false;
  uint64_t productionOutsidePoisonO1ShadowAuthority = 0;
  std::array<uint64_t, kNativeOutsidePoisonO1LockLaneCount>
      productionOutsidePoisonO1ShadowScanCallsByLane{};
  std::array<uint64_t, kNativeOutsidePoisonO1LockLaneCount>
      productionOutsidePoisonO1ShadowScanNoOverlapByLane{};
  std::array<uint64_t, kNativeOutsidePoisonO1LockLaneCount>
      productionOutsidePoisonO1ShadowScanOverlapByLane{};
  std::array<uint64_t, kNativeOutsidePoisonO1LockLaneCount>
      productionOutsidePoisonO1ShadowScanReadFailureByLane{};
  std::array<uint64_t, kNativeOutsidePoisonO1LockLaneCount>
      productionOutsidePoisonO1ShadowPreLockMutationByLane{};
  std::array<uint64_t, kNativeOutsidePoisonO1LockLaneCount>
      productionOutsidePoisonO1ShadowLockToKernelMutationByLane{};
  std::array<uint64_t, kNativeOutsidePoisonO1LaneFailureCount>
      productionOutsidePoisonO1ShadowScanFailuresByLane{};
  std::array<uint64_t, kNativeOutsidePoisonO1ScanFailureReasonCount>
      productionOutsidePoisonO1ShadowScanFailureReasons{};
  uint64_t productionOutsidePoisonO1ShadowScanCalls = 0;
  uint64_t productionOutsidePoisonO1ShadowScanNoOverlap = 0;
  uint64_t productionOutsidePoisonO1ShadowScanOverlap = 0;
  uint64_t productionOutsidePoisonO1ShadowScanReadFailure = 0;
  uint64_t productionOutsidePoisonO1ShadowScanDifferentTarget = 0;
  uint64_t productionOutsidePoisonO1ShadowScanLogicalExact = 0;
  uint64_t productionOutsidePoisonO1ShadowStorageDiagnosticMismatch = 0;
  uint64_t productionOutsidePoisonO1ShadowRealStorageDrift = 0;
  uint64_t productionOutsidePoisonO1ShadowMappingStorageDrift = 0;
  uint64_t productionOutsidePoisonO1ShadowMapAllocationDiagnosticMismatch = 0;
  std::array<uint64_t, kNativeOutsidePoisonO1UnlockDriftComponentCount>
      productionOutsidePoisonO1ShadowUnlockDrifts{};
  std::array<uint64_t, kNativeOutsidePoisonO1UnlockDriftComponentCount>
      productionOutsidePoisonO1ShadowUnlockHardFirstCauses{};
  // N/O/notReady after all normal-return, exact Unlock and outer-success
  // gates. notReady intentionally combines logical R and physical rejection;
  // it is separate from the logical matrix and remains report-only authority 0.
  std::array<uint64_t, 3u>
      productionOutsidePoisonO1ShadowPhysicalVerdicts{};
  bool productionOutsidePoisonO1ShadowScanClosureClean = false;
  bool productionOutsidePoisonO1ShadowUnlockDriftClosureClean = false;
  bool productionOutsidePoisonO1ShadowPhysicalClosureClean = false;
  std::array<uint64_t, kNativeOutsidePoisonO1ShadowMatrixSize>
      productionOutsidePoisonO1ShadowMatrix{};
  std::array<
      uint64_t, kNativeOutsidePoisonO1ShadowUnprovableReasonCount>
      productionOutsidePoisonO1ShadowUnprovableReasons{};
  uint64_t productionOutsidePoisonO1ShadowMatrixTotal = 0;
  uint64_t productionOutsidePoisonO1ShadowOffDiagonal = 0;
  uint64_t productionOutsidePoisonO1ShadowLegacyMissedOverlap = 0;
  bool productionOutsidePoisonO1ShadowAttemptClosureClean = false;
  bool productionOutsidePoisonO1ShadowLifetimeClosureClean = false;
  bool productionOutsidePoisonO1ShadowSettlementClosureClean = false;
  // Production O1 is separate from the report-only O1a shadow above. The
  // outer hook owns a provisional CPU-only transaction and the current real
  // D3D9 Lock freezes the resource/range before the original kernel. Every
  // 127th attempt retains the legacy GX SafeCopy as comparison evidence only.
  uint64_t productionOutsidePoisonAuthorityAttempts = 0;
  uint64_t productionOutsidePoisonAuthorityCreated = 0;
  uint64_t productionOutsidePoisonAuthorityOverflow = 0;
  uint64_t productionOutsidePoisonAuthorityArmed = 0;
  uint64_t productionOutsidePoisonAuthorityActive = 0;
  uint64_t productionOutsidePoisonAuthoritySettled = 0;
  uint64_t productionOutsidePoisonAuthorityCancelled = 0;
  uint64_t productionOutsidePoisonAuthorityResetAborted = 0;
  uint64_t productionOutsidePoisonAuthorityLockNotifications = 0;
  uint64_t productionOutsidePoisonAuthorityLockNoOverlap = 0;
  uint64_t productionOutsidePoisonAuthorityLockOverlap = 0;
  uint64_t productionOutsidePoisonAuthorityLockRejects = 0;
  uint64_t productionOutsidePoisonAuthorityKernelReady = 0;
  uint64_t productionOutsidePoisonAuthorityKernelRejects = 0;
  uint64_t productionOutsidePoisonAuthorityNormalReturns = 0;
  uint64_t productionOutsidePoisonAuthorityUnlockNotifications = 0;
  uint64_t productionOutsidePoisonAuthorityUnlockExact = 0;
  uint64_t productionOutsidePoisonAuthorityUnlockRejects = 0;
  uint64_t productionOutsidePoisonAuthorityCommittedNoOverlap = 0;
  uint64_t productionOutsidePoisonAuthorityCommittedRewrite = 0;
  uint64_t productionOutsidePoisonAuthorityRetained = 0;
  uint64_t productionOutsidePoisonAuthorityPoisonClears = 0;
  uint64_t productionOutsidePoisonAuthority = 0;
  uint64_t productionOutsidePoisonAuthorityEvidenceAttempts = 0;
  uint64_t productionOutsidePoisonAuthorityEvidenceComparable = 0;
  uint64_t productionOutsidePoisonAuthorityEvidenceUnprovable = 0;
  uint64_t productionOutsidePoisonAuthorityEvidenceMismatches = 0;
  uint64_t productionOutsidePoisonAuthorityEvidenceAuthority = 0;
  uint64_t productionOutsidePoisonAuthorityLegacyBackedAuthority = 0;
  std::array<uint64_t, kNativeOutsidePoisonO1ShadowMatrixSize>
      productionOutsidePoisonAuthorityEvidenceMatrix{};
  bool productionOutsidePoisonAuthorityAttemptClosureClean = false;
  bool productionOutsidePoisonAuthorityLifetimeClosureClean = false;
  bool productionOutsidePoisonAuthorityLockClosureClean = false;
  bool productionOutsidePoisonAuthorityExecutionClosureClean = false;
  bool productionOutsidePoisonAuthoritySettlementClosureClean = false;
  bool productionOutsidePoisonAuthorityEvidenceClosureClean = false;
  uint64_t productionOutsideFastKernelMarkerConflicts = 0;
  uint64_t originalUploadCalls = 0;
  uint64_t originalUploadBytes = 0;
  uint64_t bypassPreflightAttempts = 0;
  uint64_t bypassAuthorizations = 0;
  uint64_t kernelHookCalls = 0;
  uint64_t originalKernelCalls = 0;
  uint64_t originalKernelNormalReturns = 0;
  uint64_t originalKernelNormalReturnRejects = 0;
  uint64_t cpuRewriteProofAttempts = 0;
  uint64_t cpuRewriteProofExact = 0;
  uint64_t cpuRewriteProofRejects = 0;
  uint64_t originalKernelBytes = 0;
  uint64_t bypassedKernelCalls = 0;
  uint64_t bypassedKernelBytes = 0;
  uint64_t nullMappedKernelFallbacks = 0;
  uint64_t kernelPreflightRejects = 0;
  uint64_t postSkipMismatchFuses = 0;
  // A real original-kernel call after cpuSkinKernelBypassed. This is a
  // permanent hard-zero invariant; duplicate terminal calls are suppressed
  // and counted separately below.
  uint64_t postSkipNativeFallback = 0;
  uint64_t duplicateKernelCalls = 0;
  uint64_t irreversibleKernelSuppressions = 0;
  uint64_t pendingKernelAuthorizations = 0;
  uint64_t nativePoisonCreates = 0;
  uint64_t nativePoisonClears = 0;
  uint64_t nativePoisonHits = 0;
  uint64_t nativePoisonIncompleteIdentityHits = 0;
  // Same common/generation, but COM/device/layout proof was absent or
  // inconsistent. This is report-only; the draw is suppressed fail closed.
  uint64_t identityMatchedLayoutMismatchHits = 0;
  uint64_t nativePoisonOverflows = 0;
  // Fail-closed poison protocol divergences (including any wrong-thread DIP)
  // and ranges ever force-retired as leaks. Full reset leaves this at zero;
  // non-empty poison is retained until exact overwrite/resource retirement.
  uint64_t nativePoisonLeaks = 0;
  // Process-wide count of ranges currently owned by render-thread ledgers.
  uint64_t nativePoisonOutstanding = 0;
  uint64_t nativeDirectDiscardEvents = 0;
  uint64_t nativeDirectDiscardEventsWithPoison = 0;
  uint64_t nativeDirectDiscardNoPoisonEvents = 0;
  uint64_t nativeDirectDiscardRangesCleared = 0;
  uint64_t nativeDirectDiscardInvalid = 0;
  uint64_t nativeCrossMapAllocationPoisonMerges = 0;
  uint64_t nativeRetirementEventsPublished = 0;
  uint64_t nativeRetirementEventsConsumed = 0;
  uint64_t nativeRetirementRangesCleared = 0;
  uint64_t nativeRetirementQueueOverflows = 0;
  uint64_t nativeRetirementInvalidEvents = 0;
  uint64_t resetRequests = 0;
  uint64_t resetCompletions = 0;
  uint64_t resetDeferred = 0;
  uint64_t resetWrongThread = 0;
  uint64_t resetActiveTransactions = 0;
  uint64_t resetOwnerEpochDeferred = 0;
  uint64_t resetPoisonDeferred = 0;
  uint64_t resetRetirementQueueFaults = 0;
  uint64_t resetFailClosedUploads = 0;
  uint64_t resetOwnerAcks = 0;
  uint64_t resetOwnerAckMismatches = 0;
  uint64_t resetCommitBlocked = 0;
  uint64_t resetSlowPathCalls = 0;
  uint64_t indexTicketFailureMask = 0;
  uint64_t indexTicketAttempts = 0;
  uint64_t indexTicketExact = 0;
  uint64_t indexTicketSuppressed = 0;
  uint64_t indexTicketLeaks = 0;
  // Whole-upload bypass is permanently disabled. These compatibility fields
  // therefore remain zero and must not be used for kernel-skip accounting.
  uint64_t bypassedUploadCalls = 0;
  uint64_t bypassedUploadBytes = 0;
  uint64_t bypassFallbackCalls = 0;
  uint64_t bypassStateMismatches = 0;
  uint64_t bypassSideEffectFailures = 0;
  uint64_t inputPreflightRejectedUploads = 0;
  uint64_t dips = 0;
  uint64_t outsideDispatchDips = 0;
  uint64_t dispatchNoUploadDips = 0;
  uint64_t correlatedDips = 0;
  uint64_t unmatchedDips = 0;
  uint64_t uploadFanoutZero = 0;
  uint64_t uploadFanoutOne = 0;
  uint64_t uploadFanoutMany = 0;
  uint64_t maxUploadFanout = 0;
  uint64_t nestedDispatchScopes = 0;
  uint64_t nestedSemanticScopes = 0;
  uint64_t nestedUploadScopes = 0;
  uint64_t scopeFailClosedUploads = 0;
  uint64_t scopeFailClosedKernelFallbacks = 0;
  uint64_t dispatchStackOverflow = 0;
  uint64_t semanticStackOverflow = 0;
  // A dispatch may increment more than one semantic rejection reason.
  uint64_t queryMiss = 0;
  uint64_t conflict = 0;
  uint64_t unknown = 0;
  uint64_t layerMismatch = 0;
  uint64_t scopeForced = 0;
  uint64_t formatHistogram[8] = {};
  uint64_t skinModeHistogram[8] = {};
  uint64_t inputPreflightMissing[kNativeUploadInputPreflightBitCount] = {};
  uint64_t indexTicketFailureReasons[
      kNativeIndexTicketFailureBitCount] = {};
  // Appended diagnostics: keep every pre-existing counter at its original
  // offset and keep the callback ABI independent of telemetry batching.
  uint64_t telemetryFlushes = 0;
  uint64_t telemetryBatchedAdds = 0;
  uint32_t telemetryDeltaPending = 0;
  bool telemetryDeltaFaulted = false;
  // Appended production-light sampled raw-QPC diagnostics. These counters
  // are report-only and never feed observation, authorization or suppression.
  uint32_t productionTimingSamplePeriod = 0;
  uint32_t productionTimingSamplePhase = 0;
  // Clean-endpoint call closure:
  // accepted == fastInclusive == fastComplete + fastCancel;
  // rejected == fallbackInclusive. Recovery/SEH deliberately uses
  // fallbackBegin/body/complete <= fallbackInclusive independently; a
  // no-SEH hard gate may strengthen those inequalities, the producer may not.
  // kernelInclusive == kernelEvaluate and kernelNotify <= kernelOriginal;
  // per callback kind, pinEnter == pinLeave and body <= pinEnter.
  NativeSampledRawTiming productionOuterAdmissionAcceptedTiming;
  NativeSampledRawTiming productionOuterAdmissionRejectedTiming;
  NativeSampledRawTiming productionOuterFastInclusiveTiming;
  NativeSampledRawTiming productionOuterFastBodyTiming;
  NativeSampledRawTiming productionOuterFastCompleteTiming;
  NativeSampledRawTiming productionOuterFastCancelTiming;
  NativeSampledRawTiming productionKernelInclusiveTiming;
  NativeSampledRawTiming productionKernelEvaluateTiming;
  NativeSampledRawTiming productionKernelOriginalTiming;
  NativeSampledRawTiming productionKernelNotifyTiming;
  // Array order is NativeBridgeCallbackKind. Sampling uses the exact manager
  // identity: flushEpoch for Flush, dispatchEpoch for Begin/End, uploadEpoch
  // for Preflight/CpuRewrite/Upload/Fanout and correlated Dip; uncorrelated
  // Dip and every other identity zero are never sampled.
  std::array<NativeSampledRawTiming, kNativeBridgeCallbackKindCount>
      productionCallbackPinEnterTiming{};
  std::array<NativeSampledRawTiming, kNativeBridgeCallbackKindCount>
      productionCallbackBodyTiming{};
  std::array<NativeSampledRawTiming, kNativeBridgeCallbackKindCount>
      productionCallbackPinLeaveTiming{};
  // A snapshot may apply the closure above only when pending is false and
  // started == completed. Logical outer/kernel/event-graph batches and
  // sampled callback lifetimes each occupy exactly one writer generation.
  uint64_t productionTimingWritesStarted = 0;
  uint64_t productionTimingWritesCompleted = 0;
  uint32_t productionTimingWriters = 0;
  bool productionTimingSnapshotPending = false;
  // Appended v2 diagnostics for the admission-rejected generic outer root.
  // FallbackInclusive starts immediately after sampled rejection bookkeeping
  // (admission itself remains in OuterAdmissionRejected) and ends after the
  // detour pin leaves. This excludes sample-only bookkeeping from x256 cost.
  NativeSampledRawTiming productionOuterFallbackInclusiveTiming;
  NativeSampledRawTiming productionOuterFallbackBeginTiming;
  NativeSampledRawTiming productionOuterFallbackBodyTiming;
  NativeSampledRawTiming productionOuterFallbackCompleteTiming;
  // Successful production-light TLS bundles only. Each batch contributes
  // exactly three telemetry additions: this counter, kernelHookCalls and
  // originalKernelCalls. Atomic fallback does not increment this field.
  uint64_t productionFastRejectKernelBatches = 0;
  // Appended v3 production-light event graph. Every producer first checks
  // Bypass/light mode and advances a private TLS ordinal; only the selected
  // 1/256 cohort reads QPC or publishes atomics. Full diagnostics therefore
  // leaves every field below exactly zero.
  NativeSampledRawTiming productionFlushRootTiming;
  NativeSampledRawTiming productionDispatchSemanticLookupTiming;
  NativeSampledRawTiming productionDispatchBeginRootTiming;
  NativeSampledRawTiming productionDispatchEndRootTiming;
  NativeSampledRawTiming productionSemanticInclusiveTiming;
  NativeSampledRawTiming productionSemanticOriginalTiming;
  NativeSampledRawTiming productionDipDeviceRootOutsideTiming;
  NativeSampledRawTiming productionDipDeviceRootNoUploadTiming;
  NativeSampledRawTiming productionDipDeviceRootCorrelatedTiming;
  NativeSampledRawTiming productionDipBridgeOutsideTiming;
  NativeSampledRawTiming productionDipBridgeNoUploadTiming;
  NativeSampledRawTiming productionDipBridgeCorrelatedTiming;
  NativeSampledRawTiming productionDipResolveOutsideTiming;
  NativeSampledRawTiming productionDipResolveNoUploadTiming;
  NativeSampledRawTiming productionDipResolveCorrelatedTiming;
  // Semantic publishes inclusive+original from one physical Apply sample:
  // calls are equal and inclusive ticks are >= original ticks. DIP publishes
  // exactly one matching device-root/bridge class per sampled physical DIP;
  // resolve is an optional subset. NoUpload means a non-zero dispatch epoch
  // that did not correlate, including conservative scope-hazard ownership.
  // Dispatch begin/end use independent TLS ordinals because the cookie is not
  // allocated until the begin root has executed. Arbitrary snapshot windows
  // must treat them as separate populations; no false calls-equal contract is
  // implied even though clean lifetime totals should converge statistically.
  // Number of production-light outside DIPs that proved either the strict
  // pre-resource no-ticket/no-scope/no-poison contract or the immutable-input
  // poison-disjoint late contract.
  uint64_t productionOutsideDipFastPath = 0;
  // A DIP call that is not already covered by an admitted flush owns an
  // independent bridge observer transaction for its entire device/bridge/
  // manager lifetime. Begin/end are lifetime totals; mismatch is fail-closed
  // diagnostics. Successful fast outcomes partition exactly by their cover.
  uint64_t dipObserverTransactionsBegun = 0;
  uint64_t dipObserverTransactionsEnded = 0;
  uint64_t dipObserverTransactionMismatches = 0;
  // Strict production-light outside/no-upload DIPs may publish a smaller
  // drain-visible reader cover before constructing NativeDipObserverScope.
  // Clean endpoints require begin=end=commit+reject, mismatch=0; evidence is a
  // subset of reject that deliberately falls through to the old observer.
  uint64_t dipOutsideReaderBegun = 0;
  uint64_t dipOutsideReaderEnded = 0;
  uint64_t dipOutsideReaderCommits = 0;
  uint64_t dipOutsideReaderRejects = 0;
  uint64_t dipOutsideReaderEvidenceFallbacks = 0;
  uint64_t dipOutsideReaderMismatches = 0;
  uint64_t productionOutsideDipFastByFlush = 0;
  uint64_t productionOutsideDipFastByObserver = 0;
  uint64_t productionOutsideDipFastByReader = 0;
  // Appended v4 sampled admission diagnostics for the two outside-DIP probes.
  // Attempts are derived from successes + sum(rejects), so this is an outcome
  // distribution rather than an independent runtime closure witness.
  // Reject order is localScope, ticketGate, lifecycle, pendingKernel,
  // resetOrRetirement, poisonNeedsInput, poisonCountMismatch, poisonHit.
  // Local detail order is noTransactionCover, cleanSemanticOnly,
  // dispatchActive, semanticHazard, uploadOrMarker, callbackReentry,
  // otherChronology. Each sampled localScope outcome also increments exactly
  // one detail bucket; clean quiescent snapshots require their sums to match.
  // The independent early/late ordinals use distinct phases that are also
  // distinct from productionTimingSamplePhase; full diagnostics leaves every
  // field exactly zero.
  uint64_t productionOutsideDipFastEarlyAttempts = 0;
  uint64_t productionOutsideDipFastEarlySuccesses = 0;
  std::array<uint64_t, kNativeOutsideDipFastRejectReasonCount>
      productionOutsideDipFastEarlyRejects{};
  std::array<uint64_t, kNativeOutsideDipFastLocalRejectReasonCount>
      productionOutsideDipFastEarlyLocalRejects{};
  uint64_t productionOutsideDipFastLateAttempts = 0;
  uint64_t productionOutsideDipFastLateSuccesses = 0;
  std::array<uint64_t, kNativeOutsideDipFastRejectReasonCount>
      productionOutsideDipFastLateRejects{};
  std::array<uint64_t, kNativeOutsideDipFastLocalRejectReasonCount>
      productionOutsideDipFastLateLocalRejects{};
  // Appended v6 exact outside-upload admission outcomes. Bypass-light calls
  // whose entry dispatchDepth is zero form the tracked first-terminal
  // population. Accepted admissions and rejects are disjoint; each attempt
  // increments one accepted scalar or one reject bucket. Generic completion
  // additionally records the two cross-boundary exceptions needed to reconcile
  // entry ownership with final PublishCompletedUpload ownership:
  //   trackedOutside = attempt - cancellation - lifecycleExcluded
  //                    - trackedResolvedInside;
  //   expectedOutside = trackedOutside + untrackedResolvedOutside.
  // DispatchOwned remains a hard-zero exact reject sentinel because its final
  // outside subset lives only in the independent cross-terminal counter.
  uint64_t productionOutsideAdmissionAcceptedNoPoison = 0;
  uint64_t productionOutsideAdmissionAcceptedWithPoison = 0;
  uint64_t productionOutsideAdmissionCancellations = 0;
  uint64_t productionOutsideAdmissionLifecycleExcluded = 0;
  uint64_t productionOutsideAdmissionTrackedResolvedInside = 0;
  uint64_t productionOutsideAdmissionUntrackedResolvedOutside = 0;
  uint64_t productionOutsideCoverFlushBegins = 0;
  uint64_t productionOutsideCoverSemanticBegins = 0;
  uint64_t productionOutsideCoverIndependentBegins = 0;
  uint64_t productionOutsideIndependentPinBegins = 0;
  uint64_t productionOutsideIndependentPinEnds = 0;
  std::array<uint64_t, kNativeOutsideUploadRejectReasonCount>
      productionOutsideAdmissionRejectNoPoison{};
  std::array<uint64_t, kNativeOutsideUploadRejectReasonCount>
      productionOutsideAdmissionRejectWithPoison{};
  // Snapshot-derived totals. A relaxed/off-thread export may expose the
  // values while snapshotAvailable is false. Forced clean endpoints require
  // exact telemetry publication, quiescent TLS, live ingress, no cancellation
  // or lifecycle exclusion, and then both closure booleans must be true.
  uint64_t productionOutsideAdmissionAcceptedTotal = 0;
  uint64_t productionOutsideAdmissionRejectNoPoisonTotal = 0;
  uint64_t productionOutsideAdmissionRejectWithPoisonTotal = 0;
  uint64_t productionOutsideAdmissionRejectTotal = 0;
  uint64_t productionOutsideAdmissionAttemptTotal = 0;
  uint64_t productionOutsideAdmissionTrackedResolvedOutside = 0;
  uint64_t productionOutsideAdmissionResolvedExpectedOutside = 0;
  bool productionOutsideAdmissionUnknownHardZero = false;
  bool productionOutsideAdmissionSnapshotAvailable = false;
  bool productionOutsideAdmissionAcceptedResolutionClean = false;
  bool productionOutsideAdmissionOutsideClosureClean = false;
  // Aliases of the existing accepted-admission, fast-complete and
  // fallback-inclusive roots. Six accepted classes split entry poison state
  // by flush, semantic or independent coverage; reject buckets use the exact
  // enum above. Their
  // calls/ticks/max close independently against the parent root and must never
  // be summed horizontally with that parent.
  std::array<NativeSampledRawTiming,
             kNativeOutsideUploadAdmissionClassCount>
      productionOuterAdmissionAcceptedClassTiming{};
  std::array<NativeSampledRawTiming,
             kNativeOutsideUploadAdmissionClassCount>
      productionOuterFastCompleteClassTiming{};
  std::array<NativeSampledRawTiming,
             kNativeOutsideUploadRejectReasonCount>
      productionOuterFallbackReasonTiming{};
  bool productionOuterAdmissionAliasSnapshotAvailable = false;
  bool productionOuterAdmissionAcceptedClassClosureClean = false;
  bool productionOuterFastCompleteClassClosureClean = false;
  bool productionOuterFallbackReasonClosureClean = false;
  bool productionOuterFallbackReasonUnknownHardZero = false;
  NativeSampledRawTiming productionOuterDispatchSealAdmissionTiming;
  NativeSampledRawTiming productionOuterDispatchSealInclusiveTiming;
  NativeSampledRawTiming productionOuterDispatchSealBodyTiming;
  NativeSampledRawTiming productionOuterDispatchSealCompleteTiming;
  NativeSampledRawTiming productionOuterDispatchSealCancelTiming;
  // Appended v7 exact-negative Common-dispatch seal diagnostics. These
  // counters never authorize work; the manager's current-flush exact view and
  // the bridge's TLS/lifecycle proof do. Physical sealed uploads remain native
  // CPU uploads and are deliberately absent from ABI-9 upload/DIP/fanout
  // summaries. Their independent ledger closes at clean endpoints:
  //   proposals = proposalAccepted + proposalRejected;
  //   proposalAccepted = scopeCommits + proposalAborted;
  //   scopeCommits = scopeEnds;
  //   uploadsStarted = uploadsCompleted = fanoutZero+One+Many;
  //   dips = dipsWithUpload + dipsNoUpload;
  //   fanoutDipTotal = dipsWithUpload.
  uint64_t dispatchCpuOnlySealProposals = 0;
  uint64_t dispatchCpuOnlySealProposalAccepted = 0;
  uint64_t dispatchCpuOnlySealProposalRejected = 0;
  uint64_t dispatchCpuOnlySealProposalAborted = 0;
  uint64_t dispatchCpuOnlySealScopeCommits = 0;
  uint64_t dispatchCpuOnlySealScopeEnds = 0;
  uint64_t dispatchCpuOnlySealInvalidations = 0;
  uint64_t dispatchCpuOnlySealUploadsStarted = 0;
  uint64_t dispatchCpuOnlySealUploadsCompleted = 0;
  uint64_t dispatchCpuOnlySealVertices = 0;
  uint64_t dispatchCpuOnlySealBytes = 0;
  uint64_t dispatchCpuOnlySealKernelCalls = 0;
  uint64_t dispatchCpuOnlySealKernelNormalReturns = 0;
  uint64_t dispatchCpuOnlySealDips = 0;
  uint64_t dispatchCpuOnlySealDipsWithUpload = 0;
  uint64_t dispatchCpuOnlySealDipsNoUpload = 0;
  uint64_t dispatchCpuOnlySealFanoutZero = 0;
  uint64_t dispatchCpuOnlySealFanoutOne = 0;
  uint64_t dispatchCpuOnlySealFanoutMany = 0;
  uint64_t dispatchCpuOnlySealFanoutDipTotal = 0;
  uint64_t dispatchCpuOnlySealMarkerConflicts = 0;
  // Appended v8 managerless exact-negative Common-dispatch seal diagnostics.
  // Publication is direct-atomic because it executes inside the flush callback;
  // query/classification and scope/end counters use the render-thread telemetry
  // delta. Clean production-light endpoints require:
  //   localViewPublishAttempts = localViewPublishes + localViewRejects;
  //   localViewQueries = authorityRejects + candidateRejects + localCommits;
  //   managerDispatchNativeCpuOnlyScopes = managerDispatchNativeCpuOnlyEnds;
  //   proposalAccepted + localCommits = scopeCommits + proposalAborted.
  uint64_t managerDispatchNativeCpuOnlyScopes = 0;
  uint64_t managerDispatchNativeCpuOnlyEnds = 0;
  uint64_t dispatchCpuOnlySealLocalViewPublishAttempts = 0;
  uint64_t dispatchCpuOnlySealLocalViewPublishes = 0;
  uint64_t dispatchCpuOnlySealLocalViewRejects = 0;
  uint64_t dispatchCpuOnlySealLocalViewQueries = 0;
  uint64_t dispatchCpuOnlySealLocalViewAuthorityRejects = 0;
  uint64_t dispatchCpuOnlySealLocalViewCandidateRejects = 0;
  uint64_t dispatchCpuOnlySealLocalViewCommits = 0;
};

inline constexpr uint32_t kNativeBridgeCallbackAbi = 9;
inline constexpr uint32_t kNativeBridgeCallbackDrainTimeoutMs = 2000u;

enum class NativeBridgeCallbackStatus : uint8_t {
  Success = 0,
  Disabled,
  InvalidArgument,
  AbiMismatch,
  OwnerMismatch,
  ExpectedPointerMismatch,
  ReentrantOperation,
  QuiescencePending,
  Timeout,
};

// The device-owned manager retains this immutable table for its full
// registration lifetime. Callbacks are synchronous, run on the observing
// render thread, and must not throw or change registration reentrantly.
struct NativeBridgeCallbacks {
  uint32_t abiVersion = kNativeBridgeCallbackAbi;
  uint32_t structSize = 0;
  void* userData = nullptr;
  void (*onFlush)(void*, const NativeFlushObservation&) = nullptr;
  void (*onDispatchBegin)(void*, const NativeDispatchObservation&) = nullptr;
  void (*onDispatchEnd)(void*, const NativeDispatchSummary&) = nullptr;
  bool (*onBypassPreflight)(void*, const NativeUploadObservation&,
                            NativeBypassAuthorization*) = nullptr;
  // Invoked only after the original kernel trampoline returns normally and
  // before its caller can execute native VB Unlock.
  bool (*resolveCpuRewriteOutputProof)(
      void*, const NativeUploadObservation&,
      NativeCpuRewriteOutputProof*) = nullptr;
  void (*onUpload)(void*, const NativeUploadObservation&) = nullptr;
  void (*onDip)(void*, const NativeDipObservation&) = nullptr;
  void (*onUploadFanout)(void*, const NativeUploadFanoutObservation&) = nullptr;
};

// Performs the mode/fingerprint preflight. A true result means the native
// detours may be installed; observation remains disabled until the installer
// confirms that the complete hook set is live.
bool InitializeNativeBridge(uintptr_t gameBase,
                            const NativeBridgeAddresses& addresses);
void SetNativeBridgeHooksInstalled(bool installed);
// Called by the hook owner only after bypass/callback/transaction ingress is
// closed and the first poison-retiring drain has reached a clean endpoint.
// New unowned device DIPs then stop entering bridge state; already admitted
// observer scopes remain counted and must be drained a second time.
bool CloseNativeDipObserverIngressForRemoval() noexcept;
// Called only after all owned detours are disabled and quiescent. Created
// hooks and trampolines remain process-lifetime even on rollback.
void FinalizeNativeBridgeHooksRemoved();
// Reopens only raw observation/poison suppression after a failed hook drain.
// Manager callbacks and new bypass authorization remain disabled.
void RetainNativeBridgeSafetyObservation();
bool NativeBridgeHooksEnabled();
// VS-B1 在当前 dispatch 没有发布候选租约、且没有遗留 poison 时，
// 普通 CPU 上传/蒙皮只需执行原生函数，不应进入全局观察状态机。
bool NativeGpuSkinB1UnobservedNativePathSafe() noexcept;
// 在 dispatch 建立前判断当前 Common key 是否已被本批候选视图明确排除。
// 仅用于省略负候选的语义查询，不授予任何 GPU 绘制权限。
bool NativeGpuSkinB1DispatchKnownNegative(
    NativeDispatchPath path, uintptr_t renderablePart,
    uint32_t layerIndex) noexcept;
GpuSkinRuntimeConfig GetNativeBridgeRuntimeConfig();
NativeBridgeFingerprint GetNativeBridgeFingerprint();
NativeBridgeCounters GetNativeBridgeCounters();
// Hook-wrapper timing covers code outside the bridge functions themselves.
// Values are raw high-resolution-clock ticks and are cumulative diagnostics
// only; they never participate in authorization or suppression.
void RecordNativeOuterUploadTiming(uint64_t ticks) noexcept;
void RecordNativeOriginalKernelTiming(uint64_t ticks) noexcept;
bool NativeBridgeFullDiagnosticsEnabled() noexcept;
bool NativeBridgeProductionLightTimingEnabled() noexcept;
void RecordNativeProductionTiming(
    NativeProductionTimingStage stage, uint64_t ticks) noexcept;
void RecordNativeProductionTimingBatch(
    const NativeProductionTimingEntry* entries, size_t count) noexcept;
// Render-thread snapshot of report-only poison provenance. Samples contain
// only poison-only hits (the physical DIP did not already own a bypassed
// upload); no field participates in suppression or takeover decisions.
NativePoisonDiagnosticSnapshot GetNativePoisonDiagnosticSnapshot();

// Registration publishes only into an empty slot. Unregistration compares
// both owner and expected table, clears that exact table with CAS, then waits
// for all callback pins that could still reference it to drain. Timeout leaves
// the slot unpublished but reserves the retiring owner/table; that owner must
// retry unregistration and keep both objects alive until Success.
NativeBridgeCallbackStatus RegisterNativeBridgeCallbacks(
    const void* owner, const NativeBridgeCallbacks* callbacks);
NativeBridgeCallbackStatus UnregisterNativeBridgeCallbacks(
    const void* owner, const NativeBridgeCallbacks* expectedCallbacks,
    uint32_t timeoutMs = kNativeBridgeCallbackDrainTimeoutMs);

// Requests a full TLS/transaction reset. Cross-thread or in-flight requests are
// generation-deferred and block every later bypass authorization until the
// observing render thread reaches a safe boundary. Non-empty poison is never
// forgotten: it must be exactly CPU-overwritten or have a retired native
// resource identity before reset completion.
NativeBridgeResetResult RequestNativeBridgeReset(
    NativePoisonLedgerResetReason reason);
bool AcknowledgeNativeBridgeOwnerEpochRetired(uint64_t resetGeneration);
// D3D9CommonBuffer destruction may occur on any thread. The event is queued
// without allocation and consumed only at an observed-render-thread boundary.
void NotifyNativeVertexResourceRetired(
    uintptr_t commonResource, uint64_t resourceGeneration) noexcept;

// Called only from the effective DIRECT D3DLOCK_DISCARD branch after a new
// full-buffer map allocation has replaced the old one. Unlike generic DXVK
// storage relocation, this is destructive app-side invalidation, so every
// poison range for the prior backing is no longer drawable through this
// resource. Returns the number of ranges retired on the observed render TLS.
uint32_t NotifyNativeVertexDirectDiscard(
    uintptr_t commonResource, uint64_t resourceGeneration,
    uint64_t previousMapAllocationGeneration,
    uint64_t newMapAllocationGeneration, bool directMapping) noexcept;
NativeBridgeQuiescenceSnapshot GetNativeBridgeQuiescenceSnapshot();
bool WaitForNativeBridgeQuiescence(
    uint32_t timeoutMs, bool requirePoisonRetired,
    NativeBridgeQuiescenceSnapshot* finalSnapshot = nullptr);

// Compatibility entry point used by the device reset sites. This now requests
// the full bridge reset and returns only the immediately retired poison count.
uint32_t ResetNativePoisonLedger(NativePoisonLedgerResetReason reason);

void NotifyNativeFlush(const NativeFlushObservation& observation);
bool BeginNativeFlushTransaction() noexcept;
void EndNativeFlushTransaction() noexcept;
// Production-light Bypass may pass an upload with no dispatch owner and at
// most one clean top-level semantic scope straight through the original
// Game.dll upload and CPU kernel. This removes bridge bookkeeping only; it
// never skips either native function. The render hook must pair every
// successful non-zero cookie with an exact-cookie complete or cancel. gx/count
// are used only when an outstanding poison range requires a fault-safe proof
// that this native CPU upload cannot overwrite it.
uint64_t TryBeginNativeOutsideUploadFastPath(
    uintptr_t gxDeviceD3d, uint32_t vertexCount,
    NativeOutsideUploadPopulationToken* populationToken,
    NativeOutsideUploadAdmissionProbe* sampledProbe = nullptr,
    uint64_t outputByteCount = 0u,
    uint64_t* outsidePoisonShadowCookie = nullptr) noexcept;
void CompleteNativeOutsideUploadFastPath(uint64_t cookie) noexcept;
void CancelNativeOutsideUploadFastPath(uint64_t cookie) noexcept;
// Cheapest safe outside route: no dispatch owner, no poison anywhere and no
// pending authorization. The receipt can only force the original outer upload
// and original native kernel; it never enters ABI-9, O1 or poison mutation.
uint64_t TryBeginNativeOutsideNoPoisonDirectOriginal(
    uintptr_t gxDeviceD3d, uint32_t vertexCount) noexcept;
bool TryRouteNativeOutsideNoPoisonDirectOriginalKernel(
    uintptr_t gxDeviceD3d, void* mappedDst,
    bool originalTrampolineAvailable) noexcept;
void NotifyNativeOutsideNoPoisonDirectOriginalReturned(
    uintptr_t gxDeviceD3d, void* mappedDst) noexcept;
void CompleteNativeOutsideNoPoisonDirectOriginal(
    uint64_t cookie) noexcept;
void CancelNativeOutsideNoPoisonDirectOriginal(uint64_t cookie) noexcept;
// Unified hook entry. It first consumes a committed exact-negative Common
// dispatch seal and otherwise preserves the outside-only admission above.
// Both outcomes still execute the complete native outer upload and CPU kernel.
uint64_t TryBeginNativeCpuOnlyUploadFastPath(
    uintptr_t gxDeviceD3d, uint32_t vertexCount,
    uint64_t outputByteCount,
    NativeOutsideUploadPopulationToken* populationToken,
    NativeOutsideUploadAdmissionProbe* sampledProbe = nullptr,
    bool* dispatchCpuOnlySeal = nullptr,
    uint64_t* outsidePoisonShadowCookie = nullptr) noexcept;
void CompleteNativeCpuOnlyUploadFastPath(uint64_t cookie) noexcept;
void CancelNativeCpuOnlyUploadFastPath(uint64_t cookie) noexcept;
// Shared outer-owned lifetime. O0/O1a remain strictly report-only. The separate
// production O1 token may settle a manager-free CPU-only upload, and may clear
// only the exact poison bytes rewritten by a real Lock + normal CPU kernel +
// exact successful Unlock + successful outer return. Settlement must precede
// fast completion, which may process a pending reset. Cancellation and reset
// abortion remain disjoint terminal classes.
void SettleNativeOutsidePoisonShadow(
    uint64_t cookie, int32_t originalResult) noexcept;
void CancelNativeOutsidePoisonShadow(uint64_t cookie) noexcept;
// This immutable process-wide mask is published before the native hooks are
// installed. The inline gate removes the cross-module/TLS call from ordinary
// D3D9 Lock/Unlock traffic when neither report sidecar nor a provisional
// production O1 token is active. The policy bits themselves remain diagnostic
// only and can never authorize an upload or clear poison.
extern std::atomic<uint8_t> g_nativeOutsidePoisonSidecarPolicyMask;
// Non-zero only while an outer hook owns a provisional production O1 token.
// This cross-TU collection gate is never GPU-bypass authority.
extern std::atomic<uint32_t> g_nativeOutsidePoisonAuthorityActive;
// 仅在 VS-B1 的精确 NativeCpuOnly dispatch 内置位；该窗口已经由外层
// Flush detour pin 覆盖，内部 upload/kernel 可借用它而不再做原子计数。
extern thread_local bool g_nativeGpuSkinB1BorrowedFlushPinPassThrough;
inline bool NativeGpuSkinB1BorrowedFlushPinPassThroughFast() noexcept {
  return g_nativeGpuSkinB1BorrowedFlushPinPassThrough;
}
bool NativeOutsidePoisonVertexLockShadowRequiredSlow() noexcept;
inline bool NativeOutsidePoisonVertexLockShadowRequired() noexcept {
  constexpr uint8_t sidecarMask = static_cast<uint8_t>(
      GpuSkinOutsidePoisonSidecarPolicy::Both);
  if ((g_nativeOutsidePoisonSidecarPolicyMask.load(
           std::memory_order_acquire) & sidecarMask) == 0u &&
      g_nativeOutsidePoisonAuthorityActive.load(
          std::memory_order_acquire) == 0u) {
    return false;
  }
  return NativeOutsidePoisonVertexLockShadowRequiredSlow();
}
bool TryNotifyNativeOutsidePoisonVertexLockFastNoOverlap(
    const NativeOutsidePoisonVertexLockFastInput& input) noexcept;
void NotifyNativeOutsidePoisonVertexLock(
    const NativeOutsidePoisonVertexLockEvidence& evidence) noexcept;
// Unlock observation remains TLS-only. O1a consumes it as report-only evidence;
// production O1 consumes the same immutable endpoint as one mandatory part of
// its fail-closed settlement proof. The caller must capture active-lock state
// before decrement/clear and completion state after the real D3D9 HRESULT.
bool NativeOutsidePoisonVertexUnlockShadowRequiredSlow() noexcept;
inline bool NativeOutsidePoisonVertexUnlockShadowRequired() noexcept {
  constexpr uint8_t o1Mask = static_cast<uint8_t>(
      GpuSkinOutsidePoisonSidecarPolicy::O1);
  if ((g_nativeOutsidePoisonSidecarPolicyMask.load(
           std::memory_order_acquire) & o1Mask) == 0u &&
      g_nativeOutsidePoisonAuthorityActive.load(
          std::memory_order_acquire) == 0u) {
    return false;
  }
  return NativeOutsidePoisonVertexUnlockShadowRequiredSlow();
}
bool NativeOutsidePoisonVertexUnlockFastNoOverlapRequiredSlow() noexcept;
inline bool NativeOutsidePoisonVertexUnlockFastNoOverlapRequired() noexcept {
  if (g_nativeOutsidePoisonAuthorityActive.load(
          std::memory_order_acquire) == 0u) {
    return false;
  }
  return NativeOutsidePoisonVertexUnlockFastNoOverlapRequiredSlow();
}
bool NotifyNativeOutsidePoisonVertexUnlockFastNoOverlap(
    const NativeOutsidePoisonVertexUnlockFastInput& input) noexcept;
void NotifyNativeOutsidePoisonVertexUnlock(
    const NativeOutsidePoisonVertexUnlockEvidence& evidence) noexcept;

// Manager-only synchronous sidecar invoked from the current onFlush callback
// after complete assembly and (when present) accepted host submission. Empty
// authoritative views use candidates=nullptr/count=0. The bridge publishes
// only a sorted-unique, fixed-capacity copy and never changes callback ABI 9.
bool PublishNativeDispatchCpuOnlySealView(
    uint64_t flushEpoch,
    const NativeDispatchCpuOnlySealCandidate* candidates,
    uint32_t count) noexcept;
// Synchronous manager-thunk exception settlement. Native callbacks execute on
// the producing render thread, so this revokes that thread's passive current-
// flush view before manager recovery can throw, block, or swallow the error.
void InvalidateNativeDispatchCpuOnlySealViewForCallbackException() noexcept;

// Manager-only, synchronous ABI-9 sidecar. The manager may call this only as
// the last non-throwing action of a successfully paired DispatchBegin after
// proving its authoritative current-flush candidate view contains no exact
// `(renderablePart, layerIndex)` candidate. The bridge merely records a
// proposal; it becomes consumable only after the callback returns and the
// bridge commits BeginIssued under the same TLS dispatch frame.
bool ProposeCurrentNativeDispatchCpuOnlySeal(
    const NativeDispatchObservation& observation) noexcept;
void ReportNativeDispatchSemanticFailures(uint32_t failureMask);

// VS-B1 生产轻量门：当 manager 已发布当前 flush 的精确负候选集合时，
// 非候选 Common/Special dispatch 不需要创建 bridge frame；候选仍必须走
// 完整 consumer-fenced 路径。该门只在显式 B1 + light diagnostics 生效，
// Compute、VS-A/B0、full diagnostics 和异常状态一律返回 false。
bool NativeGpuSkinDispatchObservationRequired(
    NativeDispatchPath path, uintptr_t renderablePart,
    uint32_t layerIndex) noexcept;

// Apply 钩子在非候选 dispatch 中也不需要建立 SemanticFrame。候选或任何
// 非 B1 路线仍返回 true，避免把语义观察误当成授权。
bool NativeGpuSkinSemanticObservationRequired() noexcept;

uint64_t BeginNativeDispatchScope(NativeDispatchPath path,
                                  uintptr_t sceneNode,
                                  uintptr_t renderablePart,
                                  uint32_t layerIndex,
                                  int32_t stage,
                                  int32_t batchTag,
                                  bool forceFailClosed = false);
void UpdateNativeDispatchScope(uint64_t cookie,
                               int32_t stage,
                               int32_t batchTag);
void EndNativeDispatchScope(uint64_t cookie);

class NativeDispatchScope {
public:
  NativeDispatchScope(NativeDispatchPath path,
                      uintptr_t sceneNode,
                      uintptr_t renderablePart,
                      uint32_t layerIndex,
                      int32_t stage,
                      int32_t batchTag,
                      bool forceFailClosed = false,
                      bool borrowedFlushPinPassThrough = false);
  ~NativeDispatchScope();

  NativeDispatchScope(const NativeDispatchScope&) = delete;
  NativeDispatchScope& operator=(const NativeDispatchScope&) = delete;

  void updateStage(int32_t stage);
  void updateBatchTag(int32_t batchTag);

private:
  uint64_t m_cookie = 0;
  bool m_borrowedFlushPinPassThrough = false;
  bool m_previousBorrowedFlushPinPassThrough = false;
};

class NativeSemanticScope {
public:
  NativeSemanticScope(uintptr_t geosetData,
                      uintptr_t layerDispatch,
                      uintptr_t layerState,
                      uintptr_t callerAddress);
  ~NativeSemanticScope();

  NativeSemanticScope(const NativeSemanticScope&) = delete;
  NativeSemanticScope& operator=(const NativeSemanticScope&) = delete;

private:
  uint64_t m_cookie = 0;
};

NativeUploadObservation BeginNativeUpload(const NativeUploadCall& call);
class NativeUploadInFlightScope {
public:
  explicit NativeUploadInFlightScope(NativeUploadObservation& observation);
  ~NativeUploadInFlightScope();

  NativeUploadInFlightScope(const NativeUploadInFlightScope&) = delete;
  NativeUploadInFlightScope& operator=(const NativeUploadInFlightScope&) =
      delete;

private:
  NativeUploadObservation* m_observation = nullptr;
  uint64_t m_cookie = 0;
};

// Called only by the 0x6F0EDDC0 detour. The hook may call the native trampoline
// only for CallOriginal. Both other outcomes are irreversible no-rescue paths.
NativeKernelDetourOutcome EvaluateNativeSkinKernelDetour(
    uintptr_t gxDeviceD3d, void* mappedDst);
// Called by the kernel detour only after the selected original trampoline
// returns to the detour normally. A caller SEH transfer bypasses this call.
void NotifyNativeSkinKernelOriginalReturned(
    uintptr_t gxDeviceD3d, void* mappedDst) noexcept;
// CallOriginalNoNotify has no generic observation, but an exact dispatch seal
// still needs a post-trampoline endpoint distinct from outer completion. The
// hook calls this only after the original kernel returns normally.
void NotifyNativeCpuOnlySkinKernelOriginalReturned(
    uintptr_t gxDeviceD3d, void* mappedDst) noexcept;
void CompleteNativeUpload(NativeUploadObservation& observation,
                           int32_t originalResult);

// These APIs do not consume or clear the current upload. One upload may fan
// out to zero, one, or many DIPs until the next upload or dispatch-scope end.
// The D3D9 manager must report the original native stream/FVF/base state before
// any P3/P4 stream override; the upload observation remains active until its
// exact 0/1/N DIP fan-out is finalized.
bool QueryCurrentNativeUpload(NativeUploadObservation& observation);

// Sidecar publication from the D3D9 host during the synchronous proof
// callback. Keeping this outside callback payloads preserves callback ABI 9.
void PublishNativeVertexStorageDiagnostics(
    const NativeVertexOutputProof& proof,
    const NativeVertexStorageDiagnostics& diagnostics);

/**
 * @brief Starts a ticket after D3D9 IB Lock returns.
 * @param input Exact COM Lock arguments, identities, result, and mapped output.
 * @return Non-zero ticket generation when a bypassed upload is being tracked.
 * @note The mapped pointer is validated synchronously and never retained.
 */
uint64_t NotifyNativeIndexBufferLock(const NativeIndexLockInput& input);

/**
 * @brief Fault-safely compares mapped bytes with the private expected snapshot.
 * @param input Exact ticket, identity, Lock range, flags, and live mapping.
 * @return True only for a bitwise-exact INDEX16 match; hashes are diagnostic.
 * @warning This is the only API allowed to read the mapped pointer.
 */
bool ValidateNativeIndexBufferBeforeUnlock(
    const NativeIndexUnlockInput& input);

/**
 * @brief Records the exact D3D9 IB Unlock result for a ticket.
 * @param input Ticket generation, resource identity, and Unlock HRESULT.
 */
void NotifyNativeIndexBufferUnlock(const NativeIndexOperationInput& input);

/**
 * @brief Records SetIndices and publishes actual-safe proof when exact.
 * @param input Ticket generation, resource identity, and SetIndices HRESULT.
 */
void NotifyNativeSetIndices(const NativeIndexOperationInput& input);

// Covers every bridge access made by one D3D9 DIP, including fast probes,
// immutable resource capture, slow poison observation and manager resolve.
// A live flush transaction is borrowed without another process-wide atomic;
// otherwise the outermost scope publishes an independent observer pin before
// touching TLS. Nested scopes borrow the opaque TLS cookie.
class NativeDipObserverScope {
public:
  NativeDipObserverScope() noexcept;
  ~NativeDipObserverScope() noexcept;

  NativeDipObserverScope(const NativeDipObserverScope&) = delete;
  NativeDipObserverScope& operator=(const NativeDipObserverScope&) = delete;
  NativeDipObserverScope(NativeDipObserverScope&&) = delete;
  NativeDipObserverScope& operator=(NativeDipObserverScope&&) = delete;

  explicit operator bool() const noexcept { return m_admitted; }
  bool failedClosed() const noexcept { return m_failClosed; }
  bool ownsIndependentTransaction() const noexcept {
    return m_coverKind == 3u;
  }
  uint64_t cpuOnlySealCookie() const noexcept {
    return m_cpuOnlySealCookie;
  }

private:
  uint64_t m_cookie = 0u;
  uint64_t m_cpuOnlySealCookie = 0u;
  uint8_t m_coverKind = 0u;
  bool m_admitted = false;
  bool m_failClosed = false;
};

// False skips the device resolver. Outside-dispatch/no-active-upload DIPs are
// raw-only and emit no manager callback unless an existing scope hazard owns
// them. Every path still checks the poison ledger; a hit returns true and sets
// requiresSuppression even without an active upload or manager callback.
// Production-light may record the strict outside/no-ticket/no-poison subset
// before the device constructs resource identities. This never authorizes a
// takeover or suppresses a native draw.
// With no input, admission additionally requires an empty poison ledger. When
// an unrelated D3D9 index ticket or a live poison range prevents that earliest
// route, the device may retry with its already-built immutable DIP input. The
// bridge then admits only an exact resource/layout/range proof that excludes
// every poison range; a correlated ticket or any ambiguous proof stays slow.
bool TryRecordNativeOutsideDipFastPath(
    const NativeDipInput* input,
    bool externalTicketPresent,
    uint64_t cpuOnlySealCookie) noexcept;
// Strict outside/no-upload production-light terminal. A cheap local prefilter
// runs before any global activity. Candidates then own a dedicated reader
// cover that is visible to reset and hook-removal drains; no finite snapshot
// sequence is accepted as a substitute for that cover. Declare this scope
// before the device's production-timing object so the latter is destroyed and
// publishes its sampled batch while the cover is still active. Every 127th
// admitted candidate deliberately falls through to the complete old observer
// as a same-run control. Commit records ordinary raw/outside/unmatched DIP
// accounting and never authorizes or consumes GPU output.
class NativeOutsideDipReaderScope {
public:
  explicit NativeOutsideDipReaderScope(
      bool externalTicketPresent) noexcept;
  ~NativeOutsideDipReaderScope() noexcept;

  NativeOutsideDipReaderScope(const NativeOutsideDipReaderScope&) = delete;
  NativeOutsideDipReaderScope& operator=(
      const NativeOutsideDipReaderScope&) = delete;
  NativeOutsideDipReaderScope(NativeOutsideDipReaderScope&&) = delete;
  NativeOutsideDipReaderScope& operator=(
      NativeOutsideDipReaderScope&&) = delete;

  explicit operator bool() const noexcept { return m_admitted; }
  void Commit() noexcept;
  bool committed() const noexcept { return m_committed; }

private:
  bool m_admitted = false;
  bool m_committed = false;
};
// Resource-free DIP terminal for a committed exact-negative Common dispatch.
// The returned observation always has a non-zero dispatch epoch and remains
// manager-NoUpload/unmatched even when its lightweight sealed-upload fanout is
// tracked internally. This preserves ABI-9 summaries while allowing the
// device to skip stream/index snapshots and normal draw-chain resolution.
bool TryRecordNativeDispatchCpuOnlyDipFastPath(
    bool externalTicketPresent,
    NativeDipObservation* observation) noexcept;
// Storage generations are report-only provenance for poison-only hits. The
// device can avoid walking the current DXVK backing when no ledger can hit.
bool NativeDipStorageDiagnosticsRequired() noexcept;
bool NotifyNativeDrawIndexedPrimitive(const NativeDipInput& input,
                                      const NativeDipDiagnosticInput& diagnostic,
                                      NativeDipObservation* observation);

}  // namespace dxvk::war3::gpu_skin
