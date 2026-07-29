#include "war3_gpu_skin_native_bridge.h"

#include "../core/war3_memory.h"
#include "../../../util/util_time.h"

#include <windows.h>
#include <wincrypt.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <vector>

namespace dxvk::war3::gpu_skin {

std::atomic<uint8_t> g_nativeOutsidePoisonSidecarPolicyMask{0u};
std::atomic<uint32_t> g_nativeOutsidePoisonAuthorityActive{0u};
thread_local bool g_nativeGpuSkinB1BorrowedFlushPinPassThrough = false;

namespace {

static_assert(!kWholeNativeUploadBypassEnabled,
              "whole native upload bypass must remain permanently disabled");
static_assert(!GpuSkinOutsidePoisonO0Enabled(
    GpuSkinOutsidePoisonSidecarPolicy::O1));
static_assert(!GpuSkinOutsidePoisonO1Enabled(
    GpuSkinOutsidePoisonSidecarPolicy::O0));

enum class NativeOutsideDipFastRejectReason : uint8_t {
  LocalScope = 0,
  TicketGate,
  Lifecycle,
  PendingKernel,
  ResetOrRetirement,
  PoisonNeedsInput,
  PoisonCountMismatch,
  PoisonHit,
  Count,
};

enum class NativeOutsideDipFastLocalRejectReason : uint8_t {
  NoTransactionCover = 0,
  CleanSemanticOnly,
  DispatchActive,
  SemanticHazard,
  UploadOrMarker,
  CallbackReentry,
  OtherChronology,
  Count,
};

static_assert(
    size_t(NativeOutsideDipFastRejectReason::Count) ==
        kNativeOutsideDipFastRejectReasonCount,
    "outside-DIP fast reject diagnostics must match the public contract");
static_assert(
    size_t(NativeOutsideDipFastLocalRejectReason::Count) ==
        kNativeOutsideDipFastLocalRejectReasonCount,
    "outside-DIP local reject diagnostics must match the public contract");

constexpr uintptr_t kExpectedImageBase = 0x6F000000u;
constexpr uint32_t kExpectedPeOffset = 0x00000138u;
// COFF FileHeader.TimeDateStamp. 0x56BD0E1B belongs to the export directory.
constexpr uint32_t kExpectedPeTimestamp = 0x56BD0E1Cu;
constexpr uint32_t kExpectedImageSize = 0x00CD8000u;
constexpr uint32_t kExpectedImageChecksum = 0x00C96841u;
constexpr uint64_t kExpectedFileSize = 13187048ull;
constexpr uint32_t kExpectedFileVersion[4] = {1u, 27u, 0u, 52240u};

constexpr uintptr_t kFlushSortedItemsRva = 0x1380A0u;
constexpr uintptr_t kDispatchCommonRva = 0x13A5E0u;
constexpr uintptr_t kApplyDrawStateAndSamplerPairRva = 0x138EE0u;
constexpr uintptr_t kGxDeviceD3dDynamicVertexUploadRva = 0x0EEA50u;
constexpr uintptr_t kGxDeviceD3dSkinCopyKernelRva = kNativeSkinCopyKernelRva;
constexpr uintptr_t kGxDeviceD3dLockVertexRingRva = 0x0EE5D0u;
constexpr uintptr_t kGxDeviceD3dLockVertexRingCallRva = 0x0EEB76u;
constexpr uintptr_t kGxDeviceD3dSkinCopyKernelCallRva = 0x0EEB85u;
constexpr uintptr_t kGxDeviceD3dVtableRva = 0x95D87Cu;
constexpr uintptr_t kGxDeviceD3dIndexUploadRva = 0x0EEC20u;
constexpr uintptr_t kGxDeviceD3dFlushPrimitiveBatchRva = 0x0EE9F0u;
constexpr uintptr_t kRenderQueueNumOfElementsRva = 0xBC6BACu;
constexpr uintptr_t kNormalApplyReturnRva = 0x138F7Eu;
constexpr uintptr_t kMultiPassApplyReturnRva0 = 0x13A30Au;
constexpr uintptr_t kMultiPassApplyReturnRva1 = 0x13A43Eu;

constexpr size_t kGxPalettePtrOffset = 0x19Cu;
constexpr size_t kGxPaletteCountOffset = 0x1A0u;
constexpr size_t kGxSubmittedVertexCountOffset = 0x5Cu;
constexpr size_t kGxSkinModeOffset = 0x224u;
constexpr size_t kGxOutputFormatOffset = 0x228u;
static_assert(
    kGxOutputFormatOffset == kGxSkinModeOffset + sizeof(uint32_t),
    "O1a kernel state must remain one contiguous 8-byte read");
static_assert(sizeof(std::array<uint32_t, 2u>) == 8u);
constexpr size_t kGxNativeDeviceOffset = 0x584u;
constexpr size_t kGxVertexBufferArrayOffset = 0x6C0u;
constexpr size_t kGxIndexBufferOffset = 0x6D8u;
constexpr size_t kGxVertexRingBaseOffset = 0x6DCu;
constexpr size_t kGxVertexRingNextOffset = 0x6E0u;
constexpr size_t kGxIndexRingBaseOffset = 0x70Cu;
constexpr size_t kGxIndexRingNextOffset = 0x710u;
constexpr size_t kGxIndexBaseVertexOffset = 0x71Cu;

constexpr uintptr_t kFormatStrideTableRva = 0xB665CCu;
constexpr uintptr_t kFormatFvfTableRva = 0xB66CA0u;
constexpr uintptr_t kDefaultNormalRva = 0xB66CB8u;
constexpr uintptr_t kDefaultExtraRva = 0xB66CC4u;
constexpr uintptr_t kVertexGlobalsRva = 0xBC5EA0u;
constexpr uintptr_t kDefaultUvRva = 0xBC5ED4u;

constexpr size_t kGeosetVertexCountOffset = 0x0Cu;
constexpr size_t kGeosetPositionsOffset = 0x10u;
constexpr size_t kGeosetGroupSlotsOffset = 0x4Cu;
constexpr size_t kGeosetNormalsOffset = 0x58u;
constexpr size_t kGeosetPrimitiveCountOffset = 0xC8u;
constexpr size_t kGeosetPrimitiveRecordsOffset = 0xCCu;
constexpr size_t kGeosetIndexCountOffset = 0xDCu;
constexpr size_t kGeosetIndicesOffset = 0xE0u;
constexpr size_t kGeosetPaletteGroupCountOffset = 0xF0u;

constexpr size_t kGeosetStateSnapshotBeginOffset =
    kGeosetVertexCountOffset;
constexpr size_t kGeosetStateSnapshotEndOffset =
    kGeosetPaletteGroupCountOffset + sizeof(uint32_t);
constexpr size_t kGeosetStateSnapshotSize =
    kGeosetStateSnapshotEndOffset - kGeosetStateSnapshotBeginOffset;
static_assert(kGeosetStateSnapshotSize == 0xE8u);
static_assert(kGeosetPositionsOffset >= kGeosetStateSnapshotBeginOffset);
static_assert(kGeosetGroupSlotsOffset + sizeof(uint32_t) <=
              kGeosetStateSnapshotEndOffset);
static_assert(kGeosetNormalsOffset + sizeof(uint32_t) <=
              kGeosetStateSnapshotEndOffset);
static_assert(kGeosetIndicesOffset + sizeof(uint32_t) <=
              kGeosetStateSnapshotEndOffset);

constexpr uint32_t kMaxNativeVertices = 0x4000u;
constexpr uint32_t kMaxNativeIndices = 0xC000u;
constexpr uint32_t kMaxPaletteGroups = 256u;
constexpr size_t kNativePaletteGroupBytes = 48u;
constexpr size_t kNativePaletteReadabilityScratchBytes =
    kMaxPaletteGroups * kNativePaletteGroupBytes;
static_assert(kNativePaletteReadabilityScratchBytes == 0x3000u);
static_assert(kProductionGpuMinVertices != 0u &&
              kProductionGpuMinVertices <= kMaxNativeVertices);
constexpr uint32_t kWorldObjectsTag = 1u;
constexpr uint32_t kGxPrimitiveTriangleList = 3u;
constexpr uint32_t kD3dPrimitiveTriangleList = 4u;
constexpr uint32_t kD3dFormatIndex16 = 101u;
constexpr uint32_t kD3dLockNoOverwrite = 0x1000u;
constexpr uint32_t kD3dLockDiscard = 0x2000u;
// Game.dll's dynamic vertex-ring Lock calls preserve D3DLOCK_NOSYSLOCK in
// addition to the ring-mode flag (ASM 0x0EE60B / 0x0EE656). These exact values
// belong only to the O0 vertex-Lock sidecar; the established bare constants
// above retain their existing index/admission semantics.
constexpr uint32_t kNativeVertexRingLockNoOverwrite = 0x1800u;
constexpr uint32_t kNativeVertexRingLockDiscard = 0x2800u;
constexpr uint32_t kD3dResourceTypeVertexBuffer = 6u;
constexpr uint32_t kD3dPoolDefault = 0u;
constexpr uint32_t kD3dUsageDynamic = 0x200u;

constexpr std::array<uint32_t, 6> kFormatFvf = {
    0x012u, 0x052u, 0x112u, 0x152u, 0x212u, 0x252u};
constexpr std::array<uint32_t, 6> kFormatStride = {
    24u, 28u, 32u, 36u, 40u, 44u};

constexpr size_t kGxStateSnapshotBeginOffset =
    kGxSubmittedVertexCountOffset;
constexpr size_t kGxStateSnapshotEndOffset =
    kGxIndexBaseVertexOffset + sizeof(uint32_t);
constexpr size_t kGxStateSnapshotSize =
    kGxStateSnapshotEndOffset - kGxStateSnapshotBeginOffset;
static_assert(kGxStateSnapshotSize == 0x6C4u);
static_assert(kGxPalettePtrOffset >= kGxStateSnapshotBeginOffset);
static_assert(kGxOutputFormatOffset + sizeof(uint32_t) <=
              kGxStateSnapshotEndOffset);
static_assert(kGxVertexBufferArrayOffset +
                  (kFormatFvf.size() - 1u) * sizeof(uint32_t) +
                  sizeof(uint32_t) <=
              kGxStateSnapshotEndOffset);
static_assert(kGxVertexRingNextOffset +
                  (kFormatFvf.size() - 1u) * 8u + sizeof(uint32_t) <=
              kGxStateSnapshotEndOffset);

// The poison-only outside admission needs no palette, skin, submitted-count
// or index state. One bounded ReadProcessMemory-backed SafeCopy keeps the four
// overwrite-authority fields in a single fault-safe snapshot and avoids the
// much larger generic GX observation.
constexpr size_t kOutsidePoisonSnapshotBeginOffset =
    kGxOutputFormatOffset;
constexpr size_t kOutsidePoisonSnapshotEndOffset =
    kGxVertexRingNextOffset +
    (kFormatFvf.size() - 1u) * 8u + sizeof(uint32_t);
constexpr size_t kOutsidePoisonSnapshotSize =
    kOutsidePoisonSnapshotEndOffset - kOutsidePoisonSnapshotBeginOffset;
static_assert(kOutsidePoisonSnapshotBeginOffset <
              kOutsidePoisonSnapshotEndOffset);
static_assert(kGxNativeDeviceOffset >= kOutsidePoisonSnapshotBeginOffset);
static_assert(kGxVertexBufferArrayOffset >=
              kOutsidePoisonSnapshotBeginOffset);

// VS-B1 只允许 format2。毒区全部来自同一白名单时，读取 format2 所需的
// GX 状态窗口即可完成相同的覆盖证明，避免每次上传复制 0x4E4 字节快照。
constexpr uint32_t kVertexShaderBypassPoisonFormat = 2u;
constexpr size_t kVertexShaderBypassPoisonStateBegin =
    kGxNativeDeviceOffset;
constexpr size_t kVertexShaderBypassPoisonStateEnd =
    kGxVertexRingNextOffset +
    kVertexShaderBypassPoisonFormat * 8u + sizeof(uint32_t);
constexpr size_t kVertexShaderBypassPoisonStateSize =
    kVertexShaderBypassPoisonStateEnd -
    kVertexShaderBypassPoisonStateBegin;
static_assert(kVertexShaderBypassPoisonStateSize == 0x170u);
static_assert(kVertexShaderBypassPoisonStateEnd <=
              kOutsidePoisonSnapshotEndOffset);

constexpr std::array<uint8_t, 16> kExpectedMd5 = {
    0x26, 0x78, 0x61, 0xA0, 0xDF, 0xD4, 0x16, 0xDB,
    0xAD, 0x13, 0xE7, 0xEE, 0x3E, 0xC7, 0x79, 0x4A};
constexpr std::array<uint8_t, 20> kExpectedSha1 = {
    0x88, 0xAB, 0x43, 0x21, 0x60, 0xFB, 0x84, 0xC2, 0x3B, 0x09,
    0x6C, 0x3F, 0xC0, 0x22, 0xBF, 0xBC, 0xB3, 0xCB, 0x1A, 0x1A};

constexpr size_t kMaxDispatchDepth = 8u;
constexpr size_t kMaxSemanticDepth = 8u;
constexpr size_t kMaxNestedUploadDepth = 8u;
constexpr size_t kMaxNativePoisonRanges = 128u;
constexpr size_t kMaxNativeRetirementEvents = 256u;
constexpr size_t kMaxOutsidePoisonShadowDepth = 8u;
constexpr uint64_t kOutsidePoisonShadowOverflowCookie =
    std::numeric_limits<uint64_t>::max() - 4u;
constexpr uint64_t kDispatchOverflowCookie =
    std::numeric_limits<uint64_t>::max();
constexpr uint64_t kSemanticOverflowCookie =
    std::numeric_limits<uint64_t>::max() - 1u;
constexpr uint64_t kNestedUploadCookie =
    std::numeric_limits<uint64_t>::max() - 2u;
constexpr uint64_t kProductionFastRejectUploadCookie =
    std::numeric_limits<uint64_t>::max() - 3u;

GpuSkinRuntimeConfig g_runtimeConfig;
NativeBridgeFingerprint g_fingerprint;
uintptr_t g_gameBase = 0;
std::once_flag g_initializeOnce;
std::atomic<bool> g_installEligible{false};
std::atomic<bool> g_hooksEnabled{false};
std::atomic<bool> g_transactionIngressEnabled{false};
std::atomic<bool> g_callbackIngressEnabled{false};
std::atomic<bool> g_dipObserverIngressEnabled{false};
std::atomic<bool> g_bypassEnabled{false};

bool OutsidePoisonO0SidecarEnabled() noexcept {
  return GpuSkinOutsidePoisonO0Enabled(
      g_runtimeConfig.outsidePoisonSidecarPolicy);
}

bool OutsidePoisonO1SidecarEnabled() noexcept {
  return GpuSkinOutsidePoisonO1Enabled(
      g_runtimeConfig.outsidePoisonSidecarPolicy);
}

bool OutsidePoisonAnySidecarEnabled() noexcept {
  return OutsidePoisonO0SidecarEnabled() ||
      OutsidePoisonO1SidecarEnabled();
}
std::atomic<const NativeBridgeCallbacks*> g_callbacks{nullptr};
std::atomic<uint32_t> g_activeCallbackPins{0u};
std::atomic<uint32_t> g_activeFlushTransactions{0u};
std::atomic<uint32_t> g_activeDispatchTransactions{0u};
std::atomic<uint32_t> g_activeSemanticTransactions{0u};
std::atomic<uint32_t> g_activeUploadTransactions{0u};
std::atomic<uint32_t> g_activeDipObserverTransactions{0u};
// Lightweight strict-outside readers are a distinct ownership domain. They
// intentionally do not borrow the full observer counter: a corrupt observer
// must never consume another cover's published activity and appear clean.
std::atomic<uint32_t> g_activeOutsideDipReaderTransactions{0u};
std::mutex g_callbackRegistrationMutex;
const void* g_callbackOwner = nullptr;
const NativeBridgeCallbacks* g_retiringCallbacks = nullptr;
std::atomic<bool> g_callbackOwnerRegistered{false};
std::atomic<uint64_t> g_nextFlushEpoch{0};
std::atomic<uint64_t> g_nextDispatchEpoch{0};
std::atomic<uint64_t> g_nextUploadEpoch{0};
std::atomic<uint64_t> g_nextSemanticCookie{0};
std::atomic<uint64_t> g_nextIndexTicketGeneration{0};
std::atomic<uint64_t> g_observedRenderThreadId{0u};
std::atomic<uint64_t> g_resetRequestedGeneration{0u};
std::atomic<uint64_t> g_resetCompletedGeneration{0u};
std::atomic<uint64_t> g_ownerRetiredGeneration{0u};
std::atomic<uint32_t> g_resetReason{
    static_cast<uint32_t>(NativePoisonLedgerResetReason::Explicit)};
std::atomic<bool> g_resetOwnerAckRequired{false};
std::atomic<uint64_t> g_nativePoisonOutstandingRanges{0u};
SRWLOCK g_resetProtocolLock = SRWLOCK_INIT;
uint64_t g_nextResetGeneration = 0u;

class ResetProtocolLockGuard {
public:
  ResetProtocolLockGuard() noexcept {
    ::AcquireSRWLockExclusive(&g_resetProtocolLock);
  }

  ~ResetProtocolLockGuard() noexcept {
    ::ReleaseSRWLockExclusive(&g_resetProtocolLock);
  }

  ResetProtocolLockGuard(const ResetProtocolLockGuard&) = delete;
  ResetProtocolLockGuard& operator=(const ResetProtocolLockGuard&) = delete;
};

struct NativeResourceRetirementEvent {
  uintptr_t commonResource = 0u;
  uint64_t resourceGeneration = 0u;
};

SRWLOCK g_retirementQueueLock = SRWLOCK_INIT;
std::array<NativeResourceRetirementEvent, kMaxNativeRetirementEvents>
    g_retirementEvents{};
size_t g_retirementEventHead = 0u;
size_t g_retirementEventCount = 0u;
std::atomic<uint64_t> g_retirementEventsPending{0u};
std::atomic<bool> g_retirementQueueFaulted{false};
// Exactly one observed render thread may own an unpublished production-light
// telemetry delta. A release-published pending bit lets off-thread relaxed
// snapshots detect that their aggregate counters are intentionally incomplete.
std::atomic<uint32_t> g_nativeTelemetryDeltaPending{0u};
std::atomic<bool> g_nativeTelemetryDeltaFaulted{false};
// Sampled timing uses direct atomics, but one logical sample can publish
// several triplets. Started/completed generations let a snapshot reject any
// split batch without forcing these report-only counters into authorization.
std::atomic<uint64_t> g_productionTimingWritesStarted{0u};
std::atomic<uint64_t> g_productionTimingWritesCompleted{0u};
std::atomic<uint32_t> g_productionTimingWriters{0u};

struct AtomicNativeSampledRawTiming {
  std::atomic<uint64_t> calls{0u};
  std::atomic<uint64_t> ticks{0u};
  std::atomic<uint64_t> maxTicks{0u};
};

struct AtomicNativeBridgeCounters {
  std::atomic<uint64_t> beginUploadTimingCalls{0};
  std::atomic<uint64_t> beginUploadTimingTicks{0};
  std::atomic<uint64_t> beginUploadTimingMaxTicks{0};
  std::atomic<uint64_t> beginSampleCommonCalls{0};
  std::atomic<uint64_t> beginSampleCommonTicks{0};
  std::atomic<uint64_t> beginSampleCommonMaxTicks{0};
  std::atomic<uint64_t> beginSampleStateCalls{0};
  std::atomic<uint64_t> beginSampleStateTicks{0};
  std::atomic<uint64_t> beginSampleStateMaxTicks{0};
  std::atomic<uint64_t> beginSampleExactCalls{0};
  std::atomic<uint64_t> beginSampleExactTicks{0};
  std::atomic<uint64_t> beginSampleExactMaxTicks{0};
  std::atomic<uint64_t> beginSampleScopeRouteCalls{0};
  std::atomic<uint64_t> beginSampleScopeRouteTicks{0};
  std::atomic<uint64_t> beginSampleScopeRouteMaxTicks{0};
  std::atomic<uint64_t> beginSampleStateRejectRouteCalls{0};
  std::atomic<uint64_t> beginSampleStateRejectRouteTicks{0};
  std::atomic<uint64_t> beginSampleStateRejectRouteMaxTicks{0};
  std::atomic<uint64_t> beginSampleSkinRouteCalls{0};
  std::atomic<uint64_t> beginSampleSkinRouteTicks{0};
  std::atomic<uint64_t> beginSampleSkinRouteMaxTicks{0};
  std::atomic<uint64_t> beginSampleSmallRouteCalls{0};
  std::atomic<uint64_t> beginSampleSmallRouteTicks{0};
  std::atomic<uint64_t> beginSampleSmallRouteMaxTicks{0};
  std::atomic<uint64_t> beginSampleCandidateRouteCalls{0};
  std::atomic<uint64_t> beginSampleCandidateRouteTicks{0};
  std::atomic<uint64_t> beginSampleCandidateRouteMaxTicks{0};
  std::atomic<uint64_t> t2SampleGeoSnapCalls{0};
  std::atomic<uint64_t> t2SampleGeoSnapTicks{0};
  std::atomic<uint64_t> t2SampleGeoSnapMaxTicks{0};
  std::atomic<uint64_t> t2SampleGeoHeaderCalls{0};
  std::atomic<uint64_t> t2SampleGeoHeaderTicks{0};
  std::atomic<uint64_t> t2SampleGeoHeaderMaxTicks{0};
  std::atomic<uint64_t> t2SamplePositionProofCalls{0};
  std::atomic<uint64_t> t2SamplePositionProofTicks{0};
  std::atomic<uint64_t> t2SamplePositionProofMaxTicks{0};
  std::atomic<uint64_t> t2SampleNormalProofCalls{0};
  std::atomic<uint64_t> t2SampleNormalProofTicks{0};
  std::atomic<uint64_t> t2SampleNormalProofMaxTicks{0};
  std::atomic<uint64_t> t2SampleGroupProofCalls{0};
  std::atomic<uint64_t> t2SampleGroupProofTicks{0};
  std::atomic<uint64_t> t2SampleGroupProofMaxTicks{0};
  std::atomic<uint64_t> t2SamplePaletteProofCalls{0};
  std::atomic<uint64_t> t2SamplePaletteProofTicks{0};
  std::atomic<uint64_t> t2SamplePaletteProofMaxTicks{0};
  std::atomic<uint64_t> evaluateKernelTimingCalls{0};
  std::atomic<uint64_t> evaluateKernelTimingTicks{0};
  std::atomic<uint64_t> evaluateKernelTimingMaxTicks{0};
  std::atomic<uint64_t> completeUploadTimingCalls{0};
  std::atomic<uint64_t> completeUploadTimingTicks{0};
  std::atomic<uint64_t> completeUploadTimingMaxTicks{0};
  std::atomic<uint64_t> notifyNormalTimingCalls{0};
  std::atomic<uint64_t> notifyNormalTimingTicks{0};
  std::atomic<uint64_t> notifyNormalTimingMaxTicks{0};
  std::atomic<uint64_t> dipTimingCalls{0};
  std::atomic<uint64_t> dipTimingTicks{0};
  std::atomic<uint64_t> dipTimingMaxTicks{0};
  std::atomic<uint64_t> outerUploadTimingCalls{0};
  std::atomic<uint64_t> outerUploadTimingTicks{0};
  std::atomic<uint64_t> outerUploadTimingMaxTicks{0};
  std::atomic<uint64_t> originalKernelTimingCalls{0};
  std::atomic<uint64_t> originalKernelTimingTicks{0};
  std::atomic<uint64_t> originalKernelTimingMaxTicks{0};
  std::atomic<uint64_t> flushNotifications{0};
  std::atomic<uint64_t> dispatchScopes{0};
  std::atomic<uint64_t> dispatchScopeEnds{0};
  std::atomic<uint64_t> commonDispatchScopes{0};
  std::atomic<uint64_t> specialDispatchScopes{0};
  std::atomic<uint64_t> managerDispatchEagerScopes{0};
  std::atomic<uint64_t> managerDispatchLazyScopes{0};
  std::atomic<uint64_t> managerDispatchNeverScopes{0};
  std::atomic<uint64_t> managerDispatchEvidenceEagerScopes{0};
  std::atomic<uint64_t> managerDispatchBeginCallbacks{0};
  std::atomic<uint64_t> managerDispatchEndCallbacks{0};
  std::atomic<uint64_t> managerDispatchEagerBegins{0};
  std::atomic<uint64_t> managerDispatchLazyAdmissionAttempts{0};
  std::atomic<uint64_t> managerDispatchLazyAdmissions{0};
  std::atomic<uint64_t> managerDispatchEagerAdmissionFailures{0};
  std::atomic<uint64_t> managerDispatchLazyAdmissionFailures{0};
  std::atomic<uint64_t> managerDispatchNeverSafetyFailures{0};
  std::atomic<uint64_t> managerDispatchIssuedEnds{0};
  std::atomic<uint64_t> managerDispatchNoUploadEnds{0};
  std::atomic<uint64_t> managerDispatchNeverEnds{0};
  std::atomic<uint64_t> managerDispatchFailedEnds{0};
  std::atomic<uint64_t> managerDispatchSkippedUploads{0};
  std::atomic<uint64_t> managerDispatchSkippedDips{0};
  std::atomic<uint64_t> managerDispatchSkippedFanouts{0};
  std::atomic<uint64_t> semanticScopes{0};
  std::atomic<uint64_t> uploads{0};
  std::atomic<uint64_t> uploadsOutsideDispatch{0};
  std::atomic<uint64_t> nativeEligibleUploads{0};
  std::atomic<uint64_t> productionFastRejectScope{0};
  std::atomic<uint64_t> productionFastRejectState{0};
  std::atomic<uint64_t> productionFastRejectSkinFormat{0};
  std::atomic<uint64_t> productionFastRejectInput{0};
  std::atomic<uint64_t> productionFastRejectSmall{0};
  std::atomic<uint64_t> productionFastRejectUnknownState{0};
  std::atomic<uint64_t> productionCandidates{0};
  std::atomic<uint64_t> productionPoisonRetirementOnly{0};
  std::atomic<uint64_t> productionOutsideCallbacksSkipped{0};
  std::atomic<uint64_t> productionOutsideNativeFastPath{0};
  std::atomic<uint64_t> productionOutsideNoPoisonDirectAttempts{0};
  std::atomic<uint64_t> productionOutsideNoPoisonDirectKernelCalls{0};
  std::atomic<uint64_t> productionOutsideNoPoisonDirectNormalReturns{0};
  std::atomic<uint64_t>
      productionOutsideNoPoisonDirectKernelNoNormalReturns{0};
  std::atomic<uint64_t> productionOutsideNoPoisonDirectCompleted{0};
  std::atomic<uint64_t> productionOutsideNoPoisonDirectConflicts{0};
  std::atomic<uint64_t> productionOutsideNoPoisonDirectCancellations{0};
  std::atomic<uint64_t> productionOutsideNoPoisonDirectActive{0};
  std::atomic<uint64_t>
      productionOutsideNoPoisonDirectResetCompletedWhileActive{0};
  std::atomic<uint64_t> productionOutsideNoPoisonDirectLatePoison{0};
  std::atomic<uint64_t> productionOutsidePoisonScanAttempts{0};
  std::atomic<uint64_t> productionOutsidePoisonNoOverlapAdmissions{0};
  std::atomic<uint64_t> productionOutsidePoisonOverlapRejects{0};
  std::atomic<uint64_t> productionOutsidePoisonReadFailRejects{0};
  std::atomic<uint64_t> productionOutsidePoisonShadowAttempts{0};
  std::atomic<uint64_t> productionOutsidePoisonShadowCreated{0};
  std::atomic<uint64_t> productionOutsidePoisonShadowOverflow{0};
  std::atomic<uint64_t> productionOutsidePoisonShadowActive{0};
  std::atomic<uint64_t> productionOutsidePoisonShadowLockNotifications{0};
  std::atomic<uint64_t> productionOutsidePoisonShadowSettled{0};
  std::atomic<uint64_t> productionOutsidePoisonShadowCancelled{0};
  std::atomic<uint64_t> productionOutsidePoisonShadowResetAborted{0};
  std::atomic<uint64_t> productionOutsidePoisonShadowComparable{0};
  std::atomic<uint64_t> productionOutsidePoisonShadowUnprovable{0};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonShadowMatrixSize>
      productionOutsidePoisonShadowMatrix{};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonShadowUnprovableReasonCount>
      productionOutsidePoisonShadowUnprovableReasons{};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowAttempts{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowCreated{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowOverflow{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowActive{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowLockNotifications{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowKernelNotifications{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowUnlockNotifications{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowFrozen{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowSettled{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowCancelled{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowResetAborted{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowComparable{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowUnprovable{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowComparisonMissing{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowWouldClear{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowOldOverlapFrozen{0};
  std::atomic<uint64_t>
      productionOutsidePoisonO1ShadowDirectDiscardNotifications{0};
  std::atomic<uint64_t>
      productionOutsidePoisonO1ShadowDirectDiscardOldOverlapRetired{0};
  std::atomic<uint64_t>
      productionOutsidePoisonO1ShadowOldOverlapToNoOverlapDiscardExact{0};
  std::atomic<uint64_t>
      productionOutsidePoisonO1ShadowOldOverlapToNoOverlapOther{0};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonO1LockLaneCount>
      productionOutsidePoisonO1ShadowScanCallsByLane{};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonO1LockLaneCount>
      productionOutsidePoisonO1ShadowScanNoOverlapByLane{};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonO1LockLaneCount>
      productionOutsidePoisonO1ShadowScanOverlapByLane{};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonO1LockLaneCount>
      productionOutsidePoisonO1ShadowScanReadFailureByLane{};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonO1LockLaneCount>
      productionOutsidePoisonO1ShadowPreLockMutationByLane{};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonO1LockLaneCount>
      productionOutsidePoisonO1ShadowLockToKernelMutationByLane{};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonO1LaneFailureCount>
      productionOutsidePoisonO1ShadowScanFailuresByLane{};
  std::atomic<uint64_t>
      productionOutsidePoisonO1ShadowScanDifferentTarget{0};
  std::atomic<uint64_t>
      productionOutsidePoisonO1ShadowStorageDiagnosticMismatch{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowRealStorageDrift{0};
  std::atomic<uint64_t> productionOutsidePoisonO1ShadowMappingStorageDrift{0};
  std::atomic<uint64_t>
      productionOutsidePoisonO1ShadowMapAllocationDiagnosticMismatch{0};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonO1UnlockDriftComponentCount>
      productionOutsidePoisonO1ShadowUnlockDrifts{};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonO1UnlockDriftComponentCount>
      productionOutsidePoisonO1ShadowUnlockHardFirstCauses{};
  std::array<std::atomic<uint64_t>, 3u>
      productionOutsidePoisonO1ShadowPhysicalVerdicts{};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonO1ShadowMatrixSize>
      productionOutsidePoisonO1ShadowMatrix{};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonO1ShadowUnprovableReasonCount>
      productionOutsidePoisonO1ShadowUnprovableReasons{};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityAttempts{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityCreated{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityOverflow{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityArmed{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthoritySettled{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityCancelled{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityResetAborted{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityLockNotifications{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityLockNoOverlap{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityLockOverlap{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityLockRejects{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityKernelReady{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityKernelRejects{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityNormalReturns{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityUnlockNotifications{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityUnlockExact{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityUnlockRejects{0};
  std::atomic<uint64_t>
      productionOutsidePoisonAuthorityCommittedNoOverlap{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityCommittedRewrite{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityRetained{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityPoisonClears{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthority{0};
  std::atomic<uint64_t> productionOutsidePoisonAuthorityEvidenceAttempts{0};
  std::atomic<uint64_t>
      productionOutsidePoisonAuthorityEvidenceComparable{0};
  std::atomic<uint64_t>
      productionOutsidePoisonAuthorityEvidenceUnprovable{0};
  std::atomic<uint64_t>
      productionOutsidePoisonAuthorityEvidenceMismatches{0};
  std::atomic<uint64_t>
      productionOutsidePoisonAuthorityEvidenceAuthority{0};
  std::atomic<uint64_t>
      productionOutsidePoisonAuthorityLegacyBackedAuthority{0};
  std::array<std::atomic<uint64_t>,
             kNativeOutsidePoisonO1ShadowMatrixSize>
      productionOutsidePoisonAuthorityEvidenceMatrix{};
  std::atomic<uint64_t> productionOutsideFastKernelMarkerConflicts{0};
  std::atomic<uint64_t> originalUploadCalls{0};
  std::atomic<uint64_t> originalUploadBytes{0};
  std::atomic<uint64_t> bypassPreflightAttempts{0};
  std::atomic<uint64_t> bypassAuthorizations{0};
  std::atomic<uint64_t> kernelHookCalls{0};
  std::atomic<uint64_t> originalKernelCalls{0};
  std::atomic<uint64_t> originalKernelNormalReturns{0};
  std::atomic<uint64_t> originalKernelNormalReturnRejects{0};
  std::atomic<uint64_t> cpuRewriteProofAttempts{0};
  std::atomic<uint64_t> cpuRewriteProofExact{0};
  std::atomic<uint64_t> cpuRewriteProofRejects{0};
  std::atomic<uint64_t> originalKernelBytes{0};
  std::atomic<uint64_t> bypassedKernelCalls{0};
  std::atomic<uint64_t> bypassedKernelBytes{0};
  std::atomic<uint64_t> nullMappedKernelFallbacks{0};
  std::atomic<uint64_t> kernelPreflightRejects{0};
  std::atomic<uint64_t> postSkipMismatchFuses{0};
  std::atomic<uint64_t> postSkipNativeFallback{0};
  std::atomic<uint64_t> duplicateKernelCalls{0};
  std::atomic<uint64_t> irreversibleKernelSuppressions{0};
  std::atomic<uint64_t> pendingKernelAuthorizations{0};
  std::atomic<uint64_t> nativePoisonCreates{0};
  std::atomic<uint64_t> nativePoisonClears{0};
  std::atomic<uint64_t> nativePoisonHits{0};
  std::atomic<uint64_t> nativePoisonIncompleteIdentityHits{0};
  std::atomic<uint64_t> identityMatchedLayoutMismatchHits{0};
  std::atomic<uint64_t> nativePoisonOverflows{0};
  std::atomic<uint64_t> nativePoisonLeaks{0};
  std::atomic<uint64_t> nativeDirectDiscardEvents{0};
  std::atomic<uint64_t> nativeDirectDiscardEventsWithPoison{0};
  std::atomic<uint64_t> nativeDirectDiscardNoPoisonEvents{0};
  std::atomic<uint64_t> nativeDirectDiscardRangesCleared{0};
  std::atomic<uint64_t> nativeDirectDiscardInvalid{0};
  std::atomic<uint64_t> nativeCrossMapAllocationPoisonMerges{0};
  std::atomic<uint64_t> nativeRetirementEventsPublished{0};
  std::atomic<uint64_t> nativeRetirementEventsConsumed{0};
  std::atomic<uint64_t> nativeRetirementRangesCleared{0};
  std::atomic<uint64_t> nativeRetirementQueueOverflows{0};
  std::atomic<uint64_t> nativeRetirementInvalidEvents{0};
  std::atomic<uint64_t> resetRequests{0};
  std::atomic<uint64_t> resetCompletions{0};
  std::atomic<uint64_t> resetDeferred{0};
  std::atomic<uint64_t> resetWrongThread{0};
  std::atomic<uint64_t> resetActiveTransactions{0};
  std::atomic<uint64_t> resetOwnerEpochDeferred{0};
  std::atomic<uint64_t> resetPoisonDeferred{0};
  std::atomic<uint64_t> resetRetirementQueueFaults{0};
  std::atomic<uint64_t> resetFailClosedUploads{0};
  std::atomic<uint64_t> resetOwnerAcks{0};
  std::atomic<uint64_t> resetOwnerAckMismatches{0};
  std::atomic<uint64_t> resetCommitBlocked{0};
  std::atomic<uint64_t> resetSlowPathCalls{0};
  std::atomic<uint64_t> indexTicketFailureMask{0};
  std::atomic<uint64_t> indexTicketAttempts{0};
  std::atomic<uint64_t> indexTicketExact{0};
  std::atomic<uint64_t> indexTicketSuppressed{0};
  std::atomic<uint64_t> indexTicketLeaks{0};
  std::atomic<uint64_t> bypassedUploadCalls{0};
  std::atomic<uint64_t> bypassedUploadBytes{0};
  std::atomic<uint64_t> bypassFallbackCalls{0};
  std::atomic<uint64_t> bypassStateMismatches{0};
  std::atomic<uint64_t> bypassSideEffectFailures{0};
  std::atomic<uint64_t> inputPreflightRejectedUploads{0};
  std::atomic<uint64_t> dips{0};
  std::atomic<uint64_t> outsideDispatchDips{0};
  std::atomic<uint64_t> dispatchNoUploadDips{0};
  std::atomic<uint64_t> correlatedDips{0};
  std::atomic<uint64_t> unmatchedDips{0};
  std::atomic<uint64_t> productionOutsideDipFastPath{0};
  std::atomic<uint64_t> dipObserverTransactionsBegun{0};
  std::atomic<uint64_t> dipObserverTransactionsEnded{0};
  std::atomic<uint64_t> dipObserverTransactionMismatches{0};
  std::atomic<uint64_t> dipOutsideReaderBegun{0};
  std::atomic<uint64_t> dipOutsideReaderEnded{0};
  std::atomic<uint64_t> dipOutsideReaderCommits{0};
  std::atomic<uint64_t> dipOutsideReaderRejects{0};
  std::atomic<uint64_t> dipOutsideReaderEvidenceFallbacks{0};
  std::atomic<uint64_t> dipOutsideReaderMismatches{0};
  std::atomic<uint64_t> productionOutsideDipFastByFlush{0};
  std::atomic<uint64_t> productionOutsideDipFastByObserver{0};
  std::atomic<uint64_t> productionOutsideDipFastByReader{0};
  std::atomic<uint64_t> productionOutsideDipFastEarlySuccesses{0};
  std::array<std::atomic<uint64_t>, kNativeOutsideDipFastRejectReasonCount>
      productionOutsideDipFastEarlyRejects{};
  std::array<
      std::atomic<uint64_t>, kNativeOutsideDipFastLocalRejectReasonCount>
      productionOutsideDipFastEarlyLocalRejects{};
  std::atomic<uint64_t> productionOutsideDipFastLateSuccesses{0};
  std::array<std::atomic<uint64_t>, kNativeOutsideDipFastRejectReasonCount>
      productionOutsideDipFastLateRejects{};
  std::array<
      std::atomic<uint64_t>, kNativeOutsideDipFastLocalRejectReasonCount>
      productionOutsideDipFastLateLocalRejects{};
  std::atomic<uint64_t> dispatchCpuOnlySealProposals{0};
  std::atomic<uint64_t> dispatchCpuOnlySealProposalAccepted{0};
  std::atomic<uint64_t> dispatchCpuOnlySealProposalRejected{0};
  std::atomic<uint64_t> dispatchCpuOnlySealProposalAborted{0};
  std::atomic<uint64_t> dispatchCpuOnlySealScopeCommits{0};
  std::atomic<uint64_t> dispatchCpuOnlySealScopeEnds{0};
  std::atomic<uint64_t> dispatchCpuOnlySealInvalidations{0};
  std::atomic<uint64_t> dispatchCpuOnlySealUploadsStarted{0};
  std::atomic<uint64_t> dispatchCpuOnlySealUploadsCompleted{0};
  std::atomic<uint64_t> dispatchCpuOnlySealVertices{0};
  std::atomic<uint64_t> dispatchCpuOnlySealBytes{0};
  std::atomic<uint64_t> dispatchCpuOnlySealKernelCalls{0};
  std::atomic<uint64_t> dispatchCpuOnlySealKernelNormalReturns{0};
  std::atomic<uint64_t> dispatchCpuOnlySealDips{0};
  std::atomic<uint64_t> dispatchCpuOnlySealDipsWithUpload{0};
  std::atomic<uint64_t> dispatchCpuOnlySealDipsNoUpload{0};
  std::atomic<uint64_t> dispatchCpuOnlySealFanoutZero{0};
  std::atomic<uint64_t> dispatchCpuOnlySealFanoutOne{0};
  std::atomic<uint64_t> dispatchCpuOnlySealFanoutMany{0};
  std::atomic<uint64_t> dispatchCpuOnlySealFanoutDipTotal{0};
  std::atomic<uint64_t> dispatchCpuOnlySealMarkerConflicts{0};
  std::atomic<uint64_t> managerDispatchNativeCpuOnlyScopes{0};
  std::atomic<uint64_t> managerDispatchNativeCpuOnlyEnds{0};
  std::atomic<uint64_t> dispatchCpuOnlySealLocalViewPublishAttempts{0};
  std::atomic<uint64_t> dispatchCpuOnlySealLocalViewPublishes{0};
  std::atomic<uint64_t> dispatchCpuOnlySealLocalViewRejects{0};
  std::atomic<uint64_t> dispatchCpuOnlySealLocalViewQueries{0};
  std::atomic<uint64_t> dispatchCpuOnlySealLocalViewAuthorityRejects{0};
  std::atomic<uint64_t> dispatchCpuOnlySealLocalViewCandidateRejects{0};
  std::atomic<uint64_t> dispatchCpuOnlySealLocalViewCommits{0};
  std::atomic<uint64_t> uploadFanoutZero{0};
  std::atomic<uint64_t> uploadFanoutOne{0};
  std::atomic<uint64_t> uploadFanoutMany{0};
  std::atomic<uint64_t> maxUploadFanout{0};
  std::atomic<uint64_t> nestedDispatchScopes{0};
  std::atomic<uint64_t> nestedSemanticScopes{0};
  std::atomic<uint64_t> nestedUploadScopes{0};
  std::atomic<uint64_t> scopeFailClosedUploads{0};
  std::atomic<uint64_t> scopeFailClosedKernelFallbacks{0};
  std::atomic<uint64_t> dispatchStackOverflow{0};
  std::atomic<uint64_t> semanticStackOverflow{0};
  std::atomic<uint64_t> queryMiss{0};
  std::atomic<uint64_t> conflict{0};
  std::atomic<uint64_t> unknown{0};
  std::atomic<uint64_t> layerMismatch{0};
  std::atomic<uint64_t> scopeForced{0};
  std::array<std::atomic<uint64_t>, 8> formatHistogram{};
  std::array<std::atomic<uint64_t>, 8> skinModeHistogram{};
  std::array<std::atomic<uint64_t>, kNativeUploadInputPreflightBitCount>
      inputPreflightMissing{};
  std::array<std::atomic<uint64_t>, kNativeIndexTicketFailureBitCount>
      indexTicketFailureReasons{};
  std::atomic<uint64_t> telemetryFlushes{0};
  std::atomic<uint64_t> telemetryBatchedAdds{0};
  AtomicNativeSampledRawTiming productionOuterAdmissionAcceptedTiming;
  AtomicNativeSampledRawTiming productionOuterAdmissionRejectedTiming;
  AtomicNativeSampledRawTiming productionOuterFastInclusiveTiming;
  AtomicNativeSampledRawTiming productionOuterFastBodyTiming;
  AtomicNativeSampledRawTiming productionOuterFastCompleteTiming;
  AtomicNativeSampledRawTiming productionOuterFastCancelTiming;
  AtomicNativeSampledRawTiming productionKernelInclusiveTiming;
  AtomicNativeSampledRawTiming productionKernelEvaluateTiming;
  AtomicNativeSampledRawTiming productionKernelOriginalTiming;
  AtomicNativeSampledRawTiming productionKernelNotifyTiming;
  std::array<AtomicNativeSampledRawTiming, kNativeBridgeCallbackKindCount>
      productionCallbackPinEnterTiming{};
  std::array<AtomicNativeSampledRawTiming, kNativeBridgeCallbackKindCount>
      productionCallbackBodyTiming{};
  std::array<AtomicNativeSampledRawTiming, kNativeBridgeCallbackKindCount>
      productionCallbackPinLeaveTiming{};
  AtomicNativeSampledRawTiming productionOuterFallbackInclusiveTiming;
  AtomicNativeSampledRawTiming productionOuterFallbackBeginTiming;
  AtomicNativeSampledRawTiming productionOuterFallbackBodyTiming;
  AtomicNativeSampledRawTiming productionOuterFallbackCompleteTiming;
  AtomicNativeSampledRawTiming productionFlushRootTiming;
  AtomicNativeSampledRawTiming productionDispatchSemanticLookupTiming;
  AtomicNativeSampledRawTiming productionDispatchBeginRootTiming;
  AtomicNativeSampledRawTiming productionDispatchEndRootTiming;
  AtomicNativeSampledRawTiming productionSemanticInclusiveTiming;
  AtomicNativeSampledRawTiming productionSemanticOriginalTiming;
  AtomicNativeSampledRawTiming productionDipDeviceRootOutsideTiming;
  AtomicNativeSampledRawTiming productionDipDeviceRootNoUploadTiming;
  AtomicNativeSampledRawTiming productionDipDeviceRootCorrelatedTiming;
  AtomicNativeSampledRawTiming productionDipBridgeOutsideTiming;
  AtomicNativeSampledRawTiming productionDipBridgeNoUploadTiming;
  AtomicNativeSampledRawTiming productionDipBridgeCorrelatedTiming;
  AtomicNativeSampledRawTiming productionDipResolveOutsideTiming;
  AtomicNativeSampledRawTiming productionDipResolveNoUploadTiming;
  AtomicNativeSampledRawTiming productionDipResolveCorrelatedTiming;
  std::atomic<uint64_t> productionFastRejectKernelBatches{0};
  std::atomic<uint64_t> productionOutsideAdmissionAcceptedNoPoison{0};
  std::atomic<uint64_t> productionOutsideAdmissionAcceptedWithPoison{0};
  std::atomic<uint64_t> productionOutsideAdmissionCancellations{0};
  std::atomic<uint64_t> productionOutsideAdmissionLifecycleExcluded{0};
  std::atomic<uint64_t> productionOutsideAdmissionTrackedResolvedInside{0};
  std::atomic<uint64_t> productionOutsideAdmissionUntrackedResolvedOutside{0};
  std::atomic<uint64_t> productionOutsideCoverFlushBegins{0};
  std::atomic<uint64_t> productionOutsideCoverSemanticBegins{0};
  std::atomic<uint64_t> productionOutsideCoverIndependentBegins{0};
  std::atomic<uint64_t> productionOutsideIndependentPinBegins{0};
  std::atomic<uint64_t> productionOutsideIndependentPinEnds{0};
  std::array<std::atomic<uint64_t>,
             kNativeOutsideUploadRejectReasonCount>
      productionOutsideAdmissionRejectNoPoison{};
  std::array<std::atomic<uint64_t>,
             kNativeOutsideUploadRejectReasonCount>
      productionOutsideAdmissionRejectWithPoison{};
  std::array<AtomicNativeSampledRawTiming,
             kNativeOutsideUploadAdmissionClassCount>
      productionOuterAdmissionAcceptedClassTiming{};
  std::array<AtomicNativeSampledRawTiming,
             kNativeOutsideUploadAdmissionClassCount>
      productionOuterFastCompleteClassTiming{};
  std::array<AtomicNativeSampledRawTiming,
             kNativeOutsideUploadRejectReasonCount>
      productionOuterFallbackReasonTiming{};
  AtomicNativeSampledRawTiming productionOuterDispatchSealAdmissionTiming;
  AtomicNativeSampledRawTiming productionOuterDispatchSealInclusiveTiming;
  AtomicNativeSampledRawTiming productionOuterDispatchSealBodyTiming;
  AtomicNativeSampledRawTiming productionOuterDispatchSealCompleteTiming;
  AtomicNativeSampledRawTiming productionOuterDispatchSealCancelTiming;
};

AtomicNativeBridgeCounters g_counters;

class AtomicRawTickAccumulator {
public:
  AtomicRawTickAccumulator(
      bool enabled,
      std::atomic<uint64_t>& calls, std::atomic<uint64_t>& total,
      std::atomic<uint64_t>& maximum) noexcept
      : m_enabled(enabled), m_total(total), m_maximum(maximum),
        m_start(enabled ? dxvk::high_resolution_clock::get_counter() : 0) {
    if (m_enabled)
      calls.fetch_add(1u, std::memory_order_relaxed);
  }

  ~AtomicRawTickAccumulator() {
    if (!m_enabled)
      return;
    const int64_t elapsed =
        dxvk::high_resolution_clock::get_counter() - m_start;
    if (elapsed <= 0)
      return;
    const uint64_t ticks = uint64_t(elapsed);
    m_total.fetch_add(ticks, std::memory_order_relaxed);
    uint64_t current = m_maximum.load(std::memory_order_relaxed);
    while (current < ticks &&
           !m_maximum.compare_exchange_weak(
               current, ticks, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
  }

private:
  bool m_enabled;
  std::atomic<uint64_t>& m_total;
  std::atomic<uint64_t>& m_maximum;
  int64_t m_start;
};

struct NativeIndexSnapshotState {
  std::vector<uint8_t> bytes;
  uint64_t uploadEpoch = 0;
  uint64_t ticketGeneration = 0;
};

struct NativeGeosetSnapshotState {
  std::array<uint8_t, kGeosetStateSnapshotSize> bytes{};
  uintptr_t geosetData = 0u;
  uint64_t uploadEpoch = 0u;
  bool ready = false;
};

// Stack-only telemetry sink for the production T2 admission sample.  It is
// never part of NativeUploadObservation or the callback ABI, so evidence modes
// and irreversible authorization continue to consume the exact same proof.
struct NativeT2SampleDurations {
  uint64_t geoSnapTicks = 0u;
  uint64_t geoHeaderTicks = 0u;
  uint64_t positionProofTicks = 0u;
  uint64_t normalProofTicks = 0u;
  uint64_t groupProofTicks = 0u;
  uint64_t paletteProofTicks = 0u;
};

struct ActiveUploadState {
  NativeUploadObservation observation;
  NativeIndexSnapshotState indexSnapshot;
  uint32_t dipCount = 0;
  bool valid = false;
};

enum class ManagerDispatchState : uint8_t {
  // Zero initialization must be fail closed. A frame is assigned one of the
  // remaining states before it can publish any manager callback.
  FailedClosed = 0,
  EagerPending,
  PendingCommon,
  NeverSpecial,
  NativeCpuOnly,
  BeginIssued,
};

struct NativeDispatchCpuOnlySealView {
  uint64_t flushEpoch = 0u;
  uint64_t resetGeneration = 0u;
  uint32_t count = 0u;
  std::array<NativeDispatchCpuOnlySealCandidate,
             kNativeDispatchCpuOnlySealViewCapacity> candidates{};
};

struct DispatchCpuOnlySealState {
  uint64_t resetGeneration = 0u;
  bool proposed = false;
  bool committed = false;
  bool committedEver = false;
  // 首次物理上传仍使用完整证明；之后只检查可能异步变化的动态门。
  bool uploadFastPathValidated = false;
};

struct DispatchCpuOnlyActiveUpload {
  uint32_t dipCount = 0u;
  bool valid = false;
};

struct DispatchFrame {
  NativeDispatchObservation observation;
  ActiveUploadState activeUpload;
  DispatchCpuOnlySealState cpuOnlySeal;
  DispatchCpuOnlyActiveUpload cpuOnlyActiveUpload;
  uint32_t semanticScopeCount = 0;
  uint32_t uploadCount = 0;
  uint32_t dipCount = 0;
  // A live outer flush transaction already prevents reset/uninstall
  // quiescence.  Only scopes entered outside that lifetime need their own
  // cross-thread pin; the TLS depth remains authoritative in either case.
  bool globalTransactionPinned = false;
  bool callbacksAdmitted = false;
  bool b1BorrowedFlushPinPassThrough = false;
  bool previousB1BorrowedFlushPinPassThrough = false;
  ManagerDispatchState managerState = ManagerDispatchState::FailedClosed;
};

struct SemanticFrame {
  uintptr_t geosetData = 0;
  uintptr_t layerDispatch = 0;
  uintptr_t layerState = 0;
  uintptr_t callerAddress = 0;
  NativeDispatchPath path = NativeDispatchPath::Unknown;
  bool singlePassCaller = false;
  bool failClosed = false;
  bool globalTransactionPinned = false;
  uint64_t dispatchEpoch = 0;
  uint64_t cookie = 0;
};

struct InFlightUploadState {
  NativeUploadObservation* observation = nullptr;
  uint64_t renderThreadId = 0;
  uint64_t uploadEpoch = 0;
  uintptr_t gxDeviceD3d = 0;
  NativeIndexSnapshotState indexSnapshot;
  NativeGeosetSnapshotState geosetSnapshot;
  bool conflicted = false;
  bool callbacksAdmitted = false;
  bool globalTransactionPinned = false;
};

struct NativeVertexStorageSidecar {
  NativeVertexOutputProof proof;
  NativeVertexStorageDiagnostics diagnostics;
  bool valid = false;
};

struct NativePoisonRange {
  NativeEpochKey epoch;
  NativeVertexOutputProof vertexOutputProof;
  NativeVertexStorageDiagnostics storageDiagnostics;
  uint64_t fuseKey = 0;
  uint32_t baseVertex = 0;
  uint32_t vertexCount = 0;
  uint32_t stride = 0;
  uint32_t fvf = 0;
  uint32_t outputFormat = 0;
  uint32_t expectedStartIndex = 0;
  uint32_t expectedIndexCount = 0;
  bool realStorageMixed = false;
  bool mappingStorageMixed = false;
  bool mapAllocationMixed = false;
  bool mapModeMixed = false;
  bool indexSignatureMixed = false;
};

struct NativePoisonLedger {
  std::array<NativePoisonRange, kMaxNativePoisonRanges> ranges{};
  size_t count = 0;
};

enum class NativeOutsidePoisonScanResult : uint8_t {
  NoOverlap = 0,
  Overlap,
  ReadFailure,
};
static_assert(
    static_cast<uint8_t>(NativeOutsidePoisonScanResult::NoOverlap) == 0u &&
    static_cast<uint8_t>(NativeOutsidePoisonScanResult::Overlap) == 1u &&
    static_cast<uint8_t>(NativeOutsidePoisonScanResult::ReadFailure) == 2u,
    "outside poison shadow matrix requires N/O/R indices 0/1/2");

struct NativeOutsidePoisonScanObservation {
  uintptr_t gxDeviceD3d = 0u;
  uintptr_t nativeD3DDevice = 0u;
  uintptr_t comVertexBuffer = 0u;
  uint32_t vertexCount = 0u;
  uint32_t outputFormat = 0u;
  uint32_t ringNext = 0u;
  uint32_t predictedBaseVertex = 0u;
  bool snapshotCaptured = false;
  bool targetParsed = false;
};

// Immutable copy of the first exact poison range which made the legacy
// SafeCopy scan return O. This is report-only provenance: it is never consulted
// by admission, poison removal, or native-kernel suppression.
struct NativeOutsidePoisonExactOverlap {
  NativeVertexOutputProof vertexOutputProof;
  uint32_t baseVertex = 0u;
  uint32_t vertexCount = 0u;
  uint32_t stride = 0u;
  uint32_t fvf = 0u;
  uint32_t outputFormat = 0u;
  bool valid = false;
};

// One validated DIRECT DISCARD observed between the old SafeCopy scan and the
// successful Lock notification. The event is only attributable when its exact
// poison-ledger transition remains unchanged through Lock freeze.
struct NativeOutsidePoisonDirectDiscardObservation {
  uintptr_t commonResource = 0u;
  uint64_t resourceGeneration = 0u;
  uint64_t previousMapAllocationGeneration = 0u;
  uint64_t newMapAllocationGeneration = 0u;
  uint64_t mutationGenerationBefore = 0u;
  uint64_t mutationGenerationAfter = 0u;
  uint64_t poisonOutstandingBefore = 0u;
  uint64_t poisonOutstandingAfter = 0u;
  uint32_t poisonCountBefore = 0u;
  uint32_t poisonCountAfter = 0u;
  uint32_t cleared = 0u;
  uint32_t oldExactOverlapRangesCleared = 0u;
  bool exactOldOverlapRetired = false;
};

struct NativeOutsidePoisonShadowProbe {
  uint64_t cookie = 0u;
  uint64_t ownerThreadId = 0u;
  uint64_t resetGeneration = 0u;
  uint64_t flushEpoch = 0u;
  uint64_t poisonMutationGeneration = 0u;
  uint64_t poisonOutstanding = 0u;
  uint64_t o1LockLedgerGeneration = 0u;
  uint64_t o1LockPoisonOutstanding = 0u;
  uint64_t o1KernelFreezeGeneration = 0u;
  uintptr_t gxDeviceD3d = 0u;
  uint64_t outputByteCount = 0u;
  uint32_t vertexCount = 0u;
  uint32_t poisonCount = 0u;
  uint32_t o1LockPoisonCount = 0u;
  uint32_t o1LockOutputFormat = 0u;
  uint32_t o1LockVertexStride = 0u;
  uint32_t o1LockBaseVertex = 0u;
  uint32_t o1LockVertexCount = 0u;
  NativeOutsidePoisonScanResult oldResult =
      NativeOutsidePoisonScanResult::ReadFailure;
  NativeOutsidePoisonScanResult sidecarResult =
      NativeOutsidePoisonScanResult::ReadFailure;
  NativeOutsidePoisonScanResult o1SidecarResult =
      NativeOutsidePoisonScanResult::ReadFailure;
  NativeOutsidePoisonScanObservation oldObservation;
  NativeOutsidePoisonExactOverlap oldExactOverlap;
  NativeOutsidePoisonDirectDiscardObservation directDiscardObservation;
  NativeOutsidePoisonVertexLockEvidence lockEvidence;
  NativeOutsidePoisonVertexUnlockEvidence o1UnlockEvidence;
  NativeOutsidePoisonShadowUnprovableReason unprovableReason =
      NativeOutsidePoisonShadowUnprovableReason::Count;
  NativeOutsidePoisonO1ShadowUnprovableReason o1UnprovableReason =
      NativeOutsidePoisonO1ShadowUnprovableReason::Count;
  uintptr_t o1KernelGxDeviceD3d = 0u;
  uintptr_t o1KernelMappedDst = 0u;
  uint32_t o1KernelSkinMode = 0u;
  uint32_t o1KernelOutputFormat = 0u;
  uint32_t lockNotifications = 0u;
  uint32_t o1KernelNotifications = 0u;
  uint32_t o1UnlockNotifications = 0u;
  uint32_t directDiscardNotifications = 0u;
  NativeOutsidePoisonO1LockLane o1LockLane =
      NativeOutsidePoisonO1LockLane::Count;
  GpuSkinOutsidePoisonSidecarPolicy sidecarPolicy =
      GpuSkinOutsidePoisonSidecarPolicy::None;
  bool oldResultRecorded = false;
  bool o1LockVerdictRecorded = false;
  bool o1LogicalPublished = false;
  bool o1KernelStateCaptured = false;
  bool kernelNormalSeen = false;
  bool o1UnlockAfterFreeze = false;
  bool o1Frozen = false;
  bool frozen = false;
  // Production O1 owns the same outer cookie but remains independent of the
  // O0/O1a report policy. It is provisional until this call's actual Lock,
  // original kernel normal-return, exact Unlock and outer settlement close.
  bool authorityCandidate = false;
  bool authorityArmed = false;
  bool authorityUsesActualLock = false;
  bool authorityEvidence = false;
  bool authorityLegacyBacked = false;
  bool authorityLockExact = false;
  bool authorityKernelExact = false;
  bool authorityUnlockExact = false;
  bool authorityLockTerminalRecorded = false;
  bool authorityKernelTerminalRecorded = false;
  bool authorityUnlockTerminalRecorded = false;
  bool authorityEvidenceTerminalRecorded = false;
  bool authorityConflicted = false;
  bool authorityActivePublished = false;
  bool authorityFastNoOverlap = false;
  NativeOutsidePoisonScanResult authorityLockResult =
      NativeOutsidePoisonScanResult::ReadFailure;
  uint64_t authorityLockPoisonGeneration = 0u;
  uint64_t authorityLockPoisonOutstanding = 0u;
  uint32_t authorityLockPoisonCount = 0u;
  uint32_t authorityOutputFormat = 0u;
  uint32_t authorityVertexStride = 0u;
  uint32_t authorityBaseVertex = 0u;
  uint32_t authorityVertexCount = 0u;
};

struct NativeOutsidePoisonShadowState {
  std::array<NativeOutsidePoisonShadowProbe,
             kMaxOutsidePoisonShadowDepth> probes{};
  size_t depth = 0u;
  size_t overflowDepth = 0u;
  uint64_t cookieSequence = 0u;
};

struct NativeOutsideUploadFastPathState {
  uintptr_t gxDeviceD3d = 0u;
  uint64_t renderThreadId = 0u;
  uint64_t cookie = 0u;
  uint64_t resetGeneration = 0u;
  uint64_t poisonShadowCookie = 0u;
  size_t semanticDepth = 0u;
  uint64_t semanticCookie = 0u;
  bool kernelSeen = false;
  bool conflicted = false;
  bool admittedWithPoison = false;
  NativeOutsideUploadCoverKind coverKind =
      NativeOutsideUploadCoverKind::Count;
  bool admissionExactTracked = false;
};

// The no-poison direct-original route is deliberately not an authorization
// object. It retains only immutable TLS identity needed to prove that the
// nested native kernel and the enclosing outer upload stayed in the same
// transaction. It never owns a manager callback, resource, poison range,
// upload pin, or O1 token, and can therefore only select the original kernel.
struct NativeOutsideNoPoisonDirectOriginalState {
  uintptr_t gxDeviceD3d = 0u;
  uintptr_t mappedDst = 0u;
  uint64_t renderThreadId = 0u;
  uint64_t cookie = 0u;
  uint64_t resetGeneration = 0u;
  uint64_t poisonMutationGeneration = 0u;
  size_t semanticDepth = 0u;
  uint64_t semanticCookie = 0u;
  bool kernelSeen = false;
  bool kernelNormalReturned = false;
  bool conflicted = false;
  bool resetDriftRecorded = false;
  bool latePoisonRecorded = false;
};

struct NativeDispatchCpuOnlyUploadFastPathState {
  uintptr_t gxDeviceD3d = 0u;
  uintptr_t mappedDst = 0u;
  uint64_t outputByteCount = 0u;
  uint32_t vertexCount = 0u;
  uint64_t renderThreadId = 0u;
  uint64_t cookie = 0u;
  uint64_t resetGeneration = 0u;
  uint64_t dispatchEpoch = 0u;
  size_t semanticDepth = 0u;
  uint64_t semanticCookie = 0u;
  bool kernelSeen = false;
  bool kernelNormalReturned = false;
  bool conflicted = false;
};

struct ThreadState {
  uint64_t flushEpoch = 0;
  uint64_t flushCallbackEpoch = 0;
  size_t flushTransactionDepth = 0;
  NativeDispatchCpuOnlySealView dispatchCpuOnlySealView;
  uint32_t orphanUploadOrdinal = 0;
  uint32_t orphanDipOrdinal = 0;
  std::array<DispatchFrame, kMaxDispatchDepth> dispatchFrames{};
  std::array<SemanticFrame, kMaxSemanticDepth> semanticFrames{};
  size_t dispatchDepth = 0;
  size_t semanticDepth = 0;
  size_t dispatchOverflowDepth = 0;
  size_t semanticOverflowDepth = 0;
  size_t nestedUploadDepth = 0;
  std::array<NativeUploadObservation*, kMaxNestedUploadDepth>
      nestedUploadObservations{};
  InFlightUploadState inFlightUpload;
  NativeGeosetSnapshotState pendingGeosetSnapshot;
  std::vector<uint8_t> spareIndexSnapshotBytes;
  NativeVertexStorageSidecar vertexStorageSidecar;
  NativePoisonLedger poisonLedger;
  NativePoisonDiagnosticSnapshot poisonDiagnostics;
  uint64_t appliedResetGeneration = 0u;
  uint64_t deferredResetGeneration = 0u;
  NativeBridgeResetStatus deferredResetStatus =
      NativeBridgeResetStatus::Completed;
  uint64_t beginTimingSampleOrdinal = 0u;
  uint64_t productionFlushTimingOrdinal = 0u;
  uint64_t productionDispatchBeginTimingOrdinal = 0u;
  uint64_t productionDispatchEndTimingOrdinal = 0u;
  uint64_t productionOutsideDipFastEarlyOrdinal = 0u;
  uint64_t productionOutsideDipFastLateOrdinal = 0u;
  // 非候选 B1 semantic 省略路径保留固定低频证据，确保独立 poison
  // 事务仍持续验证真实语义覆盖，而不会把证据误当成授权。
  uint64_t productionSemanticObservationOrdinal = 0u;
  uint64_t outsideUploadFastPathGeneration = 0u;
  NativeOutsideUploadFastPathState outsideUploadFastPath;
  uint64_t outsideNoPoisonDirectOriginalGeneration = 0u;
  NativeOutsideNoPoisonDirectOriginalState
      outsideNoPoisonDirectOriginal;
  uint64_t dispatchCpuOnlyUploadFastPathGeneration = 0u;
  NativeDispatchCpuOnlyUploadFastPathState
      dispatchCpuOnlyUploadFastPath;
  // Report-only identity for an admission-rejected outer call that deliberately
  // owns no generic InFlightUploadState. The exact 1/127 evidence cohort keeps
  // the old generic path and is identified separately by upload epoch.
  NativeUploadObservation* productionFastRejectedUpload = nullptr;
  bool productionFastRejectedGlobalTransactionPinned = false;
  uint64_t productionEvidenceUploadEpoch = 0u;
};

thread_local ThreadState t_state;
thread_local uint32_t t_callbackPinDepth = 0u;
thread_local uint32_t t_dipObserverDepth = 0u;
thread_local uint64_t t_dipObserverCookie = 0u;
thread_local uint64_t t_dipObserverResetGeneration = 0u;
thread_local uint64_t t_dipObserverCpuOnlySealCookie = 0u;
thread_local uint64_t t_dipObserverCookieSequence = 0u;
// Independent of ThreadState so a completed bridge reset cannot align the
// retained legacy-observer evidence cohort with a fresh flush epoch.
thread_local uint64_t t_outsideDipReaderEvidenceOrdinal = 0u;
// Production O1 evidence is process-lifetime TLS as well. Bridge reset assigns
// ThreadState from {}, but must not restart the 1/127 legacy SafeCopy cohort or
// over-sample lifecycle-heavy workloads.
thread_local uint64_t t_outsidePoisonAuthorityEvidenceOrdinal = 0u;
// Production Bypass proves that the entire live Game.dll palette can actually
// be read without paying a VirtualQuery walk for every T2 candidate.  This
// buffer is deliberately TLS rather than stack storage: the maximum palette is
// 12 KiB, and its bytes are only a fault-safe readability sink.  They are never
// retained, compared, hashed, or used to authorize a native-kernel skip.
thread_local std::array<uint8_t, kNativePaletteReadabilityScratchBytes>
    t_paletteReadabilityScratch = {};
thread_local std::array<uint8_t, kOutsidePoisonSnapshotSize>
    t_outsidePoisonSnapshotScratch = {};
thread_local std::array<uint8_t, kVertexShaderBypassPoisonStateSize>
    t_vertexShaderBypassPoisonStateScratch = {};
// This sequence deliberately lives outside ThreadState. A completed bridge
// reset assigns t_state = {}, but a cache/sidecar proof must never mistake the
// post-reset empty ledger for an older ledger generation. Every actual ledger
// mutation advances this sequence, and zero remains an invalid/uninitialized
// generation even across unsigned wrap.
thread_local uint64_t t_poisonMutationGeneration = 0u;
// O0 is deliberately outside ThreadState and outside every bridge-quiescence
// predicate. It is a report-only shadow owned by the outer hook's independent
// RAII cookie; reset aborts it explicitly instead of waiting on it.
thread_local NativeOutsidePoisonShadowState t_outsidePoisonShadow;

void AdvancePoisonMutationGeneration() noexcept {
  ++t_poisonMutationGeneration;
  if (t_poisonMutationGeneration == 0u)
    ++t_poisonMutationGeneration;
}

uint64_t AdvancePoisonMutationGenerationBy(
    uint64_t generation, uint32_t count) noexcept {
  while (count-- != 0u) {
    ++generation;
    if (generation == 0u)
      ++generation;
  }
  return generation;
}

bool NonZeroGenerationAdvancedOnce(
    uint64_t previous, uint64_t current) noexcept {
  return previous != 0u && current != 0u &&
      (current == previous + 1u ||
       (previous == std::numeric_limits<uint64_t>::max() && current == 1u));
}

bool OutsideUploadFastPathActive() noexcept {
  return t_state.outsideUploadFastPath.cookie != 0u;
}

bool OutsideNoPoisonDirectOriginalActive() noexcept {
  return t_state.outsideNoPoisonDirectOriginal.cookie != 0u;
}

bool DispatchCpuOnlyUploadFastPathActive() noexcept {
  return t_state.dispatchCpuOnlyUploadFastPath.cookie != 0u;
}

uint64_t NextOutsideUploadFastPathCookie() noexcept {
  ++t_state.outsideUploadFastPathGeneration;
  if (t_state.outsideUploadFastPathGeneration == 0u)
    ++t_state.outsideUploadFastPathGeneration;
  return t_state.outsideUploadFastPathGeneration;
}

uint64_t NextOutsideNoPoisonDirectOriginalCookie() noexcept {
  ++t_state.outsideNoPoisonDirectOriginalGeneration;
  if (t_state.outsideNoPoisonDirectOriginalGeneration == 0u)
    ++t_state.outsideNoPoisonDirectOriginalGeneration;
  return t_state.outsideNoPoisonDirectOriginalGeneration;
}

uint64_t NextDispatchCpuOnlyUploadFastPathCookie() noexcept {
  ++t_state.dispatchCpuOnlyUploadFastPathGeneration;
  if (t_state.dispatchCpuOnlyUploadFastPathGeneration == 0u)
    ++t_state.dispatchCpuOnlyUploadFastPathGeneration;
  return t_state.dispatchCpuOnlyUploadFastPathGeneration;
}

// Keep this list deliberately narrow. These fields are diagnostic totals only:
// none participates in epochs, ordinals, authorization decisions, poison
// mutation, retirement, reset, resource ownership, index tickets,
// normal-return proof, or timing.
#define DXVK_NATIVE_TELEMETRY_SCALAR_FIELDS(X) \
  X(dispatchScopes)                             \
  X(dispatchScopeEnds)                          \
  X(commonDispatchScopes)                       \
  X(specialDispatchScopes)                      \
  X(semanticScopes)                             \
  X(managerDispatchEagerScopes)                 \
  X(managerDispatchLazyScopes)                  \
  X(managerDispatchNeverScopes)                 \
  X(managerDispatchNativeCpuOnlyScopes)         \
  X(managerDispatchNativeCpuOnlyEnds)           \
  X(managerDispatchEvidenceEagerScopes)         \
  X(managerDispatchBeginCallbacks)              \
  X(managerDispatchEndCallbacks)                \
  X(managerDispatchEagerBegins)                 \
  X(managerDispatchLazyAdmissionAttempts)       \
  X(managerDispatchLazyAdmissions)              \
  X(managerDispatchIssuedEnds)                  \
  X(managerDispatchNoUploadEnds)                \
  X(managerDispatchNeverEnds)                   \
  X(managerDispatchSkippedUploads)              \
  X(managerDispatchSkippedDips)                 \
  X(managerDispatchSkippedFanouts)              \
  X(productionFastRejectScope)                  \
  X(productionFastRejectUnknownState)           \
  X(productionOutsideCallbacksSkipped)          \
  X(productionOutsideNativeFastPath)            \
  X(productionOutsideNoPoisonDirectAttempts)    \
  X(productionOutsideNoPoisonDirectKernelCalls) \
  X(productionOutsideNoPoisonDirectNormalReturns) \
  X(productionOutsideNoPoisonDirectKernelNoNormalReturns) \
  X(productionOutsideNoPoisonDirectCompleted)   \
  X(uploads)                                    \
  X(uploadsOutsideDispatch)                     \
  X(originalUploadCalls)                        \
  X(originalUploadBytes)                        \
  X(inputPreflightRejectedUploads)              \
  X(uploadFanoutZero)                           \
  X(uploadFanoutOne)                            \
  X(uploadFanoutMany)                           \
  X(dips)                                       \
  X(outsideDispatchDips)                        \
  X(dispatchNoUploadDips)                       \
  X(correlatedDips)                             \
  X(unmatchedDips)                              \
  X(productionOutsideDipFastPath)               \
  X(dipObserverTransactionsBegun)               \
  X(dipObserverTransactionsEnded)               \
  X(dipOutsideReaderBegun)                       \
  X(dipOutsideReaderEnded)                       \
  X(dipOutsideReaderCommits)                     \
  X(dipOutsideReaderRejects)                     \
  X(dipOutsideReaderEvidenceFallbacks)           \
  X(productionOutsideDipFastByFlush)             \
  X(productionOutsideDipFastByObserver)          \
  X(productionOutsideDipFastByReader)            \
  X(dispatchCpuOnlySealProposals)                \
  X(dispatchCpuOnlySealProposalAccepted)         \
  X(dispatchCpuOnlySealProposalRejected)         \
  X(dispatchCpuOnlySealProposalAborted)          \
  X(dispatchCpuOnlySealScopeCommits)             \
  X(dispatchCpuOnlySealScopeEnds)                \
  X(dispatchCpuOnlySealInvalidations)            \
  X(dispatchCpuOnlySealUploadsStarted)           \
  X(dispatchCpuOnlySealUploadsCompleted)         \
  X(dispatchCpuOnlySealVertices)                 \
  X(dispatchCpuOnlySealBytes)                    \
  X(dispatchCpuOnlySealKernelCalls)              \
  X(dispatchCpuOnlySealKernelNormalReturns)      \
  X(dispatchCpuOnlySealDips)                     \
  X(dispatchCpuOnlySealDipsWithUpload)           \
  X(dispatchCpuOnlySealDipsNoUpload)             \
  X(dispatchCpuOnlySealFanoutZero)                \
  X(dispatchCpuOnlySealFanoutOne)                 \
  X(dispatchCpuOnlySealFanoutMany)                \
  X(dispatchCpuOnlySealFanoutDipTotal)            \
  X(dispatchCpuOnlySealMarkerConflicts)           \
  X(dispatchCpuOnlySealLocalViewQueries)          \
  X(dispatchCpuOnlySealLocalViewAuthorityRejects) \
  X(dispatchCpuOnlySealLocalViewCandidateRejects) \
  X(dispatchCpuOnlySealLocalViewCommits)          \
  X(kernelHookCalls)                            \
  X(originalKernelCalls)                        \
  X(originalKernelBytes)                        \
  X(productionFastRejectKernelBatches)           \
  X(productionOutsideAdmissionAcceptedNoPoison)  \
  X(productionOutsideAdmissionAcceptedWithPoison) \
  X(productionOutsideAdmissionCancellations)     \
  X(productionOutsideAdmissionLifecycleExcluded) \
  X(productionOutsideAdmissionTrackedResolvedInside) \
  X(productionOutsideAdmissionUntrackedResolvedOutside) \
  X(productionOutsideCoverFlushBegins)          \
  X(productionOutsideCoverSemanticBegins)       \
  X(productionOutsideCoverIndependentBegins)    \
  X(productionOutsideIndependentPinBegins)      \
  X(productionOutsideIndependentPinEnds)        \
  X(productionOutsidePoisonAuthorityAttempts)     \
  X(productionOutsidePoisonAuthorityCreated)      \
  X(productionOutsidePoisonAuthorityArmed)        \
  X(productionOutsidePoisonAuthoritySettled)      \
  X(productionOutsidePoisonAuthorityLockNotifications) \
  X(productionOutsidePoisonAuthorityLockNoOverlap) \
  X(productionOutsidePoisonAuthorityLockOverlap)  \
  X(productionOutsidePoisonAuthorityKernelReady)  \
  X(productionOutsidePoisonAuthorityNormalReturns) \
  X(productionOutsidePoisonAuthorityUnlockNotifications) \
  X(productionOutsidePoisonAuthorityUnlockExact)  \
  X(productionOutsidePoisonAuthorityCommittedNoOverlap) \
  X(productionOutsidePoisonAuthorityCommittedRewrite) \
  X(productionOutsidePoisonAuthorityPoisonClears) \
  X(productionOutsidePoisonAuthority)

struct NativeTelemetryDelta {
  uint64_t ownerThreadId = 0u;
  uint64_t batchedAdds = 0u;
#define DECLARE_NATIVE_TELEMETRY_FIELD(name) uint64_t name = 0u;
  DXVK_NATIVE_TELEMETRY_SCALAR_FIELDS(DECLARE_NATIVE_TELEMETRY_FIELD)
#undef DECLARE_NATIVE_TELEMETRY_FIELD
  std::array<uint64_t, 8> formatHistogram{};
  std::array<uint64_t, 8> skinModeHistogram{};
  std::array<uint64_t, kNativeOutsideUploadRejectReasonCount>
      productionOutsideAdmissionRejectNoPoison{};
  std::array<uint64_t, kNativeOutsideUploadRejectReasonCount>
      productionOutsideAdmissionRejectWithPoison{};
  bool enabled = false;
  bool dirty = false;
  bool flushing = false;
};

// This lifetime is intentionally independent from ThreadState: a bridge reset
// may assign t_state = {}, but it must first publish this delta and may never
// make a migrated thread appear to own the old thread's unpublished totals.
thread_local NativeTelemetryDelta t_nativeTelemetryDelta;

void FaultNativeTelemetryDelta() noexcept {
  t_nativeTelemetryDelta.enabled = false;
  g_nativeTelemetryDeltaFaulted.store(true, std::memory_order_release);
}

void PublishNativeTelemetryDeltaPending() noexcept {
  NativeTelemetryDelta& telemetry = t_nativeTelemetryDelta;
  if (telemetry.dirty)
    return;
  // Publish before the first local mutation. Off-thread relaxed snapshots may
  // now observe stale aggregate counters, but must also observe pending=1.
  g_nativeTelemetryDeltaPending.store(1u, std::memory_order_release);
  telemetry.dirty = true;
}

bool TryBatchNativeTelemetryAdd(uint64_t& delta, uint64_t value = 1u) noexcept {
  NativeTelemetryDelta& telemetry = t_nativeTelemetryDelta;
  if (!telemetry.enabled || telemetry.flushing)
    return false;

  constexpr uint64_t maxValue = std::numeric_limits<uint64_t>::max();
  if (value > maxValue - delta ||
      value > maxValue - telemetry.batchedAdds) {
    // Do not mutate the existing delta. It remains pending for its exact owner
    // to merge, while this and every later event fall back to the atomics.
    FaultNativeTelemetryDelta();
    return false;
  }

  PublishNativeTelemetryDeltaPending();
  delta += value;
  telemetry.batchedAdds += value;
  return true;
}

void AddNativeTelemetryCounter(std::atomic<uint64_t>& aggregate,
                               uint64_t& delta,
                               uint64_t value = 1u) noexcept {
  if (!TryBatchNativeTelemetryAdd(delta, value))
    aggregate.fetch_add(value, std::memory_order_relaxed);
}

void RecordNativeOutsideAdmissionAccepted(bool entryHadPoison) noexcept {
  NativeTelemetryDelta& telemetry = t_nativeTelemetryDelta;
  if (entryHadPoison) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsideAdmissionAcceptedWithPoison,
        telemetry.productionOutsideAdmissionAcceptedWithPoison);
  } else {
    AddNativeTelemetryCounter(
        g_counters.productionOutsideAdmissionAcceptedNoPoison,
        telemetry.productionOutsideAdmissionAcceptedNoPoison);
  }
}

void RecordNativeOutsideCoverBegin(
    NativeOutsideUploadCoverKind coverKind) noexcept {
  switch (coverKind) {
    case NativeOutsideUploadCoverKind::Flush:
      AddNativeTelemetryCounter(
          g_counters.productionOutsideCoverFlushBegins,
          t_nativeTelemetryDelta.productionOutsideCoverFlushBegins);
      break;
    case NativeOutsideUploadCoverKind::Semantic:
      AddNativeTelemetryCounter(
          g_counters.productionOutsideCoverSemanticBegins,
          t_nativeTelemetryDelta.productionOutsideCoverSemanticBegins);
      break;
    case NativeOutsideUploadCoverKind::Independent:
      AddNativeTelemetryCounter(
          g_counters.productionOutsideCoverIndependentBegins,
          t_nativeTelemetryDelta.productionOutsideCoverIndependentBegins);
      break;
    case NativeOutsideUploadCoverKind::Count:
      break;
  }
}

void RecordNativeOutsideIndependentPinBegin() noexcept {
  AddNativeTelemetryCounter(
      g_counters.productionOutsideIndependentPinBegins,
      t_nativeTelemetryDelta.productionOutsideIndependentPinBegins);
}

void RecordNativeOutsideIndependentPinEnd() noexcept {
  AddNativeTelemetryCounter(
      g_counters.productionOutsideIndependentPinEnds,
      t_nativeTelemetryDelta.productionOutsideIndependentPinEnds);
}

void RecordNativeOutsideAdmissionCancellation() noexcept {
  AddNativeTelemetryCounter(
      g_counters.productionOutsideAdmissionCancellations,
      t_nativeTelemetryDelta.productionOutsideAdmissionCancellations);
}

void RecordNativeOutsideAdmissionLifecycleExcluded() noexcept {
  AddNativeTelemetryCounter(
      g_counters.productionOutsideAdmissionLifecycleExcluded,
      t_nativeTelemetryDelta.productionOutsideAdmissionLifecycleExcluded);
}

void RecordNativeOutsideAdmissionTrackedResolvedInside() noexcept {
  AddNativeTelemetryCounter(
      g_counters.productionOutsideAdmissionTrackedResolvedInside,
      t_nativeTelemetryDelta.productionOutsideAdmissionTrackedResolvedInside);
}

void RecordNativeOutsideAdmissionUntrackedResolvedOutside() noexcept {
  AddNativeTelemetryCounter(
      g_counters.productionOutsideAdmissionUntrackedResolvedOutside,
      t_nativeTelemetryDelta.productionOutsideAdmissionUntrackedResolvedOutside);
}

void RecordNativeOutsideAdmissionReject(
    NativeOutsideUploadRejectReason reason, bool entryHadPoison,
    bool lifecycleExcluded = false) noexcept {
  const size_t reasonIndex = static_cast<size_t>(reason);
  // Unknown is a deliberate hard-zero sentinel. An invalid producer outcome
  // stays uncounted so the accepted+reject closure fails visibly instead of
  // laundering a new terminal path through an ambiguous bucket.
  if (reasonIndex ==
          static_cast<size_t>(NativeOutsideUploadRejectReason::Unknown) ||
      reasonIndex >= kNativeOutsideUploadRejectReasonCount) {
    return;
  }

  NativeTelemetryDelta& telemetry = t_nativeTelemetryDelta;
  if (entryHadPoison) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsideAdmissionRejectWithPoison[reasonIndex],
        telemetry.productionOutsideAdmissionRejectWithPoison[reasonIndex]);
  } else {
    AddNativeTelemetryCounter(
        g_counters.productionOutsideAdmissionRejectNoPoison[reasonIndex],
        telemetry.productionOutsideAdmissionRejectNoPoison[reasonIndex]);
  }
  if (lifecycleExcluded) {
    RecordNativeOutsideAdmissionLifecycleExcluded();
  }
}

bool TryBatchNativeOutsideFastTelemetry() noexcept {
  NativeTelemetryDelta& telemetry = t_nativeTelemetryDelta;
  if (!telemetry.enabled || telemetry.flushing)
    return false;

  constexpr uint64_t maxValue = std::numeric_limits<uint64_t>::max();
  constexpr uint64_t addCount = 11u;
  if (telemetry.batchedAdds > maxValue - addCount ||
      telemetry.productionFastRejectScope == maxValue ||
      telemetry.productionFastRejectUnknownState == maxValue ||
      telemetry.productionOutsideCallbacksSkipped == maxValue ||
      telemetry.productionOutsideNativeFastPath == maxValue ||
      telemetry.uploads == maxValue ||
      telemetry.uploadsOutsideDispatch == maxValue ||
      telemetry.originalUploadCalls == maxValue ||
      telemetry.inputPreflightRejectedUploads == maxValue ||
      telemetry.formatHistogram[7] == maxValue ||
      telemetry.skinModeHistogram[7] == maxValue ||
      telemetry.uploadFanoutZero == maxValue) {
    FaultNativeTelemetryDelta();
    return false;
  }

  PublishNativeTelemetryDeltaPending();
  ++telemetry.productionFastRejectScope;
  ++telemetry.productionFastRejectUnknownState;
  ++telemetry.productionOutsideCallbacksSkipped;
  ++telemetry.productionOutsideNativeFastPath;
  ++telemetry.uploads;
  ++telemetry.uploadsOutsideDispatch;
  ++telemetry.originalUploadCalls;
  ++telemetry.inputPreflightRejectedUploads;
  ++telemetry.formatHistogram[7];
  ++telemetry.skinModeHistogram[7];
  ++telemetry.uploadFanoutZero;
  telemetry.batchedAdds += addCount;
  return true;
}

void RecordNativeOutsideFastTelemetry() noexcept {
  if (TryBatchNativeOutsideFastTelemetry())
    return;
  g_counters.productionFastRejectScope.fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.productionFastRejectUnknownState.fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.productionOutsideCallbacksSkipped.fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.productionOutsideNativeFastPath.fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.uploads.fetch_add(1u, std::memory_order_relaxed);
  g_counters.uploadsOutsideDispatch.fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.originalUploadCalls.fetch_add(1u, std::memory_order_relaxed);
  g_counters.inputPreflightRejectedUploads.fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.formatHistogram[7].fetch_add(1u, std::memory_order_relaxed);
  g_counters.skinModeHistogram[7].fetch_add(1u, std::memory_order_relaxed);
  g_counters.uploadFanoutZero.fetch_add(1u, std::memory_order_relaxed);
}

bool TryBatchNativeOutsideFastKernelTelemetry() noexcept {
  NativeTelemetryDelta& telemetry = t_nativeTelemetryDelta;
  if (!telemetry.enabled || telemetry.flushing)
    return false;

  constexpr uint64_t maxValue = std::numeric_limits<uint64_t>::max();
  constexpr uint64_t addCount = 2u;
  if (telemetry.batchedAdds > maxValue - addCount ||
      telemetry.kernelHookCalls == maxValue ||
      telemetry.originalKernelCalls == maxValue) {
    FaultNativeTelemetryDelta();
    return false;
  }

  PublishNativeTelemetryDeltaPending();
  ++telemetry.kernelHookCalls;
  ++telemetry.originalKernelCalls;
  telemetry.batchedAdds += addCount;
  return true;
}

void RecordNativeOutsideFastKernelTelemetry() noexcept {
  if (TryBatchNativeOutsideFastKernelTelemetry())
    return;
  g_counters.kernelHookCalls.fetch_add(1u, std::memory_order_relaxed);
  g_counters.originalKernelCalls.fetch_add(1u, std::memory_order_relaxed);
}

enum class NativeOutsideDipFastCover : uint8_t {
  Flush = 0,
  Observer,
  Reader,
};

bool TryBatchNativeOutsideDipFastTelemetry(
    NativeOutsideDipFastCover cover) noexcept {
  NativeTelemetryDelta& telemetry = t_nativeTelemetryDelta;
  if (!telemetry.enabled || telemetry.flushing)
    return false;

  constexpr uint64_t maxValue = std::numeric_limits<uint64_t>::max();
  constexpr uint64_t addCount = 5u;
  uint64_t* coverDelta = nullptr;
  switch (cover) {
    case NativeOutsideDipFastCover::Flush:
      coverDelta = &telemetry.productionOutsideDipFastByFlush;
      break;
    case NativeOutsideDipFastCover::Observer:
      coverDelta = &telemetry.productionOutsideDipFastByObserver;
      break;
    case NativeOutsideDipFastCover::Reader:
      coverDelta = &telemetry.productionOutsideDipFastByReader;
      break;
  }
  if (coverDelta == nullptr)
    return false;
  if (telemetry.batchedAdds > maxValue - addCount ||
      telemetry.dips == maxValue ||
      telemetry.outsideDispatchDips == maxValue ||
      telemetry.unmatchedDips == maxValue ||
      telemetry.productionOutsideDipFastPath == maxValue ||
      *coverDelta == maxValue) {
    FaultNativeTelemetryDelta();
    return false;
  }

  PublishNativeTelemetryDeltaPending();
  ++telemetry.dips;
  ++telemetry.outsideDispatchDips;
  ++telemetry.unmatchedDips;
  ++telemetry.productionOutsideDipFastPath;
  ++*coverDelta;
  telemetry.batchedAdds += addCount;
  return true;
}

void RecordNativeOutsideDipFastTelemetry(
    NativeOutsideDipFastCover cover) noexcept {
  if (TryBatchNativeOutsideDipFastTelemetry(cover))
    return;
  g_counters.dips.fetch_add(1u, std::memory_order_relaxed);
  g_counters.outsideDispatchDips.fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.unmatchedDips.fetch_add(1u, std::memory_order_relaxed);
  g_counters.productionOutsideDipFastPath.fetch_add(
      1u, std::memory_order_relaxed);
  switch (cover) {
    case NativeOutsideDipFastCover::Flush:
      g_counters.productionOutsideDipFastByFlush.fetch_add(
          1u, std::memory_order_relaxed);
      break;
    case NativeOutsideDipFastCover::Observer:
      g_counters.productionOutsideDipFastByObserver.fetch_add(
          1u, std::memory_order_relaxed);
      break;
    case NativeOutsideDipFastCover::Reader:
      g_counters.productionOutsideDipFastByReader.fetch_add(
          1u, std::memory_order_relaxed);
      break;
  }
}

bool TryBatchNativeProductionFastRejectKernelTelemetry() noexcept {
  NativeTelemetryDelta& telemetry = t_nativeTelemetryDelta;
  if (!telemetry.enabled || telemetry.flushing)
    return false;

  constexpr uint64_t maxValue = std::numeric_limits<uint64_t>::max();
  constexpr uint64_t addCount = 3u;
  if (telemetry.batchedAdds > maxValue - addCount ||
      telemetry.productionFastRejectKernelBatches == maxValue ||
      telemetry.kernelHookCalls == maxValue ||
      telemetry.originalKernelCalls == maxValue) {
    FaultNativeTelemetryDelta();
    return false;
  }

  PublishNativeTelemetryDeltaPending();
  ++telemetry.productionFastRejectKernelBatches;
  ++telemetry.kernelHookCalls;
  ++telemetry.originalKernelCalls;
  telemetry.batchedAdds += addCount;
  return true;
}

void RecordNativeProductionFastRejectKernelTelemetry() noexcept {
  if (TryBatchNativeProductionFastRejectKernelTelemetry())
    return;
  // The batch marker counts successful TLS bundles only. Atomic fallback keeps
  // the two historical public totals exact without pretending it was batched.
  g_counters.kernelHookCalls.fetch_add(1u, std::memory_order_relaxed);
  g_counters.originalKernelCalls.fetch_add(1u, std::memory_order_relaxed);
}

bool FlushNativeTelemetryDeltaForCurrentThread() noexcept {
  NativeTelemetryDelta& telemetry = t_nativeTelemetryDelta;
  const uint64_t currentThread = ::GetCurrentThreadId();
  const uint64_t observedThread =
      g_observedRenderThreadId.load(std::memory_order_acquire);

  if (!telemetry.dirty) {
    // A pending bit without this TLS delta belongs to a previous/migrated
    // thread. Never let the new thread clear it merely because a Windows thread
    // id was reused or NotifyNativeFlush moved to another thread.
    if (g_nativeTelemetryDeltaPending.load(std::memory_order_acquire) != 0u) {
      FaultNativeTelemetryDelta();
      return false;
    }
    return true;
  }

  if (telemetry.ownerThreadId == 0u ||
      telemetry.ownerThreadId != currentThread ||
      observedThread != currentThread) {
    FaultNativeTelemetryDelta();
    return false;
  }

  // pending remains release-visible as 1 for the whole merge. Each source is
  // cleared only after its aggregate RMW; pending=0 is the final publication.
  telemetry.flushing = true;
#define FLUSH_NATIVE_TELEMETRY_FIELD(name)                                  \
  do {                                                                      \
    if (telemetry.name != 0u) {                                             \
      g_counters.name.fetch_add(telemetry.name, std::memory_order_relaxed); \
      telemetry.name = 0u;                                                  \
    }                                                                       \
  } while (false);
  DXVK_NATIVE_TELEMETRY_SCALAR_FIELDS(FLUSH_NATIVE_TELEMETRY_FIELD)
#undef FLUSH_NATIVE_TELEMETRY_FIELD
  for (size_t i = 0u; i < telemetry.formatHistogram.size(); ++i) {
    if (telemetry.formatHistogram[i] != 0u) {
      g_counters.formatHistogram[i].fetch_add(
          telemetry.formatHistogram[i], std::memory_order_relaxed);
      telemetry.formatHistogram[i] = 0u;
    }
    if (telemetry.skinModeHistogram[i] != 0u) {
      g_counters.skinModeHistogram[i].fetch_add(
          telemetry.skinModeHistogram[i], std::memory_order_relaxed);
      telemetry.skinModeHistogram[i] = 0u;
    }
  }
  for (size_t i = 0u;
       i < telemetry.productionOutsideAdmissionRejectNoPoison.size(); ++i) {
    if (telemetry.productionOutsideAdmissionRejectNoPoison[i] != 0u) {
      g_counters.productionOutsideAdmissionRejectNoPoison[i].fetch_add(
          telemetry.productionOutsideAdmissionRejectNoPoison[i],
          std::memory_order_relaxed);
      telemetry.productionOutsideAdmissionRejectNoPoison[i] = 0u;
    }
    if (telemetry.productionOutsideAdmissionRejectWithPoison[i] != 0u) {
      g_counters.productionOutsideAdmissionRejectWithPoison[i].fetch_add(
          telemetry.productionOutsideAdmissionRejectWithPoison[i],
          std::memory_order_relaxed);
      telemetry.productionOutsideAdmissionRejectWithPoison[i] = 0u;
    }
  }
  g_counters.telemetryBatchedAdds.fetch_add(
      telemetry.batchedAdds, std::memory_order_relaxed);
  // This completion sequence pairs with the reader's acquire double-check.
  g_counters.telemetryFlushes.fetch_add(1u, std::memory_order_release);
  telemetry.batchedAdds = 0u;
  telemetry.dirty = false;
  telemetry.flushing = false;
  if (g_nativeTelemetryDeltaFaulted.load(std::memory_order_acquire))
    telemetry.enabled = false;
  g_nativeTelemetryDeltaPending.store(0u, std::memory_order_release);
  return true;
}

void ConfigureNativeTelemetryAtFlush(uint64_t currentThread) noexcept {
  NativeTelemetryDelta& telemetry = t_nativeTelemetryDelta;
  const uint64_t observedThread =
      g_observedRenderThreadId.load(std::memory_order_acquire);
  if (observedThread == 0u || observedThread != currentThread) {
    // NotifyNativeFlush is the render-thread identity boundary. Once it moves,
    // this TLS cannot prove ownership and all future additions stay atomic.
    if (observedThread != 0u)
      FaultNativeTelemetryDelta();
    else
      telemetry.enabled = false;
    return;
  }

  if (telemetry.ownerThreadId == 0u) {
    if (g_nativeTelemetryDeltaPending.load(std::memory_order_acquire) != 0u) {
      FaultNativeTelemetryDelta();
      return;
    }
    telemetry.ownerThreadId = currentThread;
  } else if (telemetry.ownerThreadId != currentThread) {
    FaultNativeTelemetryDelta();
    return;
  }

  telemetry.enabled =
      g_runtimeConfig.mode == GpuSkinMode::Bypass &&
      !g_runtimeConfig.fullDiagnostics &&
      !g_nativeTelemetryDeltaFaulted.load(std::memory_order_acquire);
}

#undef DXVK_NATIVE_TELEMETRY_SCALAR_FIELDS

NativeBridgeResetResult ProcessPendingBridgeReset();
void ApplyPendingResetFailClosed();

bool CurrentDipObserverCoverActive() noexcept {
  return t_dipObserverDepth != 0u && t_dipObserverCookie != 0u &&
      t_dipObserverResetGeneration == t_state.appliedResetGeneration;
}

uint64_t NextDipObserverCookie(uint64_t resetGeneration) noexcept {
  ++t_dipObserverCookieSequence;
  if (t_dipObserverCookieSequence == 0u)
    ++t_dipObserverCookieSequence;
  uint64_t cookie = t_dipObserverCookieSequence ^
      (uint64_t(::GetCurrentThreadId()) << 32u) ^
      (resetGeneration * 0x9E3779B97F4A7C15ull);
  if (cookie == 0u)
    cookie = 0xD1B54A32D192ED03ull ^ t_dipObserverCookieSequence;
  return cookie != 0u ? cookie : 1u;
}

void AcquireIndexSnapshotStorage(NativeIndexSnapshotState& state) {
  // One render thread owns both the in-flight and active ticket. Recycle the
  // largest INDEX16 allocation instead of reallocating it for every upload.
  state.uploadEpoch = 0u;
  state.ticketGeneration = 0u;
  state.bytes.clear();
  if (t_state.spareIndexSnapshotBytes.capacity() > state.bytes.capacity())
    state.bytes.swap(t_state.spareIndexSnapshotBytes);
  state.bytes.clear();
  t_state.spareIndexSnapshotBytes.clear();
}

void RecycleIndexSnapshotStorage(NativeIndexSnapshotState& state) {
  state.uploadEpoch = 0u;
  state.ticketGeneration = 0u;
  state.bytes.clear();
  if (state.bytes.capacity() >
      t_state.spareIndexSnapshotBytes.capacity()) {
    state.bytes.swap(t_state.spareIndexSnapshotBytes);
  }
  state.bytes.clear();
  t_state.spareIndexSnapshotBytes.clear();
}

bool ReadModulePath(uintptr_t gameBase, char (&path)[MAX_PATH]) {
  const DWORD length = ::GetModuleFileNameA(
      reinterpret_cast<HMODULE>(gameBase), path, MAX_PATH);
  return length != 0u && length < MAX_PATH;
}

bool QueryFileVersion(const char* path, uint32_t (&version)[4]) {
  HMODULE module = ::LoadLibraryA("version.dll");
  if (module == nullptr)
    return false;

  using GetFileVersionInfoSizeAFn = DWORD(WINAPI*)(LPCSTR, LPDWORD);
  using GetFileVersionInfoAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID);
  using VerQueryValueAFn = BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT);

  const auto getSize = reinterpret_cast<GetFileVersionInfoSizeAFn>(
      ::GetProcAddress(module, "GetFileVersionInfoSizeA"));
  const auto getInfo = reinterpret_cast<GetFileVersionInfoAFn>(
      ::GetProcAddress(module, "GetFileVersionInfoA"));
  const auto query = reinterpret_cast<VerQueryValueAFn>(
      ::GetProcAddress(module, "VerQueryValueA"));
  if (getSize == nullptr || getInfo == nullptr || query == nullptr) {
    ::FreeLibrary(module);
    return false;
  }

  DWORD ignored = 0;
  const DWORD byteCount = getSize(path, &ignored);
  if (byteCount == 0u) {
    ::FreeLibrary(module);
    return false;
  }

  std::vector<uint8_t> bytes(byteCount);
  if (!getInfo(path, ignored, byteCount, bytes.data())) {
    ::FreeLibrary(module);
    return false;
  }

  VS_FIXEDFILEINFO* fixed = nullptr;
  UINT fixedSize = 0;
  const bool ok =
      query(bytes.data(), "\\", reinterpret_cast<void**>(&fixed), &fixedSize) &&
      fixed != nullptr && fixedSize >= sizeof(VS_FIXEDFILEINFO) &&
      fixed->dwSignature == 0xFEEF04BDu;
  if (ok) {
    version[0] = HIWORD(fixed->dwFileVersionMS);
    version[1] = LOWORD(fixed->dwFileVersionMS);
    version[2] = HIWORD(fixed->dwFileVersionLS);
    version[3] = LOWORD(fixed->dwFileVersionLS);
  }

  ::FreeLibrary(module);
  return ok;
}

bool HashFileMd5Sha1(const char* path,
                     uint64_t& fileSize,
                     uint8_t (&md5)[16],
                     uint8_t (&sha1)[20]) {
  HMODULE module = ::LoadLibraryA("advapi32.dll");
  if (module == nullptr)
    return false;

  using AcquireFn = BOOL(WINAPI*)(HCRYPTPROV*, LPCSTR, LPCSTR, DWORD, DWORD);
  using CreateHashFn = BOOL(WINAPI*)(HCRYPTPROV, ALG_ID, HCRYPTKEY, DWORD,
                                     HCRYPTHASH*);
  using HashDataFn = BOOL(WINAPI*)(HCRYPTHASH, const BYTE*, DWORD, DWORD);
  using GetHashFn = BOOL(WINAPI*)(HCRYPTHASH, DWORD, BYTE*, DWORD*, DWORD);
  using DestroyHashFn = BOOL(WINAPI*)(HCRYPTHASH);
  using ReleaseFn = BOOL(WINAPI*)(HCRYPTPROV, DWORD);

  const auto acquire = reinterpret_cast<AcquireFn>(
      ::GetProcAddress(module, "CryptAcquireContextA"));
  const auto createHash = reinterpret_cast<CreateHashFn>(
      ::GetProcAddress(module, "CryptCreateHash"));
  const auto hashData = reinterpret_cast<HashDataFn>(
      ::GetProcAddress(module, "CryptHashData"));
  const auto getHash = reinterpret_cast<GetHashFn>(
      ::GetProcAddress(module, "CryptGetHashParam"));
  const auto destroyHash = reinterpret_cast<DestroyHashFn>(
      ::GetProcAddress(module, "CryptDestroyHash"));
  const auto release = reinterpret_cast<ReleaseFn>(
      ::GetProcAddress(module, "CryptReleaseContext"));
  if (acquire == nullptr || createHash == nullptr || hashData == nullptr ||
      getHash == nullptr || destroyHash == nullptr || release == nullptr) {
    ::FreeLibrary(module);
    return false;
  }

  HANDLE file = ::CreateFileA(path, GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    ::FreeLibrary(module);
    return false;
  }

  LARGE_INTEGER size = {};
  HCRYPTPROV provider = 0;
  HCRYPTHASH md5Hash = 0;
  HCRYPTHASH sha1Hash = 0;
  bool ok = ::GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
      acquire(&provider, nullptr, nullptr, PROV_RSA_FULL,
              CRYPT_VERIFYCONTEXT | CRYPT_SILENT) &&
      createHash(provider, CALG_MD5, 0, 0, &md5Hash) &&
      createHash(provider, CALG_SHA1, 0, 0, &sha1Hash);

  std::array<uint8_t, 64u * 1024u> buffer = {};
  while (ok) {
    DWORD bytesRead = 0;
    if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                    &bytesRead, nullptr)) {
      ok = false;
      break;
    }
    if (bytesRead == 0u)
      break;
    ok = hashData(md5Hash, buffer.data(), bytesRead, 0) &&
         hashData(sha1Hash, buffer.data(), bytesRead, 0);
  }

  if (ok) {
    DWORD md5Size = sizeof(md5);
    DWORD sha1Size = sizeof(sha1);
    ok = getHash(md5Hash, HP_HASHVAL, md5, &md5Size, 0) &&
         md5Size == sizeof(md5) &&
         getHash(sha1Hash, HP_HASHVAL, sha1, &sha1Size, 0) &&
         sha1Size == sizeof(sha1);
  }

  fileSize = ok ? static_cast<uint64_t>(size.QuadPart) : 0u;
  if (sha1Hash != 0)
    destroyHash(sha1Hash);
  if (md5Hash != 0)
    destroyHash(md5Hash);
  if (provider != 0)
    release(provider, 0);
  ::CloseHandle(file);
  ::FreeLibrary(module);
  return ok;
}

template<size_t N>
bool MatchBytes(uintptr_t address, const std::array<uint8_t, N>& expected) {
  return dxvk::war3::SafeEqual(reinterpret_cast<const void*>(address),
                               expected.data(), N);
}

bool MatchRel32Call(uintptr_t callAddress, uintptr_t expectedTarget) {
  std::array<uint8_t, 5> bytes = {};
  if (!dxvk::war3::SafeCopy(bytes.data(),
                            reinterpret_cast<const void*>(callAddress),
                            bytes.size()))
    return false;
  if (bytes[0] != 0xE8u)
    return false;
  int32_t displacement = 0;
  std::memcpy(&displacement, bytes.data() + 1u, sizeof(displacement));
  return callAddress + bytes.size() + displacement == expectedTarget;
}

uint32_t ValidateOpcodeFingerprint(uintptr_t gameBase) {
  // IDA ASM 0x6F138EE0: whole-function fastcall prologue and ret 4.
  constexpr std::array<uint8_t, 17> applyPrefix = {
      0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57, 0x8B, 0xFA, 0x8B,
      0xF1, 0x8B, 0x0F, 0xE8, 0xDF, 0xA9, 0xFA, 0xFF};
  constexpr std::array<uint8_t, 7> applyTail = {
      0x5F, 0x5E, 0x5B, 0x5D, 0xC2, 0x04, 0x00};

  // IDA ASM 0x6F0EEA50: EH4 prologue and 13-stack-argument ret 34h.
  constexpr std::array<uint8_t, 5> uploadPrefix = {
      0x55, 0x8B, 0xEC, 0x6A, 0xFE};
  constexpr std::array<uint8_t, 13> uploadEh4 = {
      0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50,
      0x83, 0xEC, 0x0C, 0x53, 0x56, 0x57};
  constexpr std::array<uint8_t, 10> uploadTail = {
      0x59, 0x5F, 0x5E, 0x5B, 0x8B, 0xE5, 0x5D, 0xC2, 0x34, 0x00};

  // IDA ASM 0x6F0EDDC0: void thiscall(self, mappedDst), ret 4.
  constexpr std::array<uint8_t, 24> kernelPrefix = {
      0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C, 0x8B, 0x91,
      0x90, 0x00, 0x00, 0x00, 0x89, 0x4D, 0xFC, 0x53,
      0x8B, 0x5D, 0x08, 0xF6, 0xC2, 0x06, 0x74, 0x76};
  constexpr std::array<uint8_t, 9> kernelTail = {
      0x5F, 0x5E, 0x5B, 0x8B, 0xE5, 0x5D, 0xC2, 0x04, 0x00};

  std::array<uint8_t, 8> flushPrefix = {
      0x55, 0x8B, 0xEC, 0xA1, 0x00, 0x00, 0x00, 0x00};
  constexpr std::array<uint8_t, 18> dispatchPrefix = {
      0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x3C, 0x53, 0x56, 0x8B,
      0x72, 0x0C, 0x8B, 0xD9, 0x57, 0x56, 0x89, 0x75, 0xF8};

  bool flushAddressValid =
      gameBase <= std::numeric_limits<uint32_t>::max() -
          kRenderQueueNumOfElementsRva;
  if (flushAddressValid) {
    const uint32_t relocatedNumOfElements =
        static_cast<uint32_t>(gameBase + kRenderQueueNumOfElementsRva);
    std::memcpy(flushPrefix.data() + 4u, &relocatedNumOfElements,
                sizeof(relocatedNumOfElements));
  }

  uint32_t failureMask = NativeOpcodeFailureNone;
  auto record = [&failureMask](bool valid,
                               NativeOpcodeFingerprintFailure failure) {
    if (!valid)
      failureMask |= failure;
  };
  record(MatchBytes(gameBase + kApplyDrawStateAndSamplerPairRva, applyPrefix),
         NativeOpcodeFailureApplyPrefix);
  record(MatchBytes(gameBase + kApplyDrawStateAndSamplerPairRva + 0x7Au,
                    applyTail),
         NativeOpcodeFailureApplyTail);
  record(MatchBytes(gameBase + kGxDeviceD3dDynamicVertexUploadRva,
                    uploadPrefix),
         NativeOpcodeFailureUploadPrefix);
  record(MatchBytes(gameBase + kGxDeviceD3dDynamicVertexUploadRva + 0x0Fu,
                    uploadEh4),
         NativeOpcodeFailureUploadEh4);
  record(MatchBytes(gameBase + kGxDeviceD3dDynamicVertexUploadRva + 0x1C0u,
                    uploadTail),
         NativeOpcodeFailureUploadTail);
  record(MatchBytes(gameBase + kGxDeviceD3dSkinCopyKernelRva, kernelPrefix),
         NativeOpcodeFailureKernelPrefix);
  record(MatchBytes(gameBase + kGxDeviceD3dSkinCopyKernelRva + 0x428u,
                    kernelTail),
         NativeOpcodeFailureKernelTail);
  record(MatchRel32Call(gameBase + kGxDeviceD3dLockVertexRingCallRva,
                        gameBase + kGxDeviceD3dLockVertexRingRva),
         NativeOpcodeFailureLockVertexRingCall);
  record(MatchRel32Call(gameBase + kGxDeviceD3dSkinCopyKernelCallRva,
                        gameBase + kGxDeviceD3dSkinCopyKernelRva),
         NativeOpcodeFailureSkinCopyKernelCall);
  record(flushAddressValid &&
             MatchBytes(gameBase + kFlushSortedItemsRva, flushPrefix),
         NativeOpcodeFailureFlushPrefix);
  record(MatchBytes(gameBase + kDispatchCommonRva, dispatchPrefix),
         NativeOpcodeFailureDispatchPrefix);
  return failureMask;
}

bool ValidateVtableFingerprint(uintptr_t gameBase) {
  uint32_t upload = 0;
  uint32_t index = 0;
  uint32_t flush = 0;
  const uintptr_t vtable = gameBase + kGxDeviceD3dVtableRva;
  if (!dxvk::war3::SafeCopy(
          &upload, reinterpret_cast<const void*>(vtable + 0x68u),
          sizeof(upload)) ||
      !dxvk::war3::SafeCopy(
          &index, reinterpret_cast<const void*>(vtable + 0x6Cu),
          sizeof(index)) ||
      !dxvk::war3::SafeCopy(
          &flush, reinterpret_cast<const void*>(vtable + 0x70u),
          sizeof(flush))) {
    return false;
  }
  return upload == static_cast<uint32_t>(gameBase + kGxDeviceD3dDynamicVertexUploadRva) &&
         index == static_cast<uint32_t>(gameBase + kGxDeviceD3dIndexUploadRva) &&
         flush == static_cast<uint32_t>(gameBase + kGxDeviceD3dFlushPrimitiveBatchRva) &&
         MatchRel32Call(gameBase + kGxDeviceD3dSkinCopyKernelCallRva,
                        gameBase + kGxDeviceD3dSkinCopyKernelRva);
}

NativeBridgeFingerprint ValidateFingerprint(
    uintptr_t gameBase, const NativeBridgeAddresses& addresses) {
  NativeBridgeFingerprint result = {};
  result.loadedImageBase = gameBase;
  if (addresses.flushSortedItemsRva != kFlushSortedItemsRva ||
      addresses.dispatchCommonRva != kDispatchCommonRva ||
      addresses.applyDrawStateAndSamplerPairRva !=
          kApplyDrawStateAndSamplerPairRva ||
       addresses.gxDeviceD3dDynamicVertexUploadRva !=
           kGxDeviceD3dDynamicVertexUploadRva ||
       addresses.gxDeviceD3dSkinCopyKernelRva !=
           kGxDeviceD3dSkinCopyKernelRva) {
    result.failureMask |= NativeFingerprintFailureAddressBook;
  }

  IMAGE_DOS_HEADER dos = {};
  const void* dosAddress = reinterpret_cast<const void*>(gameBase);
  if (dxvk::war3::SafeCopy(&dos, dosAddress, sizeof(dos))) {
    result.peValidMask |= NativePeFingerprintDosReadable;
    result.observedPeOffset = static_cast<uint32_t>(dos.e_lfanew);
    if (dos.e_magic == IMAGE_DOS_SIGNATURE)
      result.peValidMask |= NativePeFingerprintDosSignature;
    if (dos.e_lfanew == static_cast<LONG>(kExpectedPeOffset) &&
        gameBase <= std::numeric_limits<uintptr_t>::max() -
            kExpectedPeOffset) {
      result.peValidMask |= NativePeFingerprintPeOffset;
    }
  }

  IMAGE_NT_HEADERS32 nt = {};
  const uint32_t ntPrerequisites = NativePeFingerprintDosReadable |
      NativePeFingerprintDosSignature |
      NativePeFingerprintPeOffset;
  if ((result.peValidMask & ntPrerequisites) == ntPrerequisites) {
    const void* ntAddress = reinterpret_cast<const void*>(
        gameBase + kExpectedPeOffset);
    if (dxvk::war3::SafeCopy(&nt, ntAddress, sizeof(nt))) {
      result.peValidMask |= NativePeFingerprintNtReadable;
      result.observedNtSignature = nt.Signature;
      result.observedMachine = nt.FileHeader.Machine;
      result.observedOptionalMagic = nt.OptionalHeader.Magic;
      result.observedImageBase = nt.OptionalHeader.ImageBase;
      result.peTimestamp = nt.FileHeader.TimeDateStamp;
      result.imageSize = nt.OptionalHeader.SizeOfImage;
      result.imageChecksum = nt.OptionalHeader.CheckSum;

      if (result.observedNtSignature == IMAGE_NT_SIGNATURE)
        result.peValidMask |= NativePeFingerprintNtSignature;
      if (result.observedMachine == IMAGE_FILE_MACHINE_I386)
        result.peValidMask |= NativePeFingerprintMachine;
      if (result.observedOptionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        result.peValidMask |= NativePeFingerprintOptionalMagic;
      if (result.observedImageBase == kExpectedImageBase) {
        result.peValidMask |= NativePeFingerprintImageBase |
            NativePeFingerprintImageBasePreferred;
      } else if (gameBase <= std::numeric_limits<uint32_t>::max() &&
                 result.observedImageBase == static_cast<uint32_t>(gameBase)) {
        // The exact file hash binds the raw PE header to kExpectedImageBase.
        // DYNAMIC_BASE loaders may normalize the mapped header to the actual
        // module base; only that one exact runtime value is also accepted.
        result.peValidMask |= NativePeFingerprintImageBase |
            NativePeFingerprintImageBaseRuntimeRelocated;
      }
      if (result.peTimestamp == kExpectedPeTimestamp)
        result.peValidMask |= NativePeFingerprintTimestamp;
      if (result.imageSize == kExpectedImageSize)
        result.peValidMask |= NativePeFingerprintImageSize;
      if (result.imageChecksum == kExpectedImageChecksum)
        result.peValidMask |= NativePeFingerprintChecksum;
    }
  }
  result.peFailureMask =
      kNativePeFingerprintRequiredMask & ~result.peValidMask;
  if (result.peFailureMask != 0u)
    result.failureMask |= NativeFingerprintFailurePeImage;

  char path[MAX_PATH] = {};
  const bool hasPath = ReadModulePath(gameBase, path);
  if (!hasPath || !QueryFileVersion(path, result.fileVersion) ||
      std::memcmp(result.fileVersion, kExpectedFileVersion,
                  sizeof(kExpectedFileVersion)) != 0) {
    result.failureMask |= NativeFingerprintFailureVersion;
  }

  if (!hasPath ||
      !HashFileMd5Sha1(path, result.fileSize, result.md5, result.sha1) ||
      result.fileSize != kExpectedFileSize ||
      std::memcmp(result.md5, kExpectedMd5.data(), kExpectedMd5.size()) != 0 ||
      std::memcmp(result.sha1, kExpectedSha1.data(), kExpectedSha1.size()) != 0) {
    result.failureMask |= NativeFingerprintFailureFileHash;
  }

  result.opcodeFailureMask = ValidateOpcodeFingerprint(gameBase);
  if (result.opcodeFailureMask != NativeOpcodeFailureNone)
    result.failureMask |= NativeFingerprintFailureOpcodes;
  if (!ValidateVtableFingerprint(gameBase))
    result.failureMask |= NativeFingerprintFailureVtable;

  result.exactMatch = result.failureMask == NativeFingerprintFailureNone;
  return result;
}

bool SemanticScopeHazardActive() {
  return t_state.semanticOverflowDepth != 0u ||
      (t_state.semanticDepth != 0u &&
       t_state.semanticFrames[t_state.semanticDepth - 1u].failClosed);
}

bool ScopeHazardActive() {
  return t_state.dispatchOverflowDepth != 0u ||
      SemanticScopeHazardActive() ||
      (t_state.dispatchDepth != 0u &&
       t_state.dispatchFrames[t_state.dispatchDepth - 1u]
           .observation.failClosed);
}

DispatchFrame* CurrentDispatch() {
  if (t_state.dispatchOverflowDepth != 0u ||
      SemanticScopeHazardActive() || t_state.dispatchDepth == 0u) {
    return nullptr;
  }
  return &t_state.dispatchFrames[t_state.dispatchDepth - 1u];
}

ActiveUploadState* CurrentBypassedIndexUpload() {
  DispatchFrame* dispatch = CurrentDispatch();
  if (dispatch == nullptr || !dispatch->activeUpload.valid ||
      !dispatch->activeUpload.observation.cpuSkinKernelBypassed) {
    return nullptr;
  }
  return &dispatch->activeUpload;
}

SemanticFrame* CurrentSemantic() {
  if (t_state.dispatchOverflowDepth != 0u ||
      t_state.semanticOverflowDepth != 0u ||
      t_state.semanticDepth == 0u) {
    return nullptr;
  }
  return &t_state.semanticFrames[t_state.semanticDepth - 1u];
}

bool CurrentThreadCallbacksAdmitted() {
  if (g_callbackIngressEnabled.load(std::memory_order_acquire))
    return true;
  if (t_state.inFlightUpload.observation != nullptr &&
      t_state.inFlightUpload.callbacksAdmitted) {
    return true;
  }
  return t_state.dispatchDepth != 0u &&
      t_state.dispatchFrames[t_state.dispatchDepth - 1u].callbacksAdmitted;
}

enum class ProductionCallbackTimingPart : uint8_t {
  PinEnter = 0,
  Body,
  PinLeave,
};

bool ProductionLightTimingEnabled() noexcept {
  return g_runtimeConfig.mode == GpuSkinMode::Bypass &&
      !g_runtimeConfig.fullDiagnostics;
}

static_assert(
    (kNativeProductionTimingSamplePeriod &
     (kNativeProductionTimingSamplePeriod - 1u)) == 0u,
    "production timing event ordinals require a power-of-two period");
static_assert(
    kNativeProductionTimingSamplePhase < kNativeProductionTimingSamplePeriod,
    "production timing phase must be inside the sampling period");
static_assert(
    kNativeOutsideDipFastEarlySamplePhase <
        kNativeProductionTimingSamplePeriod &&
    kNativeOutsideDipFastLateSamplePhase <
        kNativeProductionTimingSamplePeriod,
    "outside-DIP probe phases must be inside the sampling period");
static_assert(
    kNativeOutsideDipFastEarlySamplePhase !=
        kNativeProductionTimingSamplePhase &&
    kNativeOutsideDipFastLateSamplePhase !=
        kNativeProductionTimingSamplePhase &&
    kNativeOutsideDipFastEarlySamplePhase !=
        kNativeOutsideDipFastLateSamplePhase,
    "outside-DIP probes and production timing require distinct phases");

bool SampleNextProductionEventOrdinalAtPhase(
    uint64_t& ordinal, uint32_t phase) noexcept {
  if (!ProductionLightTimingEnabled())
    return false;
  ++ordinal;
  return (ordinal & (kNativeProductionTimingSamplePeriod - 1u)) == phase;
}

bool SampleNextProductionEventOrdinal(uint64_t& ordinal) noexcept {
  return SampleNextProductionEventOrdinalAtPhase(
      ordinal, kNativeProductionTimingSamplePhase);
}

uint64_t ProductionEventElapsedTicks(int64_t start, int64_t end) noexcept {
  return end > start ? uint64_t(end - start) : 0u;
}

class ProductionTimingWriteGuard {
public:
  explicit ProductionTimingWriteGuard(bool active) noexcept
      : m_active(active) {
    if (!m_active)
      return;
    g_productionTimingWritesStarted.fetch_add(
        1u, std::memory_order_acq_rel);
    g_productionTimingWriters.fetch_add(
        1u, std::memory_order_acq_rel);
  }

  ~ProductionTimingWriteGuard() noexcept {
    Complete();
  }

  void Complete() noexcept {
    if (!m_active)
      return;
    g_productionTimingWritesCompleted.fetch_add(
        1u, std::memory_order_release);
    g_productionTimingWriters.fetch_sub(
        1u, std::memory_order_release);
    m_active = false;
  }

  ProductionTimingWriteGuard(const ProductionTimingWriteGuard&) = delete;
  ProductionTimingWriteGuard& operator=(
      const ProductionTimingWriteGuard&) = delete;

private:
  bool m_active = false;
};

bool IsProductionTimingStableIdSampled(uint64_t stableId) noexcept {
  return stableId != 0u && ProductionLightTimingEnabled() &&
      stableId % kNativeProductionTimingSamplePeriod ==
          kNativeProductionTimingSamplePhase;
}

void RecordAtomicSampledRawTiming(
    AtomicNativeSampledRawTiming& timing, uint64_t ticks) noexcept {
  timing.calls.fetch_add(1u, std::memory_order_relaxed);
  timing.ticks.fetch_add(ticks, std::memory_order_relaxed);
  uint64_t current = timing.maxTicks.load(std::memory_order_relaxed);
  while (current < ticks &&
         !timing.maxTicks.compare_exchange_weak(
             current, ticks, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

void RecordProductionCallbackTiming(
    NativeBridgeCallbackKind kind, ProductionCallbackTimingPart part,
    uint64_t ticks) noexcept {
  const size_t index = static_cast<size_t>(kind);
  if (index >= kNativeBridgeCallbackKindCount ||
      !ProductionLightTimingEnabled()) {
    return;
  }
  switch (part) {
    case ProductionCallbackTimingPart::PinEnter:
      RecordAtomicSampledRawTiming(
          g_counters.productionCallbackPinEnterTiming[index], ticks);
      break;
    case ProductionCallbackTimingPart::Body:
      RecordAtomicSampledRawTiming(
          g_counters.productionCallbackBodyTiming[index], ticks);
      break;
    case ProductionCallbackTimingPart::PinLeave:
      RecordAtomicSampledRawTiming(
          g_counters.productionCallbackPinLeaveTiming[index], ticks);
      break;
  }
}

class ProductionCallbackTiming {
public:
  ProductionCallbackTiming(
      NativeBridgeCallbackKind kind, uint64_t stableId) noexcept
      : m_kind(kind),
        m_enabled(IsProductionTimingStableIdSampled(stableId)),
        m_writeGuard(m_enabled),
        m_pinEnterStart(m_enabled
            ? dxvk::high_resolution_clock::get_counter() : 0) {
  }

  void EndPinEnter() noexcept {
    if (!m_enabled)
      return;
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    RecordProductionCallbackTiming(
        m_kind, ProductionCallbackTimingPart::PinEnter,
        end > m_pinEnterStart ? uint64_t(end - m_pinEnterStart) : 0u);
  }

  void BeginBody() noexcept {
    if (!m_enabled)
      return;
    m_bodyStart = dxvk::high_resolution_clock::get_counter();
    m_bodyActive = true;
  }

  void EndBody() noexcept {
    if (!m_enabled || !m_bodyActive)
      return;
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    RecordProductionCallbackTiming(
        m_kind, ProductionCallbackTimingPart::Body,
        end > m_bodyStart ? uint64_t(end - m_bodyStart) : 0u);
    m_bodyActive = false;
  }

  void BeginPinLeave() noexcept {
    if (!m_enabled)
      return;
    m_pinLeaveStart = dxvk::high_resolution_clock::get_counter();
  }

  void EndPinLeave() noexcept {
    if (!m_enabled)
      return;
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    RecordProductionCallbackTiming(
        m_kind, ProductionCallbackTimingPart::PinLeave,
        end > m_pinLeaveStart ? uint64_t(end - m_pinLeaveStart) : 0u);
  }

  bool enabled() const noexcept {
    return m_enabled;
  }

  void CompleteWrite() noexcept {
    m_writeGuard.Complete();
  }

private:
  NativeBridgeCallbackKind m_kind;
  bool m_enabled = false;
  ProductionTimingWriteGuard m_writeGuard;
  bool m_bodyActive = false;
  int64_t m_pinEnterStart = 0;
  int64_t m_bodyStart = 0;
  int64_t m_pinLeaveStart = 0;
};

class NativeBridgeCallbackPin {
public:
  NativeBridgeCallbackPin(
      NativeBridgeCallbackKind kind, uint64_t stableId) noexcept
      : m_timing(kind, stableId) {
    if (!CurrentThreadCallbacksAdmitted() ||
        g_callbacks.load(std::memory_order_acquire) == nullptr) {
      m_timing.EndPinEnter();
      return;
    }

    // Increment before the authoritative reload. An unregister CAS may race
    // the optimistic load above, but it cannot retire the table while a pin
    // that can still observe it is active.
    g_activeCallbackPins.fetch_add(1u, std::memory_order_seq_cst);
    const NativeBridgeCallbacks* callbacks =
        g_callbacks.load(std::memory_order_seq_cst);
    if (callbacks == nullptr || !CurrentThreadCallbacksAdmitted()) {
      g_activeCallbackPins.fetch_sub(1u, std::memory_order_seq_cst);
      m_timing.EndPinEnter();
      return;
    }

    m_callbacks = callbacks;
    ++t_callbackPinDepth;
    m_timing.EndPinEnter();
  }

  ~NativeBridgeCallbackPin() {
    if (m_callbacks == nullptr) {
      m_timing.BeginPinLeave();
      m_timing.EndPinLeave();
      m_timing.CompleteWrite();
    } else {
      // A sampled leave must publish before quiescence can observe the final
      // pin as zero. Keep one temporary sentinel pin live, time the original
      // decrement, publish its ticks, then release the sentinel.
      const bool sampledLeave = m_timing.enabled();
      if (sampledLeave) {
        g_activeCallbackPins.fetch_add(1u, std::memory_order_seq_cst);
      }
      m_timing.BeginPinLeave();
      --t_callbackPinDepth;
      g_activeCallbackPins.fetch_sub(1u, std::memory_order_seq_cst);
      m_timing.EndPinLeave();
      m_timing.CompleteWrite();
      if (sampledLeave) {
        g_activeCallbackPins.fetch_sub(1u, std::memory_order_seq_cst);
      }
    }
  }

  NativeBridgeCallbackPin(const NativeBridgeCallbackPin&) = delete;
  NativeBridgeCallbackPin& operator=(const NativeBridgeCallbackPin&) = delete;

  const NativeBridgeCallbacks* get() const {
    return m_callbacks;
  }

  void BeginCallbackBody() noexcept {
    m_timing.BeginBody();
  }

  void EndCallbackBody() noexcept {
    m_timing.EndBody();
  }

private:
  ProductionCallbackTiming m_timing;
  const NativeBridgeCallbacks* m_callbacks = nullptr;
};

DispatchFrame* FindDispatchFrameByEpoch(uint64_t dispatchEpoch) noexcept {
  if (dispatchEpoch == 0u)
    return nullptr;
  const size_t depth = std::min(
      t_state.dispatchDepth, t_state.dispatchFrames.size());
  for (size_t i = depth; i != 0u; --i) {
    DispatchFrame& frame = t_state.dispatchFrames[i - 1u];
    if (frame.observation.epoch.dispatchEpoch == dispatchEpoch)
      return &frame;
  }
  return nullptr;
}

bool SameNativeDispatchObservationExact(
    const NativeDispatchObservation& lhs,
    const NativeDispatchObservation& rhs) noexcept {
  return lhs.epoch.renderThreadId == rhs.epoch.renderThreadId &&
      lhs.epoch.flushEpoch == rhs.epoch.flushEpoch &&
      lhs.epoch.dispatchEpoch == rhs.epoch.dispatchEpoch &&
      lhs.path == rhs.path && lhs.sceneNode == rhs.sceneNode &&
      lhs.renderablePart == rhs.renderablePart &&
      lhs.layerIndex == rhs.layerIndex &&
      lhs.nestingDepth == rhs.nestingDepth && lhs.stage == rhs.stage &&
      lhs.batchTag == rhs.batchTag && lhs.failClosed == rhs.failClosed;
}

bool CurrentThreadIsObservedRenderThread();

bool NativeDispatchCpuOnlySealCandidateLess(
    const NativeDispatchCpuOnlySealCandidate& lhs,
    const NativeDispatchCpuOnlySealCandidate& rhs) noexcept {
  if (lhs.renderablePart != rhs.renderablePart)
    return lhs.renderablePart < rhs.renderablePart;
  return lhs.layerIndex < rhs.layerIndex;
}

bool SameNativeDispatchCpuOnlySealCandidate(
    const NativeDispatchCpuOnlySealCandidate& lhs,
    const NativeDispatchCpuOnlySealCandidate& rhs) noexcept {
  return lhs.renderablePart == rhs.renderablePart &&
      lhs.layerIndex == rhs.layerIndex;
}

void InvalidateNativeDispatchCpuOnlySealView() noexcept {
  // flushEpoch is the publication word. Clear it before touching the payload so
  // no nested or later query can consume a partially replaced view.
  t_state.dispatchCpuOnlySealView.flushEpoch = 0u;
  t_state.dispatchCpuOnlySealView.resetGeneration = 0u;
  t_state.dispatchCpuOnlySealView.count = 0u;
}

class NativeFlushCallbackEpochScope {
public:
  explicit NativeFlushCallbackEpochScope(uint64_t flushEpoch) noexcept
      : m_flushEpoch(flushEpoch) {
    t_state.flushCallbackEpoch = flushEpoch;
  }

  ~NativeFlushCallbackEpochScope() noexcept {
    if (!m_completed)
      InvalidateNativeDispatchCpuOnlySealView();
    if (t_state.flushCallbackEpoch == m_flushEpoch)
      t_state.flushCallbackEpoch = 0u;
  }

  void Complete() noexcept {
    m_completed = true;
    if (t_state.flushCallbackEpoch == m_flushEpoch)
      t_state.flushCallbackEpoch = 0u;
  }

  NativeFlushCallbackEpochScope(
      const NativeFlushCallbackEpochScope&) = delete;
  NativeFlushCallbackEpochScope& operator=(
      const NativeFlushCallbackEpochScope&) = delete;

private:
  uint64_t m_flushEpoch = 0u;
  bool m_completed = false;
};

bool NativeDispatchCpuOnlySealViewPublicationSafetyExact(
    uint64_t flushEpoch, uint64_t expectedResetGeneration) noexcept {
  if (g_runtimeConfig.mode != GpuSkinMode::Bypass ||
      g_runtimeConfig.fullDiagnostics || flushEpoch == 0u ||
      t_state.flushCallbackEpoch != flushEpoch ||
      t_state.flushTransactionDepth != 1u || t_callbackPinDepth != 1u ||
      t_state.dispatchDepth != 0u || t_state.semanticDepth != 0u ||
      t_state.dispatchOverflowDepth != 0u ||
      t_state.semanticOverflowDepth != 0u ||
      t_state.nestedUploadDepth != 0u ||
      t_state.inFlightUpload.observation != nullptr ||
      t_state.productionFastRejectedUpload != nullptr ||
      t_state.productionFastRejectedGlobalTransactionPinned ||
      t_state.productionEvidenceUploadEpoch != 0u ||
      OutsideUploadFastPathActive() ||
      DispatchCpuOnlyUploadFastPathActive() ||
      t_dipObserverDepth != 0u || t_dipObserverCookie != 0u ||
      t_state.poisonLedger.count != 0u ||
      g_nativePoisonOutstandingRanges.load(
          std::memory_order_acquire) != 0u ||
      g_counters.pendingKernelAuthorizations.load(
          std::memory_order_acquire) != 0u ||
      g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
      g_retirementQueueFaulted.load(std::memory_order_acquire) ||
      !g_hooksEnabled.load(std::memory_order_acquire) ||
      !g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      !g_callbackIngressEnabled.load(std::memory_order_acquire) ||
      !g_bypassEnabled.load(std::memory_order_acquire) ||
      !g_callbackOwnerRegistered.load(std::memory_order_acquire) ||
      g_callbacks.load(std::memory_order_acquire) == nullptr ||
      !CurrentThreadIsObservedRenderThread()) {
    return false;
  }
  const uint64_t resetRequested =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  return resetRequested == expectedResetGeneration &&
      resetRequested ==
          g_resetCompletedGeneration.load(std::memory_order_acquire) &&
      resetRequested == t_state.appliedResetGeneration;
}

bool DispatchCpuOnlySealPhysicalPathAllowed(
    const DispatchFrame& frame) noexcept {
  return frame.managerState == ManagerDispatchState::BeginIssued ||
      frame.managerState == ManagerDispatchState::NativeCpuOnly;
}

bool DispatchCpuOnlySealBaseSafetyExact(
    const DispatchFrame& frame, uint32_t expectedCallbackPinDepth,
    bool allowSemantic, bool allowUploadMarker) noexcept {
  if (g_runtimeConfig.mode != GpuSkinMode::Bypass ||
      g_runtimeConfig.fullDiagnostics ||
      frame.observation.path != NativeDispatchPath::Common ||
      frame.observation.failClosed ||
      frame.observation.nestingDepth != 0u ||
      frame.observation.epoch.renderThreadId == 0u ||
      frame.observation.epoch.renderThreadId != ::GetCurrentThreadId() ||
      frame.observation.epoch.flushEpoch == 0u ||
      frame.observation.epoch.flushEpoch != t_state.flushEpoch ||
      frame.observation.epoch.dispatchEpoch == 0u ||
      (frame.observation.epoch.dispatchEpoch %
       kNativeBeginTimingSamplePeriod) == 0u ||
      t_state.dispatchDepth != 1u || CurrentDispatch() != &frame ||
      t_state.flushTransactionDepth == 0u ||
      t_state.dispatchOverflowDepth != 0u ||
      t_state.semanticOverflowDepth != 0u ||
      t_state.nestedUploadDepth != 0u ||
      t_state.inFlightUpload.observation != nullptr ||
      t_state.productionFastRejectedUpload != nullptr ||
      t_state.productionEvidenceUploadEpoch != 0u ||
      OutsideUploadFastPathActive() ||
      (!allowUploadMarker && DispatchCpuOnlyUploadFastPathActive()) ||
      t_callbackPinDepth != expectedCallbackPinDepth ||
      t_state.poisonLedger.count != 0u ||
      g_nativePoisonOutstandingRanges.load(
          std::memory_order_acquire) != 0u ||
      g_counters.pendingKernelAuthorizations.load(
          std::memory_order_acquire) != 0u ||
      g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
      g_retirementQueueFaulted.load(std::memory_order_acquire) ||
      !g_hooksEnabled.load(std::memory_order_acquire) ||
      !g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      !g_callbackIngressEnabled.load(std::memory_order_acquire) ||
      !g_bypassEnabled.load(std::memory_order_acquire) ||
      !CurrentThreadIsObservedRenderThread()) {
    return false;
  }
  const uint64_t resetRequested =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  if (resetRequested !=
          g_resetCompletedGeneration.load(std::memory_order_acquire) ||
      resetRequested != t_state.appliedResetGeneration ||
      (frame.cpuOnlySeal.resetGeneration != 0u &&
       frame.cpuOnlySeal.resetGeneration != resetRequested)) {
    return false;
  }
  if (!allowSemantic)
    return t_state.semanticDepth == 0u;
  if (t_state.semanticDepth == 0u)
    return true;
  const SemanticFrame* semantic = CurrentSemantic();
  return t_state.semanticDepth == 1u && semantic != nullptr &&
      semantic->cookie != 0u && !semantic->failClosed &&
      semantic->dispatchEpoch ==
          frame.observation.epoch.dispatchEpoch &&
      semantic->path == NativeDispatchPath::Common;
}

bool DispatchCpuOnlySealUploadDynamicSafetyExact(
    const DispatchFrame& frame) noexcept {
  // Dispatch 的路径、线程、epoch 与 managerState 在提交 sealed scope 时
  // 已经完成一次完整证明；每次物理上传只重新读取可能异步变化的门：
  // reset、poison、retirement、callback pin、嵌套与 semantic 所有权。
  // 任何动态门不满足都立即回到原有完整观察路径。
  if (g_runtimeConfig.mode != GpuSkinMode::Bypass ||
      g_runtimeConfig.fullDiagnostics ||
      !DispatchCpuOnlySealPhysicalPathAllowed(frame) ||
      frame.observation.epoch.flushEpoch == 0u ||
      frame.observation.epoch.dispatchEpoch == 0u ||
      frame.observation.epoch.flushEpoch != t_state.flushEpoch ||
      (frame.observation.epoch.dispatchEpoch %
       kNativeBeginTimingSamplePeriod) == 0u ||
      frame.observation.epoch.renderThreadId != ::GetCurrentThreadId() ||
      t_state.dispatchDepth != 1u || CurrentDispatch() != &frame ||
      t_state.flushTransactionDepth == 0u ||
      t_state.dispatchOverflowDepth != 0u ||
      t_state.semanticOverflowDepth != 0u ||
      t_state.nestedUploadDepth != 0u ||
      t_state.inFlightUpload.observation != nullptr ||
      t_state.productionFastRejectedUpload != nullptr ||
      t_state.productionFastRejectedGlobalTransactionPinned ||
      t_state.productionEvidenceUploadEpoch != 0u ||
      OutsideUploadFastPathActive() ||
      DispatchCpuOnlyUploadFastPathActive() ||
      t_callbackPinDepth != 0u ||
      t_state.poisonLedger.count != 0u ||
      g_nativePoisonOutstandingRanges.load(
          std::memory_order_acquire) != 0u ||
      g_counters.pendingKernelAuthorizations.load(
          std::memory_order_acquire) != 0u ||
      g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
      g_retirementQueueFaulted.load(std::memory_order_acquire) ||
      !g_hooksEnabled.load(std::memory_order_acquire) ||
      !g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      !g_callbackIngressEnabled.load(std::memory_order_acquire) ||
      !g_bypassEnabled.load(std::memory_order_acquire) ||
      !CurrentThreadIsObservedRenderThread()) {
    return false;
  }
  const uint64_t resetRequested =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  if (resetRequested !=
          g_resetCompletedGeneration.load(std::memory_order_acquire) ||
      resetRequested != t_state.appliedResetGeneration ||
      (frame.cpuOnlySeal.resetGeneration != 0u &&
       resetRequested != frame.cpuOnlySeal.resetGeneration)) {
    return false;
  }
  if (t_state.semanticDepth == 0u)
    return true;
  const SemanticFrame* semantic = CurrentSemantic();
  return t_state.semanticDepth == 1u && semantic != nullptr &&
      semantic->cookie != 0u && !semantic->failClosed &&
      semantic->dispatchEpoch == frame.observation.epoch.dispatchEpoch &&
      semantic->path == NativeDispatchPath::Common;
}

bool TryCommitNativeDispatchCpuOnlySealFromLocalView(
    DispatchFrame& frame) noexcept {
  AddNativeTelemetryCounter(
      g_counters.dispatchCpuOnlySealLocalViewQueries,
      t_nativeTelemetryDelta.dispatchCpuOnlySealLocalViewQueries);
  const auto authorityReject = []() noexcept {
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealLocalViewAuthorityRejects,
        t_nativeTelemetryDelta.dispatchCpuOnlySealLocalViewAuthorityRejects);
    return false;
  };

  const NativeDispatchCpuOnlySealView& view =
      t_state.dispatchCpuOnlySealView;
  const uint64_t resetRequested =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  if (view.flushEpoch == 0u ||
      view.flushEpoch != frame.observation.epoch.flushEpoch ||
      view.resetGeneration != resetRequested ||
      view.count > kNativeDispatchCpuOnlySealViewCapacity ||
      t_state.flushTransactionDepth != 1u ||
      !DispatchCpuOnlySealBaseSafetyExact(frame, 0u, false, false)) {
    return authorityReject();
  }
  const uint32_t viewCount = view.count;

  const NativeDispatchCpuOnlySealCandidate key = {
      frame.observation.renderablePart,
      frame.observation.layerIndex,
  };
  const auto begin = view.candidates.begin();
  const auto end = begin + viewCount;
  const bool candidatePresent = std::binary_search(
      begin, end, key, NativeDispatchCpuOnlySealCandidateLess);
  if (candidatePresent) {
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealLocalViewCandidateRejects,
        t_nativeTelemetryDelta.dispatchCpuOnlySealLocalViewCandidateRejects);
    return false;
  }

  // Candidate lookup is bounded but not an authorization transaction. Repeat
  // the lifecycle/reset proof immediately before committing the TLS state so a
  // concurrent reset request cannot be hidden behind the negative search.
  if (resetRequested !=
      g_resetRequestedGeneration.load(std::memory_order_acquire) ||
      view.flushEpoch != frame.observation.epoch.flushEpoch ||
      view.resetGeneration != resetRequested ||
      view.count != viewCount ||
      !DispatchCpuOnlySealBaseSafetyExact(frame, 0u, false, false)) {
    return authorityReject();
  }

  frame.managerState = ManagerDispatchState::NativeCpuOnly;
  frame.cpuOnlySeal.committed = true;
  frame.cpuOnlySeal.committedEver = true;
  frame.cpuOnlySeal.resetGeneration = resetRequested;
  AddNativeTelemetryCounter(
      g_counters.managerDispatchNativeCpuOnlyScopes,
      t_nativeTelemetryDelta.managerDispatchNativeCpuOnlyScopes);
  AddNativeTelemetryCounter(
      g_counters.dispatchCpuOnlySealLocalViewCommits,
      t_nativeTelemetryDelta.dispatchCpuOnlySealLocalViewCommits);
  AddNativeTelemetryCounter(
      g_counters.dispatchCpuOnlySealScopeCommits,
      t_nativeTelemetryDelta.dispatchCpuOnlySealScopeCommits);
  return true;
}

bool NativeGpuSkinDispatchObservationRequiredInternal(
    NativeDispatchPath path, uintptr_t renderablePart,
    uint32_t layerIndex) noexcept {
  // 这是一个“只读负授权”门：它只能证明当前 dispatch 不在已经发布的
  // 候选集合中，不能单独授权任何 GPU 绘制。所有正候选仍进入原有 ABI-9
  // frame 和 manager preflight；视图/代际/重置任一不确定就保守观察。
  if (g_runtimeConfig.mode != GpuSkinMode::Bypass ||
      g_runtimeConfig.fullDiagnostics ||
      g_runtimeConfig.executionRoute !=
          GpuSkinExecutionRoute::VertexShaderBypass ||
      !g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      !g_callbackIngressEnabled.load(std::memory_order_acquire) ||
      !g_bypassEnabled.load(std::memory_order_acquire) ||
      !g_hooksEnabled.load(std::memory_order_acquire) ||
      !CurrentThreadIsObservedRenderThread() ||
      renderablePart == 0u ||
      t_state.dispatchDepth != 0u ||
      t_state.semanticDepth != 0u ||
      t_state.dispatchOverflowDepth != 0u ||
      t_state.semanticOverflowDepth != 0u ||
      t_state.nestedUploadDepth != 0u ||
      t_state.flushTransactionDepth != 1u ||
      t_state.inFlightUpload.observation != nullptr ||
      t_state.productionFastRejectedUpload != nullptr ||
      t_state.productionEvidenceUploadEpoch != 0u ||
      OutsideUploadFastPathActive() ||
      DispatchCpuOnlyUploadFastPathActive() ||
      t_state.poisonLedger.count != 0u ||
      g_nativePoisonOutstandingRanges.load(std::memory_order_acquire) != 0u ||
      g_counters.pendingKernelAuthorizations.load(
          std::memory_order_acquire) != 0u ||
      g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
      g_retirementQueueFaulted.load(std::memory_order_acquire)) {
    return true;
  }

  const uint64_t resetRequested =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  const uint64_t resetCompleted =
      g_resetCompletedGeneration.load(std::memory_order_acquire);
  const NativeDispatchCpuOnlySealView& view =
      t_state.dispatchCpuOnlySealView;
  if (resetRequested != resetCompleted ||
      resetRequested != t_state.appliedResetGeneration ||
      view.flushEpoch == 0u || view.flushEpoch != t_state.flushEpoch ||
      view.resetGeneration != resetRequested ||
      view.count > kNativeDispatchCpuOnlySealViewCapacity) {
    return true;
  }

  if (path == NativeDispatchPath::Special)
    return false;
  if (path != NativeDispatchPath::Common)
    return true;

  const NativeDispatchCpuOnlySealCandidate key = {
      renderablePart, layerIndex};
  const auto begin = view.candidates.begin();
  const auto end = begin + view.count;
  return std::binary_search(
      begin, end, key, NativeDispatchCpuOnlySealCandidateLess);
}

bool NativeGpuSkinSemanticObservationRequiredInternal() noexcept {
  if (g_runtimeConfig.mode != GpuSkinMode::Bypass ||
      g_runtimeConfig.fullDiagnostics ||
      g_runtimeConfig.executionRoute !=
          GpuSkinExecutionRoute::VertexShaderBypass) {
    return true;
  }
  const DispatchFrame* dispatch = CurrentDispatch();
  if (dispatch == nullptr) {
    ++t_state.productionSemanticObservationOrdinal;
    return (t_state.productionSemanticObservationOrdinal % 127u) == 0u;
  }
  // NativeCpuOnly/NeverSpecial are the exact negative windows. Any other
  // state stays observed, including BeginIssued and failed/uncertain states.
  if (dispatch->managerState != ManagerDispatchState::NativeCpuOnly &&
      dispatch->managerState != ManagerDispatchState::NeverSpecial)
    return true;
  ++t_state.productionSemanticObservationOrdinal;
  return (t_state.productionSemanticObservationOrdinal % 127u) == 0u;
}

bool NativeGpuSkinB1UnobservedNativePathSafeInternal() noexcept {
  // 只有 VS-B1 的负候选窗口可以走这里。Compute、VS-A/B0 以及任何
  // 生命周期不确定状态仍必须保留完整原生观察路径。
  if (g_runtimeConfig.mode != GpuSkinMode::Bypass ||
      g_runtimeConfig.fullDiagnostics ||
      g_runtimeConfig.executionRoute !=
          GpuSkinExecutionRoute::VertexShaderBypass ||
      !g_runtimeConfig.executionRouteExplicit ||
      g_runtimeConfig.executionRouteInvalid ||
      !g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      !g_callbackIngressEnabled.load(std::memory_order_acquire) ||
      !g_bypassEnabled.load(std::memory_order_acquire) ||
      !g_hooksEnabled.load(std::memory_order_acquire) ||
      !CurrentThreadIsObservedRenderThread()) {
    return false;
  }
  const DispatchFrame* dispatch = CurrentDispatch();
  // 空视图必须继续走一次完整观察以学习下一批候选；只有已有候选视图
  // 下的精确 NativeCpuOnly 负 dispatch 才能安全直通。
  const NativeDispatchCpuOnlySealView& view =
      t_state.dispatchCpuOnlySealView;
  if (dispatch == nullptr || !dispatch->cpuOnlySeal.committed ||
      view.count == 0u ||
      view.flushEpoch != dispatch->observation.epoch.flushEpoch ||
      dispatch->activeUpload.valid || dispatch->cpuOnlyActiveUpload.valid ||
      t_state.dispatchOverflowDepth != 0u ||
      t_state.semanticOverflowDepth != 0u ||
      t_state.nestedUploadDepth != 0u ||
      t_state.inFlightUpload.observation != nullptr ||
      t_state.productionFastRejectedUpload != nullptr ||
      t_state.productionEvidenceUploadEpoch != 0u ||
      OutsideUploadFastPathActive() ||
      DispatchCpuOnlyUploadFastPathActive() ||
      t_state.outsideNoPoisonDirectOriginal.cookie != 0u ||
      t_outsidePoisonShadow.depth != 0u ||
      t_outsidePoisonShadow.overflowDepth != 0u ||
      t_state.poisonLedger.count != 0u ||
      g_nativePoisonOutstandingRanges.load(
          std::memory_order_acquire) != 0u ||
      g_counters.pendingKernelAuthorizations.load(
          std::memory_order_acquire) != 0u ||
      g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
      g_retirementQueueFaulted.load(std::memory_order_acquire)) {
    return false;
  }
  return true;
}

bool NativeGpuSkinB1DispatchKnownNegativeInternal(
    NativeDispatchPath path, uintptr_t renderablePart,
    uint32_t layerIndex) noexcept {
  if (g_runtimeConfig.mode != GpuSkinMode::Bypass ||
      g_runtimeConfig.fullDiagnostics ||
      g_runtimeConfig.executionRoute !=
          GpuSkinExecutionRoute::VertexShaderBypass ||
      !g_runtimeConfig.executionRouteExplicit ||
      g_runtimeConfig.executionRouteInvalid || path != NativeDispatchPath::Common ||
      renderablePart == 0u || t_state.dispatchDepth != 0u ||
      t_state.flushTransactionDepth != 1u ||
      !g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      !g_callbackIngressEnabled.load(std::memory_order_acquire) ||
      !g_bypassEnabled.load(std::memory_order_acquire) ||
      !g_hooksEnabled.load(std::memory_order_acquire) ||
      !CurrentThreadIsObservedRenderThread()) {
    return false;
  }
  const NativeDispatchCpuOnlySealView& view =
      t_state.dispatchCpuOnlySealView;
  const uint64_t resetRequested =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  if (view.count == 0u || view.count > kNativeDispatchCpuOnlySealViewCapacity ||
      view.flushEpoch == 0u || view.flushEpoch != t_state.flushEpoch ||
      view.resetGeneration != resetRequested ||
      resetRequested != g_resetCompletedGeneration.load(
          std::memory_order_acquire) ||
      resetRequested != t_state.appliedResetGeneration ||
      t_state.poisonLedger.count != 0u ||
      g_nativePoisonOutstandingRanges.load(
          std::memory_order_acquire) != 0u ||
      g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
      g_retirementQueueFaulted.load(std::memory_order_acquire)) {
    return false;
  }
  const NativeDispatchCpuOnlySealCandidate key = {
      renderablePart, layerIndex};
  const auto begin = view.candidates.begin();
  return !std::binary_search(begin, begin + view.count, key,
                             NativeDispatchCpuOnlySealCandidateLess);
}

void AbortDispatchCpuOnlySealProposal(DispatchFrame& frame) noexcept {
  if (!frame.cpuOnlySeal.proposed)
    return;
  frame.cpuOnlySeal.proposed = false;
  AddNativeTelemetryCounter(
      g_counters.dispatchCpuOnlySealProposalAborted,
      t_nativeTelemetryDelta.dispatchCpuOnlySealProposalAborted);
}

void CommitDispatchCpuOnlySealProposal(DispatchFrame& frame) noexcept {
  if (!frame.cpuOnlySeal.proposed)
    return;
  if (frame.managerState != ManagerDispatchState::BeginIssued ||
      frame.uploadCount != 0u || frame.dipCount != 0u ||
      frame.activeUpload.valid ||
      !DispatchCpuOnlySealBaseSafetyExact(frame, 1u, false, false)) {
    AbortDispatchCpuOnlySealProposal(frame);
    return;
  }
  frame.cpuOnlySeal.proposed = false;
  frame.cpuOnlySeal.committed = true;
  frame.cpuOnlySeal.committedEver = true;
  frame.cpuOnlySeal.resetGeneration =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  AddNativeTelemetryCounter(
      g_counters.dispatchCpuOnlySealScopeCommits,
      t_nativeTelemetryDelta.dispatchCpuOnlySealScopeCommits);
}

bool ManagerDispatchBeginWasIssued(uint64_t dispatchEpoch) noexcept {
  const DispatchFrame* frame = FindDispatchFrameByEpoch(dispatchEpoch);
  return frame != nullptr &&
      frame->managerState == ManagerDispatchState::BeginIssued;
}

enum class ManagerSkippedCallbackKind : uint8_t {
  Upload,
  Dip,
  Fanout,
};

bool ManagerCallbackAllowedForEpoch(
    uint64_t dispatchEpoch, ManagerSkippedCallbackKind kind) noexcept {
  // Orphan observations have no dispatch begin by definition. Evidence modes
  // and poison retirement retain their historical orphan callback contract.
  if (dispatchEpoch == 0u)
    return true;
  if (ManagerDispatchBeginWasIssued(dispatchEpoch))
    return true;
  switch (kind) {
    case ManagerSkippedCallbackKind::Upload:
      AddNativeTelemetryCounter(
          g_counters.managerDispatchSkippedUploads,
          t_nativeTelemetryDelta.managerDispatchSkippedUploads);
      break;
    case ManagerSkippedCallbackKind::Dip:
      AddNativeTelemetryCounter(
          g_counters.managerDispatchSkippedDips,
          t_nativeTelemetryDelta.managerDispatchSkippedDips);
      break;
    case ManagerSkippedCallbackKind::Fanout:
      AddNativeTelemetryCounter(
          g_counters.managerDispatchSkippedFanouts,
          t_nativeTelemetryDelta.managerDispatchSkippedFanouts);
      break;
  }
  return false;
}

void FailManagerDispatchAdmission(DispatchFrame& frame) noexcept {
  const ManagerDispatchState previous = frame.managerState;
  if (previous == ManagerDispatchState::FailedClosed)
    return;
  frame.managerState = ManagerDispatchState::FailedClosed;
  frame.observation.failClosed = true;
  if (previous == ManagerDispatchState::PendingCommon) {
    g_counters.managerDispatchLazyAdmissionFailures.fetch_add(
        1u, std::memory_order_relaxed);
  } else if (previous == ManagerDispatchState::NeverSpecial) {
    g_counters.managerDispatchNeverSafetyFailures.fetch_add(
        1u, std::memory_order_relaxed);
  } else {
    g_counters.managerDispatchEagerAdmissionFailures.fetch_add(
        1u, std::memory_order_relaxed);
  }
}

bool IssueManagerDispatchBegin(DispatchFrame& frame, bool lazy) {
  if (frame.managerState == ManagerDispatchState::BeginIssued)
    return true;
  AbortDispatchCpuOnlySealProposal(frame);
  if (lazy) {
    AddNativeTelemetryCounter(
        g_counters.managerDispatchLazyAdmissionAttempts,
        t_nativeTelemetryDelta.managerDispatchLazyAdmissionAttempts);
  }
  // ABI 9 has no prefix counts. Admission is therefore legal only before the
  // first correlated native event; otherwise CPU fallback is the only safe
  // result.
  if (frame.uploadCount != 0u || frame.dipCount != 0u ||
      frame.activeUpload.valid) {
    FailManagerDispatchAdmission(frame);
    return false;
  }

  NativeBridgeCallbackPin callbackPin(
      NativeBridgeCallbackKind::DispatchBegin,
      frame.observation.epoch.dispatchEpoch);
  const NativeBridgeCallbacks* callbacks = callbackPin.get();
  if (callbacks == nullptr || callbacks->onDispatchBegin == nullptr) {
    FailManagerDispatchAdmission(frame);
    return false;
  }
  callbackPin.BeginCallbackBody();
  callbacks->onDispatchBegin(callbacks->userData, frame.observation);
  callbackPin.EndCallbackBody();
  frame.managerState = ManagerDispatchState::BeginIssued;
  // The manager sidecar may only propose during the callback. Commit after
  // the callback body has returned and BeginIssued is authoritative; any
  // rejected/throwing/unpaired manager path leaves no proposal to consume.
  CommitDispatchCpuOnlySealProposal(frame);
  AddNativeTelemetryCounter(
      g_counters.managerDispatchBeginCallbacks,
      t_nativeTelemetryDelta.managerDispatchBeginCallbacks);
  if (lazy) {
    AddNativeTelemetryCounter(
        g_counters.managerDispatchLazyAdmissions,
        t_nativeTelemetryDelta.managerDispatchLazyAdmissions);
  } else {
    AddNativeTelemetryCounter(
        g_counters.managerDispatchEagerBegins,
        t_nativeTelemetryDelta.managerDispatchEagerBegins);
  }
  return true;
}

bool ManagerLazyDispatchSafetyClean(bool nested,
                                    bool forceFailClosed) noexcept {
  const uint64_t observedThread =
      g_observedRenderThreadId.load(std::memory_order_acquire);
  return !nested && !forceFailClosed &&
      t_state.dispatchOverflowDepth == 0u &&
      t_state.semanticDepth == 0u &&
      t_state.semanticOverflowDepth == 0u &&
      t_state.nestedUploadDepth == 0u &&
      t_state.inFlightUpload.observation == nullptr &&
      !OutsideUploadFastPathActive() &&
      !DispatchCpuOnlyUploadFastPathActive() &&
      t_state.poisonLedger.count == 0u &&
      observedThread != 0u && observedThread == ::GetCurrentThreadId() &&
      g_resetRequestedGeneration.load(std::memory_order_acquire) ==
          g_resetCompletedGeneration.load(std::memory_order_acquire) &&
      g_retirementEventsPending.load(std::memory_order_acquire) == 0u &&
      !g_retirementQueueFaulted.load(std::memory_order_acquire) &&
      g_bypassEnabled.load(std::memory_order_acquire) &&
      g_callbackIngressEnabled.load(std::memory_order_acquire) &&
      g_callbackOwnerRegistered.load(std::memory_order_acquire) &&
      g_callbacks.load(std::memory_order_acquire) != nullptr;
}

void PromoteDeferredManagerDispatchForHazard(DispatchFrame& frame) {
  if (frame.managerState != ManagerDispatchState::PendingCommon &&
      frame.managerState != ManagerDispatchState::NeverSpecial) {
    return;
  }

  // ABI 9 cannot replay an omitted event prefix. A hazard discovered after a
  // deferred scope began may promote the scope only while that prefix is
  // still empty. Otherwise the native path remains CPU-only/fail-closed and
  // the hard-gate counter exposes the contract violation.
  frame.observation.failClosed = true;
  const bool wasNever =
      frame.managerState == ManagerDispatchState::NeverSpecial;
  IssueManagerDispatchBegin(frame, !wasNever);
}

bool WaitForCallbackPinsToDrain(uint32_t timeoutMs) {
  const ULONGLONG start = ::GetTickCount64();
  while (g_activeCallbackPins.load(std::memory_order_seq_cst) != 0u) {
    if (timeoutMs != std::numeric_limits<uint32_t>::max() &&
        ::GetTickCount64() - start >= timeoutMs) {
      return false;
    }
    if (!::SwitchToThread())
      ::Sleep(1u);
  }
  return true;
}

uint64_t RequestBridgeResetGeneration(
    NativePoisonLedgerResetReason reason) {
  ResetProtocolLockGuard protocolLock;
  // Final bypass commit takes this same low-frequency lock. Blocking bypass is
  // therefore the first visible action, before any request payload changes.
  g_bypassEnabled.store(false, std::memory_order_release);
  uint64_t generation = ++g_nextResetGeneration;
  if (generation == 0u) {
    generation = 1u;
    g_nextResetGeneration = generation;
    g_resetCompletedGeneration.store(0u, std::memory_order_relaxed);
    g_ownerRetiredGeneration.store(0u, std::memory_order_relaxed);
  }
  const bool ownerAckRequired =
      (reason == NativePoisonLedgerResetReason::MapEpoch ||
       reason == NativePoisonLedgerResetReason::DeviceEpoch) &&
      (g_callbackOwnerRegistered.load(std::memory_order_acquire) ||
       g_callbacks.load(std::memory_order_acquire) != nullptr);
  g_resetReason.store(static_cast<uint32_t>(reason),
                      std::memory_order_relaxed);
  g_resetOwnerAckRequired.store(ownerAckRequired,
                                std::memory_order_relaxed);
  // Release-publish generation last so readers never authorize against a
  // request whose reason/owner-ack payload has not been written yet.
  g_resetRequestedGeneration.store(generation, std::memory_order_release);
  g_counters.resetRequests.fetch_add(1u, std::memory_order_relaxed);
  return generation;
}

void SubtractPoisonOutstanding(uint64_t count) {
  uint64_t current =
      g_nativePoisonOutstandingRanges.load(std::memory_order_relaxed);
  while (current != 0u) {
    const uint64_t next = current > count ? current - count : 0u;
    if (g_nativePoisonOutstandingRanges.compare_exchange_weak(
            current, next, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      return;
    }
  }
}

uint64_t PoisonRangeEnd(const NativePoisonRange& range) {
  return uint64_t(range.baseVertex) + range.vertexCount;
}

bool SameVertexOutputProof(const NativeVertexOutputProof& lhs,
                           const NativeVertexOutputProof& rhs) {
  return lhs.resource.commonResource == rhs.resource.commonResource &&
      lhs.resource.comVertexBuffer == rhs.resource.comVertexBuffer &&
      lhs.resource.resourceGeneration == rhs.resource.resourceGeneration &&
      lhs.nativeD3DDevice == rhs.nativeD3DDevice &&
      lhs.outputFormat == rhs.outputFormat &&
      lhs.vertexStride == rhs.vertexStride && lhs.fvf == rhs.fvf;
}

NativeVertexStorageDiagnostics FreezePublishedStorageDiagnostics(
    const NativeVertexOutputProof& proof) {
  const NativeVertexStorageSidecar& sidecar =
      t_state.vertexStorageSidecar;
  return sidecar.valid && SameVertexOutputProof(sidecar.proof, proof)
      ? sidecar.diagnostics
      : NativeVertexStorageDiagnostics{};
}

bool SamePoisonLayout(const NativePoisonRange& lhs,
                      const NativePoisonRange& rhs) {
  return lhs.vertexOutputProof.resource.commonResource ==
          rhs.vertexOutputProof.resource.commonResource &&
      lhs.vertexOutputProof.resource.comVertexBuffer ==
          rhs.vertexOutputProof.resource.comVertexBuffer &&
      lhs.vertexOutputProof.resource.resourceGeneration ==
          rhs.vertexOutputProof.resource.resourceGeneration &&
      lhs.vertexOutputProof.nativeD3DDevice ==
          rhs.vertexOutputProof.nativeD3DDevice &&
      lhs.vertexOutputProof.outputFormat ==
          rhs.vertexOutputProof.outputFormat &&
      lhs.vertexOutputProof.vertexStride ==
          rhs.vertexOutputProof.vertexStride &&
      lhs.vertexOutputProof.fvf == rhs.vertexOutputProof.fvf &&
      lhs.stride == rhs.stride && lhs.fvf == rhs.fvf &&
      lhs.outputFormat == rhs.outputFormat;
}

bool SameCpuOverwriteTarget(const NativePoisonRange& poison,
                             const NativePoisonRange& coverage) {
  return poison.vertexOutputProof.resource.commonResource ==
          coverage.vertexOutputProof.resource.commonResource &&
      poison.vertexOutputProof.resource.resourceGeneration ==
          coverage.vertexOutputProof.resource.resourceGeneration &&
      poison.vertexOutputProof.nativeD3DDevice ==
          coverage.vertexOutputProof.nativeD3DDevice &&
      poison.vertexOutputProof.resource.comVertexBuffer ==
          coverage.vertexOutputProof.resource.comVertexBuffer &&
      poison.stride == coverage.stride && poison.fvf == coverage.fvf &&
      poison.outputFormat == coverage.outputFormat;
}

bool CpuRewriteProofMatchesUpload(
    const NativeUploadObservation& upload,
    const NativeCpuRewriteOutputProof& proof) {
  const NativeVertexOutputProof& vertexProof = proof.vertexOutputProof;
  const uint64_t proofEnd = uint64_t(proof.baseVertex) + proof.vertexCount;
  const uint64_t proofByteOffset =
      uint64_t(proof.baseVertex) * vertexProof.vertexStride;
  const uint64_t proofByteLength =
      uint64_t(proof.vertexCount) * vertexProof.vertexStride;
  return vertexProof.resource.commonResource != 0u &&
      vertexProof.resource.comVertexBuffer != 0u &&
      vertexProof.resource.resourceGeneration != 0u &&
      vertexProof.resource.comVertexBuffer == upload.nativeVertexBuffer &&
      vertexProof.nativeD3DDevice != 0u &&
      vertexProof.nativeD3DDevice == upload.nativeD3DDevice &&
      vertexProof.outputFormat == upload.outputFormat &&
      vertexProof.vertexStride == upload.outputStride &&
      vertexProof.fvf == upload.fvf &&
      proof.baseVertex == upload.ringBaseVertexAfter &&
      proof.vertexCount != 0u && proof.vertexCount == upload.vertexCount &&
      proofEnd <= kMaxNativeVertices &&
      proofEnd == upload.ringNextVertexAfter &&
      proofByteOffset <= std::numeric_limits<uint32_t>::max() &&
      proofByteLength <= std::numeric_limits<uint32_t>::max() &&
      proof.byteOffset == proofByteOffset &&
      proof.byteLength == proofByteLength;
}

bool CpuUploadMayOverlapPoison(const NativeUploadObservation& upload) {
  if (!upload.cpuSkinOriginalRangeExact || upload.mappedDst == 0u ||
      upload.nativeD3DDevice == 0u || upload.nativeVertexBuffer == 0u ||
      upload.vertexCount == 0u || upload.outputStride == 0u ||
      upload.outputFormat >= kFormatFvf.size()) {
    return false;
  }

  const uint64_t uploadBegin = upload.ringBaseVertexAfter;
  const uint64_t uploadEnd = upload.ringNextVertexAfter;
  if (uploadEnd <= uploadBegin ||
      uploadEnd - uploadBegin != upload.vertexCount ||
      uploadEnd > kMaxNativeVertices) {
    return false;
  }

  const NativePoisonLedger& ledger = t_state.poisonLedger;
  for (size_t i = 0u; i < ledger.count; ++i) {
    const NativePoisonRange& poison = ledger.ranges[i];
    const NativeVertexOutputProof& proof = poison.vertexOutputProof;
    if (proof.nativeD3DDevice != upload.nativeD3DDevice ||
        proof.resource.comVertexBuffer != upload.nativeVertexBuffer ||
        proof.outputFormat != upload.outputFormat ||
        proof.vertexStride != upload.outputStride ||
        proof.fvf != upload.fvf ||
        poison.stride != upload.outputStride || poison.fvf != upload.fvf ||
        poison.outputFormat != upload.outputFormat) {
      continue;
    }
    const uint64_t poisonEnd = PoisonRangeEnd(poison);
    if (uploadEnd > poison.baseVertex && uploadBegin < poisonEnd)
      return true;
  }
  return false;
}

bool PredictedCpuUploadMayOverlapPoison(
    const NativeUploadObservation& upload) {
  const NativePoisonLedger& ledger = t_state.poisonLedger;
  if (ledger.count == 0u)
    return false;

  // Incomplete state cannot exclude an overwrite. Keep the retirement path
  // fail-closed; RefreshKernelState and the host proof remain authoritative.
  if (!upload.nativeDeviceStateReadable || upload.nativeD3DDevice == 0u ||
      upload.nativeVertexBuffer == 0u || upload.vertexCount == 0u ||
      upload.vertexCount > kMaxNativeVertices || upload.outputStride == 0u ||
      upload.outputFormat >= kFormatFvf.size() ||
      upload.ringNextVertexBefore > kMaxNativeVertices) {
    return true;
  }

  // IDA 0x6F0EE5D0: LockDynamicVertexRing wraps to zero only when oldNext +
  // count exceeds 0x4000; otherwise the next slice starts at oldNext.
  const bool wraps = upload.ringNextVertexBefore >
      kMaxNativeVertices - upload.vertexCount;
  const uint64_t uploadBegin = wraps ? 0u : upload.ringNextVertexBefore;
  const uint64_t uploadEnd = uploadBegin + upload.vertexCount;
  for (size_t i = 0u; i < ledger.count; ++i) {
    const NativePoisonRange& poison = ledger.ranges[i];
    const NativeVertexOutputProof& proof = poison.vertexOutputProof;
    if (proof.nativeD3DDevice != upload.nativeD3DDevice ||
        proof.resource.comVertexBuffer != upload.nativeVertexBuffer ||
        proof.outputFormat != upload.outputFormat ||
        proof.vertexStride != upload.outputStride ||
        proof.fvf != upload.fvf ||
        poison.stride != upload.outputStride || poison.fvf != upload.fvf ||
        poison.outputFormat != upload.outputFormat) {
      continue;
    }
    const uint64_t poisonEnd = PoisonRangeEnd(poison);
    if (uploadEnd > poison.baseVertex && uploadBegin < poisonEnd)
      return true;
  }
  return false;
}

bool ReadOutsidePoisonSnapshotU32(size_t absoluteOffset,
                                  uint32_t& value) noexcept {
  if (absoluteOffset < kOutsidePoisonSnapshotBeginOffset ||
      absoluteOffset >
          kOutsidePoisonSnapshotEndOffset - sizeof(uint32_t)) {
    return false;
  }
  const size_t relativeOffset =
      absoluteOffset - kOutsidePoisonSnapshotBeginOffset;
  std::memcpy(&value,
              t_outsidePoisonSnapshotScratch.data() + relativeOffset,
              sizeof(value));
  return true;
}

bool OutsidePoisonExactOverlapMatchesRange(
    const NativeOutsidePoisonExactOverlap& frozen,
    const NativePoisonRange& current) noexcept {
  const NativeVertexOutputProof& lhs = frozen.vertexOutputProof;
  const NativeVertexOutputProof& rhs = current.vertexOutputProof;
  return frozen.valid &&
      lhs.resource.commonResource == rhs.resource.commonResource &&
      lhs.resource.comVertexBuffer == rhs.resource.comVertexBuffer &&
      lhs.resource.resourceGeneration == rhs.resource.resourceGeneration &&
      lhs.nativeD3DDevice == rhs.nativeD3DDevice &&
      lhs.outputFormat == rhs.outputFormat &&
      lhs.vertexStride == rhs.vertexStride && lhs.fvf == rhs.fvf &&
      frozen.baseVertex == current.baseVertex &&
      frozen.vertexCount == current.vertexCount &&
      frozen.stride == current.stride && frozen.fvf == current.fvf &&
      frozen.outputFormat == current.outputFormat;
}

bool OutsidePoisonExactOverlapMatchesOldTarget(
    const NativeOutsidePoisonExactOverlap& frozen,
    const NativeOutsidePoisonScanObservation& target) noexcept {
  if (!frozen.valid || !target.snapshotCaptured || !target.targetParsed ||
      target.nativeD3DDevice == 0u || target.comVertexBuffer == 0u ||
      target.vertexCount == 0u || target.vertexCount > kMaxNativeVertices ||
      frozen.vertexCount == 0u ||
      frozen.outputFormat >= kFormatFvf.size()) {
    return false;
  }
  const NativeVertexOutputProof& proof = frozen.vertexOutputProof;
  if (proof.resource.commonResource == 0u ||
      proof.resource.comVertexBuffer == 0u ||
      proof.resource.resourceGeneration == 0u ||
      proof.nativeD3DDevice == 0u ||
      proof.nativeD3DDevice != target.nativeD3DDevice ||
      proof.resource.comVertexBuffer != target.comVertexBuffer ||
      proof.outputFormat != target.outputFormat ||
      proof.outputFormat != frozen.outputFormat ||
      proof.vertexStride != kFormatStride[proof.outputFormat] ||
      proof.vertexStride != frozen.stride ||
      proof.fvf != kFormatFvf[proof.outputFormat] ||
      proof.fvf != frozen.fvf) {
    return false;
  }
  const uint64_t uploadBegin = target.predictedBaseVertex;
  const uint64_t uploadEnd = uploadBegin + target.vertexCount;
  const uint64_t poisonBegin = frozen.baseVertex;
  const uint64_t poisonEnd = poisonBegin + frozen.vertexCount;
  return uploadEnd <= kMaxNativeVertices &&
      poisonEnd <= kMaxNativeVertices && uploadEnd > poisonBegin &&
      uploadBegin < poisonEnd;
}

bool ProveVertexShaderBypassPoisonTarget(
    uintptr_t gxDeviceD3d, uint32_t vertexCount,
    const NativePoisonLedger& ledger,
    NativeOutsidePoisonScanObservation* scanObservation,
    NativeOutsidePoisonExactOverlap* exactOverlap,
    NativeOutsidePoisonScanResult& result) noexcept {
  if (g_runtimeConfig.executionRoute !=
          GpuSkinExecutionRoute::VertexShaderBypass ||
      ledger.count == 0u) {
    return false;
  }

  // VS-B1 只允许 format2；任何历史格式都必须回到完整快照路径。
  for (size_t i = 0u; i < ledger.count; ++i) {
    const NativePoisonRange& poison = ledger.ranges[i];
    const NativeVertexOutputProof& proof = poison.vertexOutputProof;
    if (poison.outputFormat != kVertexShaderBypassPoisonFormat ||
        poison.stride != kFormatStride[kVertexShaderBypassPoisonFormat] ||
        poison.fvf != kFormatFvf[kVertexShaderBypassPoisonFormat] ||
        proof.outputFormat != kVertexShaderBypassPoisonFormat ||
        proof.vertexStride != kFormatStride[kVertexShaderBypassPoisonFormat] ||
        proof.fvf != kFormatFvf[kVertexShaderBypassPoisonFormat]) {
      return false;
    }
  }

  uint32_t outputFormat = 0u;
  if (gxDeviceD3d == 0u || vertexCount == 0u ||
      vertexCount > kMaxNativeVertices ||
      kGxOutputFormatOffset >
          std::numeric_limits<uintptr_t>::max() - gxDeviceD3d ||
      !dxvk::war3::SafeCopy(
          &outputFormat,
          reinterpret_cast<const void*>(gxDeviceD3d +
                                        kGxOutputFormatOffset),
          sizeof(outputFormat))) {
    result = NativeOutsidePoisonScanResult::ReadFailure;
    return true;
  }
  if (outputFormat != kVertexShaderBypassPoisonFormat) {
    if (scanObservation != nullptr) {
      scanObservation->snapshotCaptured = true;
      scanObservation->outputFormat = outputFormat;
      scanObservation->targetParsed = true;
    }
    result = NativeOutsidePoisonScanResult::NoOverlap;
    return true;
  }

  if (kVertexShaderBypassPoisonStateBegin >
          std::numeric_limits<uintptr_t>::max() - gxDeviceD3d ||
      !dxvk::war3::SafeCopy(
          t_vertexShaderBypassPoisonStateScratch.data(),
          reinterpret_cast<const void*>(gxDeviceD3d +
                                        kVertexShaderBypassPoisonStateBegin),
          t_vertexShaderBypassPoisonStateScratch.size())) {
    result = NativeOutsidePoisonScanResult::ReadFailure;
    return true;
  }
  const auto readStateU32 = [](size_t absoluteOffset,
                               uint32_t& value) noexcept {
    if (absoluteOffset < kVertexShaderBypassPoisonStateBegin ||
        absoluteOffset >
            kVertexShaderBypassPoisonStateEnd - sizeof(uint32_t)) {
      return false;
    }
    const size_t relativeOffset =
        absoluteOffset - kVertexShaderBypassPoisonStateBegin;
    std::memcpy(&value,
                t_vertexShaderBypassPoisonStateScratch.data() +
                    relativeOffset,
                sizeof(value));
    return true;
  };
  uint32_t nativeDevice32 = 0u;
  uint32_t vertexBuffer32 = 0u;
  uint32_t ringNext = 0u;
  if (!readStateU32(kGxNativeDeviceOffset, nativeDevice32) ||
      !readStateU32(
          kGxVertexBufferArrayOffset +
              kVertexShaderBypassPoisonFormat * sizeof(uint32_t),
          vertexBuffer32) ||
      !readStateU32(
          kGxVertexRingNextOffset +
              kVertexShaderBypassPoisonFormat * 8u,
          ringNext) ||
      nativeDevice32 == 0u || vertexBuffer32 == 0u ||
      ringNext > kMaxNativeVertices) {
    result = NativeOutsidePoisonScanResult::ReadFailure;
    return true;
  }

  const uint64_t uploadBegin =
      ringNext > kMaxNativeVertices - vertexCount ? 0u : ringNext;
  const uint64_t uploadEnd = uploadBegin + vertexCount;
  if (scanObservation != nullptr) {
    scanObservation->snapshotCaptured = true;
    scanObservation->nativeD3DDevice = nativeDevice32;
    scanObservation->comVertexBuffer = vertexBuffer32;
    scanObservation->outputFormat = outputFormat;
    scanObservation->ringNext = ringNext;
    scanObservation->predictedBaseVertex =
        static_cast<uint32_t>(uploadBegin);
    scanObservation->targetParsed = true;
  }
  const uintptr_t nativeDevice = nativeDevice32;
  const uintptr_t vertexBuffer = vertexBuffer32;
  for (size_t i = 0u; i < ledger.count; ++i) {
    const NativePoisonRange& poison = ledger.ranges[i];
    const NativeVertexOutputProof& proof = poison.vertexOutputProof;
    if (proof.nativeD3DDevice != nativeDevice ||
        proof.resource.comVertexBuffer != vertexBuffer) {
      continue;
    }
    const uint64_t poisonEnd = PoisonRangeEnd(poison);
    if (uploadEnd > poison.baseVertex && uploadBegin < poisonEnd) {
      if (exactOverlap != nullptr) {
        exactOverlap->vertexOutputProof = proof;
        exactOverlap->baseVertex = poison.baseVertex;
        exactOverlap->vertexCount = poison.vertexCount;
        exactOverlap->stride = poison.stride;
        exactOverlap->fvf = poison.fvf;
        exactOverlap->outputFormat = poison.outputFormat;
        exactOverlap->valid = true;
      }
      result = NativeOutsidePoisonScanResult::Overlap;
      return true;
    }
  }
  result = NativeOutsidePoisonScanResult::NoOverlap;
  return true;
}

NativeOutsidePoisonScanResult ProveOutsideCpuUploadNoPoisonOverlap(
    uintptr_t gxDeviceD3d, uint32_t vertexCount,
    NativeOutsidePoisonScanObservation* scanObservation,
    NativeOutsidePoisonExactOverlap* exactOverlap) noexcept {
  if (scanObservation != nullptr) {
    *scanObservation = {};
    scanObservation->gxDeviceD3d = gxDeviceD3d;
    scanObservation->vertexCount = vertexCount;
  }
  if (exactOverlap != nullptr)
    *exactOverlap = {};
  const NativePoisonLedger& ledger = t_state.poisonLedger;
  if (ledger.count == 0u)
    return NativeOutsidePoisonScanResult::NoOverlap;
  if (ledger.count > ledger.ranges.size() || gxDeviceD3d == 0u ||
      vertexCount == 0u || vertexCount > kMaxNativeVertices ||
      kOutsidePoisonSnapshotBeginOffset >
          std::numeric_limits<uintptr_t>::max() - gxDeviceD3d) {
    return NativeOutsidePoisonScanResult::ReadFailure;
  }

  NativeOutsidePoisonScanResult bypassFastResult =
      NativeOutsidePoisonScanResult::ReadFailure;
  if (ProveVertexShaderBypassPoisonTarget(
          gxDeviceD3d, vertexCount, ledger, scanObservation, exactOverlap,
          bypassFastResult)) {
    return bypassFastResult;
  }

  const uintptr_t snapshotAddress =
      gxDeviceD3d + kOutsidePoisonSnapshotBeginOffset;
  if (!dxvk::war3::SafeCopy(
          t_outsidePoisonSnapshotScratch.data(),
          reinterpret_cast<const void*>(snapshotAddress),
          t_outsidePoisonSnapshotScratch.size())) {
    return NativeOutsidePoisonScanResult::ReadFailure;
  }
  if (scanObservation != nullptr)
    scanObservation->snapshotCaptured = true;

  uint32_t outputFormat = 0u;
  uint32_t nativeDevice32 = 0u;
  uint32_t vertexBuffer32 = 0u;
  uint32_t ringNext = 0u;
  if (!ReadOutsidePoisonSnapshotU32(
          kGxOutputFormatOffset, outputFormat) ||
      outputFormat >= kFormatFvf.size() ||
      !ReadOutsidePoisonSnapshotU32(
          kGxNativeDeviceOffset, nativeDevice32) ||
      !ReadOutsidePoisonSnapshotU32(
          kGxVertexBufferArrayOffset +
              size_t(outputFormat) * sizeof(uint32_t),
          vertexBuffer32) ||
      !ReadOutsidePoisonSnapshotU32(
          kGxVertexRingNextOffset + size_t(outputFormat) * 8u,
          ringNext) ||
      nativeDevice32 == 0u || vertexBuffer32 == 0u ||
      ringNext > kMaxNativeVertices) {
    return NativeOutsidePoisonScanResult::ReadFailure;
  }

  // IDA 0x6F0EE5D0: wrap only when oldNext + count exceeds 0x4000.
  const bool wraps =
      ringNext > kMaxNativeVertices - vertexCount;
  const uint64_t uploadBegin = wraps ? 0u : ringNext;
  const uint64_t uploadEnd = uploadBegin + vertexCount;
  const uintptr_t nativeDevice = nativeDevice32;
  const uintptr_t vertexBuffer = vertexBuffer32;
  const uint32_t stride = kFormatStride[outputFormat];
  const uint32_t fvf = kFormatFvf[outputFormat];
  if (scanObservation != nullptr) {
    scanObservation->nativeD3DDevice = nativeDevice;
    scanObservation->comVertexBuffer = vertexBuffer;
    scanObservation->outputFormat = outputFormat;
    scanObservation->ringNext = ringNext;
    scanObservation->predictedBaseVertex =
        static_cast<uint32_t>(uploadBegin);
    scanObservation->targetParsed = true;
  }

  for (size_t i = 0u; i < ledger.count; ++i) {
    const NativePoisonRange& poison = ledger.ranges[i];
    const NativeVertexOutputProof& proof = poison.vertexOutputProof;
    const uint64_t poisonEnd = PoisonRangeEnd(poison);
    const bool poisonProofComplete =
        proof.resource.commonResource != 0u &&
        proof.resource.comVertexBuffer != 0u &&
        proof.resource.resourceGeneration != 0u &&
        proof.nativeD3DDevice != 0u &&
        proof.outputFormat < kFormatFvf.size() &&
        proof.vertexStride == kFormatStride[proof.outputFormat] &&
        proof.fvf == kFormatFvf[proof.outputFormat] &&
        poison.vertexCount != 0u && poisonEnd <= kMaxNativeVertices &&
        poison.outputFormat == proof.outputFormat &&
        poison.stride == proof.vertexStride &&
        poison.fvf == proof.fvf;
    if (!poisonProofComplete)
      return NativeOutsidePoisonScanResult::ReadFailure;

    // This is the exact physical overwrite key used by
    // PredictedCpuUploadMayOverlapPoison. A complete, explicitly different
    // key cannot refer to the live native lock target; incomplete keys above
    // always fail closed.
    if (proof.nativeD3DDevice != nativeDevice ||
        proof.resource.comVertexBuffer != vertexBuffer ||
        proof.outputFormat != outputFormat ||
        proof.vertexStride != stride || proof.fvf != fvf) {
      continue;
    }
    if (uploadEnd > poison.baseVertex && uploadBegin < poisonEnd) {
      if (exactOverlap != nullptr) {
        exactOverlap->vertexOutputProof = proof;
        exactOverlap->baseVertex = poison.baseVertex;
        exactOverlap->vertexCount = poison.vertexCount;
        exactOverlap->stride = poison.stride;
        exactOverlap->fvf = poison.fvf;
        exactOverlap->outputFormat = poison.outputFormat;
        exactOverlap->valid = true;
      }
      return NativeOutsidePoisonScanResult::Overlap;
    }
  }
  return NativeOutsidePoisonScanResult::NoOverlap;
}

void MarkOutsidePoisonShadowUnprovable(
    NativeOutsidePoisonShadowProbe& probe,
    NativeOutsidePoisonShadowUnprovableReason reason) noexcept {
  if (probe.unprovableReason ==
      NativeOutsidePoisonShadowUnprovableReason::Count) {
    probe.unprovableReason = reason;
  }
}

void MarkOutsidePoisonO1ShadowUnprovable(
    NativeOutsidePoisonShadowProbe& probe,
    NativeOutsidePoisonO1ShadowUnprovableReason reason) noexcept {
  if (probe.o1UnprovableReason ==
      NativeOutsidePoisonO1ShadowUnprovableReason::Count) {
    probe.o1UnprovableReason = reason;
  }
}

bool OutsidePoisonProbeCollectsO0(
    const NativeOutsidePoisonShadowProbe& probe) noexcept {
  return GpuSkinOutsidePoisonO0Enabled(probe.sidecarPolicy);
}

bool OutsidePoisonProbeCollectsO1(
    const NativeOutsidePoisonShadowProbe& probe) noexcept {
  return GpuSkinOutsidePoisonO1Enabled(probe.sidecarPolicy);
}

void ValidateOutsidePoisonO1ShadowAtKernelEntry(
    uintptr_t gxDeviceD3d, void* mappedDst) noexcept;
void ValidateOutsidePoisonAuthorityAtKernelEntry(
    uintptr_t gxDeviceD3d, void* mappedDst) noexcept;

void CaptureOutsidePoisonO1KernelState(
    uintptr_t gxDeviceD3d, void* mappedDst) noexcept {
  if (OutsidePoisonO1SidecarEnabled())
    ValidateOutsidePoisonO1ShadowAtKernelEntry(gxDeviceD3d, mappedDst);
  ValidateOutsidePoisonAuthorityAtKernelEntry(gxDeviceD3d, mappedDst);
}

uint64_t NextOutsidePoisonShadowCookie() noexcept {
  do {
    ++t_outsidePoisonShadow.cookieSequence;
    if (t_outsidePoisonShadow.cookieSequence == 0u)
      ++t_outsidePoisonShadow.cookieSequence;
  } while (t_outsidePoisonShadow.cookieSequence ==
           kOutsidePoisonShadowOverflowCookie);
  return t_outsidePoisonShadow.cookieSequence;
}

void SubtractOutsidePoisonShadowActive(uint64_t count) noexcept {
  uint64_t current =
      g_counters.productionOutsidePoisonShadowActive.load(
          std::memory_order_relaxed);
  while (current != 0u) {
    const uint64_t next = current > count ? current - count : 0u;
    if (g_counters.productionOutsidePoisonShadowActive.compare_exchange_weak(
            current, next, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      return;
    }
  }
}

void SubtractOutsidePoisonO1ShadowActive(uint64_t count) noexcept {
  uint64_t current =
      g_counters.productionOutsidePoisonO1ShadowActive.load(
          std::memory_order_relaxed);
  while (current != 0u) {
    const uint64_t next = current > count ? current - count : 0u;
    if (g_counters.productionOutsidePoisonO1ShadowActive.compare_exchange_weak(
            current, next, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      return;
    }
  }
}

uint64_t BeginOutsidePoisonShadow(
    uintptr_t gxDeviceD3d, uint32_t vertexCount,
    uint64_t outputByteCount, uint64_t resetGeneration,
    bool authorityCandidate, bool authorityEvidence) noexcept {
  const bool o0Enabled = OutsidePoisonO0SidecarEnabled();
  const bool o1Enabled = OutsidePoisonO1SidecarEnabled();
  if (!o0Enabled && !o1Enabled && !authorityCandidate)
    return 0u;
  if (o0Enabled) {
    g_counters.productionOutsidePoisonShadowAttempts.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (o1Enabled) {
    g_counters.productionOutsidePoisonO1ShadowAttempts.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (authorityCandidate) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsidePoisonAuthorityAttempts,
        t_nativeTelemetryDelta.productionOutsidePoisonAuthorityAttempts);
    if (authorityEvidence) {
      g_counters.productionOutsidePoisonAuthorityEvidenceAttempts.fetch_add(
          1u, std::memory_order_relaxed);
    }
  }

  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (state.depth != 0u) {
    // A nested outer upload can advance the same native ring between the
    // parent's SafeCopy and Lock. LIFO keeps both lifetimes paired, but the
    // parent is no longer independently comparable.
    if (OutsidePoisonProbeCollectsO0(
            state.probes[state.depth - 1u])) {
      MarkOutsidePoisonShadowUnprovable(
          state.probes[state.depth - 1u],
          NativeOutsidePoisonShadowUnprovableReason::Reentry);
    }
    if (OutsidePoisonProbeCollectsO1(
            state.probes[state.depth - 1u])) {
      MarkOutsidePoisonO1ShadowUnprovable(
          state.probes[state.depth - 1u],
          NativeOutsidePoisonO1ShadowUnprovableReason::Reentry);
    }
    if (state.probes[state.depth - 1u].authorityArmed)
      state.probes[state.depth - 1u].authorityConflicted = true;
  }
  if (state.overflowDepth != 0u || state.depth >= state.probes.size()) {
    ++state.overflowDepth;
    if (o0Enabled) {
      g_counters.productionOutsidePoisonShadowOverflow.fetch_add(
          1u, std::memory_order_relaxed);
    }
    if (o1Enabled) {
      g_counters.productionOutsidePoisonO1ShadowOverflow.fetch_add(
          1u, std::memory_order_relaxed);
    }
    if (authorityCandidate) {
      g_counters.productionOutsidePoisonAuthorityOverflow.fetch_add(
          1u, std::memory_order_relaxed);
      if (authorityEvidence) {
        // Overflow has no probe slot which could later publish the actual-Lock
        // comparison. Classify the evidence attempt at its creation endpoint
        // so the evidence lifetime remains exact without weakening admission.
        g_counters.productionOutsidePoisonAuthorityEvidenceUnprovable
            .fetch_add(1u, std::memory_order_relaxed);
      }
    }
    return kOutsidePoisonShadowOverflowCookie;
  }

  NativeOutsidePoisonShadowProbe& probe = state.probes[state.depth++];
  probe = {};
  probe.sidecarPolicy = g_runtimeConfig.outsidePoisonSidecarPolicy;
  probe.cookie = NextOutsidePoisonShadowCookie();
  probe.ownerThreadId = ::GetCurrentThreadId();
  probe.resetGeneration = resetGeneration;
  probe.flushEpoch = t_state.flushEpoch;
  probe.poisonMutationGeneration = t_poisonMutationGeneration;
  probe.poisonOutstanding =
      g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
  probe.gxDeviceD3d = gxDeviceD3d;
  probe.outputByteCount = outputByteCount;
  probe.vertexCount = vertexCount;
  probe.poisonCount = static_cast<uint32_t>(t_state.poisonLedger.count);
  probe.authorityCandidate = authorityCandidate;
  probe.authorityEvidence = authorityEvidence;
  if (o0Enabled) {
    g_counters.productionOutsidePoisonShadowCreated.fetch_add(
        1u, std::memory_order_relaxed);
    g_counters.productionOutsidePoisonShadowActive.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (o1Enabled) {
    g_counters.productionOutsidePoisonO1ShadowCreated.fetch_add(
        1u, std::memory_order_relaxed);
    g_counters.productionOutsidePoisonO1ShadowActive.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (authorityCandidate) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsidePoisonAuthorityCreated,
        t_nativeTelemetryDelta.productionOutsidePoisonAuthorityCreated);
  }
  return probe.cookie;
}

bool ArmOutsidePoisonAuthority(
    uint64_t cookie, bool usesActualLock, bool legacyBacked) noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (cookie == 0u || cookie == kOutsidePoisonShadowOverflowCookie ||
      state.overflowDepth != 0u || state.depth == 0u ||
      state.probes[state.depth - 1u].cookie != cookie) {
    return false;
  }
  NativeOutsidePoisonShadowProbe& probe = state.probes[state.depth - 1u];
  if (!probe.authorityCandidate || probe.authorityArmed ||
      probe.ownerThreadId != ::GetCurrentThreadId()) {
    probe.authorityConflicted = true;
    return false;
  }
  probe.authorityArmed = true;
  probe.authorityUsesActualLock = usesActualLock;
  probe.authorityLegacyBacked = legacyBacked;
  probe.authorityActivePublished = true;
  g_nativeOutsidePoisonAuthorityActive.fetch_add(
      1u, std::memory_order_acq_rel);
  AddNativeTelemetryCounter(
      g_counters.productionOutsidePoisonAuthorityArmed,
      t_nativeTelemetryDelta.productionOutsidePoisonAuthorityArmed);
  return true;
}

void ReleaseOutsidePoisonAuthorityActive(
    NativeOutsidePoisonShadowProbe& probe) noexcept {
  if (!probe.authorityActivePublished)
    return;
  probe.authorityActivePublished = false;
  const uint32_t previous = g_nativeOutsidePoisonAuthorityActive.fetch_sub(
      1u, std::memory_order_acq_rel);
  if (previous == 0u) {
    g_nativeOutsidePoisonAuthorityActive.store(0u, std::memory_order_release);
    probe.authorityConflicted = true;
  }
}

void RecordOutsidePoisonShadowOldResult(
    uint64_t cookie, NativeOutsidePoisonScanResult result,
    const NativeOutsidePoisonScanObservation& observation,
    const NativeOutsidePoisonExactOverlap& exactOverlap) noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (cookie == 0u || cookie == kOutsidePoisonShadowOverflowCookie)
    return;
  if (state.overflowDepth != 0u || state.depth == 0u ||
      state.probes[state.depth - 1u].cookie != cookie) {
    if (state.depth != 0u) {
      NativeOutsidePoisonShadowProbe& top =
          state.probes[state.depth - 1u];
      if (OutsidePoisonProbeCollectsO0(top)) {
        MarkOutsidePoisonShadowUnprovable(
            top,
            NativeOutsidePoisonShadowUnprovableReason::OwnerOrLifo);
      }
      if (OutsidePoisonProbeCollectsO1(top)) {
        MarkOutsidePoisonO1ShadowUnprovable(
            top,
            NativeOutsidePoisonO1ShadowUnprovableReason::OwnerOrLifo);
      }
    }
    return;
  }
  NativeOutsidePoisonShadowProbe& probe = state.probes[state.depth - 1u];
  probe.oldResult = result;
  probe.oldObservation = observation;
  if (result == NativeOutsidePoisonScanResult::Overlap &&
      exactOverlap.valid &&
      (OutsidePoisonProbeCollectsO1(probe) || probe.authorityEvidence)) {
    probe.oldExactOverlap = exactOverlap;
    if (OutsidePoisonProbeCollectsO1(probe)) {
      g_counters.productionOutsidePoisonO1ShadowOldOverlapFrozen.fetch_add(
          1u, std::memory_order_relaxed);
    }
  }
  probe.oldResultRecorded = true;
}

void LatchOutsidePoisonShadowLifecycleFailure(uint64_t cookie) noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (cookie == 0u || cookie == kOutsidePoisonShadowOverflowCookie)
    return;
  if (state.overflowDepth != 0u || state.depth == 0u ||
      state.probes[state.depth - 1u].cookie != cookie) {
    if (state.depth != 0u) {
      NativeOutsidePoisonShadowProbe& top =
          state.probes[state.depth - 1u];
      if (OutsidePoisonProbeCollectsO0(top)) {
        MarkOutsidePoisonShadowUnprovable(
            top,
            NativeOutsidePoisonShadowUnprovableReason::OwnerOrLifo);
      }
      if (OutsidePoisonProbeCollectsO1(top)) {
        MarkOutsidePoisonO1ShadowUnprovable(
            top,
            NativeOutsidePoisonO1ShadowUnprovableReason::OwnerOrLifo);
      }
    }
    return;
  }
  NativeOutsidePoisonShadowProbe& probe =
      state.probes[state.depth - 1u];
  if (OutsidePoisonProbeCollectsO0(probe)) {
    MarkOutsidePoisonShadowUnprovable(
        probe,
        NativeOutsidePoisonShadowUnprovableReason::ResetOrRetirement);
  }
  if (OutsidePoisonProbeCollectsO1(probe)) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe,
        NativeOutsidePoisonO1ShadowUnprovableReason::ResetOrRetirement);
  }
  if (probe.authorityCandidate)
    probe.authorityConflicted = true;
}

bool ResolveOutsidePoisonShadowFormat(
    uint32_t fvf, uint32_t& outputFormat,
    uint32_t& vertexStride) noexcept {
  size_t matches = 0u;
  for (size_t i = 0u; i < kFormatFvf.size(); ++i) {
    if (kFormatFvf[i] != fvf)
      continue;
    outputFormat = static_cast<uint32_t>(i);
    vertexStride = kFormatStride[i];
    ++matches;
  }
  return matches == 1u;
}

bool OutsidePoisonShadowStorageComplete(
    const NativeOutsidePoisonVertexLockEvidence& evidence) noexcept {
  const bool pointerShapeExact = evidence.realBuffer != 0u &&
      evidence.mappingBuffer != 0u && evidence.mappedAllocation != 0u &&
      evidence.mappedAllocationBase != 0u && evidence.mappedPointer != 0u;
  const bool generationsExact =
      evidence.storage.realStorageGeneration != 0u &&
      evidence.storage.mappingStorageGeneration != 0u &&
      evidence.storage.mapAllocationGeneration != 0u;
  const bool sharedStorageExact =
      evidence.realBuffer != evidence.mappingBuffer ||
      evidence.storage.realStorageGeneration ==
          evidence.storage.mappingStorageGeneration;
  return pointerShapeExact && generationsExact && sharedStorageExact &&
      evidence.storage.mapMode <= kNativeVertexMapModeDirect;
}

bool OutsidePoisonShadowStorageMatches(
    const NativePoisonRange& poison,
    const NativeOutsidePoisonVertexLockEvidence& evidence) noexcept {
  const NativeVertexStorageDiagnostics& current = evidence.storage;
  const NativeVertexStorageDiagnostics& origin =
      poison.storageDiagnostics;
  return !poison.realStorageMixed && !poison.mappingStorageMixed &&
      !poison.mapAllocationMixed && !poison.mapModeMixed &&
      origin.realStorageGeneration != 0u &&
      origin.realStorageGeneration == current.realStorageGeneration &&
      origin.mappingStorageGeneration != 0u &&
      origin.mappingStorageGeneration == current.mappingStorageGeneration &&
      origin.mapAllocationGeneration != 0u &&
      origin.mapAllocationGeneration == current.mapAllocationGeneration &&
      origin.mapMode == current.mapMode;
}

NativeOutsidePoisonScanResult ScanOutsidePoisonShadowLock(
    const NativeOutsidePoisonVertexLockEvidence& evidence,
    uint32_t outputFormat, uint32_t vertexStride,
    uint32_t baseVertex, uint32_t vertexCount) noexcept {
  const uint64_t lockEnd = uint64_t(baseVertex) + vertexCount;
  const NativePoisonLedger& ledger = t_state.poisonLedger;
  if (ledger.count > ledger.ranges.size())
    return NativeOutsidePoisonScanResult::ReadFailure;
  for (size_t i = 0u; i < ledger.count; ++i) {
    const NativePoisonRange& poison = ledger.ranges[i];
    const NativeVertexOutputProof& proof = poison.vertexOutputProof;
    const uint64_t poisonEnd = PoisonRangeEnd(poison);
    const bool poisonProofComplete =
        proof.resource.commonResource != 0u &&
        proof.resource.comVertexBuffer != 0u &&
        proof.resource.resourceGeneration != 0u &&
        proof.nativeD3DDevice != 0u &&
        proof.outputFormat < kFormatFvf.size() &&
        proof.vertexStride == kFormatStride[proof.outputFormat] &&
        proof.fvf == kFormatFvf[proof.outputFormat] &&
        poison.vertexCount != 0u && poisonEnd <= kMaxNativeVertices &&
        poison.outputFormat == proof.outputFormat &&
        poison.stride == proof.vertexStride && poison.fvf == proof.fvf;
    if (!poisonProofComplete)
      return NativeOutsidePoisonScanResult::ReadFailure;

    if (proof.nativeD3DDevice != evidence.nativeD3DDevice ||
        proof.resource.comVertexBuffer !=
            evidence.resource.comVertexBuffer ||
        proof.outputFormat != outputFormat ||
        proof.vertexStride != vertexStride || proof.fvf != evidence.descFvf) {
      continue;
    }

    // Same physical COM/layout with a different common-resource generation or
    // backing generation is an ABA/incomplete proof, never a different target.
    if (proof.resource.commonResource != evidence.resource.commonResource ||
        proof.resource.resourceGeneration !=
            evidence.resource.resourceGeneration ||
        !OutsidePoisonShadowStorageMatches(poison, evidence)) {
      return NativeOutsidePoisonScanResult::ReadFailure;
    }
    if (lockEnd > poison.baseVertex && baseVertex < poisonEnd)
      return NativeOutsidePoisonScanResult::Overlap;
  }
  return NativeOutsidePoisonScanResult::NoOverlap;
}

enum class NativeOutsidePoisonO1TargetMatch : uint8_t {
  DifferentTarget = 0,
  ExactTarget,
  ReadFailure,
};

constexpr NativeOutsidePoisonO1TargetMatch
ClassifyOutsidePoisonO1TargetMatch(
    bool commonResourceExact, bool comVertexBufferExact,
    bool nativeDeviceExact, bool layoutExact,
    bool resourceGenerationExact) noexcept {
  if (!commonResourceExact && !comVertexBufferExact)
    return NativeOutsidePoisonO1TargetMatch::DifferentTarget;
  if (commonResourceExact && comVertexBufferExact && nativeDeviceExact &&
      layoutExact && resourceGenerationExact) {
    return NativeOutsidePoisonO1TargetMatch::ExactTarget;
  }
  return NativeOutsidePoisonO1TargetMatch::ReadFailure;
}

constexpr bool ValidateOutsidePoisonO1TargetMatchTruthTable() noexcept {
  for (uint32_t bits = 0u; bits != 32u; ++bits) {
    const bool commonResourceExact = (bits & (1u << 0u)) != 0u;
    const bool comVertexBufferExact = (bits & (1u << 1u)) != 0u;
    const bool nativeDeviceExact = (bits & (1u << 2u)) != 0u;
    const bool layoutExact = (bits & (1u << 3u)) != 0u;
    const bool resourceGenerationExact = (bits & (1u << 4u)) != 0u;
    const NativeOutsidePoisonO1TargetMatch result =
        ClassifyOutsidePoisonO1TargetMatch(
            commonResourceExact, comVertexBufferExact, nativeDeviceExact,
            layoutExact, resourceGenerationExact);
    const bool differentTarget =
        !commonResourceExact && !comVertexBufferExact;
    const bool exactTarget = commonResourceExact && comVertexBufferExact &&
        nativeDeviceExact && layoutExact && resourceGenerationExact;
    if ((result == NativeOutsidePoisonO1TargetMatch::DifferentTarget) !=
            differentTarget ||
        (result == NativeOutsidePoisonO1TargetMatch::ExactTarget) !=
            exactTarget ||
        (result == NativeOutsidePoisonO1TargetMatch::ReadFailure) !=
            (!differentTarget && !exactTarget)) {
      return false;
    }
  }
  return true;
}

static_assert(ValidateOutsidePoisonO1TargetMatchTruthTable());
static_assert(
    ClassifyOutsidePoisonO1TargetMatch(
        true, false, true, true, true) ==
    NativeOutsidePoisonO1TargetMatch::ReadFailure);
static_assert(
    ClassifyOutsidePoisonO1TargetMatch(
        false, true, true, true, true) ==
    NativeOutsidePoisonO1TargetMatch::ReadFailure);

enum class NativeOutsidePoisonO1MutationWindow : uint8_t {
  Stable = 0,
  PreLockAllowed,
  LockToKernelFailure,
};

constexpr NativeOutsidePoisonO1MutationWindow
ClassifyOutsidePoisonO1MutationWindow(
    bool preLockMutation, bool lockToKernelMutation,
    bool /*postKernelMutation*/) noexcept {
  if (lockToKernelMutation)
    return NativeOutsidePoisonO1MutationWindow::LockToKernelFailure;
  return preLockMutation
      ? NativeOutsidePoisonO1MutationWindow::PreLockAllowed
      : NativeOutsidePoisonO1MutationWindow::Stable;
}

constexpr bool ValidateOutsidePoisonO1MutationTruthTable() noexcept {
  for (uint32_t bits = 0u; bits != 8u; ++bits) {
    const bool preLockMutation = (bits & (1u << 0u)) != 0u;
    const bool lockToKernelMutation = (bits & (1u << 1u)) != 0u;
    const bool postKernelMutation = (bits & (1u << 2u)) != 0u;
    const NativeOutsidePoisonO1MutationWindow result =
        ClassifyOutsidePoisonO1MutationWindow(
            preLockMutation, lockToKernelMutation, postKernelMutation);
    const NativeOutsidePoisonO1MutationWindow expected = lockToKernelMutation
        ? NativeOutsidePoisonO1MutationWindow::LockToKernelFailure
        : (preLockMutation
            ? NativeOutsidePoisonO1MutationWindow::PreLockAllowed
            : NativeOutsidePoisonO1MutationWindow::Stable);
    if (result != expected)
      return false;
  }
  return true;
}

static_assert(ValidateOutsidePoisonO1MutationTruthTable());
static_assert(
    ClassifyOutsidePoisonO1MutationWindow(true, false, true) ==
    NativeOutsidePoisonO1MutationWindow::PreLockAllowed);
static_assert(
    ClassifyOutsidePoisonO1MutationWindow(false, true, false) ==
    NativeOutsidePoisonO1MutationWindow::LockToKernelFailure);

constexpr NativeOutsidePoisonO1LockLane ClassifyOutsidePoisonO1LockLane(
    uint32_t mapMode, uint32_t flags) noexcept {
  if (mapMode == kNativeVertexMapModeBuffer) {
    if (flags == kNativeVertexRingLockNoOverwrite)
      return NativeOutsidePoisonO1LockLane::BufferNoOverwrite;
    if (flags == kNativeVertexRingLockDiscard)
      return NativeOutsidePoisonO1LockLane::BufferDiscard;
  } else if (mapMode == kNativeVertexMapModeDirect) {
    if (flags == kNativeVertexRingLockNoOverwrite)
      return NativeOutsidePoisonO1LockLane::DirectNoOverwrite;
    if (flags == kNativeVertexRingLockDiscard)
      return NativeOutsidePoisonO1LockLane::DirectDiscard;
  }
  return NativeOutsidePoisonO1LockLane::Count;
}

static_assert(
    ClassifyOutsidePoisonO1LockLane(
        kNativeVertexMapModeBuffer, kNativeVertexRingLockNoOverwrite) ==
    NativeOutsidePoisonO1LockLane::BufferNoOverwrite);
static_assert(
    ClassifyOutsidePoisonO1LockLane(
        kNativeVertexMapModeBuffer, kNativeVertexRingLockDiscard) ==
    NativeOutsidePoisonO1LockLane::BufferDiscard);
static_assert(
    ClassifyOutsidePoisonO1LockLane(
        kNativeVertexMapModeDirect, kNativeVertexRingLockNoOverwrite) ==
    NativeOutsidePoisonO1LockLane::DirectNoOverwrite);
static_assert(
    ClassifyOutsidePoisonO1LockLane(
        kNativeVertexMapModeDirect, kNativeVertexRingLockDiscard) ==
    NativeOutsidePoisonO1LockLane::DirectDiscard);

struct NativeOutsidePoisonO1ScanOutcome {
  NativeOutsidePoisonScanResult result =
      NativeOutsidePoisonScanResult::ReadFailure;
  NativeOutsidePoisonO1ScanFailureReason failureReason =
      NativeOutsidePoisonO1ScanFailureReason::LedgerIncomplete;
  uint32_t outputFormat = 0u;
  uint32_t vertexStride = 0u;
  uint32_t baseVertex = 0u;
  uint32_t vertexCount = 0u;
  bool differentTarget = false;
  bool storageDiagnosticMismatch = false;
  bool realStorageDrift = false;
  bool mappingStorageDrift = false;
  bool mapAllocationDiagnosticMismatch = false;
};

NativeOutsidePoisonO1ScanOutcome ScanOutsidePoisonO1ShadowLock(
    const NativeOutsidePoisonVertexLockEvidence& evidence,
    uint32_t expectedVertexCount, uint64_t outputByteCount) noexcept {
  NativeOutsidePoisonO1ScanOutcome outcome = {};
  const NativeVertexResourceIdentity& current = evidence.resource;
  const NativePoisonLedger& ledger = t_state.poisonLedger;
  if (t_poisonMutationGeneration == 0u ||
      ledger.count > ledger.ranges.size()) {
    outcome.failureReason =
        NativeOutsidePoisonO1ScanFailureReason::LedgerIncomplete;
    return outcome;
  }
  if (current.commonResource == 0u || current.comVertexBuffer == 0u ||
      current.resourceGeneration == 0u ||
      evidence.nativeD3DDevice == 0u) {
    outcome.failureReason =
        NativeOutsidePoisonO1ScanFailureReason::CurrentIncomplete;
    return outcome;
  }

  if (!ResolveOutsidePoisonShadowFormat(
          evidence.descFvf, outcome.outputFormat, outcome.vertexStride) ||
      uint64_t(kMaxNativeVertices) * outcome.vertexStride !=
          evidence.descSize) {
    outcome.failureReason =
        NativeOutsidePoisonO1ScanFailureReason::Layout;
    return outcome;
  }

  const uint64_t expectedBytes =
      uint64_t(expectedVertexCount) * outcome.vertexStride;
  const bool rangeExact = expectedVertexCount != 0u &&
      evidence.mappedAllocation != 0u &&
      evidence.mappedAllocationBase != 0u && evidence.mappedPointer != 0u &&
      outcome.vertexStride != 0u &&
      evidence.offset % outcome.vertexStride == 0u &&
      evidence.size % outcome.vertexStride == 0u && evidence.size != 0u &&
      uint64_t(evidence.offset) + evidence.size <= evidence.descSize &&
      evidence.mappedAllocationBase <=
          std::numeric_limits<uintptr_t>::max() - evidence.offset &&
      evidence.mappedPointer == evidence.mappedAllocationBase + evidence.offset &&
      evidence.size == expectedBytes &&
      (outputByteCount == 0u || outputByteCount == expectedBytes);
  if (!rangeExact) {
    outcome.failureReason = NativeOutsidePoisonO1ScanFailureReason::Range;
    return outcome;
  }
  outcome.baseVertex = evidence.offset / outcome.vertexStride;
  outcome.vertexCount = evidence.size / outcome.vertexStride;
  const uint64_t lockEnd =
      uint64_t(outcome.baseVertex) + outcome.vertexCount;
  if (outcome.vertexCount != expectedVertexCount ||
      lockEnd > kMaxNativeVertices) {
    outcome.failureReason = NativeOutsidePoisonO1ScanFailureReason::Range;
    return outcome;
  }

  for (size_t i = 0u; i < ledger.count; ++i) {
    const NativePoisonRange& poison = ledger.ranges[i];
    const NativeVertexOutputProof& proof = poison.vertexOutputProof;
    if (proof.resource.commonResource == 0u ||
        proof.resource.comVertexBuffer == 0u) {
      outcome.failureReason =
          NativeOutsidePoisonO1ScanFailureReason::PoisonIncomplete;
      return outcome;
    }

    const bool commonResourceExact =
        proof.resource.commonResource == current.commonResource;
    const bool comVertexBufferExact =
        proof.resource.comVertexBuffer == current.comVertexBuffer;
    if (!commonResourceExact && !comVertexBufferExact) {
      outcome.differentTarget = true;
      continue;
    }
    if (!commonResourceExact || !comVertexBufferExact) {
      outcome.failureReason =
          NativeOutsidePoisonO1ScanFailureReason::PartialIdentity;
      return outcome;
    }
    if (proof.nativeD3DDevice == 0u) {
      outcome.failureReason =
          NativeOutsidePoisonO1ScanFailureReason::PoisonIncomplete;
      return outcome;
    }
    const bool nativeDeviceExact =
        proof.nativeD3DDevice == evidence.nativeD3DDevice;
    if (!nativeDeviceExact) {
      outcome.failureReason = NativeOutsidePoisonO1ScanFailureReason::Device;
      return outcome;
    }
    if (proof.resource.resourceGeneration == 0u) {
      outcome.failureReason =
          NativeOutsidePoisonO1ScanFailureReason::PoisonIncomplete;
      return outcome;
    }
    const bool resourceGenerationExact =
        proof.resource.resourceGeneration == current.resourceGeneration;
    if (!resourceGenerationExact) {
      outcome.failureReason =
          NativeOutsidePoisonO1ScanFailureReason::ResourceGeneration;
      return outcome;
    }

    const bool poisonLayoutComplete =
        proof.outputFormat < kFormatFvf.size() &&
        proof.vertexStride == kFormatStride[proof.outputFormat] &&
        proof.fvf == kFormatFvf[proof.outputFormat] &&
        poison.outputFormat == proof.outputFormat &&
        poison.stride == proof.vertexStride && poison.fvf == proof.fvf;
    if (!poisonLayoutComplete) {
      outcome.failureReason =
          NativeOutsidePoisonO1ScanFailureReason::PoisonIncomplete;
      return outcome;
    }
    const bool layoutExact = proof.outputFormat == outcome.outputFormat &&
        proof.vertexStride == outcome.vertexStride &&
        proof.fvf == evidence.descFvf;
    if (!layoutExact) {
      outcome.failureReason = NativeOutsidePoisonO1ScanFailureReason::Layout;
      return outcome;
    }

    const NativeOutsidePoisonO1TargetMatch targetMatch =
        ClassifyOutsidePoisonO1TargetMatch(
            commonResourceExact, comVertexBufferExact, nativeDeviceExact,
            layoutExact, resourceGenerationExact);
    if (targetMatch != NativeOutsidePoisonO1TargetMatch::ExactTarget)
      return outcome;

    const uint64_t poisonEnd = PoisonRangeEnd(poison);
    if (poison.vertexCount == 0u || poisonEnd > kMaxNativeVertices) {
      outcome.failureReason = NativeOutsidePoisonO1ScanFailureReason::Range;
      return outcome;
    }

    const NativeVertexStorageDiagnostics& origin = poison.storageDiagnostics;
    const NativeVertexStorageDiagnostics& observed = evidence.storage;
    const bool realStorageDrift = poison.realStorageMixed ||
        origin.realStorageGeneration == 0u ||
        observed.realStorageGeneration == 0u ||
        origin.realStorageGeneration != observed.realStorageGeneration;
    const bool mappingStorageDrift = poison.mappingStorageMixed ||
        origin.mappingStorageGeneration == 0u ||
        observed.mappingStorageGeneration == 0u ||
        origin.mappingStorageGeneration != observed.mappingStorageGeneration;
    const bool mapAllocationDiagnosticMismatch = poison.mapAllocationMixed ||
        origin.mapAllocationGeneration == 0u ||
        observed.mapAllocationGeneration == 0u ||
        origin.mapAllocationGeneration != observed.mapAllocationGeneration;
    const bool mapModeDrift = poison.mapModeMixed ||
        origin.mapMode != observed.mapMode;
    outcome.realStorageDrift |= realStorageDrift;
    outcome.mappingStorageDrift |= mappingStorageDrift;
    outcome.mapAllocationDiagnosticMismatch |=
        mapAllocationDiagnosticMismatch;
    outcome.storageDiagnosticMismatch |= realStorageDrift ||
        mappingStorageDrift || mapAllocationDiagnosticMismatch || mapModeDrift;

    if (lockEnd > poison.baseVertex &&
        outcome.baseVertex < poisonEnd) {
      outcome.result = NativeOutsidePoisonScanResult::Overlap;
      return outcome;
    }
  }
  outcome.result = NativeOutsidePoisonScanResult::NoOverlap;
  return outcome;
}

void RecordOutsidePoisonO1ScanOutcome(
    NativeOutsidePoisonO1LockLane lane,
    const NativeOutsidePoisonO1ScanOutcome& outcome) noexcept {
  const size_t laneIndex = static_cast<size_t>(lane);
  if (laneIndex >= kNativeOutsidePoisonO1LockLaneCount)
    return;
  g_counters.productionOutsidePoisonO1ShadowScanCallsByLane[laneIndex]
      .fetch_add(1u, std::memory_order_relaxed);
  if (outcome.result == NativeOutsidePoisonScanResult::NoOverlap) {
    g_counters.productionOutsidePoisonO1ShadowScanNoOverlapByLane[laneIndex]
        .fetch_add(1u, std::memory_order_relaxed);
  } else if (outcome.result == NativeOutsidePoisonScanResult::Overlap) {
    g_counters.productionOutsidePoisonO1ShadowScanOverlapByLane[laneIndex]
        .fetch_add(1u, std::memory_order_relaxed);
    g_counters.productionOutsidePoisonO1ShadowWouldClear.fetch_add(
        1u, std::memory_order_relaxed);
  } else {
    g_counters.productionOutsidePoisonO1ShadowScanReadFailureByLane[laneIndex]
        .fetch_add(1u, std::memory_order_relaxed);
    const size_t reasonIndex = static_cast<size_t>(outcome.failureReason);
    if (reasonIndex < kNativeOutsidePoisonO1ScanFailureReasonCount) {
      const size_t laneReasonIndex =
          laneIndex * kNativeOutsidePoisonO1ScanFailureReasonCount +
          reasonIndex;
      g_counters.productionOutsidePoisonO1ShadowScanFailuresByLane[
          laneReasonIndex].fetch_add(1u, std::memory_order_relaxed);
    }
  }
  if (outcome.differentTarget) {
    g_counters.productionOutsidePoisonO1ShadowScanDifferentTarget.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (outcome.storageDiagnosticMismatch) {
    g_counters.productionOutsidePoisonO1ShadowStorageDiagnosticMismatch
        .fetch_add(1u, std::memory_order_relaxed);
  }
  if (outcome.realStorageDrift) {
    g_counters.productionOutsidePoisonO1ShadowRealStorageDrift.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (outcome.mappingStorageDrift) {
    g_counters.productionOutsidePoisonO1ShadowMappingStorageDrift.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (outcome.mapAllocationDiagnosticMismatch) {
    g_counters
        .productionOutsidePoisonO1ShadowMapAllocationDiagnosticMismatch
        .fetch_add(1u, std::memory_order_relaxed);
  }
}

bool OutsidePoisonO1OldOverlapToNoOverlapFromExactDiscard(
    const NativeOutsidePoisonShadowProbe& probe) noexcept {
  const NativeOutsidePoisonScanObservation& oldTarget =
      probe.oldObservation;
  const NativeOutsidePoisonExactOverlap& oldOverlap =
      probe.oldExactOverlap;
  const NativeOutsidePoisonDirectDiscardObservation& discard =
      probe.directDiscardObservation;
  const NativeOutsidePoisonVertexLockEvidence& lock = probe.lockEvidence;
  const NativeVertexOutputProof& oldProof = oldOverlap.vertexOutputProof;

  if (!probe.oldResultRecorded ||
      probe.oldResult != NativeOutsidePoisonScanResult::Overlap ||
      !probe.o1LockVerdictRecorded ||
      probe.o1SidecarResult != NativeOutsidePoisonScanResult::NoOverlap ||
      probe.o1LockLane != NativeOutsidePoisonO1LockLane::DirectDiscard ||
      probe.lockNotifications != 1u ||
      probe.directDiscardNotifications != 1u ||
      !discard.exactOldOverlapRetired || discard.cleared == 0u ||
      discard.oldExactOverlapRangesCleared != 1u ||
      !OutsidePoisonExactOverlapMatchesOldTarget(oldOverlap, oldTarget) ||
      oldTarget.gxDeviceD3d != probe.gxDeviceD3d) {
    return false;
  }

  const bool identityExact =
      discard.commonResource == oldProof.resource.commonResource &&
      discard.resourceGeneration == oldProof.resource.resourceGeneration &&
      lock.resource.commonResource == oldProof.resource.commonResource &&
      lock.resource.comVertexBuffer == oldProof.resource.comVertexBuffer &&
      lock.resource.resourceGeneration == oldProof.resource.resourceGeneration &&
      lock.nativeD3DDevice == oldProof.nativeD3DDevice;
  const bool layoutAndRangeExact =
      probe.o1LockOutputFormat == oldProof.outputFormat &&
      probe.o1LockVertexStride == oldProof.vertexStride &&
      lock.descFvf == oldProof.fvf &&
      probe.o1LockBaseVertex == oldTarget.predictedBaseVertex &&
      probe.o1LockVertexCount == oldTarget.vertexCount &&
      probe.o1LockVertexCount == probe.vertexCount &&
      uint64_t(lock.offset) ==
          uint64_t(probe.o1LockBaseVertex) * probe.o1LockVertexStride &&
      uint64_t(lock.size) ==
          uint64_t(probe.o1LockVertexCount) * probe.o1LockVertexStride;
  const bool mapGenerationExact =
      NonZeroGenerationAdvancedOnce(
          discard.previousMapAllocationGeneration,
          discard.newMapAllocationGeneration) &&
      lock.storage.mapMode == kNativeVertexMapModeDirect &&
      lock.storage.mapAllocationGeneration ==
          discard.newMapAllocationGeneration;
  const bool discardToLockLedgerExact =
      discard.mutationGenerationAfter != 0u &&
      discard.mutationGenerationAfter == probe.o1LockLedgerGeneration &&
      discard.poisonCountAfter == probe.o1LockPoisonCount &&
      discard.poisonOutstandingAfter == probe.o1LockPoisonOutstanding &&
      discard.mutationGenerationAfter ==
          AdvancePoisonMutationGenerationBy(
              discard.mutationGenerationBefore, discard.cleared) &&
      discard.poisonCountBefore >= discard.cleared &&
      discard.poisonCountAfter ==
          discard.poisonCountBefore - discard.cleared &&
      discard.poisonOutstandingBefore >= discard.cleared &&
      discard.poisonOutstandingAfter ==
          discard.poisonOutstandingBefore - discard.cleared;
  return identityExact && layoutAndRangeExact && mapGenerationExact &&
      discardToLockLedgerExact;
}

void PublishOutsidePoisonO1LogicalComparison(
    NativeOutsidePoisonShadowProbe& probe) noexcept {
  if (probe.o1LogicalPublished || !probe.o1LockVerdictRecorded)
    return;
  probe.o1LogicalPublished = true;
  g_counters.productionOutsidePoisonO1ShadowComparable.fetch_add(
      1u, std::memory_order_relaxed);
  const size_t oldIndex = static_cast<size_t>(probe.oldResult);
  const size_t logicalIndex = static_cast<size_t>(probe.o1SidecarResult);
  const size_t matrixIndex = oldIndex * 3u + logicalIndex;
  if (probe.oldResultRecorded &&
      matrixIndex < g_counters.productionOutsidePoisonO1ShadowMatrix.size()) {
    if (probe.oldResult == NativeOutsidePoisonScanResult::Overlap &&
        probe.o1SidecarResult ==
            NativeOutsidePoisonScanResult::NoOverlap) {
      if (OutsidePoisonO1OldOverlapToNoOverlapFromExactDiscard(probe)) {
        g_counters
            .productionOutsidePoisonO1ShadowOldOverlapToNoOverlapDiscardExact
            .fetch_add(1u, std::memory_order_relaxed);
      } else {
        g_counters
            .productionOutsidePoisonO1ShadowOldOverlapToNoOverlapOther
            .fetch_add(1u, std::memory_order_relaxed);
      }
    }
    // Published at Lock freeze, before any kernel/Unlock/outer settlement.
    // This matrix is legacy x immutable logical verdict, never physical-ready.
    g_counters.productionOutsidePoisonO1ShadowMatrix[matrixIndex].fetch_add(
        1u, std::memory_order_relaxed);
  } else {
    g_counters.productionOutsidePoisonO1ShadowComparisonMissing.fetch_add(
        1u, std::memory_order_relaxed);
  }
}

void FreezeOutsidePoisonO1ShadowAtSuccessfulLock(
    NativeOutsidePoisonShadowProbe& probe) noexcept {
  const NativeOutsidePoisonVertexLockEvidence& evidence = probe.lockEvidence;
  probe.o1LockLane = ClassifyOutsidePoisonO1LockLane(
      evidence.storage.mapMode, evidence.effectiveFlags);
  const size_t laneIndex = static_cast<size_t>(probe.o1LockLane);

  // The baseline is intentionally captured after D3D9 has published ppbData,
  // activeLock and depth 1, and after DIRECT DISCARD retirement. Begin-to-Lock
  // mutations are diagnostic only and cannot invalidate this Lock verdict.
  probe.o1LockLedgerGeneration = t_poisonMutationGeneration;
  probe.o1LockPoisonCount =
      static_cast<uint32_t>(t_state.poisonLedger.count);
  probe.o1LockPoisonOutstanding =
      g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
  const bool preLockMutation =
      probe.poisonMutationGeneration != probe.o1LockLedgerGeneration ||
      probe.poisonCount != probe.o1LockPoisonCount ||
      probe.poisonOutstanding != probe.o1LockPoisonOutstanding;
  if (preLockMutation &&
      laneIndex < kNativeOutsidePoisonO1LockLaneCount) {
    g_counters.productionOutsidePoisonO1ShadowPreLockMutationByLane[laneIndex]
        .fetch_add(1u, std::memory_order_relaxed);
  }

  if (probe.ownerThreadId == 0u ||
      probe.ownerThreadId != ::GetCurrentThreadId()) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::OwnerOrLifo);
  } else if (probe.flushEpoch == 0u ||
             probe.flushEpoch != t_state.flushEpoch) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::Reentry);
  } else if (probe.resetGeneration !=
                 g_resetRequestedGeneration.load(std::memory_order_acquire) ||
             probe.resetGeneration !=
                 g_resetCompletedGeneration.load(std::memory_order_acquire) ||
             probe.resetGeneration != t_state.appliedResetGeneration ||
             g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
             g_retirementQueueFaulted.load(std::memory_order_acquire)) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe,
         NativeOutsidePoisonO1ShadowUnprovableReason::ResetOrRetirement);
  }

  if (probe.lockNotifications == 0u) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::NoLock);
  } else if (probe.lockNotifications != 1u) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::MultipleLocks);
  }

  const bool descriptorExact = probe.lockNotifications == 1u &&
      evidence.result >= 0 &&
      evidence.descType == kD3dResourceTypeVertexBuffer &&
      evidence.descPool == kD3dPoolDefault &&
      (evidence.descUsage & kD3dUsageDynamic) != 0u &&
      evidence.lockDepth == 1u && evidence.storage.activeLock &&
      evidence.storage.activeLockFlags == evidence.effectiveFlags &&
      evidence.requestedFlags == evidence.effectiveFlags &&
      laneIndex < kNativeOutsidePoisonO1LockLaneCount;
  if (!descriptorExact) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::LockDescriptor);
  }

  if (probe.o1UnprovableReason ==
      NativeOutsidePoisonO1ShadowUnprovableReason::Count) {
    NativeOutsidePoisonO1ScanOutcome outcome =
        ScanOutsidePoisonO1ShadowLock(
            evidence, probe.vertexCount, probe.outputByteCount);
    if (outcome.result != NativeOutsidePoisonScanResult::ReadFailure &&
        (probe.o1LockLedgerGeneration == 0u ||
         probe.o1LockLedgerGeneration != t_poisonMutationGeneration ||
         probe.o1LockPoisonCount != t_state.poisonLedger.count ||
         probe.o1LockPoisonOutstanding !=
             g_nativePoisonOutstandingRanges.load(
                 std::memory_order_acquire))) {
      outcome.result = NativeOutsidePoisonScanResult::ReadFailure;
      outcome.failureReason =
          NativeOutsidePoisonO1ScanFailureReason::LedgerIncomplete;
    }
    probe.o1SidecarResult = outcome.result;
    probe.o1LockOutputFormat = outcome.outputFormat;
    probe.o1LockVertexStride = outcome.vertexStride;
    probe.o1LockBaseVertex = outcome.baseVertex;
    probe.o1LockVertexCount = outcome.vertexCount;
    probe.o1LockVerdictRecorded = true;
    RecordOutsidePoisonO1ScanOutcome(probe.o1LockLane, outcome);
    PublishOutsidePoisonO1LogicalComparison(probe);
  }
}

void FreezeOutsidePoisonAuthorityAtSuccessfulLock(
    NativeOutsidePoisonShadowProbe& probe) noexcept {
  if (!probe.authorityArmed || probe.authorityLockTerminalRecorded)
    return;
  probe.authorityLockTerminalRecorded = true;
  const NativeOutsidePoisonVertexLockEvidence& evidence = probe.lockEvidence;
  const NativeOutsidePoisonO1LockLane lane =
      ClassifyOutsidePoisonO1LockLane(
          evidence.storage.mapMode, evidence.effectiveFlags);
  const bool lifecycleExact = probe.ownerThreadId != 0u &&
      probe.ownerThreadId == ::GetCurrentThreadId() &&
      probe.flushEpoch != 0u && probe.flushEpoch == t_state.flushEpoch &&
      probe.resetGeneration ==
          g_resetRequestedGeneration.load(std::memory_order_acquire) &&
      probe.resetGeneration ==
          g_resetCompletedGeneration.load(std::memory_order_acquire) &&
      probe.resetGeneration == t_state.appliedResetGeneration &&
      g_retirementEventsPending.load(std::memory_order_acquire) == 0u &&
      !g_retirementQueueFaulted.load(std::memory_order_acquire);
  const bool descriptorExact = probe.lockNotifications == 1u &&
      evidence.result >= 0 &&
      evidence.descType == kD3dResourceTypeVertexBuffer &&
      evidence.descPool == kD3dPoolDefault &&
      (evidence.descUsage & kD3dUsageDynamic) != 0u &&
      evidence.lockDepth == 1u && evidence.storage.activeLock &&
      evidence.storage.activeLockFlags == evidence.effectiveFlags &&
      evidence.requestedFlags == evidence.effectiveFlags &&
      lane != NativeOutsidePoisonO1LockLane::Count;

  NativeOutsidePoisonO1ScanOutcome outcome = {};
  if (lifecycleExact && descriptorExact) {
    probe.authorityLockPoisonGeneration = t_poisonMutationGeneration;
    probe.authorityLockPoisonCount =
        static_cast<uint32_t>(t_state.poisonLedger.count);
    probe.authorityLockPoisonOutstanding =
        g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
    outcome = ScanOutsidePoisonO1ShadowLock(
        evidence, probe.vertexCount, probe.outputByteCount);
    const bool ledgerExact =
        probe.authorityLockPoisonGeneration != 0u &&
        probe.authorityLockPoisonGeneration == t_poisonMutationGeneration &&
        probe.authorityLockPoisonCount == t_state.poisonLedger.count &&
        probe.authorityLockPoisonOutstanding ==
            g_nativePoisonOutstandingRanges.load(std::memory_order_acquire) &&
        probe.authorityLockPoisonOutstanding ==
            probe.authorityLockPoisonCount;
    if (!ledgerExact)
      outcome.result = NativeOutsidePoisonScanResult::ReadFailure;
  }

  probe.authorityLockResult = outcome.result;
  probe.authorityOutputFormat = outcome.outputFormat;
  probe.authorityVertexStride = outcome.vertexStride;
  probe.authorityBaseVertex = outcome.baseVertex;
  probe.authorityVertexCount = outcome.vertexCount;
  probe.authorityLockExact =
      outcome.result != NativeOutsidePoisonScanResult::ReadFailure;

  // Populate the shared immutable logical fields so an evidence sample can
  // reuse the established exact-DIRECT-DISCARD explanation without granting
  // report-only O1a any authority.
  if (!probe.o1LockVerdictRecorded) {
    probe.o1LockLane = lane;
    probe.o1LockLedgerGeneration = probe.authorityLockPoisonGeneration;
    probe.o1LockPoisonCount = probe.authorityLockPoisonCount;
    probe.o1LockPoisonOutstanding = probe.authorityLockPoisonOutstanding;
    probe.o1SidecarResult = outcome.result;
    probe.o1LockOutputFormat = outcome.outputFormat;
    probe.o1LockVertexStride = outcome.vertexStride;
    probe.o1LockBaseVertex = outcome.baseVertex;
    probe.o1LockVertexCount = outcome.vertexCount;
    probe.o1LockVerdictRecorded = probe.authorityLockExact;
  }

  if (outcome.result == NativeOutsidePoisonScanResult::NoOverlap) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsidePoisonAuthorityLockNoOverlap,
        t_nativeTelemetryDelta.productionOutsidePoisonAuthorityLockNoOverlap);
  } else if (outcome.result == NativeOutsidePoisonScanResult::Overlap) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsidePoisonAuthorityLockOverlap,
        t_nativeTelemetryDelta.productionOutsidePoisonAuthorityLockOverlap);
  } else {
    probe.authorityConflicted = true;
    g_counters.productionOutsidePoisonAuthorityLockRejects.fetch_add(
        1u, std::memory_order_relaxed);
  }

  if (probe.authorityEvidence) {
    probe.authorityEvidenceTerminalRecorded = true;
    if (!probe.oldResultRecorded || !probe.authorityLockExact) {
      g_counters.productionOutsidePoisonAuthorityEvidenceUnprovable.fetch_add(
          1u, std::memory_order_relaxed);
    } else {
      const size_t oldIndex = static_cast<size_t>(probe.oldResult);
      const size_t actualIndex = static_cast<size_t>(outcome.result);
      const size_t matrixIndex = oldIndex * 3u + actualIndex;
      if (matrixIndex <
          g_counters.productionOutsidePoisonAuthorityEvidenceMatrix.size()) {
        g_counters.productionOutsidePoisonAuthorityEvidenceMatrix[matrixIndex]
            .fetch_add(1u, std::memory_order_relaxed);
        g_counters.productionOutsidePoisonAuthorityEvidenceComparable
            .fetch_add(1u, std::memory_order_relaxed);
      }
      const bool exactDiscard =
          probe.oldResult == NativeOutsidePoisonScanResult::Overlap &&
          outcome.result == NativeOutsidePoisonScanResult::NoOverlap &&
          OutsidePoisonO1OldOverlapToNoOverlapFromExactDiscard(probe);
      if (probe.oldResult != outcome.result && !exactDiscard) {
        g_counters.productionOutsidePoisonAuthorityEvidenceMismatches
            .fetch_add(1u, std::memory_order_relaxed);
        probe.authorityConflicted = true;
      }
    }
  }
}

void ValidateOutsidePoisonO1ShadowAtKernelEntry(
    uintptr_t gxDeviceD3d, void* mappedDst) noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (state.overflowDepth != 0u || state.depth == 0u)
    return;

  NativeOutsidePoisonShadowProbe& probe = state.probes[state.depth - 1u];
  if (!OutsidePoisonProbeCollectsO1(probe))
    return;
  if (probe.o1Frozen || probe.frozen) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::Reentry);
    return;
  }

  g_counters.productionOutsidePoisonO1ShadowKernelNotifications.fetch_add(
      1u, std::memory_order_relaxed);
  ++probe.o1KernelNotifications;
  if (probe.o1KernelNotifications != 1u) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::MultipleKernels);
    return;
  }

  probe.o1KernelGxDeviceD3d = gxDeviceD3d;
  probe.o1KernelMappedDst = reinterpret_cast<uintptr_t>(mappedDst);
  probe.o1KernelFreezeGeneration = t_poisonMutationGeneration;

  if (probe.ownerThreadId == 0u ||
      probe.ownerThreadId != ::GetCurrentThreadId() ||
      probe.gxDeviceD3d == 0u || probe.gxDeviceD3d != gxDeviceD3d) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::OwnerOrLifo);
  } else if (probe.flushEpoch == 0u ||
             probe.flushEpoch != t_state.flushEpoch) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::Reentry);
  } else if (probe.resetGeneration !=
                 g_resetRequestedGeneration.load(std::memory_order_acquire) ||
             probe.resetGeneration !=
                 g_resetCompletedGeneration.load(std::memory_order_acquire) ||
             probe.resetGeneration != t_state.appliedResetGeneration ||
             g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
             g_retirementQueueFaulted.load(std::memory_order_acquire)) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe,
        NativeOutsidePoisonO1ShadowUnprovableReason::ResetOrRetirement);
  }

  if (probe.lockNotifications == 0u) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::NoLock);
  } else if (probe.lockNotifications != 1u) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::MultipleLocks);
  } else if (!probe.o1LockVerdictRecorded) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::LockDescriptor);
  }

  const bool lockToKernelMutation = probe.o1LockLedgerGeneration == 0u ||
      probe.o1LockLedgerGeneration != t_poisonMutationGeneration ||
      probe.o1LockPoisonCount != t_state.poisonLedger.count ||
      probe.o1LockPoisonOutstanding !=
          g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
  if (lockToKernelMutation) {
    const size_t laneIndex = static_cast<size_t>(probe.o1LockLane);
    if (laneIndex < kNativeOutsidePoisonO1LockLaneCount) {
      g_counters
          .productionOutsidePoisonO1ShadowLockToKernelMutationByLane[laneIndex]
          .fetch_add(1u, std::memory_order_relaxed);
    }
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::PoisonMutation);
  }

  const NativeOutsidePoisonVertexLockEvidence& evidence = probe.lockEvidence;
  const bool lockProofExact = probe.o1LockVerdictRecorded &&
      evidence.resource.commonResource != 0u &&
      evidence.resource.comVertexBuffer != 0u &&
      evidence.resource.resourceGeneration != 0u &&
      evidence.nativeD3DDevice != 0u &&
      probe.o1LockOutputFormat < kFormatFvf.size() &&
      probe.o1LockVertexStride ==
          kFormatStride[probe.o1LockOutputFormat] &&
      evidence.descFvf == kFormatFvf[probe.o1LockOutputFormat];
  if (!lockProofExact) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::ResourceIdentity);
  }

  const bool mappedRangeExact = lockProofExact &&
      evidence.mappedAllocation != 0u &&
      evidence.mappedAllocationBase != 0u && evidence.mappedPointer != 0u &&
      evidence.mappedAllocationBase <=
          std::numeric_limits<uintptr_t>::max() - evidence.offset &&
      evidence.mappedPointer == evidence.mappedAllocationBase + evidence.offset &&
      evidence.mappedPointer == reinterpret_cast<uintptr_t>(mappedDst) &&
      probe.o1LockBaseVertex ==
          evidence.offset / probe.o1LockVertexStride &&
      probe.o1LockVertexCount ==
          evidence.size / probe.o1LockVertexStride &&
      probe.o1LockVertexCount == probe.vertexCount &&
      uint64_t(probe.o1LockBaseVertex) + probe.o1LockVertexCount <=
          kMaxNativeVertices;
  if (!mappedRangeExact) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::KernelMappedDst);
  }

  std::array<uint32_t, 2u> kernelState = {};
  const bool kernelAddressExact = gxDeviceD3d != 0u &&
      kGxSkinModeOffset <=
          std::numeric_limits<uintptr_t>::max() - gxDeviceD3d &&
      gxDeviceD3d + kGxSkinModeOffset <=
          std::numeric_limits<uintptr_t>::max() -
              (sizeof(kernelState) - 1u);
  if (!kernelAddressExact ||
      !dxvk::war3::SafeCopy(
          kernelState.data(),
          reinterpret_cast<const void*>(gxDeviceD3d + kGxSkinModeOffset),
          sizeof(kernelState))) {
    MarkOutsidePoisonO1ShadowUnprovable(
        probe, NativeOutsidePoisonO1ShadowUnprovableReason::KernelStateRead);
  } else {
    // IDA 0x6F0EDDC0 consumes GX +0x224/+0x228 before the original kernel.
    probe.o1KernelSkinMode = kernelState[0u];
    probe.o1KernelOutputFormat = kernelState[1u];
    probe.o1KernelStateCaptured = true;
    if (probe.o1KernelSkinMode > 1u) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe, NativeOutsidePoisonO1ShadowUnprovableReason::KernelMode);
    } else if (probe.o1KernelOutputFormat >= kFormatFvf.size() ||
               probe.o1KernelOutputFormat != probe.o1LockOutputFormat ||
               kFormatFvf[probe.o1KernelOutputFormat] != evidence.descFvf ||
               kFormatStride[probe.o1KernelOutputFormat] !=
                   probe.o1LockVertexStride) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe, NativeOutsidePoisonO1ShadowUnprovableReason::KernelFormat);
    }
  }

  probe.o1Frozen = true;
  g_counters.productionOutsidePoisonO1ShadowFrozen.fetch_add(
      1u, std::memory_order_relaxed);
}

void ValidateOutsidePoisonAuthorityAtKernelEntry(
    uintptr_t gxDeviceD3d, void* mappedDst) noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (state.overflowDepth != 0u || state.depth == 0u)
    return;
  NativeOutsidePoisonShadowProbe& probe = state.probes[state.depth - 1u];
  if (!probe.authorityArmed || probe.authorityKernelTerminalRecorded)
    return;
  probe.authorityKernelTerminalRecorded = true;
  if (!OutsidePoisonProbeCollectsO1(probe))
    ++probe.o1KernelNotifications;

  if (probe.authorityFastNoOverlap) {
    const bool ledgerExact =
        probe.authorityLockPoisonGeneration != 0u &&
        probe.authorityLockPoisonGeneration == t_poisonMutationGeneration &&
        probe.authorityLockPoisonCount == t_state.poisonLedger.count &&
        probe.authorityLockPoisonOutstanding ==
            g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
    const bool kernelExact = !probe.authorityConflicted &&
        probe.authorityLockExact && probe.lockNotifications == 1u &&
        probe.o1KernelNotifications == 1u &&
        probe.ownerThreadId == ::GetCurrentThreadId() &&
        probe.gxDeviceD3d == gxDeviceD3d && mappedDst != nullptr &&
        ledgerExact;
    probe.o1KernelGxDeviceD3d = gxDeviceD3d;
    probe.o1KernelMappedDst = reinterpret_cast<uintptr_t>(mappedDst);
    probe.o1KernelFreezeGeneration = t_poisonMutationGeneration;
    probe.authorityKernelExact = kernelExact;
    if (kernelExact) {
      AddNativeTelemetryCounter(
          g_counters.productionOutsidePoisonAuthorityKernelReady,
          t_nativeTelemetryDelta.productionOutsidePoisonAuthorityKernelReady);
    } else {
      probe.authorityConflicted = true;
      g_counters.productionOutsidePoisonAuthorityKernelRejects.fetch_add(
          1u, std::memory_order_relaxed);
    }
    return;
  }

  const NativeOutsidePoisonVertexLockEvidence& evidence = probe.lockEvidence;
  const bool ledgerExact = probe.authorityLockPoisonGeneration != 0u &&
      probe.authorityLockPoisonGeneration == t_poisonMutationGeneration &&
      probe.authorityLockPoisonCount == t_state.poisonLedger.count &&
      probe.authorityLockPoisonOutstanding ==
          g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
  const bool mappedExact = probe.authorityLockExact &&
      probe.lockNotifications == 1u && probe.o1KernelNotifications == 1u &&
      probe.ownerThreadId == ::GetCurrentThreadId() &&
      probe.gxDeviceD3d == gxDeviceD3d && mappedDst != nullptr &&
      evidence.mappedPointer == reinterpret_cast<uintptr_t>(mappedDst) &&
      evidence.mappedAllocationBase != 0u &&
      evidence.offset <= std::numeric_limits<uintptr_t>::max() -
          evidence.mappedAllocationBase &&
      evidence.mappedAllocationBase + evidence.offset ==
          reinterpret_cast<uintptr_t>(mappedDst) &&
      probe.authorityVertexCount == probe.vertexCount &&
      uint64_t(probe.authorityBaseVertex) + probe.authorityVertexCount <=
          kMaxNativeVertices;
  bool kernelExact = !probe.authorityConflicted && ledgerExact && mappedExact;

  // Only an overlapping rewrite needs to prove that the original kernel will
  // actually write mode 0/1 bytes in the Lock-derived format. N performs no
  // poison mutation and therefore avoids this remaining RPM call entirely.
  if (kernelExact &&
      probe.authorityLockResult == NativeOutsidePoisonScanResult::Overlap) {
    std::array<uint32_t, 2u> kernelState = {};
    const bool addressExact = gxDeviceD3d != 0u &&
        kGxSkinModeOffset <=
            std::numeric_limits<uintptr_t>::max() - gxDeviceD3d &&
        gxDeviceD3d + kGxSkinModeOffset <=
            std::numeric_limits<uintptr_t>::max() -
                (sizeof(kernelState) - 1u);
    kernelExact = addressExact && dxvk::war3::SafeCopy(
        kernelState.data(),
        reinterpret_cast<const void*>(gxDeviceD3d + kGxSkinModeOffset),
        sizeof(kernelState));
    if (kernelExact) {
      probe.o1KernelSkinMode = kernelState[0u];
      probe.o1KernelOutputFormat = kernelState[1u];
      probe.o1KernelStateCaptured = true;
      kernelExact = probe.o1KernelSkinMode <= 1u &&
          probe.o1KernelOutputFormat == probe.authorityOutputFormat &&
          probe.authorityOutputFormat < kFormatFvf.size() &&
          evidence.descFvf == kFormatFvf[probe.authorityOutputFormat];
    }
  }
  probe.o1KernelGxDeviceD3d = gxDeviceD3d;
  probe.o1KernelMappedDst = reinterpret_cast<uintptr_t>(mappedDst);
  probe.o1KernelFreezeGeneration = t_poisonMutationGeneration;
  probe.authorityKernelExact = kernelExact;
  if (kernelExact) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsidePoisonAuthorityKernelReady,
        t_nativeTelemetryDelta.productionOutsidePoisonAuthorityKernelReady);
  } else {
    probe.authorityConflicted = true;
    g_counters.productionOutsidePoisonAuthorityKernelRejects.fetch_add(
        1u, std::memory_order_relaxed);
  }
}

bool OutsidePoisonAuthorityKernelReady(
    uint64_t cookie, uintptr_t gxDeviceD3d, void* mappedDst) noexcept {
  const NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (cookie == 0u || state.overflowDepth != 0u || state.depth == 0u)
    return false;
  const NativeOutsidePoisonShadowProbe& probe =
      state.probes[state.depth - 1u];
  return probe.cookie == cookie && probe.authorityArmed &&
      probe.authorityKernelExact && !probe.authorityConflicted &&
      probe.gxDeviceD3d == gxDeviceD3d &&
      probe.o1KernelMappedDst == reinterpret_cast<uintptr_t>(mappedDst);
}

void FreezeOutsidePoisonShadowAtKernelNormalReturn(
    uintptr_t gxDeviceD3d, void* mappedDst) noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (state.overflowDepth != 0u || state.depth == 0u)
    return;
  NativeOutsidePoisonShadowProbe& probe = state.probes[state.depth - 1u];
  const bool o0Enabled = OutsidePoisonProbeCollectsO0(probe);
  const bool o1Enabled = OutsidePoisonProbeCollectsO1(probe);
  const bool authorityEnabled = probe.authorityArmed;
  if (!o0Enabled && !o1Enabled && !authorityEnabled)
    return;
  const bool firstNormalReturn = !probe.kernelNormalSeen;
  probe.kernelNormalSeen = true;
  if (authorityEnabled && firstNormalReturn) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsidePoisonAuthorityNormalReturns,
        t_nativeTelemetryDelta.productionOutsidePoisonAuthorityNormalReturns);
  } else if (authorityEnabled && !firstNormalReturn) {
    probe.authorityConflicted = true;
  }
  if (probe.frozen) {
    if (o0Enabled) {
      MarkOutsidePoisonShadowUnprovable(
          probe, NativeOutsidePoisonShadowUnprovableReason::Reentry);
    }
    if (o1Enabled) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe, NativeOutsidePoisonO1ShadowUnprovableReason::Reentry);
    }
    return;
  }

  if (!o0Enabled) {
    // O1 already froze its independent Lock/kernel-entry evidence. A normal
    // trampoline return only closes the shared lifetime before Unlock; it must
    // not run the O0 normal-return comparison or touch any O0 counter.
    probe.frozen = true;
    return;
  }

  if (probe.ownerThreadId == 0u ||
      probe.ownerThreadId != ::GetCurrentThreadId() ||
      probe.gxDeviceD3d == 0u || probe.gxDeviceD3d != gxDeviceD3d ||
      !probe.oldResultRecorded) {
    MarkOutsidePoisonShadowUnprovable(
        probe, NativeOutsidePoisonShadowUnprovableReason::OwnerOrLifo);
  } else if (probe.flushEpoch == 0u ||
             probe.flushEpoch != t_state.flushEpoch) {
    // A flush-epoch transition can advance or replace the same native ring
    // target between the legacy prediction and the observed Lock.
    MarkOutsidePoisonShadowUnprovable(
        probe, NativeOutsidePoisonShadowUnprovableReason::Reentry);
  } else if (probe.resetGeneration !=
                 g_resetRequestedGeneration.load(std::memory_order_acquire) ||
             probe.resetGeneration !=
                 g_resetCompletedGeneration.load(std::memory_order_acquire) ||
             probe.resetGeneration != t_state.appliedResetGeneration ||
             g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
             g_retirementQueueFaulted.load(std::memory_order_acquire)) {
    MarkOutsidePoisonShadowUnprovable(
        probe,
        NativeOutsidePoisonShadowUnprovableReason::ResetOrRetirement);
  } else if (probe.poisonMutationGeneration == 0u ||
             probe.poisonMutationGeneration !=
                 t_poisonMutationGeneration ||
             probe.poisonCount != t_state.poisonLedger.count ||
             probe.poisonOutstanding !=
                 g_nativePoisonOutstandingRanges.load(
                     std::memory_order_acquire)) {
    MarkOutsidePoisonShadowUnprovable(
        probe, NativeOutsidePoisonShadowUnprovableReason::PoisonMutation);
  }

  if (probe.lockNotifications == 0u) {
    MarkOutsidePoisonShadowUnprovable(
        probe, NativeOutsidePoisonShadowUnprovableReason::NoLock);
  } else if (probe.lockNotifications != 1u) {
    MarkOutsidePoisonShadowUnprovable(
        probe, NativeOutsidePoisonShadowUnprovableReason::MultipleLocks);
  }

  const NativeOutsidePoisonVertexLockEvidence& evidence =
      probe.lockEvidence;
  uint32_t outputFormat = 0u;
  uint32_t vertexStride = 0u;
  const bool formatExact = probe.lockNotifications == 1u &&
      ResolveOutsidePoisonShadowFormat(
          evidence.descFvf, outputFormat, vertexStride) &&
      uint64_t(kMaxNativeVertices) * vertexStride == evidence.descSize &&
      (!probe.oldObservation.targetParsed ||
       probe.oldObservation.outputFormat == outputFormat) &&
      (probe.outputByteCount == 0u ||
       (probe.vertexCount != 0u &&
        probe.outputByteCount == uint64_t(probe.vertexCount) * vertexStride));
  if (!formatExact) {
    MarkOutsidePoisonShadowUnprovable(
        probe, NativeOutsidePoisonShadowUnprovableReason::FormatOrFvf);
  }

  const bool descriptorExact = formatExact && evidence.result >= 0 &&
      evidence.descType == kD3dResourceTypeVertexBuffer &&
      evidence.descPool == kD3dPoolDefault &&
      (evidence.descUsage & kD3dUsageDynamic) != 0u &&
      evidence.lockDepth == 1u && evidence.storage.activeLock &&
      evidence.storage.activeLockFlags == evidence.effectiveFlags &&
      evidence.requestedFlags == evidence.effectiveFlags &&
      (evidence.effectiveFlags == kNativeVertexRingLockNoOverwrite ||
       evidence.effectiveFlags == kNativeVertexRingLockDiscard);
  if (!descriptorExact) {
    MarkOutsidePoisonShadowUnprovable(
        probe, NativeOutsidePoisonShadowUnprovableReason::LockDescriptor);
  }

  const bool resourceExact = descriptorExact &&
      evidence.nativeD3DDevice != 0u &&
      evidence.resource.commonResource != 0u &&
      evidence.resource.comVertexBuffer != 0u &&
      evidence.resource.resourceGeneration != 0u &&
      (!probe.oldObservation.targetParsed ||
       (probe.oldObservation.nativeD3DDevice == evidence.nativeD3DDevice &&
        probe.oldObservation.comVertexBuffer ==
            evidence.resource.comVertexBuffer));
  if (!resourceExact) {
    MarkOutsidePoisonShadowUnprovable(
        probe, NativeOutsidePoisonShadowUnprovableReason::ResourceIdentity);
  }

  const bool storageExact = resourceExact &&
      OutsidePoisonShadowStorageComplete(evidence);
  if (!storageExact) {
    MarkOutsidePoisonShadowUnprovable(
        probe, NativeOutsidePoisonShadowUnprovableReason::StorageIdentity);
  }

  uint32_t baseVertex = 0u;
  uint32_t lockVertexCount = 0u;
  bool rangeExact = storageExact && vertexStride != 0u &&
      evidence.offset % vertexStride == 0u &&
      evidence.size % vertexStride == 0u &&
      evidence.size != 0u &&
      uint64_t(evidence.offset) + evidence.size <= evidence.descSize &&
      evidence.mappedAllocationBase <=
          std::numeric_limits<uintptr_t>::max() - evidence.offset &&
      evidence.mappedPointer == evidence.mappedAllocationBase + evidence.offset &&
      evidence.mappedPointer == reinterpret_cast<uintptr_t>(mappedDst);
  if (rangeExact) {
    baseVertex = evidence.offset / vertexStride;
    lockVertexCount = evidence.size / vertexStride;
    rangeExact = lockVertexCount == probe.vertexCount &&
        uint64_t(baseVertex) + lockVertexCount <= kMaxNativeVertices &&
        (!probe.oldObservation.targetParsed ||
         (probe.oldObservation.predictedBaseVertex == baseVertex &&
          probe.oldObservation.vertexCount == lockVertexCount));
  }
  if (rangeExact && probe.oldObservation.targetParsed) {
    const bool wraps = probe.oldObservation.ringNext >
        kMaxNativeVertices - probe.vertexCount;
    const uint32_t expectedFlags =
        wraps ? kNativeVertexRingLockDiscard
              : kNativeVertexRingLockNoOverwrite;
    rangeExact = evidence.effectiveFlags == expectedFlags;
  }
  if (!rangeExact) {
    MarkOutsidePoisonShadowUnprovable(
        probe, NativeOutsidePoisonShadowUnprovableReason::Range);
  }

  if (probe.unprovableReason ==
      NativeOutsidePoisonShadowUnprovableReason::Count) {
    probe.sidecarResult = ScanOutsidePoisonShadowLock(
        evidence, outputFormat, vertexStride, baseVertex, lockVertexCount);
  }

  probe.frozen = true;
}

size_t FindOutsidePoisonShadowProbe(uint64_t cookie) noexcept {
  const NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  for (size_t i = state.depth; i != 0u; --i) {
    if (state.probes[i - 1u].cookie == cookie)
      return i - 1u;
  }
  return state.probes.size();
}

void EraseOutsidePoisonShadowProbe(size_t index) noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (index >= state.depth)
    return;
  const bool o0Enabled =
      OutsidePoisonProbeCollectsO0(state.probes[index]);
  const bool o1Enabled =
      OutsidePoisonProbeCollectsO1(state.probes[index]);
  for (size_t i = index + 1u; i < state.depth; ++i)
    state.probes[i - 1u] = state.probes[i];
  --state.depth;
  state.probes[state.depth] = {};
  if (o0Enabled)
    SubtractOutsidePoisonShadowActive(1u);
  if (o1Enabled)
    SubtractOutsidePoisonO1ShadowActive(1u);
}

bool OutsidePoisonO1UnlockIdentityExact(
    const NativeOutsidePoisonShadowProbe& probe) noexcept {
  const NativeOutsidePoisonVertexLockEvidence& lock = probe.lockEvidence;
  const NativeOutsidePoisonVertexUnlockEvidence& unlock =
      probe.o1UnlockEvidence;
  const bool mappedPointerExact = unlock.mappedAllocationBase != 0u &&
      lock.offset <= std::numeric_limits<uintptr_t>::max() -
          unlock.mappedAllocationBase &&
      unlock.mappedAllocationBase + lock.offset == lock.mappedPointer;
  // The D3D9 resource and app-visible mapped allocation are the hard
  // Lock-to-Unlock identities. DXVK may replace real/mapping backings while
  // preserving that allocation and its bytes, so their pointer/generation
  // drift is recorded below but cannot manufacture a physical rejection.
  return unlock.resource.commonResource != 0u &&
      unlock.resource.commonResource == lock.resource.commonResource &&
      unlock.resource.comVertexBuffer != 0u &&
      unlock.resource.comVertexBuffer == lock.resource.comVertexBuffer &&
      unlock.nativeD3DDevice != 0u &&
      unlock.nativeD3DDevice == lock.nativeD3DDevice &&
      unlock.mappedAllocation != 0u &&
      unlock.mappedAllocation == lock.mappedAllocation &&
      unlock.mappedAllocationBase == lock.mappedAllocationBase &&
      unlock.completionMappedAllocation == unlock.mappedAllocation &&
      unlock.completionMappedAllocationBase == unlock.mappedAllocationBase &&
      unlock.activeLockObserved && unlock.entryStorage.activeLock &&
      unlock.entryStorage.activeLockFlags == lock.effectiveFlags &&
      unlock.activeLockOwnerIdentity == lock.resource.comVertexBuffer &&
      unlock.activeLockOffset == lock.offset &&
      unlock.activeLockSize == lock.size &&
      unlock.activeLockFlags == lock.effectiveFlags &&
      unlock.entryLockDepth == 1u && unlock.completionLockDepth == 0u &&
      !unlock.completionStorage.activeLock &&
      unlock.completionStorage.activeLockFlags == 0u &&
      unlock.entryStorage.mapMode == lock.storage.mapMode &&
      unlock.completionStorage.mapMode == unlock.entryStorage.mapMode &&
      mappedPointerExact;
}

struct NativeOutsidePoisonO1UnlockDriftStatus {
  bool resourceExact = false;
  bool realExact = false;
  bool mappingExact = false;
  bool mapAllocationExact = false;
};

NativeOutsidePoisonO1UnlockDriftStatus
GetOutsidePoisonO1UnlockDriftStatus(
    const NativeOutsidePoisonShadowProbe& probe) noexcept {
  const NativeOutsidePoisonVertexLockEvidence& lock = probe.lockEvidence;
  const NativeOutsidePoisonVertexUnlockEvidence& unlock =
      probe.o1UnlockEvidence;
  NativeOutsidePoisonO1UnlockDriftStatus status = {};
  status.resourceExact = unlock.resource.resourceGeneration != 0u &&
      unlock.resource.resourceGeneration == lock.resource.resourceGeneration &&
      unlock.completionResourceGeneration ==
          unlock.resource.resourceGeneration;
  status.realExact = unlock.realBuffer != 0u &&
      unlock.realBuffer == lock.realBuffer &&
      unlock.completionRealBuffer == unlock.realBuffer &&
      unlock.entryStorage.realStorageGeneration != 0u &&
      unlock.entryStorage.realStorageGeneration ==
          lock.storage.realStorageGeneration &&
      unlock.completionStorage.realStorageGeneration ==
          unlock.entryStorage.realStorageGeneration;
  status.mappingExact = unlock.mappingBuffer != 0u &&
      unlock.mappingBuffer == lock.mappingBuffer &&
      unlock.completionMappingBuffer == unlock.mappingBuffer &&
      unlock.entryStorage.mappingStorageGeneration != 0u &&
      unlock.entryStorage.mappingStorageGeneration ==
          lock.storage.mappingStorageGeneration &&
      unlock.completionStorage.mappingStorageGeneration ==
          unlock.entryStorage.mappingStorageGeneration;
  status.mapAllocationExact = unlock.mappedAllocation != 0u &&
      unlock.mappedAllocation == lock.mappedAllocation &&
      unlock.mappedAllocationBase == lock.mappedAllocationBase &&
      unlock.completionMappedAllocation == unlock.mappedAllocation &&
      unlock.completionMappedAllocationBase == unlock.mappedAllocationBase &&
      unlock.entryStorage.mapAllocationGeneration != 0u &&
      unlock.entryStorage.mapAllocationGeneration ==
          lock.storage.mapAllocationGeneration &&
      unlock.completionStorage.mapAllocationGeneration ==
          unlock.entryStorage.mapAllocationGeneration;
  return status;
}

void RecordOutsidePoisonO1UnlockDrifts(
    const NativeOutsidePoisonO1UnlockDriftStatus& status) noexcept {
  const std::array<bool, kNativeOutsidePoisonO1UnlockDriftComponentCount>
      exact = {{
          status.resourceExact,
          status.realExact,
          status.mappingExact,
          status.mapAllocationExact,
      }};
  for (size_t i = 0u; i < exact.size(); ++i) {
    if (!exact[i]) {
      g_counters.productionOutsidePoisonO1ShadowUnlockDrifts[i].fetch_add(
          1u, std::memory_order_relaxed);
    }
  }
}

NativeOutsidePoisonO1UnlockDriftComponent
OutsidePoisonO1UnlockHardFirstCause(
    const NativeOutsidePoisonO1UnlockDriftStatus& status) noexcept {
  if (!status.resourceExact)
    return NativeOutsidePoisonO1UnlockDriftComponent::Resource;
  if (!status.mapAllocationExact)
    return NativeOutsidePoisonO1UnlockDriftComponent::MapAllocation;
  // real/mapping diagnosticStorageGeneration is observation-only. Pointer
  // identity remains covered by OutsidePoisonO1UnlockIdentityExact.
  return NativeOutsidePoisonO1UnlockDriftComponent::Count;
}

void ClearPoisonCoveredByCpuUpload(
    const NativeUploadObservation& upload,
    const NativeCpuRewriteOutputProof& proof);

void RecordOutsidePoisonAuthorityMissingTerminals(
    NativeOutsidePoisonShadowProbe& probe) noexcept {
  if (!probe.authorityArmed)
    return;
  if (!probe.authorityLockTerminalRecorded) {
    probe.authorityLockTerminalRecorded = true;
    g_counters.productionOutsidePoisonAuthorityLockRejects.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (!probe.authorityKernelTerminalRecorded) {
    probe.authorityKernelTerminalRecorded = true;
    g_counters.productionOutsidePoisonAuthorityKernelRejects.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (!probe.authorityUnlockTerminalRecorded) {
    probe.authorityUnlockTerminalRecorded = true;
    g_counters.productionOutsidePoisonAuthorityUnlockRejects.fetch_add(
        1u, std::memory_order_relaxed);
  }
}

void RecordOutsidePoisonAuthorityMissingEvidence(
    NativeOutsidePoisonShadowProbe& probe) noexcept {
  if (!probe.authorityEvidence || probe.authorityEvidenceTerminalRecorded)
    return;
  probe.authorityEvidenceTerminalRecorded = true;
  g_counters.productionOutsidePoisonAuthorityEvidenceUnprovable.fetch_add(
      1u, std::memory_order_relaxed);
}

bool OutsidePoisonAuthorityCoverageStillLive(
    const NativeOutsidePoisonShadowProbe& probe) noexcept {
  const NativeOutsidePoisonVertexLockEvidence& evidence = probe.lockEvidence;
  NativePoisonRange coverage = {};
  coverage.vertexOutputProof.resource = evidence.resource;
  coverage.vertexOutputProof.nativeD3DDevice = evidence.nativeD3DDevice;
  coverage.vertexOutputProof.outputFormat = probe.authorityOutputFormat;
  coverage.vertexOutputProof.vertexStride = probe.authorityVertexStride;
  coverage.vertexOutputProof.fvf = evidence.descFvf;
  coverage.baseVertex = probe.authorityBaseVertex;
  coverage.vertexCount = probe.authorityVertexCount;
  coverage.stride = probe.authorityVertexStride;
  coverage.fvf = evidence.descFvf;
  coverage.outputFormat = probe.authorityOutputFormat;
  const uint64_t coverageEnd = PoisonRangeEnd(coverage);
  for (size_t i = 0u; i < t_state.poisonLedger.count; ++i) {
    const NativePoisonRange& poison = t_state.poisonLedger.ranges[i];
    if (SameCpuOverwriteTarget(poison, coverage) &&
        coverageEnd > poison.baseVertex &&
        uint64_t(coverage.baseVertex) < PoisonRangeEnd(poison)) {
      return true;
    }
  }
  return false;
}

bool ClearPoisonCoveredByOutsideAuthority(
    const NativeOutsidePoisonShadowProbe& probe) noexcept {
  const NativeOutsidePoisonVertexLockEvidence& evidence = probe.lockEvidence;
  NativeUploadObservation upload = {};
  upload.cpuSkinOriginalReturnedNormally = true;
  upload.nativeVertexBuffer = evidence.resource.comVertexBuffer;
  upload.nativeD3DDevice = evidence.nativeD3DDevice;
  upload.outputFormat = probe.authorityOutputFormat;
  upload.outputStride = probe.authorityVertexStride;
  upload.fvf = evidence.descFvf;
  upload.ringBaseVertexAfter = probe.authorityBaseVertex;
  upload.ringNextVertexAfter = probe.authorityBaseVertex +
      probe.authorityVertexCount;
  upload.vertexCount = probe.authorityVertexCount;
  NativeCpuRewriteOutputProof proof = {};
  proof.vertexOutputProof.resource = evidence.resource;
  proof.vertexOutputProof.nativeD3DDevice = evidence.nativeD3DDevice;
  proof.vertexOutputProof.outputFormat = probe.authorityOutputFormat;
  proof.vertexOutputProof.vertexStride = probe.authorityVertexStride;
  proof.vertexOutputProof.fvf = evidence.descFvf;
  proof.baseVertex = probe.authorityBaseVertex;
  proof.vertexCount = probe.authorityVertexCount;
  proof.byteOffset = probe.authorityBaseVertex * probe.authorityVertexStride;
  proof.byteLength = probe.authorityVertexCount * probe.authorityVertexStride;
  if (!CpuRewriteProofMatchesUpload(upload, proof))
    return false;
  ClearPoisonCoveredByCpuUpload(upload, proof);
  return !OutsidePoisonAuthorityCoverageStillLive(probe);
}

void FinalizeOutsidePoisonShadow(
    uint64_t cookie, bool cancelled, int32_t originalResult) noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (cookie == 0u)
    return;
  if (cookie == kOutsidePoisonShadowOverflowCookie) {
    if (state.overflowDepth != 0u)
      --state.overflowDepth;
    return;
  }
  const size_t index = FindOutsidePoisonShadowProbe(cookie);
  if (index >= state.depth)
    return;  // A reset already classified and cleared this exact lifetime.
  NativeOutsidePoisonShadowProbe& probe = state.probes[index];
  const bool o0Enabled = OutsidePoisonProbeCollectsO0(probe);
  const bool o1Enabled = OutsidePoisonProbeCollectsO1(probe);
  if (index + 1u != state.depth) {
    if (o0Enabled) {
      MarkOutsidePoisonShadowUnprovable(
          probe, NativeOutsidePoisonShadowUnprovableReason::OwnerOrLifo);
    }
    if (o1Enabled) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe, NativeOutsidePoisonO1ShadowUnprovableReason::OwnerOrLifo);
    }
    if (probe.authorityArmed)
      probe.authorityConflicted = true;
  }

  if (cancelled) {
    if (o0Enabled) {
      g_counters.productionOutsidePoisonShadowCancelled.fetch_add(
          1u, std::memory_order_relaxed);
    }
    if (o1Enabled) {
      g_counters.productionOutsidePoisonO1ShadowCancelled.fetch_add(
          1u, std::memory_order_relaxed);
    }
    if (probe.authorityCandidate) {
      g_counters.productionOutsidePoisonAuthorityCancelled.fetch_add(
          1u, std::memory_order_relaxed);
      RecordOutsidePoisonAuthorityMissingEvidence(probe);
    }
    if (probe.authorityArmed) {
      RecordOutsidePoisonAuthorityMissingTerminals(probe);
      g_counters.productionOutsidePoisonAuthorityRetained.fetch_add(
          1u, std::memory_order_relaxed);
      ReleaseOutsidePoisonAuthorityActive(probe);
    }
    EraseOutsidePoisonShadowProbe(index);
    return;
  }

  if (o0Enabled) {
    if (!probe.kernelNormalSeen) {
      MarkOutsidePoisonShadowUnprovable(
          probe, NativeOutsidePoisonShadowUnprovableReason::KernelNotNormal);
    } else if (!probe.frozen) {
      MarkOutsidePoisonShadowUnprovable(
          probe, NativeOutsidePoisonShadowUnprovableReason::OwnerOrLifo);
    }

    if (probe.unprovableReason !=
        NativeOutsidePoisonShadowUnprovableReason::Count) {
      const size_t reason = static_cast<size_t>(probe.unprovableReason);
      g_counters.productionOutsidePoisonShadowUnprovable.fetch_add(
          1u, std::memory_order_relaxed);
      if (reason <
          g_counters.productionOutsidePoisonShadowUnprovableReasons.size()) {
        g_counters.productionOutsidePoisonShadowUnprovableReasons[reason]
            .fetch_add(1u, std::memory_order_relaxed);
      }
    } else {
      const size_t oldIndex = static_cast<size_t>(probe.oldResult);
      const size_t sidecarIndex = static_cast<size_t>(probe.sidecarResult);
      const size_t matrixIndex = oldIndex * 3u + sidecarIndex;
      if (matrixIndex <
          g_counters.productionOutsidePoisonShadowMatrix.size()) {
        g_counters.productionOutsidePoisonShadowMatrix[matrixIndex].fetch_add(
            1u, std::memory_order_relaxed);
        g_counters.productionOutsidePoisonShadowComparable.fetch_add(
            1u, std::memory_order_relaxed);
      }
    }
  }

  NativeOutsidePoisonO1UnlockDriftStatus unlockDriftStatus = {};
  if (o1Enabled && probe.o1UnlockNotifications == 1u) {
    unlockDriftStatus = GetOutsidePoisonO1UnlockDriftStatus(probe);
    RecordOutsidePoisonO1UnlockDrifts(unlockDriftStatus);
  }

  if (o1Enabled && probe.o1UnprovableReason ==
          NativeOutsidePoisonO1ShadowUnprovableReason::Count) {
    if (probe.o1KernelNotifications == 0u || !probe.o1Frozen) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe,
          NativeOutsidePoisonO1ShadowUnprovableReason::KernelNotObserved);
    } else if (!probe.kernelNormalSeen) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe,
          NativeOutsidePoisonO1ShadowUnprovableReason::KernelNotNormal);
    } else if (probe.o1UnlockNotifications == 0u) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe,
          NativeOutsidePoisonO1ShadowUnprovableReason::UnlockNotObserved);
    } else if (probe.o1UnlockNotifications != 1u) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe,
          NativeOutsidePoisonO1ShadowUnprovableReason::MultipleUnlocks);
    } else if (!probe.o1UnlockAfterFreeze) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe,
          NativeOutsidePoisonO1ShadowUnprovableReason::UnlockBeforeFreeze);
    } else if (probe.o1UnlockEvidence.result < 0) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe, NativeOutsidePoisonO1ShadowUnprovableReason::UnlockFailed);
    } else if (!OutsidePoisonO1UnlockIdentityExact(probe)) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe, NativeOutsidePoisonO1ShadowUnprovableReason::UnlockIdentity);
    } else {
      const NativeOutsidePoisonO1UnlockDriftComponent hardFirstCause =
          OutsidePoisonO1UnlockHardFirstCause(unlockDriftStatus);
      if (hardFirstCause !=
          NativeOutsidePoisonO1UnlockDriftComponent::Count) {
        MarkOutsidePoisonO1ShadowUnprovable(
            probe,
            NativeOutsidePoisonO1ShadowUnprovableReason::UnlockGeneration);
        const size_t hardIndex = static_cast<size_t>(hardFirstCause);
        g_counters.productionOutsidePoisonO1ShadowUnlockHardFirstCauses[
            hardIndex].fetch_add(1u, std::memory_order_relaxed);
      }
    }
    if (probe.o1UnprovableReason ==
            NativeOutsidePoisonO1ShadowUnprovableReason::Count &&
        originalResult < 0) {
      // 0x6F0EEC00 returns SetStreamSource's HRESULT. It proves only that the
      // outer path completed; the independent Unlock proof above is mandatory.
      MarkOutsidePoisonO1ShadowUnprovable(
          probe, NativeOutsidePoisonO1ShadowUnprovableReason::OuterResult);
    }
  }

  if (o1Enabled && probe.o1UnprovableReason !=
          NativeOutsidePoisonO1ShadowUnprovableReason::Count) {
    const size_t reason = static_cast<size_t>(probe.o1UnprovableReason);
    g_counters.productionOutsidePoisonO1ShadowUnprovable.fetch_add(
        1u, std::memory_order_relaxed);
    if (reason <
        g_counters.productionOutsidePoisonO1ShadowUnprovableReasons.size()) {
      g_counters.productionOutsidePoisonO1ShadowUnprovableReasons[reason]
          .fetch_add(1u, std::memory_order_relaxed);
    }
  }
  if (o1Enabled) {
    size_t physicalVerdictIndex =
        static_cast<size_t>(NativeOutsidePoisonScanResult::ReadFailure);
    if (probe.o1UnprovableReason ==
            NativeOutsidePoisonO1ShadowUnprovableReason::Count &&
        probe.o1SidecarResult != NativeOutsidePoisonScanResult::ReadFailure) {
      physicalVerdictIndex = static_cast<size_t>(probe.o1SidecarResult);
    }
    g_counters.productionOutsidePoisonO1ShadowPhysicalVerdicts[
        physicalVerdictIndex].fetch_add(1u, std::memory_order_relaxed);
  }
  if (probe.authorityCandidate) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsidePoisonAuthoritySettled,
        t_nativeTelemetryDelta.productionOutsidePoisonAuthoritySettled);
    RecordOutsidePoisonAuthorityMissingEvidence(probe);
  }
  if (probe.authorityArmed) {
    RecordOutsidePoisonAuthorityMissingTerminals(probe);
    const bool ledgerExact = probe.authorityLockPoisonGeneration != 0u &&
        probe.authorityLockPoisonGeneration == t_poisonMutationGeneration &&
        probe.authorityLockPoisonCount == t_state.poisonLedger.count &&
        probe.authorityLockPoisonOutstanding ==
            g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
    const bool physicalExact = probe.authorityUsesActualLock &&
        probe.authorityLockExact && probe.authorityKernelExact &&
        probe.kernelNormalSeen && probe.authorityUnlockExact &&
        originalResult >= 0 && ledgerExact && !probe.authorityConflicted;
    bool committed = false;
    if (physicalExact &&
        probe.authorityLockResult ==
            NativeOutsidePoisonScanResult::NoOverlap) {
      AddNativeTelemetryCounter(
          g_counters.productionOutsidePoisonAuthorityCommittedNoOverlap,
          t_nativeTelemetryDelta
              .productionOutsidePoisonAuthorityCommittedNoOverlap);
      committed = true;
    } else if (physicalExact &&
               probe.authorityLockResult ==
                   NativeOutsidePoisonScanResult::Overlap &&
               probe.o1KernelStateCaptured &&
               probe.o1KernelSkinMode <= 1u &&
               ClearPoisonCoveredByOutsideAuthority(probe)) {
      AddNativeTelemetryCounter(
          g_counters.productionOutsidePoisonAuthorityCommittedRewrite,
          t_nativeTelemetryDelta
              .productionOutsidePoisonAuthorityCommittedRewrite);
      AddNativeTelemetryCounter(
          g_counters.productionOutsidePoisonAuthorityPoisonClears,
          t_nativeTelemetryDelta.productionOutsidePoisonAuthorityPoisonClears);
      committed = true;
    }
    if (committed) {
      if (probe.authorityEvidence) {
        g_counters.productionOutsidePoisonAuthorityEvidenceAuthority.fetch_add(
            1u, std::memory_order_relaxed);
      }
      if (probe.authorityLegacyBacked) {
        g_counters.productionOutsidePoisonAuthorityLegacyBackedAuthority
            .fetch_add(1u, std::memory_order_relaxed);
      }
      AddNativeTelemetryCounter(
          g_counters.productionOutsidePoisonAuthority,
          t_nativeTelemetryDelta.productionOutsidePoisonAuthority);
    } else {
      g_counters.productionOutsidePoisonAuthorityRetained.fetch_add(
          1u, std::memory_order_relaxed);
    }
    ReleaseOutsidePoisonAuthorityActive(probe);
  }
  if (o0Enabled) {
    g_counters.productionOutsidePoisonShadowSettled.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (o1Enabled) {
    g_counters.productionOutsidePoisonO1ShadowSettled.fetch_add(
        1u, std::memory_order_relaxed);
  }
  EraseOutsidePoisonShadowProbe(index);
}

void AbortOutsidePoisonShadowForReset() noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (state.depth != 0u) {
    uint64_t o0Aborted = 0u;
    uint64_t o1Aborted = 0u;
    uint64_t authorityAborted = 0u;
    for (size_t i = 0u; i < state.depth; ++i) {
      NativeOutsidePoisonShadowProbe& probe = state.probes[i];
      o0Aborted += OutsidePoisonProbeCollectsO0(probe) ? 1u : 0u;
      o1Aborted += OutsidePoisonProbeCollectsO1(probe) ? 1u : 0u;
      authorityAborted += probe.authorityCandidate ? 1u : 0u;
      if (probe.authorityCandidate)
        RecordOutsidePoisonAuthorityMissingEvidence(probe);
      if (probe.authorityArmed) {
        RecordOutsidePoisonAuthorityMissingTerminals(probe);
        g_counters.productionOutsidePoisonAuthorityRetained.fetch_add(
            1u, std::memory_order_relaxed);
        ReleaseOutsidePoisonAuthorityActive(probe);
      }
    }
    if (o0Aborted != 0u) {
      g_counters.productionOutsidePoisonShadowResetAborted.fetch_add(
          o0Aborted, std::memory_order_relaxed);
      SubtractOutsidePoisonShadowActive(o0Aborted);
    }
    if (o1Aborted != 0u) {
      g_counters.productionOutsidePoisonO1ShadowResetAborted.fetch_add(
          o1Aborted, std::memory_order_relaxed);
      SubtractOutsidePoisonO1ShadowActive(o1Aborted);
    }
    if (authorityAborted != 0u) {
      g_counters.productionOutsidePoisonAuthorityResetAborted.fetch_add(
          authorityAborted, std::memory_order_relaxed);
    }
  }
  state.probes = {};
  state.depth = 0u;
  // Overflow attempts were already terminally classified at creation.
  state.overflowDepth = 0u;
}

bool PoisonRangesTouch(const NativePoisonRange& lhs,
                       const NativePoisonRange& rhs) {
  return uint64_t(lhs.baseVertex) <= PoisonRangeEnd(rhs) &&
      uint64_t(rhs.baseVertex) <= PoisonRangeEnd(lhs);
}

void RemovePoisonRange(size_t index) {
  NativePoisonLedger& ledger = t_state.poisonLedger;
  if (index >= ledger.count)
    return;
  --ledger.count;
  if (index != ledger.count)
    ledger.ranges[index] = ledger.ranges[ledger.count];
  ledger.ranges[ledger.count] = {};
  SubtractPoisonOutstanding(1u);
  if (ledger.count == 0u)
    ledger = {};
  AdvancePoisonMutationGeneration();
}

bool RetireNativeBypassPoisonAfterExactConsumerInternal(
    const NativeUploadObservation& observation,
    bool exactSingleDip, bool consumerSettlementExact) noexcept {
  // 只处理 VS-B1 的轻量生产路径。完整诊断仍保留旧 poison 生命周期，
  // 这样证据模式不会因为提前退休而失去原生环覆盖样本。
  if (g_runtimeConfig.mode != GpuSkinMode::Bypass ||
      g_runtimeConfig.fullDiagnostics ||
      g_runtimeConfig.executionRoute !=
          GpuSkinExecutionRoute::VertexShaderBypass ||
      !g_runtimeConfig.executionRouteExplicit ||
      g_runtimeConfig.executionRouteInvalid || !exactSingleDip ||
      !consumerSettlementExact || observation.epoch.uploadEpoch == 0u ||
      observation.fuseKey == 0u || !observation.cpuSkinKernelBypassed ||
      observation.postSkipMismatch || !observation.bypassAuthorized ||
      observation.outputFormat != kVertexShaderBypassPoisonFormat ||
      observation.outputStride !=
          kFormatStride[kVertexShaderBypassPoisonFormat] ||
      observation.vertexCount == 0u ||
      observation.ringNextVertexAfter <= observation.ringBaseVertexAfter ||
      observation.ringNextVertexAfter > kMaxNativeVertices ||
      observation.expectedIndexCount == 0u ||
      (observation.expectedIndexCount % 3u) != 0u ||
      observation.predictedIndexRingNext !=
          observation.predictedIndexRingBase +
              observation.expectedIndexCount ||
      observation.indexTicket.suppressed ||
      !observation.indexTicket.exact ||
      observation.indexTicket.failureMask != NativeIndexTicketFailureNone ||
      (observation.indexTicket.stageMask &
       (NativeIndexTicketStageExpectedProof |
        NativeIndexTicketStageLockAttempt |
        NativeIndexTicketStageLockExact |
        NativeIndexTicketStageContentsExact |
        NativeIndexTicketStageUnlockExact |
        NativeIndexTicketStageSetIndicesExact |
        NativeIndexTicketStageActualProof |
        NativeIndexTicketStageDipConsumed)) !=
          (NativeIndexTicketStageExpectedProof |
           NativeIndexTicketStageLockAttempt |
           NativeIndexTicketStageLockExact |
           NativeIndexTicketStageContentsExact |
           NativeIndexTicketStageUnlockExact |
           NativeIndexTicketStageSetIndicesExact |
           NativeIndexTicketStageActualProof |
           NativeIndexTicketStageDipConsumed)) {
    return false;
  }

  DispatchFrame* dispatch = CurrentDispatch();
  if (dispatch == nullptr || !dispatch->activeUpload.valid ||
      dispatch->activeUpload.dipCount != 1u ||
      dispatch->activeUpload.observation.epoch.uploadEpoch !=
          observation.epoch.uploadEpoch ||
      dispatch->activeUpload.observation.fuseKey != observation.fuseKey ||
      !dispatch->activeUpload.observation.cpuSkinKernelBypassed ||
      dispatch->observation.epoch.dispatchEpoch !=
          observation.epoch.dispatchEpoch ||
      dispatch->observation.epoch.flushEpoch != observation.epoch.flushEpoch ||
      t_state.dispatchDepth != 1u ||
      t_state.flushTransactionDepth == 0u ||
      t_callbackPinDepth == 0u ||
      t_state.poisonLedger.count == 0u ||
      g_nativePoisonOutstandingRanges.load(std::memory_order_acquire) == 0u ||
      g_resetRequestedGeneration.load(std::memory_order_acquire) !=
          g_resetCompletedGeneration.load(std::memory_order_acquire) ||
      g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
      g_retirementQueueFaulted.load(std::memory_order_acquire)) {
    return false;
  }

  NativePoisonRange coverage = {};
  coverage.vertexOutputProof = observation.vertexOutputProof;
  coverage.baseVertex = observation.ringBaseVertexAfter;
  coverage.vertexCount = observation.vertexCount;
  coverage.stride = observation.outputStride;
  coverage.fvf = observation.fvf;
  coverage.outputFormat = observation.outputFormat;
  if (coverage.vertexOutputProof.resource.commonResource == 0u ||
      coverage.vertexOutputProof.resource.comVertexBuffer == 0u ||
      coverage.vertexOutputProof.resource.resourceGeneration == 0u ||
      coverage.vertexOutputProof.nativeD3DDevice == 0u ||
      coverage.vertexOutputProof.nativeD3DDevice !=
          observation.nativeD3DDevice ||
      coverage.vertexOutputProof.outputFormat != coverage.outputFormat ||
      coverage.vertexOutputProof.vertexStride != coverage.stride ||
      coverage.vertexOutputProof.fvf != coverage.fvf ||
      uint64_t(coverage.baseVertex) + coverage.vertexCount !=
          observation.ringNextVertexAfter) {
    return false;
  }

  NativePoisonLedger& ledger = t_state.poisonLedger;
  for (size_t i = 0u; i < ledger.count; ++i) {
    NativePoisonRange& range = ledger.ranges[i];
    if (range.epoch.uploadEpoch != observation.epoch.uploadEpoch ||
        range.fuseKey != observation.fuseKey ||
        range.baseVertex != coverage.baseVertex ||
        range.vertexCount != coverage.vertexCount ||
        range.expectedStartIndex != observation.predictedIndexRingBase ||
        range.expectedIndexCount != observation.expectedIndexCount ||
        range.realStorageMixed || range.mappingStorageMixed ||
        range.mapAllocationMixed || range.mapModeMixed ||
        range.indexSignatureMixed || !SamePoisonLayout(range, coverage)) {
      continue;
    }
    // 只移除未与其他 poison 合并的完整区间；合并区间保守保留，交给
    // 后续真实 Lock 覆盖或 discard 退休，避免错误清掉相邻 stale 数据。
    RemovePoisonRange(i);
    g_counters.nativePoisonClears.fetch_add(1u, std::memory_order_relaxed);
    return true;
  }
  return false;
}

enum class PoisonCreateResult : uint8_t {
  Created = 0,
  InvalidRange,
  Overflow,
};

PoisonCreateResult CreateNativePoisonRange(
    const NativeUploadObservation& upload, uint64_t fuseKey,
    const NativeVertexOutputProof& outputProof,
    uint32_t expectedStartIndex, uint32_t expectedIndexCount) {
  const uint64_t end = uint64_t(upload.ringBaseVertexAfter) +
      upload.vertexCount;
  if (outputProof.resource.commonResource == 0u ||
      outputProof.resource.comVertexBuffer == 0u ||
      outputProof.resource.resourceGeneration == 0u ||
      outputProof.nativeD3DDevice == 0u ||
      outputProof.resource.comVertexBuffer != upload.nativeVertexBuffer ||
      outputProof.nativeD3DDevice != upload.nativeD3DDevice ||
      outputProof.outputFormat != upload.outputFormat ||
      outputProof.vertexStride != upload.outputStride ||
      outputProof.fvf != upload.fvf || upload.nativeD3DDevice == 0u ||
      upload.nativeVertexBuffer == 0u ||
      upload.outputFormat >= kFormatFvf.size() ||
      upload.outputStride != kFormatStride[upload.outputFormat] ||
      upload.fvf != kFormatFvf[upload.outputFormat] || fuseKey == 0u ||
      upload.vertexCount == 0u || end > kMaxNativeVertices ||
      end != upload.ringNextVertexAfter) {
    return PoisonCreateResult::InvalidRange;
  }

  NativePoisonLedger& ledger = t_state.poisonLedger;
  NativePoisonRange merged = {};
  merged.epoch = upload.epoch;
  merged.vertexOutputProof = outputProof;
  merged.storageDiagnostics =
      FreezePublishedStorageDiagnostics(outputProof);
  merged.fuseKey = fuseKey;
  merged.baseVertex = upload.ringBaseVertexAfter;
  merged.vertexCount = upload.vertexCount;
  merged.stride = upload.outputStride;
  merged.fvf = upload.fvf;
  merged.outputFormat = upload.outputFormat;
  merged.expectedStartIndex = expectedStartIndex;
  merged.expectedIndexCount = expectedIndexCount;

  // Adjacent stale slices are one suppression interval. Keep the oldest
  // provenance when coalescing; a fuse key is diagnostic and can never make
  // any byte in the coalesced native range valid again.
  bool mergedOne = false;
  do {
    mergedOne = false;
    for (size_t i = 0u; i < ledger.count; ++i) {
      const NativePoisonRange& current = ledger.ranges[i];
      if (!SamePoisonLayout(current, merged) ||
          !PoisonRangesTouch(current, merged)) {
        continue;
      }

      const uint64_t mergedEnd = std::max(
          PoisonRangeEnd(current), PoisonRangeEnd(merged));
      if (current.storageDiagnostics.mapMode ==
              kNativeVertexMapModeDirect &&
          merged.storageDiagnostics.mapMode ==
              kNativeVertexMapModeDirect &&
          current.storageDiagnostics.mapAllocationGeneration != 0u &&
          merged.storageDiagnostics.mapAllocationGeneration != 0u &&
          current.storageDiagnostics.mapAllocationGeneration !=
              merged.storageDiagnostics.mapAllocationGeneration) {
        g_counters.nativeCrossMapAllocationPoisonMerges.fetch_add(
            1u, std::memory_order_relaxed);
      }
      const bool realStorageMixed = current.realStorageMixed ||
          merged.realStorageMixed ||
          current.storageDiagnostics.realStorageGeneration !=
              merged.storageDiagnostics.realStorageGeneration;
      const bool mappingStorageMixed = current.mappingStorageMixed ||
          merged.mappingStorageMixed ||
          current.storageDiagnostics.mappingStorageGeneration !=
              merged.storageDiagnostics.mappingStorageGeneration;
      const bool mapAllocationMixed = current.mapAllocationMixed ||
          merged.mapAllocationMixed ||
          current.storageDiagnostics.mapAllocationGeneration !=
              merged.storageDiagnostics.mapAllocationGeneration;
      const bool mapModeMixed = current.mapModeMixed ||
          merged.mapModeMixed ||
          current.storageDiagnostics.mapMode !=
              merged.storageDiagnostics.mapMode;
      const bool indexSignatureMixed = current.indexSignatureMixed ||
          merged.indexSignatureMixed ||
          current.expectedStartIndex != merged.expectedStartIndex ||
          current.expectedIndexCount != merged.expectedIndexCount;
      merged.baseVertex = std::min(
          current.baseVertex, merged.baseVertex);
      merged.vertexCount = static_cast<uint32_t>(
          mergedEnd - merged.baseVertex);
      if (current.epoch.uploadEpoch != 0u &&
          (merged.epoch.uploadEpoch == 0u ||
           current.epoch.uploadEpoch < merged.epoch.uploadEpoch)) {
        merged.epoch = current.epoch;
        merged.fuseKey = current.fuseKey;
        merged.storageDiagnostics = current.storageDiagnostics;
        merged.expectedStartIndex = current.expectedStartIndex;
        merged.expectedIndexCount = current.expectedIndexCount;
      }
      merged.realStorageMixed = realStorageMixed;
      merged.mappingStorageMixed = mappingStorageMixed;
      merged.mapAllocationMixed = mapAllocationMixed;
      merged.mapModeMixed = mapModeMixed;
      merged.indexSignatureMixed = indexSignatureMixed;
      RemovePoisonRange(i);
      mergedOne = true;
      break;
    }
  } while (mergedOne);

  if (ledger.count >= ledger.ranges.size()) {
    g_counters.nativePoisonOverflows.fetch_add(
        1u, std::memory_order_relaxed);
    return PoisonCreateResult::Overflow;
  }
  ledger.ranges[ledger.count] = merged;
  ++ledger.count;
  AdvancePoisonMutationGeneration();
  g_nativePoisonOutstandingRanges.fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.nativePoisonCreates.fetch_add(
      1u, std::memory_order_relaxed);
  return PoisonCreateResult::Created;
}

void ClearPoisonCoveredByCpuUpload(
    const NativeUploadObservation& upload,
    const NativeCpuRewriteOutputProof& proof) {
  if (!upload.cpuSkinOriginalReturnedNormally ||
      !CpuRewriteProofMatchesUpload(upload, proof)) {
    return;
  }
  NativePoisonRange coverage = {};
  coverage.vertexOutputProof = proof.vertexOutputProof;
  coverage.baseVertex = proof.baseVertex;
  coverage.vertexCount = proof.vertexCount;
  coverage.stride = proof.vertexOutputProof.vertexStride;
  coverage.fvf = proof.vertexOutputProof.fvf;
  coverage.outputFormat = proof.vertexOutputProof.outputFormat;
  const uint64_t coverageEnd = PoisonRangeEnd(coverage);
  if (coverage.vertexOutputProof.resource.commonResource == 0u ||
      coverage.vertexOutputProof.resource.resourceGeneration == 0u ||
      coverageEnd > kMaxNativeVertices) {
    return;
  }

  NativePoisonLedger& ledger = t_state.poisonLedger;
  uint64_t clearMutations = 0u;
  for (size_t i = 0u; i < ledger.count;) {
    NativePoisonRange& range = ledger.ranges[i];
    const uint64_t rangeEnd = PoisonRangeEnd(range);
    if (!SameCpuOverwriteTarget(range, coverage) ||
        coverageEnd <= range.baseVertex ||
        uint64_t(coverage.baseVertex) >= rangeEnd) {
      ++i;
      continue;
    }

    if (coverage.baseVertex <= range.baseVertex &&
        coverageEnd >= rangeEnd) {
      RemovePoisonRange(i);
      ++clearMutations;
      continue;
    }
    if (coverage.baseVertex <= range.baseVertex) {
      range.baseVertex = static_cast<uint32_t>(coverageEnd);
      range.vertexCount = static_cast<uint32_t>(
          rangeEnd - coverageEnd);
      AdvancePoisonMutationGeneration();
      ++clearMutations;
    } else if (coverageEnd >= rangeEnd) {
      range.vertexCount = coverage.baseVertex - range.baseVertex;
      AdvancePoisonMutationGeneration();
      ++clearMutations;
    }
    ++i;
  }

  // A middle overwrite needs two surviving poison intervals. If the fixed
  // ledger cannot represent both, retain the wider poison range fail-closed.
  for (size_t i = 0u; i < ledger.count; ++i) {
    NativePoisonRange& range = ledger.ranges[i];
    const uint64_t rangeEnd = PoisonRangeEnd(range);
    if (!SameCpuOverwriteTarget(range, coverage) ||
        coverage.baseVertex <= range.baseVertex ||
        coverageEnd >= rangeEnd) {
      continue;
    }
    if (ledger.count >= ledger.ranges.size()) {
      g_counters.nativePoisonOverflows.fetch_add(
          1u, std::memory_order_relaxed);
      continue;
    }

    NativePoisonRange right = range;
    right.baseVertex = static_cast<uint32_t>(coverageEnd);
    right.vertexCount = static_cast<uint32_t>(rangeEnd - coverageEnd);
    range.vertexCount = coverage.baseVertex - range.baseVertex;
    ledger.ranges[ledger.count] = right;
    ++ledger.count;
    AdvancePoisonMutationGeneration();
    g_nativePoisonOutstandingRanges.fetch_add(
        1u, std::memory_order_relaxed);
    ++clearMutations;
  }
  if (clearMutations != 0u) {
    g_counters.nativePoisonClears.fetch_add(
        clearMutations, std::memory_order_relaxed);
  }
}

bool FindNativePoisonHit(const NativeDipInput& input,
                         NativePoisonRange& hit,
                         NativePoisonHitKind& hitKind,
                         bool recordDiagnostics = true) {
  hitKind = NativePoisonHitKind::None;
  const int64_t effectiveBase = int64_t(input.baseVertexIndex) +
      int64_t(input.minVertexIndex);
  const bool rangeExact = effectiveBase >= 0 && input.numVertices != 0u &&
      uint64_t(effectiveBase) + input.numVertices <=
          uint64_t(std::numeric_limits<uint32_t>::max()) + 1u;
  const uint64_t dipBegin = rangeExact
      ? static_cast<uint64_t>(effectiveBase) : 0u;
  const uint64_t dipEnd = rangeExact
      ? dipBegin + input.numVertices : 0u;
  const bool inputIdentityComplete =
      input.stream0Resource.commonResource != 0u &&
      input.stream0Resource.resourceGeneration != 0u;

  const NativePoisonLedger& ledger = t_state.poisonLedger;
  for (size_t i = 0u; i < ledger.count; ++i) {
    const NativePoisonRange& range = ledger.ranges[i];
    const NativeVertexOutputProof& proof = range.vertexOutputProof;

    // An incomplete common/generation pair cannot exclude any range in this
    // render thread's poison ledger. Suppress on its first entry regardless of
    // COM state; only a complete, explicitly different pair may continue.
    if (!inputIdentityComplete) {
      hit = range;
      hitKind = NativePoisonHitKind::IncompleteIdentity;
      if (recordDiagnostics) {
        g_counters.nativePoisonIncompleteIdentityHits.fetch_add(
            1u, std::memory_order_relaxed);
      }
      return true;
    }
    if (proof.resource.commonResource !=
            input.stream0Resource.commonResource ||
        proof.resource.resourceGeneration !=
            input.stream0Resource.resourceGeneration) {
      continue;
    }

    const bool formatsComplete =
        input.outputFormat < kFormatFvf.size() &&
        proof.outputFormat < kFormatFvf.size();
    const bool outputLayoutExact =
        input.stream0Resource.comVertexBuffer != 0u &&
        proof.resource.comVertexBuffer ==
            input.stream0Resource.comVertexBuffer &&
        input.nativeD3DDevice != 0u && proof.nativeD3DDevice != 0u &&
        proof.nativeD3DDevice == input.nativeD3DDevice && formatsComplete &&
        input.vertexStride != 0u && proof.vertexStride != 0u &&
        proof.vertexStride == input.vertexStride &&
        input.vertexStride == kFormatStride[input.outputFormat] &&
        proof.vertexStride == kFormatStride[proof.outputFormat] &&
        input.fvf != 0u && proof.fvf != 0u && proof.fvf == input.fvf &&
        input.fvf == kFormatFvf[input.outputFormat] &&
        proof.fvf == kFormatFvf[proof.outputFormat] &&
        proof.outputFormat == input.outputFormat &&
        range.stride == proof.vertexStride && range.fvf == proof.fvf &&
        range.outputFormat == proof.outputFormat;
    if (!outputLayoutExact) {
      // 0x6F0EEB85 writes with the producer layout and 0x6F0EEBB6 makes those
      // bytes irreversible. 0x6F0EEBCF/0x6F0EEC00 then set mutable FVF/stream
      // state, and 0x6F0EEA43 later issues DIP without a producer-layout proof.
      // Once common+generation match, a missing or different COM/device/layout
      // cannot safely translate vertex intervals: suppress the whole resource.
      hit = range;
      hitKind = NativePoisonHitKind::LayoutMismatch;
      if (recordDiagnostics) {
        g_counters.identityMatchedLayoutMismatchHits.fetch_add(
            1u, std::memory_order_relaxed);
      }
      return true;
    }
    if (!rangeExact) {
      hit = range;
      hitKind = NativePoisonHitKind::InexactRange;
      return true;
    }
    // Exact producer/consumer layout is the sole case where vertex intervals
    // are comparable; only then can a proven non-overlap avoid this range.
    if (dipEnd <= range.baseVertex || dipBegin >= PoisonRangeEnd(range)) {
      continue;
    }
    hit = range;
    hitKind = NativePoisonHitKind::ExactRangeOverlap;
    return true;
  }
  return false;
}

enum class PoisonDiagnosticRelation : uint8_t {
  Same = 0,
  Different,
  Mixed,
  Unknown,
};

PoisonDiagnosticRelation ClassifyPoisonGeneration(
    uint64_t current, uint64_t poison, bool mixed) {
  if (mixed)
    return PoisonDiagnosticRelation::Mixed;
  if (current == 0u || poison == 0u)
    return PoisonDiagnosticRelation::Unknown;
  return current == poison ? PoisonDiagnosticRelation::Same
                           : PoisonDiagnosticRelation::Different;
}

void IncrementPoisonRelation(PoisonDiagnosticRelation relation,
                             uint64_t& same, uint64_t& different,
                             uint64_t& mixed, uint64_t& unknown) {
  switch (relation) {
  case PoisonDiagnosticRelation::Same:
    ++same;
    break;
  case PoisonDiagnosticRelation::Different:
    ++different;
    break;
  case PoisonDiagnosticRelation::Mixed:
    ++mixed;
    break;
  case PoisonDiagnosticRelation::Unknown:
    ++unknown;
    break;
  }
}

void RecordPoisonOnlyDiagnostic(
    const NativeDipObservation& observation,
    const NativeDipDiagnosticInput& diagnostic,
    const NativePoisonRange& hit, NativePoisonHitKind hitKind) {
  NativePoisonDiagnosticSnapshot& snapshot = t_state.poisonDiagnostics;
  const uint64_t sequence = ++snapshot.poisonOnlyHits;

  switch (diagnostic.scope) {
  case NativeDipScope::ActiveUpload:
    ++snapshot.scopeActiveUpload;
    break;
  case NativeDipScope::OutsideDispatch:
    ++snapshot.scopeOutsideDispatch;
    break;
  case NativeDipScope::DispatchNoUpload:
    ++snapshot.scopeDispatchNoUpload;
    break;
  case NativeDipScope::ScopeHazard:
    ++snapshot.scopeHazard;
    break;
  default:
    ++snapshot.scopeUnknown;
    break;
  }

  switch (hitKind) {
  case NativePoisonHitKind::IncompleteIdentity:
    ++snapshot.hitIncompleteIdentity;
    break;
  case NativePoisonHitKind::LayoutMismatch:
    ++snapshot.hitLayoutMismatch;
    break;
  case NativePoisonHitKind::InexactRange:
    ++snapshot.hitInexactRange;
    break;
  case NativePoisonHitKind::ExactRangeOverlap:
    ++snapshot.hitExactRangeOverlap;
    break;
  default:
    ++snapshot.hitUnknown;
    break;
  }

  IncrementPoisonRelation(
      ClassifyPoisonGeneration(
          diagnostic.stream0Storage.realStorageGeneration,
          hit.storageDiagnostics.realStorageGeneration,
          hit.realStorageMixed || hit.mapModeMixed),
      snapshot.realStorageSame, snapshot.realStorageDifferent,
      snapshot.realStorageMixed, snapshot.realStorageUnknown);
  IncrementPoisonRelation(
      ClassifyPoisonGeneration(
          diagnostic.stream0Storage.mappingStorageGeneration,
          hit.storageDiagnostics.mappingStorageGeneration,
          hit.mappingStorageMixed || hit.mapModeMixed),
      snapshot.mappingStorageSame, snapshot.mappingStorageDifferent,
      snapshot.mappingStorageMixed, snapshot.mappingStorageUnknown);
  IncrementPoisonRelation(
      ClassifyPoisonGeneration(
          diagnostic.stream0Storage.mapAllocationGeneration,
          hit.storageDiagnostics.mapAllocationGeneration,
          hit.mapAllocationMixed || hit.mapModeMixed),
      snapshot.mapAllocationSame, snapshot.mapAllocationDifferent,
      snapshot.mapAllocationMixed, snapshot.mapAllocationUnknown);

  if (hit.indexSignatureMixed) {
    ++snapshot.indexSignatureMixed;
  } else if (hit.expectedIndexCount == 0u ||
             (hit.expectedIndexCount % 3u) != 0u) {
    ++snapshot.indexSignatureUnknown;
  } else if (observation.dip.primitiveType ==
                 kD3dPrimitiveTriangleList &&
             observation.dip.minVertexIndex == 0u &&
             observation.dip.startIndex == hit.expectedStartIndex &&
             observation.dip.primitiveCount ==
                 hit.expectedIndexCount / 3u) {
    ++snapshot.indexSignatureSame;
  } else {
    ++snapshot.indexSignatureDifferent;
  }

  if (snapshot.sampleCount >= snapshot.samples.size()) {
    ++snapshot.sampleOverflow;
    return;
  }
  NativePoisonDiagnosticSample& sample =
      snapshot.samples[snapshot.sampleCount++];
  sample.sequence = sequence;
  sample.currentEpoch = observation.epoch;
  sample.poisonEpoch = hit.epoch;
  sample.poisonFuseKey = hit.fuseKey;
  sample.currentStorage = diagnostic.stream0Storage;
  sample.poisonStorage = hit.storageDiagnostics;
  sample.dispatchPath = diagnostic.dispatchPath;
  sample.scope = diagnostic.scope;
  sample.hitKind = hitKind;
  sample.stage = diagnostic.stage;
  sample.batchTag = diagnostic.batchTag;
  sample.baseVertexIndex = observation.dip.baseVertexIndex;
  sample.minVertexIndex = observation.dip.minVertexIndex;
  sample.numVertices = observation.dip.numVertices;
  sample.startIndex = observation.dip.startIndex;
  sample.primitiveCount = observation.dip.primitiveCount;
  sample.primitiveType = observation.dip.primitiveType;
  sample.poisonBaseVertex = hit.baseVertex;
  sample.poisonVertexCount = hit.vertexCount;
  sample.originExpectedStartIndex = hit.expectedStartIndex;
  sample.originExpectedIndexCount = hit.expectedIndexCount;
  sample.poisonRealStorageMixed = hit.realStorageMixed;
  sample.poisonMappingStorageMixed = hit.mappingStorageMixed;
  sample.poisonMapAllocationMixed = hit.mapAllocationMixed;
  sample.poisonMapModeMixed = hit.mapModeMixed;
  sample.poisonIndexSignatureMixed = hit.indexSignatureMixed;
}

void ApplyNativePoisonObservation(
    NativeDipObservation& observation,
    const NativeDipDiagnosticInput& diagnostic) {
  NativePoisonRange hit = {};
  NativePoisonHitKind hitKind = NativePoisonHitKind::None;
  if (!FindNativePoisonHit(observation.dip, hit, hitKind))
    return;

  const bool hadBypassedSource = observation.sourceUploadKernelBypassed;
  if (!hadBypassedSource) {
    RecordPoisonOnlyDiagnostic(
        observation, diagnostic, hit, hitKind);
  }
  observation.nativeRangePoisoned = true;
  observation.nativePoisonHadBypassedSource = hadBypassedSource;
  observation.requiresSuppression = true;
  observation.sourceUploadKernelBypassed = true;
  if (observation.sourceUploadFuseKey == 0u)
    observation.sourceUploadFuseKey = hit.fuseKey;
  g_counters.nativePoisonHits.fetch_add(1u, std::memory_order_relaxed);
}

void UpdateMax(std::atomic<uint64_t>& target, uint64_t value) {
  uint64_t current = target.load(std::memory_order_relaxed);
  while (current < value &&
         !target.compare_exchange_weak(current, value,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
  }
}

void RecordSampledTiming(std::atomic<uint64_t>& calls,
                         std::atomic<uint64_t>& total,
                         std::atomic<uint64_t>& maximum,
                         int64_t start,
                         int64_t end) noexcept {
  if (start <= 0 || end <= start)
    return;
  const uint64_t ticks = uint64_t(end - start);
  calls.fetch_add(1u, std::memory_order_relaxed);
  total.fetch_add(ticks, std::memory_order_relaxed);
  UpdateMax(maximum, ticks);
}

uint64_t SampleDurationTicks(int64_t start, int64_t end) noexcept {
  return start > 0 && end >= start ? uint64_t(end - start) : 0u;
}

void RecordSampledDuration(std::atomic<uint64_t>& calls,
                           std::atomic<uint64_t>& total,
                           std::atomic<uint64_t>& maximum,
                           uint64_t ticks) noexcept {
  // A very short stage can legitimately land in one QPC tick.  Count that
  // sample so all six T2 sub-stages remain exactly paired with exact.calls.
  calls.fetch_add(1u, std::memory_order_relaxed);
  total.fetch_add(ticks, std::memory_order_relaxed);
  UpdateMax(maximum, ticks);
}

uint64_t NextIndexTicketGeneration() {
  uint64_t generation = g_nextIndexTicketGeneration.fetch_add(
      1u, std::memory_order_relaxed) + 1u;
  if (generation == 0u) {
    generation = g_nextIndexTicketGeneration.fetch_add(
        1u, std::memory_order_relaxed) + 1u;
  }
  return generation;
}

void RecordIndexTicketFailures(uint64_t failureMask) {
  if (failureMask == NativeIndexTicketFailureNone)
    return;
  g_counters.indexTicketFailureMask.fetch_or(
      failureMask, std::memory_order_relaxed);
  for (size_t i = 0u; i < kNativeIndexTicketFailureBitCount; ++i) {
    if ((failureMask & (uint64_t{1} << i)) != 0u) {
      g_counters.indexTicketFailureReasons[i].fetch_add(
          1u, std::memory_order_relaxed);
    }
  }
}

void SuppressIndexTicketObservation(NativeUploadObservation& upload,
                                    uint64_t failureMask) {
  NativeIndexTicketObservation& ticket = upload.indexTicket;
  const uint64_t newFailures = failureMask & ~ticket.failureMask;
  ticket.failureMask |= failureMask;
  RecordIndexTicketFailures(newFailures);

  ticket.exact = false;
  upload.observedPreflight &= ~NativePreflightIndexPathActualProof;
  upload.nativeObservationEligible = false;
  upload.takeoverEligible = false;
  upload.postSkipMismatchMask |= 1u << 7;
  upload.bypassFailure = NativeBypassFailureReason::PostSkipMismatch;
  if (!ticket.suppressed) {
    ticket.suppressed = true;
    g_counters.indexTicketSuppressed.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (!upload.postSkipMismatch) {
    upload.postSkipMismatch = true;
    g_counters.postSkipMismatchFuses.fetch_add(
        1u, std::memory_order_relaxed);
    g_counters.bypassStateMismatches.fetch_add(
        1u, std::memory_order_relaxed);
  }
}

void SuppressIndexTicket(ActiveUploadState& active,
                         uint64_t failureMask) {
  SuppressIndexTicketObservation(active.observation, failureMask);
}

void FinalizeIndexTicket(ActiveUploadState& active) {
  NativeIndexTicketObservation& ticket = active.observation.indexTicket;
  const bool attempted =
      (ticket.stageMask & NativeIndexTicketStageLockAttempt) != 0u;
  const bool consumed =
      (ticket.stageMask & NativeIndexTicketStageDipConsumed) != 0u;
  if (!active.observation.cpuSkinKernelBypassed || consumed ||
      ticket.suppressed) {
    return;
  }

  if (!ticket.leaked) {
    ticket.leaked = true;
    g_counters.indexTicketLeaks.fetch_add(1u, std::memory_order_relaxed);
  }
  uint64_t failureMask = NativeIndexTicketFailureTicketLeak;
  if (!attempted)
    failureMask |= NativeIndexTicketFailureActualProofMissing;
  SuppressIndexTicket(active, failureMask);
}

void PublishUploadFanout(const NativeUploadObservation& upload,
                         uint32_t dipCount,
                         bool notifyCallback = true) {
  if (dipCount == 0u)
    g_counters.uploadFanoutZero.fetch_add(1u, std::memory_order_relaxed);
  else if (dipCount == 1u)
    g_counters.uploadFanoutOne.fetch_add(1u, std::memory_order_relaxed);
  else
    g_counters.uploadFanoutMany.fetch_add(1u, std::memory_order_relaxed);
  UpdateMax(g_counters.maxUploadFanout, dipCount);

  if (!notifyCallback ||
      !ManagerCallbackAllowedForEpoch(
          upload.epoch.dispatchEpoch,
          ManagerSkippedCallbackKind::Fanout)) {
    return;
  }
  NativeBridgeCallbackPin callbackPin(
      NativeBridgeCallbackKind::Fanout, upload.epoch.uploadEpoch);
  const NativeBridgeCallbacks* callbacks = callbackPin.get();
  if (callbacks != nullptr && callbacks->onUploadFanout != nullptr) {
    NativeUploadFanoutObservation fanout = {};
    fanout.epoch = upload.epoch;
    fanout.path = upload.path;
    fanout.sourceUploadFuseKey = upload.fuseKey;
    fanout.dipCount = dipCount;
    fanout.indexTicket = upload.indexTicket;
    fanout.sourceUploadKernelBypassed = upload.cpuSkinKernelBypassed;
    fanout.sourceUploadPostSkipMismatch = upload.postSkipMismatch;
    callbackPin.BeginCallbackBody();
    callbacks->onUploadFanout(callbacks->userData, fanout);
    callbackPin.EndCallbackBody();
  }
}

void FinalizeActiveUpload(DispatchFrame& dispatch) {
  ActiveUploadState& active = dispatch.activeUpload;
  if (!active.valid)
    return;

  FinalizeIndexTicket(active);
  active.observation.fanoutCount = active.dipCount;
  PublishUploadFanout(active.observation, active.dipCount);
  RecycleIndexSnapshotStorage(active.indexSnapshot);
  active = {};
}

void FinalizeDispatchCpuOnlyActiveUpload(DispatchFrame& dispatch) noexcept {
  DispatchCpuOnlyActiveUpload& active = dispatch.cpuOnlyActiveUpload;
  if (!active.valid)
    return;

  if (active.dipCount == 0u) {
    AddNativeTelemetryCounter(
        g_counters.uploadFanoutZero,
        t_nativeTelemetryDelta.uploadFanoutZero);
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealFanoutZero,
        t_nativeTelemetryDelta.dispatchCpuOnlySealFanoutZero);
  } else if (active.dipCount == 1u) {
    AddNativeTelemetryCounter(
        g_counters.uploadFanoutOne,
        t_nativeTelemetryDelta.uploadFanoutOne);
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealFanoutOne,
        t_nativeTelemetryDelta.dispatchCpuOnlySealFanoutOne);
  } else {
    AddNativeTelemetryCounter(
        g_counters.uploadFanoutMany,
        t_nativeTelemetryDelta.uploadFanoutMany);
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealFanoutMany,
        t_nativeTelemetryDelta.dispatchCpuOnlySealFanoutMany);
  }
  AddNativeTelemetryCounter(
      g_counters.dispatchCpuOnlySealFanoutDipTotal,
      t_nativeTelemetryDelta.dispatchCpuOnlySealFanoutDipTotal,
      active.dipCount);
  UpdateMax(g_counters.maxUploadFanout, active.dipCount);
  active = {};
}

void InvalidateDispatchCpuOnlySeal(DispatchFrame& dispatch) noexcept {
  AbortDispatchCpuOnlySealProposal(dispatch);
  if (!dispatch.cpuOnlySeal.committed)
    return;
  dispatch.cpuOnlySeal.committed = false;
  AddNativeTelemetryCounter(
      g_counters.dispatchCpuOnlySealInvalidations,
      t_nativeTelemetryDelta.dispatchCpuOnlySealInvalidations);
  NativeDispatchCpuOnlyUploadFastPathState& marker =
      t_state.dispatchCpuOnlyUploadFastPath;
  if (marker.cookie != 0u && marker.dispatchEpoch ==
          dispatch.observation.epoch.dispatchEpoch && !marker.conflicted) {
    marker.conflicted = true;
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealMarkerConflicts,
        t_nativeTelemetryDelta.dispatchCpuOnlySealMarkerConflicts);
  }
  FinalizeDispatchCpuOnlyActiveUpload(dispatch);
}

void PoisonActiveBypassForScopeHazard(DispatchFrame& dispatch) {
  ActiveUploadState& active = dispatch.activeUpload;
  if (!active.valid || !active.observation.cpuSkinKernelBypassed) {
    return;
  }

  if (!active.observation.scopeFailClosed) {
    active.observation.scopeFailClosed = true;
    g_counters.scopeFailClosedUploads.fetch_add(
        1u, std::memory_order_relaxed);
  }
  active.observation.postSkipMismatchMask |= 1u << 6;
  SuppressIndexTicket(active, NativeIndexTicketFailureScopeHazard);
}

bool CurrentThreadIsObservedRenderThread() {
  const uint64_t observed =
      g_observedRenderThreadId.load(std::memory_order_acquire);
  return observed != 0u && observed == ::GetCurrentThreadId();
}

bool CurrentThreadTlsQuiescent() {
  return t_state.flushTransactionDepth == 0u &&
      t_state.dispatchDepth == 0u && t_state.semanticDepth == 0u &&
      t_state.dispatchOverflowDepth == 0u &&
      t_state.semanticOverflowDepth == 0u &&
      t_state.nestedUploadDepth == 0u &&
      t_state.inFlightUpload.observation == nullptr &&
      t_state.productionFastRejectedUpload == nullptr &&
      !t_state.productionFastRejectedGlobalTransactionPinned &&
      t_state.productionEvidenceUploadEpoch == 0u &&
      !OutsideNoPoisonDirectOriginalActive() &&
      !OutsideUploadFastPathActive() &&
      !DispatchCpuOnlyUploadFastPathActive() &&
      t_callbackPinDepth == 0u && t_dipObserverDepth == 0u &&
      t_dipObserverCookie == 0u &&
      t_dipObserverResetGeneration == 0u &&
      t_dipObserverCpuOnlySealCookie == 0u;
}

bool GlobalBridgeTransactionsQuiescent() {
  // A live flush pin covers every normal inner dispatch/semantic/upload scope.
  // The three inner counters therefore represent standalone and exceptional
  // pins, not a process-wide count of all live TLS frames. Full DIP observers
  // and strict outside-DIP readers are independent drain-visible domains.
  // Quiescence is the conjunction of every one of those covers.
  return g_activeFlushTransactions.load(std::memory_order_seq_cst) == 0u &&
      g_activeDispatchTransactions.load(std::memory_order_seq_cst) == 0u &&
      g_activeSemanticTransactions.load(std::memory_order_seq_cst) == 0u &&
      g_activeUploadTransactions.load(std::memory_order_seq_cst) == 0u &&
      g_activeDipObserverTransactions.load(
          std::memory_order_seq_cst) == 0u &&
      g_activeOutsideDipReaderTransactions.load(
          std::memory_order_seq_cst) == 0u &&
      g_activeCallbackPins.load(std::memory_order_acquire) == 0u &&
      g_counters.pendingKernelAuthorizations.load(
          std::memory_order_acquire) == 0u;
}

struct RetirementConsumeResult {
  uint32_t events = 0u;
  uint32_t ranges = 0u;
};

RetirementConsumeResult ConsumeNativeResourceRetirements() {
  RetirementConsumeResult result = {};
  if (!CurrentThreadIsObservedRenderThread() ||
      !CurrentThreadTlsQuiescent() ||
      !GlobalBridgeTransactionsQuiescent()) {
    return result;
  }

  std::array<NativeResourceRetirementEvent, kMaxNativeRetirementEvents>
      events{};
  ::AcquireSRWLockExclusive(&g_retirementQueueLock);
  const size_t eventCount = g_retirementEventCount;
  for (size_t i = 0u; i < eventCount; ++i) {
    events[i] = g_retirementEvents[
        (g_retirementEventHead + i) % g_retirementEvents.size()];
  }
  g_retirementEvents = {};
  g_retirementEventHead = 0u;
  g_retirementEventCount = 0u;
  g_retirementEventsPending.store(0u, std::memory_order_release);
  ::ReleaseSRWLockExclusive(&g_retirementQueueLock);

  result.events = static_cast<uint32_t>(eventCount);
  NativePoisonLedger& ledger = t_state.poisonLedger;
  for (size_t eventIndex = 0u; eventIndex < eventCount; ++eventIndex) {
    const NativeResourceRetirementEvent& event = events[eventIndex];
    for (size_t rangeIndex = 0u; rangeIndex < ledger.count;) {
      const NativeVertexResourceIdentity& identity =
          ledger.ranges[rangeIndex].vertexOutputProof.resource;
      if (identity.commonResource == event.commonResource &&
          identity.resourceGeneration == event.resourceGeneration) {
        RemovePoisonRange(rangeIndex);
        ++result.ranges;
        continue;
      }
      ++rangeIndex;
    }
  }
  if (result.events != 0u) {
    g_counters.nativeRetirementEventsConsumed.fetch_add(
        result.events, std::memory_order_relaxed);
  }
  if (result.ranges != 0u) {
    g_counters.nativeRetirementRangesCleared.fetch_add(
        result.ranges, std::memory_order_relaxed);
    g_counters.nativePoisonClears.fetch_add(
        result.ranges, std::memory_order_relaxed);
  }
  return result;
}

void RecordResetDeferred(uint64_t generation,
                         NativeBridgeResetStatus status) {
  if (t_state.deferredResetGeneration == generation &&
      t_state.deferredResetStatus == status) {
    return;
  }
  t_state.deferredResetGeneration = generation;
  t_state.deferredResetStatus = status;
  g_counters.resetDeferred.fetch_add(1u, std::memory_order_relaxed);
  switch (status) {
    case NativeBridgeResetStatus::DeferredNoObservedRenderThread:
    case NativeBridgeResetStatus::DeferredWrongThread:
      g_counters.resetWrongThread.fetch_add(1u, std::memory_order_relaxed);
      break;
    case NativeBridgeResetStatus::DeferredActiveTransactions:
      g_counters.resetActiveTransactions.fetch_add(
          1u, std::memory_order_relaxed);
      break;
    case NativeBridgeResetStatus::DeferredOwnerEpoch:
      g_counters.resetOwnerEpochDeferred.fetch_add(
          1u, std::memory_order_relaxed);
      break;
    case NativeBridgeResetStatus::DeferredPoisonRetirement:
      g_counters.resetPoisonDeferred.fetch_add(
          1u, std::memory_order_relaxed);
      break;
    case NativeBridgeResetStatus::DeferredRetirementQueueFault:
      g_counters.resetRetirementQueueFaults.fetch_add(
          1u, std::memory_order_relaxed);
      break;
    case NativeBridgeResetStatus::Completed:
      break;
  }
}

void MarkResetFailClosed(NativeUploadObservation& upload) {
  upload.scopeFailClosed = true;
  if (!upload.cpuSkinKernelBypassed)
    return;
  constexpr uint32_t kResetSuppressedMask = 1u << 8;
  if ((upload.postSkipMismatchMask & kResetSuppressedMask) == 0u) {
    upload.postSkipMismatchMask |= kResetSuppressedMask;
    g_counters.resetFailClosedUploads.fetch_add(
        1u, std::memory_order_relaxed);
  }
  upload.bypassFailure = NativeBypassFailureReason::PostSkipMismatch;
  SuppressIndexTicketObservation(
      upload, NativeIndexTicketFailureScopeHazard);
}

void ApplyPendingResetFailClosed() {
  const uint64_t requested =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  if (requested == 0u || requested ==
      g_resetCompletedGeneration.load(std::memory_order_acquire) ||
      !CurrentThreadIsObservedRenderThread()) {
    return;
  }

  InvalidateNativeDispatchCpuOnlySealView();
  for (size_t i = 0u; i < t_state.dispatchDepth; ++i) {
    DispatchFrame& dispatch = t_state.dispatchFrames[i];
    InvalidateDispatchCpuOnlySeal(dispatch);
    ActiveUploadState& active = dispatch.activeUpload;
    if (active.valid)
      MarkResetFailClosed(active.observation);
  }
  if (t_state.inFlightUpload.observation != nullptr)
    MarkResetFailClosed(*t_state.inFlightUpload.observation);
  const size_t nestedCount = std::min(
      t_state.nestedUploadDepth,
      t_state.nestedUploadObservations.size());
  for (size_t i = 0u; i < nestedCount; ++i) {
    if (t_state.nestedUploadObservations[i] != nullptr)
      MarkResetFailClosed(*t_state.nestedUploadObservations[i]);
  }
}

NativeBridgeResetResult ProcessPendingBridgeReset() {
  NativeBridgeResetResult result = {};

  // The overwhelmingly common upload/dispatch path has neither a pending
  // reset nor a resource-retirement event. Make that state lock-free. A
  // racing publisher linearizes after these acquire loads and is consumed at
  // the next native boundary; real reset work (requested != completed) and
  // every published retirement event still enter the protocol-locked path.
  const uint64_t fastRequested =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  const uint64_t fastCompleted =
      g_resetCompletedGeneration.load(std::memory_order_acquire);
  if ((fastRequested == 0u || fastRequested == fastCompleted) &&
      g_retirementEventsPending.load(std::memory_order_acquire) == 0u) {
    result.status = NativeBridgeResetStatus::Completed;
    result.requestedGeneration = fastRequested;
    result.completedGeneration = fastCompleted;
    return result;
  }
  g_counters.resetSlowPathCalls.fetch_add(
      1u, std::memory_order_relaxed);

  uint64_t requested = 0u;
  uint64_t completed = 0u;
  uint64_t ownerRetired = 0u;
  bool ownerAckRequired = false;
  {
    ResetProtocolLockGuard protocolLock;
    requested = g_resetRequestedGeneration.load(std::memory_order_acquire);
    completed = g_resetCompletedGeneration.load(std::memory_order_relaxed);
    ownerRetired = g_ownerRetiredGeneration.load(std::memory_order_relaxed);
    ownerAckRequired = g_resetOwnerAckRequired.load(
        std::memory_order_relaxed);
    result.reason = static_cast<NativePoisonLedgerResetReason>(
        g_resetReason.load(std::memory_order_relaxed));
  }
  result.requestedGeneration = requested;
  result.completedGeneration = completed;
  result.currentThreadIsObservedRenderThread =
      CurrentThreadIsObservedRenderThread();
  result.currentThreadTlsQuiescent = CurrentThreadTlsQuiescent();
  const RetirementConsumeResult retired =
      ConsumeNativeResourceRetirements();
  result.poisonRangesCleared = retired.ranges;
  result.resourceRetirementObserved = retired.events != 0u;
  uint64_t globalPoisonOutstanding =
      g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
  result.poisonRangesOutstanding = static_cast<uint32_t>(std::min<uint64_t>(
      globalPoisonOutstanding, std::numeric_limits<uint32_t>::max()));
  result.retirementEventsPending = static_cast<uint32_t>(
      std::min<uint64_t>(
          g_retirementEventsPending.load(std::memory_order_acquire),
          std::numeric_limits<uint32_t>::max()));
  result.retirementQueueFaulted =
      g_retirementQueueFaulted.load(std::memory_order_acquire);
  result.ownerEpochRetired = !ownerAckRequired ||
      ownerRetired == requested;

  if (requested == 0u || completed == requested) {
    result.status = NativeBridgeResetStatus::Completed;
    return result;
  }

  const uint64_t observed =
      g_observedRenderThreadId.load(std::memory_order_acquire);
  if (observed == 0u) {
    result.status =
        NativeBridgeResetStatus::DeferredNoObservedRenderThread;
    RecordResetDeferred(requested, result.status);
    return result;
  }
  if (!result.currentThreadIsObservedRenderThread) {
    result.status = NativeBridgeResetStatus::DeferredWrongThread;
    RecordResetDeferred(requested, result.status);
    return result;
  }

  ApplyPendingResetFailClosed();
  result.currentThreadTlsQuiescent = CurrentThreadTlsQuiescent();
  if (result.currentThreadIsObservedRenderThread &&
      result.currentThreadTlsQuiescent) {
    FlushNativeTelemetryDeltaForCurrentThread();
  }
  if (!result.currentThreadTlsQuiescent ||
      !GlobalBridgeTransactionsQuiescent() ||
      g_nativeTelemetryDeltaPending.load(std::memory_order_acquire) != 0u) {
    result.status = NativeBridgeResetStatus::DeferredActiveTransactions;
    RecordResetDeferred(requested, result.status);
    return result;
  }
  if (!result.ownerEpochRetired) {
    result.status = NativeBridgeResetStatus::DeferredOwnerEpoch;
    RecordResetDeferred(requested, result.status);
    return result;
  }

  if (result.retirementQueueFaulted) {
    result.status = NativeBridgeResetStatus::DeferredRetirementQueueFault;
    RecordResetDeferred(requested, result.status);
    return result;
  }

  globalPoisonOutstanding =
      g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
  result.poisonRangesOutstanding = static_cast<uint32_t>(std::min<uint64_t>(
      globalPoisonOutstanding, std::numeric_limits<uint32_t>::max()));
  if (globalPoisonOutstanding != 0u ||
      t_state.poisonLedger.count != 0u ||
      g_retirementEventsPending.load(std::memory_order_acquire) != 0u) {
    result.status = NativeBridgeResetStatus::DeferredPoisonRetirement;
    RecordResetDeferred(requested, result.status);
    return result;
  }

  {
    ResetProtocolLockGuard protocolLock;
    const uint64_t currentRequested =
        g_resetRequestedGeneration.load(std::memory_order_acquire);
    const bool exactOwnerAck =
        !g_resetOwnerAckRequired.load(std::memory_order_relaxed) ||
        g_ownerRetiredGeneration.load(std::memory_order_relaxed) == requested;
    if (currentRequested != requested || !exactOwnerAck ||
        g_retirementQueueFaulted.load(std::memory_order_acquire) ||
        g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
        g_nativePoisonOutstandingRanges.load(std::memory_order_acquire) != 0u ||
        g_nativeTelemetryDeltaPending.load(std::memory_order_acquire) != 0u ||
        !CurrentThreadTlsQuiescent() ||
        !GlobalBridgeTransactionsQuiescent()) {
      result.status = NativeBridgeResetStatus::DeferredActiveTransactions;
      RecordResetDeferred(currentRequested, result.status);
      return result;
    }

    AbortOutsidePoisonShadowForReset();
    t_state = {};
    // Reset owns a semantic ledger epoch transition even though completion is
    // allowed only after every poison range has retired. Keep the independent
    // TLS sequence monotonic across the whole-ThreadState clear.
    AdvancePoisonMutationGeneration();
    t_state.appliedResetGeneration = requested;
    g_resetCompletedGeneration.store(requested, std::memory_order_release);
    const bool canResumeBypass =
        g_hooksEnabled.load(std::memory_order_acquire) &&
        g_callbackIngressEnabled.load(std::memory_order_acquire) &&
        !g_retirementQueueFaulted.load(std::memory_order_relaxed);
    g_bypassEnabled.store(canResumeBypass, std::memory_order_release);
  }
  result.completedGeneration = requested;
  result.status = NativeBridgeResetStatus::Completed;
  g_counters.resetCompletions.fetch_add(1u, std::memory_order_relaxed);
  return result;
}

ActiveUploadState* FindBypassedScopeHazard() {
  if (!ScopeHazardActive())
    return nullptr;
  for (size_t depth = t_state.dispatchDepth; depth != 0u; --depth) {
    ActiveUploadState& active =
        t_state.dispatchFrames[depth - 1u].activeUpload;
    if (active.valid && active.observation.cpuSkinKernelBypassed)
      return &active;
  }
  return nullptr;
}

NativeDispatchPath ClassifySemanticPath(uintptr_t callerAddress,
                                        NativeDispatchPath dispatchPath,
                                        bool& singlePassCaller) {
  singlePassCaller = false;
  const uintptr_t callerRva =
      callerAddress >= g_gameBase ? callerAddress - g_gameBase : 0u;
  if (callerRva == kMultiPassApplyReturnRva0 ||
      callerRva == kMultiPassApplyReturnRva1) {
    return NativeDispatchPath::MultiPass;
  }
  if (callerRva == kNormalApplyReturnRva)
    singlePassCaller = true;
  if (dispatchPath == NativeDispatchPath::Special)
    return NativeDispatchPath::Special;
  if (dispatchPath == NativeDispatchPath::Transparent)
    return NativeDispatchPath::Transparent;
  if (dispatchPath != NativeDispatchPath::Common || !singlePassCaller)
    return NativeDispatchPath::Unknown;
  return NativeDispatchPath::Common;
}

bool ReadFastU32(uintptr_t base, size_t offset, uint32_t& value) {
  if (base == 0u || offset > std::numeric_limits<uintptr_t>::max() - base)
    return false;
  return dxvk::war3::SafeCopy(
      &value, reinterpret_cast<const void*>(base + offset), sizeof(value));
}

bool ReadFastPtr(uintptr_t base, size_t offset, uintptr_t& value) {
  uint32_t pointer = 0u;
  if (base == 0u || offset > std::numeric_limits<uintptr_t>::max() - base ||
      !dxvk::war3::SafeCopy(
          &pointer, reinterpret_cast<const void*>(base + offset),
          sizeof(pointer))) {
    return false;
  }
  value = pointer;
  return true;
}

void CaptureGeosetSnapshot(uintptr_t geosetData, uint64_t uploadEpoch,
                           NativeGeosetSnapshotState& snapshot) {
  snapshot.geosetData = geosetData;
  snapshot.uploadEpoch = uploadEpoch;
  snapshot.ready = false;
  if (geosetData == 0u || uploadEpoch == 0u ||
      kGeosetStateSnapshotBeginOffset >
          std::numeric_limits<uintptr_t>::max() - geosetData) {
    return;
  }
  // CGeosetData's proof fields are one immutable resource header span. If a
  // page boundary makes the contiguous read fail, every accessor below falls
  // back to the former exact field-by-field SafeCopy path.
  snapshot.ready = dxvk::war3::SafeCopy(
      snapshot.bytes.data(),
      reinterpret_cast<const void*>(
          geosetData + kGeosetStateSnapshotBeginOffset),
      snapshot.bytes.size());
}

bool ReadGeosetSnapshotDword(const NativeGeosetSnapshotState* snapshot,
                             uintptr_t geosetData, uint64_t uploadEpoch,
                             size_t offset, uint32_t& value) {
  if (snapshot != nullptr && snapshot->ready &&
      snapshot->geosetData == geosetData &&
      snapshot->uploadEpoch == uploadEpoch &&
      offset >= kGeosetStateSnapshotBeginOffset) {
    const size_t relativeOffset =
        offset - kGeosetStateSnapshotBeginOffset;
    if (relativeOffset <= snapshot->bytes.size() - sizeof(value)) {
      std::memcpy(&value, snapshot->bytes.data() + relativeOffset,
                  sizeof(value));
      return true;
    }
  }
  return ReadFastU32(geosetData, offset, value);
}

bool ReadGeosetSnapshotPtr(const NativeGeosetSnapshotState* snapshot,
                           uintptr_t geosetData, uint64_t uploadEpoch,
                           size_t offset, uintptr_t& value) {
  uint32_t pointer = 0u;
  if (!ReadGeosetSnapshotDword(
          snapshot, geosetData, uploadEpoch, offset, pointer)) {
    return false;
  }
  value = pointer;
  return true;
}

class GxDeviceStateReader {
public:
  explicit GxDeviceStateReader(uintptr_t base)
      : m_base(base),
        m_snapshotReady(CaptureSnapshot(base, m_snapshot)) {
  }

  bool ReadU32(size_t offset, uint32_t& value) const {
    if (ReadSnapshotDword(offset, value))
      return true;
    return ReadFastU32(m_base, offset, value);
  }

  bool ReadPtr(size_t offset, uintptr_t& value) const {
    uint32_t pointer = 0u;
    if (ReadSnapshotDword(offset, pointer)) {
      value = pointer;
      return true;
    }
    return ReadFastPtr(m_base, offset, value);
  }

private:
  static bool CaptureSnapshot(
      uintptr_t base,
      std::array<uint8_t, kGxStateSnapshotSize>& snapshot) {
    if (base == 0u ||
        kGxStateSnapshotBeginOffset >
            std::numeric_limits<uintptr_t>::max() - base) {
      return false;
    }
    return dxvk::war3::SafeCopy(
        snapshot.data(),
        reinterpret_cast<const void*>(base + kGxStateSnapshotBeginOffset),
        snapshot.size());
  }

  bool ReadSnapshotDword(size_t offset, uint32_t& value) const {
    if (!m_snapshotReady || offset < kGxStateSnapshotBeginOffset)
      return false;
    const size_t relativeOffset = offset - kGxStateSnapshotBeginOffset;
    if (relativeOffset > m_snapshot.size() - sizeof(value))
      return false;
    std::memcpy(&value, m_snapshot.data() + relativeOffset, sizeof(value));
    return true;
  }

  uintptr_t m_base = 0u;
  std::array<uint8_t, kGxStateSnapshotSize> m_snapshot;
  bool m_snapshotReady = false;
};

struct IndexBytesProof {
  uint64_t hash = 0;
  uint32_t count = 0;
  uint32_t minIndex = 0;
  uint32_t maxIndex = 0;
};

bool ComputeIndexBytesProofFromSnapshot(const std::vector<uint8_t>& bytes,
                                        uint32_t count,
                                        IndexBytesProof& proof) {
  proof = {};
  if (count == 0u || count > kMaxNativeIndices ||
      bytes.size() != size_t(count) * sizeof(uint16_t)) {
    return false;
  }

  constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
  constexpr uint64_t kFnvPrime = 0x100000001b3ull;
  uint64_t hash = kFnvOffset;
  uint32_t minIndex = std::numeric_limits<uint16_t>::max();
  uint32_t maxIndex = 0u;
  for (size_t i = 0u; i < bytes.size(); i += sizeof(uint16_t)) {
    hash = (hash ^ bytes[i]) * kFnvPrime;
    hash = (hash ^ bytes[i + 1u]) * kFnvPrime;
    const uint32_t index = uint32_t(bytes[i]) |
        (uint32_t(bytes[i + 1u]) << 8u);
    minIndex = std::min(minIndex, index);
    maxIndex = std::max(maxIndex, index);
  }

  proof.hash = hash;
  proof.count = count;
  proof.minIndex = minIndex;
  proof.maxIndex = maxIndex;
  return true;
}

uint64_t CaptureExpectedIndexSnapshot(
    const NativeUploadObservation& upload,
    uint32_t expectedIndexCount,
    const NativeGeosetSnapshotState* geosetSnapshot,
    std::vector<uint8_t>& snapshot,
    IndexBytesProof& proof) {
  snapshot.clear();
  proof = {};
  uint32_t primitiveCount = 0u;
  uint32_t primitiveType = 0u;
  uint32_t primitiveIndexCount = 0u;
  uint32_t liveIndexCount = 0u;
  uintptr_t primitiveRecords = 0u;
  uintptr_t liveIndices = 0u;
  if (upload.geosetData == 0u ||
      !ReadGeosetSnapshotDword(
          geosetSnapshot, upload.geosetData, upload.epoch.uploadEpoch,
          kGeosetPrimitiveCountOffset, primitiveCount) ||
      !ReadGeosetSnapshotPtr(
          geosetSnapshot, upload.geosetData, upload.epoch.uploadEpoch,
          kGeosetPrimitiveRecordsOffset, primitiveRecords) ||
      !ReadGeosetSnapshotDword(
          geosetSnapshot, upload.geosetData, upload.epoch.uploadEpoch,
          kGeosetIndexCountOffset, liveIndexCount) ||
      !ReadGeosetSnapshotPtr(
          geosetSnapshot, upload.geosetData, upload.epoch.uploadEpoch,
          kGeosetIndicesOffset, liveIndices) ||
      primitiveRecords == 0u || liveIndices == 0u) {
    return NativeIndexTicketFailureExpectedProofMissing;
  }
  std::array<uint32_t, 2u> primitiveSnapshot = {};
  if (dxvk::war3::SafeCopy(
          primitiveSnapshot.data(),
          reinterpret_cast<const void*>(primitiveRecords),
          sizeof(primitiveSnapshot))) {
    primitiveType = primitiveSnapshot[0u];
    primitiveIndexCount = primitiveSnapshot[1u];
  } else if (!ReadFastU32(primitiveRecords, 0u, primitiveType) ||
             !ReadFastU32(primitiveRecords, sizeof(uint32_t),
                          primitiveIndexCount)) {
    return NativeIndexTicketFailureExpectedProofMissing;
  }
  const uint32_t snapshotIndexCount = expectedIndexCount != 0u
      ? expectedIndexCount : liveIndexCount;
  if (snapshotIndexCount == 0u ||
      snapshotIndexCount > kMaxNativeIndices ||
      (snapshotIndexCount % 3u) != 0u || primitiveCount != 1u ||
      liveIndexCount != snapshotIndexCount ||
      primitiveIndexCount != snapshotIndexCount) {
    return NativeIndexTicketFailureIndexCountMismatch;
  }
  if (primitiveType != kGxPrimitiveTriangleList)
    return NativeIndexTicketFailureExpectedProofMissing;

  const size_t byteCount =
      size_t(snapshotIndexCount) * sizeof(uint16_t);
  static_assert(size_t(kMaxNativeIndices) * sizeof(uint16_t) == 0x18000u,
                "native INDEX16 snapshot bound must remain 0x18000 bytes");
  try {
    snapshot.resize(byteCount);
  } catch (...) {
    snapshot.clear();
    return NativeIndexTicketFailureSnapshotAllocationFailed;
  }
  if (!dxvk::war3::SafeCopy(
          snapshot.data(), reinterpret_cast<const void*>(liveIndices),
          byteCount)) {
    snapshot.clear();
    return NativeIndexTicketFailureExpectedProofMissing |
        NativeIndexTicketFailureMappedRangeUnreadable;
  }
  if (!ComputeIndexBytesProofFromSnapshot(
          snapshot, snapshotIndexCount, proof)) {
    snapshot.clear();
    return NativeIndexTicketFailureExpectedProofMissing;
  }
  if (upload.vertexCount == 0u || proof.maxIndex >= upload.vertexCount)
    return NativeIndexTicketFailureIndexRangeMismatch;
  return NativeIndexTicketFailureNone;
}

uint64_t PrepareExpectedIndexTicket(NativeUploadObservation& upload,
                                    uint32_t expectedIndexCount,
                                    const NativeGeosetSnapshotState*
                                        geosetSnapshot,
                                    NativeIndexSnapshotState& snapshotState,
                                    uint32_t& resolvedIndexCount,
                                    uint32_t& predictedStartIndex) {
  AcquireIndexSnapshotStorage(snapshotState);
  resolvedIndexCount = 0u;
  predictedStartIndex = 0u;
  IndexBytesProof proof = {};
  uint64_t failureMask = CaptureExpectedIndexSnapshot(
      upload, expectedIndexCount, geosetSnapshot, snapshotState.bytes,
      proof);
  resolvedIndexCount = proof.count;
  if (failureMask == NativeIndexTicketFailureNone &&
      (upload.indexRingBaseBefore > upload.indexRingNextBefore ||
       upload.indexRingNextBefore > kMaxNativeIndices)) {
    failureMask |= NativeIndexTicketFailureRingStateMismatch;
  }
  if (failureMask == NativeIndexTicketFailureNone) {
    predictedStartIndex = upload.indexRingNextBefore >
        kMaxNativeIndices - resolvedIndexCount
        ? 0u : upload.indexRingNextBefore;
  }
  if (predictedStartIndex > kMaxNativeIndices ||
      resolvedIndexCount > kMaxNativeIndices - predictedStartIndex) {
    failureMask |= NativeIndexTicketFailureLockOffsetMismatch |
        NativeIndexTicketFailureLockSizeMismatch;
  }
  if (failureMask != NativeIndexTicketFailureNone)
    return failureMask;

  const bool wraps = upload.indexRingNextBefore >
      kMaxNativeIndices - resolvedIndexCount;
  NativeIndexTicketObservation& ticket = upload.indexTicket;
  ticket = {};
  ticket.uploadEpoch = upload.epoch.uploadEpoch;
  ticket.expectedIndexHash = proof.hash;
  ticket.expectedIndexCount = proof.count;
  ticket.expectedMinIndex = proof.minIndex;
  ticket.expectedMaxIndex = proof.maxIndex;
  ticket.predictedStartIndex = predictedStartIndex;
  ticket.expectedLockOffset = predictedStartIndex * sizeof(uint16_t);
  ticket.expectedLockSize = resolvedIndexCount * sizeof(uint16_t);
  ticket.expectedLockFlags = wraps
      ? kD3dLockDiscard : kD3dLockNoOverwrite;
  ticket.stageMask = NativeIndexTicketStageExpectedProof;
  snapshotState.uploadEpoch = upload.epoch.uploadEpoch;
  return NativeIndexTicketFailureNone;
}

bool CompareMappedIndexBytes(
    const void* mappedPointer,
    const NativeIndexSnapshotState& snapshot,
    uint32_t count,
    IndexBytesProof& actual,
    bool& contentsExact) {
  actual = {};
  contentsExact = false;
  if (mappedPointer == nullptr || count == 0u || count > kMaxNativeIndices ||
      snapshot.bytes.size() != size_t(count) * sizeof(uint16_t)) {
    return false;
  }

  constexpr size_t kChunkSize = 512u;
  constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
  constexpr uint64_t kFnvPrime = 0x100000001b3ull;
  const uintptr_t mappedAddress =
      reinterpret_cast<uintptr_t>(mappedPointer);
  const size_t byteCount = snapshot.bytes.size();
  if (byteCount > std::numeric_limits<uintptr_t>::max() - mappedAddress)
    return false;

  std::array<uint8_t, kChunkSize> chunkBytes = {};
  uint64_t hash = kFnvOffset;
  uint32_t minIndex = std::numeric_limits<uint16_t>::max();
  uint32_t maxIndex = 0u;
  bool equal = true;
  for (size_t offset = 0u; offset < byteCount;) {
    const size_t chunk = std::min(kChunkSize, byteCount - offset);
    if ((chunk & 1u) != 0u ||
        !dxvk::war3::SafeCopy(
            chunkBytes.data(),
            reinterpret_cast<const void*>(mappedAddress + offset), chunk)) {
      return false;
    }
    if (std::memcmp(
            chunkBytes.data(), snapshot.bytes.data() + offset, chunk) != 0) {
      equal = false;
    }
    for (size_t i = 0u; i < chunk; ++i)
      hash = (hash ^ chunkBytes[i]) * kFnvPrime;
    for (size_t i = 0u; i < chunk; i += sizeof(uint16_t)) {
      const uint32_t index = uint32_t(chunkBytes[i]) |
          (uint32_t(chunkBytes[i + 1u]) << 8u);
      minIndex = std::min(minIndex, index);
      maxIndex = std::max(maxIndex, index);
    }
    offset += chunk;
  }

  actual.hash = hash;
  actual.count = count;
  actual.minIndex = minIndex;
  actual.maxIndex = maxIndex;
  contentsExact = equal;
  return true;
}

uint64_t ValidateIndexTicketContext(
    const ActiveUploadState& active,
    const NativeIndexResourceIdentity& resource,
    uint64_t ticketGeneration) {
  const NativeUploadObservation& upload = active.observation;
  const NativeIndexTicketObservation& ticket = upload.indexTicket;
  uint64_t failureMask = NativeIndexTicketFailureNone;
  if (upload.epoch.renderThreadId != ::GetCurrentThreadId())
    failureMask |= NativeIndexTicketFailureThreadMismatch;
  if (ticket.uploadEpoch == 0u ||
      ticket.uploadEpoch != upload.epoch.uploadEpoch)
    failureMask |= NativeIndexTicketFailureUploadEpochMismatch;
  if (ticket.ticketGeneration == 0u || ticketGeneration == 0u ||
      ticket.ticketGeneration != ticketGeneration)
    failureMask |= NativeIndexTicketFailureTicketGenerationMismatch;
  if (resource.resourceGeneration == 0u)
    failureMask |= NativeIndexTicketFailureResourceGenerationMissing;
  else if (ticket.resourceGeneration != resource.resourceGeneration)
    failureMask |= NativeIndexTicketFailureResourceGenerationMismatch;
  if (resource.commonResource == 0u)
    failureMask |= NativeIndexTicketFailureCommonResourceMissing;
  if (resource.comIndexBuffer == 0u)
    failureMask |= NativeIndexTicketFailureComIndexBufferMissing;
  if (ticket.commonResource != resource.commonResource ||
      ticket.comIndexBuffer != resource.comIndexBuffer)
    failureMask |= NativeIndexTicketFailureResourceIdentityMismatch;
  if (resource.comIndexBuffer != upload.nativeIndexBuffer)
    failureMask |= NativeIndexTicketFailureNativeComIdentityMismatch;
  if (resource.indexFormat != kD3dFormatIndex16 ||
      ticket.indexFormat != resource.indexFormat)
    failureMask |= NativeIndexTicketFailureIndexFormatMismatch;
  return failureMask;
}

bool ReadLiveIndexRing(const NativeUploadObservation& upload,
                       uintptr_t& liveIndexBuffer,
                       uint32_t& liveStartIndex,
                       uint32_t& liveNextIndex) {
  return ReadFastPtr(upload.gxDeviceD3d, kGxIndexBufferOffset,
                     liveIndexBuffer) &&
      ReadFastU32(upload.gxDeviceD3d, kGxIndexRingBaseOffset,
                  liveStartIndex) &&
      ReadFastU32(upload.gxDeviceD3d, kGxIndexRingNextOffset,
                  liveNextIndex);
}

bool LiveIndexLockStateMatches(const NativeUploadObservation& upload,
                               uintptr_t expectedComIndexBuffer) {
  uintptr_t liveIndexBuffer = 0u;
  uint32_t liveStartIndex = 0u;
  uint32_t liveNextIndex = 0u;
  return ReadLiveIndexRing(upload, liveIndexBuffer, liveStartIndex,
                           liveNextIndex) &&
      liveIndexBuffer == expectedComIndexBuffer &&
      liveStartIndex == upload.predictedIndexRingBase &&
      liveNextIndex == upload.indexRingNextBefore;
}

bool LiveIndexRingMatches(const NativeUploadObservation& upload,
                          uintptr_t expectedComIndexBuffer) {
  uintptr_t liveIndexBuffer = 0u;
  uint32_t liveStartIndex = 0u;
  uint32_t liveNextIndex = 0u;
  return ReadLiveIndexRing(upload, liveIndexBuffer, liveStartIndex,
                           liveNextIndex) &&
      liveIndexBuffer == expectedComIndexBuffer &&
      liveStartIndex == upload.predictedIndexRingBase &&
      liveNextIndex == upload.predictedIndexRingNext;
}

bool IsReadableVertexRange(uintptr_t pointer, uint32_t count, uint32_t stride) {
  if (pointer == 0u || count == 0u || count > kMaxNativeVertices || stride == 0u)
    return false;
  const size_t byteCount = static_cast<size_t>(count) * stride;
  return dxvk::war3::IsReadableRangeFast(
      reinterpret_cast<const void*>(pointer), byteCount);
}

bool UvLayoutSupported(uint32_t format, const NativeUploadCall& call) {
  const bool uv0 = call.uv0 != 0u && call.uv0Stride == 8u;
  const bool uv1 = call.uv1 != 0u && call.uv1Stride == 8u;
  if (format <= 1u)
    return (call.uv0 == 0u || uv0) && (call.uv1 == 0u || uv1);
  if (format <= 3u)
    return uv0 && (call.uv1 == 0u || uv1);
  if (format <= 5u)
    return uv0 && uv1;
  return false;
}

bool ReadNativeDeviceState(NativeUploadObservation& observation, bool after) {
  const GxDeviceStateReader state(observation.gxDeviceD3d);
  uint32_t format = 0;
  uint32_t skinMode = 0;
  uint32_t paletteCount = 0;
  uint32_t submittedVertexCount = 0;
  uintptr_t palette = 0;
  bool readable =
      state.ReadU32(kGxOutputFormatOffset, format) &&
      state.ReadU32(kGxSkinModeOffset, skinMode) &&
      state.ReadU32(kGxPaletteCountOffset, paletteCount) &&
      state.ReadU32(kGxSubmittedVertexCountOffset, submittedVertexCount) &&
      state.ReadPtr(kGxPalettePtrOffset, palette);

  observation.outputFormat = format;
  observation.skinMode = skinMode;
  observation.paletteGroupCount = paletteCount;
  observation.palette = palette;
  if (format < kFormatFvf.size()) {
    observation.fvf = kFormatFvf[format];
    observation.outputStride = kFormatStride[format];
  }

  uintptr_t nativeDevice = 0;
  uintptr_t vertexBuffer = 0;
  uintptr_t indexBuffer = 0;
  uint32_t ringBase = 0;
  uint32_t ringNext = 0;
  uint32_t indexRingBase = 0;
  uint32_t indexRingNext = 0;
  uint32_t priorIndexBase = 0;
  readable = state.ReadPtr(kGxNativeDeviceOffset, nativeDevice) && readable;
  readable = state.ReadPtr(kGxIndexBufferOffset, indexBuffer) && readable;
  if (format < kFormatFvf.size()) {
    readable = state.ReadPtr(
        kGxVertexBufferArrayOffset + format * sizeof(uint32_t),
        vertexBuffer) && readable;
    readable = state.ReadU32(kGxVertexRingBaseOffset + format * 8u,
                             ringBase) && readable;
    readable = state.ReadU32(kGxVertexRingNextOffset + format * 8u,
                             ringNext) && readable;
  } else {
    readable = false;
  }
  readable = state.ReadU32(kGxIndexRingBaseOffset, indexRingBase) && readable;
  readable = state.ReadU32(kGxIndexRingNextOffset, indexRingNext) && readable;
  readable = state.ReadU32(kGxIndexBaseVertexOffset, priorIndexBase) &&
      readable;

  observation.nativeD3DDevice = nativeDevice;
  observation.nativeVertexBuffer = vertexBuffer;
  observation.nativeIndexBuffer = indexBuffer;
  observation.priorIndexBaseVertex = priorIndexBase;
  if (after) {
    observation.ringBaseVertexAfter = ringBase;
    observation.ringNextVertexAfter = ringNext;
  } else {
    observation.submittedVertexCountBefore = submittedVertexCount;
    observation.ringBaseVertexBefore = ringBase;
    observation.ringNextVertexBefore = ringNext;
    observation.indexRingBaseBefore = indexRingBase;
    observation.indexRingNextBefore = indexRingNext;
    observation.nativeDeviceStateReadable = readable;
  }
  return readable;
}

// 生产旁路的第一道筛选只需要知道 skinMode 与输出格式。二者在 GX
// 状态中连续排列；先读这 8 字节可以避免把 0x6C4 字节完整状态快照
// 复制到每一个明显不会成为蒙皮候选的上传中。完整快照仍然只在
// skinMode/format 通过后执行，授权、毒账本和回退条件不因此放宽。
bool ReadNativeSkinFormatState(NativeUploadObservation& observation) {
  std::array<uint32_t, 2u> skinFormat = {};
  if (observation.gxDeviceD3d == 0u ||
      kGxSkinModeOffset >
          std::numeric_limits<uintptr_t>::max() -
              observation.gxDeviceD3d ||
      !dxvk::war3::SafeCopy(
          skinFormat.data(),
          reinterpret_cast<const void*>(observation.gxDeviceD3d +
                                        kGxSkinModeOffset),
          sizeof(skinFormat))) {
    observation.nativeDeviceStateReadable = false;
    return false;
  }
  observation.skinMode = skinFormat[0];
  observation.outputFormat = skinFormat[1];
  if (observation.outputFormat < kFormatFvf.size()) {
    observation.fvf = kFormatFvf[observation.outputFormat];
    observation.outputStride = kFormatStride[observation.outputFormat];
  } else {
    observation.fvf = 0u;
    observation.outputStride = 0u;
  }
  observation.nativeDeviceStateReadable = true;
  return true;
}

uint64_t BuildNativeScopePreflight(
    const NativeUploadObservation& observation, bool singlePassCaller) {
  uint64_t bits = 0u;
  if (g_fingerprint.exactMatch)
    bits |= NativePreflightExactFingerprint;
  if (observation.path == NativeDispatchPath::Common)
    bits |= NativePreflightCommonDispatch;
  if (observation.stage == 11)
    bits |= NativePreflightStage11;
  if (observation.batchTag == static_cast<int32_t>(kWorldObjectsTag))
    bits |= NativePreflightWorldObjectsTag;
  if (singlePassCaller)
    bits |= NativePreflightSinglePassCaller;
  return bits;
}

enum class NativePaletteReadabilityPolicy : uint8_t {
  MetadataRange,
  FaultSafeCopy,
};

uint64_t BuildNativePreflight(NativeUploadObservation& observation,
                               const NativeUploadCall& call,
                               bool singlePassCaller,
                               const NativeGeosetSnapshotState* geosetSnapshot,
                               NativePaletteReadabilityPolicy palettePolicy,
                               NativeT2SampleDurations* sampleDurations) {
  uint64_t bits = BuildNativeScopePreflight(observation, singlePassCaller);
  if (observation.skinMode == 1u)
    bits |= NativePreflightSkinMode1;
  if (observation.outputFormat < kFormatFvf.size())
    bits |= NativePreflightSupportedFormat;

  uintptr_t geosetPositions = 0;
  uintptr_t geosetNormals = 0;
  uintptr_t geosetGroupSlots = 0;
  uint32_t geosetVertexCount = 0;
  uint32_t geosetPaletteCount = 0;
  const int64_t geoHeaderStart = sampleDurations != nullptr
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  ReadGeosetSnapshotDword(
      geosetSnapshot, observation.geosetData,
      observation.epoch.uploadEpoch, kGeosetVertexCountOffset,
      geosetVertexCount);
  ReadGeosetSnapshotDword(
      geosetSnapshot, observation.geosetData,
      observation.epoch.uploadEpoch, kGeosetPaletteGroupCountOffset,
      geosetPaletteCount);
  ReadGeosetSnapshotPtr(
      geosetSnapshot, observation.geosetData,
      observation.epoch.uploadEpoch, kGeosetPositionsOffset,
      geosetPositions);
  ReadGeosetSnapshotPtr(
      geosetSnapshot, observation.geosetData,
      observation.epoch.uploadEpoch, kGeosetNormalsOffset, geosetNormals);
  ReadGeosetSnapshotPtr(
      geosetSnapshot, observation.geosetData,
      observation.epoch.uploadEpoch, kGeosetGroupSlotsOffset,
      geosetGroupSlots);
  if (sampleDurations != nullptr) {
    sampleDurations->geoHeaderTicks = SampleDurationTicks(
        geoHeaderStart, dxvk::high_resolution_clock::get_counter());
  }
  observation.geosetVertexCount = geosetVertexCount;

  if (call.vertexCount != 0u && call.vertexCount <= kMaxNativeVertices &&
      call.vertexCount == geosetVertexCount)
    bits |= NativePreflightVertexCountMatch;
  const int64_t positionProofStart = sampleDurations != nullptr
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  if (call.positions == geosetPositions &&
      IsReadableVertexRange(call.positions, call.vertexCount,
                            call.positionStride))
    bits |= NativePreflightPositionSourceMatch;
  if (sampleDurations != nullptr) {
    sampleDurations->positionProofTicks = SampleDurationTicks(
        positionProofStart, dxvk::high_resolution_clock::get_counter());
  }
  if (call.positionStride == 12u)
    bits |= NativePreflightPositionStride12;
  const int64_t normalProofStart = sampleDurations != nullptr
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  if (call.normals == geosetNormals &&
      IsReadableVertexRange(call.normals, call.vertexCount,
                            call.normalStride))
    bits |= NativePreflightNormalSourceMatch;
  if (sampleDurations != nullptr) {
    sampleDurations->normalProofTicks = SampleDurationTicks(
        normalProofStart, dxvk::high_resolution_clock::get_counter());
  }
  if (call.normalStride == 12u)
    bits |= NativePreflightNormalStride12;
  const int64_t groupProofStart = sampleDurations != nullptr
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  if (call.groupSlots == geosetGroupSlots &&
      IsReadableVertexRange(call.groupSlots, call.vertexCount,
                            call.groupSlotStride))
    bits |= NativePreflightGroupSlotsMatch;
  if (sampleDurations != nullptr) {
    sampleDurations->groupProofTicks = SampleDurationTicks(
        groupProofStart, dxvk::high_resolution_clock::get_counter());
  }
  if (call.groupSlotStride == 1u)
    bits |= NativePreflightGroupStride1;

  if (observation.palette != 0u)
    bits |= NativePreflightPalettePresent;
  if (observation.paletteGroupCount != 0u &&
      observation.paletteGroupCount <= kMaxPaletteGroups &&
      observation.paletteGroupCount == geosetPaletteCount)
    bits |= NativePreflightPaletteCountValid;
  const int64_t paletteProofStart = sampleDurations != nullptr
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  bool paletteReadable = false;
  if (observation.paletteGroupCount != 0u &&
      observation.paletteGroupCount <= kMaxPaletteGroups) {
    const size_t paletteBytes =
        static_cast<size_t>(observation.paletteGroupCount) *
        kNativePaletteGroupBytes;
    if (palettePolicy == NativePaletteReadabilityPolicy::FaultSafeCopy) {
      paletteReadable = dxvk::war3::SafeCopy(
          t_paletteReadabilityScratch.data(),
          reinterpret_cast<const void*>(observation.palette), paletteBytes);
    } else {
      paletteReadable = dxvk::war3::IsReadableRangeFast(
          reinterpret_cast<const void*>(observation.palette), paletteBytes);
    }
  }
  if (paletteReadable)
    bits |= NativePreflightPaletteReadable;
  if (sampleDurations != nullptr) {
    sampleDurations->paletteProofTicks = SampleDurationTicks(
        paletteProofStart, dxvk::high_resolution_clock::get_counter());
  }
  if (UvLayoutSupported(observation.outputFormat, call))
    bits |= NativePreflightUvLayoutSupported;
  return bits;
}


uint64_t NativeUploadByteCount(const NativeUploadObservation& observation) {
  return uint64_t(observation.vertexCount) * observation.outputStride;
}

bool ReadUploadGlobals(const NativeUploadObservation& observation) {
  if (g_gameBase == 0u)
    return false;

  const uint32_t expected[13] = {
      observation.vertexCount,
      uint32_t(observation.positions),
      observation.positionStride,
      uint32_t(observation.normals != 0u
                   ? observation.normals
                   : g_gameBase + kDefaultNormalRva),
      observation.normalStride,
      uint32_t(observation.extra != 0u
                   ? observation.extra
                   : g_gameBase + kDefaultExtraRva),
      observation.extraStride,
      uint32_t(observation.groupSlots),
      observation.groupSlotStride,
      uint32_t(observation.uv0 != 0u
                   ? observation.uv0
                   : g_gameBase + kDefaultUvRva),
      uint32_t(observation.uv1 != 0u
                   ? observation.uv1
                   : g_gameBase + kDefaultUvRva),
      observation.uv0Stride,
      observation.uv1Stride,
  };
  const uintptr_t globals = g_gameBase + kVertexGlobalsRva;
  std::array<uint32_t, std::size(expected)> actualSnapshot;
  static_assert(sizeof(actualSnapshot) == 13u * sizeof(uint32_t));
  if (dxvk::war3::SafeCopy(
          actualSnapshot.data(), reinterpret_cast<const void*>(globals),
          13u * sizeof(uint32_t))) {
    return std::memcmp(actualSnapshot.data(), expected, sizeof(expected)) == 0;
  }
  for (size_t i = 0u; i < std::size(expected); ++i) {
    uint32_t actual = 0u;
    if (!ReadFastU32(globals, i * sizeof(uint32_t), actual) ||
        actual != expected[i]) {
      return false;
    }
  }
  return true;
}

bool RefreshKernelState(NativeUploadObservation& observation,
                        uintptr_t gxDeviceD3d, void* mappedDst) {
  uint32_t format = 0u;
  uint32_t skinMode = 0u;
  uint32_t paletteCount = 0u;
  uint32_t submittedVertexCount = 0u;
  uint32_t ringBase = 0u;
  uint32_t ringNext = 0u;
  uint32_t indexBase = 0u;
  uint32_t indexNext = 0u;
  uint32_t priorIndexBase = 0u;
  uintptr_t palette = 0u;
  uintptr_t nativeDevice = 0u;
  uintptr_t vertexBuffer = 0u;
  uintptr_t indexBuffer = 0u;
  const size_t ringOffset = size_t(observation.outputFormat) * 8u;

  if (gxDeviceD3d == 0u || gxDeviceD3d != observation.gxDeviceD3d ||
      observation.outputFormat >= kFormatFvf.size()) {
    return false;
  }

  const GxDeviceStateReader state(gxDeviceD3d);
  if (!state.ReadU32(kGxOutputFormatOffset, format) ||
      !state.ReadU32(kGxSkinModeOffset, skinMode) ||
      !state.ReadU32(kGxPaletteCountOffset, paletteCount) ||
      !state.ReadU32(kGxSubmittedVertexCountOffset, submittedVertexCount) ||
      !state.ReadPtr(kGxPalettePtrOffset, palette) ||
      !state.ReadPtr(kGxNativeDeviceOffset, nativeDevice) ||
      !state.ReadPtr(kGxVertexBufferArrayOffset +
                         size_t(observation.outputFormat) * sizeof(uint32_t),
                     vertexBuffer) ||
      !state.ReadPtr(kGxIndexBufferOffset, indexBuffer) ||
      !state.ReadU32(kGxVertexRingBaseOffset + ringOffset, ringBase) ||
      !state.ReadU32(kGxVertexRingNextOffset + ringOffset, ringNext) ||
      !state.ReadU32(kGxIndexRingBaseOffset, indexBase) ||
      !state.ReadU32(kGxIndexRingNextOffset, indexNext) ||
      !state.ReadU32(kGxIndexBaseVertexOffset, priorIndexBase)) {
    return false;
  }

  const uint64_t expectedSubmitted =
      uint64_t(observation.submittedVertexCountBefore) +
      observation.vertexCount;
  const bool exact = mappedDst != nullptr &&
      format == observation.outputFormat && skinMode == observation.skinMode &&
      paletteCount == observation.paletteGroupCount &&
      palette == observation.palette && nativeDevice != 0u &&
      vertexBuffer != 0u && indexBuffer != 0u &&
      expectedSubmitted <= std::numeric_limits<uint32_t>::max() &&
      submittedVertexCount == uint32_t(expectedSubmitted) &&
      ringBase <= ringNext && ringNext <= kMaxNativeVertices &&
      ringNext - ringBase == observation.vertexCount &&
      indexBase == observation.indexRingBaseBefore &&
      indexNext == observation.indexRingNextBefore &&
      priorIndexBase == observation.priorIndexBaseVertex &&
      ReadUploadGlobals(observation);
  if (!exact)
    return false;

  observation.mappedDst = reinterpret_cast<uintptr_t>(mappedDst);
  observation.nativeD3DDevice = nativeDevice;
  observation.nativeVertexBuffer = vertexBuffer;
  observation.nativeIndexBuffer = indexBuffer;
  observation.ringBaseVertexAfter = ringBase;
  observation.ringNextVertexAfter = ringNext;
  observation.nativeDeviceStateReadable = true;
  observation.observedPreflight |=
      NativePreflightKernelMappedDestination |
      NativePreflightKernelStateExact |
      NativePreflightNativeBuffersInitialized |
      NativePreflightNativeRingReplayable |
      NativePreflightNativeRingReady;
  return true;
}

uint64_t BeginUploadInFlight(NativeUploadObservation& observation) {
  const bool nestedActive = t_state.nestedUploadDepth != 0u ||
      t_state.inFlightUpload.observation != nullptr ||
      t_state.productionFastRejectedUpload != nullptr;
  const bool evidenceSample =
      t_state.productionEvidenceUploadEpoch == observation.epoch.uploadEpoch;
  if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      observation.epoch.uploadEpoch == 0u ||
      observation.epoch.renderThreadId != ::GetCurrentThreadId() ||
      observation.gxDeviceD3d == 0u) {
    return 0u;
  }
  // This is the same no-InFlight contract the generic production reject used
  // before the optimization. Retain only a stack-lifetime TLS identity so the
  // sole 0x0EEB85 kernel detour can avoid treating the expected missing
  // observation as an authorization failure. The 1/127 evidence sample and
  // dispatch-owned rejects keep the pre-optimization generic route; nested
  // calls and every poison-retirement proof keep the full in-flight machine.
  if (observation.productionFastRejected &&
      !observation.poisonRetirementOnly && !nestedActive) {
    if (observation.mode == GpuSkinMode::Bypass &&
        !g_runtimeConfig.fullDiagnostics &&
        observation.epoch.dispatchEpoch == 0u && !evidenceSample) {
      const bool globalTransactionPinned =
          t_state.flushTransactionDepth == 0u;
      if (globalTransactionPinned) {
        // Publish before the report-only TLS sentinel, then recheck the
        // shutdown admission gate authoritatively. A drain that won the race
        // observes either no TLS mutation or this independent upload pin.
        g_activeUploadTransactions.fetch_add(
            1u, std::memory_order_seq_cst);
        if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
            !CurrentThreadIsObservedRenderThread() ||
            observation.epoch.renderThreadId != ::GetCurrentThreadId() ||
            observation.epoch.uploadEpoch == 0u ||
            observation.gxDeviceD3d == 0u) {
          g_activeUploadTransactions.fetch_sub(
              1u, std::memory_order_seq_cst);
          ProcessPendingBridgeReset();
          return 0u;
        }
      }
      t_state.productionFastRejectedUpload = &observation;
      t_state.productionFastRejectedGlobalTransactionPinned =
          globalTransactionPinned;
      return kProductionFastRejectUploadCookie;
    }
    // Preserve the pre-optimization no-InFlight behavior for full diagnostics,
    // dispatch-owned rejects and the exact 1/127 generic evidence cohort.
    return 0u;
  }
  const bool globalTransactionPinned = !nestedActive &&
      t_state.flushTransactionDepth == 0u;
  if (nestedActive || globalTransactionPinned) {
    // Outside a flush, publish the pin before touching any transaction-owned
    // TLS. Nested uploads retain their historical independent pin even when a
    // flush exists because their shared sentinel cookie has no per-level flag.
    g_activeUploadTransactions.fetch_add(1u, std::memory_order_seq_cst);
  }
  if (globalTransactionPinned &&
      (!g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
       !CurrentThreadIsObservedRenderThread() ||
       observation.epoch.uploadEpoch == 0u ||
       observation.epoch.renderThreadId != ::GetCurrentThreadId() ||
       observation.gxDeviceD3d == 0u)) {
    g_activeUploadTransactions.fetch_sub(1u, std::memory_order_seq_cst);
    ProcessPendingBridgeReset();
    return 0u;
  }
  if (nestedActive) {
    // Nested uploads are an exceptional fail-closed path. Keep their original
    // independent pin so the constant nested cookie remains self-contained.
    DispatchFrame* dispatch = FindDispatchFrameByEpoch(
        observation.epoch.dispatchEpoch);
    if (dispatch != nullptr) {
      InvalidateDispatchCpuOnlySeal(*dispatch);
      PromoteDeferredManagerDispatchForHazard(*dispatch);
    }
    NativeUploadObservation* parent = t_state.inFlightUpload.observation;
    if (parent != nullptr) {
      if (!parent->scopeFailClosed) {
        parent->scopeFailClosed = true;
        g_counters.scopeFailClosedUploads.fetch_add(
            1u, std::memory_order_relaxed);
      }
      if (parent->cpuSkinKernelBypassed) {
        parent->bypassFailure = NativeBypassFailureReason::PostSkipMismatch;
        parent->postSkipMismatchMask |= 1u << 5;
      }
    }
    t_state.inFlightUpload.conflicted = true;
    if (!observation.scopeFailClosed) {
      observation.scopeFailClosed = true;
      g_counters.scopeFailClosedUploads.fetch_add(
          1u, std::memory_order_relaxed);
    }
    if (t_state.nestedUploadDepth <
        t_state.nestedUploadObservations.size()) {
      t_state.nestedUploadObservations[t_state.nestedUploadDepth] =
          &observation;
    }
    ++t_state.nestedUploadDepth;
    g_counters.nestedUploadScopes.fetch_add(1u,
                                             std::memory_order_relaxed);
    return kNestedUploadCookie;
  }

  RecycleIndexSnapshotStorage(t_state.inFlightUpload.indexSnapshot);
  t_state.inFlightUpload = {};
  t_state.inFlightUpload.observation = &observation;
  t_state.inFlightUpload.renderThreadId = observation.epoch.renderThreadId;
  t_state.inFlightUpload.uploadEpoch = observation.epoch.uploadEpoch;
  t_state.inFlightUpload.gxDeviceD3d = observation.gxDeviceD3d;
  if (t_state.pendingGeosetSnapshot.geosetData ==
          observation.geosetData &&
      t_state.pendingGeosetSnapshot.uploadEpoch ==
          observation.epoch.uploadEpoch) {
    t_state.inFlightUpload.geosetSnapshot =
        t_state.pendingGeosetSnapshot;
  }
  t_state.inFlightUpload.callbacksAdmitted =
      g_callbackIngressEnabled.load(std::memory_order_acquire) ||
      (t_state.dispatchDepth != 0u &&
       t_state.dispatchFrames[t_state.dispatchDepth - 1u].callbacksAdmitted);
  t_state.inFlightUpload.globalTransactionPinned = globalTransactionPinned;
  observation.observedPreflight |= NativePreflightOuterUploadInFlight;
  return observation.epoch.uploadEpoch;
}

void EndUploadInFlight(NativeUploadObservation* observation,
                       uint64_t cookie) {
  bool evidenceCleared = false;
  if (t_state.productionEvidenceUploadEpoch != 0u &&
      observation != nullptr &&
      t_state.productionEvidenceUploadEpoch ==
          observation->epoch.uploadEpoch) {
    t_state.productionEvidenceUploadEpoch = 0u;
    evidenceCleared = true;
  }
  if (cookie == kProductionFastRejectUploadCookie) {
    if (t_state.productionFastRejectedUpload == observation) {
      const bool globalTransactionPinned =
          t_state.productionFastRejectedGlobalTransactionPinned;
      t_state.productionFastRejectedUpload = nullptr;
      t_state.productionFastRejectedGlobalTransactionPinned = false;
      if (globalTransactionPinned) {
        g_activeUploadTransactions.fetch_sub(
            1u, std::memory_order_seq_cst);
      }
      ProcessPendingBridgeReset();
    }
    if (evidenceCleared)
      ProcessPendingBridgeReset();
    return;
  }
  if (cookie == kNestedUploadCookie) {
    if (t_state.nestedUploadDepth == 0u) {
      // A duplicate/stale shared sentinel must never underflow the process-wide
      // pin. Surface the fail-closed invariant breach through the existing hard
      // zero state-mismatch diagnostic.
      g_counters.bypassStateMismatches.fetch_add(
          1u, std::memory_order_relaxed);
      if (evidenceCleared)
        ProcessPendingBridgeReset();
      return;
    }
    --t_state.nestedUploadDepth;
    if (t_state.nestedUploadDepth <
        t_state.nestedUploadObservations.size()) {
      t_state.nestedUploadObservations[t_state.nestedUploadDepth] = nullptr;
    }
    g_activeUploadTransactions.fetch_sub(1u, std::memory_order_seq_cst);
    ProcessPendingBridgeReset();
    return;
  }
  if (observation == nullptr || cookie == 0u ||
      t_state.inFlightUpload.observation != observation ||
      t_state.inFlightUpload.uploadEpoch != cookie) {
    if (evidenceCleared)
      ProcessPendingBridgeReset();
    return;
  }
  const bool globalTransactionPinned =
      t_state.inFlightUpload.globalTransactionPinned;
  RecycleIndexSnapshotStorage(t_state.inFlightUpload.indexSnapshot);
  t_state.inFlightUpload = {};
  if (t_state.pendingGeosetSnapshot.uploadEpoch == cookie)
    t_state.pendingGeosetSnapshot = {};
  if (globalTransactionPinned) {
    g_activeUploadTransactions.fetch_sub(1u, std::memory_order_seq_cst);
  }
  ProcessPendingBridgeReset();
}

void DecrementPendingKernelAuthorization() {
  uint64_t current =
      g_counters.pendingKernelAuthorizations.load(std::memory_order_relaxed);
  while (current != 0u &&
         !g_counters.pendingKernelAuthorizations.compare_exchange_weak(
             current, current - 1u, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

void PublishCompletedUpload(NativeUploadObservation& observation) {
  if (observation.completionPublished)
    return;
  observation.completionPublished = true;
  g_counters.uploads.fetch_add(1u, std::memory_order_relaxed);
  const uint64_t byteCount = NativeUploadByteCount(observation);
  if (observation.originalUploadExecuted) {
    g_counters.originalUploadCalls.fetch_add(1u, std::memory_order_relaxed);
    g_counters.originalUploadBytes.fetch_add(
        byteCount, std::memory_order_relaxed);
  }
  if (observation.nativeObservationEligible) {
    g_counters.nativeEligibleUploads.fetch_add(1u,
                                                std::memory_order_relaxed);
  }
  const uint64_t missingInputPreflight =
      kNativeUploadInputRequiredPreflight & ~observation.observedPreflight;
  if (missingInputPreflight != 0u) {
    g_counters.inputPreflightRejectedUploads.fetch_add(
        1u, std::memory_order_relaxed);
    if (g_runtimeConfig.fullDiagnostics) {
      for (size_t i = 0; i < kNativeUploadInputPreflightBitCount; i++) {
        if ((missingInputPreflight & (uint64_t{1} << i)) != 0u) {
          g_counters.inputPreflightMissing[i].fetch_add(
              1u, std::memory_order_relaxed);
        }
      }
    }
  }
  const size_t formatBucket = observation.nativeDeviceStateReadable &&
          observation.outputFormat < 7u
      ? observation.outputFormat : 7u;
  const size_t skinBucket = observation.nativeDeviceStateReadable &&
          observation.skinMode < 7u
      ? observation.skinMode : 7u;
  if (observation.productionFastRejected &&
      !observation.nativeDeviceStateReadable) {
    g_counters.productionFastRejectUnknownState.fetch_add(
        1u, std::memory_order_relaxed);
  }
  g_counters.formatHistogram[formatBucket].fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.skinModeHistogram[skinBucket].fetch_add(
      1u, std::memory_order_relaxed);

  DispatchFrame* dispatch = CurrentDispatch();
  bool outsideDispatch = false;
  if (dispatch != nullptr &&
      dispatch->observation.epoch.dispatchEpoch ==
          observation.epoch.dispatchEpoch) {
    if (observation.outsideAdmissionPopulation.populationClass ==
        NativeOutsideUploadPopulationClass::Tracked) {
      RecordNativeOutsideAdmissionTrackedResolvedInside();
    }
    ActiveUploadState& active = dispatch->activeUpload;
    RecycleIndexSnapshotStorage(active.indexSnapshot);
    active = {};
    active.observation = observation;
    if (t_state.inFlightUpload.observation == &observation &&
        t_state.inFlightUpload.uploadEpoch == observation.epoch.uploadEpoch &&
        t_state.inFlightUpload.indexSnapshot.uploadEpoch ==
            observation.epoch.uploadEpoch) {
      active.indexSnapshot =
          std::move(t_state.inFlightUpload.indexSnapshot);
    }
    active.dipCount = 0u;
    active.valid = true;
  } else {
    outsideDispatch = true;
    if (observation.outsideAdmissionPopulation.populationClass ==
        NativeOutsideUploadPopulationClass::Untracked) {
      // Publish the cross-population terminal (and therefore the telemetry
      // pending bit) before the historical direct atomic outside counter. An
      // off-thread relaxed snapshot can observe an incomplete pair only while
      // snapshotAvailable is authoritatively false.
      RecordNativeOutsideAdmissionUntrackedResolvedOutside();
    }
    g_counters.uploadsOutsideDispatch.fetch_add(1u,
                                                 std::memory_order_relaxed);
  }

  // In production bypass, a scope-less fast reject can never participate in
  // dispatch pairing, DIP resolution, layout learning, or poison ownership.
  // Bridge-local counters remain authoritative, so avoid two manager callback
  // pins and six manager mutex acquisitions for this dominant (~80%) class.
  const bool bridgeOnlyOutsideReject = outsideDispatch &&
      observation.epoch.dispatchEpoch == 0u &&
      observation.mode == GpuSkinMode::Bypass &&
      observation.productionFastRejected &&
      !observation.cpuSkinKernelBypassed;
  if (bridgeOnlyOutsideReject) {
    g_counters.productionOutsideCallbacksSkipped.fetch_add(
        1u, std::memory_order_relaxed);
  }
  const bool managerUploadAllowed = !bridgeOnlyOutsideReject &&
      ManagerCallbackAllowedForEpoch(
          observation.epoch.dispatchEpoch,
          ManagerSkippedCallbackKind::Upload);
  if (managerUploadAllowed) {
    NativeBridgeCallbackPin callbackPin(
        NativeBridgeCallbackKind::Upload,
        observation.epoch.uploadEpoch);
    const NativeBridgeCallbacks* callbacks = callbackPin.get();
    if (callbacks != nullptr && callbacks->onUpload != nullptr) {
      callbackPin.BeginCallbackBody();
      callbacks->onUpload(callbacks->userData, observation);
      callbackPin.EndCallbackBody();
    }
  }
  if (outsideDispatch)
    PublishUploadFanout(observation, 0u, !bridgeOnlyOutsideReject);
}

uint64_t BeginDispatchScope(NativeDispatchPath path,
                            uintptr_t sceneNode,
                            uintptr_t renderablePart,
                            uint32_t layerIndex,
                            int32_t stage,
                            int32_t batchTag,
                            bool forceFailClosed) {
  ProcessPendingBridgeReset();
  if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst))
    return 0;
  const bool globalTransactionPinned =
      t_state.flushTransactionDepth == 0u;
  if (globalTransactionPinned) {
    // Publish before any frame/TLS mutation. A concurrent reset or hook drain
    // can then use either this pin or the already-live outer flush pin.
    g_activeDispatchTransactions.fetch_add(1u, std::memory_order_seq_cst);
    if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
        !CurrentThreadIsObservedRenderThread()) {
      g_activeDispatchTransactions.fetch_sub(
          1u, std::memory_order_seq_cst);
      ProcessPendingBridgeReset();
      return 0u;
    }
  }
  if (forceFailClosed) {
    g_counters.scopeForced.fetch_add(1u, std::memory_order_relaxed);
  }
  if (t_state.dispatchDepth >= t_state.dispatchFrames.size()) {
    // Overflow uses a shared sentinel cookie, so retain its historical
    // independent pin instead of adding another side stack merely to optimize
    // an already-failed exceptional path.
    if (!globalTransactionPinned) {
      g_activeDispatchTransactions.fetch_add(1u, std::memory_order_seq_cst);
    }
    DispatchFrame& outer =
        t_state.dispatchFrames[t_state.dispatchDepth - 1u];
    InvalidateDispatchCpuOnlySeal(outer);
    PromoteDeferredManagerDispatchForHazard(outer);
    PoisonActiveBypassForScopeHazard(outer);
    g_counters.dispatchStackOverflow.fetch_add(1u, std::memory_order_relaxed);
    ++t_state.dispatchOverflowDepth;
    g_nativeGpuSkinB1BorrowedFlushPinPassThrough = false;
    return kDispatchOverflowCookie;
  }

  const bool nested = t_state.dispatchDepth != 0u;
  if (nested) {
    DispatchFrame& outer =
        t_state.dispatchFrames[t_state.dispatchDepth - 1u];
    InvalidateDispatchCpuOnlySeal(outer);
    PromoteDeferredManagerDispatchForHazard(outer);
    PoisonActiveBypassForScopeHazard(outer);
    g_counters.nestedDispatchScopes.fetch_add(1u,
                                               std::memory_order_relaxed);
  }
  DispatchFrame& frame = t_state.dispatchFrames[t_state.dispatchDepth++];
  frame = {};
  frame.previousB1BorrowedFlushPinPassThrough =
      g_nativeGpuSkinB1BorrowedFlushPinPassThrough;
  g_nativeGpuSkinB1BorrowedFlushPinPassThrough = false;
  frame.globalTransactionPinned = globalTransactionPinned;
  frame.observation.epoch.renderThreadId = ::GetCurrentThreadId();
  frame.observation.epoch.flushEpoch = t_state.flushEpoch;
  frame.observation.epoch.dispatchEpoch =
      g_nextDispatchEpoch.fetch_add(1u, std::memory_order_relaxed) + 1u;
  frame.observation.path = path;
  frame.observation.sceneNode = sceneNode;
  frame.observation.renderablePart = renderablePart;
  frame.observation.layerIndex = layerIndex;
  frame.observation.nestingDepth =
      static_cast<uint32_t>(t_state.dispatchDepth - 1u);
  frame.observation.stage = stage;
  frame.observation.batchTag = batchTag;
  frame.observation.failClosed = nested || forceFailClosed ||
      g_resetRequestedGeneration.load(std::memory_order_acquire) !=
          g_resetCompletedGeneration.load(std::memory_order_acquire);
  frame.callbacksAdmitted =
      g_callbackIngressEnabled.load(std::memory_order_acquire);

  AddNativeTelemetryCounter(
      g_counters.dispatchScopes, t_nativeTelemetryDelta.dispatchScopes);
  if (path == NativeDispatchPath::Common) {
    AddNativeTelemetryCounter(
        g_counters.commonDispatchScopes,
        t_nativeTelemetryDelta.commonDispatchScopes);
  } else if (path == NativeDispatchPath::Special) {
    AddNativeTelemetryCounter(
        g_counters.specialDispatchScopes,
        t_nativeTelemetryDelta.specialDispatchScopes);
  }

  const bool productionLightBypass =
      g_runtimeConfig.mode == GpuSkinMode::Bypass &&
      !g_runtimeConfig.fullDiagnostics;
  const bool evidenceEager = productionLightBypass &&
      (frame.observation.epoch.dispatchEpoch %
       kNativeBeginTimingSamplePeriod) == 0u;
  const bool localSealEligible = productionLightBypass && !evidenceEager &&
      path == NativeDispatchPath::Common &&
      frame.observation.nestingDepth == 0u &&
      !frame.observation.failClosed;
  if (localSealEligible &&
      TryCommitNativeDispatchCpuOnlySealFromLocalView(frame)) {
    frame.b1BorrowedFlushPinPassThrough = true;
    g_nativeGpuSkinB1BorrowedFlushPinPassThrough = true;
    return frame.observation.epoch.dispatchEpoch;
  }
  const bool lazySafetyClean = productionLightBypass && !evidenceEager &&
      ManagerLazyDispatchSafetyClean(nested, forceFailClosed);
  if (lazySafetyClean && path == NativeDispatchPath::Special) {
    frame.managerState = ManagerDispatchState::NeverSpecial;
    AddNativeTelemetryCounter(
        g_counters.managerDispatchNeverScopes,
        t_nativeTelemetryDelta.managerDispatchNeverScopes);
  } else {
    // Evidence, unsafe or locally non-authoritative/candidate-positive Common
    // scopes preserve the original manager begin. A committed exact-negative
    // local view returned above and never enters this ABI-9 lifetime.
    frame.managerState = ManagerDispatchState::EagerPending;
    AddNativeTelemetryCounter(
        g_counters.managerDispatchEagerScopes,
        t_nativeTelemetryDelta.managerDispatchEagerScopes);
    if (evidenceEager) {
      AddNativeTelemetryCounter(
          g_counters.managerDispatchEvidenceEagerScopes,
          t_nativeTelemetryDelta.managerDispatchEvidenceEagerScopes);
    }
    IssueManagerDispatchBegin(frame, false);
  }
  return frame.observation.epoch.dispatchEpoch;
}

void EndDispatchScope(uint64_t cookie) {
  if (cookie == kDispatchOverflowCookie) {
    if (t_state.dispatchOverflowDepth != 0u) {
      --t_state.dispatchOverflowDepth;
      if (t_state.dispatchOverflowDepth == 0u &&
          t_state.dispatchDepth != 0u) {
        g_nativeGpuSkinB1BorrowedFlushPinPassThrough =
            t_state.dispatchFrames[t_state.dispatchDepth - 1u]
                .b1BorrowedFlushPinPassThrough;
      }
      g_activeDispatchTransactions.fetch_sub(
          1u, std::memory_order_seq_cst);
      ProcessPendingBridgeReset();
    }
    return;
  }
  if (cookie == 0u || t_state.dispatchDepth == 0u)
    return;
  DispatchFrame& frame = t_state.dispatchFrames[t_state.dispatchDepth - 1u];
  if (frame.observation.epoch.dispatchEpoch != cookie)
    return;

  // A cross-thread reset may arrive after the final DIP. Mark/fuse the active
  // bypass before fan-out and dispatch-end callbacks consume this frame.
  ApplyPendingResetFailClosed();
  if (frame.cpuOnlySeal.committed &&
      !DispatchCpuOnlySealBaseSafetyExact(
          frame, 0u, false, false)) {
    InvalidateDispatchCpuOnlySeal(frame);
  }
  FinalizeActiveUpload(frame);
  FinalizeDispatchCpuOnlyActiveUpload(frame);

  if ((frame.managerState == ManagerDispatchState::PendingCommon ||
       frame.managerState == ManagerDispatchState::NeverSpecial) &&
      !ManagerLazyDispatchSafetyClean(
          frame.observation.nestingDepth != 0u,
          frame.observation.failClosed)) {
    PromoteDeferredManagerDispatchForHazard(frame);
  }

  NativeDispatchSummary summary = {};
  summary.dispatch = frame.observation;
  summary.semanticScopeCount = frame.semanticScopeCount;
  summary.uploadCount = frame.uploadCount;
  summary.dipCount = frame.dipCount;

  if (frame.managerState == ManagerDispatchState::BeginIssued) {
    NativeBridgeCallbackPin callbackPin(
        NativeBridgeCallbackKind::DispatchEnd,
        summary.dispatch.epoch.dispatchEpoch);
    const NativeBridgeCallbacks* callbacks = callbackPin.get();
    if (callbacks != nullptr && callbacks->onDispatchEnd != nullptr) {
      callbackPin.BeginCallbackBody();
      callbacks->onDispatchEnd(callbacks->userData, summary);
      callbackPin.EndCallbackBody();
      AddNativeTelemetryCounter(
          g_counters.managerDispatchEndCallbacks,
          t_nativeTelemetryDelta.managerDispatchEndCallbacks);
      AddNativeTelemetryCounter(
          g_counters.managerDispatchIssuedEnds,
          t_nativeTelemetryDelta.managerDispatchIssuedEnds);
    } else {
      frame.managerState = ManagerDispatchState::FailedClosed;
      frame.observation.failClosed = true;
      g_counters.managerDispatchFailedEnds.fetch_add(
          1u, std::memory_order_relaxed);
    }
  } else if (frame.managerState == ManagerDispatchState::NativeCpuOnly) {
    // The fixed TLS exact-negative view owns this physical scope. It has no
    // manager begin and therefore must never manufacture an ABI-9 end.
    AddNativeTelemetryCounter(
        g_counters.managerDispatchNativeCpuOnlyEnds,
        t_nativeTelemetryDelta.managerDispatchNativeCpuOnlyEnds);
  } else if (frame.managerState == ManagerDispatchState::PendingCommon) {
    // A Common scope with no upload has no ABI-9 manager lifetime at all.
    // DIPs are allowed in this class: they were deliberately suppressed from
    // the manager while the physical native ordinal remained observable.  An
    // upload, however, must have admitted the lazy begin before its ordinal was
    // allocated, so seeing one here is a bridge bug that ABI 9 cannot replay.
    if (frame.uploadCount == 0u) {
      AddNativeTelemetryCounter(
          g_counters.managerDispatchNoUploadEnds,
          t_nativeTelemetryDelta.managerDispatchNoUploadEnds);
    } else {
      IssueManagerDispatchBegin(frame, true);
      g_counters.managerDispatchFailedEnds.fetch_add(
          1u, std::memory_order_relaxed);
    }
  } else if (frame.managerState == ManagerDispatchState::NeverSpecial) {
    AddNativeTelemetryCounter(
        g_counters.managerDispatchNeverEnds,
        t_nativeTelemetryDelta.managerDispatchNeverEnds);
  } else {
    if (frame.managerState == ManagerDispatchState::EagerPending)
      FailManagerDispatchAdmission(frame);
    g_counters.managerDispatchFailedEnds.fetch_add(
        1u, std::memory_order_relaxed);
  }

  AddNativeTelemetryCounter(
      g_counters.dispatchScopeEnds,
      t_nativeTelemetryDelta.dispatchScopeEnds);
  if (frame.cpuOnlySeal.committedEver) {
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealScopeEnds,
        t_nativeTelemetryDelta.dispatchCpuOnlySealScopeEnds);
  }
  const bool globalTransactionPinned = frame.globalTransactionPinned;
  const bool previousB1BorrowedFlushPinPassThrough =
      frame.previousB1BorrowedFlushPinPassThrough;
  frame = {};
  --t_state.dispatchDepth;
  g_nativeGpuSkinB1BorrowedFlushPinPassThrough =
      previousB1BorrowedFlushPinPassThrough;
  if (globalTransactionPinned) {
    g_activeDispatchTransactions.fetch_sub(1u, std::memory_order_seq_cst);
  }
  ProcessPendingBridgeReset();
}

uint64_t BeginSemanticScope(uintptr_t geosetData,
                            uintptr_t layerDispatch,
                            uintptr_t layerState,
                            uintptr_t callerAddress) {
  ProcessPendingBridgeReset();
  if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst))
    return 0;
  DispatchFrame* dispatch = CurrentDispatch();
  // VS-B1 的语义帧总是在已有 dispatch 帧内消费；借用其生命周期 pin，
  // 避免每个 draw 再做一对 seq_cst 原子和一次 reset 扫描。无 dispatch
  // 的异常语义帧仍保留独立 pin，不能扩大安全假设。
  const bool borrowDispatchTransaction =
      g_runtimeConfig.executionRoute ==
          GpuSkinExecutionRoute::VertexShaderBypass &&
      dispatch != nullptr;
  const bool globalTransactionPinned =
      t_state.flushTransactionDepth == 0u && !borrowDispatchTransaction;
  if (globalTransactionPinned) {
    // Match dispatch admission: publish before semantic classification or any
    // TLS mutation when no enclosing flush already owns quiescence.
    g_activeSemanticTransactions.fetch_add(1u, std::memory_order_seq_cst);
    if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
        !CurrentThreadIsObservedRenderThread()) {
      g_activeSemanticTransactions.fetch_sub(
          1u, std::memory_order_seq_cst);
      ProcessPendingBridgeReset();
      return 0u;
    }
  }
  if (t_state.semanticDepth >= t_state.semanticFrames.size()) {
    // As with dispatch overflow, preserve a standalone pin for the sentinel
    // route. Normal scopes below can reuse the enclosing flush pin.
    if (!globalTransactionPinned) {
      g_activeSemanticTransactions.fetch_add(1u, std::memory_order_seq_cst);
    }
    if (t_state.dispatchDepth != 0u) {
      DispatchFrame& outer =
          t_state.dispatchFrames[t_state.dispatchDepth - 1u];
      InvalidateDispatchCpuOnlySeal(outer);
      PromoteDeferredManagerDispatchForHazard(outer);
      PoisonActiveBypassForScopeHazard(outer);
    }
    g_counters.semanticStackOverflow.fetch_add(1u, std::memory_order_relaxed);
    ++t_state.semanticOverflowDepth;
    return kSemanticOverflowCookie;
  }

  const bool nested = t_state.semanticDepth != 0u;
  if (nested) {
    if (dispatch != nullptr) {
      InvalidateDispatchCpuOnlySeal(*dispatch);
      PromoteDeferredManagerDispatchForHazard(*dispatch);
      PoisonActiveBypassForScopeHazard(*dispatch);
    }
    g_counters.nestedSemanticScopes.fetch_add(1u,
                                               std::memory_order_relaxed);
  }
  const NativeDispatchPath dispatchPath = dispatch != nullptr
      ? dispatch->observation.path
      : NativeDispatchPath::Unknown;
  bool singlePassCaller = false;
  const NativeDispatchPath semanticPath =
      ClassifySemanticPath(callerAddress, dispatchPath, singlePassCaller);

  SemanticFrame& frame = t_state.semanticFrames[t_state.semanticDepth++];
  frame = {};
  frame.globalTransactionPinned = globalTransactionPinned;
  frame.geosetData = geosetData;
  frame.layerDispatch = layerDispatch;
  frame.layerState = layerState;
  frame.callerAddress = callerAddress;
  frame.path = semanticPath;
  frame.singlePassCaller = singlePassCaller;
  frame.failClosed = nested ||
      (dispatch != nullptr && dispatch->observation.failClosed);
  frame.dispatchEpoch = dispatch != nullptr
      ? dispatch->observation.epoch.dispatchEpoch
      : 0u;
  frame.cookie =
      g_nextSemanticCookie.fetch_add(1u, std::memory_order_relaxed) + 1u;
  if (dispatch != nullptr)
    ++dispatch->semanticScopeCount;
  AddNativeTelemetryCounter(
      g_counters.semanticScopes, t_nativeTelemetryDelta.semanticScopes);
  return frame.cookie;
}

void EndSemanticScope(uint64_t cookie) {
  if (cookie == kSemanticOverflowCookie) {
    if (t_state.semanticOverflowDepth != 0u) {
      --t_state.semanticOverflowDepth;
      g_activeSemanticTransactions.fetch_sub(
          1u, std::memory_order_seq_cst);
      ProcessPendingBridgeReset();
    }
    return;
  }
  if (cookie == 0u || t_state.semanticDepth == 0u)
    return;
  SemanticFrame& frame = t_state.semanticFrames[t_state.semanticDepth - 1u];
  if (frame.cookie != cookie)
    return;
  const bool globalTransactionPinned = frame.globalTransactionPinned;
  frame = {};
  --t_state.semanticDepth;
  if (globalTransactionPinned) {
    g_activeSemanticTransactions.fetch_sub(1u, std::memory_order_seq_cst);
  }
  ProcessPendingBridgeReset();
}

}  // namespace

bool NativeGpuSkinB1UnobservedNativePathSafe() noexcept {
  return NativeGpuSkinB1UnobservedNativePathSafeInternal();
}

bool NativeGpuSkinB1DispatchKnownNegative(
    NativeDispatchPath path, uintptr_t renderablePart,
    uint32_t layerIndex) noexcept {
  return NativeGpuSkinB1DispatchKnownNegativeInternal(
      path, renderablePart, layerIndex);
}

bool RetireNativeBypassPoisonAfterExactConsumer(
    const NativeUploadObservation& observation,
    bool exactSingleDip, bool consumerSettlementExact) noexcept {
  return RetireNativeBypassPoisonAfterExactConsumerInternal(
      observation, exactSingleDip, consumerSettlementExact);
}

bool NativeGpuSkinDispatchObservationRequired(
    NativeDispatchPath path, uintptr_t renderablePart,
    uint32_t layerIndex) noexcept {
  return NativeGpuSkinDispatchObservationRequiredInternal(
      path, renderablePart, layerIndex);
}

bool NativeGpuSkinSemanticObservationRequired() noexcept {
  return NativeGpuSkinSemanticObservationRequiredInternal();
}

bool PublishNativeDispatchCpuOnlySealView(
    uint64_t flushEpoch,
    const NativeDispatchCpuOnlySealCandidate* candidates,
    uint32_t count) noexcept {
  g_counters.dispatchCpuOnlySealLocalViewPublishAttempts.fetch_add(
      1u, std::memory_order_relaxed);
  InvalidateNativeDispatchCpuOnlySealView();
  const auto reject = []() noexcept {
    InvalidateNativeDispatchCpuOnlySealView();
    g_counters.dispatchCpuOnlySealLocalViewRejects.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  };

  if (count > kNativeDispatchCpuOnlySealViewCapacity ||
      (count != 0u && candidates == nullptr)) {
    return reject();
  }
  const uint64_t resetGeneration =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  if (!NativeDispatchCpuOnlySealViewPublicationSafetyExact(
          flushEpoch, resetGeneration)) {
    return reject();
  }

  NativeDispatchCpuOnlySealView& view =
      t_state.dispatchCpuOnlySealView;
  for (uint32_t i = 0u; i < count; ++i) {
    const NativeDispatchCpuOnlySealCandidate candidate = candidates[i];
    if (candidate.renderablePart == 0u ||
        (i != 0u && !NativeDispatchCpuOnlySealCandidateLess(
            candidates[i - 1u], candidate))) {
      return reject();
    }
    view.candidates[i] = candidate;
  }

  // The manager owns the source for the synchronous call, but re-read it after
  // the fixed copy so pointer reuse, mutation or an accidentally unsorted input
  // cannot be promoted into TLS authority. Revalidate lifecycle publications
  // after that read and publish flushEpoch only as the final write.
  for (uint32_t i = 0u; i < count; ++i) {
    if (!SameNativeDispatchCpuOnlySealCandidate(
            candidates[i], view.candidates[i]) ||
        (i != 0u && !NativeDispatchCpuOnlySealCandidateLess(
            view.candidates[i - 1u], view.candidates[i]))) {
      return reject();
    }
  }
  if (!NativeDispatchCpuOnlySealViewPublicationSafetyExact(
          flushEpoch, resetGeneration)) {
    return reject();
  }

  view.resetGeneration = resetGeneration;
  view.count = count;
  view.flushEpoch = flushEpoch;
  g_counters.dispatchCpuOnlySealLocalViewPublishes.fetch_add(
      1u, std::memory_order_relaxed);
  return true;
}

void InvalidateNativeDispatchCpuOnlySealViewForCallbackException() noexcept {
  InvalidateNativeDispatchCpuOnlySealView();
}

bool ProposeCurrentNativeDispatchCpuOnlySeal(
    const NativeDispatchObservation& observation) noexcept {
  AddNativeTelemetryCounter(
      g_counters.dispatchCpuOnlySealProposals,
      t_nativeTelemetryDelta.dispatchCpuOnlySealProposals);
  const auto reject = []() noexcept {
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealProposalRejected,
        t_nativeTelemetryDelta.dispatchCpuOnlySealProposalRejected);
    return false;
  };

  DispatchFrame* frame = CurrentDispatch();
  if (frame == nullptr ||
      frame->managerState != ManagerDispatchState::EagerPending ||
      frame->cpuOnlySeal.proposed || frame->cpuOnlySeal.committed ||
      frame->cpuOnlySeal.committedEver || frame->uploadCount != 0u ||
      frame->dipCount != 0u || frame->activeUpload.valid ||
      frame->cpuOnlyActiveUpload.valid ||
      !SameNativeDispatchObservationExact(
          frame->observation, observation)) {
    return reject();
  }

  frame->cpuOnlySeal.resetGeneration =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  if (!DispatchCpuOnlySealBaseSafetyExact(
          *frame, 1u, false, false)) {
    frame->cpuOnlySeal = {};
    return reject();
  }

  frame->cpuOnlySeal.proposed = true;
  AddNativeTelemetryCounter(
      g_counters.dispatchCpuOnlySealProposalAccepted,
      t_nativeTelemetryDelta.dispatchCpuOnlySealProposalAccepted);
  return true;
}

NativeDipObserverScope::NativeDipObserverScope() noexcept {
  // BeginNativeFlushTransaction already proved the observed render thread and
  // published a process-wide owner before incrementing this TLS depth. That
  // owner prevents reset from clearing the TLS state until the enclosing flush
  // ends, so a nested device DIP borrows it directly. Re-running the reset and
  // thread admission here cannot make the cover stronger; it only repeats
  // cross-thread loads for every physical draw.
  if (t_state.flushTransactionDepth != 0u) {
    // A flush-borrowed re-entrant DIP still nests an already-live independent
    // observer. Revoke that outer stack token permanently just like the
    // explicit coverKind=2 path below; never let a hybrid cover reuse it.
    if (t_dipObserverDepth != 0u)
      t_dipObserverCpuOnlySealCookie = 0u;
    m_coverKind = 1u;
    m_admitted = true;
    return;
  }

  // Complete any already-quiescent reset before deciding whether this DIP
  // needs a new process-wide owner. Once the counter is published, reset is
  // forbidden from clearing this TLS until the destructor releases it.
  ProcessPendingBridgeReset();

  const uint64_t observedThread =
      g_observedRenderThreadId.load(std::memory_order_acquire);
  if (observedThread != 0u &&
      observedThread != ::GetCurrentThreadId()) {
    // War3's device is not multithreaded. Reject before the device walks D3D
    // stream/index state; the slow publish guard remains as a race backstop if
    // the observed identity changes after this admission point.
    g_counters.nativePoisonLeaks.fetch_add(
        1u, std::memory_order_relaxed);
    g_bypassEnabled.store(false, std::memory_order_release);
    m_failClosed = true;
    return;
  }

  if (t_dipObserverDepth != 0u) {
    if (t_dipObserverCookie == 0u ||
        t_dipObserverDepth == std::numeric_limits<uint32_t>::max()) {
      g_counters.dipObserverTransactionMismatches.fetch_add(
          1u, std::memory_order_relaxed);
      g_bypassEnabled.store(false, std::memory_order_release);
      m_failClosed = true;
      return;
    }
    // A nested device DIP makes the outer stack token permanently unusable.
    // Never restore it after the nested scope exits: the caller must take the
    // exact lifecycle route for the remainder of the outer scope.
    t_dipObserverCpuOnlySealCookie = 0u;
    ++t_dipObserverDepth;
    m_cookie = t_dipObserverCookie;
    m_coverKind = 2u;
    m_admitted = true;
    return;
  }

  if (!g_dipObserverIngressEnabled.load(std::memory_order_seq_cst) ||
      !g_hooksEnabled.load(std::memory_order_acquire)) {
    return;
  }

  // Publish before any observer-owned TLS mutation. Closing ingress can race
  // the optimistic read, but the authoritative reload below leaves either no
  // TLS state or a remotely visible independent transaction.
  g_activeDipObserverTransactions.fetch_add(
      1u, std::memory_order_seq_cst);
  if (!g_dipObserverIngressEnabled.load(std::memory_order_seq_cst) ||
      !g_hooksEnabled.load(std::memory_order_acquire)) {
    g_activeDipObserverTransactions.fetch_sub(
        1u, std::memory_order_seq_cst);
    ProcessPendingBridgeReset();
    return;
  }

  const uint64_t resetGeneration =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  const uint64_t cookie = NextDipObserverCookie(resetGeneration);
  t_dipObserverCookie = cookie;
  t_dipObserverResetGeneration = resetGeneration;
  t_dipObserverDepth = 1u;
  m_cookie = cookie;
  m_coverKind = 3u;
  m_admitted = true;
  // This seal can authorize only the default CPU/no-callback outside-DIP
  // result. The independent observer pin prevents reset/uninstall from
  // completing while it is live; per-DIP authorization and poison backstops
  // remain authoritative in TryRecordNativeOutsideDipFastPath.
  if (g_runtimeConfig.mode == GpuSkinMode::Bypass &&
      !g_runtimeConfig.fullDiagnostics && t_state.flushEpoch != 0u &&
      resetGeneration == t_state.appliedResetGeneration) {
    t_dipObserverCpuOnlySealCookie = cookie;
    m_cpuOnlySealCookie = cookie;
  } else {
    t_dipObserverCpuOnlySealCookie = 0u;
  }
  AddNativeTelemetryCounter(
      g_counters.dipObserverTransactionsBegun,
      t_nativeTelemetryDelta.dipObserverTransactionsBegun);
}

NativeDipObserverScope::~NativeDipObserverScope() noexcept {
  if (!m_admitted)
    return;

  if (m_coverKind == 1u)
    return;

  // Revoke the stack/TLS capability before any exactness check or global
  // observer decrement. A corrupt transaction may intentionally remain as a
  // process-wide fail-closed sentinel, but it must never leave a usable seal.
  t_dipObserverCpuOnlySealCookie = 0u;
  m_cpuOnlySealCookie = 0u;

  if (m_coverKind == 2u) {
    if (m_cookie == 0u || t_dipObserverCookie != m_cookie ||
        t_dipObserverDepth < 2u) {
      g_counters.dipObserverTransactionMismatches.fetch_add(
          1u, std::memory_order_relaxed);
      g_bypassEnabled.store(false, std::memory_order_release);
      return;
    }
    --t_dipObserverDepth;
    return;
  }

  if (m_coverKind != 3u) {
    g_counters.dipObserverTransactionMismatches.fetch_add(
        1u, std::memory_order_relaxed);
    g_bypassEnabled.store(false, std::memory_order_release);
    return;
  }

  const bool exact = m_cookie != 0u &&
      t_dipObserverCookie == m_cookie && t_dipObserverDepth == 1u &&
      g_activeDipObserverTransactions.load(
          std::memory_order_seq_cst) != 0u;
  if (!exact) {
    g_counters.dipObserverTransactionMismatches.fetch_add(
        1u, std::memory_order_relaxed);
    g_bypassEnabled.store(false, std::memory_order_release);
    if (g_activeDipObserverTransactions.load(
            std::memory_order_seq_cst) == 0u) {
      // Restore a remotely visible permanent sentinel if the aggregate was
      // corrupted. False-clean reset/unload is worse than a fail-closed leak.
      g_activeDipObserverTransactions.fetch_add(
          1u, std::memory_order_seq_cst);
    }
    return;
  }

  // Clear TLS before publishing the final process-wide decrement. A clean
  // quiescence observer can therefore never see active=0 with a live cookie.
  t_dipObserverDepth = 0u;
  t_dipObserverCookie = 0u;
  t_dipObserverResetGeneration = 0u;
  AddNativeTelemetryCounter(
      g_counters.dipObserverTransactionsEnded,
      t_nativeTelemetryDelta.dipObserverTransactionsEnded);
  uint32_t previous = g_activeDipObserverTransactions.load(
      std::memory_order_seq_cst);
  while (previous != 0u &&
         !g_activeDipObserverTransactions.compare_exchange_weak(
             previous, previous - 1u,
             std::memory_order_seq_cst,
             std::memory_order_seq_cst)) {
  }
  if (previous == 0u) {
    g_counters.dipObserverTransactionMismatches.fetch_add(
        1u, std::memory_order_relaxed);
    g_bypassEnabled.store(false, std::memory_order_release);
    g_activeDipObserverTransactions.fetch_add(
        1u, std::memory_order_seq_cst);
  }
  ProcessPendingBridgeReset();
}

namespace {

bool OutsideDipReaderLocalStateExact() noexcept {
  return t_state.flushEpoch != 0u &&
      t_state.flushTransactionDepth == 0u &&
      t_state.flushCallbackEpoch == 0u &&
      t_state.dispatchDepth == 0u && t_state.semanticDepth == 0u &&
      t_state.dispatchOverflowDepth == 0u &&
      t_state.semanticOverflowDepth == 0u &&
      t_state.nestedUploadDepth == 0u &&
      t_state.inFlightUpload.observation == nullptr &&
      t_state.productionFastRejectedUpload == nullptr &&
      !t_state.productionFastRejectedGlobalTransactionPinned &&
      t_state.productionEvidenceUploadEpoch == 0u &&
      !OutsideUploadFastPathActive() &&
      !DispatchCpuOnlyUploadFastPathActive() &&
      t_callbackPinDepth == 0u && t_dipObserverDepth == 0u &&
      t_dipObserverCookie == 0u &&
      t_dipObserverResetGeneration == 0u &&
      t_dipObserverCpuOnlySealCookie == 0u;
}

void RecordOutsideDipReaderBegin() noexcept {
  AddNativeTelemetryCounter(
      g_counters.dipOutsideReaderBegun,
      t_nativeTelemetryDelta.dipOutsideReaderBegun);
}

void RecordOutsideDipReaderReject(bool evidenceFallback) noexcept {
  AddNativeTelemetryCounter(
      g_counters.dipOutsideReaderRejects,
      t_nativeTelemetryDelta.dipOutsideReaderRejects);
  if (evidenceFallback) {
    AddNativeTelemetryCounter(
        g_counters.dipOutsideReaderEvidenceFallbacks,
        t_nativeTelemetryDelta.dipOutsideReaderEvidenceFallbacks);
  }
}

void RecordOutsideDipReaderEnd() noexcept {
  AddNativeTelemetryCounter(
      g_counters.dipOutsideReaderEnded,
      t_nativeTelemetryDelta.dipOutsideReaderEnded);
}

bool ReleaseOutsideDipReaderCover() noexcept {
  // This scope owns exactly one cover. A single seq_cst RMW preserves the
  // hook-removal/reset linearization while avoiding the old load+CAS loop on
  // every ordinary DIP. Unsigned underflow deliberately leaves a non-zero
  // remotely visible sentinel on impossible double-release corruption.
  const uint32_t previous =
      g_activeOutsideDipReaderTransactions.fetch_sub(
          1u, std::memory_order_seq_cst);
  if (previous != 0u)
    return true;

  // Preserve a remotely visible fail-closed sentinel. A false-clean hook drain
  // is more dangerous than retaining the bridge after impossible corruption.
  g_counters.dipOutsideReaderMismatches.fetch_add(
      1u, std::memory_order_relaxed);
  g_bypassEnabled.store(false, std::memory_order_release);
  return false;
}

void RejectAndReleaseOutsideDipReader(bool evidenceFallback) noexcept {
  RecordOutsideDipReaderReject(evidenceFallback);
  RecordOutsideDipReaderEnd();
  (void)ReleaseOutsideDipReaderCover();
  ProcessPendingBridgeReset();
}

}  // namespace

NativeOutsideDipReaderScope::NativeOutsideDipReaderScope(
    bool externalTicketPresent) noexcept {
  // Keep the overwhelmingly common non-candidate path free of process-wide
  // RMWs. Any ambiguity is retried by the complete observer path.
  if (g_runtimeConfig.mode != GpuSkinMode::Bypass ||
      g_runtimeConfig.fullDiagnostics || externalTicketPresent ||
      !OutsideDipReaderLocalStateExact() ||
      t_state.poisonLedger.count != 0u ||
      !g_dipObserverIngressEnabled.load(std::memory_order_seq_cst) ||
      !g_hooksEnabled.load(std::memory_order_acquire) ||
      !g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      !g_callbackIngressEnabled.load(std::memory_order_acquire) ||
      !g_bypassEnabled.load(std::memory_order_acquire)) {
    return;
  }

  // Publish activity before the authority snapshot. Reset completion and both
  // hook-removal drains include this independent counter, closing the finite-
  // snapshot TOCTOU that a truly stateless implementation would have.
  g_activeOutsideDipReaderTransactions.fetch_add(
      1u, std::memory_order_seq_cst);

  const uint64_t currentThread = ::GetCurrentThreadId();
  const uint64_t observedThread =
      g_observedRenderThreadId.load(std::memory_order_acquire);
  const uint64_t resetRequested =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  const uint64_t resetCompleted =
      g_resetCompletedGeneration.load(std::memory_order_acquire);
  const bool authorityExact =
      g_activeOutsideDipReaderTransactions.load(
          std::memory_order_seq_cst) != 0u &&
      g_dipObserverIngressEnabled.load(std::memory_order_seq_cst) &&
      g_hooksEnabled.load(std::memory_order_acquire) &&
      g_transactionIngressEnabled.load(std::memory_order_seq_cst) &&
      g_callbackIngressEnabled.load(std::memory_order_acquire) &&
      g_bypassEnabled.load(std::memory_order_acquire) &&
      observedThread != 0u && observedThread == currentThread &&
      resetRequested == resetCompleted &&
      t_state.appliedResetGeneration == resetRequested &&
      g_counters.pendingKernelAuthorizations.load(
          std::memory_order_acquire) == 0u &&
      g_retirementEventsPending.load(std::memory_order_acquire) == 0u &&
      !g_retirementQueueFaulted.load(std::memory_order_acquire) &&
      g_nativePoisonOutstandingRanges.load(
          std::memory_order_acquire) == 0u;
  // The complete TLS/poison predicate was checked immediately before this
  // thread published its cover. The intervening operations are only global
  // atomic loads and GetCurrentThreadId; no other thread can mutate this
  // thread's TLS. Re-reading that full predicate here added a redundant TLS
  // walk to every admitted outside DIP without strengthening authority.
  if (!authorityExact) {
    // Match the full observer's two-phase ingress protocol: a thread that read
    // an optimistic open gate before hook removal may publish a transient pin,
    // but a failed authoritative reload owns no reader transaction and must
    // not mutate telemetry after the clean uninstall endpoint.
    (void)ReleaseOutsideDipReaderCover();
    ProcessPendingBridgeReset();
    return;
  }

  RecordOutsideDipReaderBegin();

  // This independent cohort never authorizes the lighter path. It keeps the
  // complete observer admission and terminal accounting alive in the same run.
  const uint64_t evidenceOrdinal = ++t_outsideDipReaderEvidenceOrdinal;
  if ((evidenceOrdinal % kNativeOutsideDipReaderEvidencePeriod) == 0u) {
    RejectAndReleaseOutsideDipReader(true);
    return;
  }

  m_admitted = true;
}

void NativeOutsideDipReaderScope::Commit() noexcept {
  if (!m_admitted || m_committed)
    return;
  RecordNativeOutsideDipFastTelemetry(NativeOutsideDipFastCover::Reader);
  ++t_state.orphanDipOrdinal;
  AddNativeTelemetryCounter(
      g_counters.dipOutsideReaderCommits,
      t_nativeTelemetryDelta.dipOutsideReaderCommits);
  m_committed = true;
}

NativeOutsideDipReaderScope::~NativeOutsideDipReaderScope() noexcept {
  if (!m_admitted)
    return;
  if (!m_committed)
    RecordOutsideDipReaderReject(false);
  RecordOutsideDipReaderEnd();
  (void)ReleaseOutsideDipReaderCover();
  m_admitted = false;
  ProcessPendingBridgeReset();
}

bool InitializeNativeBridge(uintptr_t gameBase,
                            const NativeBridgeAddresses& addresses) {
  std::call_once(g_initializeOnce, [gameBase, addresses]() {
    g_runtimeConfig = GpuSkinRuntimeConfig::fromEnvironment();
    g_nativeOutsidePoisonSidecarPolicyMask.store(
        static_cast<uint8_t>(
            g_runtimeConfig.outsidePoisonSidecarPolicy),
        std::memory_order_release);
    g_gameBase = gameBase;
    if (g_runtimeConfig.mode == GpuSkinMode::Disabled || gameBase == 0u)
      return;
    g_fingerprint = ValidateFingerprint(gameBase, addresses);
    g_installEligible.store(g_fingerprint.exactMatch,
                            std::memory_order_release);
  });
  return g_installEligible.load(std::memory_order_acquire);
}

void SetNativeBridgeHooksInstalled(bool installed) {
  const bool enabled = installed &&
      g_installEligible.load(std::memory_order_acquire);
  if (enabled) {
    g_hooksEnabled.store(true, std::memory_order_release);
    g_dipObserverIngressEnabled.store(true, std::memory_order_seq_cst);
    g_transactionIngressEnabled.store(true, std::memory_order_seq_cst);
    g_callbackIngressEnabled.store(true, std::memory_order_release);
    const bool resetPending =
        g_resetRequestedGeneration.load(std::memory_order_acquire) !=
        g_resetCompletedGeneration.load(std::memory_order_acquire);
    g_bypassEnabled.store(
        !resetPending &&
            !g_retirementQueueFaulted.load(std::memory_order_acquire),
        std::memory_order_release);
    return;
  }

  // Stop new authorization and callback ingress first. Existing admitted TLS
  // transactions may still publish their terminal callback while detour pins
  // drain, which is required to settle/fuse an already committed bypass.
  g_bypassEnabled.store(false, std::memory_order_release);
  g_callbackIngressEnabled.store(false, std::memory_order_release);
  g_transactionIngressEnabled.store(false, std::memory_order_seq_cst);
  RequestBridgeResetGeneration(
      NativePoisonLedgerResetReason::BridgeDisabled);
  ProcessPendingBridgeReset();
}

bool CloseNativeDipObserverIngressForRemoval() noexcept {
  // Only the hook owner may call this after its first drain. At this point no
  // new bypass can create poison; a racing full observer is still covered by
  // its published counter and consumed by the mandatory second drain. Strict
  // outside readers also require the already-closed transaction ingress, so
  // any fully admitted reader must have left before this gate can close.
  if (g_bypassEnabled.load(std::memory_order_acquire) ||
      g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      g_callbackIngressEnabled.load(std::memory_order_acquire) ||
      g_counters.pendingKernelAuthorizations.load(
          std::memory_order_acquire) != 0u ||
      g_nativePoisonOutstandingRanges.load(std::memory_order_acquire) != 0u ||
      g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
      g_retirementQueueFaulted.load(std::memory_order_acquire) ||
      g_resetRequestedGeneration.load(std::memory_order_acquire) !=
          g_resetCompletedGeneration.load(std::memory_order_acquire) ||
      g_activeCallbackPins.load(std::memory_order_acquire) != 0u ||
      g_activeFlushTransactions.load(std::memory_order_seq_cst) != 0u ||
      g_activeDispatchTransactions.load(std::memory_order_seq_cst) != 0u ||
      g_activeSemanticTransactions.load(std::memory_order_seq_cst) != 0u ||
      g_activeUploadTransactions.load(std::memory_order_seq_cst) != 0u ||
      g_activeOutsideDipReaderTransactions.load(
          std::memory_order_seq_cst) != 0u) {
    return false;
  }
  g_dipObserverIngressEnabled.store(false, std::memory_order_seq_cst);
  return true;
}

void FinalizeNativeBridgeHooksRemoved() {
  g_bypassEnabled.store(false, std::memory_order_release);
  g_callbackIngressEnabled.store(false, std::memory_order_release);
  g_transactionIngressEnabled.store(false, std::memory_order_seq_cst);
  g_dipObserverIngressEnabled.store(false, std::memory_order_seq_cst);
  g_hooksEnabled.store(false, std::memory_order_release);
}

void RetainNativeBridgeSafetyObservation() {
  g_bypassEnabled.store(false, std::memory_order_release);
  g_callbackIngressEnabled.store(false, std::memory_order_release);
  g_transactionIngressEnabled.store(false, std::memory_order_seq_cst);
  g_dipObserverIngressEnabled.store(true, std::memory_order_seq_cst);
  g_hooksEnabled.store(true, std::memory_order_release);
}

bool NativeBridgeHooksEnabled() {
  return g_hooksEnabled.load(std::memory_order_acquire);
}

GpuSkinRuntimeConfig GetNativeBridgeRuntimeConfig() {
  return g_runtimeConfig;
}

NativeBridgeFingerprint GetNativeBridgeFingerprint() {
  return g_fingerprint;
}

NativeBridgeCounters GetNativeBridgeCounters() {
  // A forced snapshot on the observed render thread is exact. An off-thread
  // relaxed snapshot cannot consume another TLS and may therefore report
  // aggregate counters that lag while telemetryDeltaPending remains 1.
  if (CurrentThreadIsObservedRenderThread())
    FlushNativeTelemetryDeltaForCurrentThread();
  const uint64_t telemetryFlushSequenceBefore =
      g_counters.telemetryFlushes.load(std::memory_order_acquire);
  const uint32_t telemetryPendingBefore =
      g_nativeTelemetryDeltaPending.load(std::memory_order_acquire);
  const bool telemetryFaultedBefore =
      g_nativeTelemetryDeltaFaulted.load(std::memory_order_acquire);
  const uint64_t productionWritesStartedBefore =
      g_productionTimingWritesStarted.load(std::memory_order_acquire);
  const uint64_t productionWritesCompletedBefore =
      g_productionTimingWritesCompleted.load(std::memory_order_acquire);
  const uint32_t productionWritersBefore =
      g_productionTimingWriters.load(std::memory_order_acquire);
  NativeBridgeCounters result = {};
  // Read the immutable mask consumed by the D3D9 Lock/kernel/Unlock gates,
  // rather than copying the parser-side runtime config a second time. This
  // makes configCounterExact an independent publication/consumer proof.
  result.productionOutsidePoisonSidecarPolicy = static_cast<uint32_t>(
      g_nativeOutsidePoisonSidecarPolicyMask.load(
          std::memory_order_acquire));
  result.productionOutsidePoisonSidecarPolicyExplicit =
      g_runtimeConfig.outsidePoisonSidecarPolicyExplicit;
  result.productionOutsidePoisonSidecarPolicyInvalid =
      g_runtimeConfig.outsidePoisonSidecarPolicyInvalid;
  result.hotPathTimingFrequency = uint64_t(
      dxvk::high_resolution_clock::get_frequency());
#define COPY_COUNTER(name) \
  result.name = g_counters.name.load(std::memory_order_relaxed)
  COPY_COUNTER(beginUploadTimingCalls);
  COPY_COUNTER(beginUploadTimingTicks);
  COPY_COUNTER(beginUploadTimingMaxTicks);
  COPY_COUNTER(beginSampleCommonCalls);
  COPY_COUNTER(beginSampleCommonTicks);
  COPY_COUNTER(beginSampleCommonMaxTicks);
  COPY_COUNTER(beginSampleStateCalls);
  COPY_COUNTER(beginSampleStateTicks);
  COPY_COUNTER(beginSampleStateMaxTicks);
  COPY_COUNTER(beginSampleExactCalls);
  COPY_COUNTER(beginSampleExactTicks);
  COPY_COUNTER(beginSampleExactMaxTicks);
  COPY_COUNTER(beginSampleScopeRouteCalls);
  COPY_COUNTER(beginSampleScopeRouteTicks);
  COPY_COUNTER(beginSampleScopeRouteMaxTicks);
  COPY_COUNTER(beginSampleStateRejectRouteCalls);
  COPY_COUNTER(beginSampleStateRejectRouteTicks);
  COPY_COUNTER(beginSampleStateRejectRouteMaxTicks);
  COPY_COUNTER(beginSampleSkinRouteCalls);
  COPY_COUNTER(beginSampleSkinRouteTicks);
  COPY_COUNTER(beginSampleSkinRouteMaxTicks);
  COPY_COUNTER(beginSampleSmallRouteCalls);
  COPY_COUNTER(beginSampleSmallRouteTicks);
  COPY_COUNTER(beginSampleSmallRouteMaxTicks);
  COPY_COUNTER(beginSampleCandidateRouteCalls);
  COPY_COUNTER(beginSampleCandidateRouteTicks);
  COPY_COUNTER(beginSampleCandidateRouteMaxTicks);
  COPY_COUNTER(t2SampleGeoSnapCalls);
  COPY_COUNTER(t2SampleGeoSnapTicks);
  COPY_COUNTER(t2SampleGeoSnapMaxTicks);
  COPY_COUNTER(t2SampleGeoHeaderCalls);
  COPY_COUNTER(t2SampleGeoHeaderTicks);
  COPY_COUNTER(t2SampleGeoHeaderMaxTicks);
  COPY_COUNTER(t2SamplePositionProofCalls);
  COPY_COUNTER(t2SamplePositionProofTicks);
  COPY_COUNTER(t2SamplePositionProofMaxTicks);
  COPY_COUNTER(t2SampleNormalProofCalls);
  COPY_COUNTER(t2SampleNormalProofTicks);
  COPY_COUNTER(t2SampleNormalProofMaxTicks);
  COPY_COUNTER(t2SampleGroupProofCalls);
  COPY_COUNTER(t2SampleGroupProofTicks);
  COPY_COUNTER(t2SampleGroupProofMaxTicks);
  COPY_COUNTER(t2SamplePaletteProofCalls);
  COPY_COUNTER(t2SamplePaletteProofTicks);
  COPY_COUNTER(t2SamplePaletteProofMaxTicks);
  COPY_COUNTER(evaluateKernelTimingCalls);
  COPY_COUNTER(evaluateKernelTimingTicks);
  COPY_COUNTER(evaluateKernelTimingMaxTicks);
  COPY_COUNTER(completeUploadTimingCalls);
  COPY_COUNTER(completeUploadTimingTicks);
  COPY_COUNTER(completeUploadTimingMaxTicks);
  COPY_COUNTER(notifyNormalTimingCalls);
  COPY_COUNTER(notifyNormalTimingTicks);
  COPY_COUNTER(notifyNormalTimingMaxTicks);
  COPY_COUNTER(dipTimingCalls);
  COPY_COUNTER(dipTimingTicks);
  COPY_COUNTER(dipTimingMaxTicks);
  COPY_COUNTER(outerUploadTimingCalls);
  COPY_COUNTER(outerUploadTimingTicks);
  COPY_COUNTER(outerUploadTimingMaxTicks);
  COPY_COUNTER(originalKernelTimingCalls);
  COPY_COUNTER(originalKernelTimingTicks);
  COPY_COUNTER(originalKernelTimingMaxTicks);
  COPY_COUNTER(flushNotifications);
  COPY_COUNTER(dispatchScopes);
  COPY_COUNTER(dispatchScopeEnds);
  COPY_COUNTER(commonDispatchScopes);
  COPY_COUNTER(specialDispatchScopes);
  COPY_COUNTER(managerDispatchEagerScopes);
  COPY_COUNTER(managerDispatchLazyScopes);
  COPY_COUNTER(managerDispatchNeverScopes);
  COPY_COUNTER(managerDispatchNativeCpuOnlyScopes);
  COPY_COUNTER(managerDispatchNativeCpuOnlyEnds);
  COPY_COUNTER(managerDispatchEvidenceEagerScopes);
  COPY_COUNTER(managerDispatchBeginCallbacks);
  COPY_COUNTER(managerDispatchEndCallbacks);
  COPY_COUNTER(managerDispatchEagerBegins);
  COPY_COUNTER(managerDispatchLazyAdmissionAttempts);
  COPY_COUNTER(managerDispatchLazyAdmissions);
  COPY_COUNTER(managerDispatchEagerAdmissionFailures);
  COPY_COUNTER(managerDispatchLazyAdmissionFailures);
  COPY_COUNTER(managerDispatchNeverSafetyFailures);
  COPY_COUNTER(managerDispatchIssuedEnds);
  COPY_COUNTER(managerDispatchNoUploadEnds);
  COPY_COUNTER(managerDispatchNeverEnds);
  COPY_COUNTER(managerDispatchFailedEnds);
  COPY_COUNTER(managerDispatchSkippedUploads);
  COPY_COUNTER(managerDispatchSkippedDips);
  COPY_COUNTER(managerDispatchSkippedFanouts);
  COPY_COUNTER(semanticScopes);
  COPY_COUNTER(uploads);
  COPY_COUNTER(uploadsOutsideDispatch);
  COPY_COUNTER(nativeEligibleUploads);
  COPY_COUNTER(productionFastRejectScope);
  COPY_COUNTER(productionFastRejectState);
  COPY_COUNTER(productionFastRejectSkinFormat);
  COPY_COUNTER(productionFastRejectInput);
  COPY_COUNTER(productionFastRejectSmall);
  COPY_COUNTER(productionFastRejectUnknownState);
  COPY_COUNTER(productionCandidates);
  COPY_COUNTER(productionPoisonRetirementOnly);
  COPY_COUNTER(productionOutsideCallbacksSkipped);
  COPY_COUNTER(productionOutsideNativeFastPath);
  COPY_COUNTER(productionOutsideNoPoisonDirectAttempts);
  COPY_COUNTER(productionOutsideNoPoisonDirectKernelCalls);
  COPY_COUNTER(productionOutsideNoPoisonDirectNormalReturns);
  COPY_COUNTER(productionOutsideNoPoisonDirectKernelNoNormalReturns);
  COPY_COUNTER(productionOutsideNoPoisonDirectCompleted);
  COPY_COUNTER(productionOutsideNoPoisonDirectConflicts);
  COPY_COUNTER(productionOutsideNoPoisonDirectCancellations);
  COPY_COUNTER(productionOutsideNoPoisonDirectActive);
  COPY_COUNTER(productionOutsideNoPoisonDirectResetCompletedWhileActive);
  COPY_COUNTER(productionOutsideNoPoisonDirectLatePoison);
  COPY_COUNTER(productionOutsidePoisonScanAttempts);
  COPY_COUNTER(productionOutsidePoisonNoOverlapAdmissions);
  COPY_COUNTER(productionOutsidePoisonOverlapRejects);
  COPY_COUNTER(productionOutsidePoisonReadFailRejects);
  COPY_COUNTER(productionOutsidePoisonShadowAttempts);
  COPY_COUNTER(productionOutsidePoisonShadowCreated);
  COPY_COUNTER(productionOutsidePoisonShadowOverflow);
  COPY_COUNTER(productionOutsidePoisonShadowActive);
  COPY_COUNTER(productionOutsidePoisonShadowLockNotifications);
  COPY_COUNTER(productionOutsidePoisonShadowSettled);
  COPY_COUNTER(productionOutsidePoisonShadowCancelled);
  COPY_COUNTER(productionOutsidePoisonShadowResetAborted);
  COPY_COUNTER(productionOutsidePoisonShadowComparable);
  COPY_COUNTER(productionOutsidePoisonShadowUnprovable);
  COPY_COUNTER(productionOutsidePoisonO1ShadowAttempts);
  COPY_COUNTER(productionOutsidePoisonO1ShadowCreated);
  COPY_COUNTER(productionOutsidePoisonO1ShadowOverflow);
  COPY_COUNTER(productionOutsidePoisonO1ShadowActive);
  COPY_COUNTER(productionOutsidePoisonO1ShadowLockNotifications);
  COPY_COUNTER(productionOutsidePoisonO1ShadowKernelNotifications);
  COPY_COUNTER(productionOutsidePoisonO1ShadowUnlockNotifications);
  COPY_COUNTER(productionOutsidePoisonO1ShadowFrozen);
  COPY_COUNTER(productionOutsidePoisonO1ShadowSettled);
  COPY_COUNTER(productionOutsidePoisonO1ShadowCancelled);
  COPY_COUNTER(productionOutsidePoisonO1ShadowResetAborted);
  COPY_COUNTER(productionOutsidePoisonO1ShadowComparable);
  COPY_COUNTER(productionOutsidePoisonO1ShadowUnprovable);
  COPY_COUNTER(productionOutsidePoisonO1ShadowComparisonMissing);
  COPY_COUNTER(productionOutsidePoisonO1ShadowWouldClear);
  COPY_COUNTER(productionOutsidePoisonO1ShadowOldOverlapFrozen);
  COPY_COUNTER(productionOutsidePoisonO1ShadowDirectDiscardNotifications);
  COPY_COUNTER(
      productionOutsidePoisonO1ShadowDirectDiscardOldOverlapRetired);
  COPY_COUNTER(
      productionOutsidePoisonO1ShadowOldOverlapToNoOverlapDiscardExact);
  COPY_COUNTER(productionOutsidePoisonO1ShadowOldOverlapToNoOverlapOther);
  COPY_COUNTER(productionOutsidePoisonO1ShadowScanDifferentTarget);
  COPY_COUNTER(productionOutsidePoisonO1ShadowStorageDiagnosticMismatch);
  COPY_COUNTER(productionOutsidePoisonO1ShadowRealStorageDrift);
  COPY_COUNTER(productionOutsidePoisonO1ShadowMappingStorageDrift);
  COPY_COUNTER(
      productionOutsidePoisonO1ShadowMapAllocationDiagnosticMismatch);
  result.productionOutsidePoisonO1ShadowAuthority =
      kNativeOutsidePoisonO1ShadowAuthority;
  COPY_COUNTER(productionOutsidePoisonAuthorityAttempts);
  COPY_COUNTER(productionOutsidePoisonAuthorityCreated);
  COPY_COUNTER(productionOutsidePoisonAuthorityOverflow);
  COPY_COUNTER(productionOutsidePoisonAuthorityArmed);
  result.productionOutsidePoisonAuthorityActive =
      g_nativeOutsidePoisonAuthorityActive.load(std::memory_order_acquire);
  COPY_COUNTER(productionOutsidePoisonAuthoritySettled);
  COPY_COUNTER(productionOutsidePoisonAuthorityCancelled);
  COPY_COUNTER(productionOutsidePoisonAuthorityResetAborted);
  COPY_COUNTER(productionOutsidePoisonAuthorityLockNotifications);
  COPY_COUNTER(productionOutsidePoisonAuthorityLockNoOverlap);
  COPY_COUNTER(productionOutsidePoisonAuthorityLockOverlap);
  COPY_COUNTER(productionOutsidePoisonAuthorityLockRejects);
  COPY_COUNTER(productionOutsidePoisonAuthorityKernelReady);
  COPY_COUNTER(productionOutsidePoisonAuthorityKernelRejects);
  COPY_COUNTER(productionOutsidePoisonAuthorityNormalReturns);
  COPY_COUNTER(productionOutsidePoisonAuthorityUnlockNotifications);
  COPY_COUNTER(productionOutsidePoisonAuthorityUnlockExact);
  COPY_COUNTER(productionOutsidePoisonAuthorityUnlockRejects);
  COPY_COUNTER(productionOutsidePoisonAuthorityCommittedNoOverlap);
  COPY_COUNTER(productionOutsidePoisonAuthorityCommittedRewrite);
  COPY_COUNTER(productionOutsidePoisonAuthorityRetained);
  COPY_COUNTER(productionOutsidePoisonAuthorityPoisonClears);
  COPY_COUNTER(productionOutsidePoisonAuthority);
  COPY_COUNTER(productionOutsidePoisonAuthorityEvidenceAttempts);
  COPY_COUNTER(productionOutsidePoisonAuthorityEvidenceComparable);
  COPY_COUNTER(productionOutsidePoisonAuthorityEvidenceUnprovable);
  COPY_COUNTER(productionOutsidePoisonAuthorityEvidenceMismatches);
  COPY_COUNTER(productionOutsidePoisonAuthorityEvidenceAuthority);
  COPY_COUNTER(productionOutsidePoisonAuthorityLegacyBackedAuthority);
  COPY_COUNTER(productionOutsideFastKernelMarkerConflicts);
  COPY_COUNTER(originalUploadCalls);
  COPY_COUNTER(originalUploadBytes);
  COPY_COUNTER(bypassPreflightAttempts);
  COPY_COUNTER(bypassAuthorizations);
  COPY_COUNTER(kernelHookCalls);
  COPY_COUNTER(originalKernelCalls);
  COPY_COUNTER(originalKernelNormalReturns);
  COPY_COUNTER(originalKernelNormalReturnRejects);
  COPY_COUNTER(cpuRewriteProofAttempts);
  COPY_COUNTER(cpuRewriteProofExact);
  COPY_COUNTER(cpuRewriteProofRejects);
  COPY_COUNTER(originalKernelBytes);
  COPY_COUNTER(bypassedKernelCalls);
  COPY_COUNTER(bypassedKernelBytes);
  COPY_COUNTER(nullMappedKernelFallbacks);
  COPY_COUNTER(kernelPreflightRejects);
  COPY_COUNTER(postSkipMismatchFuses);
  COPY_COUNTER(postSkipNativeFallback);
  COPY_COUNTER(duplicateKernelCalls);
  COPY_COUNTER(irreversibleKernelSuppressions);
  COPY_COUNTER(pendingKernelAuthorizations);
  COPY_COUNTER(nativePoisonCreates);
  COPY_COUNTER(nativePoisonClears);
  COPY_COUNTER(nativePoisonHits);
  COPY_COUNTER(nativePoisonIncompleteIdentityHits);
  COPY_COUNTER(identityMatchedLayoutMismatchHits);
  COPY_COUNTER(nativePoisonOverflows);
  COPY_COUNTER(nativePoisonLeaks);
  COPY_COUNTER(nativeDirectDiscardEvents);
  COPY_COUNTER(nativeDirectDiscardEventsWithPoison);
  COPY_COUNTER(nativeDirectDiscardNoPoisonEvents);
  COPY_COUNTER(nativeDirectDiscardRangesCleared);
  COPY_COUNTER(nativeDirectDiscardInvalid);
  COPY_COUNTER(nativeCrossMapAllocationPoisonMerges);
  COPY_COUNTER(nativeRetirementEventsPublished);
  COPY_COUNTER(nativeRetirementEventsConsumed);
  COPY_COUNTER(nativeRetirementRangesCleared);
  COPY_COUNTER(nativeRetirementQueueOverflows);
  COPY_COUNTER(nativeRetirementInvalidEvents);
  COPY_COUNTER(resetRequests);
  COPY_COUNTER(resetCompletions);
  COPY_COUNTER(resetDeferred);
  COPY_COUNTER(resetWrongThread);
  COPY_COUNTER(resetActiveTransactions);
  COPY_COUNTER(resetOwnerEpochDeferred);
  COPY_COUNTER(resetPoisonDeferred);
  COPY_COUNTER(resetRetirementQueueFaults);
  COPY_COUNTER(resetFailClosedUploads);
  COPY_COUNTER(resetOwnerAcks);
  COPY_COUNTER(resetOwnerAckMismatches);
  COPY_COUNTER(resetCommitBlocked);
  COPY_COUNTER(resetSlowPathCalls);
  COPY_COUNTER(indexTicketFailureMask);
  COPY_COUNTER(indexTicketAttempts);
  COPY_COUNTER(indexTicketExact);
  COPY_COUNTER(indexTicketSuppressed);
  COPY_COUNTER(indexTicketLeaks);
  COPY_COUNTER(bypassedUploadCalls);
  COPY_COUNTER(bypassedUploadBytes);
  COPY_COUNTER(bypassFallbackCalls);
  COPY_COUNTER(bypassStateMismatches);
  COPY_COUNTER(bypassSideEffectFailures);
  COPY_COUNTER(inputPreflightRejectedUploads);
  COPY_COUNTER(dips);
  COPY_COUNTER(outsideDispatchDips);
  COPY_COUNTER(dispatchNoUploadDips);
  COPY_COUNTER(correlatedDips);
  COPY_COUNTER(unmatchedDips);
  COPY_COUNTER(dipObserverTransactionsBegun);
  COPY_COUNTER(dipObserverTransactionsEnded);
  COPY_COUNTER(dipObserverTransactionMismatches);
  COPY_COUNTER(dipOutsideReaderBegun);
  COPY_COUNTER(dipOutsideReaderEnded);
  COPY_COUNTER(dipOutsideReaderCommits);
  COPY_COUNTER(dipOutsideReaderRejects);
  COPY_COUNTER(dipOutsideReaderEvidenceFallbacks);
  COPY_COUNTER(dipOutsideReaderMismatches);
  COPY_COUNTER(productionOutsideDipFastByFlush);
  COPY_COUNTER(productionOutsideDipFastByObserver);
  COPY_COUNTER(productionOutsideDipFastByReader);
  COPY_COUNTER(dispatchCpuOnlySealProposals);
  COPY_COUNTER(dispatchCpuOnlySealProposalAccepted);
  COPY_COUNTER(dispatchCpuOnlySealProposalRejected);
  COPY_COUNTER(dispatchCpuOnlySealProposalAborted);
  COPY_COUNTER(dispatchCpuOnlySealScopeCommits);
  COPY_COUNTER(dispatchCpuOnlySealScopeEnds);
  COPY_COUNTER(dispatchCpuOnlySealInvalidations);
  COPY_COUNTER(dispatchCpuOnlySealUploadsStarted);
  COPY_COUNTER(dispatchCpuOnlySealUploadsCompleted);
  COPY_COUNTER(dispatchCpuOnlySealVertices);
  COPY_COUNTER(dispatchCpuOnlySealBytes);
  COPY_COUNTER(dispatchCpuOnlySealKernelCalls);
  COPY_COUNTER(dispatchCpuOnlySealKernelNormalReturns);
  COPY_COUNTER(dispatchCpuOnlySealDips);
  COPY_COUNTER(dispatchCpuOnlySealDipsWithUpload);
  COPY_COUNTER(dispatchCpuOnlySealDipsNoUpload);
  COPY_COUNTER(dispatchCpuOnlySealFanoutZero);
  COPY_COUNTER(dispatchCpuOnlySealFanoutOne);
  COPY_COUNTER(dispatchCpuOnlySealFanoutMany);
  COPY_COUNTER(dispatchCpuOnlySealFanoutDipTotal);
  COPY_COUNTER(dispatchCpuOnlySealMarkerConflicts);
  COPY_COUNTER(dispatchCpuOnlySealLocalViewPublishAttempts);
  COPY_COUNTER(dispatchCpuOnlySealLocalViewPublishes);
  COPY_COUNTER(dispatchCpuOnlySealLocalViewRejects);
  COPY_COUNTER(dispatchCpuOnlySealLocalViewQueries);
  COPY_COUNTER(dispatchCpuOnlySealLocalViewAuthorityRejects);
  COPY_COUNTER(dispatchCpuOnlySealLocalViewCandidateRejects);
  COPY_COUNTER(dispatchCpuOnlySealLocalViewCommits);
  COPY_COUNTER(uploadFanoutZero);
  COPY_COUNTER(uploadFanoutOne);
  COPY_COUNTER(uploadFanoutMany);
  COPY_COUNTER(maxUploadFanout);
  COPY_COUNTER(nestedDispatchScopes);
  COPY_COUNTER(nestedSemanticScopes);
  COPY_COUNTER(nestedUploadScopes);
  COPY_COUNTER(scopeFailClosedUploads);
  COPY_COUNTER(scopeFailClosedKernelFallbacks);
  COPY_COUNTER(dispatchStackOverflow);
  COPY_COUNTER(semanticStackOverflow);
  COPY_COUNTER(queryMiss);
  COPY_COUNTER(conflict);
  COPY_COUNTER(unknown);
  COPY_COUNTER(layerMismatch);
  COPY_COUNTER(scopeForced);
  COPY_COUNTER(telemetryBatchedAdds);
  COPY_COUNTER(productionFastRejectKernelBatches);
  COPY_COUNTER(productionOutsideDipFastPath);
  COPY_COUNTER(productionOutsideDipFastEarlySuccesses);
  COPY_COUNTER(productionOutsideDipFastLateSuccesses);
  COPY_COUNTER(productionOutsideAdmissionAcceptedNoPoison);
  COPY_COUNTER(productionOutsideAdmissionAcceptedWithPoison);
  COPY_COUNTER(productionOutsideAdmissionCancellations);
  COPY_COUNTER(productionOutsideAdmissionLifecycleExcluded);
  COPY_COUNTER(productionOutsideAdmissionTrackedResolvedInside);
  COPY_COUNTER(productionOutsideAdmissionUntrackedResolvedOutside);
  COPY_COUNTER(productionOutsideCoverFlushBegins);
  COPY_COUNTER(productionOutsideCoverSemanticBegins);
  COPY_COUNTER(productionOutsideCoverIndependentBegins);
  COPY_COUNTER(productionOutsideIndependentPinBegins);
  COPY_COUNTER(productionOutsideIndependentPinEnds);
#undef COPY_COUNTER
  for (size_t i = 0; i < 8u; i++) {
    result.formatHistogram[i] =
        g_counters.formatHistogram[i].load(std::memory_order_relaxed);
    result.skinModeHistogram[i] =
        g_counters.skinModeHistogram[i].load(std::memory_order_relaxed);
  }
  for (size_t i = 0; i < kNativeUploadInputPreflightBitCount; i++) {
    result.inputPreflightMissing[i] =
        g_counters.inputPreflightMissing[i].load(std::memory_order_relaxed);
  }
  for (size_t i = 0; i < kNativeIndexTicketFailureBitCount; i++) {
    result.indexTicketFailureReasons[i] =
        g_counters.indexTicketFailureReasons[i].load(
            std::memory_order_relaxed);
  }
  bool poisonShadowArithmeticClean = true;
  const auto checkedPoisonShadowAdd = [&poisonShadowArithmeticClean](
      uint64_t& destination, uint64_t value) {
    if (value > std::numeric_limits<uint64_t>::max() - destination) {
      poisonShadowArithmeticClean = false;
      return;
    }
    destination += value;
  };
  for (size_t i = 0u; i < kNativeOutsidePoisonShadowMatrixSize; ++i) {
    result.productionOutsidePoisonShadowMatrix[i] =
        g_counters.productionOutsidePoisonShadowMatrix[i].load(
            std::memory_order_relaxed);
    checkedPoisonShadowAdd(
        result.productionOutsidePoisonShadowMatrixTotal,
        result.productionOutsidePoisonShadowMatrix[i]);
    if (i / 3u != i % 3u) {
      checkedPoisonShadowAdd(
          result.productionOutsidePoisonShadowOffDiagonal,
          result.productionOutsidePoisonShadowMatrix[i]);
    }
  }
  result.productionOutsidePoisonShadowLegacyMissedOverlap =
      result.productionOutsidePoisonShadowMatrix[1u];
  uint64_t poisonShadowUnprovableReasonTotal = 0u;
  for (size_t i = 0u;
       i < kNativeOutsidePoisonShadowUnprovableReasonCount; ++i) {
    result.productionOutsidePoisonShadowUnprovableReasons[i] =
        g_counters.productionOutsidePoisonShadowUnprovableReasons[i].load(
            std::memory_order_relaxed);
    checkedPoisonShadowAdd(
        poisonShadowUnprovableReasonTotal,
        result.productionOutsidePoisonShadowUnprovableReasons[i]);
  }
  uint64_t poisonShadowAttemptTerminal = 0u;
  checkedPoisonShadowAdd(
      poisonShadowAttemptTerminal,
      result.productionOutsidePoisonShadowCreated);
  checkedPoisonShadowAdd(
      poisonShadowAttemptTerminal,
      result.productionOutsidePoisonShadowOverflow);
  result.productionOutsidePoisonShadowAttemptClosureClean =
      poisonShadowArithmeticClean &&
      result.productionOutsidePoisonShadowAttempts ==
          poisonShadowAttemptTerminal &&
      (GpuSkinOutsidePoisonO0Enabled(
           g_runtimeConfig.outsidePoisonSidecarPolicy)
           ? result.productionOutsidePoisonShadowAttempts ==
                 result.productionOutsidePoisonScanAttempts
           : result.productionOutsidePoisonShadowAttempts == 0u);
  uint64_t poisonShadowLifetimeTerminal = 0u;
  checkedPoisonShadowAdd(
      poisonShadowLifetimeTerminal,
      result.productionOutsidePoisonShadowSettled);
  checkedPoisonShadowAdd(
      poisonShadowLifetimeTerminal,
      result.productionOutsidePoisonShadowCancelled);
  checkedPoisonShadowAdd(
      poisonShadowLifetimeTerminal,
      result.productionOutsidePoisonShadowResetAborted);
  checkedPoisonShadowAdd(
      poisonShadowLifetimeTerminal,
      result.productionOutsidePoisonShadowActive);
  result.productionOutsidePoisonShadowLifetimeClosureClean =
      poisonShadowArithmeticClean &&
      result.productionOutsidePoisonShadowCreated ==
          poisonShadowLifetimeTerminal;
  uint64_t poisonShadowSettlementTerminal = 0u;
  checkedPoisonShadowAdd(
      poisonShadowSettlementTerminal,
      result.productionOutsidePoisonShadowComparable);
  checkedPoisonShadowAdd(
      poisonShadowSettlementTerminal,
      result.productionOutsidePoisonShadowUnprovable);
  result.productionOutsidePoisonShadowSettlementClosureClean =
      poisonShadowArithmeticClean &&
      result.productionOutsidePoisonShadowSettled ==
          poisonShadowSettlementTerminal &&
      result.productionOutsidePoisonShadowComparable ==
          result.productionOutsidePoisonShadowMatrixTotal &&
      result.productionOutsidePoisonShadowUnprovable ==
          poisonShadowUnprovableReasonTotal;

  bool poisonO1ShadowArithmeticClean = true;
  const auto checkedPoisonO1ShadowAdd = [&poisonO1ShadowArithmeticClean](
      uint64_t& destination, uint64_t value) {
    if (value > std::numeric_limits<uint64_t>::max() - destination) {
      poisonO1ShadowArithmeticClean = false;
      return;
    }
    destination += value;
  };
  bool poisonO1ShadowLaneClosureClean = true;
  uint64_t poisonO1ShadowScanFailureTotal = 0u;
  for (size_t lane = 0u;
       lane < kNativeOutsidePoisonO1LockLaneCount; ++lane) {
    result.productionOutsidePoisonO1ShadowScanCallsByLane[lane] =
        g_counters.productionOutsidePoisonO1ShadowScanCallsByLane[lane].load(
            std::memory_order_relaxed);
    result.productionOutsidePoisonO1ShadowScanNoOverlapByLane[lane] =
        g_counters
            .productionOutsidePoisonO1ShadowScanNoOverlapByLane[lane]
            .load(std::memory_order_relaxed);
    result.productionOutsidePoisonO1ShadowScanOverlapByLane[lane] =
        g_counters.productionOutsidePoisonO1ShadowScanOverlapByLane[lane]
            .load(std::memory_order_relaxed);
    result.productionOutsidePoisonO1ShadowScanReadFailureByLane[lane] =
        g_counters
            .productionOutsidePoisonO1ShadowScanReadFailureByLane[lane]
            .load(std::memory_order_relaxed);
    result.productionOutsidePoisonO1ShadowPreLockMutationByLane[lane] =
        g_counters
            .productionOutsidePoisonO1ShadowPreLockMutationByLane[lane]
            .load(std::memory_order_relaxed);
    result.productionOutsidePoisonO1ShadowLockToKernelMutationByLane[lane] =
        g_counters
            .productionOutsidePoisonO1ShadowLockToKernelMutationByLane[lane]
            .load(std::memory_order_relaxed);

    checkedPoisonO1ShadowAdd(
        result.productionOutsidePoisonO1ShadowScanCalls,
        result.productionOutsidePoisonO1ShadowScanCallsByLane[lane]);
    checkedPoisonO1ShadowAdd(
        result.productionOutsidePoisonO1ShadowScanNoOverlap,
        result.productionOutsidePoisonO1ShadowScanNoOverlapByLane[lane]);
    checkedPoisonO1ShadowAdd(
        result.productionOutsidePoisonO1ShadowScanOverlap,
        result.productionOutsidePoisonO1ShadowScanOverlapByLane[lane]);
    checkedPoisonO1ShadowAdd(
        result.productionOutsidePoisonO1ShadowScanReadFailure,
        result.productionOutsidePoisonO1ShadowScanReadFailureByLane[lane]);

    uint64_t laneTerminal = 0u;
    checkedPoisonO1ShadowAdd(
        laneTerminal,
        result.productionOutsidePoisonO1ShadowScanNoOverlapByLane[lane]);
    checkedPoisonO1ShadowAdd(
        laneTerminal,
        result.productionOutsidePoisonO1ShadowScanOverlapByLane[lane]);
    checkedPoisonO1ShadowAdd(
        laneTerminal,
        result.productionOutsidePoisonO1ShadowScanReadFailureByLane[lane]);
    uint64_t laneFailureTotal = 0u;
    for (size_t reason = 0u;
         reason < kNativeOutsidePoisonO1ScanFailureReasonCount; ++reason) {
      const size_t laneReason =
          lane * kNativeOutsidePoisonO1ScanFailureReasonCount + reason;
      result.productionOutsidePoisonO1ShadowScanFailuresByLane[laneReason] =
          g_counters
              .productionOutsidePoisonO1ShadowScanFailuresByLane[laneReason]
              .load(std::memory_order_relaxed);
      checkedPoisonO1ShadowAdd(
          laneFailureTotal,
          result.productionOutsidePoisonO1ShadowScanFailuresByLane[
              laneReason]);
      checkedPoisonO1ShadowAdd(
          result.productionOutsidePoisonO1ShadowScanFailureReasons[reason],
          result.productionOutsidePoisonO1ShadowScanFailuresByLane[
              laneReason]);
    }
    checkedPoisonO1ShadowAdd(
        poisonO1ShadowScanFailureTotal, laneFailureTotal);
    poisonO1ShadowLaneClosureClean = poisonO1ShadowLaneClosureClean &&
        result.productionOutsidePoisonO1ShadowScanCallsByLane[lane] ==
            laneTerminal &&
        result.productionOutsidePoisonO1ShadowScanReadFailureByLane[lane] ==
            laneFailureTotal;
  }
  checkedPoisonO1ShadowAdd(
      result.productionOutsidePoisonO1ShadowScanLogicalExact,
      result.productionOutsidePoisonO1ShadowScanNoOverlap);
  checkedPoisonO1ShadowAdd(
      result.productionOutsidePoisonO1ShadowScanLogicalExact,
      result.productionOutsidePoisonO1ShadowScanOverlap);
  uint64_t poisonO1ShadowScanTerminal = 0u;
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowScanTerminal,
      result.productionOutsidePoisonO1ShadowScanNoOverlap);
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowScanTerminal,
      result.productionOutsidePoisonO1ShadowScanOverlap);
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowScanTerminal,
      result.productionOutsidePoisonO1ShadowScanReadFailure);
  result.productionOutsidePoisonO1ShadowScanClosureClean =
      poisonO1ShadowArithmeticClean && poisonO1ShadowLaneClosureClean &&
      result.productionOutsidePoisonO1ShadowScanCalls ==
          poisonO1ShadowScanTerminal &&
      result.productionOutsidePoisonO1ShadowScanReadFailure ==
          poisonO1ShadowScanFailureTotal &&
      result.productionOutsidePoisonO1ShadowScanLogicalExact ==
          result.productionOutsidePoisonO1ShadowScanNoOverlap +
              result.productionOutsidePoisonO1ShadowScanOverlap;

  uint64_t poisonO1ShadowUnlockHardTotal = 0u;
  for (size_t i = 0u;
       i < kNativeOutsidePoisonO1UnlockDriftComponentCount; ++i) {
    result.productionOutsidePoisonO1ShadowUnlockDrifts[i] =
        g_counters.productionOutsidePoisonO1ShadowUnlockDrifts[i].load(
            std::memory_order_relaxed);
    result.productionOutsidePoisonO1ShadowUnlockHardFirstCauses[i] =
        g_counters
            .productionOutsidePoisonO1ShadowUnlockHardFirstCauses[i]
            .load(std::memory_order_relaxed);
    checkedPoisonO1ShadowAdd(
        poisonO1ShadowUnlockHardTotal,
        result.productionOutsidePoisonO1ShadowUnlockHardFirstCauses[i]);
  }
  uint64_t poisonO1ShadowPhysicalTotal = 0u;
  for (size_t i = 0u;
       i < result.productionOutsidePoisonO1ShadowPhysicalVerdicts.size();
       ++i) {
    result.productionOutsidePoisonO1ShadowPhysicalVerdicts[i] =
        g_counters.productionOutsidePoisonO1ShadowPhysicalVerdicts[i]
            .load(std::memory_order_relaxed);
    checkedPoisonO1ShadowAdd(
        poisonO1ShadowPhysicalTotal,
        result.productionOutsidePoisonO1ShadowPhysicalVerdicts[i]);
  }
  for (size_t i = 0u; i < kNativeOutsidePoisonO1ShadowMatrixSize; ++i) {
    result.productionOutsidePoisonO1ShadowMatrix[i] =
        g_counters.productionOutsidePoisonO1ShadowMatrix[i].load(
            std::memory_order_relaxed);
    checkedPoisonO1ShadowAdd(
        result.productionOutsidePoisonO1ShadowMatrixTotal,
        result.productionOutsidePoisonO1ShadowMatrix[i]);
    if (i / 3u != i % 3u) {
      checkedPoisonO1ShadowAdd(
          result.productionOutsidePoisonO1ShadowOffDiagonal,
          result.productionOutsidePoisonO1ShadowMatrix[i]);
    }
  }
  result.productionOutsidePoisonO1ShadowLegacyMissedOverlap =
      result.productionOutsidePoisonO1ShadowMatrix[1u];
  uint64_t poisonO1ShadowUnprovableReasonTotal = 0u;
  for (size_t i = 0u;
       i < kNativeOutsidePoisonO1ShadowUnprovableReasonCount; ++i) {
    result.productionOutsidePoisonO1ShadowUnprovableReasons[i] =
        g_counters.productionOutsidePoisonO1ShadowUnprovableReasons[i].load(
            std::memory_order_relaxed);
    checkedPoisonO1ShadowAdd(
        poisonO1ShadowUnprovableReasonTotal,
        result.productionOutsidePoisonO1ShadowUnprovableReasons[i]);
  }
  uint64_t poisonO1ShadowAttemptTerminal = 0u;
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowAttemptTerminal,
      result.productionOutsidePoisonO1ShadowCreated);
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowAttemptTerminal,
      result.productionOutsidePoisonO1ShadowOverflow);
  result.productionOutsidePoisonO1ShadowAttemptClosureClean =
      poisonO1ShadowArithmeticClean &&
      result.productionOutsidePoisonO1ShadowAttempts ==
          poisonO1ShadowAttemptTerminal &&
      (GpuSkinOutsidePoisonO1Enabled(
           g_runtimeConfig.outsidePoisonSidecarPolicy)
           ? result.productionOutsidePoisonO1ShadowAttempts ==
                 result.productionOutsidePoisonScanAttempts
           : result.productionOutsidePoisonO1ShadowAttempts == 0u);
  uint64_t poisonO1ShadowLifetimeTerminal = 0u;
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowLifetimeTerminal,
      result.productionOutsidePoisonO1ShadowSettled);
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowLifetimeTerminal,
      result.productionOutsidePoisonO1ShadowCancelled);
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowLifetimeTerminal,
      result.productionOutsidePoisonO1ShadowResetAborted);
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowLifetimeTerminal,
      result.productionOutsidePoisonO1ShadowActive);
  result.productionOutsidePoisonO1ShadowLifetimeClosureClean =
      poisonO1ShadowArithmeticClean &&
      result.productionOutsidePoisonO1ShadowCreated ==
          poisonO1ShadowLifetimeTerminal;
  uint64_t poisonO1ShadowOverlapColumn = 0u;
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowOverlapColumn,
      result.productionOutsidePoisonO1ShadowMatrix[1u]);
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowOverlapColumn,
      result.productionOutsidePoisonO1ShadowMatrix[4u]);
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowOverlapColumn,
      result.productionOutsidePoisonO1ShadowMatrix[7u]);
  uint64_t poisonO1ShadowComparisonTerminal =
      result.productionOutsidePoisonO1ShadowMatrixTotal;
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowComparisonTerminal,
      result.productionOutsidePoisonO1ShadowComparisonMissing);
  uint64_t poisonO1ShadowOldOverlapToNoOverlapJoint = 0u;
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowOldOverlapToNoOverlapJoint,
      result
          .productionOutsidePoisonO1ShadowOldOverlapToNoOverlapDiscardExact);
  checkedPoisonO1ShadowAdd(
      poisonO1ShadowOldOverlapToNoOverlapJoint,
      result.productionOutsidePoisonO1ShadowOldOverlapToNoOverlapOther);
  result.productionOutsidePoisonO1ShadowDiscardJointClosureClean =
      poisonO1ShadowArithmeticClean &&
      result.productionOutsidePoisonO1ShadowMatrix[3u] ==
          poisonO1ShadowOldOverlapToNoOverlapJoint &&
      result.productionOutsidePoisonO1ShadowOldOverlapToNoOverlapOther == 0u &&
      result
              .productionOutsidePoisonO1ShadowOldOverlapToNoOverlapDiscardExact <=
          result
              .productionOutsidePoisonO1ShadowDirectDiscardOldOverlapRetired &&
      result
              .productionOutsidePoisonO1ShadowDirectDiscardOldOverlapRetired <=
          result.productionOutsidePoisonO1ShadowDirectDiscardNotifications &&
      result
              .productionOutsidePoisonO1ShadowOldOverlapToNoOverlapDiscardExact <=
          result.productionOutsidePoisonO1ShadowOldOverlapFrozen &&
      result.productionOutsidePoisonO1ShadowDirectDiscardNotifications <=
          result.nativeDirectDiscardEvents;
  const size_t poisonO1ShadowUnlockGenerationReason = static_cast<size_t>(
      NativeOutsidePoisonO1ShadowUnprovableReason::UnlockGeneration);
  result.productionOutsidePoisonO1ShadowUnlockDriftClosureClean =
      poisonO1ShadowArithmeticClean &&
      poisonO1ShadowUnlockHardTotal ==
          result.productionOutsidePoisonO1ShadowUnprovableReasons[
              poisonO1ShadowUnlockGenerationReason] &&
      result.productionOutsidePoisonO1ShadowUnlockHardFirstCauses[
          static_cast<size_t>(
              NativeOutsidePoisonO1UnlockDriftComponent::Real)] == 0u &&
      result.productionOutsidePoisonO1ShadowUnlockHardFirstCauses[
          static_cast<size_t>(
              NativeOutsidePoisonO1UnlockDriftComponent::Mapping)] == 0u;
  result.productionOutsidePoisonO1ShadowPhysicalClosureClean =
      poisonO1ShadowArithmeticClean &&
      poisonO1ShadowPhysicalTotal ==
          result.productionOutsidePoisonO1ShadowSettled;
  result.productionOutsidePoisonO1ShadowSettlementClosureClean =
      poisonO1ShadowArithmeticClean &&
      result.productionOutsidePoisonO1ShadowComparable ==
          result.productionOutsidePoisonO1ShadowScanCalls &&
      result.productionOutsidePoisonO1ShadowComparable ==
          poisonO1ShadowComparisonTerminal &&
      result.productionOutsidePoisonO1ShadowComparisonMissing == 0u &&
      result.productionOutsidePoisonO1ShadowUnprovable ==
          poisonO1ShadowUnprovableReasonTotal &&
      result.productionOutsidePoisonO1ShadowUnprovable <=
          result.productionOutsidePoisonO1ShadowSettled &&
      result.productionOutsidePoisonO1ShadowWouldClear ==
          result.productionOutsidePoisonO1ShadowScanOverlap &&
      result.productionOutsidePoisonO1ShadowWouldClear ==
          poisonO1ShadowOverlapColumn &&
      result.productionOutsidePoisonO1ShadowDiscardJointClosureClean &&
      result.productionOutsidePoisonO1ShadowScanClosureClean &&
      result.productionOutsidePoisonO1ShadowUnlockDriftClosureClean &&
      result.productionOutsidePoisonO1ShadowPhysicalClosureClean &&
      result.productionOutsidePoisonO1ShadowAuthority == 0u;

  bool poisonAuthorityArithmeticClean = true;
  const auto checkedPoisonAuthorityAdd = [&poisonAuthorityArithmeticClean](
      uint64_t& destination, uint64_t value) {
    if (value > std::numeric_limits<uint64_t>::max() - destination) {
      poisonAuthorityArithmeticClean = false;
      return;
    }
    destination += value;
  };
  uint64_t poisonAuthorityAttemptTerminal =
      result.productionOutsidePoisonAuthorityCreated;
  checkedPoisonAuthorityAdd(
      poisonAuthorityAttemptTerminal,
      result.productionOutsidePoisonAuthorityOverflow);
  result.productionOutsidePoisonAuthorityAttemptClosureClean =
      poisonAuthorityArithmeticClean &&
      result.productionOutsidePoisonAuthorityAttempts ==
          poisonAuthorityAttemptTerminal;

  uint64_t poisonAuthorityLifetimeTerminal =
      result.productionOutsidePoisonAuthoritySettled;
  checkedPoisonAuthorityAdd(
      poisonAuthorityLifetimeTerminal,
      result.productionOutsidePoisonAuthorityCancelled);
  checkedPoisonAuthorityAdd(
      poisonAuthorityLifetimeTerminal,
      result.productionOutsidePoisonAuthorityResetAborted);
  checkedPoisonAuthorityAdd(
      poisonAuthorityLifetimeTerminal,
      result.productionOutsidePoisonAuthorityActive);
  result.productionOutsidePoisonAuthorityLifetimeClosureClean =
      poisonAuthorityArithmeticClean &&
      result.productionOutsidePoisonAuthorityCreated ==
          poisonAuthorityLifetimeTerminal;

  uint64_t poisonAuthorityLockTerminal =
      result.productionOutsidePoisonAuthorityLockNoOverlap;
  checkedPoisonAuthorityAdd(
      poisonAuthorityLockTerminal,
      result.productionOutsidePoisonAuthorityLockOverlap);
  checkedPoisonAuthorityAdd(
      poisonAuthorityLockTerminal,
      result.productionOutsidePoisonAuthorityLockRejects);
  result.productionOutsidePoisonAuthorityLockClosureClean =
      poisonAuthorityArithmeticClean &&
      result.productionOutsidePoisonAuthorityArmed ==
          poisonAuthorityLockTerminal &&
      result.productionOutsidePoisonAuthorityLockNotifications <=
          result.productionOutsidePoisonAuthorityArmed;

  uint64_t poisonAuthorityKernelTerminal =
      result.productionOutsidePoisonAuthorityKernelReady;
  checkedPoisonAuthorityAdd(
      poisonAuthorityKernelTerminal,
      result.productionOutsidePoisonAuthorityKernelRejects);
  uint64_t poisonAuthorityUnlockTerminal =
      result.productionOutsidePoisonAuthorityUnlockExact;
  checkedPoisonAuthorityAdd(
      poisonAuthorityUnlockTerminal,
      result.productionOutsidePoisonAuthorityUnlockRejects);
  result.productionOutsidePoisonAuthorityExecutionClosureClean =
      poisonAuthorityArithmeticClean &&
      result.productionOutsidePoisonAuthorityArmed ==
          poisonAuthorityKernelTerminal &&
      result.productionOutsidePoisonAuthorityArmed ==
          poisonAuthorityUnlockTerminal &&
      result.productionOutsidePoisonAuthorityNormalReturns <=
          result.productionOutsidePoisonAuthorityArmed &&
      result.productionOutsidePoisonAuthorityUnlockNotifications <=
          result.productionOutsidePoisonAuthorityArmed;

  uint64_t poisonAuthorityCommitTerminal =
      result.productionOutsidePoisonAuthorityCommittedNoOverlap;
  checkedPoisonAuthorityAdd(
      poisonAuthorityCommitTerminal,
      result.productionOutsidePoisonAuthorityCommittedRewrite);
  uint64_t poisonAuthoritySettlementTerminal =
      result.productionOutsidePoisonAuthority;
  checkedPoisonAuthorityAdd(
      poisonAuthoritySettlementTerminal,
      result.productionOutsidePoisonAuthorityRetained);
  result.productionOutsidePoisonAuthoritySettlementClosureClean =
      poisonAuthorityArithmeticClean &&
      result.productionOutsidePoisonAuthority ==
          poisonAuthorityCommitTerminal &&
      result.productionOutsidePoisonAuthorityArmed ==
          poisonAuthoritySettlementTerminal &&
      result.productionOutsidePoisonAuthorityPoisonClears ==
          result.productionOutsidePoisonAuthorityCommittedRewrite;

  uint64_t poisonAuthorityEvidenceMatrixTotal = 0u;
  for (size_t i = 0u;
       i < result.productionOutsidePoisonAuthorityEvidenceMatrix.size(); ++i) {
    result.productionOutsidePoisonAuthorityEvidenceMatrix[i] =
        g_counters.productionOutsidePoisonAuthorityEvidenceMatrix[i].load(
            std::memory_order_relaxed);
    checkedPoisonAuthorityAdd(
        poisonAuthorityEvidenceMatrixTotal,
        result.productionOutsidePoisonAuthorityEvidenceMatrix[i]);
  }
  uint64_t poisonAuthorityEvidenceTerminal =
      result.productionOutsidePoisonAuthorityEvidenceComparable;
  checkedPoisonAuthorityAdd(
      poisonAuthorityEvidenceTerminal,
      result.productionOutsidePoisonAuthorityEvidenceUnprovable);
  result.productionOutsidePoisonAuthorityEvidenceClosureClean =
      poisonAuthorityArithmeticClean &&
      result.productionOutsidePoisonAuthorityEvidenceAttempts <=
          result.productionOutsidePoisonAuthorityAttempts &&
      result.productionOutsidePoisonAuthorityEvidenceAttempts ==
          poisonAuthorityEvidenceTerminal &&
      result.productionOutsidePoisonAuthorityEvidenceComparable ==
          poisonAuthorityEvidenceMatrixTotal &&
      result.productionOutsidePoisonAuthorityEvidenceMismatches == 0u &&
      result.productionOutsidePoisonAuthorityEvidenceAuthority == 0u &&
      result.productionOutsidePoisonAuthorityLegacyBackedAuthority == 0u;

  const auto allZero = [](const auto& values) noexcept {
    return std::all_of(
        values.begin(), values.end(),
        [](uint64_t value) noexcept { return value == 0u; });
  };
  const bool o0CountersZero =
      result.productionOutsidePoisonShadowAttempts == 0u &&
      result.productionOutsidePoisonShadowCreated == 0u &&
      result.productionOutsidePoisonShadowOverflow == 0u &&
      result.productionOutsidePoisonShadowActive == 0u &&
      result.productionOutsidePoisonShadowLockNotifications == 0u &&
      result.productionOutsidePoisonShadowSettled == 0u &&
      result.productionOutsidePoisonShadowCancelled == 0u &&
      result.productionOutsidePoisonShadowResetAborted == 0u &&
      result.productionOutsidePoisonShadowComparable == 0u &&
      result.productionOutsidePoisonShadowUnprovable == 0u &&
      result.productionOutsidePoisonShadowMatrixTotal == 0u &&
      result.productionOutsidePoisonShadowOffDiagonal == 0u &&
      result.productionOutsidePoisonShadowLegacyMissedOverlap == 0u &&
      allZero(result.productionOutsidePoisonShadowMatrix) &&
      allZero(result.productionOutsidePoisonShadowUnprovableReasons);
  const bool o1CountersZero =
      result.productionOutsidePoisonO1ShadowAttempts == 0u &&
      result.productionOutsidePoisonO1ShadowCreated == 0u &&
      result.productionOutsidePoisonO1ShadowOverflow == 0u &&
      result.productionOutsidePoisonO1ShadowActive == 0u &&
      result.productionOutsidePoisonO1ShadowLockNotifications == 0u &&
      result.productionOutsidePoisonO1ShadowKernelNotifications == 0u &&
      result.productionOutsidePoisonO1ShadowUnlockNotifications == 0u &&
      result.productionOutsidePoisonO1ShadowFrozen == 0u &&
      result.productionOutsidePoisonO1ShadowSettled == 0u &&
      result.productionOutsidePoisonO1ShadowCancelled == 0u &&
      result.productionOutsidePoisonO1ShadowResetAborted == 0u &&
      result.productionOutsidePoisonO1ShadowComparable == 0u &&
      result.productionOutsidePoisonO1ShadowUnprovable == 0u &&
      result.productionOutsidePoisonO1ShadowComparisonMissing == 0u &&
      result.productionOutsidePoisonO1ShadowWouldClear == 0u &&
      result.productionOutsidePoisonO1ShadowOldOverlapFrozen == 0u &&
      result.productionOutsidePoisonO1ShadowDirectDiscardNotifications ==
          0u &&
      result
              .productionOutsidePoisonO1ShadowDirectDiscardOldOverlapRetired ==
          0u &&
      result
              .productionOutsidePoisonO1ShadowOldOverlapToNoOverlapDiscardExact ==
          0u &&
      result.productionOutsidePoisonO1ShadowOldOverlapToNoOverlapOther ==
          0u &&
      result.productionOutsidePoisonO1ShadowScanCalls == 0u &&
      result.productionOutsidePoisonO1ShadowScanNoOverlap == 0u &&
      result.productionOutsidePoisonO1ShadowScanOverlap == 0u &&
      result.productionOutsidePoisonO1ShadowScanReadFailure == 0u &&
      result.productionOutsidePoisonO1ShadowScanDifferentTarget == 0u &&
      result.productionOutsidePoisonO1ShadowScanLogicalExact == 0u &&
      result.productionOutsidePoisonO1ShadowStorageDiagnosticMismatch == 0u &&
      result.productionOutsidePoisonO1ShadowRealStorageDrift == 0u &&
      result.productionOutsidePoisonO1ShadowMappingStorageDrift == 0u &&
      result
              .productionOutsidePoisonO1ShadowMapAllocationDiagnosticMismatch ==
          0u &&
      result.productionOutsidePoisonO1ShadowMatrixTotal == 0u &&
      result.productionOutsidePoisonO1ShadowOffDiagonal == 0u &&
      result.productionOutsidePoisonO1ShadowLegacyMissedOverlap == 0u &&
      result.productionOutsidePoisonO1ShadowAuthority == 0u &&
      allZero(result.productionOutsidePoisonO1ShadowScanCallsByLane) &&
      allZero(result.productionOutsidePoisonO1ShadowScanNoOverlapByLane) &&
      allZero(result.productionOutsidePoisonO1ShadowScanOverlapByLane) &&
      allZero(result.productionOutsidePoisonO1ShadowScanReadFailureByLane) &&
      allZero(result.productionOutsidePoisonO1ShadowPreLockMutationByLane) &&
      allZero(
          result.productionOutsidePoisonO1ShadowLockToKernelMutationByLane) &&
      allZero(result.productionOutsidePoisonO1ShadowScanFailuresByLane) &&
      allZero(result.productionOutsidePoisonO1ShadowScanFailureReasons) &&
      allZero(result.productionOutsidePoisonO1ShadowUnlockDrifts) &&
      allZero(result.productionOutsidePoisonO1ShadowUnlockHardFirstCauses) &&
      allZero(result.productionOutsidePoisonO1ShadowPhysicalVerdicts) &&
      allZero(result.productionOutsidePoisonO1ShadowMatrix) &&
      allZero(result.productionOutsidePoisonO1ShadowUnprovableReasons);
  const GpuSkinOutsidePoisonSidecarPolicy sidecarPolicy =
      g_runtimeConfig.outsidePoisonSidecarPolicy;
  const bool policyValid =
      GpuSkinOutsidePoisonSidecarPolicyValid(sidecarPolicy) &&
      !result.productionOutsidePoisonSidecarPolicyInvalid;
  const bool o0PolicyEnabled =
      GpuSkinOutsidePoisonO0Enabled(sidecarPolicy);
  const bool o1PolicyEnabled =
      GpuSkinOutsidePoisonO1Enabled(sidecarPolicy);
  result.productionOutsidePoisonSidecarPolicyClosureClean =
      policyValid &&
      (o0PolicyEnabled
           ? result.productionOutsidePoisonShadowAttemptClosureClean &&
               result.productionOutsidePoisonShadowLifetimeClosureClean &&
               result.productionOutsidePoisonShadowSettlementClosureClean
           : o0CountersZero) &&
      (o1PolicyEnabled
           ? result.productionOutsidePoisonO1ShadowAttemptClosureClean &&
               result.productionOutsidePoisonO1ShadowLifetimeClosureClean &&
               result.productionOutsidePoisonO1ShadowSettlementClosureClean &&
               result
                   .productionOutsidePoisonO1ShadowDiscardJointClosureClean &&
               result.productionOutsidePoisonO1ShadowScanClosureClean &&
               result
                   .productionOutsidePoisonO1ShadowUnlockDriftClosureClean &&
               result.productionOutsidePoisonO1ShadowPhysicalClosureClean &&
               result.productionOutsidePoisonO1ShadowAuthority == 0u
           : o1CountersZero);
  for (size_t i = 0; i < kNativeOutsideDipFastRejectReasonCount; ++i) {
    result.productionOutsideDipFastEarlyRejects[i] =
        g_counters.productionOutsideDipFastEarlyRejects[i].load(
            std::memory_order_relaxed);
    result.productionOutsideDipFastLateRejects[i] =
        g_counters.productionOutsideDipFastLateRejects[i].load(
            std::memory_order_relaxed);
    result.productionOutsideDipFastEarlyAttempts +=
        result.productionOutsideDipFastEarlyRejects[i];
    result.productionOutsideDipFastLateAttempts +=
        result.productionOutsideDipFastLateRejects[i];
  }
  result.productionOutsideDipFastEarlyAttempts +=
      result.productionOutsideDipFastEarlySuccesses;
  result.productionOutsideDipFastLateAttempts +=
      result.productionOutsideDipFastLateSuccesses;
  for (size_t i = 0;
       i < kNativeOutsideDipFastLocalRejectReasonCount; ++i) {
    result.productionOutsideDipFastEarlyLocalRejects[i] =
        g_counters.productionOutsideDipFastEarlyLocalRejects[i].load(
            std::memory_order_relaxed);
    result.productionOutsideDipFastLateLocalRejects[i] =
        g_counters.productionOutsideDipFastLateLocalRejects[i].load(
            std::memory_order_relaxed);
  }
  bool outsideAdmissionArithmeticClean = true;
  const auto checkedOutsideAdd = [&outsideAdmissionArithmeticClean](
      uint64_t& destination, uint64_t value) {
    if (value > std::numeric_limits<uint64_t>::max() - destination) {
      outsideAdmissionArithmeticClean = false;
      return;
    }
    destination += value;
  };
  for (size_t i = 0u; i < kNativeOutsideUploadRejectReasonCount; ++i) {
    result.productionOutsideAdmissionRejectNoPoison[i] =
        g_counters.productionOutsideAdmissionRejectNoPoison[i].load(
            std::memory_order_relaxed);
    result.productionOutsideAdmissionRejectWithPoison[i] =
        g_counters.productionOutsideAdmissionRejectWithPoison[i].load(
            std::memory_order_relaxed);
    checkedOutsideAdd(
        result.productionOutsideAdmissionRejectNoPoisonTotal,
        result.productionOutsideAdmissionRejectNoPoison[i]);
    checkedOutsideAdd(
        result.productionOutsideAdmissionRejectWithPoisonTotal,
        result.productionOutsideAdmissionRejectWithPoison[i]);
  }
  checkedOutsideAdd(
      result.productionOutsideAdmissionAcceptedTotal,
      result.productionOutsideAdmissionAcceptedNoPoison);
  checkedOutsideAdd(
      result.productionOutsideAdmissionAcceptedTotal,
      result.productionOutsideAdmissionAcceptedWithPoison);
  checkedOutsideAdd(
      result.productionOutsideAdmissionRejectTotal,
      result.productionOutsideAdmissionRejectNoPoisonTotal);
  checkedOutsideAdd(
      result.productionOutsideAdmissionRejectTotal,
      result.productionOutsideAdmissionRejectWithPoisonTotal);
  checkedOutsideAdd(
      result.productionOutsideAdmissionAttemptTotal,
      result.productionOutsideAdmissionAcceptedTotal);
  checkedOutsideAdd(
      result.productionOutsideAdmissionAttemptTotal,
      result.productionOutsideAdmissionRejectTotal);

  uint64_t trackedResolvedOutside =
      result.productionOutsideAdmissionAttemptTotal;
  const auto checkedOutsideSubtract = [&outsideAdmissionArithmeticClean](
      uint64_t& destination, uint64_t value) {
    if (value > destination) {
      outsideAdmissionArithmeticClean = false;
      return;
    }
    destination -= value;
  };
  if (outsideAdmissionArithmeticClean) {
    checkedOutsideSubtract(
        trackedResolvedOutside,
        result.productionOutsideAdmissionCancellations);
    checkedOutsideSubtract(
        trackedResolvedOutside,
        result.productionOutsideAdmissionLifecycleExcluded);
    checkedOutsideSubtract(
        trackedResolvedOutside,
        result.productionOutsideAdmissionTrackedResolvedInside);
  }
  if (outsideAdmissionArithmeticClean) {
    result.productionOutsideAdmissionTrackedResolvedOutside =
        trackedResolvedOutside;
    result.productionOutsideAdmissionResolvedExpectedOutside =
        trackedResolvedOutside;
    checkedOutsideAdd(
        result.productionOutsideAdmissionResolvedExpectedOutside,
        result.productionOutsideAdmissionUntrackedResolvedOutside);
  }
  if (!outsideAdmissionArithmeticClean) {
    result.productionOutsideAdmissionTrackedResolvedOutside = 0u;
    result.productionOutsideAdmissionResolvedExpectedOutside = 0u;
  }
  result.productionOutsideAdmissionUnknownHardZero =
      result.productionOutsideAdmissionRejectNoPoison[
          static_cast<size_t>(NativeOutsideUploadRejectReason::Unknown)] ==
          0u &&
      result.productionOutsideAdmissionRejectWithPoison[
          static_cast<size_t>(NativeOutsideUploadRejectReason::Unknown)] ==
          0u;
  const auto copySampledTiming = [](
      NativeSampledRawTiming& destination,
      const AtomicNativeSampledRawTiming& source) {
    destination.calls = source.calls.load(std::memory_order_relaxed);
    destination.ticks = source.ticks.load(std::memory_order_relaxed);
    destination.maxTicks = source.maxTicks.load(std::memory_order_relaxed);
  };
  result.productionTimingSamplePeriod =
      uint32_t(kNativeProductionTimingSamplePeriod);
  result.productionTimingSamplePhase =
      uint32_t(kNativeProductionTimingSamplePhase);
  copySampledTiming(
      result.productionOuterAdmissionAcceptedTiming,
      g_counters.productionOuterAdmissionAcceptedTiming);
  copySampledTiming(
      result.productionOuterAdmissionRejectedTiming,
      g_counters.productionOuterAdmissionRejectedTiming);
  copySampledTiming(
      result.productionOuterFastInclusiveTiming,
      g_counters.productionOuterFastInclusiveTiming);
  copySampledTiming(
      result.productionOuterFastBodyTiming,
      g_counters.productionOuterFastBodyTiming);
  copySampledTiming(
      result.productionOuterFastCompleteTiming,
      g_counters.productionOuterFastCompleteTiming);
  copySampledTiming(
      result.productionOuterFastCancelTiming,
      g_counters.productionOuterFastCancelTiming);
  copySampledTiming(
      result.productionKernelInclusiveTiming,
      g_counters.productionKernelInclusiveTiming);
  copySampledTiming(
      result.productionKernelEvaluateTiming,
      g_counters.productionKernelEvaluateTiming);
  copySampledTiming(
      result.productionKernelOriginalTiming,
      g_counters.productionKernelOriginalTiming);
  copySampledTiming(
      result.productionKernelNotifyTiming,
      g_counters.productionKernelNotifyTiming);
  for (size_t i = 0u; i < kNativeBridgeCallbackKindCount; ++i) {
    copySampledTiming(
        result.productionCallbackPinEnterTiming[i],
        g_counters.productionCallbackPinEnterTiming[i]);
    copySampledTiming(
        result.productionCallbackBodyTiming[i],
        g_counters.productionCallbackBodyTiming[i]);
    copySampledTiming(
        result.productionCallbackPinLeaveTiming[i],
        g_counters.productionCallbackPinLeaveTiming[i]);
  }
  copySampledTiming(
      result.productionOuterFallbackInclusiveTiming,
      g_counters.productionOuterFallbackInclusiveTiming);
  copySampledTiming(
      result.productionOuterFallbackBeginTiming,
      g_counters.productionOuterFallbackBeginTiming);
  copySampledTiming(
      result.productionOuterFallbackBodyTiming,
      g_counters.productionOuterFallbackBodyTiming);
  copySampledTiming(
      result.productionOuterFallbackCompleteTiming,
      g_counters.productionOuterFallbackCompleteTiming);
  copySampledTiming(
      result.productionFlushRootTiming,
      g_counters.productionFlushRootTiming);
  copySampledTiming(
      result.productionDispatchSemanticLookupTiming,
      g_counters.productionDispatchSemanticLookupTiming);
  copySampledTiming(
      result.productionDispatchBeginRootTiming,
      g_counters.productionDispatchBeginRootTiming);
  copySampledTiming(
      result.productionDispatchEndRootTiming,
      g_counters.productionDispatchEndRootTiming);
  copySampledTiming(
      result.productionSemanticInclusiveTiming,
      g_counters.productionSemanticInclusiveTiming);
  copySampledTiming(
      result.productionSemanticOriginalTiming,
      g_counters.productionSemanticOriginalTiming);
  copySampledTiming(
      result.productionDipDeviceRootOutsideTiming,
      g_counters.productionDipDeviceRootOutsideTiming);
  copySampledTiming(
      result.productionDipDeviceRootNoUploadTiming,
      g_counters.productionDipDeviceRootNoUploadTiming);
  copySampledTiming(
      result.productionDipDeviceRootCorrelatedTiming,
      g_counters.productionDipDeviceRootCorrelatedTiming);
  copySampledTiming(
      result.productionDipBridgeOutsideTiming,
      g_counters.productionDipBridgeOutsideTiming);
  copySampledTiming(
      result.productionDipBridgeNoUploadTiming,
      g_counters.productionDipBridgeNoUploadTiming);
  copySampledTiming(
      result.productionDipBridgeCorrelatedTiming,
      g_counters.productionDipBridgeCorrelatedTiming);
  copySampledTiming(
      result.productionDipResolveOutsideTiming,
      g_counters.productionDipResolveOutsideTiming);
  copySampledTiming(
      result.productionDipResolveNoUploadTiming,
      g_counters.productionDipResolveNoUploadTiming);
  copySampledTiming(
      result.productionDipResolveCorrelatedTiming,
      g_counters.productionDipResolveCorrelatedTiming);
  uint64_t acceptedClassCalls = 0u;
  uint64_t acceptedClassTicks = 0u;
  uint64_t acceptedClassMaxTicks = 0u;
  uint64_t completeClassCalls = 0u;
  uint64_t completeClassTicks = 0u;
  uint64_t completeClassMaxTicks = 0u;
  for (size_t i = 0u;
       i < kNativeOutsideUploadAdmissionClassCount; ++i) {
    copySampledTiming(
        result.productionOuterAdmissionAcceptedClassTiming[i],
        g_counters.productionOuterAdmissionAcceptedClassTiming[i]);
    copySampledTiming(
        result.productionOuterFastCompleteClassTiming[i],
        g_counters.productionOuterFastCompleteClassTiming[i]);
    acceptedClassCalls +=
        result.productionOuterAdmissionAcceptedClassTiming[i].calls;
    acceptedClassTicks +=
        result.productionOuterAdmissionAcceptedClassTiming[i].ticks;
    acceptedClassMaxTicks = std::max(
        acceptedClassMaxTicks,
        result.productionOuterAdmissionAcceptedClassTiming[i].maxTicks);
    completeClassCalls +=
        result.productionOuterFastCompleteClassTiming[i].calls;
    completeClassTicks +=
        result.productionOuterFastCompleteClassTiming[i].ticks;
    completeClassMaxTicks = std::max(
        completeClassMaxTicks,
        result.productionOuterFastCompleteClassTiming[i].maxTicks);
  }
  uint64_t fallbackReasonCalls = 0u;
  uint64_t fallbackReasonTicks = 0u;
  uint64_t fallbackReasonMaxTicks = 0u;
  for (size_t i = 0u; i < kNativeOutsideUploadRejectReasonCount; ++i) {
    copySampledTiming(
        result.productionOuterFallbackReasonTiming[i],
        g_counters.productionOuterFallbackReasonTiming[i]);
    fallbackReasonCalls += result.productionOuterFallbackReasonTiming[i].calls;
    fallbackReasonTicks += result.productionOuterFallbackReasonTiming[i].ticks;
    fallbackReasonMaxTicks = std::max(
        fallbackReasonMaxTicks,
        result.productionOuterFallbackReasonTiming[i].maxTicks);
  }
  copySampledTiming(
      result.productionOuterDispatchSealAdmissionTiming,
      g_counters.productionOuterDispatchSealAdmissionTiming);
  copySampledTiming(
      result.productionOuterDispatchSealInclusiveTiming,
      g_counters.productionOuterDispatchSealInclusiveTiming);
  copySampledTiming(
      result.productionOuterDispatchSealBodyTiming,
      g_counters.productionOuterDispatchSealBodyTiming);
  copySampledTiming(
      result.productionOuterDispatchSealCompleteTiming,
      g_counters.productionOuterDispatchSealCompleteTiming);
  copySampledTiming(
      result.productionOuterDispatchSealCancelTiming,
      g_counters.productionOuterDispatchSealCancelTiming);
  const uint32_t productionWritersAfter =
      g_productionTimingWriters.load(std::memory_order_acquire);
  const uint64_t productionWritesCompletedAfter =
      g_productionTimingWritesCompleted.load(std::memory_order_acquire);
  const uint64_t productionWritesStartedAfter =
      g_productionTimingWritesStarted.load(std::memory_order_acquire);
  result.productionTimingWritesStarted = productionWritesStartedAfter;
  result.productionTimingWritesCompleted = productionWritesCompletedAfter;
  result.productionTimingWriters = productionWritersAfter;
  result.productionTimingSnapshotPending =
      productionWritersBefore != 0u || productionWritersAfter != 0u ||
      productionWritesStartedBefore != productionWritesCompletedBefore ||
      productionWritesStartedAfter != productionWritesCompletedAfter ||
      productionWritesStartedBefore != productionWritesStartedAfter ||
      productionWritesCompletedBefore != productionWritesCompletedAfter;
  result.nativePoisonOutstanding =
      g_nativePoisonOutstandingRanges.load(std::memory_order_relaxed);
  const uint32_t telemetryPendingAfter =
      g_nativeTelemetryDeltaPending.load(std::memory_order_acquire);
  const bool telemetryFaultedAfter =
      g_nativeTelemetryDeltaFaulted.load(std::memory_order_acquire);
  const uint64_t telemetryFlushSequenceAfter =
      g_counters.telemetryFlushes.load(std::memory_order_acquire);
  result.telemetryFlushes = telemetryFlushSequenceAfter;
  result.telemetryDeltaPending = telemetryPendingBefore != 0u ||
      telemetryPendingAfter != 0u ||
      telemetryFlushSequenceBefore != telemetryFlushSequenceAfter;
  result.telemetryDeltaFaulted =
      telemetryFaultedBefore || telemetryFaultedAfter;
  const NativeSampledRawTiming& unknownFallbackTiming =
      result.productionOuterFallbackReasonTiming[
          static_cast<size_t>(NativeOutsideUploadRejectReason::Unknown)];
  const bool productionLightPolicy =
      g_runtimeConfig.mode == GpuSkinMode::Bypass &&
      !g_runtimeConfig.fullDiagnostics;
  result.productionOuterAdmissionAliasSnapshotAvailable =
      productionLightPolicy && !result.productionTimingSnapshotPending;
  result.productionOuterFallbackReasonUnknownHardZero =
      result.productionOuterAdmissionAliasSnapshotAvailable &&
      unknownFallbackTiming.calls == 0u && unknownFallbackTiming.ticks == 0u &&
      unknownFallbackTiming.maxTicks == 0u;
  result.productionOuterAdmissionAcceptedClassClosureClean =
      result.productionOuterAdmissionAliasSnapshotAvailable &&
      acceptedClassCalls ==
          result.productionOuterAdmissionAcceptedTiming.calls &&
      acceptedClassTicks ==
          result.productionOuterAdmissionAcceptedTiming.ticks &&
      acceptedClassMaxTicks ==
          result.productionOuterAdmissionAcceptedTiming.maxTicks;
  result.productionOuterFastCompleteClassClosureClean =
      result.productionOuterAdmissionAliasSnapshotAvailable &&
      completeClassCalls == result.productionOuterFastCompleteTiming.calls &&
      completeClassTicks == result.productionOuterFastCompleteTiming.ticks &&
      completeClassMaxTicks ==
          result.productionOuterFastCompleteTiming.maxTicks;
  result.productionOuterFallbackReasonClosureClean =
      result.productionOuterAdmissionAliasSnapshotAvailable &&
      fallbackReasonCalls ==
          result.productionOuterFallbackInclusiveTiming.calls &&
      fallbackReasonTicks ==
          result.productionOuterFallbackInclusiveTiming.ticks &&
      fallbackReasonMaxTicks ==
          result.productionOuterFallbackInclusiveTiming.maxTicks &&
      result.productionOuterFallbackReasonUnknownHardZero;
  result.productionOutsideAdmissionUnknownHardZero =
      productionLightPolicy &&
      result.productionOutsideAdmissionUnknownHardZero &&
      unknownFallbackTiming.calls == 0u && unknownFallbackTiming.ticks == 0u &&
      unknownFallbackTiming.maxTicks == 0u;
  result.productionOutsideAdmissionSnapshotAvailable =
      productionLightPolicy && CurrentThreadIsObservedRenderThread() &&
      CurrentThreadTlsQuiescent() &&
      g_transactionIngressEnabled.load(std::memory_order_seq_cst) &&
      g_bypassEnabled.load(std::memory_order_acquire) &&
      result.telemetryDeltaPending == 0u && !result.telemetryDeltaFaulted &&
      outsideAdmissionArithmeticClean &&
      result.productionOutsideAdmissionCancellations == 0u &&
      result.productionOutsideAdmissionLifecycleExcluded == 0u &&
      result.productionOutsideAdmissionUnknownHardZero;
  result.productionOutsideAdmissionAcceptedResolutionClean =
      result.productionOutsideAdmissionSnapshotAvailable &&
      result.productionOutsideAdmissionAcceptedTotal ==
          result.productionOutsideNativeFastPath;
  result.productionOutsideAdmissionOutsideClosureClean =
      result.productionOutsideAdmissionSnapshotAvailable &&
      result.productionOutsideAdmissionResolvedExpectedOutside ==
          result.uploadsOutsideDispatch;
  return result;
}

void RecordNativeOuterUploadTiming(uint64_t ticks) noexcept {
  if (!g_runtimeConfig.fullDiagnostics)
    return;
  g_counters.outerUploadTimingCalls.fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.outerUploadTimingTicks.fetch_add(
      ticks, std::memory_order_relaxed);
  UpdateMax(g_counters.outerUploadTimingMaxTicks, ticks);
}

void RecordNativeOriginalKernelTiming(uint64_t ticks) noexcept {
  if (!g_runtimeConfig.fullDiagnostics)
    return;
  g_counters.originalKernelTimingCalls.fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.originalKernelTimingTicks.fetch_add(
      ticks, std::memory_order_relaxed);
  UpdateMax(g_counters.originalKernelTimingMaxTicks, ticks);
}

bool NativeBridgeFullDiagnosticsEnabled() noexcept {
  return g_runtimeConfig.fullDiagnostics;
}

bool NativeBridgeProductionLightTimingEnabled() noexcept {
  return ProductionLightTimingEnabled();
}

void RecordNativeProductionTiming(
    NativeProductionTimingStage stage, uint64_t ticks) noexcept {
  const NativeProductionTimingEntry entry = {stage, ticks};
  RecordNativeProductionTimingBatch(&entry, 1u);
}

void RecordNativeProductionTimingBatch(
    const NativeProductionTimingEntry* entries, size_t count) noexcept {
  if (!ProductionLightTimingEnabled() || entries == nullptr || count == 0u)
    return;
  ProductionTimingWriteGuard writeGuard(true);
  for (size_t i = 0u; i < count; ++i) {
    AtomicNativeSampledRawTiming* timing = nullptr;
    switch (entries[i].stage) {
      case NativeProductionTimingStage::OuterAdmissionAccepted:
        timing = &g_counters.productionOuterAdmissionAcceptedTiming;
        break;
      case NativeProductionTimingStage::OuterAdmissionRejected:
        timing = &g_counters.productionOuterAdmissionRejectedTiming;
        break;
      case NativeProductionTimingStage::OuterFastInclusive:
        timing = &g_counters.productionOuterFastInclusiveTiming;
        break;
      case NativeProductionTimingStage::OuterFastBody:
        timing = &g_counters.productionOuterFastBodyTiming;
        break;
      case NativeProductionTimingStage::OuterFastComplete:
        timing = &g_counters.productionOuterFastCompleteTiming;
        break;
      case NativeProductionTimingStage::OuterFastCancel:
        timing = &g_counters.productionOuterFastCancelTiming;
        break;
      case NativeProductionTimingStage::KernelInclusive:
        timing = &g_counters.productionKernelInclusiveTiming;
        break;
      case NativeProductionTimingStage::KernelEvaluate:
        timing = &g_counters.productionKernelEvaluateTiming;
        break;
      case NativeProductionTimingStage::KernelOriginal:
        timing = &g_counters.productionKernelOriginalTiming;
        break;
      case NativeProductionTimingStage::KernelNotify:
        timing = &g_counters.productionKernelNotifyTiming;
        break;
      case NativeProductionTimingStage::OuterFallbackInclusive:
        timing = &g_counters.productionOuterFallbackInclusiveTiming;
        break;
      case NativeProductionTimingStage::OuterFallbackBegin:
        timing = &g_counters.productionOuterFallbackBeginTiming;
        break;
      case NativeProductionTimingStage::OuterFallbackBody:
        timing = &g_counters.productionOuterFallbackBodyTiming;
        break;
      case NativeProductionTimingStage::OuterFallbackComplete:
        timing = &g_counters.productionOuterFallbackCompleteTiming;
        break;
      case NativeProductionTimingStage::FlushRoot:
        timing = &g_counters.productionFlushRootTiming;
        break;
      case NativeProductionTimingStage::DispatchSemanticLookup:
        timing = &g_counters.productionDispatchSemanticLookupTiming;
        break;
      case NativeProductionTimingStage::DispatchBeginRoot:
        timing = &g_counters.productionDispatchBeginRootTiming;
        break;
      case NativeProductionTimingStage::DispatchEndRoot:
        timing = &g_counters.productionDispatchEndRootTiming;
        break;
      case NativeProductionTimingStage::SemanticInclusive:
        timing = &g_counters.productionSemanticInclusiveTiming;
        break;
      case NativeProductionTimingStage::SemanticOriginal:
        timing = &g_counters.productionSemanticOriginalTiming;
        break;
      case NativeProductionTimingStage::DipDeviceRootOutside:
        timing = &g_counters.productionDipDeviceRootOutsideTiming;
        break;
      case NativeProductionTimingStage::DipDeviceRootNoUpload:
        timing = &g_counters.productionDipDeviceRootNoUploadTiming;
        break;
      case NativeProductionTimingStage::DipDeviceRootCorrelated:
        timing = &g_counters.productionDipDeviceRootCorrelatedTiming;
        break;
      case NativeProductionTimingStage::DipBridgeOutside:
        timing = &g_counters.productionDipBridgeOutsideTiming;
        break;
      case NativeProductionTimingStage::DipBridgeNoUpload:
        timing = &g_counters.productionDipBridgeNoUploadTiming;
        break;
      case NativeProductionTimingStage::DipBridgeCorrelated:
        timing = &g_counters.productionDipBridgeCorrelatedTiming;
        break;
      case NativeProductionTimingStage::DipResolveOutside:
        timing = &g_counters.productionDipResolveOutsideTiming;
        break;
      case NativeProductionTimingStage::DipResolveNoUpload:
        timing = &g_counters.productionDipResolveNoUploadTiming;
        break;
      case NativeProductionTimingStage::DipResolveCorrelated:
        timing = &g_counters.productionDipResolveCorrelatedTiming;
        break;
      case NativeProductionTimingStage::OuterAdmissionAcceptedClass:
        if (entries[i].bucket <
            kNativeOutsideUploadAdmissionClassCount) {
          timing = &g_counters.productionOuterAdmissionAcceptedClassTiming[
              entries[i].bucket];
        }
        break;
      case NativeProductionTimingStage::OuterFastCompleteClass:
        if (entries[i].bucket <
            kNativeOutsideUploadAdmissionClassCount) {
          timing = &g_counters.productionOuterFastCompleteClassTiming[
              entries[i].bucket];
        }
        break;
      case NativeProductionTimingStage::OuterFallbackReason:
        if (entries[i].bucket != static_cast<uint8_t>(
                NativeOutsideUploadRejectReason::Unknown) &&
            entries[i].bucket < kNativeOutsideUploadRejectReasonCount) {
          timing = &g_counters.productionOuterFallbackReasonTiming[
              entries[i].bucket];
        }
        break;
      case NativeProductionTimingStage::OuterDispatchSealAdmission:
        timing = &g_counters.productionOuterDispatchSealAdmissionTiming;
        break;
      case NativeProductionTimingStage::OuterDispatchSealInclusive:
        timing = &g_counters.productionOuterDispatchSealInclusiveTiming;
        break;
      case NativeProductionTimingStage::OuterDispatchSealBody:
        timing = &g_counters.productionOuterDispatchSealBodyTiming;
        break;
      case NativeProductionTimingStage::OuterDispatchSealComplete:
        timing = &g_counters.productionOuterDispatchSealCompleteTiming;
        break;
      case NativeProductionTimingStage::OuterDispatchSealCancel:
        timing = &g_counters.productionOuterDispatchSealCancelTiming;
        break;
    }
    if (timing != nullptr)
      RecordAtomicSampledRawTiming(*timing, entries[i].ticks);
  }
}

NativePoisonDiagnosticSnapshot GetNativePoisonDiagnosticSnapshot() {
  return t_state.poisonDiagnostics;
}

NativeBridgeCallbackStatus RegisterNativeBridgeCallbacks(
    const void* owner, const NativeBridgeCallbacks* callbacks) {
  if (owner == nullptr || callbacks == nullptr)
    return NativeBridgeCallbackStatus::InvalidArgument;
  if (callbacks->abiVersion != kNativeBridgeCallbackAbi ||
      (callbacks->structSize != 0u &&
       callbacks->structSize < sizeof(NativeBridgeCallbacks))) {
    return NativeBridgeCallbackStatus::AbiMismatch;
  }
  if (t_callbackPinDepth != 0u)
    return NativeBridgeCallbackStatus::ReentrantOperation;
  if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      !g_callbackIngressEnabled.load(std::memory_order_acquire))
    return NativeBridgeCallbackStatus::Disabled;

  std::lock_guard<std::mutex> lock(g_callbackRegistrationMutex);
  if (g_retiringCallbacks != nullptr) {
    return g_callbackOwner != owner
        ? NativeBridgeCallbackStatus::OwnerMismatch
        : NativeBridgeCallbackStatus::QuiescencePending;
  }

  const NativeBridgeCallbacks* current =
      g_callbacks.load(std::memory_order_acquire);
  if (current != nullptr) {
    if (g_callbackOwner != owner)
      return NativeBridgeCallbackStatus::OwnerMismatch;
    return current == callbacks
        ? NativeBridgeCallbackStatus::Success
        : NativeBridgeCallbackStatus::ExpectedPointerMismatch;
  }
  if (g_callbackOwner != nullptr && g_callbackOwner != owner)
    return NativeBridgeCallbackStatus::OwnerMismatch;

  const NativeBridgeCallbacks* expected = nullptr;
  if (!g_callbacks.compare_exchange_strong(
          expected, callbacks, std::memory_order_seq_cst,
          std::memory_order_seq_cst)) {
    return NativeBridgeCallbackStatus::ExpectedPointerMismatch;
  }
  g_callbackOwner = owner;
  g_callbackOwnerRegistered.store(true, std::memory_order_release);
  return NativeBridgeCallbackStatus::Success;
}

NativeBridgeCallbackStatus UnregisterNativeBridgeCallbacks(
    const void* owner, const NativeBridgeCallbacks* expectedCallbacks,
    uint32_t timeoutMs) {
  if (owner == nullptr || expectedCallbacks == nullptr)
    return NativeBridgeCallbackStatus::InvalidArgument;
  if (t_callbackPinDepth != 0u)
    return NativeBridgeCallbackStatus::ReentrantOperation;

  std::lock_guard<std::mutex> lock(g_callbackRegistrationMutex);
  const NativeBridgeCallbacks* current =
      g_callbacks.load(std::memory_order_acquire);
  if (current == nullptr && g_retiringCallbacks == nullptr &&
      g_callbackOwner == nullptr) {
    return NativeBridgeCallbackStatus::Success;
  }
  if (g_callbackOwner != owner)
    return NativeBridgeCallbackStatus::OwnerMismatch;

  if (g_retiringCallbacks != nullptr) {
    if (g_retiringCallbacks != expectedCallbacks)
      return NativeBridgeCallbackStatus::ExpectedPointerMismatch;
  } else {
    if (current != expectedCallbacks)
      return NativeBridgeCallbackStatus::ExpectedPointerMismatch;
    const NativeBridgeCallbacks* expected = expectedCallbacks;
    if (!g_callbacks.compare_exchange_strong(
            expected, nullptr, std::memory_order_seq_cst,
            std::memory_order_seq_cst)) {
      return NativeBridgeCallbackStatus::ExpectedPointerMismatch;
    }
    g_retiringCallbacks = expectedCallbacks;
  }

  if (!WaitForCallbackPinsToDrain(timeoutMs))
    return NativeBridgeCallbackStatus::Timeout;
  g_retiringCallbacks = nullptr;
  g_callbackOwner = nullptr;
  g_callbackOwnerRegistered.store(false, std::memory_order_release);
  return NativeBridgeCallbackStatus::Success;
}

NativeBridgeResetResult RequestNativeBridgeReset(
    NativePoisonLedgerResetReason reason) {
  const uint64_t generation = RequestBridgeResetGeneration(reason);
  ApplyPendingResetFailClosed();
  NativeBridgeResetResult result = ProcessPendingBridgeReset();
  result.reason = reason;
  result.requestedGeneration = generation;
  return result;
}

bool AcknowledgeNativeBridgeOwnerEpochRetired(uint64_t resetGeneration) {
  bool accepted = false;
  {
    ResetProtocolLockGuard protocolLock;
    const uint64_t requested =
        g_resetRequestedGeneration.load(std::memory_order_acquire);
    if (resetGeneration != 0u && resetGeneration == requested) {
      g_ownerRetiredGeneration.store(
          resetGeneration, std::memory_order_release);
      accepted = true;
    }
  }
  if (!accepted) {
    g_counters.resetOwnerAckMismatches.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }
  g_counters.resetOwnerAcks.fetch_add(1u, std::memory_order_relaxed);
  ProcessPendingBridgeReset();
  return true;
}

void NotifyNativeVertexResourceRetired(
    uintptr_t commonResource, uint64_t resourceGeneration) noexcept {
  if (commonResource == 0u || resourceGeneration == 0u) {
    g_counters.nativeRetirementInvalidEvents.fetch_add(
        1u, std::memory_order_relaxed);
    g_retirementQueueFaulted.store(true, std::memory_order_release);
    g_bypassEnabled.store(false, std::memory_order_release);
    return;
  }
  // While bypass is open, retain events even before outstanding increments:
  // a host proof and the final protocol-locked commit have a narrow interval.
  // Outside that interval, zero outstanding means no range can consume this
  // exact retirement event.
  if (g_nativePoisonOutstandingRanges.load(std::memory_order_acquire) == 0u &&
      !g_bypassEnabled.load(std::memory_order_acquire)) {
    return;
  }

  bool queued = false;
  bool duplicate = false;
  ::AcquireSRWLockExclusive(&g_retirementQueueLock);
  for (size_t i = 0u; i < g_retirementEventCount; ++i) {
    const NativeResourceRetirementEvent& event = g_retirementEvents[
        (g_retirementEventHead + i) % g_retirementEvents.size()];
    if (event.commonResource == commonResource &&
        event.resourceGeneration == resourceGeneration) {
      duplicate = true;
      break;
    }
  }
  if (!duplicate && g_retirementEventCount < g_retirementEvents.size()) {
    const size_t tail = (g_retirementEventHead + g_retirementEventCount) %
        g_retirementEvents.size();
    g_retirementEvents[tail] = {commonResource, resourceGeneration};
    ++g_retirementEventCount;
    g_retirementEventsPending.store(
        g_retirementEventCount, std::memory_order_release);
    queued = true;
  }
  ::ReleaseSRWLockExclusive(&g_retirementQueueLock);

  if (queued) {
    g_counters.nativeRetirementEventsPublished.fetch_add(
        1u, std::memory_order_relaxed);
    return;
  }
  if (duplicate)
    return;

  g_counters.nativeRetirementQueueOverflows.fetch_add(
      1u, std::memory_order_relaxed);
  g_retirementQueueFaulted.store(true, std::memory_order_release);
  g_bypassEnabled.store(false, std::memory_order_release);
}

uint32_t NotifyNativeVertexDirectDiscard(
    uintptr_t commonResource, uint64_t resourceGeneration,
    uint64_t previousMapAllocationGeneration,
    uint64_t newMapAllocationGeneration, bool directMapping) noexcept {
  if (!NativeBridgeHooksEnabled())
    return 0u;

  const bool generationAdvanced = NonZeroGenerationAdvancedOnce(
      previousMapAllocationGeneration, newMapAllocationGeneration);
  if (!directMapping || commonResource == 0u || resourceGeneration == 0u ||
      previousMapAllocationGeneration == 0u ||
      newMapAllocationGeneration == 0u || !generationAdvanced ||
      !CurrentThreadIsObservedRenderThread()) {
    g_counters.nativeDirectDiscardInvalid.fetch_add(
        1u, std::memory_order_relaxed);
    return 0u;
  }

  g_counters.nativeDirectDiscardEvents.fetch_add(
      1u, std::memory_order_relaxed);

  // Joint attribution is latched only on the current report-only TLS probe and
  // only before its successful Lock notification. The established discard
  // retirement below remains authoritative and is unchanged by this sidecar.
  NativeOutsidePoisonShadowProbe* discardProbe = nullptr;
  if (OutsidePoisonO1SidecarEnabled() ||
      g_nativeOutsidePoisonAuthorityActive.load(
          std::memory_order_acquire) != 0u) {
    NativeOutsidePoisonShadowState& shadowState = t_outsidePoisonShadow;
    if (shadowState.overflowDepth == 0u && shadowState.depth != 0u) {
      NativeOutsidePoisonShadowProbe& probe =
          shadowState.probes[shadowState.depth - 1u];
      const bool o1Enabled = OutsidePoisonProbeCollectsO1(probe);
      const bool authorityEnabled = probe.authorityArmed;
      if ((o1Enabled || authorityEnabled) && !probe.frozen &&
          probe.lockNotifications == 0u) {
        ++probe.directDiscardNotifications;
        if (o1Enabled) {
          g_counters
              .productionOutsidePoisonO1ShadowDirectDiscardNotifications
              .fetch_add(1u, std::memory_order_relaxed);
        }
        if (probe.directDiscardNotifications == 1u) {
          NativeOutsidePoisonDirectDiscardObservation& observation =
              probe.directDiscardObservation;
          observation.commonResource = commonResource;
          observation.resourceGeneration = resourceGeneration;
          observation.previousMapAllocationGeneration =
              previousMapAllocationGeneration;
          observation.newMapAllocationGeneration = newMapAllocationGeneration;
          observation.mutationGenerationBefore =
              t_poisonMutationGeneration;
          observation.poisonCountBefore =
              static_cast<uint32_t>(t_state.poisonLedger.count);
          observation.poisonOutstandingBefore =
              g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
          discardProbe = &probe;
        }
      }
    }
  }

  NativePoisonLedger& ledger = t_state.poisonLedger;
  uint32_t cleared = 0u;
  for (size_t i = 0u; i < ledger.count;) {
    const NativeVertexResourceIdentity& identity =
        ledger.ranges[i].vertexOutputProof.resource;
    if (identity.commonResource == commonResource &&
        identity.resourceGeneration == resourceGeneration) {
      if (discardProbe != nullptr &&
          OutsidePoisonExactOverlapMatchesRange(
              discardProbe->oldExactOverlap, ledger.ranges[i])) {
        ++discardProbe->directDiscardObservation
              .oldExactOverlapRangesCleared;
      }
      RemovePoisonRange(i);
      ++cleared;
      continue;
    }
    ++i;
  }
  if (discardProbe != nullptr) {
    NativeOutsidePoisonDirectDiscardObservation& observation =
        discardProbe->directDiscardObservation;
    observation.cleared = cleared;
    observation.mutationGenerationAfter = t_poisonMutationGeneration;
    observation.poisonCountAfter =
        static_cast<uint32_t>(t_state.poisonLedger.count);
    observation.poisonOutstandingAfter =
        g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
    const bool oldProbeStableAtDiscard =
        discardProbe->ownerThreadId == ::GetCurrentThreadId() &&
        discardProbe->oldResultRecorded &&
        discardProbe->oldResult == NativeOutsidePoisonScanResult::Overlap &&
        OutsidePoisonExactOverlapMatchesOldTarget(
            discardProbe->oldExactOverlap,
            discardProbe->oldObservation) &&
        observation.commonResource ==
            discardProbe->oldExactOverlap.vertexOutputProof.resource
                .commonResource &&
        observation.resourceGeneration ==
            discardProbe->oldExactOverlap.vertexOutputProof.resource
                .resourceGeneration &&
        observation.mutationGenerationBefore ==
            discardProbe->poisonMutationGeneration &&
        observation.poisonCountBefore == discardProbe->poisonCount &&
        observation.poisonOutstandingBefore ==
            discardProbe->poisonOutstanding;
    const bool exactLedgerTransition = cleared != 0u &&
        observation.oldExactOverlapRangesCleared == 1u &&
        observation.poisonCountBefore >= cleared &&
        observation.poisonCountAfter ==
            observation.poisonCountBefore - cleared &&
        observation.poisonOutstandingBefore >= cleared &&
        observation.poisonOutstandingAfter ==
            observation.poisonOutstandingBefore - cleared &&
        observation.mutationGenerationAfter ==
            AdvancePoisonMutationGenerationBy(
                observation.mutationGenerationBefore, cleared);
    observation.exactOldOverlapRetired = oldProbeStableAtDiscard &&
        exactLedgerTransition &&
        NonZeroGenerationAdvancedOnce(
            observation.previousMapAllocationGeneration,
            observation.newMapAllocationGeneration);
    // Production evidence consumes the same immutable receipt, but report-only
    // O1a counters must remain physically cold while that policy is disabled.
    // Otherwise a valid authority sample contaminates the unrelated cold gate.
    if (observation.exactOldOverlapRetired &&
        OutsidePoisonProbeCollectsO1(*discardProbe)) {
      g_counters
          .productionOutsidePoisonO1ShadowDirectDiscardOldOverlapRetired
          .fetch_add(1u, std::memory_order_relaxed);
    }
  }
  if (cleared != 0u) {
    g_counters.nativeDirectDiscardEventsWithPoison.fetch_add(
        1u, std::memory_order_relaxed);
    g_counters.nativeDirectDiscardRangesCleared.fetch_add(
        cleared, std::memory_order_relaxed);
    g_counters.nativePoisonClears.fetch_add(
        cleared, std::memory_order_relaxed);
  } else {
    g_counters.nativeDirectDiscardNoPoisonEvents.fetch_add(
        1u, std::memory_order_relaxed);
  }
  return cleared;
}

NativeBridgeQuiescenceSnapshot GetNativeBridgeQuiescenceSnapshot() {
  if (CurrentThreadIsObservedRenderThread())
    FlushNativeTelemetryDeltaForCurrentThread();
  const uint64_t telemetryFlushSequenceBefore =
      g_counters.telemetryFlushes.load(std::memory_order_acquire);
  const uint32_t telemetryPendingBefore =
      g_nativeTelemetryDeltaPending.load(std::memory_order_acquire);
  const bool telemetryFaultedBefore =
      g_nativeTelemetryDeltaFaulted.load(std::memory_order_acquire);
  NativeBridgeQuiescenceSnapshot result = {};
  result.observedRenderThreadId =
      g_observedRenderThreadId.load(std::memory_order_acquire);
  result.requestedResetGeneration =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  result.completedResetGeneration =
      g_resetCompletedGeneration.load(std::memory_order_acquire);
  result.ownerRetiredGeneration =
      g_ownerRetiredGeneration.load(std::memory_order_acquire);
  result.pendingKernelAuthorizations =
      g_counters.pendingKernelAuthorizations.load(std::memory_order_acquire);
  result.poisonRangesOutstanding =
      g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
  result.retirementEventsPending =
      g_retirementEventsPending.load(std::memory_order_acquire);
  result.activeCallbackPins =
      g_activeCallbackPins.load(std::memory_order_acquire);
  result.activeFlushTransactions =
      g_activeFlushTransactions.load(std::memory_order_seq_cst);
  result.activeDispatchTransactions =
      g_activeDispatchTransactions.load(std::memory_order_seq_cst);
  result.activeSemanticTransactions =
      g_activeSemanticTransactions.load(std::memory_order_seq_cst);
  result.activeUploadTransactions =
      g_activeUploadTransactions.load(std::memory_order_seq_cst);
  const uint32_t activeDipObservers =
      g_activeDipObserverTransactions.load(std::memory_order_seq_cst);
  const uint32_t activeOutsideDipReaders =
      g_activeOutsideDipReaderTransactions.load(std::memory_order_seq_cst);
  result.activeDipObserverTransactions =
      activeDipObservers > std::numeric_limits<uint32_t>::max() -
              activeOutsideDipReaders
      ? std::numeric_limits<uint32_t>::max()
      : activeDipObservers + activeOutsideDipReaders;
  result.resetPending = result.requestedResetGeneration !=
      result.completedResetGeneration;
  result.currentThreadIsObservedRenderThread =
      CurrentThreadIsObservedRenderThread();
  result.currentThreadTlsQuiescent = CurrentThreadTlsQuiescent();
  result.safetyObservationEnabled =
      g_hooksEnabled.load(std::memory_order_acquire);
  result.transactionIngressEnabled =
      g_transactionIngressEnabled.load(std::memory_order_seq_cst);
  result.callbackIngressEnabled =
      g_callbackIngressEnabled.load(std::memory_order_acquire);
  result.dipObserverIngressEnabled =
      g_dipObserverIngressEnabled.load(std::memory_order_seq_cst);
  result.newBypassBlocked =
      !g_bypassEnabled.load(std::memory_order_acquire);
  result.retirementQueueFaulted =
      g_retirementQueueFaulted.load(std::memory_order_acquire);
  const uint32_t telemetryPendingAfter =
      g_nativeTelemetryDeltaPending.load(std::memory_order_acquire);
  const bool telemetryFaultedAfter =
      g_nativeTelemetryDeltaFaulted.load(std::memory_order_acquire);
  const uint64_t telemetryFlushSequenceAfter =
      g_counters.telemetryFlushes.load(std::memory_order_acquire);
  result.telemetryDeltaPending = telemetryPendingBefore != 0u ||
      telemetryPendingAfter != 0u ||
      telemetryFlushSequenceBefore != telemetryFlushSequenceAfter;
  result.telemetryDeltaFaulted =
      telemetryFaultedBefore || telemetryFaultedAfter;
  return result;
}

bool WaitForNativeBridgeQuiescence(
    uint32_t timeoutMs, bool requirePoisonRetired,
    NativeBridgeQuiescenceSnapshot* finalSnapshot) {
  const ULONGLONG start = ::GetTickCount64();
  for (;;) {
    if (CurrentThreadIsObservedRenderThread())
      ProcessPendingBridgeReset();
    const NativeBridgeQuiescenceSnapshot snapshot =
        GetNativeBridgeQuiescenceSnapshot();
    const bool quiescent = snapshot.activeCallbackPins == 0u &&
        snapshot.activeFlushTransactions == 0u &&
        snapshot.activeDispatchTransactions == 0u &&
        snapshot.activeSemanticTransactions == 0u &&
        snapshot.activeUploadTransactions == 0u &&
        snapshot.activeDipObserverTransactions == 0u &&
        snapshot.pendingKernelAuthorizations == 0u &&
        snapshot.retirementEventsPending == 0u &&
        snapshot.telemetryDeltaPending == 0u &&
        !snapshot.resetPending &&
        !snapshot.telemetryDeltaFaulted &&
        !snapshot.retirementQueueFaulted &&
        (!snapshot.currentThreadIsObservedRenderThread ||
         snapshot.currentThreadTlsQuiescent) &&
        (!requirePoisonRetired ||
         snapshot.poisonRangesOutstanding == 0u);
    if (quiescent) {
      if (finalSnapshot != nullptr)
        *finalSnapshot = snapshot;
      return true;
    }
    if (timeoutMs != std::numeric_limits<uint32_t>::max() &&
        ::GetTickCount64() - start >= timeoutMs) {
      if (finalSnapshot != nullptr)
        *finalSnapshot = snapshot;
      return false;
    }
    if (!::SwitchToThread())
      ::Sleep(1u);
  }
}

uint32_t ResetNativePoisonLedger(NativePoisonLedgerResetReason reason) {
  return RequestNativeBridgeReset(reason).poisonRangesCleared;
}

void NotifyNativeFlush(const NativeFlushObservation& source) {
  const bool productionTimingSampled = SampleNextProductionEventOrdinal(
      t_state.productionFlushTimingOrdinal);
  const int64_t productionTimingStart = productionTimingSampled
      ? dxvk::high_resolution_clock::get_counter() : 0;
  const uint64_t currentThread = ::GetCurrentThreadId();
  // A current-flush candidate view is never transferable to the next flush,
  // including disabled ingress and callback failure paths.
  InvalidateNativeDispatchCpuOnlySealView();
  t_state.flushCallbackEpoch = 0u;
  // This is the regular production publication boundary. It is deliberately
  // before the ingress early return so shutdown/disable can still publish the
  // owning render thread's final diagnostic delta.
  FlushNativeTelemetryDeltaForCurrentThread();
  if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst)) {
    // FlushSortedItems is a shared process-lifetime hook. Even after native
    // GPU-skin transaction ingress closes, its observed render-thread boundary
    // must be allowed to settle a reset requested by an off-thread uninstaller.
    if (CurrentThreadIsObservedRenderThread())
      ProcessPendingBridgeReset();
    if (productionTimingSampled) {
      const int64_t productionTimingEnd =
          dxvk::high_resolution_clock::get_counter();
      RecordNativeProductionTiming(
          NativeProductionTimingStage::FlushRoot,
          ProductionEventElapsedTicks(
              productionTimingStart, productionTimingEnd));
    }
    return;
  }
  uint64_t expectedThread = 0u;
  g_observedRenderThreadId.compare_exchange_strong(
      expectedThread, currentThread, std::memory_order_release,
      std::memory_order_relaxed);
  ConfigureNativeTelemetryAtFlush(currentThread);
  ProcessPendingBridgeReset();
  NativeFlushObservation observation = source;
  observation.mode = g_runtimeConfig.mode;
  observation.renderThreadId = currentThread;
  observation.flushEpoch =
      g_nextFlushEpoch.fetch_add(1u, std::memory_order_relaxed) + 1u;
  g_counters.flushNotifications.fetch_add(1u, std::memory_order_relaxed);
  {
    NativeFlushCallbackEpochScope flushCallbackEpoch(
        observation.flushEpoch);
    NativeBridgeCallbackPin callbackPin(
        NativeBridgeCallbackKind::Flush, observation.flushEpoch);
    const NativeBridgeCallbacks* callbacks = callbackPin.get();
    if (callbacks != nullptr && callbacks->onFlush != nullptr) {
      callbackPin.BeginCallbackBody();
      callbacks->onFlush(callbacks->userData, observation);
      callbackPin.EndCallbackBody();
    }
    flushCallbackEpoch.Complete();
  }
  // The manager may acknowledge owner-epoch retirement from onFlush. Complete
  // that reset before publishing this new flush epoch to subsequent dispatches.
  ProcessPendingBridgeReset();
  t_state.flushEpoch = observation.flushEpoch;
  t_state.orphanUploadOrdinal = 0u;
  t_state.orphanDipOrdinal = 0u;
  if (productionTimingSampled) {
    const int64_t productionTimingEnd =
        dxvk::high_resolution_clock::get_counter();
    RecordNativeProductionTiming(
        NativeProductionTimingStage::FlushRoot,
        ProductionEventElapsedTicks(
            productionTimingStart, productionTimingEnd));
  }
}

bool BeginNativeFlushTransaction() noexcept {
  if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      !CurrentThreadIsObservedRenderThread())
    return false;
  // Publish before mutating TLS, then make the ingress/thread check
  // authoritative. If shutdown won the race, rollback without ever exposing a
  // transaction-owned frame to a drain that already observed zero.
  g_activeFlushTransactions.fetch_add(1u, std::memory_order_seq_cst);
  if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      !CurrentThreadIsObservedRenderThread()) {
    g_activeFlushTransactions.fetch_sub(1u, std::memory_order_seq_cst);
    ProcessPendingBridgeReset();
    return false;
  }
  ++t_state.flushTransactionDepth;
  return true;
}

NativeOutsideUploadCoverKind ResolveOutsideUploadCover() noexcept {
  if (t_state.flushTransactionDepth != 0u)
    return NativeOutsideUploadCoverKind::Flush;

  const SemanticFrame* semantic = CurrentSemantic();
  if (t_state.semanticDepth == 1u && semantic != nullptr &&
      semantic->cookie != 0u && semantic->dispatchEpoch == 0u &&
      !semantic->failClosed && semantic->globalTransactionPinned) {
    return NativeOutsideUploadCoverKind::Semantic;
  }
  return NativeOutsideUploadCoverKind::Independent;
}

bool OutsideUploadCoverStillExact(
    const NativeOutsideUploadFastPathState& marker) noexcept {
  switch (marker.coverKind) {
    case NativeOutsideUploadCoverKind::Flush:
      return t_state.flushTransactionDepth != 0u;
    case NativeOutsideUploadCoverKind::Semantic: {
      const SemanticFrame* semantic = CurrentSemantic();
      return marker.semanticDepth == 1u && t_state.semanticDepth == 1u &&
          semantic != nullptr && marker.semanticCookie != 0u &&
          semantic->cookie == marker.semanticCookie &&
          semantic->dispatchEpoch == 0u && !semantic->failClosed &&
          semantic->globalTransactionPinned;
    }
    case NativeOutsideUploadCoverKind::Independent:
      return true;
    case NativeOutsideUploadCoverKind::Count:
      return false;
  }
  return false;
}

bool OutsideNoPoisonDirectSemanticExact(
    const NativeOutsideNoPoisonDirectOriginalState& receipt) noexcept {
  const SemanticFrame* semantic = CurrentSemantic();
  return (receipt.semanticDepth == 0u && t_state.semanticDepth == 0u &&
          semantic == nullptr) ||
      (receipt.semanticDepth == 1u && t_state.semanticDepth == 1u &&
       semantic != nullptr && receipt.semanticCookie != 0u &&
       semantic->cookie == receipt.semanticCookie &&
       semantic->dispatchEpoch == 0u && !semantic->failClosed);
}

void ConflictOutsideNoPoisonDirectOriginal(
    NativeOutsideNoPoisonDirectOriginalState& receipt,
    bool resetCompletedWhileActive = false,
    bool latePoison = false) noexcept {
  if (!receipt.conflicted) {
    g_counters.productionOutsideNoPoisonDirectConflicts.fetch_add(
        1u, std::memory_order_relaxed);
    receipt.conflicted = true;
  }
  if (resetCompletedWhileActive && !receipt.resetDriftRecorded) {
    g_counters.productionOutsideNoPoisonDirectResetCompletedWhileActive
        .fetch_add(1u, std::memory_order_relaxed);
    receipt.resetDriftRecorded = true;
  }
  if (latePoison && !receipt.latePoisonRecorded) {
    g_counters.productionOutsideNoPoisonDirectLatePoison.fetch_add(
        1u, std::memory_order_relaxed);
    receipt.latePoisonRecorded = true;
  }
}

bool OutsideNoPoisonDirectOriginalStillExact(
    NativeOutsideNoPoisonDirectOriginalState& receipt,
    uintptr_t gxDeviceD3d, bool recordConflict = true) noexcept {
  const uint64_t resetRequested =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  const uint64_t resetCompleted =
      g_resetCompletedGeneration.load(std::memory_order_acquire);
  const uint64_t poisonOutstanding =
      g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
  const bool latePoison = t_state.poisonLedger.count != 0u ||
      poisonOutstanding != 0u ||
      t_poisonMutationGeneration != receipt.poisonMutationGeneration;
  const bool resetCompletedWhileActive =
      resetCompleted != receipt.resetGeneration;
  const bool exact = receipt.cookie != 0u &&
      receipt.gxDeviceD3d != 0u && receipt.gxDeviceD3d == gxDeviceD3d &&
      receipt.renderThreadId == ::GetCurrentThreadId() &&
      g_runtimeConfig.mode == GpuSkinMode::Bypass &&
      !g_runtimeConfig.fullDiagnostics &&
      g_transactionIngressEnabled.load(std::memory_order_seq_cst) &&
      g_bypassEnabled.load(std::memory_order_acquire) &&
      CurrentThreadIsObservedRenderThread() &&
      resetRequested == receipt.resetGeneration &&
      resetCompleted == receipt.resetGeneration &&
      t_state.appliedResetGeneration == receipt.resetGeneration &&
      !latePoison &&
      g_retirementEventsPending.load(std::memory_order_acquire) == 0u &&
      !g_retirementQueueFaulted.load(std::memory_order_acquire) &&
      g_counters.pendingKernelAuthorizations.load(
          std::memory_order_acquire) == 0u &&
      g_activeCallbackPins.load(std::memory_order_acquire) == 0u &&
      t_callbackPinDepth == 0u && t_state.flushCallbackEpoch == 0u &&
      t_dipObserverDepth == 0u && t_dipObserverCookie == 0u &&
      t_dipObserverResetGeneration == 0u &&
      t_dipObserverCpuOnlySealCookie == 0u &&
      t_state.dispatchDepth == 0u &&
      t_state.dispatchOverflowDepth == 0u &&
      t_state.semanticOverflowDepth == 0u &&
      t_state.nestedUploadDepth == 0u &&
      t_state.inFlightUpload.observation == nullptr &&
      t_state.productionFastRejectedUpload == nullptr &&
      !t_state.productionFastRejectedGlobalTransactionPinned &&
      t_state.productionEvidenceUploadEpoch == 0u &&
      !OutsideUploadFastPathActive() &&
      !DispatchCpuOnlyUploadFastPathActive() &&
      t_outsidePoisonShadow.depth == 0u &&
      t_outsidePoisonShadow.overflowDepth == 0u &&
      g_nativeOutsidePoisonAuthorityActive.load(
          std::memory_order_acquire) == 0u &&
      OutsideNoPoisonDirectSemanticExact(receipt);
  if (!exact && recordConflict) {
    ConflictOutsideNoPoisonDirectOriginal(
        receipt, resetCompletedWhileActive, latePoison);
  }
  return exact;
}

uint64_t TryBeginNativeOutsideNoPoisonDirectOriginal(
    uintptr_t gxDeviceD3d, uint32_t vertexCount) noexcept {
  // This is deliberately before every generic admission/timing path. Preserve
  // the shared 1/127 evidence cohort first so it continues to exercise the
  // full observer, callback ABI and poison proof unchanged.
  if (g_runtimeConfig.mode != GpuSkinMode::Bypass ||
      g_runtimeConfig.fullDiagnostics) {
    return 0u;
  }
  if (OutsideNoPoisonDirectOriginalActive()) {
    ConflictOutsideNoPoisonDirectOriginal(
        t_state.outsideNoPoisonDirectOriginal);
    return 0u;
  }
  if (((t_state.beginTimingSampleOrdinal + 1u) %
       kNativeBeginTimingSamplePeriod) == 0u) {
    return 0u;
  }

  const SemanticFrame* semantic = CurrentSemantic();
  const bool semanticClean = t_state.semanticDepth == 0u ||
      (t_state.semanticDepth == 1u && semantic != nullptr &&
       semantic->cookie != 0u && semantic->dispatchEpoch == 0u &&
       !semantic->failClosed);
  const uint64_t resetRequested =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  const uint64_t resetCompleted =
      g_resetCompletedGeneration.load(std::memory_order_acquire);
  if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
      !g_bypassEnabled.load(std::memory_order_acquire) ||
      !CurrentThreadIsObservedRenderThread() || gxDeviceD3d == 0u ||
      vertexCount == 0u ||
      resetRequested != resetCompleted ||
      resetRequested != t_state.appliedResetGeneration ||
      t_state.poisonLedger.count != 0u ||
      g_nativePoisonOutstandingRanges.load(
          std::memory_order_acquire) != 0u ||
      g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
      g_retirementQueueFaulted.load(std::memory_order_acquire) ||
      g_counters.pendingKernelAuthorizations.load(
          std::memory_order_acquire) != 0u ||
      g_activeCallbackPins.load(std::memory_order_acquire) != 0u ||
      t_callbackPinDepth != 0u || t_state.flushCallbackEpoch != 0u ||
      t_dipObserverDepth != 0u || t_dipObserverCookie != 0u ||
      t_dipObserverResetGeneration != 0u ||
      t_dipObserverCpuOnlySealCookie != 0u ||
      t_state.dispatchDepth != 0u || !semanticClean ||
      t_state.dispatchOverflowDepth != 0u ||
      t_state.semanticOverflowDepth != 0u ||
      t_state.nestedUploadDepth != 0u ||
      t_state.inFlightUpload.observation != nullptr ||
      t_state.productionFastRejectedUpload != nullptr ||
      t_state.productionFastRejectedGlobalTransactionPinned ||
      t_state.productionEvidenceUploadEpoch != 0u ||
      OutsideUploadFastPathActive() ||
      DispatchCpuOnlyUploadFastPathActive() ||
      t_outsidePoisonShadow.depth != 0u ||
      t_outsidePoisonShadow.overflowDepth != 0u ||
      g_nativeOutsidePoisonAuthorityActive.load(
          std::memory_order_acquire) != 0u) {
    return 0u;
  }

  NativeOutsideNoPoisonDirectOriginalState& receipt =
      t_state.outsideNoPoisonDirectOriginal;
  receipt = {};
  receipt.gxDeviceD3d = gxDeviceD3d;
  receipt.renderThreadId = ::GetCurrentThreadId();
  receipt.cookie = NextOutsideNoPoisonDirectOriginalCookie();
  receipt.resetGeneration = resetRequested;
  receipt.poisonMutationGeneration = t_poisonMutationGeneration;
  receipt.semanticDepth = t_state.semanticDepth;
  receipt.semanticCookie = semantic != nullptr ? semantic->cookie : 0u;

  // Revalidate asynchronous publishers after the TLS receipt becomes visible.
  // The outer original has not started yet, so a race can still roll back to
  // the unchanged generic path without creating a direct attempt.
  if (!OutsideNoPoisonDirectOriginalStillExact(
          receipt, gxDeviceD3d, false)) {
    receipt = {};
    return 0u;
  }

  ++t_state.beginTimingSampleOrdinal;
  AddNativeTelemetryCounter(
      g_counters.productionOutsideNoPoisonDirectAttempts,
      t_nativeTelemetryDelta.productionOutsideNoPoisonDirectAttempts);
  RecordNativeOutsideAdmissionAccepted(false);
  g_counters.productionOutsideNoPoisonDirectActive.fetch_add(
      1u, std::memory_order_relaxed);
  return receipt.cookie;
}

bool TryRouteNativeOutsideNoPoisonDirectOriginalKernel(
    uintptr_t gxDeviceD3d, void* mappedDst,
    bool originalTrampolineAvailable) noexcept {
  NativeOutsideNoPoisonDirectOriginalState& receipt =
      t_state.outsideNoPoisonDirectOriginal;
  if (receipt.cookie == 0u)
    return false;
  if (!originalTrampolineAvailable) {
    ConflictOutsideNoPoisonDirectOriginal(receipt);
    return true;
  }

  const bool lifecycleExact =
      OutsideNoPoisonDirectOriginalStillExact(receipt, gxDeviceD3d);
  const bool exact = !receipt.kernelSeen && !receipt.conflicted &&
      lifecycleExact;
  if (!exact) {
    ConflictOutsideNoPoisonDirectOriginal(receipt);
  }
  if (receipt.kernelSeen)
    ConflictOutsideNoPoisonDirectOriginal(receipt);
  receipt.kernelSeen = true;
  receipt.mappedDst = reinterpret_cast<uintptr_t>(mappedDst);
  RecordNativeOutsideFastKernelTelemetry();
  AddNativeTelemetryCounter(
      g_counters.productionOutsideNoPoisonDirectKernelCalls,
      t_nativeTelemetryDelta.productionOutsideNoPoisonDirectKernelCalls);
  if (mappedDst == nullptr) {
    g_counters.nullMappedKernelFallbacks.fetch_add(
        1u, std::memory_order_relaxed);
  }
  // Once a direct receipt exists, every nested kernel is forced to original,
  // even after re-entry or identity drift. Returning true prevents the generic
  // evaluator from acquiring any bypass authority for this transaction.
  return true;
}

void NotifyNativeOutsideNoPoisonDirectOriginalReturned(
    uintptr_t gxDeviceD3d, void* mappedDst) noexcept {
  NativeOutsideNoPoisonDirectOriginalState& receipt =
      t_state.outsideNoPoisonDirectOriginal;
  if (receipt.cookie == 0u)
    return;
  const bool lifecycleExact =
      OutsideNoPoisonDirectOriginalStillExact(receipt, gxDeviceD3d);
  const bool exact = receipt.kernelSeen &&
      !receipt.kernelNormalReturned && !receipt.conflicted &&
      receipt.mappedDst == reinterpret_cast<uintptr_t>(mappedDst) &&
      lifecycleExact;
  if (!exact)
    ConflictOutsideNoPoisonDirectOriginal(receipt);
  if (receipt.kernelNormalReturned) {
    ConflictOutsideNoPoisonDirectOriginal(receipt);
    return;
  }
  receipt.kernelNormalReturned = true;
  AddNativeTelemetryCounter(
      g_counters.productionOutsideNoPoisonDirectNormalReturns,
      t_nativeTelemetryDelta.productionOutsideNoPoisonDirectNormalReturns);
}

void CompleteNativeOutsideNoPoisonDirectOriginal(
    uint64_t cookie) noexcept {
  NativeOutsideNoPoisonDirectOriginalState& receipt =
      t_state.outsideNoPoisonDirectOriginal;
  if (cookie == 0u || receipt.cookie == 0u || receipt.cookie != cookie) {
    if (receipt.cookie != 0u)
      ConflictOutsideNoPoisonDirectOriginal(receipt);
    return;
  }
  const bool lifecycleExact = OutsideNoPoisonDirectOriginalStillExact(
      receipt, receipt.gxDeviceD3d);
  const bool exact = receipt.kernelSeen && !receipt.conflicted &&
      lifecycleExact;
  if (!exact)
    ConflictOutsideNoPoisonDirectOriginal(receipt);
  if (receipt.kernelSeen && !receipt.kernelNormalReturned) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsideNoPoisonDirectKernelNoNormalReturns,
        t_nativeTelemetryDelta
            .productionOutsideNoPoisonDirectKernelNoNormalReturns);
  }
  receipt = {};
  AddNativeTelemetryCounter(
      g_counters.productionOutsideNoPoisonDirectCompleted,
      t_nativeTelemetryDelta.productionOutsideNoPoisonDirectCompleted);
  RecordNativeOutsideFastTelemetry();
  g_counters.productionOutsideNoPoisonDirectActive.fetch_sub(
      1u, std::memory_order_relaxed);
  if (g_resetRequestedGeneration.load(std::memory_order_acquire) !=
          g_resetCompletedGeneration.load(std::memory_order_acquire) ||
      g_retirementEventsPending.load(std::memory_order_acquire) != 0u) {
    ProcessPendingBridgeReset();
  }
}

void CancelNativeOutsideNoPoisonDirectOriginal(uint64_t cookie) noexcept {
  NativeOutsideNoPoisonDirectOriginalState& receipt =
      t_state.outsideNoPoisonDirectOriginal;
  if (cookie == 0u || receipt.cookie == 0u || receipt.cookie != cookie) {
    if (receipt.cookie != 0u)
      ConflictOutsideNoPoisonDirectOriginal(receipt);
    return;
  }
  receipt = {};
  g_counters.productionOutsideNoPoisonDirectCancellations.fetch_add(
      1u, std::memory_order_relaxed);
  RecordNativeOutsideAdmissionCancellation();
  g_counters.productionOutsideNoPoisonDirectActive.fetch_sub(
      1u, std::memory_order_relaxed);
  if (g_resetRequestedGeneration.load(std::memory_order_acquire) !=
          g_resetCompletedGeneration.load(std::memory_order_acquire) ||
      g_retirementEventsPending.load(std::memory_order_acquire) != 0u) {
    ProcessPendingBridgeReset();
  }
}

void PromoteOutsideUploadCoverToIndependent(
    NativeOutsideUploadFastPathState& marker) noexcept {
  if (marker.cookie == 0u ||
      marker.coverKind == NativeOutsideUploadCoverKind::Independent)
    return;
  // A malformed terminal must not let the enclosing semantic/flush scope
  // release the only remotely visible owner while this TLS marker survives.
  // Publish a fail-closed upload pin before changing its ownership class. The
  // unchanged marker-conflict hard gate makes the intentionally unmatched pin
  // visible rather than allowing reset/uninstall to observe false quiescence.
  g_activeUploadTransactions.fetch_add(1u, std::memory_order_seq_cst);
  RecordNativeOutsideIndependentPinBegin();
  marker.coverKind = NativeOutsideUploadCoverKind::Independent;
}

uint64_t TryBeginNativeOutsideUploadFastPath(
    uintptr_t gxDeviceD3d, uint32_t vertexCount,
    NativeOutsideUploadPopulationToken* populationToken,
    NativeOutsideUploadAdmissionProbe* sampledProbe,
    uint64_t outputByteCount,
    uint64_t* outsidePoisonShadowCookie) noexcept {
  // Preserve full evidence mode and leave one normal production sample out of
  // every 127 upload attempts on the generic path. A successful fast-path
  // admission reserves its ordinal before entering the original native call;
  // every rejected admission is advanced by BeginNativeUpload instead.
  if (sampledProbe != nullptr)
    *sampledProbe = {};
  if (populationToken != nullptr)
    *populationToken = {};
  if (outsidePoisonShadowCookie != nullptr)
    *outsidePoisonShadowCookie = 0u;
  const SemanticFrame* semantic = CurrentSemantic();
  const bool semanticOnlyScopeClean = t_state.semanticDepth == 0u ||
      (t_state.semanticDepth == 1u && semantic != nullptr &&
       semantic->dispatchEpoch == 0u && semantic->cookie != 0u &&
       !semantic->failClosed);
  const bool entryHadPoison = t_state.poisonLedger.count != 0u;
  const bool productionLight =
      g_runtimeConfig.mode == GpuSkinMode::Bypass &&
      !g_runtimeConfig.fullDiagnostics;
  const bool exactTracked = productionLight && t_state.dispatchDepth == 0u;
  if (populationToken != nullptr && productionLight) {
    populationToken->populationClass = exactTracked
        ? NativeOutsideUploadPopulationClass::Tracked
        : NativeOutsideUploadPopulationClass::Untracked;
  }
  const auto reject = [=](
      NativeOutsideUploadRejectReason reason,
      bool lifecycleExcluded = false) noexcept -> uint64_t {
    if (sampledProbe != nullptr)
      sampledProbe->rejectReason = reason;
    if (exactTracked) {
      RecordNativeOutsideAdmissionReject(
          reason, entryHadPoison, lifecycleExcluded);
      if (populationToken != nullptr && lifecycleExcluded)
        populationToken->lifecycleAlreadyExcluded = true;
    }
    return 0u;
  };
  if (OutsideUploadFastPathActive()) {
    // A re-entrant +0x68 upload can advance the same native ring after the
    // outer prediction. Poison-bearing admission must never reuse that stale
    // interval; keep the owner marker live but permanently conflict it.
    t_state.outsideUploadFastPath.conflicted = true;
    g_counters.productionOutsideFastKernelMarkerConflicts.fetch_add(
        1u, std::memory_order_relaxed);
    return reject(NativeOutsideUploadRejectReason::ActiveFastMarker);
  }
  const uint64_t resetRequested =
      g_resetRequestedGeneration.load(std::memory_order_acquire);
  const uint64_t resetCompleted =
      g_resetCompletedGeneration.load(std::memory_order_acquire);
  // Preserve the original short-circuit predicate order exactly. Recording
  // happens only at the first terminal branch and never re-reads an async gate
  // to infer a reason after the fact.
  if (g_runtimeConfig.mode != GpuSkinMode::Bypass)
    return reject(NativeOutsideUploadRejectReason::ModeNotBypass);
  if (g_runtimeConfig.fullDiagnostics)
    return reject(NativeOutsideUploadRejectReason::FullDiagnostics);
  if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst)) {
    return reject(
        NativeOutsideUploadRejectReason::IngressClosed, true);
  }
  if (!g_bypassEnabled.load(std::memory_order_acquire))
    return reject(NativeOutsideUploadRejectReason::BypassDisabled);
  if (!CurrentThreadIsObservedRenderThread())
    return reject(NativeOutsideUploadRejectReason::WrongThread);
  if (gxDeviceD3d == 0u)
    return reject(NativeOutsideUploadRejectReason::NullDevice);
  if (t_state.dispatchDepth != 0u)
    return reject(NativeOutsideUploadRejectReason::DispatchOwned);
  // A semantic scope without a dispatch owner is still provably unable to
  // authorize takeover. Keep the Apply frame live for its caller, but do not
  // force its native upload through the generic observer.
  if (!semanticOnlyScopeClean)
    return reject(NativeOutsideUploadRejectReason::SemanticScopeHazard);
  if (t_state.dispatchOverflowDepth != 0u)
    return reject(NativeOutsideUploadRejectReason::DispatchOverflow);
  if (t_state.semanticOverflowDepth != 0u)
    return reject(NativeOutsideUploadRejectReason::SemanticOverflow);
  if (t_state.nestedUploadDepth != 0u)
    return reject(NativeOutsideUploadRejectReason::NestedUpload);
  if (t_state.inFlightUpload.observation != nullptr)
    return reject(NativeOutsideUploadRejectReason::GenericUploadInFlight);
  if (t_state.productionFastRejectedUpload != nullptr) {
    return reject(
        NativeOutsideUploadRejectReason::FastRejectUploadInFlight);
  }
  if (t_state.productionEvidenceUploadEpoch != 0u)
    return reject(NativeOutsideUploadRejectReason::EvidenceUploadInFlight);
  if (g_retirementEventsPending.load(std::memory_order_acquire) != 0u)
    return reject(NativeOutsideUploadRejectReason::RetirementPending);
  if (g_retirementQueueFaulted.load(std::memory_order_acquire))
    return reject(NativeOutsideUploadRejectReason::RetirementQueueFault);
  if (resetRequested != resetCompleted) {
    return reject(
        NativeOutsideUploadRejectReason::ResetGenerationMismatch);
  }
  // Check the evidence cohort before any poison-only GX read. Rejected
  // admission is advanced exactly once by BeginNativeUpload.
  if (((t_state.beginTimingSampleOrdinal + 1u) %
       kNativeBeginTimingSamplePeriod) == 0u) {
    return reject(NativeOutsideUploadRejectReason::EvidenceCohort);
  }

  const bool admittedWithPoison = entryHadPoison;
  uint64_t poisonShadowCookie = 0u;
  bool legacyPoisonScanPerformed = false;
  bool legacyPoisonScanAdmitted = false;
  bool authorityUsesActualLock = false;
  bool authorityLegacyBacked = false;
  if (admittedWithPoison) {
    const bool poisonSidecarEnabled = OutsidePoisonAnySidecarEnabled();
    ++t_outsidePoisonAuthorityEvidenceOrdinal;
    if (t_outsidePoisonAuthorityEvidenceOrdinal == 0u)
      ++t_outsidePoisonAuthorityEvidenceOrdinal;
    const bool authorityEvidence =
        (t_outsidePoisonAuthorityEvidenceOrdinal %
         kNativeOutsidePoisonAuthorityEvidencePeriod) == 0u;
    // The successful Lock is the independent layout authority. Its FVF,
    // descriptor size, byte range and mapped pointer derive the exact format
    // even when the outer hook cannot pre-compute outputByteCount from optional
    // source pointers. A non-zero outer byte count remains an additional exact
    // equality inside ScanOutsidePoisonO1ShadowLock; zero does not weaken the
    // Lock-derived range proof.
    const bool needLegacyScan = poisonSidecarEnabled || authorityEvidence;
    poisonShadowCookie = outsidePoisonShadowCookie != nullptr
        ? BeginOutsidePoisonShadow(
              gxDeviceD3d, vertexCount, outputByteCount, resetRequested,
              true, authorityEvidence)
        : 0u;
    if (outsidePoisonShadowCookie != nullptr)
      *outsidePoisonShadowCookie = poisonShadowCookie;
    if (needLegacyScan) {
      legacyPoisonScanPerformed = true;
      authorityLegacyBacked = true;
      g_counters.productionOutsidePoisonScanAttempts.fetch_add(
          1u, std::memory_order_relaxed);
      NativeOutsidePoisonScanObservation scanObservation = {};
      NativeOutsidePoisonExactOverlap exactOverlap = {};
      const NativeOutsidePoisonScanResult scan =
          ProveOutsideCpuUploadNoPoisonOverlap(
          gxDeviceD3d, vertexCount, &scanObservation,
          (OutsidePoisonO1SidecarEnabled() || authorityEvidence)
              ? &exactOverlap : nullptr);
      RecordOutsidePoisonShadowOldResult(
          poisonShadowCookie, scan, scanObservation, exactOverlap);
      if (scan == NativeOutsidePoisonScanResult::ReadFailure) {
        g_counters.productionOutsidePoisonReadFailRejects.fetch_add(
            1u, std::memory_order_relaxed);
        if (authorityEvidence) {
          ArmOutsidePoisonAuthority(
              poisonShadowCookie, false, true);
        }
        return reject(NativeOutsideUploadRejectReason::PoisonReadFailure);
      }
      if (scan == NativeOutsidePoisonScanResult::Overlap) {
        g_counters.productionOutsidePoisonOverlapRejects.fetch_add(
            1u, std::memory_order_relaxed);
        if (authorityEvidence) {
          ArmOutsidePoisonAuthority(
              poisonShadowCookie, false, true);
        }
        return reject(NativeOutsideUploadRejectReason::PoisonOverlap);
      }
      legacyPoisonScanAdmitted = true;
    } else {
      authorityUsesActualLock = true;
    }

    // Reset/retirement publishers may race either the evidence snapshot or
    // provisional token creation. They cannot mutate this render TLS, but any
    // publication invalidates manager-free admission.
    bool revalidationFailed = false;
    bool lifecycleExcluded = false;
    if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst)) {
      revalidationFailed = true;
      lifecycleExcluded = true;
    } else if (!g_bypassEnabled.load(std::memory_order_acquire)) {
      revalidationFailed = true;
    } else if (g_resetRequestedGeneration.load(std::memory_order_acquire) !=
               resetRequested) {
      revalidationFailed = true;
    } else if (g_resetCompletedGeneration.load(std::memory_order_acquire) !=
               resetCompleted) {
      revalidationFailed = true;
    } else if (g_retirementEventsPending.load(std::memory_order_acquire) !=
               0u) {
      revalidationFailed = true;
    } else if (g_retirementQueueFaulted.load(std::memory_order_acquire)) {
      revalidationFailed = true;
    }
    if (revalidationFailed) {
      LatchOutsidePoisonShadowLifecycleFailure(poisonShadowCookie);
      if (legacyPoisonScanPerformed) {
        g_counters.productionOutsidePoisonReadFailRejects.fetch_add(
            1u, std::memory_order_relaxed);
      }
      return reject(
          NativeOutsideUploadRejectReason::PoisonPostScanRevalidation,
          lifecycleExcluded);
    }
  }

  const NativeOutsideUploadCoverKind coverKind =
      ResolveOutsideUploadCover();
  const bool ownsIndependentPin =
      coverKind == NativeOutsideUploadCoverKind::Independent;
  if (ownsIndependentPin) {
    // The native outer-upload detour has its own uninstall pin, but an upload
    // without an enclosing flush or exact semantic owner still needs a
    // remotely visible bridge pin. Publish it before revalidating every
    // asynchronous admission gate.
    g_activeUploadTransactions.fetch_add(1u, std::memory_order_seq_cst);
    RecordNativeOutsideIndependentPinBegin();
    bool revalidationFailed = false;
    bool lifecycleExcluded = false;
    if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst)) {
      revalidationFailed = true;
      lifecycleExcluded = true;
    } else if (!g_bypassEnabled.load(std::memory_order_acquire)) {
      revalidationFailed = true;
    } else if (!CurrentThreadIsObservedRenderThread()) {
      revalidationFailed = true;
    } else if (g_resetRequestedGeneration.load(std::memory_order_acquire) !=
               resetRequested) {
      revalidationFailed = true;
    } else if (g_resetCompletedGeneration.load(std::memory_order_acquire) !=
               resetCompleted) {
      revalidationFailed = true;
    } else if (g_retirementEventsPending.load(std::memory_order_acquire) !=
               0u) {
      revalidationFailed = true;
    } else if (g_retirementQueueFaulted.load(std::memory_order_acquire)) {
      revalidationFailed = true;
    }
    if (revalidationFailed) {
      LatchOutsidePoisonShadowLifecycleFailure(poisonShadowCookie);
      g_activeUploadTransactions.fetch_sub(1u, std::memory_order_seq_cst);
      RecordNativeOutsideIndependentPinEnd();
      if (legacyPoisonScanPerformed) {
        g_counters.productionOutsidePoisonReadFailRejects.fetch_add(
            1u, std::memory_order_relaxed);
      }
      ProcessPendingBridgeReset();
      return reject(
          NativeOutsideUploadRejectReason::IndependentPinRevalidation,
          lifecycleExcluded);
    }
  }
  if (admittedWithPoison &&
      !ArmOutsidePoisonAuthority(
          poisonShadowCookie, authorityUsesActualLock,
          authorityLegacyBacked)) {
    LatchOutsidePoisonShadowLifecycleFailure(poisonShadowCookie);
    if (ownsIndependentPin) {
      g_activeUploadTransactions.fetch_sub(1u, std::memory_order_seq_cst);
      RecordNativeOutsideIndependentPinEnd();
      ProcessPendingBridgeReset();
    }
    return reject(NativeOutsideUploadRejectReason::PoisonReadFailure);
  }
  if (legacyPoisonScanAdmitted) {
    g_counters.productionOutsidePoisonNoOverlapAdmissions.fetch_add(
        1u, std::memory_order_relaxed);
  }
  ++t_state.beginTimingSampleOrdinal;
  const uint64_t cookie = NextOutsideUploadFastPathCookie();
  t_state.outsideUploadFastPath = {};
  t_state.outsideUploadFastPath.gxDeviceD3d = gxDeviceD3d;
  t_state.outsideUploadFastPath.renderThreadId = ::GetCurrentThreadId();
  t_state.outsideUploadFastPath.cookie = cookie;
  t_state.outsideUploadFastPath.resetGeneration = resetRequested;
  t_state.outsideUploadFastPath.poisonShadowCookie = poisonShadowCookie;
  t_state.outsideUploadFastPath.semanticDepth = t_state.semanticDepth;
  t_state.outsideUploadFastPath.semanticCookie =
      semantic != nullptr ? semantic->cookie : 0u;
  t_state.outsideUploadFastPath.admittedWithPoison =
      admittedWithPoison;
  t_state.outsideUploadFastPath.coverKind = coverKind;
  t_state.outsideUploadFastPath.admissionExactTracked = exactTracked;
  RecordNativeOutsideCoverBegin(coverKind);
  RecordNativeOutsideAdmissionAccepted(entryHadPoison);
  if (sampledProbe != nullptr) {
    if (entryHadPoison)
      sampledProbe->acceptedClass =
          coverKind == NativeOutsideUploadCoverKind::Flush
          ? NativeOutsideUploadAdmissionClass::PoisonFlush
          : coverKind == NativeOutsideUploadCoverKind::Semantic
          ? NativeOutsideUploadAdmissionClass::PoisonSemantic
          : NativeOutsideUploadAdmissionClass::PoisonIndependent;
    else
      sampledProbe->acceptedClass =
          coverKind == NativeOutsideUploadCoverKind::Flush
          ? NativeOutsideUploadAdmissionClass::NoPoisonFlush
          : coverKind == NativeOutsideUploadCoverKind::Semantic
          ? NativeOutsideUploadAdmissionClass::NoPoisonSemantic
          : NativeOutsideUploadAdmissionClass::NoPoisonIndependent;
  }
  return cookie;
}

void CompleteNativeOutsideUploadFastPath(uint64_t cookie) noexcept {
  NativeOutsideUploadFastPathState& marker =
      t_state.outsideUploadFastPath;
  if (cookie == 0u || marker.cookie == 0u || marker.cookie != cookie) {
    PromoteOutsideUploadCoverToIndependent(marker);
    g_counters.productionOutsideFastKernelMarkerConflicts.fetch_add(
        1u, std::memory_order_relaxed);
    return;
  }
  if (!OutsideUploadCoverStillExact(marker)) {
    PromoteOutsideUploadCoverToIndependent(marker);
    marker.conflicted = true;
    g_counters.productionOutsideFastKernelMarkerConflicts.fetch_add(
        1u, std::memory_order_relaxed);
    return;
  }
  if (!marker.kernelSeen) {
    g_counters.productionOutsideFastKernelMarkerConflicts.fetch_add(
        1u, std::memory_order_relaxed);
  }
  const bool ownsIndependentPin =
      marker.coverKind == NativeOutsideUploadCoverKind::Independent;
  marker = {};

  // Match the existing outside-fast accounting exactly. A poison-only GX
  // snapshot is admission authority, not retained observation evidence, so
  // byte count stays zero and both histograms use the unknown bucket. No
  // manager callback or fanout callback is legal.
  RecordNativeOutsideFastTelemetry();
  if (ownsIndependentPin) {
    g_activeUploadTransactions.fetch_sub(1u, std::memory_order_seq_cst);
    RecordNativeOutsideIndependentPinEnd();
    ProcessPendingBridgeReset();
  }
}

void CancelNativeOutsideUploadFastPath(uint64_t cookie) noexcept {
  NativeOutsideUploadFastPathState& marker =
      t_state.outsideUploadFastPath;
  if (cookie == 0u || marker.cookie == 0u || marker.cookie != cookie) {
    PromoteOutsideUploadCoverToIndependent(marker);
    g_counters.productionOutsideFastKernelMarkerConflicts.fetch_add(
        1u, std::memory_order_relaxed);
    return;
  }
  if (!OutsideUploadCoverStillExact(marker)) {
    PromoteOutsideUploadCoverToIndependent(marker);
    marker.conflicted = true;
    g_counters.productionOutsideFastKernelMarkerConflicts.fetch_add(
        1u, std::memory_order_relaxed);
    return;
  }
  const bool ownsIndependentPin =
      marker.coverKind == NativeOutsideUploadCoverKind::Independent;
  const bool admissionExactTracked = marker.admissionExactTracked;
  marker = {};
  if (admissionExactTracked)
    RecordNativeOutsideAdmissionCancellation();
  if (ownsIndependentPin) {
    g_activeUploadTransactions.fetch_sub(1u, std::memory_order_seq_cst);
    RecordNativeOutsideIndependentPinEnd();
    ProcessPendingBridgeReset();
  }
}

namespace {

uint64_t TryBeginNativeDispatchCpuOnlyUploadFastPath(
    uintptr_t gxDeviceD3d, uint32_t vertexCount,
    uint64_t outputByteCount) noexcept {
  DispatchFrame* frame = CurrentDispatch();
  if (frame == nullptr || !frame->cpuOnlySeal.committed ||
      !DispatchCpuOnlySealPhysicalPathAllowed(*frame) ||
      gxDeviceD3d == 0u || vertexCount == 0u ||
      outputByteCount == 0u ||
      DispatchCpuOnlyUploadFastPathActive()) {
    return 0u;
  }

  // A generic 1/127 evidence upload remains active until this next physical
  // upload boundary. Settle it with the old ABI-9 callback before considering
  // the sealed route, then revalidate every lifecycle publication because the
  // callback is allowed to request reset/uninstall.
  if (frame->activeUpload.valid)
    FinalizeActiveUpload(*frame);
  FinalizeDispatchCpuOnlyActiveUpload(*frame);
  const bool uploadSafetyExact = frame->cpuOnlySeal.uploadFastPathValidated
      ? DispatchCpuOnlySealUploadDynamicSafetyExact(*frame)
      : DispatchCpuOnlySealBaseSafetyExact(*frame, 0u, true, false);
  if (!frame->cpuOnlySeal.committed || frame->activeUpload.valid ||
      !uploadSafetyExact) {
    InvalidateDispatchCpuOnlySeal(*frame);
    return 0u;
  }
  frame->cpuOnlySeal.uploadFastPathValidated = true;

  const SemanticFrame* semantic = CurrentSemantic();
  const uint64_t cookie =
      NextDispatchCpuOnlyUploadFastPathCookie();
  NativeDispatchCpuOnlyUploadFastPathState& marker =
      t_state.dispatchCpuOnlyUploadFastPath;
  marker = {};
  marker.gxDeviceD3d = gxDeviceD3d;
  marker.renderThreadId = ::GetCurrentThreadId();
  marker.cookie = cookie;
  marker.resetGeneration = frame->cpuOnlySeal.resetGeneration;
  marker.dispatchEpoch = frame->observation.epoch.dispatchEpoch;
  marker.semanticDepth = t_state.semanticDepth;
  marker.semanticCookie = semantic != nullptr ? semantic->cookie : 0u;
  marker.outputByteCount = outputByteCount;
  marker.vertexCount = vertexCount;
  frame->cpuOnlyActiveUpload = {};
  frame->cpuOnlyActiveUpload.valid = true;
  AddNativeTelemetryCounter(
      g_counters.dispatchCpuOnlySealUploadsStarted,
      t_nativeTelemetryDelta.dispatchCpuOnlySealUploadsStarted);
  return cookie;
}

}  // namespace

uint64_t TryBeginNativeCpuOnlyUploadFastPath(
    uintptr_t gxDeviceD3d, uint32_t vertexCount,
    uint64_t outputByteCount,
    NativeOutsideUploadPopulationToken* populationToken,
    NativeOutsideUploadAdmissionProbe* sampledProbe,
    bool* dispatchCpuOnlySeal,
    uint64_t* outsidePoisonShadowCookie) noexcept {
  if (dispatchCpuOnlySeal != nullptr)
    *dispatchCpuOnlySeal = false;
  if (populationToken != nullptr)
    *populationToken = {};
  if (sampledProbe != nullptr)
    *sampledProbe = {};
  if (outsidePoisonShadowCookie != nullptr)
    *outsidePoisonShadowCookie = 0u;

  // A re-entrant outer call underneath a direct-original receipt is kept on
  // the generic CPU path. Its kernel hook will still be forced to original by
  // the owning receipt, so no nested observer can acquire bypass authority.
  if (OutsideNoPoisonDirectOriginalActive())
    return 0u;

  const uint64_t dispatchCookie =
      TryBeginNativeDispatchCpuOnlyUploadFastPath(
          gxDeviceD3d, vertexCount, outputByteCount);
  if (dispatchCookie != 0u) {
    if (populationToken != nullptr) {
      populationToken->populationClass =
          NativeOutsideUploadPopulationClass::Untracked;
    }
    if (dispatchCpuOnlySeal != nullptr)
      *dispatchCpuOnlySeal = true;
    return dispatchCookie;
  }
  return TryBeginNativeOutsideUploadFastPath(
      gxDeviceD3d, vertexCount, populationToken, sampledProbe,
      outputByteCount, outsidePoisonShadowCookie);
}

void CompleteNativeCpuOnlyUploadFastPath(uint64_t cookie) noexcept {
  NativeDispatchCpuOnlyUploadFastPathState& marker =
      t_state.dispatchCpuOnlyUploadFastPath;
  if (cookie != 0u && marker.cookie == cookie) {
    DispatchFrame* frame = CurrentDispatch();
    const bool exact = frame != nullptr &&
        frame->cpuOnlySeal.committed &&
        DispatchCpuOnlySealPhysicalPathAllowed(*frame) &&
        frame->observation.epoch.dispatchEpoch == marker.dispatchEpoch &&
        frame->cpuOnlyActiveUpload.valid && marker.kernelSeen &&
        marker.kernelNormalReturned && !marker.conflicted &&
        marker.renderThreadId == ::GetCurrentThreadId() &&
        marker.resetGeneration == frame->cpuOnlySeal.resetGeneration &&
        DispatchCpuOnlySealBaseSafetyExact(
            *frame, 0u, true, true);
    const uint32_t vertexCount = marker.vertexCount;
    const uint64_t outputByteCount = marker.outputByteCount;
    marker = {};
    if (!exact) {
      AddNativeTelemetryCounter(
          g_counters.dispatchCpuOnlySealMarkerConflicts,
          t_nativeTelemetryDelta.dispatchCpuOnlySealMarkerConflicts);
      if (frame != nullptr) {
        frame->cpuOnlyActiveUpload = {};
        InvalidateDispatchCpuOnlySeal(*frame);
      }
      return;
    }

    // This is a physical native CPU upload, not an ABI-9 manager upload. Keep
    // the global raw/format/kernel contracts exact while the independent seal
    // ledger owns its manager-free fanout.
    AddNativeTelemetryCounter(
        g_counters.uploads, t_nativeTelemetryDelta.uploads);
    AddNativeTelemetryCounter(
        g_counters.originalUploadCalls,
        t_nativeTelemetryDelta.originalUploadCalls);
    AddNativeTelemetryCounter(
        g_counters.originalUploadBytes,
        t_nativeTelemetryDelta.originalUploadBytes,
        outputByteCount);
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealUploadsCompleted,
        t_nativeTelemetryDelta.dispatchCpuOnlySealUploadsCompleted);
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealVertices,
        t_nativeTelemetryDelta.dispatchCpuOnlySealVertices,
        vertexCount);
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealBytes,
        t_nativeTelemetryDelta.dispatchCpuOnlySealBytes,
        outputByteCount);
    NativeTelemetryDelta& telemetry = t_nativeTelemetryDelta;
    if (!TryBatchNativeTelemetryAdd(telemetry.formatHistogram[7]))
      g_counters.formatHistogram[7].fetch_add(
          1u, std::memory_order_relaxed);
    if (!TryBatchNativeTelemetryAdd(telemetry.skinModeHistogram[7]))
      g_counters.skinModeHistogram[7].fetch_add(
          1u, std::memory_order_relaxed);
    return;
  }
  CompleteNativeOutsideUploadFastPath(cookie);
}

void CancelNativeCpuOnlyUploadFastPath(uint64_t cookie) noexcept {
  NativeDispatchCpuOnlyUploadFastPathState& marker =
      t_state.dispatchCpuOnlyUploadFastPath;
  if (cookie != 0u && marker.cookie == cookie) {
    DispatchFrame* frame = CurrentDispatch();
    marker = {};
    if (frame != nullptr) {
      frame->cpuOnlyActiveUpload = {};
      InvalidateDispatchCpuOnlySeal(*frame);
    }
    return;
  }
  CancelNativeOutsideUploadFastPath(cookie);
}

void SettleNativeOutsidePoisonShadow(
    uint64_t cookie, int32_t originalResult) noexcept {
  FinalizeOutsidePoisonShadow(cookie, false, originalResult);
}

void CancelNativeOutsidePoisonShadow(uint64_t cookie) noexcept {
  FinalizeOutsidePoisonShadow(cookie, true, 0);
}

bool NativeOutsidePoisonVertexLockShadowRequiredSlow() noexcept {
  const NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  return state.overflowDepth == 0u && state.depth != 0u &&
      (OutsidePoisonProbeCollectsO0(state.probes[state.depth - 1u]) ||
       OutsidePoisonProbeCollectsO1(state.probes[state.depth - 1u]) ||
       (state.probes[state.depth - 1u].authorityArmed &&
        !state.probes[state.depth - 1u].authorityFastNoOverlap)) &&
      !state.probes[state.depth - 1u].frozen;
}

bool TryNotifyNativeOutsidePoisonVertexLockFastNoOverlap(
    const NativeOutsidePoisonVertexLockFastInput& input) noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (g_runtimeConfig.executionRoute !=
          GpuSkinExecutionRoute::VertexShaderBypass ||
      state.overflowDepth != 0u || state.depth == 0u) {
    return false;
  }
  NativeOutsidePoisonShadowProbe& probe = state.probes[state.depth - 1u];
  // sidecar 与 1/127 authority evidence 必须保留完整 Lock/Unlock 证据；
  // 轻量端点只收窄产品 O1 的确定 NoOverlap 子集。
  if (!probe.authorityArmed || !probe.authorityUsesActualLock ||
      probe.authorityEvidence || probe.authorityLockTerminalRecorded ||
      probe.frozen || OutsidePoisonProbeCollectsO0(probe) ||
      OutsidePoisonProbeCollectsO1(probe)) {
    return false;
  }

  uint32_t outputFormat = 0u;
  uint32_t vertexStride = 0u;
  const NativeOutsidePoisonO1LockLane lane =
      ClassifyOutsidePoisonO1LockLane(
          input.storage.mapMode, input.effectiveFlags);
  const bool lifecycleExact = probe.ownerThreadId != 0u &&
      probe.ownerThreadId == ::GetCurrentThreadId() &&
      probe.flushEpoch != 0u && probe.flushEpoch == t_state.flushEpoch &&
      probe.resetGeneration ==
          g_resetRequestedGeneration.load(std::memory_order_acquire) &&
      probe.resetGeneration ==
          g_resetCompletedGeneration.load(std::memory_order_acquire) &&
      probe.resetGeneration == t_state.appliedResetGeneration &&
      g_retirementEventsPending.load(std::memory_order_acquire) == 0u &&
      !g_retirementQueueFaulted.load(std::memory_order_acquire);
  const bool descriptorExact = input.result >= 0 &&
      input.descType == kD3dResourceTypeVertexBuffer &&
      input.descPool == kD3dPoolDefault &&
      (input.descUsage & kD3dUsageDynamic) != 0u &&
      input.lockDepth == 1u && input.storage.activeLock &&
      input.storage.activeLockFlags == input.effectiveFlags &&
      input.requestedFlags == input.effectiveFlags &&
      lane != NativeOutsidePoisonO1LockLane::Count &&
      ResolveOutsidePoisonShadowFormat(
          input.descFvf, outputFormat, vertexStride) &&
      input.descSize == uint64_t(kMaxNativeVertices) * vertexStride &&
      input.offset % vertexStride == 0u &&
      input.size % vertexStride == 0u && input.size != 0u &&
      uint64_t(input.offset) + input.size <= input.descSize;
  const uint32_t baseVertex = descriptorExact
      ? input.offset / vertexStride : 0u;
  const uint32_t vertexCount = descriptorExact
      ? input.size / vertexStride : 0u;
  const bool identityExact = lifecycleExact && descriptorExact &&
      input.resource.commonResource != 0u &&
      input.resource.comVertexBuffer != 0u &&
      input.resource.resourceGeneration != 0u &&
      input.nativeD3DDevice != 0u &&
      vertexCount == probe.vertexCount &&
      (probe.outputByteCount == 0u ||
       probe.outputByteCount == uint64_t(input.size)) &&
      uint64_t(baseVertex) + vertexCount <= kMaxNativeVertices;
  if (!identityExact)
    return false;

  const NativePoisonLedger& ledger = t_state.poisonLedger;
  const uint64_t poisonOutstanding =
      g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
  if (ledger.count == 0u || ledger.count > ledger.ranges.size() ||
      poisonOutstanding != ledger.count) {
    return false;
  }
  NativeOutsidePoisonVertexLockEvidence fastEvidence = {};
  fastEvidence.resource = input.resource;
  fastEvidence.storage = input.storage;
  fastEvidence.nativeD3DDevice = input.nativeD3DDevice;
  fastEvidence.descFvf = input.descFvf;
  const uint64_t lockEnd = uint64_t(baseVertex) + vertexCount;
  for (size_t i = 0u; i < ledger.count; ++i) {
    const NativePoisonRange& poison = ledger.ranges[i];
    const NativeVertexOutputProof& proof = poison.vertexOutputProof;
    const uint64_t poisonEnd = PoisonRangeEnd(poison);
    const bool proofComplete = proof.resource.commonResource != 0u &&
        proof.resource.comVertexBuffer != 0u &&
        proof.resource.resourceGeneration != 0u &&
        proof.nativeD3DDevice != 0u &&
        proof.outputFormat < kFormatFvf.size() &&
        proof.vertexStride == kFormatStride[proof.outputFormat] &&
        proof.fvf == kFormatFvf[proof.outputFormat] &&
        poison.vertexCount != 0u && poisonEnd <= kMaxNativeVertices &&
        poison.outputFormat == proof.outputFormat &&
        poison.stride == proof.vertexStride &&
        poison.fvf == proof.fvf;
    if (!proofComplete)
      return false;
    if (proof.nativeD3DDevice != input.nativeD3DDevice ||
        proof.resource.comVertexBuffer != input.resource.comVertexBuffer ||
        proof.outputFormat != outputFormat ||
        proof.vertexStride != vertexStride || proof.fvf != input.descFvf) {
      continue;
    }
    if (proof.resource.commonResource != input.resource.commonResource ||
        proof.resource.resourceGeneration !=
            input.resource.resourceGeneration ||
        !OutsidePoisonShadowStorageMatches(poison, fastEvidence)) {
      return false;
    }
    if (lockEnd > poison.baseVertex && baseVertex < poisonEnd)
      return false;
  }

  AddNativeTelemetryCounter(
      g_counters.productionOutsidePoisonAuthorityLockNotifications,
      t_nativeTelemetryDelta
          .productionOutsidePoisonAuthorityLockNotifications);
  ++probe.lockNotifications;
  probe.authorityLockTerminalRecorded = true;
  probe.authorityLockPoisonGeneration = t_poisonMutationGeneration;
  probe.authorityLockPoisonCount = static_cast<uint32_t>(ledger.count);
  probe.authorityLockPoisonOutstanding = poisonOutstanding;
  probe.authorityLockResult = NativeOutsidePoisonScanResult::NoOverlap;
  probe.authorityOutputFormat = outputFormat;
  probe.authorityVertexStride = vertexStride;
  probe.authorityBaseVertex = baseVertex;
  probe.authorityVertexCount = vertexCount;
  probe.authorityLockExact = true;
  probe.authorityFastNoOverlap = true;
  probe.lockEvidence.resource = input.resource;
  probe.lockEvidence.storage = input.storage;
  probe.lockEvidence.nativeD3DDevice = input.nativeD3DDevice;
  probe.lockEvidence.descFvf = input.descFvf;
  probe.lockEvidence.offset = input.offset;
  probe.lockEvidence.size = input.size;
  probe.lockEvidence.effectiveFlags = input.effectiveFlags;
  AddNativeTelemetryCounter(
      g_counters.productionOutsidePoisonAuthorityLockNoOverlap,
      t_nativeTelemetryDelta.productionOutsidePoisonAuthorityLockNoOverlap);
  return true;
}

void NotifyNativeOutsidePoisonVertexLock(
    const NativeOutsidePoisonVertexLockEvidence& evidence) noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (state.overflowDepth != 0u || state.depth == 0u)
    return;
  NativeOutsidePoisonShadowProbe& probe = state.probes[state.depth - 1u];
  const bool o0Enabled = OutsidePoisonProbeCollectsO0(probe);
  const bool o1Enabled = OutsidePoisonProbeCollectsO1(probe);
  const bool authorityEnabled = probe.authorityArmed;
  if (!o0Enabled && !o1Enabled && !authorityEnabled)
    return;
  if (probe.frozen) {
    if (o0Enabled) {
      MarkOutsidePoisonShadowUnprovable(
          probe, NativeOutsidePoisonShadowUnprovableReason::Reentry);
    }
    if (o1Enabled) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe, NativeOutsidePoisonO1ShadowUnprovableReason::Reentry);
    }
    if (authorityEnabled)
      probe.authorityConflicted = true;
    return;
  }
  if (o0Enabled) {
    g_counters.productionOutsidePoisonShadowLockNotifications.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (o1Enabled) {
    g_counters.productionOutsidePoisonO1ShadowLockNotifications.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (authorityEnabled) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsidePoisonAuthorityLockNotifications,
        t_nativeTelemetryDelta
            .productionOutsidePoisonAuthorityLockNotifications);
  }
  ++probe.lockNotifications;
  if (probe.lockNotifications == 1u) {
    probe.lockEvidence = evidence;
  } else {
    if (o0Enabled) {
      MarkOutsidePoisonShadowUnprovable(
          probe, NativeOutsidePoisonShadowUnprovableReason::MultipleLocks);
    }
    if (o1Enabled) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe, NativeOutsidePoisonO1ShadowUnprovableReason::MultipleLocks);
    }
    if (authorityEnabled)
      probe.authorityConflicted = true;
  }
  if (probe.ownerThreadId != ::GetCurrentThreadId()) {
    if (o0Enabled) {
      MarkOutsidePoisonShadowUnprovable(
          probe, NativeOutsidePoisonShadowUnprovableReason::OwnerOrLifo);
    }
    if (o1Enabled) {
      MarkOutsidePoisonO1ShadowUnprovable(
          probe, NativeOutsidePoisonO1ShadowUnprovableReason::OwnerOrLifo);
    }
  }
  if (o1Enabled && probe.lockNotifications == 1u)
    FreezeOutsidePoisonO1ShadowAtSuccessfulLock(probe);
  if (authorityEnabled && probe.lockNotifications == 1u)
    FreezeOutsidePoisonAuthorityAtSuccessfulLock(probe);
}

bool NativeOutsidePoisonVertexUnlockShadowRequiredSlow() noexcept {
  const NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  return state.overflowDepth == 0u && state.depth != 0u &&
      (OutsidePoisonProbeCollectsO1(state.probes[state.depth - 1u]) ||
       (state.probes[state.depth - 1u].authorityArmed &&
        !state.probes[state.depth - 1u].authorityFastNoOverlap));
}

bool NativeOutsidePoisonVertexUnlockFastNoOverlapRequiredSlow() noexcept {
  const NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  return state.overflowDepth == 0u && state.depth != 0u &&
      state.probes[state.depth - 1u].authorityArmed &&
      state.probes[state.depth - 1u].authorityFastNoOverlap &&
      !state.probes[state.depth - 1u].authorityUnlockTerminalRecorded;
}

bool NotifyNativeOutsidePoisonVertexUnlockFastNoOverlap(
    const NativeOutsidePoisonVertexUnlockFastInput& input) noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (state.overflowDepth != 0u || state.depth == 0u)
    return false;
  NativeOutsidePoisonShadowProbe& probe = state.probes[state.depth - 1u];
  if (!probe.authorityArmed || !probe.authorityFastNoOverlap ||
      probe.authorityUnlockTerminalRecorded)
    return false;

  probe.authorityUnlockTerminalRecorded = true;
  AddNativeTelemetryCounter(
      g_counters.productionOutsidePoisonAuthorityUnlockNotifications,
      t_nativeTelemetryDelta
          .productionOutsidePoisonAuthorityUnlockNotifications);
  ++probe.o1UnlockNotifications;
  const NativeOutsidePoisonVertexLockEvidence& lock = probe.lockEvidence;
  probe.authorityUnlockExact = probe.o1UnlockNotifications == 1u &&
      probe.authorityKernelExact && probe.kernelNormalSeen && probe.frozen &&
      probe.ownerThreadId == ::GetCurrentThreadId() && input.result >= 0 &&
      input.resource.commonResource != 0u &&
      input.resource.commonResource == lock.resource.commonResource &&
      input.resource.comVertexBuffer != 0u &&
      input.resource.comVertexBuffer == lock.resource.comVertexBuffer &&
      input.resource.resourceGeneration != 0u &&
      input.resource.resourceGeneration ==
          lock.resource.resourceGeneration &&
      input.nativeD3DDevice != 0u &&
      input.nativeD3DDevice == lock.nativeD3DDevice &&
      input.activeLockOwnerIdentity == lock.resource.comVertexBuffer &&
      input.activeLockOffset == lock.offset &&
      input.activeLockSize == lock.size &&
      input.activeLockFlags == lock.effectiveFlags &&
      input.entryLockDepth == 1u && input.completionLockDepth == 0u;
  if (probe.authorityUnlockExact) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsidePoisonAuthorityUnlockExact,
        t_nativeTelemetryDelta.productionOutsidePoisonAuthorityUnlockExact);
  } else {
    probe.authorityConflicted = true;
    g_counters.productionOutsidePoisonAuthorityUnlockRejects.fetch_add(
        1u, std::memory_order_relaxed);
  }
  return true;
}

void NotifyNativeOutsidePoisonVertexUnlock(
    const NativeOutsidePoisonVertexUnlockEvidence& evidence) noexcept {
  NativeOutsidePoisonShadowState& state = t_outsidePoisonShadow;
  if (state.overflowDepth != 0u || state.depth == 0u)
    return;

  NativeOutsidePoisonShadowProbe& probe = state.probes[state.depth - 1u];
  const bool o1Enabled = OutsidePoisonProbeCollectsO1(probe);
  const bool authorityEnabled = probe.authorityArmed;
  if (!o1Enabled && !authorityEnabled)
    return;
  if (o1Enabled) {
    g_counters.productionOutsidePoisonO1ShadowUnlockNotifications.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (authorityEnabled) {
    AddNativeTelemetryCounter(
        g_counters.productionOutsidePoisonAuthorityUnlockNotifications,
        t_nativeTelemetryDelta
            .productionOutsidePoisonAuthorityUnlockNotifications);
  }
  ++probe.o1UnlockNotifications;
  if (probe.o1UnlockNotifications == 1u) {
    probe.o1UnlockEvidence = evidence;
    probe.o1UnlockAfterFreeze = probe.o1Frozen && probe.frozen;
  }
  if (authorityEnabled && !probe.authorityUnlockTerminalRecorded) {
    probe.authorityUnlockTerminalRecorded = true;
    const NativeOutsidePoisonO1UnlockDriftStatus drift =
        GetOutsidePoisonO1UnlockDriftStatus(probe);
    probe.authorityUnlockExact = probe.o1UnlockNotifications == 1u &&
        probe.authorityKernelExact && probe.kernelNormalSeen && probe.frozen &&
        evidence.result >= 0 && OutsidePoisonO1UnlockIdentityExact(probe) &&
        OutsidePoisonO1UnlockHardFirstCause(drift) ==
            NativeOutsidePoisonO1UnlockDriftComponent::Count;
    if (probe.authorityUnlockExact) {
      AddNativeTelemetryCounter(
          g_counters.productionOutsidePoisonAuthorityUnlockExact,
          t_nativeTelemetryDelta.productionOutsidePoisonAuthorityUnlockExact);
    } else {
      probe.authorityConflicted = true;
      g_counters.productionOutsidePoisonAuthorityUnlockRejects.fetch_add(
          1u, std::memory_order_relaxed);
    }
  } else if (authorityEnabled) {
    probe.authorityConflicted = true;
  }
}

void EndNativeFlushTransaction() noexcept {
  if (t_state.flushTransactionDepth == 0u)
    return;
  --t_state.flushTransactionDepth;
  g_activeFlushTransactions.fetch_sub(1u, std::memory_order_seq_cst);
  ProcessPendingBridgeReset();
}

void ReportNativeDispatchSemanticFailures(uint32_t failureMask) {
  if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst))
    return;
  if ((failureMask & NativeDispatchSemanticFailureQueryMiss) != 0u)
    g_counters.queryMiss.fetch_add(1u, std::memory_order_relaxed);
  if ((failureMask & NativeDispatchSemanticFailureConflict) != 0u)
    g_counters.conflict.fetch_add(1u, std::memory_order_relaxed);
  if ((failureMask & NativeDispatchSemanticFailureUnknown) != 0u)
    g_counters.unknown.fetch_add(1u, std::memory_order_relaxed);
  if ((failureMask & NativeDispatchSemanticFailureLayerMismatch) != 0u)
    g_counters.layerMismatch.fetch_add(1u, std::memory_order_relaxed);
}

uint64_t BeginNativeDispatchScope(NativeDispatchPath path,
                                  uintptr_t sceneNode,
                                  uintptr_t renderablePart,
                                  uint32_t layerIndex,
                                  int32_t stage,
                                  int32_t batchTag,
                                  bool forceFailClosed) {
  // Disabled is immutable after InitializeNativeBridge's call_once and can
  // never open transaction ingress. Avoid the otherwise-empty reset/atomic/
  // timing path without weakening hook-removal or reset transition handling.
  if (g_runtimeConfig.mode == GpuSkinMode::Disabled)
    return 0u;

  const bool productionTimingSampled = SampleNextProductionEventOrdinal(
      t_state.productionDispatchBeginTimingOrdinal);
  const int64_t productionTimingStart = productionTimingSampled
      ? dxvk::high_resolution_clock::get_counter() : 0;
  const uint64_t cookie = BeginDispatchScope(
      path, sceneNode, renderablePart, layerIndex, stage, batchTag,
      forceFailClosed);
  if (productionTimingSampled) {
    const int64_t productionTimingEnd =
        dxvk::high_resolution_clock::get_counter();
    RecordNativeProductionTiming(
        NativeProductionTimingStage::DispatchBeginRoot,
        ProductionEventElapsedTicks(
            productionTimingStart, productionTimingEnd));
  }
  return cookie;
}

void UpdateNativeDispatchScope(uint64_t cookie,
                               int32_t stage,
                               int32_t batchTag) {
  DispatchFrame* frame = CurrentDispatch();
  if (frame == nullptr || frame->observation.epoch.dispatchEpoch != cookie)
    return;
  frame->observation.stage = stage;
  frame->observation.batchTag = batchTag;
}

void EndNativeDispatchScope(uint64_t cookie) {
  // EndDispatchScope is already a no-op for a zero cookie. Return before the
  // production timing sampler so a disabled/rejected Begin stays truly empty.
  if (cookie == 0u)
    return;

  const bool productionTimingSampled = SampleNextProductionEventOrdinal(
      t_state.productionDispatchEndTimingOrdinal);
  const int64_t productionTimingStart = productionTimingSampled
      ? dxvk::high_resolution_clock::get_counter() : 0;
  EndDispatchScope(cookie);
  if (productionTimingSampled) {
    const int64_t productionTimingEnd =
        dxvk::high_resolution_clock::get_counter();
    RecordNativeProductionTiming(
        NativeProductionTimingStage::DispatchEndRoot,
        ProductionEventElapsedTicks(
            productionTimingStart, productionTimingEnd));
  }
}

NativeDispatchScope::NativeDispatchScope(NativeDispatchPath path,
                                         uintptr_t sceneNode,
                                         uintptr_t renderablePart,
                                         uint32_t layerIndex,
                                         int32_t stage,
                                         int32_t batchTag,
                                         bool forceFailClosed,
                                         bool borrowedFlushPinPassThrough)
    : m_borrowedFlushPinPassThrough(borrowedFlushPinPassThrough) {
  if (m_borrowedFlushPinPassThrough) {
    m_previousBorrowedFlushPinPassThrough =
        g_nativeGpuSkinB1BorrowedFlushPinPassThrough;
    g_nativeGpuSkinB1BorrowedFlushPinPassThrough = true;
    return;
  }
  m_cookie = BeginNativeDispatchScope(path, sceneNode, renderablePart,
                                       layerIndex, stage, batchTag,
                                       forceFailClosed);
}

NativeDispatchScope::~NativeDispatchScope() {
  if (m_borrowedFlushPinPassThrough) {
    g_nativeGpuSkinB1BorrowedFlushPinPassThrough =
        m_previousBorrowedFlushPinPassThrough;
  } else {
    EndNativeDispatchScope(m_cookie);
  }
}

void NativeDispatchScope::updateStage(int32_t stage) {
  if (m_borrowedFlushPinPassThrough)
    return;
  DispatchFrame* frame = CurrentDispatch();
  const int32_t batchTag = frame != nullptr
      ? frame->observation.batchTag
      : -1;
  UpdateNativeDispatchScope(m_cookie, stage, batchTag);
}

void NativeDispatchScope::updateBatchTag(int32_t batchTag) {
  if (m_borrowedFlushPinPassThrough)
    return;
  DispatchFrame* frame = CurrentDispatch();
  const int32_t stage = frame != nullptr ? frame->observation.stage : -1;
  UpdateNativeDispatchScope(m_cookie, stage, batchTag);
}

NativeSemanticScope::NativeSemanticScope(uintptr_t geosetData,
                                         uintptr_t layerDispatch,
                                         uintptr_t layerState,
                                         uintptr_t callerAddress)
    : m_cookie(BeginSemanticScope(geosetData, layerDispatch, layerState,
                                  callerAddress)) {
}

NativeSemanticScope::~NativeSemanticScope() {
  EndSemanticScope(m_cookie);
}

NativeUploadObservation BeginNativeUpload(const NativeUploadCall& call) {
  AtomicRawTickAccumulator timing(
      g_runtimeConfig.fullDiagnostics,
      g_counters.beginUploadTimingCalls,
      g_counters.beginUploadTimingTicks,
      g_counters.beginUploadTimingMaxTicks);
  const bool sampleBeginTiming =
      g_runtimeConfig.mode == GpuSkinMode::Bypass &&
      (++t_state.beginTimingSampleOrdinal %
       kNativeBeginTimingSamplePeriod) == 0u;
  const int64_t beginSampleStart = sampleBeginTiming
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  NativeUploadObservation observation = {};
  observation.outsideAdmissionPopulation = call.outsideAdmissionPopulation;
  ProcessPendingBridgeReset();
  if (!g_transactionIngressEnabled.load(std::memory_order_seq_cst)) {
    NativeOutsideUploadPopulationToken& population =
        observation.outsideAdmissionPopulation;
    if (population.populationClass ==
            NativeOutsideUploadPopulationClass::Tracked &&
        !population.lifecycleAlreadyExcluded) {
      // TryBegin may have published a tracked first-terminal immediately
      // before reset/drain closed ingress. Settle that already-counted attempt
      // exactly once even though this observation can never allocate an epoch
      // or reach PublishCompletedUpload.
      RecordNativeOutsideAdmissionLifecycleExcluded();
      population.lifecycleAlreadyExcluded = true;
    }
    return observation;
  }

  const bool scopeOverflow = t_state.dispatchOverflowDepth != 0u ||
      SemanticScopeHazardActive();
  DispatchFrame* dispatch = CurrentDispatch();
  SemanticFrame* semantic = CurrentSemantic();
  if (semantic != nullptr &&
      (dispatch == nullptr || semantic->dispatchEpoch == 0u ||
       semantic->dispatchEpoch != dispatch->observation.epoch.dispatchEpoch)) {
    semantic = nullptr;
  }
  // A committed exact-negative seal owns only the independent native-CPU
  // route. Reaching generic upload observation means that proof no longer
  // covers this dispatch; revoke it permanently before allocating any ABI-9
  // upload ordinal or callback-visible state.
  if (dispatch != nullptr && dispatch->cpuOnlySeal.committed)
    InvalidateDispatchCpuOnlySeal(*dispatch);
  if (dispatch != nullptr &&
      dispatch->managerState == ManagerDispatchState::PendingCommon) {
    // ABI 9 has no prefix counts: publish the deferred begin before finalizing
    // an older upload, allocating this upload's ordinal, or exposing any
    // upload/DIP/fan-out callback. Failure marks the physical frame fail-closed
    // and all manager authorization remains unavailable.
    IssueManagerDispatchBegin(*dispatch, true);
  }
  if (dispatch != nullptr)
    FinalizeActiveUpload(*dispatch);

  observation.mode = g_runtimeConfig.mode;
  observation.epoch.renderThreadId = ::GetCurrentThreadId();
  observation.epoch.flushEpoch = t_state.flushEpoch;
  observation.epoch.dispatchEpoch = dispatch != nullptr
      ? dispatch->observation.epoch.dispatchEpoch
      : 0u;
  observation.epoch.uploadEpoch =
      g_nextUploadEpoch.fetch_add(1u, std::memory_order_relaxed) + 1u;
  observation.epoch.uploadOrdinal = dispatch != nullptr
      ? ++dispatch->uploadCount
      : ++t_state.orphanUploadOrdinal;
  observation.path = semantic != nullptr
      ? semantic->path
      : (dispatch != nullptr ? dispatch->observation.path
                             : NativeDispatchPath::Unknown);
  observation.scopeFailClosed = scopeOverflow ||
      (dispatch != nullptr && dispatch->observation.failClosed) ||
      (semantic != nullptr && semantic->failClosed) ||
      g_resetRequestedGeneration.load(std::memory_order_acquire) !=
          g_resetCompletedGeneration.load(std::memory_order_acquire);
  if (observation.scopeFailClosed) {
    g_counters.scopeFailClosedUploads.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (dispatch != nullptr) {
    observation.sceneNode = dispatch->observation.sceneNode;
    observation.renderablePart = dispatch->observation.renderablePart;
    observation.stage = dispatch->observation.stage;
    observation.batchTag = dispatch->observation.batchTag;
  }
  if (semantic != nullptr) {
    observation.geosetData = semantic->geosetData;
    observation.layerDispatch = semantic->layerDispatch;
    observation.layerState = semantic->layerState;
    observation.semanticCaller = semantic->callerAddress;
  }

  observation.gxDeviceD3d = call.gxDeviceD3d;
  observation.positions = call.positions;
  observation.normals = call.normals;
  observation.extra = call.extra;
  observation.groupSlots = call.groupSlots;
  observation.uv0 = call.uv0;
  observation.uv1 = call.uv1;
  observation.vertexCount = call.vertexCount;
  observation.positionStride = call.positionStride;
  observation.normalStride = call.normalStride;
  observation.extraStride = call.extraStride;
  observation.groupSlotStride = call.groupSlotStride;
  observation.uv0Stride = call.uv0Stride;
  observation.uv1Stride = call.uv1Stride;

  const bool singlePassCaller =
      semantic != nullptr && semantic->singlePassCaller;
  const bool productionBypass = observation.mode == GpuSkinMode::Bypass;
  const auto markProductionFastReject = [&](bool stateReadAttempted) {
    observation.productionFastRejected = true;
    // Preserve the existing exact 1/127 generic evidence path end-to-end. The
    // epoch is consumed by the outer in-flight RAII and never participates in
    // authorization, poison ownership or callback identity.
    if (sampleBeginTiming) {
      t_state.productionEvidenceUploadEpoch =
          observation.epoch.uploadEpoch;
    }
    if (t_state.poisonLedger.count == 0u)
      return;
    if (!stateReadAttempted)
      ReadNativeDeviceState(observation, false);
    observation.poisonRetirementOnly =
        !observation.nativeDeviceStateReadable ||
        PredictedCpuUploadMayOverlapPoison(observation);
    if (observation.poisonRetirementOnly) {
      g_counters.productionPoisonRetirementOnly.fetch_add(
          1u, std::memory_order_relaxed);
    }
  };

  // Observe/Dual are evidence modes and deliberately retain their complete
  // state capture. Production bypass, however, must not pay Game.dll memory
  // proof costs for uploads that TLS scope semantics already reject.
  observation.observedPreflight =
      BuildNativeScopePreflight(observation, singlePassCaller);
  const int64_t commonSampleEnd = sampleBeginTiming
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  if (sampleBeginTiming) {
    RecordSampledTiming(
        g_counters.beginSampleCommonCalls,
        g_counters.beginSampleCommonTicks,
        g_counters.beginSampleCommonMaxTicks,
        beginSampleStart, commonSampleEnd);
  }
  if (productionBypass &&
      (observation.scopeFailClosed ||
       (observation.observedPreflight &
        kNativeUploadScopeRequiredPreflight) !=
           kNativeUploadScopeRequiredPreflight)) {
    g_counters.productionFastRejectScope.fetch_add(
        1u, std::memory_order_relaxed);
    markProductionFastReject(false);
    if (sampleBeginTiming) {
      RecordSampledTiming(
          g_counters.beginSampleScopeRouteCalls,
          g_counters.beginSampleScopeRouteTicks,
          g_counters.beginSampleScopeRouteMaxTicks,
          beginSampleStart, dxvk::high_resolution_clock::get_counter());
    }
    return observation;
  }

  if (!productionBypass) {
    CaptureGeosetSnapshot(
        observation.geosetData, observation.epoch.uploadEpoch,
        t_state.pendingGeosetSnapshot);
    if (ReadNativeDeviceState(observation, false)) {
      if (g_resetRequestedGeneration.load(std::memory_order_acquire) !=
          g_resetCompletedGeneration.load(std::memory_order_acquire)) {
        observation.scopeFailClosed = true;
      }
    }
    observation.observedPreflight = BuildNativePreflight(
        observation, call, singlePassCaller,
        &t_state.pendingGeosetSnapshot,
        NativePaletteReadabilityPolicy::MetadataRange, nullptr);
    return observation;
  }

  // T1：先读取连续的 skinMode/format，尽早丢弃绝大多数非蒙皮上传。
  // 完整 GX 快照、Geoset 快照和范围证明只保留给真正的候选。
  const int64_t stateSampleStart = sampleBeginTiming
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  const bool skinFormatReadable = ReadNativeSkinFormatState(observation);
  const int64_t stateSampleEnd = sampleBeginTiming
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  if (sampleBeginTiming) {
    RecordSampledTiming(
        g_counters.beginSampleStateCalls,
        g_counters.beginSampleStateTicks,
        g_counters.beginSampleStateMaxTicks,
        stateSampleStart, stateSampleEnd);
  }
  if (!skinFormatReadable) {
    g_counters.productionFastRejectState.fetch_add(
        1u, std::memory_order_relaxed);
    markProductionFastReject(true);
    if (sampleBeginTiming) {
      RecordSampledTiming(
          g_counters.beginSampleStateRejectRouteCalls,
          g_counters.beginSampleStateRejectRouteTicks,
          g_counters.beginSampleStateRejectRouteMaxTicks,
          beginSampleStart, dxvk::high_resolution_clock::get_counter());
    }
    return observation;
  }
  if (observation.skinMode == 1u)
    observation.observedPreflight |= NativePreflightSkinMode1;
  if (observation.outputFormat < kFormatFvf.size())
    observation.observedPreflight |= NativePreflightSupportedFormat;
  if (observation.skinMode != 1u ||
      observation.outputFormat >= kFormatFvf.size()) {
    g_counters.productionFastRejectSkinFormat.fetch_add(
        1u, std::memory_order_relaxed);
    markProductionFastReject(true);
    if (sampleBeginTiming) {
      RecordSampledTiming(
          g_counters.beginSampleSkinRouteCalls,
          g_counters.beginSampleSkinRouteTicks,
          g_counters.beginSampleSkinRouteMaxTicks,
          beginSampleStart, dxvk::high_resolution_clock::get_counter());
    }
    return observation;
  }

  // The manager's production policy already keeps these small geosets on the
  // native SSE kernel.  Reject them here before the full GX snapshot, the
  // 0xE8 geoset snapshot and the four fault-closed input-range proofs.  This
  // path never authorizes a skip.  If an old poisoned range can overlap,
  // markProductionFastReject preserves the existing retirement-only proof.
  if (call.vertexCount < kProductionGpuMinVertices) {
    g_counters.productionFastRejectSmall.fetch_add(
        1u, std::memory_order_relaxed);
    markProductionFastReject(true);
    if (sampleBeginTiming) {
      RecordSampledTiming(
          g_counters.beginSampleSmallRouteCalls,
          g_counters.beginSampleSmallRouteTicks,
          g_counters.beginSampleSmallRouteMaxTicks,
          beginSampleStart, dxvk::high_resolution_clock::get_counter());
    }
    return observation;
  }

  // 候选仍然必须通过原来的完整快照；这里没有把任何字段默认为成功，
  // 因此快照失败仍只会进入原生 CPU 路径。
  const bool nativeStateReadable = ReadNativeDeviceState(observation, false);
  if (!nativeStateReadable) {
    g_counters.productionFastRejectState.fetch_add(
        1u, std::memory_order_relaxed);
    markProductionFastReject(true);
    if (sampleBeginTiming) {
      RecordSampledTiming(
          g_counters.beginSampleStateRejectRouteCalls,
          g_counters.beginSampleStateRejectRouteTicks,
          g_counters.beginSampleStateRejectRouteMaxTicks,
          beginSampleStart, dxvk::high_resolution_clock::get_counter());
    }
    return observation;
  }

  // T2: only a scope+skin candidate receives exact geoset/input proofs and is
  // admitted to the kernel-time authorization transaction.
  const int64_t exactSampleStart = sampleBeginTiming
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  NativeT2SampleDurations t2SampleDurations = {};
  const int64_t geoSnapSampleStart = sampleBeginTiming
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  CaptureGeosetSnapshot(
      observation.geosetData, observation.epoch.uploadEpoch,
      t_state.pendingGeosetSnapshot);
  if (sampleBeginTiming) {
    t2SampleDurations.geoSnapTicks = SampleDurationTicks(
        geoSnapSampleStart, dxvk::high_resolution_clock::get_counter());
  }
  observation.observedPreflight = BuildNativePreflight(
      observation, call, singlePassCaller,
      &t_state.pendingGeosetSnapshot,
      NativePaletteReadabilityPolicy::FaultSafeCopy,
      sampleBeginTiming ? &t2SampleDurations : nullptr);
  const int64_t exactSampleEnd = sampleBeginTiming
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  if (sampleBeginTiming) {
    RecordSampledTiming(
        g_counters.beginSampleExactCalls,
        g_counters.beginSampleExactTicks,
        g_counters.beginSampleExactMaxTicks,
        exactSampleStart, exactSampleEnd);
    RecordSampledDuration(
        g_counters.t2SampleGeoSnapCalls,
        g_counters.t2SampleGeoSnapTicks,
        g_counters.t2SampleGeoSnapMaxTicks,
        t2SampleDurations.geoSnapTicks);
    RecordSampledDuration(
        g_counters.t2SampleGeoHeaderCalls,
        g_counters.t2SampleGeoHeaderTicks,
        g_counters.t2SampleGeoHeaderMaxTicks,
        t2SampleDurations.geoHeaderTicks);
    RecordSampledDuration(
        g_counters.t2SamplePositionProofCalls,
        g_counters.t2SamplePositionProofTicks,
        g_counters.t2SamplePositionProofMaxTicks,
        t2SampleDurations.positionProofTicks);
    RecordSampledDuration(
        g_counters.t2SampleNormalProofCalls,
        g_counters.t2SampleNormalProofTicks,
        g_counters.t2SampleNormalProofMaxTicks,
        t2SampleDurations.normalProofTicks);
    RecordSampledDuration(
        g_counters.t2SampleGroupProofCalls,
        g_counters.t2SampleGroupProofTicks,
        g_counters.t2SampleGroupProofMaxTicks,
        t2SampleDurations.groupProofTicks);
    RecordSampledDuration(
        g_counters.t2SamplePaletteProofCalls,
        g_counters.t2SamplePaletteProofTicks,
        g_counters.t2SamplePaletteProofMaxTicks,
        t2SampleDurations.paletteProofTicks);
  }
  if ((observation.observedPreflight &
       kNativeUploadInputRequiredPreflight) !=
      kNativeUploadInputRequiredPreflight) {
    g_counters.productionFastRejectInput.fetch_add(
        1u, std::memory_order_relaxed);
    markProductionFastReject(true);
  } else {
    g_counters.productionCandidates.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (sampleBeginTiming) {
    RecordSampledTiming(
        g_counters.beginSampleCandidateRouteCalls,
        g_counters.beginSampleCandidateRouteTicks,
        g_counters.beginSampleCandidateRouteMaxTicks,
        beginSampleStart, dxvk::high_resolution_clock::get_counter());
  }
  return observation;
}


NativeUploadInFlightScope::NativeUploadInFlightScope(
    NativeUploadObservation& observation)
    : m_observation(&observation),
      m_cookie(BeginUploadInFlight(observation)) {
}

NativeUploadInFlightScope::~NativeUploadInFlightScope() {
  EndUploadInFlight(m_observation, m_cookie);
}

NativeKernelDetourOutcome EvaluateNativeSkinKernelDetour(
    uintptr_t gxDeviceD3d, void* mappedDst) {
  if (OutsidePoisonO1SidecarEnabled() ||
      g_nativeOutsidePoisonAuthorityActive.load(
          std::memory_order_acquire) != 0u) {
    CaptureOutsidePoisonO1KernelState(gxDeviceD3d, mappedDst);
  }
  NativeDispatchCpuOnlyUploadFastPathState& dispatchMarker =
      t_state.dispatchCpuOnlyUploadFastPath;
  const bool dispatchMarkerActive = dispatchMarker.cookie != 0u;
  DispatchFrame* sealedDispatch = dispatchMarkerActive
      ? CurrentDispatch() : nullptr;
  const SemanticFrame* sealedSemantic = dispatchMarkerActive
      ? CurrentSemantic() : nullptr;
  const bool dispatchSemanticExact = dispatchMarkerActive && (
      (dispatchMarker.semanticDepth == 0u &&
       t_state.semanticDepth == 0u && sealedSemantic == nullptr) ||
      (dispatchMarker.semanticDepth == 1u &&
       t_state.semanticDepth == 1u && sealedSemantic != nullptr &&
       dispatchMarker.semanticCookie != 0u &&
       sealedSemantic->cookie == dispatchMarker.semanticCookie &&
       sealedSemantic->dispatchEpoch == dispatchMarker.dispatchEpoch &&
       !sealedSemantic->failClosed &&
       sealedSemantic->path == NativeDispatchPath::Common));
  const bool dispatchMarkerExact = dispatchMarkerActive &&
      sealedDispatch != nullptr && sealedDispatch->cpuOnlySeal.committed &&
      DispatchCpuOnlySealPhysicalPathAllowed(*sealedDispatch) &&
      sealedDispatch->observation.epoch.dispatchEpoch ==
          dispatchMarker.dispatchEpoch &&
      sealedDispatch->cpuOnlyActiveUpload.valid &&
      dispatchMarker.gxDeviceD3d != 0u &&
      dispatchMarker.gxDeviceD3d == gxDeviceD3d &&
      dispatchMarker.renderThreadId == ::GetCurrentThreadId() &&
      !dispatchMarker.kernelSeen && !dispatchMarker.conflicted &&
      dispatchMarker.resetGeneration ==
          sealedDispatch->cpuOnlySeal.resetGeneration &&
      dispatchSemanticExact &&
      DispatchCpuOnlySealBaseSafetyExact(
          *sealedDispatch, 0u, true, true);
  if (dispatchMarkerExact) {
    dispatchMarker.kernelSeen = true;
    dispatchMarker.mappedDst = reinterpret_cast<uintptr_t>(mappedDst);
    AddNativeTelemetryCounter(
        g_counters.kernelHookCalls,
        t_nativeTelemetryDelta.kernelHookCalls);
    AddNativeTelemetryCounter(
        g_counters.originalKernelCalls,
        t_nativeTelemetryDelta.originalKernelCalls);
    AddNativeTelemetryCounter(
        g_counters.originalKernelBytes,
        t_nativeTelemetryDelta.originalKernelBytes,
        dispatchMarker.outputByteCount);
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealKernelCalls,
        t_nativeTelemetryDelta.dispatchCpuOnlySealKernelCalls);
    if (mappedDst == nullptr) {
      g_counters.nullMappedKernelFallbacks.fetch_add(
          1u, std::memory_order_relaxed);
    }
    return NativeKernelDetourOutcome::CallOriginalNoNotify;
  }
  if (dispatchMarkerActive) {
    if (!dispatchMarker.conflicted) {
      AddNativeTelemetryCounter(
          g_counters.dispatchCpuOnlySealMarkerConflicts,
          t_nativeTelemetryDelta.dispatchCpuOnlySealMarkerConflicts);
    }
    dispatchMarker.conflicted = true;
    if (sealedDispatch != nullptr)
      InvalidateDispatchCpuOnlySeal(*sealedDispatch);
  }

  // The outer upload hook proved that this production-light call has no
  // dispatch owner and at most one clean top-level semantic scope. When poison
  // exists, the provisional O1 transaction has now frozen this call's actual
  // D3D9 Lock and live-ledger verdict. Revalidate that exact TLS proof under
  // its already-published flush/upload transaction before avoiding the generic
  // authorization state machine.
  // Reset/retirement cannot complete or clear this TLS while that cover is
  // live, and this branch can only select the original native kernel; it never
  // authorizes a bypass. The native kernel is still called exactly once by
  // the hook.
  NativeOutsideUploadFastPathState& outsideMarker =
      t_state.outsideUploadFastPath;
  const bool outsideMarkerActive = outsideMarker.cookie != 0u;
  const SemanticFrame* liveSemantic = outsideMarkerActive
      ? CurrentSemantic() : nullptr;
  const bool outsideSemanticExact = outsideMarkerActive && (
      (outsideMarker.semanticDepth == 0u &&
       t_state.semanticDepth == 0u && liveSemantic == nullptr) ||
      (outsideMarker.semanticDepth == 1u &&
       t_state.semanticDepth == 1u && liveSemantic != nullptr &&
       outsideMarker.semanticCookie != 0u &&
       liveSemantic->cookie == outsideMarker.semanticCookie &&
       liveSemantic->dispatchEpoch == 0u && !liveSemantic->failClosed));
  const bool outsidePoisonAuthorityExact = outsideMarkerActive &&
      (!outsideMarker.admittedWithPoison ||
       OutsidePoisonAuthorityKernelReady(
           outsideMarker.poisonShadowCookie, gxDeviceD3d, mappedDst));
  const bool outsideMarkerExact = outsideMarkerActive &&
      outsideMarker.gxDeviceD3d != 0u &&
      outsideMarker.gxDeviceD3d == gxDeviceD3d &&
      outsideMarker.renderThreadId != 0u &&
      outsideMarker.renderThreadId == ::GetCurrentThreadId() &&
      !outsideMarker.kernelSeen && !outsideMarker.conflicted &&
      outsidePoisonAuthorityExact &&
      (outsideMarker.admittedWithPoison ||
       t_state.poisonLedger.count == 0u) &&
      outsideSemanticExact &&
      t_state.dispatchDepth == 0u &&
      t_state.dispatchOverflowDepth == 0u &&
      t_state.semanticOverflowDepth == 0u &&
      t_state.nestedUploadDepth == 0u &&
      t_state.inFlightUpload.observation == nullptr &&
      t_state.productionFastRejectedUpload == nullptr &&
      t_state.productionEvidenceUploadEpoch == 0u &&
      outsideMarker.resetGeneration == t_state.appliedResetGeneration &&
      OutsideUploadCoverStillExact(outsideMarker);
  if (outsideMarkerExact) {
    outsideMarker.kernelSeen = true;
    RecordNativeOutsideFastKernelTelemetry();
    if (mappedDst == nullptr) {
      g_counters.nullMappedKernelFallbacks.fetch_add(
          1u, std::memory_order_relaxed);
    }
    return NativeKernelDetourOutcome::CallOriginalNoNotify;
  }
  if (outsideMarkerActive) {
    // Do not consume or clear a mismatched marker. Its owning outer guard is
    // the only authority that can settle the exact cookie. The generic path
    // below still calls the original kernel; any nested observation is kept
    // CPU-only so no new poison can appear during the outer transaction.
    g_counters.productionOutsideFastKernelMarkerConflicts.fetch_add(
        1u, std::memory_order_relaxed);
    outsideMarker.conflicted = true;
  }
  AtomicRawTickAccumulator timing(
      g_runtimeConfig.fullDiagnostics,
      g_counters.evaluateKernelTimingCalls,
      g_counters.evaluateKernelTimingTicks,
      g_counters.evaluateKernelTimingMaxTicks);
  ApplyPendingResetFailClosed();
  NativeUploadObservation* productionReject =
      t_state.productionFastRejectedUpload;
  if (productionReject != nullptr &&
      t_state.nestedUploadDepth == 0u &&
      t_state.inFlightUpload.observation == nullptr &&
      productionReject->mode == GpuSkinMode::Bypass &&
      productionReject->epoch.renderThreadId == ::GetCurrentThreadId() &&
      productionReject->epoch.dispatchEpoch == 0u &&
      productionReject->gxDeviceD3d == gxDeviceD3d &&
      productionReject->productionFastRejected &&
      !productionReject->poisonRetirementOnly &&
      t_state.productionEvidenceUploadEpoch !=
          productionReject->epoch.uploadEpoch) {
    // ASM contract: the native 0x0EEB85 call remains exactly once and its
    // caller-owned 0x0EEB7B..0x0EEB8A SEH still owns NULL/fault behavior. This
    // route changes report-only management only; the hook still selects and
    // invokes the original trampoline, and Notify deliberately sees no generic
    // in-flight observation just as it did before this optimization.
    RecordNativeProductionFastRejectKernelTelemetry();
    if (mappedDst == nullptr) {
      g_counters.nullMappedKernelFallbacks.fetch_add(
          1u, std::memory_order_relaxed);
    }
    return NativeKernelDetourOutcome::CallOriginalNoNotify;
  }
  g_counters.kernelHookCalls.fetch_add(1u, std::memory_order_relaxed);
  NativeUploadObservation* observation = nullptr;
  if (t_state.nestedUploadDepth == 0u) {
    observation = t_state.inFlightUpload.observation;
  } else if (t_state.nestedUploadDepth <=
             t_state.nestedUploadObservations.size()) {
    observation =
        t_state.nestedUploadObservations[t_state.nestedUploadDepth - 1u];
  }
  const uint64_t byteCount = observation != nullptr
      ? NativeUploadByteCount(*observation)
      : 0u;
  const bool kernelWasAlreadyCalled =
      observation != nullptr && observation->cpuSkinKernelCalled;
  const auto suppressIrreversible = [&](NativeBypassFailureReason reason) {
    if (observation != nullptr) {
      observation->bypassFailure = reason;
      observation->scopeFailClosed = true;
    }
    g_counters.duplicateKernelCalls.fetch_add(1u,
                                               std::memory_order_relaxed);
    g_counters.irreversibleKernelSuppressions.fetch_add(
        1u, std::memory_order_relaxed);
    return NativeKernelDetourOutcome::IrreversibleNoCpuRescue;
  };
  const auto callOriginal = [&](NativeBypassFailureReason reason,
                                 bool preflightReject) {
    if (observation != nullptr && observation->cpuSkinKernelBypassed)
      return suppressIrreversible(reason);
    if (observation != nullptr) {
      observation->bypassFailure = reason;
      // This bit records selection only. The hook publishes normal return in
      // a separate post-trampoline notification that caller SEH skips.
      observation->cpuSkinOriginalTrampolineSelected = true;
      if (t_state.inFlightUpload.observation == observation)
        RecycleIndexSnapshotStorage(
            t_state.inFlightUpload.indexSnapshot);
    }
    g_counters.originalKernelCalls.fetch_add(1u,
                                               std::memory_order_relaxed);
    g_counters.originalKernelBytes.fetch_add(byteCount,
                                              std::memory_order_relaxed);
    if (preflightReject) {
      g_counters.kernelPreflightRejects.fetch_add(
          1u, std::memory_order_relaxed);
      g_counters.bypassFallbackCalls.fetch_add(
          1u, std::memory_order_relaxed);
    }
    return NativeKernelDetourOutcome::CallOriginal;
  };

  if (observation != nullptr && !observation->cpuSkinKernelCalled)
    observation->cpuSkinKernelCalled = true;

  if (observation != nullptr &&
      (kernelWasAlreadyCalled || observation->cpuSkinKernelBypassed ||
       observation->mappedDst != 0u)) {
    return suppressIrreversible(
        NativeBypassFailureReason::DuplicateKernelCall);
  }

  // The original code deliberately passes NULL through to the kernel so the
  // caller-owned SEH can absorb the first write fault. Never intercept it.
  if (mappedDst == nullptr) {
    g_counters.nullMappedKernelFallbacks.fetch_add(
        1u, std::memory_order_relaxed);
    return callOriginal(NativeBypassFailureReason::NullMappedDestination,
                        observation != nullptr &&
                            observation->mode == GpuSkinMode::Bypass);
  }

  if (observation == nullptr)
    return callOriginal(NativeBypassFailureReason::MissingInFlightUpload,
                        false);

  const uint64_t renderThreadId = ::GetCurrentThreadId();
  const bool threadMatch =
      t_state.inFlightUpload.renderThreadId == renderThreadId &&
      observation->epoch.renderThreadId == renderThreadId;
  const bool selfMatch =
      t_state.inFlightUpload.gxDeviceD3d == gxDeviceD3d &&
      observation->gxDeviceD3d == gxDeviceD3d;
  const bool epochMatch = t_state.inFlightUpload.uploadEpoch != 0u &&
      t_state.inFlightUpload.uploadEpoch == observation->epoch.uploadEpoch &&
      !t_state.inFlightUpload.conflicted;
  const bool outerInFlight =
      (observation->observedPreflight &
       NativePreflightOuterUploadInFlight) != 0u;
  if (observation->mode != GpuSkinMode::Bypass) {
    if (threadMatch && selfMatch && epochMatch && outerInFlight) {
      observation->cpuSkinOriginalRangeExact =
          RefreshKernelState(*observation, gxDeviceD3d, mappedDst);
    }
    return callOriginal(NativeBypassFailureReason::ModeNotBypass, false);
  }
  if (observation->poisonRetirementOnly) {
    if (threadMatch && selfMatch && epochMatch && outerInFlight) {
      observation->cpuSkinOriginalRangeExact =
          RefreshKernelState(*observation, gxDeviceD3d, mappedDst);
    }
    // This upload was rejected before bypass authorization. Its sole bridge
    // duty is to prove a normal CPU rewrite if it actually overlaps old poison.
    return callOriginal(NativeBypassFailureReason::InputPreflightFailed, false);
  }
  if (outsideMarkerActive) {
    observation->scopeFailClosed = true;
    g_counters.scopeFailClosedKernelFallbacks.fetch_add(
        1u, std::memory_order_relaxed);
    return callOriginal(
        NativeBypassFailureReason::DispatchScopeFailClosed, true);
  }
  if (!g_bypassEnabled.load(std::memory_order_acquire) ||
      g_resetRequestedGeneration.load(std::memory_order_acquire) !=
          g_resetCompletedGeneration.load(std::memory_order_acquire)) {
    return callOriginal(NativeBypassFailureReason::BridgeResetPending, true);
  }

  g_counters.bypassPreflightAttempts.fetch_add(
      1u, std::memory_order_relaxed);
  if (observation->scopeFailClosed) {
    g_counters.scopeFailClosedKernelFallbacks.fetch_add(
        1u, std::memory_order_relaxed);
    return callOriginal(NativeBypassFailureReason::DispatchScopeFailClosed,
                        true);
  }
  if (!ManagerDispatchBeginWasIssued(
          observation->epoch.dispatchEpoch)) {
    // No manager dispatch lifetime means there can be no ABI-9 authorization,
    // even if every native proof below would otherwise match.
    return callOriginal(NativeBypassFailureReason::ManagerRejected, true);
  }
  if (!threadMatch) {
    return callOriginal(NativeBypassFailureReason::ThreadMismatch, true);
  }
  if (!selfMatch) {
    return callOriginal(NativeBypassFailureReason::SelfMismatch, true);
  }
  if (!epochMatch) {
    return callOriginal(NativeBypassFailureReason::EpochMismatch, true);
  }
  if (!outerInFlight) {
    g_counters.bypassStateMismatches.fetch_add(
        1u, std::memory_order_relaxed);
    return callOriginal(NativeBypassFailureReason::NativeStateStale, true);
  }
  if ((observation->observedPreflight &
       kNativeUploadInputRequiredPreflight) !=
      kNativeUploadInputRequiredPreflight) {
    return callOriginal(NativeBypassFailureReason::InputPreflightFailed, true);
  }
  observation->cpuSkinOriginalRangeExact =
      RefreshKernelState(*observation, gxDeviceD3d, mappedDst);
  if (!observation->cpuSkinOriginalRangeExact) {
    g_counters.bypassStateMismatches.fetch_add(
        1u, std::memory_order_relaxed);
    return callOriginal(NativeBypassFailureReason::NativeStateStale, true);
  }

  // Freeze the exact INDEX16 source before asking the manager to authorize a
  // skip. The manager receives only hash/range diagnostics; this private byte
  // vector remains the sole ContentsExact authority through Unlock.
  NativeIndexSnapshotState* snapshotState =
      t_state.inFlightUpload.observation == observation &&
          t_state.inFlightUpload.uploadEpoch ==
              observation->epoch.uploadEpoch
      ? &t_state.inFlightUpload.indexSnapshot : nullptr;
  if (snapshotState == nullptr) {
    return callOriginal(NativeBypassFailureReason::NativeStateStale, true);
  }
  uint32_t capturedIndexCount = 0u;
  uint32_t predictedStartIndex = 0u;
  const NativeGeosetSnapshotState* geosetSnapshot =
      t_state.inFlightUpload.geosetSnapshot.geosetData ==
              observation->geosetData &&
          t_state.inFlightUpload.geosetSnapshot.uploadEpoch ==
              observation->epoch.uploadEpoch
      ? &t_state.inFlightUpload.geosetSnapshot : nullptr;
  const uint64_t indexProofFailure = PrepareExpectedIndexTicket(
      *observation, 0u, geosetSnapshot, *snapshotState,
      capturedIndexCount, predictedStartIndex);
  if (indexProofFailure != NativeIndexTicketFailureNone) {
    observation->indexTicket.failureMask |= indexProofFailure;
    RecordIndexTicketFailures(indexProofFailure);
    return callOriginal(NativeBypassFailureReason::AuthorizationMismatch,
                        true);
  }

  NativeBridgeCallbackPin callbackPin(
      NativeBridgeCallbackKind::Preflight,
      observation->epoch.uploadEpoch);
  const NativeBridgeCallbacks* callbacks = callbackPin.get();
  NativeBypassAuthorization authorization = {};
  const auto invokeBypassPreflight = [&]() {
    callbackPin.BeginCallbackBody();
    const bool accepted = callbacks->onBypassPreflight(
        callbacks->userData, *observation, &authorization);
    callbackPin.EndCallbackBody();
    return accepted;
  };
  if (callbacks == nullptr || callbacks->onBypassPreflight == nullptr ||
      !invokeBypassPreflight()) {
    return callOriginal(NativeBypassFailureReason::ManagerRejected, true);
  }
  if (!g_bypassEnabled.load(std::memory_order_acquire) ||
      g_resetRequestedGeneration.load(std::memory_order_acquire) !=
          g_resetCompletedGeneration.load(std::memory_order_acquire)) {
    return callOriginal(NativeBypassFailureReason::BridgeResetPending, true);
  }

  constexpr uint32_t requiredConsumers =
      static_cast<uint32_t>(GpuSkinConsumerBits::Main) |
      static_cast<uint32_t>(GpuSkinConsumerBits::Shadow) |
      static_cast<uint32_t>(GpuSkinConsumerBits::Outline);
  constexpr uint64_t managerApprovedPreflight =
      NativePreflightResourceGenerationReady |
      NativePreflightStaticResourceReady |
      NativePreflightPaletteCopied |
      NativePreflightGpuOutputReady |
      NativePreflightBatchSubmitted |
      NativePreflightExactGpuJob |
      NativePreflightStaticInputsMatch |
      NativePreflightAllConsumersLeased |
      NativePreflightHostDrawSafe |
      NativePreflightIndexPathPredicted |
      NativePreflightNoUnsupportedDrawPath |
      NativePreflightCpuBaselineExact |
      NativePreflightBypassKeyNotFused;
  const bool indexCountValid = capturedIndexCount != 0u &&
      capturedIndexCount <= kMaxNativeIndices &&
      (capturedIndexCount % 3u) == 0u &&
      authorization.expectedIndexCount == capturedIndexCount &&
      observation->indexRingBaseBefore <=
          observation->indexRingNextBefore &&
      observation->indexRingNextBefore <= kMaxNativeIndices;
  const NativeVertexOutputProof& vertexProof =
      authorization.vertexOutputProof;
  const bool vertexProofValid =
      vertexProof.resource.commonResource != 0u &&
      vertexProof.resource.comVertexBuffer != 0u &&
      vertexProof.resource.resourceGeneration != 0u &&
      vertexProof.resource.comVertexBuffer ==
          observation->nativeVertexBuffer &&
      vertexProof.nativeD3DDevice == observation->nativeD3DDevice &&
      vertexProof.outputFormat == observation->outputFormat &&
      vertexProof.vertexStride == observation->outputStride &&
      vertexProof.fvf == observation->fvf;
  if (authorization.fuseKey == 0u || authorization.token == 0u ||
      !indexCountValid || !vertexProofValid ||
      authorization.predictedStartIndex != predictedStartIndex ||
      authorization.requiredConsumerBits != requiredConsumers ||
      (authorization.approvedPreflight &
       NativePreflightIndexPathActualProof) != 0u ||
      (authorization.approvedPreflight & managerApprovedPreflight) !=
          managerApprovedPreflight) {
    return callOriginal(NativeBypassFailureReason::AuthorizationMismatch,
                        true);
  }

  observation->observedPreflight |=
      authorization.approvedPreflight &
      ~NativePreflightIndexPathActualProof;
  if ((observation->observedPreflight & kP4BypassRequiredPreflight) !=
      kP4BypassRequiredPreflight) {
    return callOriginal(NativeBypassFailureReason::InputPreflightFailed, true);
  }
  // IDA ASM fixes the logical stale-range commit at the return from the sole
  // 0x0EEB85 kernel call; 0x0EEBB6 Unlock follows on both normal and handled
  // exception paths. Reset publication and this final poison commit share one
  // protocol lock, eliminating the check-then-commit window.
  PoisonCreateResult poisonResult = PoisonCreateResult::InvalidRange;
  bool resetBlockedCommit = false;
  {
    ResetProtocolLockGuard protocolLock;
    if (!g_bypassEnabled.load(std::memory_order_acquire) ||
        g_resetRequestedGeneration.load(std::memory_order_acquire) !=
            g_resetCompletedGeneration.load(std::memory_order_acquire) ||
        g_retirementQueueFaulted.load(std::memory_order_acquire)) {
      resetBlockedCommit = true;
    } else {
      poisonResult = CreateNativePoisonRange(
          *observation, authorization.fuseKey, vertexProof,
          predictedStartIndex, authorization.expectedIndexCount);
    }
  }
  if (resetBlockedCommit) {
    g_counters.resetCommitBlocked.fetch_add(
        1u, std::memory_order_relaxed);
    return callOriginal(NativeBypassFailureReason::BridgeResetPending, true);
  }
  if (poisonResult == PoisonCreateResult::InvalidRange) {
    g_counters.bypassStateMismatches.fetch_add(
        1u, std::memory_order_relaxed);
    return callOriginal(NativeBypassFailureReason::NativeStateStale, true);
  }
  if (poisonResult == PoisonCreateResult::Overflow) {
    return callOriginal(NativeBypassFailureReason::PoisonLedgerOverflow,
                        true);
  }

  observation->bypassAuthorized = true;
  observation->vertexOutputProof = vertexProof;
  observation->bypassToken = authorization.token;
  observation->fuseKey = authorization.fuseKey;
  observation->expectedIndexCount = authorization.expectedIndexCount;
  observation->predictedIndexRingBase = predictedStartIndex;
  observation->predictedIndexRingNext = predictedStartIndex +
      authorization.expectedIndexCount;
  observation->cpuSkinKernelBypassed = true;
  observation->cpuSkinBytesSkipped = byteCount;
  observation->kernelAuthorizationPending = true;
  observation->bypassFailure = NativeBypassFailureReason::None;
  observation->requiredPreflight = kP4BypassRequiredPreflight |
      NativePreflightOriginalExecuted |
      NativePreflightOriginalSucceeded |
      NativePreflightNativeRingReady;
  g_counters.bypassAuthorizations.fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.bypassedKernelCalls.fetch_add(
      1u, std::memory_order_relaxed);
  g_counters.bypassedKernelBytes.fetch_add(
      byteCount, std::memory_order_relaxed);
  g_counters.pendingKernelAuthorizations.fetch_add(
      1u, std::memory_order_relaxed);
  return NativeKernelDetourOutcome::BypassCommitted;
}

void NotifyNativeCpuOnlySkinKernelOriginalReturned(
    uintptr_t gxDeviceD3d, void* mappedDst) noexcept {
  if (OutsidePoisonAnySidecarEnabled() ||
      g_nativeOutsidePoisonAuthorityActive.load(
          std::memory_order_acquire) != 0u) {
    FreezeOutsidePoisonShadowAtKernelNormalReturn(gxDeviceD3d, mappedDst);
  }
  NativeDispatchCpuOnlyUploadFastPathState& marker =
      t_state.dispatchCpuOnlyUploadFastPath;
  if (marker.cookie == 0u)
    return;
  DispatchFrame* frame = CurrentDispatch();
  const bool exact = frame != nullptr && frame->cpuOnlySeal.committed &&
      DispatchCpuOnlySealPhysicalPathAllowed(*frame) &&
      frame->observation.epoch.dispatchEpoch == marker.dispatchEpoch &&
      frame->cpuOnlyActiveUpload.valid && marker.kernelSeen &&
      !marker.kernelNormalReturned && !marker.conflicted &&
      marker.gxDeviceD3d == gxDeviceD3d &&
      marker.mappedDst == reinterpret_cast<uintptr_t>(mappedDst) &&
      marker.renderThreadId == ::GetCurrentThreadId() &&
      marker.resetGeneration == frame->cpuOnlySeal.resetGeneration &&
      DispatchCpuOnlySealBaseSafetyExact(
          *frame, 0u, true, true);
  if (!exact) {
    if (!marker.conflicted) {
      AddNativeTelemetryCounter(
          g_counters.dispatchCpuOnlySealMarkerConflicts,
          t_nativeTelemetryDelta.dispatchCpuOnlySealMarkerConflicts);
    }
    marker.conflicted = true;
    if (frame != nullptr)
      InvalidateDispatchCpuOnlySeal(*frame);
    return;
  }
  marker.kernelNormalReturned = true;
  AddNativeTelemetryCounter(
      g_counters.dispatchCpuOnlySealKernelNormalReturns,
      t_nativeTelemetryDelta.dispatchCpuOnlySealKernelNormalReturns);
}

void NotifyNativeSkinKernelOriginalReturned(
    uintptr_t gxDeviceD3d, void* mappedDst) noexcept {
  if (OutsidePoisonAnySidecarEnabled() ||
      g_nativeOutsidePoisonAuthorityActive.load(
          std::memory_order_acquire) != 0u) {
    FreezeOutsidePoisonShadowAtKernelNormalReturn(gxDeviceD3d, mappedDst);
  }
  AtomicRawTickAccumulator timing(
      g_runtimeConfig.fullDiagnostics,
      g_counters.notifyNormalTimingCalls,
      g_counters.notifyNormalTimingTicks,
      g_counters.notifyNormalTimingMaxTicks);
  NativeUploadObservation* observation = nullptr;
  if (t_state.nestedUploadDepth == 0u) {
    observation = t_state.inFlightUpload.observation;
  } else if (t_state.nestedUploadDepth <=
             t_state.nestedUploadObservations.size()) {
    observation =
        t_state.nestedUploadObservations[t_state.nestedUploadDepth - 1u];
  }
  // A production fast reject intentionally has no in-flight observation. Its
  // original kernel return is expected and needs no mismatch accounting.
  if (observation == nullptr)
    return;
  if (observation->gxDeviceD3d != gxDeviceD3d ||
      !observation->cpuSkinKernelCalled ||
      !observation->cpuSkinOriginalTrampolineSelected ||
      observation->cpuSkinKernelBypassed ||
      observation->cpuSkinOriginalReturnedNormally) {
    g_counters.originalKernelNormalReturnRejects.fetch_add(
        1u, std::memory_order_relaxed);
    return;
  }
  if (observation->mappedDst != 0u &&
      observation->mappedDst != reinterpret_cast<uintptr_t>(mappedDst)) {
    g_counters.originalKernelNormalReturnRejects.fetch_add(
        1u, std::memory_order_relaxed);
    return;
  }
  observation->cpuSkinOriginalReturnedNormally = true;
  g_counters.originalKernelNormalReturns.fetch_add(
      1u, std::memory_order_relaxed);

  // 0x6F0EEB8A has not executed yet and 0x6F0EEBB6 Unlock is still ahead.
  // Resolve the exact D3D9 Lock/resource identity now; CompleteNativeUpload
  // may only consume this frozen proof after the outer call succeeds.
  // A proof can clear poison only when the already-frozen COM/layout/range
  // overlaps at least one ledger entry. This conservative TLS scan avoids a
  // host callback, mutex and resource lookup for unrelated normal uploads.
  if (t_state.poisonLedger.count == 0u ||
      !observation->cpuSkinOriginalRangeExact ||
      observation->mappedDst == 0u ||
      !CpuUploadMayOverlapPoison(*observation)) {
    return;
  }
  g_counters.cpuRewriteProofAttempts.fetch_add(
      1u, std::memory_order_relaxed);
  if (observation->epoch.dispatchEpoch != 0u &&
      !ManagerDispatchBeginWasIssued(
          observation->epoch.dispatchEpoch)) {
    g_counters.cpuRewriteProofRejects.fetch_add(
        1u, std::memory_order_relaxed);
    return;
  }

  NativeBridgeCallbackPin callbackPin(
      NativeBridgeCallbackKind::CpuRewrite,
      observation->epoch.uploadEpoch);
  const NativeBridgeCallbacks* callbacks = callbackPin.get();
  NativeCpuRewriteOutputProof proof = {};
  const auto invokeCpuRewriteProof = [&]() {
    callbackPin.BeginCallbackBody();
    const bool resolved = callbacks->resolveCpuRewriteOutputProof(
        callbacks->userData, *observation, &proof);
    callbackPin.EndCallbackBody();
    return resolved;
  };
  if (callbacks == nullptr ||
      callbacks->resolveCpuRewriteOutputProof == nullptr ||
      !invokeCpuRewriteProof() ||
      !CpuRewriteProofMatchesUpload(*observation, proof)) {
    g_counters.cpuRewriteProofRejects.fetch_add(
        1u, std::memory_order_relaxed);
    return;
  }
  observation->cpuRewriteOutputProof = proof;
  observation->cpuRewriteOutputProofExact = true;
  g_counters.cpuRewriteProofExact.fetch_add(
      1u, std::memory_order_relaxed);
}

void CompleteNativeUpload(NativeUploadObservation& observation,
                           int32_t originalResult) {
  AtomicRawTickAccumulator timing(
      g_runtimeConfig.fullDiagnostics,
      g_counters.completeUploadTimingCalls,
      g_counters.completeUploadTimingTicks,
      g_counters.completeUploadTimingMaxTicks);
  if (observation.epoch.uploadEpoch == 0u || observation.completionPublished)
    return;

  ApplyPendingResetFailClosed();

  // The observation still holds the kernel-entry ring snapshot here. A real
  // original call with non-null mappedDst writes mode 0/1 before the native
  // 0x0EEBB6 Unlock, so subtract only this exact completed coverage.
  const bool exactCpuRewrite = !observation.cpuSkinKernelBypassed &&
      observation.cpuSkinOriginalTrampolineSelected &&
      observation.cpuSkinOriginalReturnedNormally &&
      observation.cpuSkinOriginalRangeExact &&
      observation.cpuRewriteOutputProofExact &&
      CpuRewriteProofMatchesUpload(
          observation, observation.cpuRewriteOutputProof) &&
      observation.mappedDst != 0u &&
      observation.vertexCount != 0u && observation.skinMode <= 1u &&
      originalResult >= 0;
  if (exactCpuRewrite)
    ClearPoisonCoveredByCpuUpload(
        observation, observation.cpuRewriteOutputProof);

  observation.originalUploadExecuted = true;
  observation.originalResult = originalResult;
  observation.observedPreflight |= NativePreflightOriginalExecuted;
  if (observation.cpuSkinOriginalReturnedNormally)
    observation.observedPreflight |=
        NativePreflightOriginalKernelReturnedNormally;
  if (!observation.productionFastRejected ||
      observation.poisonRetirementOnly) {
    ReadNativeDeviceState(observation, true);
  } else if (t_state.pendingGeosetSnapshot.uploadEpoch ==
             observation.epoch.uploadEpoch) {
    t_state.pendingGeosetSnapshot = {};
  }

  const bool ringReady = observation.outputFormat < kFormatFvf.size() &&
      observation.nativeVertexBuffer != 0u &&
      observation.ringBaseVertexAfter <= observation.ringNextVertexAfter &&
      observation.ringNextVertexAfter <= kMaxNativeVertices &&
      observation.ringNextVertexAfter - observation.ringBaseVertexAfter ==
          observation.vertexCount;
  if (ringReady)
    observation.observedPreflight |= NativePreflightNativeRingReady;
  if (originalResult >= 0 && ringReady)
    observation.observedPreflight |= NativePreflightOriginalSucceeded;

  if (observation.kernelAuthorizationPending) {
    DecrementPendingKernelAuthorization();
    observation.kernelAuthorizationPending = false;
  }

  if (observation.cpuSkinKernelBypassed) {
    const bool mismatchAlreadyPublished = observation.postSkipMismatch;
    if (originalResult < 0)
      observation.postSkipMismatchMask |= 1u << 0;
    if (!ringReady)
      observation.postSkipMismatchMask |= 1u << 1;
    if ((observation.observedPreflight & observation.requiredPreflight) !=
        observation.requiredPreflight) {
      observation.postSkipMismatchMask |= 1u << 2;
    }
    if (!observation.bypassAuthorized || observation.fuseKey == 0u ||
        observation.cpuSkinBytesSkipped == 0u) {
      observation.postSkipMismatchMask |= 1u << 3;
    }
    if (observation.bypassFailure != NativeBypassFailureReason::None)
      observation.postSkipMismatchMask |= 1u << 4;
    observation.postSkipMismatch =
        observation.postSkipMismatchMask != 0u;
    observation.nativeObservationEligible = !observation.postSkipMismatch;
    observation.takeoverEligible = !observation.postSkipMismatch;
    if (observation.postSkipMismatch && !mismatchAlreadyPublished) {
      observation.bypassFailure = NativeBypassFailureReason::PostSkipMismatch;
      g_counters.postSkipMismatchFuses.fetch_add(
          1u, std::memory_order_relaxed);
      g_counters.bypassStateMismatches.fetch_add(
          1u, std::memory_order_relaxed);
    }
  } else {
    observation.nativeObservationEligible =
        !observation.scopeFailClosed &&
        (observation.observedPreflight & observation.requiredPreflight) ==
            observation.requiredPreflight;
    observation.takeoverEligible = false;
  }
  PublishCompletedUpload(observation);
}

bool QueryCurrentNativeUpload(NativeUploadObservation& observation) {
  DispatchFrame* dispatch = CurrentDispatch();
  if (!NativeBridgeHooksEnabled() || dispatch == nullptr ||
      !dispatch->activeUpload.valid) {
    return false;
  }
  observation = dispatch->activeUpload.observation;
  observation.fanoutCount = dispatch->activeUpload.dipCount;
  return true;
}

void PublishNativeVertexStorageDiagnostics(
    const NativeVertexOutputProof& proof,
    const NativeVertexStorageDiagnostics& diagnostics) {
  NativeVertexStorageSidecar& sidecar = t_state.vertexStorageSidecar;
  sidecar = {};
  if (proof.resource.commonResource == 0u ||
      proof.resource.comVertexBuffer == 0u ||
      proof.resource.resourceGeneration == 0u ||
      proof.nativeD3DDevice == 0u) {
    return;
  }
  sidecar.proof = proof;
  sidecar.diagnostics = diagnostics;
  sidecar.valid = true;
}

uint64_t NotifyNativeIndexBufferLock(const NativeIndexLockInput& input) {
  ApplyPendingResetFailClosed();
  if (!NativeBridgeHooksEnabled())
    return 0u;
  ActiveUploadState* active = CurrentBypassedIndexUpload();
  if (active == nullptr)
    return 0u;

  NativeUploadObservation& upload = active->observation;
  NativeIndexTicketObservation& ticket = upload.indexTicket;
  if (ticket.ticketGeneration != 0u ||
      (ticket.stageMask & NativeIndexTicketStageLockAttempt) != 0u) {
    SuppressIndexTicket(*active, NativeIndexTicketFailureOutOfOrder);
    return ticket.ticketGeneration;
  }

  ticket.ticketGeneration = NextIndexTicketGeneration();
  ticket.resourceGeneration = input.resource.resourceGeneration;
  ticket.commonResource = input.resource.commonResource;
  ticket.comIndexBuffer = input.resource.comIndexBuffer;
  ticket.indexFormat = input.resource.indexFormat;
  ticket.observedLockOffset = input.offset;
  ticket.observedLockSize = input.size;
  ticket.observedLockFlags = input.flags;
  ticket.stageMask |= NativeIndexTicketStageLockAttempt;
  g_counters.indexTicketAttempts.fetch_add(1u,
                                             std::memory_order_relaxed);

  uint64_t failureMask = NativeIndexTicketFailureNone;
  const size_t expectedSnapshotSize =
      size_t(ticket.expectedIndexCount) * sizeof(uint16_t);
  if (active->indexSnapshot.uploadEpoch != upload.epoch.uploadEpoch ||
      active->indexSnapshot.bytes.size() != expectedSnapshotSize) {
    failureMask |= NativeIndexTicketFailureExpectedProofMissing;
  } else {
    active->indexSnapshot.ticketGeneration = ticket.ticketGeneration;
  }
  if ((ticket.stageMask & NativeIndexTicketStageExpectedProof) == 0u ||
      ticket.expectedIndexCount == 0u ||
      ticket.expectedIndexCount != upload.expectedIndexCount) {
    failureMask |= NativeIndexTicketFailureExpectedProofMissing;
  }
  if (upload.epoch.renderThreadId != ::GetCurrentThreadId())
    failureMask |= NativeIndexTicketFailureThreadMismatch;
  if (ticket.uploadEpoch == 0u ||
      ticket.uploadEpoch != upload.epoch.uploadEpoch)
    failureMask |= NativeIndexTicketFailureUploadEpochMismatch;
  if (input.resource.resourceGeneration == 0u)
    failureMask |= NativeIndexTicketFailureResourceGenerationMissing;
  if (input.resource.commonResource == 0u)
    failureMask |= NativeIndexTicketFailureCommonResourceMissing;
  if (input.resource.comIndexBuffer == 0u)
    failureMask |= NativeIndexTicketFailureComIndexBufferMissing;
  if (input.resource.comIndexBuffer != upload.nativeIndexBuffer)
    failureMask |= NativeIndexTicketFailureNativeComIdentityMismatch;
  if (input.resource.indexFormat != kD3dFormatIndex16)
    failureMask |= NativeIndexTicketFailureIndexFormatMismatch;
  if (input.result < 0)
    failureMask |= NativeIndexTicketFailureLockFailed;
  if (input.mappedPointer == nullptr)
    failureMask |= NativeIndexTicketFailureMappedPointerMissing;
  if (input.offset != ticket.expectedLockOffset)
    failureMask |= NativeIndexTicketFailureLockOffsetMismatch;
  if (input.size != ticket.expectedLockSize)
    failureMask |= NativeIndexTicketFailureLockSizeMismatch;
  if (input.flags != ticket.expectedLockFlags)
    failureMask |= NativeIndexTicketFailureLockFlagsMismatch;
  if (input.mappedPointer != nullptr && input.size != 0u &&
      !dxvk::war3::IsReadableRangeFast(input.mappedPointer, input.size)) {
    failureMask |= NativeIndexTicketFailureMappedRangeUnreadable;
  }
  if (!LiveIndexLockStateMatches(upload, input.resource.comIndexBuffer))
    failureMask |= NativeIndexTicketFailureRingStateMismatch;

  if (failureMask != NativeIndexTicketFailureNone) {
    SuppressIndexTicket(*active, failureMask);
    return ticket.ticketGeneration;
  }

  ticket.stageMask |= NativeIndexTicketStageLockExact;
  return ticket.ticketGeneration;
}

bool ValidateNativeIndexBufferBeforeUnlock(
    const NativeIndexUnlockInput& input) {
  ApplyPendingResetFailClosed();
  if (!NativeBridgeHooksEnabled())
    return false;
  ActiveUploadState* active = CurrentBypassedIndexUpload();
  if (active == nullptr)
    return false;

  NativeUploadObservation& upload = active->observation;
  NativeIndexTicketObservation& ticket = upload.indexTicket;
  uint64_t failureMask = ValidateIndexTicketContext(
      *active, input.resource, input.ticketGeneration);
  if (ticket.suppressed ||
      (ticket.stageMask & NativeIndexTicketStageLockExact) == 0u ||
      (ticket.stageMask & (NativeIndexTicketStageContentsExact |
                           NativeIndexTicketStageUnlockExact |
                           NativeIndexTicketStageSetIndicesExact)) != 0u) {
    failureMask |= NativeIndexTicketFailureOutOfOrder;
  }
  if (input.offset != ticket.expectedLockOffset)
    failureMask |= NativeIndexTicketFailureLockOffsetMismatch;
  if (input.size != ticket.expectedLockSize)
    failureMask |= NativeIndexTicketFailureLockSizeMismatch;
  if (input.flags != ticket.expectedLockFlags)
    failureMask |= NativeIndexTicketFailureLockFlagsMismatch;
  if (input.mappedPointer == nullptr)
    failureMask |= NativeIndexTicketFailureMappedPointerMissing;
  if (!LiveIndexRingMatches(upload, input.resource.comIndexBuffer))
    failureMask |= NativeIndexTicketFailureRingStateMismatch;

  IndexBytesProof actual = {};
  bool contentsExact = false;
  const NativeIndexSnapshotState& snapshot = active->indexSnapshot;
  const size_t expectedSnapshotSize =
      size_t(ticket.expectedIndexCount) * sizeof(uint16_t);
  if (snapshot.uploadEpoch != ticket.uploadEpoch ||
      snapshot.ticketGeneration != ticket.ticketGeneration ||
      snapshot.bytes.size() != expectedSnapshotSize) {
    failureMask |= NativeIndexTicketFailureExpectedProofMissing |
        NativeIndexTicketFailureTicketGenerationMismatch;
  }
  if (failureMask == NativeIndexTicketFailureNone) {
    if (!CompareMappedIndexBytes(
            input.mappedPointer, snapshot, ticket.expectedIndexCount,
            actual, contentsExact)) {
      failureMask |= NativeIndexTicketFailureMappedRangeUnreadable;
    }
  }
  if (actual.count != 0u) {
    ticket.actualIndexHash = actual.hash;
    ticket.actualIndexCount = actual.count;
    ticket.actualMinIndex = actual.minIndex;
    ticket.actualMaxIndex = actual.maxIndex;
  }
  if (failureMask == NativeIndexTicketFailureNone &&
      actual.count != ticket.expectedIndexCount)
    failureMask |= NativeIndexTicketFailureIndexCountMismatch;
  if (actual.count != 0u &&
      (actual.minIndex != ticket.expectedMinIndex ||
       actual.maxIndex != ticket.expectedMaxIndex ||
       actual.maxIndex >= upload.vertexCount)) {
    failureMask |= NativeIndexTicketFailureIndexRangeMismatch;
  }
  if (actual.count != 0u && actual.hash != ticket.expectedIndexHash)
    failureMask |= NativeIndexTicketFailureIndexHashMismatch;
  if (!contentsExact)
    failureMask |= NativeIndexTicketFailureContentsMismatch;

  if (failureMask != NativeIndexTicketFailureNone) {
    SuppressIndexTicket(*active, failureMask);
    return false;
  }

  ticket.stageMask |= NativeIndexTicketStageContentsExact;
  return true;
}

void NotifyNativeIndexBufferUnlock(const NativeIndexOperationInput& input) {
  ApplyPendingResetFailClosed();
  if (!NativeBridgeHooksEnabled())
    return;
  ActiveUploadState* active = CurrentBypassedIndexUpload();
  if (active == nullptr)
    return;

  NativeIndexTicketObservation& ticket = active->observation.indexTicket;
  uint64_t failureMask = ValidateIndexTicketContext(
      *active, input.resource, input.ticketGeneration);
  if (ticket.suppressed ||
      (ticket.stageMask & NativeIndexTicketStageContentsExact) == 0u) {
    failureMask |= NativeIndexTicketFailureUnlockProofMissing;
  }
  if ((ticket.stageMask & (NativeIndexTicketStageUnlockExact |
                           NativeIndexTicketStageSetIndicesExact)) != 0u) {
    failureMask |= NativeIndexTicketFailureOutOfOrder;
  }
  if (input.result < 0)
    failureMask |= NativeIndexTicketFailureUnlockFailed;
  if (failureMask != NativeIndexTicketFailureNone) {
    SuppressIndexTicket(*active, failureMask);
    return;
  }
  ticket.stageMask |= NativeIndexTicketStageUnlockExact;
}

void NotifyNativeSetIndices(const NativeIndexOperationInput& input) {
  ApplyPendingResetFailClosed();
  if (!NativeBridgeHooksEnabled())
    return;
  ActiveUploadState* active = CurrentBypassedIndexUpload();
  if (active == nullptr)
    return;

  NativeUploadObservation& upload = active->observation;
  NativeIndexTicketObservation& ticket = upload.indexTicket;
  uint64_t failureMask = ValidateIndexTicketContext(
      *active, input.resource, input.ticketGeneration);
  if (ticket.suppressed ||
      (ticket.stageMask & NativeIndexTicketStageUnlockExact) == 0u) {
    failureMask |= NativeIndexTicketFailureSetIndicesBeforeUnlock;
  }
  if ((ticket.stageMask & (NativeIndexTicketStageSetIndicesExact |
                           NativeIndexTicketStageActualProof |
                           NativeIndexTicketStageDipConsumed)) != 0u) {
    failureMask |= NativeIndexTicketFailureOutOfOrder;
  }
  if (input.result < 0)
    failureMask |= NativeIndexTicketFailureSetIndicesFailed;
  if (!LiveIndexRingMatches(upload, input.resource.comIndexBuffer))
    failureMask |= NativeIndexTicketFailureRingStateMismatch;
  if (failureMask != NativeIndexTicketFailureNone) {
    SuppressIndexTicket(*active, failureMask);
    return;
  }

  ticket.stageMask |= NativeIndexTicketStageSetIndicesExact |
      NativeIndexTicketStageActualProof;
  ticket.exact = true;
  upload.observedPreflight |= NativePreflightIndexPathActualProof;
  g_counters.indexTicketExact.fetch_add(1u, std::memory_order_relaxed);
}

bool TryRecordNativeDispatchCpuOnlyDipFastPath(
    bool externalTicketPresent,
    NativeDipObservation* output) noexcept {
  if (output != nullptr)
    *output = {};
  DispatchFrame* frame = CurrentDispatch();
  if (externalTicketPresent || frame == nullptr ||
      !frame->cpuOnlySeal.committed ||
      !DispatchCpuOnlySealPhysicalPathAllowed(*frame) ||
      frame->activeUpload.valid ||
      DispatchCpuOnlyUploadFastPathActive()) {
    return false;
  }
  if (!DispatchCpuOnlySealBaseSafetyExact(
          *frame, 0u, true, false)) {
    InvalidateDispatchCpuOnlySeal(*frame);
    return false;
  }
  if (frame->cpuOnlyActiveUpload.valid &&
      frame->cpuOnlyActiveUpload.dipCount ==
          std::numeric_limits<uint32_t>::max()) {
    InvalidateDispatchCpuOnlySeal(*frame);
    return false;
  }

  AddNativeTelemetryCounter(
      g_counters.dips, t_nativeTelemetryDelta.dips);
  AddNativeTelemetryCounter(
      g_counters.dispatchNoUploadDips,
      t_nativeTelemetryDelta.dispatchNoUploadDips);
  AddNativeTelemetryCounter(
      g_counters.unmatchedDips, t_nativeTelemetryDelta.unmatchedDips);
  AddNativeTelemetryCounter(
      g_counters.dispatchCpuOnlySealDips,
      t_nativeTelemetryDelta.dispatchCpuOnlySealDips);
  if (frame->cpuOnlyActiveUpload.valid) {
    ++frame->cpuOnlyActiveUpload.dipCount;
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealDipsWithUpload,
        t_nativeTelemetryDelta.dispatchCpuOnlySealDipsWithUpload);
  } else {
    AddNativeTelemetryCounter(
        g_counters.dispatchCpuOnlySealDipsNoUpload,
        t_nativeTelemetryDelta.dispatchCpuOnlySealDipsNoUpload);
  }

  if (output != nullptr) {
    output->epoch = frame->observation.epoch;
    // ABI-9 manager ordinals are intentionally a separate domain. A sealed
    // physical DIP is always DispatchNoUpload: upload/dip ordinals remain zero
    // and a later generic evidence event may start its manager domain at one.
    output->epoch.uploadEpoch = 0u;
    output->epoch.uploadOrdinal = 0u;
    output->epoch.dipOrdinal = 0u;
    output->correlated = false;
  }
  return true;
}

bool TryRecordNativeOutsideDipFastPath(
    const NativeDipInput* input,
    bool externalTicketPresent,
    uint64_t cpuOnlySealCookie) noexcept {
  // The device first calls without input and explicitly reports whether any
  // index-ticket mirror blocks that pre-resource route. Every early rejection
  // may retry with immutable DIP identity; an exact correlated ticket still
  // rejects here, while an unrelated ticket can become poison-disjoint. The
  // bridge independently proves that no native upload, scope hazard, poison,
  // reset or retirement can require DIP identity/provenance work. Either an
  // enclosing flush pin or the device-owned opaque DIP observer transaction
  // must cover the TLS ordinal and diagnostics below.
  if (g_runtimeConfig.mode != GpuSkinMode::Bypass ||
      g_runtimeConfig.fullDiagnostics) {
    return false;
  }

  const bool earlyProbe = input == nullptr;
  const bool sampled = SampleNextProductionEventOrdinalAtPhase(
      earlyProbe ? t_state.productionOutsideDipFastEarlyOrdinal
                 : t_state.productionOutsideDipFastLateOrdinal,
      earlyProbe ? kNativeOutsideDipFastEarlySamplePhase
                 : kNativeOutsideDipFastLateSamplePhase);
  const auto reject = [&](NativeOutsideDipFastRejectReason reason) noexcept {
    if (sampled) {
      auto& counters = earlyProbe
          ? g_counters.productionOutsideDipFastEarlyRejects
          : g_counters.productionOutsideDipFastLateRejects;
      counters[size_t(reason)].fetch_add(1u, std::memory_order_relaxed);
    }
    return false;
  };
  const auto rejectLocal = [&](
      NativeOutsideDipFastLocalRejectReason reason) noexcept {
    if (sampled) {
      auto& counters = earlyProbe
          ? g_counters.productionOutsideDipFastEarlyLocalRejects
          : g_counters.productionOutsideDipFastLateLocalRejects;
      counters[size_t(reason)].fetch_add(1u, std::memory_order_relaxed);
    }
    return reject(NativeOutsideDipFastRejectReason::LocalScope);
  };

  const bool fastCoveredByFlush = t_state.flushEpoch != 0u &&
      t_state.flushTransactionDepth != 0u;
  const bool fastCoveredByDipObserver = t_state.flushEpoch != 0u &&
      CurrentDipObserverCoverActive();
  const bool observerCpuOnlySealed = !fastCoveredByFlush &&
      fastCoveredByDipObserver &&
      cpuOnlySealCookie != 0u && t_dipObserverDepth == 1u &&
      cpuOnlySealCookie == t_dipObserverCpuOnlySealCookie &&
      cpuOnlySealCookie == t_dipObserverCookie &&
      t_dipObserverResetGeneration == t_state.appliedResetGeneration;
  if ((!fastCoveredByFlush && !fastCoveredByDipObserver) ||
      t_state.dispatchDepth != 0u || t_state.semanticDepth != 0u ||
      t_state.dispatchOverflowDepth != 0u ||
      t_state.semanticOverflowDepth != 0u ||
      t_state.nestedUploadDepth != 0u ||
      t_state.inFlightUpload.observation != nullptr ||
      t_state.productionFastRejectedUpload != nullptr ||
      t_state.productionEvidenceUploadEpoch != 0u ||
      OutsideUploadFastPathActive() || t_callbackPinDepth != 0u) {
    // Preserve the production reject path at its original cost. The detailed
    // classification exists only for the independent 1/256 diagnostic cohort.
    if (!sampled)
      return false;
    // reset 请求可以在既有 observer pin 存活时先推进 requested generation；
    // 该 pin 会阻止 reset 完成，因此 cookie/depth 仍完整但 generation 暂时
    // 不等。此时慢路径是正确结果，应归入 ResetOrRetirement，而不是把它
    // 误记为 NoTransactionCover。cookie/depth 本身损坏时仍保留后者硬失败。
    const bool observerResetGenerationDrift =
        t_dipObserverDepth != 0u && t_dipObserverCookie != 0u &&
        t_dipObserverResetGeneration != t_state.appliedResetGeneration;
    if (!fastCoveredByFlush && !fastCoveredByDipObserver &&
        observerResetGenerationDrift) {
      return reject(NativeOutsideDipFastRejectReason::ResetOrRetirement);
    }
    // reset 完成后、首个新 flush 尚未建立时，observer 可以合法持有进程级
    // lifetime pin，但 TLS 还没有新的 flushEpoch。此窗口只能走慢路径，属于
    // lifecycle cold reject，不是缺失事务覆盖；两者必须在诊断中分开，否则
    // lifecycle 硬门会把预期的 reset 冷启动误报成 cover 漏洞。
    if (!fastCoveredByFlush && !fastCoveredByDipObserver &&
        t_state.flushEpoch == 0u &&
        t_state.appliedResetGeneration != 0u) {
      return reject(NativeOutsideDipFastRejectReason::Lifecycle);
    }
    const SemanticFrame* semantic = CurrentSemantic();
    const bool cleanSemanticOnly = t_state.semanticDepth == 1u &&
        semantic != nullptr && semantic->cookie != 0u &&
        semantic->dispatchEpoch == 0u && !semantic->failClosed;
    const bool uploadOrMarker = t_state.nestedUploadDepth != 0u ||
        t_state.inFlightUpload.observation != nullptr ||
        t_state.productionFastRejectedUpload != nullptr ||
        t_state.productionEvidenceUploadEpoch != 0u ||
        OutsideUploadFastPathActive();
    NativeOutsideDipFastLocalRejectReason localReason =
        NativeOutsideDipFastLocalRejectReason::OtherChronology;
    if (t_state.dispatchDepth != 0u ||
        t_state.dispatchOverflowDepth != 0u) {
      localReason = NativeOutsideDipFastLocalRejectReason::DispatchActive;
    } else if (t_state.semanticOverflowDepth != 0u ||
               t_state.semanticDepth > 1u ||
               (t_state.semanticDepth == 1u && !cleanSemanticOnly)) {
      localReason = NativeOutsideDipFastLocalRejectReason::SemanticHazard;
    } else if (uploadOrMarker) {
      localReason = NativeOutsideDipFastLocalRejectReason::UploadOrMarker;
    } else if (t_callbackPinDepth != 0u) {
      localReason = NativeOutsideDipFastLocalRejectReason::CallbackReentry;
    } else if (!fastCoveredByFlush && !fastCoveredByDipObserver) {
      localReason =
          NativeOutsideDipFastLocalRejectReason::NoTransactionCover;
    } else if (cleanSemanticOnly) {
      localReason =
          NativeOutsideDipFastLocalRejectReason::CleanSemanticOnly;
    }
    return rejectLocal(localReason);
  }
  if ((input == nullptr && externalTicketPresent) ||
      (input != nullptr && input->indexTicketGeneration != 0u)) {
    return reject(NativeOutsideDipFastRejectReason::TicketGate);
  }

  // Keep correlated/no-upload fallback cheap: only the strict local outside
  // class reaches these cross-thread lifecycle and poison publications.
  const uint64_t poisonOutstanding =
      g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
  if (g_counters.pendingKernelAuthorizations.load(
          std::memory_order_acquire) != 0u) {
    return reject(NativeOutsideDipFastRejectReason::PendingKernel);
  }
  if (!observerCpuOnlySealed) {
    const uint64_t resetRequested =
        g_resetRequestedGeneration.load(std::memory_order_acquire);
    if (!g_hooksEnabled.load(std::memory_order_acquire) ||
        !g_transactionIngressEnabled.load(std::memory_order_seq_cst) ||
        !g_callbackIngressEnabled.load(std::memory_order_acquire) ||
        !g_bypassEnabled.load(std::memory_order_acquire) ||
        !CurrentThreadIsObservedRenderThread()) {
      return reject(NativeOutsideDipFastRejectReason::Lifecycle);
    }
    if (g_retirementEventsPending.load(std::memory_order_acquire) != 0u ||
        g_retirementQueueFaulted.load(std::memory_order_acquire) ||
        resetRequested !=
            g_resetCompletedGeneration.load(std::memory_order_acquire) ||
        t_state.appliedResetGeneration != resetRequested) {
      return reject(NativeOutsideDipFastRejectReason::ResetOrRetirement);
    }
  }

  // A pending ticket for another index buffer is not itself correlated with
  // this DIP, but it normally coexists with a live poisoned native VB slice.
  // The early no-input route must therefore stay closed. Once the device has
  // built the same immutable identities/layout/range used by the slow poison
  // path, admit only an exact proof that the current draw cannot touch any
  // poison interval. Do not publish poison diagnostics during this probe: a
  // hit falls through to the authoritative slow path, which records it once.
  if (poisonOutstanding != 0u || t_state.poisonLedger.count != 0u) {
    if (input == nullptr)
      return reject(NativeOutsideDipFastRejectReason::PoisonNeedsInput);
    if (poisonOutstanding != t_state.poisonLedger.count)
      return reject(NativeOutsideDipFastRejectReason::PoisonCountMismatch);
    NativePoisonRange poisonHit = {};
    NativePoisonHitKind poisonHitKind = NativePoisonHitKind::None;
    if (FindNativePoisonHit(
            *input, poisonHit, poisonHitKind, false)) {
      return reject(NativeOutsideDipFastRejectReason::PoisonHit);
    }
  }

  // The first lifecycle snapshot above is the policy linearization point.
  // From there to this CPU-only fast outcome, the enclosing flush or opaque
  // DIP-observer transaction prevents reset completion and TLS/poison-ledger
  // retirement. An asynchronous close may request work, but cannot make this
  // default native draw authorize or consume GPU output. Recheck only that the
  // local lifetime cover itself still exists; ticket and poison exactness stay
  // authoritative and unchanged.
  if ((fastCoveredByFlush && t_state.flushTransactionDepth == 0u) ||
      (!fastCoveredByFlush && !CurrentDipObserverCoverActive())) {
    return reject(NativeOutsideDipFastRejectReason::Lifecycle);
  }

  RecordNativeOutsideDipFastTelemetry(
      fastCoveredByFlush ? NativeOutsideDipFastCover::Flush
                         : NativeOutsideDipFastCover::Observer);
  if (sampled) {
    auto& successes = earlyProbe
        ? g_counters.productionOutsideDipFastEarlySuccesses
        : g_counters.productionOutsideDipFastLateSuccesses;
    successes.fetch_add(1u, std::memory_order_relaxed);
  }
  ++t_state.orphanDipOrdinal;
  return true;
}

bool NativeDipStorageDiagnosticsRequired() noexcept {
  return t_state.poisonLedger.count != 0u ||
      g_nativePoisonOutstandingRanges.load(std::memory_order_acquire) != 0u;
}

bool NotifyNativeDrawIndexedPrimitive(const NativeDipInput& input,
                                      const NativeDipDiagnosticInput& inputDiagnostic,
                                      NativeDipObservation* output) {
  AtomicRawTickAccumulator timing(
      g_runtimeConfig.fullDiagnostics,
      g_counters.dipTimingCalls,
      g_counters.dipTimingTicks,
      g_counters.dipTimingMaxTicks);
  ApplyPendingResetFailClosed();
  if (!NativeBridgeHooksEnabled())
    return false;
  AddNativeTelemetryCounter(g_counters.dips, t_nativeTelemetryDelta.dips);

  DispatchFrame* dispatch = CurrentDispatch();
  // The device calls this generic observer only after the independent sealed
  // DIP route declined. Never allow a later DIP/upload in the same dispatch to
  // resurrect that exact-negative proof.
  if (dispatch != nullptr && dispatch->cpuOnlySeal.committed)
    InvalidateDispatchCpuOnlySeal(*dispatch);
  NativeDipObservation observation = {};
  NativeDipDiagnosticInput diagnostic = inputDiagnostic;
  observation.dip = input;
  observation.epoch.renderThreadId = ::GetCurrentThreadId();
  observation.epoch.flushEpoch = t_state.flushEpoch;
  const auto publish = [&](bool emitCallback,
                           bool requiresManagerResolution) {
    ApplyNativePoisonObservation(observation, diagnostic);
    const uint64_t globalPoison =
        g_nativePoisonOutstandingRanges.load(std::memory_order_acquire);
    const uint64_t localPoison = t_state.poisonLedger.count;
    const uint64_t observedThread =
        g_observedRenderThreadId.load(std::memory_order_acquire);
    const bool wrongThread = observedThread != 0u &&
        observedThread != ::GetCurrentThreadId();
    const bool poisonCountMismatch = globalPoison != localPoison;
    if (poisonCountMismatch || wrongThread) {
      // Poison ownership lives on the observed render TLS. A different TLS,
      // or any divergence from the remotely visible range count, cannot prove
      // this DIP disjoint. Suppress rather than letting a missing local ledger
      // turn a lifecycle race into a native read of bypassed vertex bytes.
      observation.nativeRangePoisoned = true;
      observation.requiresSuppression = true;
      observation.sourceUploadKernelBypassed = true;
      g_counters.nativePoisonLeaks.fetch_add(
          1u, std::memory_order_relaxed);
    }
    if (output != nullptr)
      *output = observation;
    const bool managerCallbackAllowed = emitCallback &&
        ManagerCallbackAllowedForEpoch(
            observation.epoch.dispatchEpoch,
            ManagerSkippedCallbackKind::Dip);
    if (managerCallbackAllowed) {
      NativeBridgeCallbackPin callbackPin(
          NativeBridgeCallbackKind::Dip,
          observation.correlated
              ? observation.epoch.uploadEpoch : 0u);
      const NativeBridgeCallbacks* callbacks = callbackPin.get();
      if (callbacks != nullptr && callbacks->onDip != nullptr) {
        callbackPin.BeginCallbackBody();
        callbacks->onDip(callbacks->userData, observation);
        callbackPin.EndCallbackBody();
      }
    }
    return (managerCallbackAllowed && requiresManagerResolution) ||
        observation.requiresSuppression;
  };
  const auto publishScopeHazard = [&](uint32_t dipOrdinal) {
    ActiveUploadState* hazard = FindBypassedScopeHazard();
    if (hazard == nullptr)
      return false;

    observation.epoch = hazard->observation.epoch;
    observation.epoch.dipOrdinal = dipOrdinal;
    observation.sourceUploadKernelBypassed = true;
    observation.sourceUploadPostSkipMismatch =
        hazard->observation.postSkipMismatch;
    observation.sourceUploadFuseKey = hazard->observation.fuseKey;
    observation.observedPreflight = hazard->observation.observedPreflight;
    observation.indexTicket = hazard->observation.indexTicket;
    diagnostic.scope = NativeDipScope::ScopeHazard;
    diagnostic.dispatchPath = hazard->observation.path;
    diagnostic.stage = hazard->observation.stage;
    diagnostic.batchTag = hazard->observation.batchTag;
    return publish(true, true);
  };
  if (dispatch == nullptr) {
    diagnostic.scope = NativeDipScope::OutsideDispatch;
    diagnostic.dispatchPath = NativeDispatchPath::Unknown;
    diagnostic.stage = -1;
    diagnostic.batchTag = -1;
    observation.epoch.dipOrdinal = ++t_state.orphanDipOrdinal;
    AddNativeTelemetryCounter(
        g_counters.outsideDispatchDips,
        t_nativeTelemetryDelta.outsideDispatchDips);
    AddNativeTelemetryCounter(
        g_counters.unmatchedDips, t_nativeTelemetryDelta.unmatchedDips);
    if (publishScopeHazard(observation.epoch.dipOrdinal))
      return true;
    return publish(false, false);
  }

  observation.epoch = dispatch->observation.epoch;
  if (!dispatch->activeUpload.valid) {
    diagnostic.scope = NativeDipScope::DispatchNoUpload;
    diagnostic.dispatchPath = dispatch->observation.path;
    diagnostic.stage = dispatch->observation.stage;
    diagnostic.batchTag = dispatch->observation.batchTag;
    AddNativeTelemetryCounter(
        g_counters.dispatchNoUploadDips,
        t_nativeTelemetryDelta.dispatchNoUploadDips);
    AddNativeTelemetryCounter(
        g_counters.unmatchedDips, t_nativeTelemetryDelta.unmatchedDips);
    if (publishScopeHazard(0u))
      return true;
    return publish(false, false);
  }

  ActiveUploadState& active = dispatch->activeUpload;
  diagnostic.scope = NativeDipScope::ActiveUpload;
  diagnostic.dispatchPath = active.observation.path;
  diagnostic.stage = active.observation.stage;
  diagnostic.batchTag = active.observation.batchTag;
  const uint32_t dipOrdinal = dispatch->dipCount + 1u;
  observation.sourceUploadKernelBypassed =
      active.observation.cpuSkinKernelBypassed;
  observation.sourceUploadPostSkipMismatch =
      active.observation.postSkipMismatch;
  observation.sourceUploadFuseKey = active.observation.fuseKey;
  observation.epoch = active.observation.epoch;
  observation.epoch.dipOrdinal = dipOrdinal;

  const bool epochMatch =
      active.observation.epoch.renderThreadId ==
          dispatch->observation.epoch.renderThreadId &&
      active.observation.epoch.flushEpoch ==
          dispatch->observation.epoch.flushEpoch &&
      active.observation.epoch.dispatchEpoch ==
          dispatch->observation.epoch.dispatchEpoch;
  observation.correlated = epochMatch;
  if (epochMatch) {
    dispatch->dipCount = dipOrdinal;
    observation.uploadFanoutOrdinal = ++active.dipCount;
  }

  const NativeVertexOutputProof& vertexProof =
      active.observation.vertexOutputProof;
  const bool exactBypassIdentity =
      !active.observation.cpuSkinKernelBypassed ||
      (input.stream0Resource.commonResource != 0u &&
       input.stream0Resource.commonResource ==
           vertexProof.resource.commonResource &&
       input.stream0Resource.comVertexBuffer ==
           vertexProof.resource.comVertexBuffer &&
       input.stream0Resource.resourceGeneration ==
           vertexProof.resource.resourceGeneration &&
       input.nativeD3DDevice == vertexProof.nativeD3DDevice &&
       input.outputFormat == vertexProof.outputFormat);
  const bool streamMatch =
      input.stream0Resource.comVertexBuffer != 0u &&
      input.stream0Resource.comVertexBuffer ==
          active.observation.nativeVertexBuffer &&
      input.vertexStride == active.observation.outputStride &&
      input.fvf == active.observation.fvf && exactBypassIdentity;
  const bool baseMatch = input.baseVertexIndex >= 0 &&
      static_cast<uint32_t>(input.baseVertexIndex) ==
          active.observation.ringBaseVertexAfter &&
      input.numVertices == active.observation.vertexCount;
  const bool supportedPath = input.flags == NativeDipFlagNone;
  bool indexTicketMatch = true;
  if (active.observation.cpuSkinKernelBypassed) {
    NativeIndexTicketObservation& ticket =
        active.observation.indexTicket;
    uint64_t ticketFailure = ValidateIndexTicketContext(
        active, input.indexResource, input.indexTicketGeneration);
    constexpr uint32_t requiredTicketStages =
        NativeIndexTicketStageExpectedProof |
        NativeIndexTicketStageLockAttempt |
        NativeIndexTicketStageLockExact |
        NativeIndexTicketStageContentsExact |
        NativeIndexTicketStageUnlockExact |
        NativeIndexTicketStageSetIndicesExact |
        NativeIndexTicketStageActualProof;
    if (ticket.suppressed || !ticket.exact ||
        (ticket.stageMask & requiredTicketStages) != requiredTicketStages ||
        (active.observation.observedPreflight &
         NativePreflightIndexPathActualProof) == 0u) {
      ticketFailure |= NativeIndexTicketFailureActualProofMissing;
    }
    if ((ticket.stageMask & NativeIndexTicketStageDipConsumed) != 0u)
      ticketFailure |= NativeIndexTicketFailureOutOfOrder;
    if (!epochMatch)
      ticketFailure |= NativeIndexTicketFailureUploadEpochMismatch;

    const bool indexSignatureMatch =
        ticket.expectedIndexCount != 0u &&
        (ticket.expectedIndexCount % 3u) == 0u &&
        input.primitiveType == kD3dPrimitiveTriangleList &&
        input.minVertexIndex == 0u &&
        input.startIndex == ticket.predictedStartIndex &&
        input.primitiveCount == ticket.expectedIndexCount / 3u;
    if (!streamMatch || !baseMatch || !supportedPath ||
        !indexSignatureMatch) {
      ticketFailure |= NativeIndexTicketFailureDipSignatureMismatch;
    }
    if (input.indexResource.commonResource != ticket.commonResource ||
        input.indexResource.comIndexBuffer != ticket.comIndexBuffer ||
        input.indexResource.resourceGeneration !=
            ticket.resourceGeneration ||
        input.indexResource.comIndexBuffer !=
            active.observation.nativeIndexBuffer) {
      ticketFailure |= NativeIndexTicketFailureDipIdentityMismatch;
    }
    if (!LiveIndexRingMatches(active.observation,
                              input.indexResource.comIndexBuffer)) {
      ticketFailure |= NativeIndexTicketFailureRingStateMismatch;
    }

    if (ticketFailure != NativeIndexTicketFailureNone) {
      SuppressIndexTicket(active, ticketFailure);
      indexTicketMatch = false;
    } else {
      ticket.stageMask |= NativeIndexTicketStageDipConsumed;
    }
  }

  observation.observedPreflight = active.observation.observedPreflight;
  observation.indexTicket = active.observation.indexTicket;
  observation.sourceUploadPostSkipMismatch =
      active.observation.postSkipMismatch;
  if (epochMatch)
    observation.observedPreflight |= NativePreflightDipEpochMatch;
  if (streamMatch)
    observation.observedPreflight |= NativePreflightDipStreamMatch;
  if (baseMatch)
    observation.observedPreflight |= NativePreflightDipBaseVertexMatch;
  if (supportedPath)
    observation.observedPreflight |= NativePreflightNoUnsupportedDrawPath;

  observation.exactNativeMatch =
      epochMatch && streamMatch && baseMatch && supportedPath &&
      indexTicketMatch;
  observation.takeoverEligible = active.observation.takeoverEligible &&
      observation.exactNativeMatch &&
      (!observation.sourceUploadKernelBypassed ||
       (observation.observedPreflight &
        NativePreflightIndexPathActualProof) != 0u);
  if (epochMatch) {
    AddNativeTelemetryCounter(
        g_counters.correlatedDips, t_nativeTelemetryDelta.correlatedDips);
  } else {
    AddNativeTelemetryCounter(
        g_counters.unmatchedDips, t_nativeTelemetryDelta.unmatchedDips);
  }

  return publish(true, epochMatch || observation.sourceUploadKernelBypassed);
}

}  // namespace dxvk::war3::gpu_skin
