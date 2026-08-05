#pragma once

#include "war3_gpu_skin_native_bridge.h"
#include "war3_gpu_skin_resources.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace dxvk::war3::gpu_skin {

// Manager-only reasons cover native pairing and conservative eligibility
// failures that are intentionally not part of the resource allocator ABI.
enum class GpuSkinManagerFallbackReason : uint8_t {
  None = 0,
  ObserveOnly,
  ModeNotProducing,
  ConsumerNotAllowed,
  MissingFlushRequest,
  InvalidFlushEpoch,
  InvalidFlushArray,
  PendingBatchNotConsumed,
  InvalidRenderablePart,
  SpecialDispatch,
  UnsupportedDispatchPath,
  TransparentDispatch,
  UnsupportedStage,
  UnsupportedSkinMode,
  NativePreflightFailed,
  LayoutUnknown,
  LayoutChanged,
  UnsupportedOutputFormat,
  ModelRecordMiss,
  StaticResourceMiss,
  StaticResourcePending,
  StaticResourceInvalid,
  MultiPrimitiveSlice,
  PaletteSlotUnreadable,
  PaletteSlotInvalid,
  PaletteBaseUnreadable,
  PaletteCountInvalid,
  PaletteRangeUnreadable,
  PalettePointerMismatch,
  PaletteCountMismatch,
  PaletteContentMismatch,
  UploadEpochMismatch,
  DipEpochMismatch,
  DipOrdinalMismatch,
  DipSignatureMismatch,
  PreparedDrawMiss,
  BatchNotSubmitted,
  UploadRetirementMissing,
  OutputRetirementFailed,
  BudgetExhausted,
  RetirementBackpressure,
  DeviceEpochRequiresRebind,
  RenderThreadMismatch,
  BatchClaimFailed,
  CallbackReentry,
  CallbackException,
  BypassModeRequired,
  BypassPreflightFailed,
  BypassHostRejected,
  BypassAuthorizationMismatch,
  BypassPostCommitMismatch,
  BypassRestoreFailure,
  BypassFused,
  SmallBatchCpuPreferred,
  Count,
};

inline constexpr size_t kGpuSkinManagerFallbackReasonCount =
    static_cast<size_t>(GpuSkinManagerFallbackReason::Count);

enum class GpuSkinStrictUploadRejectReason : uint8_t {
  None = 0,
  DispatchPath,
  Stage,
  SkinMode,
  InputPreflight,
  OutputFormat,
  RenderableIdentity,
  CpuCompletion,
  BypassAuthorization,
  Count,
};

inline constexpr size_t kGpuSkinStrictUploadRejectReasonCount =
    static_cast<size_t>(GpuSkinStrictUploadRejectReason::Count);

// This key contains only manager-owned identifiers. The pointer fields are
// identity values and must never be dereferenced by consumers.
struct GpuSkinPreparedDrawKey {
  uint64_t frameTag = 0;
  uint64_t flushEpoch = 0;
  uint64_t batchId = 0;
  uintptr_t renderablePart = 0;
  uintptr_t geosetData = 0;
  uint32_t layerIndex = 0;
  uint32_t outputFormat = 0;
  uint32_t token = 0;
  uint32_t reserved = 0;
};

struct GpuSkinResolvedDraw {
  GpuSkinPreparedDrawKey key;
  OutputLease lease;
  GpuSkinInputLease inputLease;
  GpuSkinParityMetadata parity;
  GpuSkinConsumerPlan plan;
  GpuSkinConsumerLedger ledger;
  uint64_t leaseId = 0;
  uint64_t bypassFuseKey = 0;
  GpuSkinManagerFallbackReason fallback =
      GpuSkinManagerFallbackReason::None;
  bool exactMatch = false;

  // This authorizes the main-stream override. nativeUploadBypassed separately
  // records the irreversible fact that the source CPU kernel was skipped;
  // only an exact result carrying a live GPU lease may avoid suppression.
  bool mainOverrideAllowed = false;
  bool nativeUploadBypassed = false;
  // DIP-side provenance is diagnostic/control metadata only; it never changes
  // the Game.dll hook ABI. It distinguishes the primary correlated takeover
  // from a later draw that merely intersects a still-poisoned native range.
  bool nativeRangePoisoned = false;
  bool nativePoisonHadBypassedSource = false;
  bool nativeDipCorrelated = false;
  bool nativeDipExact = false;
  uint32_t nativeDipFlags = NativeDipFlagNone;
  uint32_t nativeDipPrimitiveCount = 0;
  uint32_t nativeDipNumVertices = 0;
  uint64_t nativeDipUploadEpoch = 0;

  explicit operator bool() const {
    return exactMatch && static_cast<bool>(lease) &&
           fallback == GpuSkinManagerFallbackReason::None;
  }

  bool requiresSuppression() const {
    return nativeUploadBypassed &&
        (!exactMatch || !static_cast<bool>(lease) ||
         fallback != GpuSkinManagerFallbackReason::None);
  }
};

struct GpuSkinPreparedDrawInfo {
  GpuSkinPreparedDrawKey key;
  OutputLease lease;
  GpuSkinInputLease inputLease;
  GpuSkinParityMetadata parity;
  uint64_t leaseId = 0;
  GpuSkinConsumerWindowState consumerState =
      GpuSkinConsumerWindowState::Open;
};

// One instance is handed to the D3D9 host before native/reimplemented flush
// traversal. The host records every copy and compute batch inside one private
// EmitCs call. No command in this value may be dispatched per draw.
struct GpuSkinPendingBatch {
  uint64_t batchId = 0;
  uint64_t renderThreadId = 0;
  FlushRequest request;
  NativeFlushObservation flush;
  GpuSkinBatchState state = GpuSkinBatchState::Pending;
  std::vector<GpuSkinStaticUpload> staticUploads;
  std::vector<GpuSkinInputCopy> inputCopies;
  std::vector<GpuSkinComputeBatch> computeBatches;
  std::vector<OutputLease> outputLeases;
  std::vector<OutputLease> inputStorageLeases;
  std::vector<std::shared_ptr<GpuSkinInputLeaseReceipt>> inputReceipts;
  std::vector<GpuSkinPreparedDrawInfo> preparedDraws;

  // 在同步 host callback 期间保持 allocator 账本与全部 buffer arena 存活。
  // 再结合 GpuSkinHostSubmitResult 的 fence，epoch 切换后即可安全退休已记录批次。
  std::shared_ptr<War3GpuSkinResources> resourceOwner;
  Rc<DxvkFence> producerRetireFence;
  uint64_t producerRetireValue = 0;

  // True when static staging or a transient upload page must be retired after
  // the host records the command. The host must signal the returned fence
  // after every copy and compute command in this batch.
  bool requiresUploadRetirement = false;
};

static_assert(std::is_same_v<
    decltype(GpuSkinPendingBatch::resourceOwner),
    std::shared_ptr<War3GpuSkinResources>>);
static_assert(!std::is_pointer_v<
    decltype(GpuSkinPendingBatch::resourceOwner)>);
static_assert(std::is_same_v<
    decltype(GpuSkinPendingBatch::producerRetireFence), Rc<DxvkFence>>);
static_assert(std::is_copy_constructible_v<GpuSkinPendingBatch>);

struct GpuSkinHostSubmitResult {
  // 只有一个私有 EmitCs 已捕获完整 batch，并把静态复制排在全部
  // computeBatches 之前时，accepted 才能为 true。只要存在静态 staging
  // 或瞬态 upload，该 CS 就必须在最后 signal uploadRetireFence/value。
  bool accepted = false;
  Rc<DxvkFence> uploadRetireFence;
  uint64_t uploadRetireValue = 0;
};

struct GpuSkinNativeBypassHostRequest {
  GpuSkinPreparedDrawKey key;
  OutputLease lease;
  // VS-B1 使用 input lease 作为真正的 draw capability；旧 compute/P4
  // 路线保持空值，仍以 post-skin output lease 为权限对象。
  GpuSkinInputLease inputLease;
  GpuSkinExecutionRoute executionRoute = GpuSkinExecutionRoute::Compute;
  DipSignature expectedDipSignature;
  NativeEpochKey epoch;
  uint64_t fuseKey = 0;
  uintptr_t gxDeviceD3d = 0;
  uintptr_t nativeD3DDevice = 0;
  uintptr_t nativeVertexBuffer = 0;
  uintptr_t nativeIndexBuffer = 0;
  uintptr_t mappedDst = 0;
  uint32_t expectedIndexCount = 0;
  uint32_t predictedStartIndex = 0;
  uint32_t requiredConsumerBits = 0;
  uint32_t vertexCount = 0;
  uint32_t outputStride = 0;
  uint32_t fvf = 0;
  uint32_t ringBaseVertexBefore = 0;
  uint32_t ringNextVertexBefore = 0;
  uint32_t ringBaseVertexAfter = 0;
  uint32_t ringNextVertexAfter = 0;
  uint32_t indexRingBaseBefore = 0;
  uint32_t indexRingNextBefore = 0;
  uint32_t predictedIndexRingBase = 0;
  uint32_t predictedIndexRingNext = 0;
};

struct GpuSkinBypassDrawResult {
  GpuSkinPreparedDrawKey key;
  uint64_t fuseKey = 0;
  bool mainConsumed = false;
  bool shadowLeaseAvailable = false;
  bool outlineLeaseAvailable = false;
  bool streamRestoreArmed = false;
};

// Host callbacks are synchronous and must not throw. queryFlushRequest must
// supply authoritative frame/map/device epochs; the manager never invents
// them from Game.dll state.
struct GpuSkinManagerHostCallbacks {
  void* userData = nullptr;
  bool (*queryFlushRequest)(void*, const NativeFlushObservation&,
                            FlushRequest*) = nullptr;
  GpuSkinHostSubmitResult (*submitFlushBatch)(
      void*, const GpuSkinPendingBatch&) = nullptr;
  bool (*preflightNativeBypass)(
      void*, const GpuSkinNativeBypassHostRequest&,
      NativeVertexOutputProof*) = nullptr;
  // Normal-return CPU path only: prove the still-active native VB Lock before
  // Game.dll reaches Unlock. This callback never plans or consumes GPU leases.
  bool (*resolveNativeCpuRewriteOutputProof)(
      void*, const NativeUploadObservation&,
      NativeCpuRewriteOutputProof*) = nullptr;
};

// Process-lifetime raw QPC aggregates.  The producer emits calls/ticks/max
// unchanged so an external report can take two snapshots and convert the
// delta with hotPathTimingFrequency without putting floating-point work or
// per-event logging on the render lane.
struct GpuSkinRawTimingDiagnostics {
  uint64_t calls = 0;
  uint64_t ticks = 0;
  uint64_t maxTicks = 0;
};

enum class GpuSkinManagerNativeThunkKind : uint8_t {
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

inline constexpr size_t kGpuSkinManagerNativeThunkKindCount =
    static_cast<size_t>(GpuSkinManagerNativeThunkKind::Count);
static_assert(kGpuSkinManagerNativeThunkKindCount == 8u);

// Production-light 1/256 samples only. Enter includes the existing callback
// admission lock, body covers the admitted manager callback (and any caught
// exception settlement), and leave includes the existing callback-exit lock.
// A rejected enter has no body/leave sample, so the exact call closure is:
//   enter.calls == body.calls + enterRejected
//   body.calls == leave.calls
struct GpuSkinManagerNativeThunkTimingDiagnostics {
  GpuSkinRawTimingDiagnostics enter;
  GpuSkinRawTimingDiagnostics body;
  GpuSkinRawTimingDiagnostics leave;
  uint64_t enterRejected = 0;
};

struct GpuSkinManagerDiagnostics {
  GpuSkinMode mode = GpuSkinMode::Disabled;
  GpuSkinExecutionRoute executionRoute = GpuSkinExecutionRoute::Compute;
  bool executionRouteExplicit = false;
  bool executionRouteInvalid = false;
  uint64_t flushCallbacks = 0;
  uint64_t dispatchBegins = 0;
  uint64_t dispatchEnds = 0;
  uint64_t dispatchCpuOnlySealViewPublishes = 0;
  uint64_t dispatchCpuOnlySealViewQueries = 0;
  uint64_t dispatchCpuOnlySealAuthorityRejects = 0;
  uint64_t dispatchCpuOnlySealCandidateRejects = 0;
  uint64_t dispatchCpuOnlySealProposals = 0;
  uint64_t nativeUploads = 0;
  // Synchronous bridge callbacks with observation.correlated=true only.
  uint64_t nativeDips = 0;
  uint64_t nativeEligibleUploads = 0;
  uint64_t modeMismatches = 0;
  uint64_t bypassPreflightAttempts = 0;
  uint64_t bypassAuthorizations = 0;
  uint64_t bypassHostAuthorizationMismatches = 0;
  uint64_t cpuRewriteProofAttempts = 0;
  uint64_t cpuRewriteProofExact = 0;
  uint64_t cpuRewriteProofRejects = 0;
  uint64_t bypassCommits = 0;
  uint64_t bypassFallbacks = 0;
  uint64_t bypassMismatches = 0;
  uint64_t bypassStale = 0;
  uint64_t bypassPending = 0;
  uint64_t bypassRestoreFailures = 0;
  uint64_t kernelPreflightRejects = 0;
  uint64_t postSkipMismatches = 0;
  uint64_t bypassFuses = 0;
  uint64_t mainSuppressed = 0;
  uint64_t shadowSuppressed = 0;
  uint64_t outlineSuppressed = 0;
  uint64_t consumerFuses = 0;
  uint64_t irreversibleConsumerTerminations = 0;
  uint64_t irreversibleConsumerCleanupNoops = 0;
  uint64_t latePoisonSuppressions = 0;
  uint64_t latePoisonMatchedOpen = 0;
  uint64_t latePoisonMatchedTerminal = 0;
  uint64_t latePoisonUnmatched = 0;
  uint64_t latePoisonCorrelated = 0;
  uint64_t latePoisonUncorrelated = 0;
  uint64_t latePoisonBypassMatchedOpen = 0;
  uint64_t latePoisonBypassMatchedTerminal = 0;
  uint64_t latePoisonBypassUnmatched = 0;
  uint64_t latePoisonPoisonOnlyMatchedOpen = 0;
  uint64_t latePoisonPoisonOnlyMatchedTerminal = 0;
  uint64_t latePoisonPoisonOnlyUnmatched = 0;
  uint64_t latePoisonExact = 0;
  uint64_t latePoisonInexact = 0;
  uint64_t latePoisonUploadEpochZero = 0;
  uint64_t latePoisonZeroGeometry = 0;
  uint64_t latePoisonFlagNone = 0;
  uint64_t latePoisonFlagDebugSkip = 0;
  uint64_t latePoisonFlagAutoInstancing = 0;
  uint64_t latePoisonFlagIndexSplit = 0;
  uint64_t latePoisonFlagRecursive = 0;
  uint64_t latePoisonFlagNonMainPass = 0;
  uint64_t latePoisonMaskNone = 0;
  uint64_t latePoisonMaskDebug = 0;
  uint64_t latePoisonMaskAutoInstancing = 0;
  uint64_t latePoisonMaskIndexSplit = 0;
  uint64_t latePoisonMaskRecursive = 0;
  uint64_t latePoisonMaskDebugRecursive = 0;
  uint64_t latePoisonMaskNonMainPass = 0;
  uint64_t latePoisonMaskDebugNonMainPass = 0;
  uint64_t latePoisonMaskRecursiveNonMainPass = 0;
  uint64_t latePoisonMaskDebugRecursiveNonMainPass = 0;
  uint64_t latePoisonMaskUnknown = 0;
  uint64_t pendingBypassAuthorizations = 0;

  uint64_t batchesPrepared = 0;
  uint64_t batchesSubmitted = 0;
  uint64_t batchesRejected = 0;
  uint64_t jobsPrepared = 0;
  uint64_t jobsSubmitted = 0;
  uint64_t computeDispatchesPrepared = 0;
  uint64_t computeDispatchesSubmitted = 0;
  uint64_t maxJobsPerDispatch = 0;
  uint64_t computeVerticesPrepared = 0;
  uint64_t computeVerticesSubmitted = 0;
  uint64_t computeRoundedInvocationsPrepared = 0;
  uint64_t computeRoundedInvocationsSubmitted = 0;
  uint64_t computeLaunchedInvocationsPrepared = 0;
  uint64_t computeLaunchedInvocationsSubmitted = 0;
  // VS-A 输入只在显式合法路线中建立；默认 Compute 路线必须始终保持为零。
  uint64_t vsInputLeasesPrepared = 0;
  uint64_t vsInputBytesPrepared = 0;
  uint64_t vsInputCopiesSubmitted = 0;
  uint64_t vsInputBytesSubmitted = 0;
  // VS-S1 要求一份输入租约同时覆盖 Main 与 Shadow；这里把掩码和值域
  // 闭合独立暴露，避免只凭环境变量或提交数量推断消费者权限。
  uint32_t vsInputExpectedConsumerMask = 0;
  uint64_t vsInputConsumerExactLeases = 0;
  uint64_t vsInputConsumerMismatches = 0;
  // VS-B0 只移除 compute output/job，CPU kernel 与 P4 authority 仍保留。
  uint64_t vsInputOnlyLeasesPrepared = 0;
  uint64_t vsInputOnlyBytesPrepared = 0;
  uint64_t vsInputOnlyComputeJobsSkipped = 0;
  uint64_t vsInputOnlyOutputBytesSkipped = 0;
  uint64_t vsInputOnlyMainResolves = 0;
  uint64_t vsInputOnlyConsumerRejects = 0;
  // Raw high-resolution-clock diagnostics. Convert counter deltas with
  // hotPathTimingFrequency; never divide process-lifetime totals by a short
  // perf-report frame count.
  uint64_t hotPathTimingFrequency = 0;
  uint64_t flushAssemblyCalls = 0;
  uint64_t flushAssemblyTicks = 0;
  uint64_t flushAssemblyMaxTicks = 0;
  uint64_t flushHostSubmitCalls = 0;
  uint64_t flushHostSubmitTicks = 0;
  uint64_t flushHostSubmitMaxTicks = 0;
  uint64_t prepareArrayCalls = 0;
  uint64_t prepareArrayTicks = 0;
  uint64_t prepareArrayMaxTicks = 0;
  // Inclusive/child relationships are intentional:
  //   flushAssembly contains control/static/prepare/collision/assemble/publish
  //   prepareArray contains queueScan
  //   candidatePositive contains binding/staticLookup/paletteCopy/build
  //   assemble contains outputLease/finalizeCompute
  //   finalizeCompute contains batchUpload
  // These closures make timer overhead and otherwise-unclassified residuals
  // visible instead of inviting callers to add overlapping stage totals.
  GpuSkinRawTimingDiagnostics flushControlTiming;
  GpuSkinRawTimingDiagnostics flushQueryTiming;
  GpuSkinRawTimingDiagnostics flushHostFinalizeTiming;
  GpuSkinRawTimingDiagnostics staticPrepareTiming;
  GpuSkinRawTimingDiagnostics queueScanTiming;
  GpuSkinRawTimingDiagnostics transparentCollisionTiming;
  GpuSkinRawTimingDiagnostics candidatePositiveTiming;
  GpuSkinRawTimingDiagnostics liveBindingTiming;
  GpuSkinRawTimingDiagnostics staticLookupTiming;
  GpuSkinRawTimingDiagnostics paletteCopyTiming;
  GpuSkinRawTimingDiagnostics candidateBuildTiming;
  GpuSkinRawTimingDiagnostics assembleTiming;
  GpuSkinRawTimingDiagnostics outputLeaseTiming;
  GpuSkinRawTimingDiagnostics finalizeComputeTiming;
  GpuSkinRawTimingDiagnostics batchUploadTiming;
  GpuSkinRawTimingDiagnostics publishTiming;
  // P4 proof stages are timed only for actual manager callbacks.  The native
  // bridge's inclusive evaluate/complete/notify timers remain the authority
  // for lock wait and bridge-only residuals.
  GpuSkinRawTimingDiagnostics preflightManagerTiming;
  GpuSkinRawTimingDiagnostics preflightHostTiming;
  GpuSkinRawTimingDiagnostics preflightFinalizeTiming;
  GpuSkinRawTimingDiagnostics validatePaletteTiming;
  GpuSkinRawTimingDiagnostics validateStaticTiming;
  GpuSkinRawTimingDiagnostics cpuProofManagerTiming;
  GpuSkinRawTimingDiagnostics cpuProofHostTiming;
  GpuSkinRawTimingDiagnostics cpuProofFinalizeTiming;
  GpuSkinRawTimingDiagnostics completionManagerTiming;
  // DIP-wide timing already exists in NativeBridgeCounters.  Manager resolve
  // timing starts only after an exact prepared-token hit, avoiding a QPC pair
  // on every unrelated native DIP while still exposing takeover work.
  GpuSkinRawTimingDiagnostics resolvePositiveTiming;
  GpuSkinRawTimingDiagnostics shadowResolvePositiveTiming;
  GpuSkinRawTimingDiagnostics planConsumerTiming;
  GpuSkinRawTimingDiagnostics commitConsumerTiming;
  GpuSkinRawTimingDiagnostics failConsumerTiming;
  GpuSkinRawTimingDiagnostics closeConsumerTiming;
  GpuSkinRawTimingDiagnostics bypassDrawResultTiming;
  GpuSkinRawTimingDiagnostics irreversibleFuseTiming;
  GpuSkinRawTimingDiagnostics terminateConsumerTiming;
  uint64_t flushElementsVisited = 0;
  uint64_t renderableReverseHits = 0;
  uint64_t renderableReverseMisses = 0;
  uint64_t renderableBloomRejects = 0;
  uint64_t renderableBloomMaybes = 0;
  uint64_t bypassStaticHintHits = 0;
  uint64_t bypassStaticHintMisses = 0;
  uint64_t liveBindingReads = 0;
  uint64_t liveBindingReadFailures = 0;
  uint64_t renderableReverseEntries = 0;
  uint64_t renderableReverseHighWater = 0;
  uint64_t productionCpuPreferredSmallJobs = 0;
  std::array<uint64_t, kGpuSkinVertexBucketCount> vertexBucketJobs = {};
  uint64_t paletteDedupCandidates = 0;
  uint64_t paletteDedupUnique = 0;
  uint64_t paletteDedupHits = 0;
  uint64_t paletteDedupBytesSaved = 0;
  uint64_t dipLeasesConsumed = 0;
  uint64_t shadowLeasesConsumed = 0;
  uint64_t formalResolveAttempts = 0;
  uint64_t formalResolveSuccesses = 0;
  uint64_t formalResolveFailures = 0;
  uint64_t shadowResolveAttempts = 0;
  uint64_t shadowResolveSuccesses = 0;

  uint64_t classified = 0;
  uint64_t resolved = 0;
  uint64_t consumed = 0;
  uint64_t cpuFallback = 0;
  uint64_t suppressed = 0;
  uint64_t reservationLeak = 0;
  uint64_t unreserved = 0;
  uint64_t unreservedPlanLookup = 0;
  uint64_t unreservedCommitLookup = 0;
  uint64_t unreservedFailLookup = 0;
  uint64_t unreservedLookupTokenMiss = 0;
  uint64_t unreservedLookupKeyMismatch = 0;
  uint64_t unreservedLookupLeaseMismatch = 0;
  uint64_t unreservedLookupNotSubmitted = 0;
  uint64_t unreservedSettlementInvalid = 0;
  uint64_t unreservedReserveMaskInvalid = 0;
  uint64_t unreservedReserveMissingKnown = 0;
  uint64_t unreservedFuseUnknownConsumer = 0;
  uint64_t unreservedTerminateUnmatched = 0;
  uint64_t duplicate = 0;
  uint64_t planMismatch = 0;
  uint64_t retireDeferred = 0;

  // Protocol state/epoch/ordinal/count faults, never raw unmatched D3D draws.
  uint64_t truePairingErrors = 0;
  uint64_t epochLeaks = 0;
  uint64_t dispatchLeaks = 0;
  uint64_t uploadLeaks = 0;

  uint64_t paletteParityChecks = 0;
  uint64_t paletteParityMatches = 0;
  uint64_t palettePointerMismatches = 0;
  uint64_t paletteCountMismatches = 0;
  uint64_t paletteContentMismatches = 0;
  uint64_t paletteUnreadableAtUpload = 0;

  uint64_t fanoutZero = 0;
  uint64_t fanoutOne = 0;
  uint64_t fanoutMany = 0;
  uint64_t maxFanout = 0;

  uint64_t pendingDispatches = 0;
  uint64_t pendingUploads = 0;
  uint64_t pendingPreparedDraws = 0;
  uint64_t pendingSubmissions = 0;
  uint64_t callbackExceptions = 0;
  uint64_t callbackReentries = 0;
  uint64_t renderThreadMismatches = 0;
  uint64_t batchClaimFailures = 0;
  uint64_t epochTransitions = 0;
  uint64_t deviceEpochRejects = 0;
  uint64_t deviceRebindAttempts = 0;
  uint64_t deviceRebindFailures = 0;
  uint64_t deviceRebindSuccesses = 0;
  uint64_t pendingDeviceEpoch = 0;
  uint64_t pendingBridgeResetGeneration = 0;
  uint64_t callbackOwnerConflicts = 0;
  uint64_t callbackDetachDeferrals = 0;
  uint64_t callbackQuarantines = 0;
  uint64_t acceptedSubmissionRecoveries = 0;
  uint64_t acceptedSubmissionRecoveryFailures = 0;
  uint64_t epochClaimAutoRetirements = 0;
  uint64_t retirementBackpressureEntries = 0;
  uint64_t retirementBackpressureRecoveries = 0;
  uint64_t retirementBackpressureRejects = 0;
  uint64_t retiredResourceEpochs = 0;
  uint64_t retiringClaims = 0;
  uint64_t retiredResourceEpochLimit = 0;
  uint64_t retiringClaimLimit = 0;
  uint64_t retiredResourceEpochHighWater = 0;
  uint64_t retiringClaimHighWater = 0;
  uint64_t retirementLimitViolations = 0;
  bool retirementBackpressured = false;
  bool deviceReady = false;
  bool callbackQuarantined = false;
  bool activeResourceInFlight = false;
  uint64_t fallbackCount = 0;

  std::array<uint64_t, 8> formatHistogram = {};
  std::array<uint64_t, 8> skinModeHistogram = {};
  // Cumulative format flow. These counters distinguish "the map emitted an
  // odd layout" from "an odd skinned upload actually reached a compute job".
  // They are diagnostics only and never authorize a consumer takeover.
  std::array<uint64_t, 8> eligibleUploadsByFormat = {};
  std::array<uint64_t, 8> outsideUploadsByFormat = {};
  std::array<uint64_t, 8> insideUploadsByFormat = {};
  std::array<uint64_t, 8> learnedLayoutsByFormat = {};
  std::array<uint64_t, 8> preparedCandidatesByFormat = {};
  std::array<uint64_t, 8> computeJobsPreparedByFormat = {};
  std::array<
      std::array<uint64_t, kGpuSkinStrictUploadRejectReasonCount>, 8>
      strictUploadRejectByFormat = {};
  std::array<uint64_t, kGpuSkinManagerFallbackReasonCount>
      fallbackByReason = {};
  std::array<uint64_t, kGpuSkinStrictUploadRejectReasonCount>
      strictUploadRejectByReason = {};
  GpuSkinDiagnostics resources;
  // Append-only diagnostics: this does not alter NativeBridgeCallbacks or its
  // ABI. Index with GpuSkinManagerNativeThunkKind. A stableId of zero is never
  // sampled; production-light uses (stableId & 0xff) == 0xa5.
  std::array<GpuSkinManagerNativeThunkTimingDiagnostics,
             kGpuSkinManagerNativeThunkKindCount>
      productionLightNativeThunkTiming = {};
};

class War3GpuSkinManager {
public:
  War3GpuSkinManager(Rc<DxvkDevice> device, uintptr_t gameBase,
                     const GpuSkinRuntimeConfig& config,
                     const GpuSkinResourceBudgets& budgets = {},
                     const GpuSkinManagerHostCallbacks& host = {});
  ~War3GpuSkinManager();

  War3GpuSkinManager(const War3GpuSkinManager&) = delete;
  War3GpuSkinManager& operator=(const War3GpuSkinManager&) = delete;

  GpuSkinMode mode() const;
  GpuSkinExecutionRoute executionRoute() const;
  bool executionRouteExplicit() const;
  bool executionRouteInvalid() const;
  void SetHostCallbacks(const GpuSkinManagerHostCallbacks& host);
  // A changed device epoch is rejected until the D3D9 owner supplies the
  // matching device explicitly on the recorded render thread.
  bool SetDevice(Rc<DxvkDevice> device, uint64_t deviceEpoch,
                 uint64_t renderThreadId);
  bool IsDeviceReady(uint64_t deviceEpoch = 0u) const;

  // Registration is explicit because hook installation and D3D9 device
  // construction have independent lifetimes. Disabled mode registers nothing.
  bool AttachNativeBridge();
  bool DetachNativeBridge();

  // Callback form. Compute-producing modes obtain the epoch request from the
  // host. Observe records the native flush only.
  void SubmitFlush(const NativeFlushObservation& observation);

  // Direct form for an integration point that already owns authoritative
  // frame/map/device epochs. Returns true when a command is pending/submitted.
  bool SubmitFlush(const FlushRequest& request,
                   const NativeFlushObservation& observation);

  void BeginDispatch(const NativeDispatchObservation& observation);
  void EndDispatch(const NativeDispatchSummary& summary);
  void NoteNativeUpload(const NativeUploadObservation& observation);
  void NoteNativeDip(const NativeDipObservation& observation);
  void NoteNativeUploadFanout(
      const NativeUploadFanoutObservation& observation);
  void NoteBypassDrawResult(const GpuSkinBypassDrawResult& result);
  // Records an actual consumer suppression after the native upload kernel was
  // skipped. The fuse key remains authoritative even when ResolveDip could not
  // recover a prepared key; repeated notifications are idempotent.
  void FuseIrreversibleBypass(
      const GpuSkinResolvedDraw& resolved,
      GpuSkinConsumerBits consumer);
  // Atomically terminates every still-open lease-backed render consumer in an
  // irreversible bypass plan. Repeated calls never suppress a bit twice.
  void TerminateIrreversibleBypassConsumers(
      const GpuSkinResolvedDraw& resolved);

  // Formal DIP consumers are mode-gated: Shadow accepts Shadow only;
  // Main/Bypass accept Main, Shadow, and Outline. Dual must use the dedicated
  // parity entry point below. A source kernel skip remains recorded in the
  // result even when exact resolution fails, so the device can suppress it.
  GpuSkinResolvedDraw ResolveDip(
      const NativeDipObservation& observation,
      GpuSkinConsumerBits consumer = GpuSkinConsumerBits::Main);
  // Dual's exact lease is parity-only and never authorizes a render consumer.
  GpuSkinResolvedDraw ResolveParityDip(
      const NativeDipObservation& observation);
  // Available in Shadow, Main, and Bypass; never in Dual or Observe.
  GpuSkinResolvedDraw ResolveShadowLease(
      const GpuSkinPreparedDrawKey& key);

  // The P4 host calls PlanConsumers synchronously from preflightNativeBypass
  // with request.expectedDipSignature. All masks are validated partitions of
  // legal consumer bits and are immutable once accepted.
  bool PlanConsumers(const GpuSkinPreparedDrawKey& key, uint64_t leaseId,
                     const GpuSkinConsumerPlan& plan);
  bool CommitConsumer(const GpuSkinPreparedDrawKey& key, uint64_t leaseId,
                      GpuSkinConsumerBits consumer);
  bool FailConsumer(const GpuSkinPreparedDrawKey& key, uint64_t leaseId,
                    GpuSkinConsumerBits consumer,
                    GpuSkinConsumerFailure failure);
  // Closes every prepared draw in the submitted batch. A false result means
  // at least one reservation had to fail closed; no GPU wait is performed.
  bool CloseBatchConsumerWindow(uint64_t batchId, uint64_t renderThreadId);

  // submitFlushBatch is the preferred path. This fallback atomically claims
  // one batch; a second Take for the same batch always fails.
  bool TakePendingBatch(uint64_t renderThreadId,
                        GpuSkinPendingBatch& output);
  bool MarkPendingBatchSubmitted(uint64_t batchId,
                                 uint64_t renderThreadId,
                                 Rc<DxvkFence> uploadRetireFence,
                                 uint64_t uploadRetireValue);

  // Call only after CloseBatchConsumerWindow and after the host has enqueued a
  // fence signal behind every committed consumer. An open/unsettled ledger is
  // fail-closed and deferred without waiting on the GPU. True means every
  // lease was queued for retirement; false leaves ownership with the manager.
  bool RetireBatch(uint64_t batchId, uint64_t renderThreadId,
                   Rc<DxvkFence> fence, uint64_t value);

  // Test-only, non-blocking maintenance on the recorded render thread. This
  // advances already-committed retirement/recovery bookkeeping and polls
  // completed fences; it never waits for the GPU or changes admission.
  bool RefreshRetirementDiagnosticsForTest(uint64_t renderThreadId);
  GpuSkinManagerDiagnostics SnapshotDiagnostics() const;
  void Reset(uint64_t bridgeResetGeneration = 0u);

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace dxvk::war3::gpu_skin
