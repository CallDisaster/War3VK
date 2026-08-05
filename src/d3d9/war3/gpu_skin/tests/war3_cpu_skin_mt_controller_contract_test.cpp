#include "../war3_cpu_skin_mt_controller_contract.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dxvk::war3::gpu_skin {

class CpuSkinMtControllerJobTestPeer {
public:
  static void holdStateMutex(CpuSkinMtControllerJob& job,
                             std::atomic<bool>& entered,
                             std::atomic<bool>& release) noexcept {
    std::lock_guard<std::mutex> lock(job.m_mutex);
    entered.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire))
      std::this_thread::yield();
  }
};

}  // namespace dxvk::war3::gpu_skin

namespace {

using namespace dxvk::war3::gpu_skin;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::cerr << __func__ << ':' << __LINE__                              \
                << ": CHECK failed: " #condition << '\n';                  \
      return false;                                                          \
    }                                                                        \
  } while (false)

constexpr uint64_t kGeneration = 7u;
constexpr uint32_t kSafeMxcsr = kCpuSkinMtMxcsrExceptionMask;
constexpr uint32_t kProducerMxcsrBefore = kSafeMxcsr | 0x01u;
constexpr uint32_t kProducerMxcsrAfter = kSafeMxcsr | 0x05u;

struct ProofFixture {
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<uint8_t> groups;
  std::vector<float> uv0;
  std::vector<float> palette;
  std::vector<uint8_t> mapped;
  CpuSkinMtControllerFrozenKey frozen;
  CpuSkinMtControllerOuterBindingProof outer;
  CpuSkinMtControllerDestinationProof destination;
  CpuSkinMtControllerUnlockProof unlock;

  explicit ProofFixture(uint32_t vertexCount =
                            kCpuSkinMtFormat2MinVertices,
                        uint32_t paletteGroups = 4u)
      : positions(size_t(vertexCount) * 3u, 1.0f),
        normals(size_t(vertexCount) * 3u, 0.5f),
        groups(vertexCount, 0u),
        uv0(size_t(vertexCount) * 2u, 0.25f),
        palette(size_t(paletteGroups) * 12u, 1.0f),
        mapped(32u * 1024u, 0xccu) {
    for (size_t i = 0; i < groups.size(); i++)
      groups[i] = static_cast<uint8_t>(i % paletteGroups);

    frozen.owner.controllerInstanceGeneration = kGeneration;
    frozen.owner.producerGeneration = 11u;
    frozen.owner.mapEpoch = 13u;
    frozen.owner.deviceEpoch = 17u;
    frozen.owner.bridgeResetGeneration = 19u;
    frozen.owner.renderThreadId = 23u;
    frozen.flush.frameTag = 29u;
    frozen.flush.flushEpoch = 31u;
    frozen.flush.batchId = 37u;
    frozen.flush.renderablePart = 0x1000u;
    frozen.flush.geosetData = 0x2000u;
    frozen.flush.candidateToken = 41u;
    frozen.flush.flushCandidateOrdinal = 3u;
    frozen.flush.layerIndex = 2u;
    frozen.flush.path = CpuSkinMtControllerPath::Common;
    frozen.flush.opaque = true;

    frozen.source.snapshotIdentity =
        reinterpret_cast<uintptr_t>(positions.data());
    frozen.source.contentHash = 43u;
    frozen.source.sealedContentToken = 47u;
    frozen.source.sourceGeneration = 53u;
    frozen.source.layoutGeneration = 59u;
    frozen.source.vertexCount = vertexCount;
    frozen.source.matrixGroupCount = paletteGroups;
    frozen.source.uvLayerCount = 1u;
    frozen.source.maxGroupSlot = paletteGroups - 1u;
    frozen.source.positions = reinterpret_cast<uintptr_t>(positions.data());
    frozen.source.normals = reinterpret_cast<uintptr_t>(normals.data());
    frozen.source.groupSlots = reinterpret_cast<uintptr_t>(groups.data());
    frozen.source.uv0 = reinterpret_cast<uintptr_t>(uv0.data());
    frozen.source.positionStride = 12u;
    frozen.source.normalStride = 12u;
    frozen.source.groupSlotStride = 1u;
    frozen.source.uv0Stride = 8u;
    frozen.source.positionByteCount = vertexCount * 12u;
    frozen.source.normalByteCount = vertexCount * 12u;
    frozen.source.groupSlotByteCount = vertexCount;
    frozen.source.uv0ByteCount = vertexCount * 8u;

    frozen.palette.palettePointer =
        reinterpret_cast<uintptr_t>(palette.data());
    frozen.palette.immutableBlobIdentity =
        reinterpret_cast<uintptr_t>(palette.data());
    frozen.palette.paletteGeneration = 61u;
    frozen.palette.paletteWriteSerial = 67u;
    frozen.palette.paletteFrameTag = frozen.flush.frameTag;
    frozen.palette.contentHash = 71u;
    frozen.palette.sealedContentToken = 73u;
    frozen.palette.groupCount = paletteGroups;
    frozen.palette.byteCount =
        paletteGroups * kCpuSkinMtPaletteMatrixBytes;
    frozen.palette.floatingPointControl = kSafeMxcsr;

    outer.controllerInstanceGeneration =
        frozen.owner.controllerInstanceGeneration;
    outer.producerGeneration = frozen.owner.producerGeneration;
    outer.mapEpoch = frozen.owner.mapEpoch;
    outer.deviceEpoch = frozen.owner.deviceEpoch;
    outer.bridgeResetGeneration = frozen.owner.bridgeResetGeneration;
    outer.frameTag = frozen.flush.frameTag;
    outer.flushEpoch = frozen.flush.flushEpoch;
    outer.batchId = frozen.flush.batchId;
    outer.sourceGeneration = frozen.source.sourceGeneration;
    outer.paletteGeneration = frozen.palette.paletteGeneration;
    outer.sourceSealedContentToken = frozen.source.sealedContentToken;
    outer.paletteSealedContentToken = frozen.palette.sealedContentToken;
    outer.dispatchEpoch = 79u;
    outer.uploadEpoch = 83u;
    outer.gxDeviceD3d = 0x9000u;
    outer.renderablePart = frozen.flush.renderablePart;
    outer.geosetData = frozen.flush.geosetData;
    outer.positions = frozen.source.positions;
    outer.normals = frozen.source.normals;
    outer.groupSlots = frozen.source.groupSlots;
    outer.uv0 = frozen.source.uv0;
    outer.extra = frozen.source.extra;
    outer.uv1 = frozen.source.uv1;
    outer.palettePointer = frozen.palette.palettePointer;
    outer.uploadOrdinal = 5u;
    outer.candidateToken = frozen.flush.candidateToken;
    outer.flushCandidateOrdinal = frozen.flush.flushCandidateOrdinal;
    outer.layerIndex = frozen.flush.layerIndex;
    outer.vertexCount = frozen.source.vertexCount;
    outer.positionStride = frozen.source.positionStride;
    outer.normalStride = frozen.source.normalStride;
    outer.groupSlotStride = frozen.source.groupSlotStride;
    outer.uv0Stride = frozen.source.uv0Stride;
    outer.extraStride = frozen.source.extraStride;
    outer.uv1Stride = frozen.source.uv1Stride;
    outer.paletteGroupCount = frozen.palette.groupCount;
    outer.skinMode = kCpuSkinMtFormat2SkinMode;
    outer.outputFormat = kCpuSkinMtFormat2OutputFormat;
    outer.fvf = kCpuSkinMtFormat2Fvf;
    outer.outputStride = kCpuSkinMtFormat2OutputStride;

    destination.commonResource = 0xa000u;
    destination.comVertexBuffer = 0xa100u;
    destination.nativeD3DDevice = 0xa200u;
    destination.mappedAllocation =
        reinterpret_cast<uintptr_t>(mapped.data());
    destination.mappedAllocationBase =
        reinterpret_cast<uintptr_t>(mapped.data());
    destination.offset = 64u;
    destination.mappedPointer =
        destination.mappedAllocationBase + destination.offset;
    destination.resourceGeneration = 89u;
    destination.realStorageGeneration = 97u;
    destination.mappingStorageGeneration = 101u;
    destination.mapAllocationGeneration = 103u;
    destination.lockSerial = 107u;
    destination.mappedAllocationByteSize = mapped.size();
    destination.dispatchEpoch = outer.dispatchEpoch;
    destination.uploadEpoch = outer.uploadEpoch;
    destination.size = vertexCount * kCpuSkinMtFormat2OutputStride;
    destination.effectiveFlags = 0x2000u;
    destination.lockDepth = 1u;
    destination.uploadOrdinal = outer.uploadOrdinal;
    destination.outputFormat = kCpuSkinMtFormat2OutputFormat;
    destination.fvf = kCpuSkinMtFormat2Fvf;
    destination.lockSucceeded = true;
    destination.lockActive = true;

    unlock.commonResource = destination.commonResource;
    unlock.comVertexBuffer = destination.comVertexBuffer;
    unlock.mappedAllocation = destination.mappedAllocation;
    unlock.mappedPointer = destination.mappedPointer;
    unlock.resourceGeneration = destination.resourceGeneration;
    unlock.mapAllocationGeneration = destination.mapAllocationGeneration;
    unlock.lockSerial = destination.lockSerial;
    unlock.unlockSerial = 109u;
    unlock.result = 0;
  }

  CpuSkinMtFormat2EligibilityInput eligibility() const {
    CpuSkinMtFormat2EligibilityInput input;
    input.path = frozen.flush.path;
    input.opaque = frozen.flush.opaque;
    input.skinMode = outer.skinMode;
    input.outputFormat = outer.outputFormat;
    input.fvf = outer.fvf;
    input.outputStride = outer.outputStride;
    input.vertexCount = frozen.source.vertexCount;
    input.positionStride = frozen.source.positionStride;
    input.normalStride = frozen.source.normalStride;
    input.groupSlotStride = frozen.source.groupSlotStride;
    input.uv0Stride = frozen.source.uv0Stride;
    input.uvLayerCount = frozen.source.uvLayerCount;
    input.paletteGroupCount = frozen.palette.groupCount;
    input.extra = frozen.source.extra;
    input.uv1 = frozen.source.uv1;
    input.positions = positions.data();
    input.positionFloatCount = positions.size();
    input.normals = normals.data();
    input.normalFloatCount = normals.size();
    input.groupSlots = groups.data();
    input.groupSlotCount = groups.size();
    input.uv0 = uv0.data();
    input.uv0FloatCount = uv0.size();
    input.palette = palette.data();
    input.paletteFloatCount = palette.size();
    input.immutableSourceSealedToken = frozen.source.sealedContentToken;
    input.paletteSealedToken = frozen.palette.sealedContentToken;
    input.frozenMxcsr = kProducerMxcsrBefore;
    input.currentMxcsr = kProducerMxcsrBefore;
    return input;
  }

  std::shared_ptr<const CpuSkinMtControllerOwnedProducerResult> result(
      uint64_t serial = 1u,
      uint8_t fill = 0x5au,
      uint32_t mxcsrBefore = kProducerMxcsrBefore,
      uint32_t mxcsrAfter = kProducerMxcsrAfter) const {
    return CpuSkinMtControllerOwnedProducerResult::Create(
        frozen, eligibility(), serial,
        std::vector<uint8_t>(destination.size, fill),
        mxcsrBefore, mxcsrAfter);
  }
};

CpuSkinMtControllerBodyCompletion GoodBodyCompletion(
    bool succeeded = true) {
  return CpuSkinMtControllerBodyCompletion{
      succeeded, kProducerMxcsrBefore, kProducerMxcsrAfter};
}

bool PrepareAwaiting(CpuSkinMtControllerJob& job,
                     const ProofFixture& fixture) {
  CHECK(job.submit(kGeneration) == CpuSkinMtControllerResult::Applied);
  CHECK(job.bindOuter(kGeneration, fixture.outer) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.awaitKernel(kGeneration, fixture.destination) ==
        CpuSkinMtControllerResult::Applied);
  return true;
}

bool PublishResult(CpuSkinMtControllerJob& job,
                   const std::shared_ptr<
                       const CpuSkinMtControllerOwnedProducerResult>& result) {
  CHECK(result);
  CHECK(job.publishProducerResult(kGeneration, result) ==
        CpuSkinMtControllerResult::Published);
  return true;
}

bool SelectCopyRoute(CpuSkinMtControllerJob& job) {
  const auto decision = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
  CHECK(decision.result == CpuSkinMtControllerResult::Applied);
  CHECK(decision.route == CpuSkinMtControllerKernelRoute::Copy);
  CHECK(decision.reason ==
        CpuSkinMtControllerKernelReason::ReadyProducerResult);
  CHECK(decision.stateLockAcquired);
  CHECK(decision.selectedNow);
  CHECK(!decision.nativeBodyLease.valid());
  return true;
}

struct CopyContext {
  bool called = false;
  bool proofValid = false;
  CpuSkinMtControllerBodyCompletion completion = GoodBodyCompletion();
};

CpuSkinMtControllerBodyCompletion CopyToMappedDestination(
    void* opaque,
    const CpuSkinMtControllerProducerResultView& result,
    const CpuSkinMtControllerDestinationProof& destination) noexcept {
  auto* context = static_cast<CopyContext*>(opaque);
  context->called = true;
  context->proofValid = result.bytes != nullptr && result.byteSize != 0u &&
      result.producerProof != nullptr && result.commitEnvelope != nullptr &&
      result.byteSize == destination.size &&
      result.commitEnvelope->destination == destination &&
      result.commitEnvelope->producerResult == *result.producerProof;
  if (context->proofValid) {
    std::memcpy(reinterpret_cast<void*>(destination.mappedPointer),
                result.bytes, result.byteSize);
  }
  CpuSkinMtControllerBodyCompletion completion = context->completion;
  completion.succeeded = completion.succeeded && context->proofValid;
  return completion;
}

struct BlockingCopyContext {
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
};

CpuSkinMtControllerBodyCompletion BlockingCopy(
    void* opaque,
    const CpuSkinMtControllerProducerResultView& result,
    const CpuSkinMtControllerDestinationProof& destination) noexcept {
  auto* context = static_cast<BlockingCopyContext*>(opaque);
  context->entered.store(true, std::memory_order_release);
  while (!context->release.load(std::memory_order_acquire))
    std::this_thread::yield();
  const bool valid = result.bytes != nullptr &&
      result.byteSize == destination.size;
  if (valid) {
    std::memcpy(reinterpret_cast<void*>(destination.mappedPointer),
                result.bytes, result.byteSize);
  }
  CpuSkinMtControllerBodyCompletion completion = GoodBodyCompletion(valid);
  return completion;
}

bool TestRuntimeGatesRemainClosed() {
  CHECK(!kCpuSkinMtControllerRuntimeIntegrated);
  CHECK(!kCpuSkinMtControllerNativeParityProven);
  CHECK(!kCpuSkinMtControllerConsumeEnabled);
  CHECK(!kCpuSkinMtControllerProductionDefault);
  return true;
}

bool TestDestinationFreeProducerResultProof() {
  ProofFixture f;
  const auto result = f.result(17u, 0x6bu);
  CHECK(result);
  CHECK(result->proofComplete());
  CHECK(result->proof().frozenKey == f.frozen);
  CHECK(result->proof().outputByteSize == f.destination.size);
  CHECK(result->proof().producerGeneration ==
        f.frozen.owner.producerGeneration);
  CHECK(result->proof().mxcsrStatusDelta ==
        CpuSkinMtCaptureMxcsrStatusDelta(
            kProducerMxcsrBefore, kProducerMxcsrAfter));
  CHECK(result->proof().mxcsrStatusDelta.raisedStatus == 0x04u);

  auto wrongFrozen = f.frozen;
  wrongFrozen.source.sealedContentToken++;
  CHECK(!CpuSkinMtControllerOwnedProducerResult::Create(
      wrongFrozen, f.eligibility(), 18u,
      std::vector<uint8_t>(f.destination.size, 0u),
      kProducerMxcsrBefore, kProducerMxcsrAfter));
  CHECK(!f.result(19u, 0u, kProducerMxcsrBefore,
                  kProducerMxcsrAfter ^ 0x2000u));
  return true;
}

bool TestPublicationIsNotConsumption() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
  CHECK(job.submit(kGeneration) == CpuSkinMtControllerResult::Applied);
  const auto result = f.result();
  CHECK(job.publishProducerResult(kGeneration, result) ==
        CpuSkinMtControllerResult::Published);
  CHECK(job.producerResultPublished());
  CHECK(!job.producerResultConsumed());
  auto snapshot = ledger.snapshot();
  CHECK(snapshot.producerResultPublications == 1u);
  CHECK(snapshot.producerResultConsumptions == 0u);
  CHECK(snapshot.liveProducerResults == 1u);

  CHECK(job.bindOuter(kGeneration, f.outer) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.awaitKernel(kGeneration, f.destination) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(SelectCopyRoute(job));
  CHECK(!job.producerResultConsumed());
  CHECK(CpuSkinMtControllerRenderCommitEnvelopeComplete(
      job.renderCommitEnvelopeProof()));
  CHECK(job.cancelOuter(kGeneration) ==
        CpuSkinMtControllerResult::Applied);
  snapshot = ledger.snapshot();
  CHECK(snapshot.producerResultConsumptions == 0u);
  CHECK(snapshot.producerResultAbandoned == 1u);
  CHECK(snapshot.producerClaimAbandoned == 1u);
  CHECK(snapshot.closureHolds());
  return true;
}

bool TestCopyCommitEnvelopeAndOuterSettlement() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
  CHECK(job.submit(kGeneration) == CpuSkinMtControllerResult::Applied);
  const auto result = f.result(23u, 0x7cu);
  CHECK(PublishResult(job, result));
  CHECK(job.bindOuter(kGeneration, f.outer) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.awaitKernel(kGeneration, f.destination) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(SelectCopyRoute(job));
  const auto envelope = job.renderCommitEnvelopeProof();
  CHECK(CpuSkinMtControllerRenderCommitEnvelopeComplete(envelope));
  CHECK(envelope.destination == f.destination);
  CHECK(envelope.producerResult == result->proof());
  CHECK(envelope.commitSerial == f.destination.lockSerial);
  auto forgedEnvelope = envelope;
  forgedEnvelope.destination.lockSerial++;
  CHECK(!CpuSkinMtControllerRenderCommitEnvelopeComplete(forgedEnvelope));
  forgedEnvelope = envelope;
  forgedEnvelope.producerResult.mxcsrStatusDelta.raisedStatus ^= 0x02u;
  CHECK(!CpuSkinMtControllerRenderCommitEnvelopeComplete(forgedEnvelope));

  CopyContext context;
  CHECK(job.copyProducerResultUnderLease(
      kGeneration, CopyToMappedDestination, &context) ==
      CpuSkinMtControllerResult::Consumed);
  CHECK(context.called && context.proofValid);
  CHECK(job.producerResultConsumed());
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
  auto settlement = job.outerSettlement();
  CHECK(settlement.settlementOpened);
  CHECK(settlement.bodyCompleted);
  CHECK(!settlement.unlockObserved);
  CHECK(settlement.producerResultConsumed);
  CHECK(settlement.bodyMxcsrStatusDelta ==
        result->proof().mxcsrStatusDelta);
  CHECK(f.mapped[f.destination.offset] == 0x7cu);

  CHECK(job.noteUnlock(kGeneration, f.unlock) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::CopiedNormal);
  const auto snapshot = ledger.snapshot();
  CHECK(snapshot.producerResultPublications == 1u);
  CHECK(snapshot.producerResultConsumptions == 1u);
  CHECK(snapshot.outerSettlementsOpened == 1u);
  CHECK(snapshot.outerSettlementsCompleted == 1u);
  CHECK(snapshot.mxcsrStatusDeltasRecorded == 1u);
  CHECK(snapshot.closureHolds());
  return true;
}

bool TestCopyMxcsrStatusDeltaMismatchFailsClosed() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
  CHECK(PrepareAwaiting(job, f));
  CHECK(PublishResult(job, f.result()));
  CHECK(SelectCopyRoute(job));
  CopyContext context;
  context.completion.mxcsrAfter = kSafeMxcsr | 0x09u;
  CHECK(job.copyProducerResultUnderLease(
      kGeneration, CopyToMappedDestination, &context) ==
      CpuSkinMtControllerResult::MxcsrStatusDeltaMismatch);
  CHECK(job.producerResultConsumed());
  CHECK(job.noteUnlock(kGeneration, f.unlock) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::CopyFault);
  const auto snapshot = ledger.snapshot();
  CHECK(snapshot.producerResultConsumptions == 1u);
  CHECK(snapshot.mxcsrStatusDeltaMismatches == 1u);
  CHECK(snapshot.guardedCopyBodyFaults == 1u);
  CHECK(snapshot.closureHolds());
  return true;
}

bool TestNativeLeaseDefersResetThroughOuterSettlement() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
  CHECK(PrepareAwaiting(job, f));
  const auto decision = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
  CHECK(decision.result == CpuSkinMtControllerResult::Applied);
  CHECK(decision.route == CpuSkinMtControllerKernelRoute::Native);
  CHECK(decision.nativeBodyLease.valid());
  CHECK(decision.nativeBodyLease.activeGeneration() == kGeneration);
  CHECK(decision.nativeBodyLease.lockSerial() == f.destination.lockSerial);
  CHECK(job.cancelForReset(kGeneration, kGeneration + 1u) ==
        CpuSkinMtControllerResult::Deferred);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
  CHECK(job.activeGeneration() == kGeneration);
  CHECK(job.outerSettlement().nativeBodyLeaseInProgress);

  CHECK(job.noteUnlock(kGeneration, f.unlock) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
  CHECK(job.completeNativeBody(
      kGeneration, decision.nativeBodyLease, GoodBodyCompletion()) ==
      CpuSkinMtControllerResult::Applied);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::ResetCancelled);
  CHECK(job.activeGeneration() == kGeneration + 1u);
  const auto snapshot = ledger.snapshot();
  CHECK(snapshot.deferredResetCancellations == 1u);
  CHECK(snapshot.nativeBodyLeases == 1u);
  CHECK(snapshot.nativeBodiesCompleted == 1u);
  CHECK(snapshot.outerSettlementsCompleted == 1u);
  CHECK(snapshot.closureHolds());
  return true;
}

bool TestNativeLeaseRejectsForgeryAndDuplicateOwner() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
  CHECK(PrepareAwaiting(job, f));
  const auto first = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
  CHECK(first.route == CpuSkinMtControllerKernelRoute::Native);
  CHECK(first.selectedNow && first.nativeBodyLease.valid());
  CHECK(job.completeNativeBody(
      kGeneration, CpuSkinMtControllerNativeBodyLease{},
      GoodBodyCompletion()) ==
      CpuSkinMtControllerResult::InvalidNativeLease);
  CHECK(!job.outerSettlement().bodyCompleted);
  CHECK(job.completeNativeBody(
      kGeneration, first.nativeBodyLease, GoodBodyCompletion()) ==
      CpuSkinMtControllerResult::Applied);
  CHECK(job.completeNativeBody(
      kGeneration, first.nativeBodyLease, GoodBodyCompletion()) ==
      CpuSkinMtControllerResult::AlreadyApplied);
  CHECK(job.noteUnlock(kGeneration, f.unlock) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::OriginalNormal);
  const auto snapshot = ledger.snapshot();
  CHECK(snapshot.invalidNativeLeaseRejects == 1u);
  CHECK(snapshot.nativeBodiesCompleted == 1u);
  CHECK(snapshot.closureHolds());
  return true;
}

bool TestNativeCompletionThenOuterCancelDefers() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
  CHECK(PrepareAwaiting(job, f));
  const auto decision = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
  CHECK(decision.nativeBodyLease.valid());
  CHECK(job.completeNativeBody(
      kGeneration, decision.nativeBodyLease, GoodBodyCompletion()) ==
      CpuSkinMtControllerResult::Applied);
  CHECK(job.cancelOuter(kGeneration) ==
        CpuSkinMtControllerResult::Deferred);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
  CHECK(job.noteUnlock(kGeneration, f.unlock) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::OuterCancelled);
  CHECK(ledger.snapshot().deferredOuterCancellations == 1u);
  CHECK(ledger.snapshot().closureHolds());
  return true;
}

bool TestNativeBodyUnlockRace() {
  for (uint32_t iteration = 0; iteration < 200u; iteration++) {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    const auto decision = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
    CHECK(decision.nativeBodyLease.valid());
    std::atomic<bool> start{false};
    CpuSkinMtControllerResult body =
        CpuSkinMtControllerResult::InvalidTransition;
    CpuSkinMtControllerResult unlock =
        CpuSkinMtControllerResult::InvalidTransition;
    std::thread bodyThread([&] {
      while (!start.load(std::memory_order_acquire)) { }
      body = job.completeNativeBody(
          kGeneration, decision.nativeBodyLease, GoodBodyCompletion());
    });
    std::thread unlockThread([&] {
      while (!start.load(std::memory_order_acquire)) { }
      unlock = job.noteUnlock(kGeneration, f.unlock);
    });
    start.store(true, std::memory_order_release);
    bodyThread.join();
    unlockThread.join();
    CHECK(body == CpuSkinMtControllerResult::Applied);
    CHECK(unlock == CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::OriginalNormal);
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestNativeResetBodyRaceDefers() {
  for (uint32_t iteration = 0; iteration < 200u; iteration++) {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    const auto decision = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
    CHECK(decision.nativeBodyLease.valid());
    std::atomic<bool> start{false};
    CpuSkinMtControllerResult body =
        CpuSkinMtControllerResult::InvalidTransition;
    CpuSkinMtControllerResult cancel =
        CpuSkinMtControllerResult::InvalidTransition;
    std::thread bodyThread([&] {
      while (!start.load(std::memory_order_acquire)) { }
      body = job.completeNativeBody(
          kGeneration, decision.nativeBodyLease, GoodBodyCompletion());
    });
    std::thread cancelThread([&] {
      while (!start.load(std::memory_order_acquire)) { }
      cancel = job.cancelForReset(kGeneration, kGeneration + 1u);
    });
    start.store(true, std::memory_order_release);
    bodyThread.join();
    cancelThread.join();
    CHECK(body == CpuSkinMtControllerResult::Applied);
    CHECK(cancel == CpuSkinMtControllerResult::Deferred);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
    CHECK(job.noteUnlock(kGeneration, f.unlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::ResetCancelled);
    CHECK(job.activeGeneration() == kGeneration + 1u);
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestCopyBodyResetRaceDefers() {
  for (uint32_t iteration = 0; iteration < 100u; iteration++) {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(PublishResult(job, f.result()));
    CHECK(SelectCopyRoute(job));
    BlockingCopyContext context;
    CpuSkinMtControllerResult copy =
        CpuSkinMtControllerResult::InvalidTransition;
    CpuSkinMtControllerResult cancel =
        CpuSkinMtControllerResult::InvalidTransition;
    std::thread copyThread([&] {
      copy = job.copyProducerResultUnderLease(
          kGeneration, BlockingCopy, &context);
    });
    while (!context.entered.load(std::memory_order_acquire))
      std::this_thread::yield();
    std::thread cancelThread([&] {
      cancel = job.cancelForReset(kGeneration, kGeneration + 1u);
    });
    context.release.store(true, std::memory_order_release);
    copyThread.join();
    cancelThread.join();
    CHECK(copy == CpuSkinMtControllerResult::Consumed);
    CHECK(cancel == CpuSkinMtControllerResult::Deferred);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
    CHECK(job.noteUnlock(kGeneration, f.unlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::ResetCancelled);
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestStateLockContentionIssuesNativeLease() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
  CHECK(PrepareAwaiting(job, f));
  CHECK(PublishResult(job, f.result()));
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::thread holder([&] {
    CpuSkinMtControllerJobTestPeer::holdStateMutex(
        job, entered, release);
  });
  while (!entered.load(std::memory_order_acquire))
    std::this_thread::yield();
  const auto decision = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
  CHECK(decision.route == CpuSkinMtControllerKernelRoute::Native);
  CHECK(decision.reason ==
        CpuSkinMtControllerKernelReason::StateLockContended);
  CHECK(!decision.stateLockAcquired);
  CHECK(decision.selectedNow && decision.nativeBodyLease.valid());
  release.store(true, std::memory_order_release);
  holder.join();
  CHECK(job.cancelOuter(kGeneration) == CpuSkinMtControllerResult::Deferred);
  CHECK(job.completeNativeBody(
      kGeneration, decision.nativeBodyLease, GoodBodyCompletion()) ==
      CpuSkinMtControllerResult::Applied);
  CHECK(job.noteUnlock(kGeneration, f.unlock) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::OuterCancelled);
  CHECK(ledger.snapshot().producerResultAbandoned == 1u);
  CHECK(ledger.snapshot().closureHolds());
  return true;
}

bool TestProducerPublicationVersusNativeRace() {
  for (uint32_t iteration = 0; iteration < 200u; iteration++) {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    const auto result = f.result();
    std::atomic<bool> start{false};
    CpuSkinMtControllerResult publication =
        CpuSkinMtControllerResult::InvalidTransition;
    CpuSkinMtControllerKernelDecision decision;
    std::thread producer([&] {
      while (!start.load(std::memory_order_acquire)) { }
      publication = job.publishProducerResult(kGeneration, result);
    });
    std::thread owner([&] {
      while (!start.load(std::memory_order_acquire)) { }
      decision = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
    });
    start.store(true, std::memory_order_release);
    producer.join();
    owner.join();
    CHECK(decision.result == CpuSkinMtControllerResult::Applied);
    CHECK(decision.selectedNow);
    if (decision.route == CpuSkinMtControllerKernelRoute::Copy) {
      CHECK(publication == CpuSkinMtControllerResult::Published);
      CopyContext context;
      CHECK(job.copyProducerResultUnderLease(
          kGeneration, CopyToMappedDestination, &context) ==
          CpuSkinMtControllerResult::Consumed);
    } else {
      CHECK(decision.route == CpuSkinMtControllerKernelRoute::Native);
      CHECK(decision.nativeBodyLease.valid());
      CHECK(publication == CpuSkinMtControllerResult::Published ||
            publication == CpuSkinMtControllerResult::LateProducerReady);
      CHECK(job.completeNativeBody(
          kGeneration, decision.nativeBodyLease, GoodBodyCompletion()) ==
          CpuSkinMtControllerResult::Applied);
    }
    CHECK(job.noteUnlock(kGeneration, f.unlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() ==
          (decision.route == CpuSkinMtControllerKernelRoute::Copy
              ? CpuSkinMtControllerTerminal::CopiedNormal
              : CpuSkinMtControllerTerminal::OriginalNormal));
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestSingleRouteClaimRace() {
  for (uint32_t iteration = 0; iteration < 200u; iteration++) {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(PublishResult(job, f.result()));
    std::atomic<bool> start{false};
    CpuSkinMtControllerKernelDecision first;
    CpuSkinMtControllerKernelDecision second;
    std::thread a([&] {
      while (!start.load(std::memory_order_acquire)) { }
      first = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
    });
    std::thread b([&] {
      while (!start.load(std::memory_order_acquire)) { }
      second = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
    });
    start.store(true, std::memory_order_release);
    a.join();
    b.join();
    CHECK(first.selectedNow != second.selectedNow);
    const auto& selected = first.selectedNow ? first : second;
    if (selected.route == CpuSkinMtControllerKernelRoute::Copy) {
      CopyContext context;
      CHECK(job.copyProducerResultUnderLease(
          kGeneration, CopyToMappedDestination, &context) ==
          CpuSkinMtControllerResult::Consumed);
    } else {
      CHECK(selected.route == CpuSkinMtControllerKernelRoute::Native);
      CHECK(selected.nativeBodyLease.valid());
      CHECK(job.completeNativeBody(
          kGeneration, selected.nativeBodyLease, GoodBodyCompletion()) ==
          CpuSkinMtControllerResult::Applied);
    }
    CHECK(job.noteUnlock(kGeneration, f.unlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestUnlockBeforeCopyRejectsWriteAndSettlesClaim() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
  CHECK(PrepareAwaiting(job, f));
  CHECK(PublishResult(job, f.result()));
  CHECK(SelectCopyRoute(job));
  CHECK(job.noteUnlock(kGeneration, f.unlock) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::CopyFault);
  CopyContext context;
  CHECK(job.copyProducerResultUnderLease(
      kGeneration, CopyToMappedDestination, &context) ==
      CpuSkinMtControllerResult::CopyAfterUnlock);
  CHECK(!context.called);
  CHECK(!job.producerResultConsumed());
  const auto snapshot = ledger.snapshot();
  CHECK(snapshot.producerResultAbandoned == 1u);
  CHECK(snapshot.producerClaimAbandoned == 1u);
  CHECK(snapshot.closureHolds());
  return true;
}

bool TestTemplateCancelWaitsForCopyUnlock() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
  CHECK(PrepareAwaiting(job, f));
  CHECK(PublishResult(job, f.result()));
  CHECK(SelectCopyRoute(job));
  CopyContext context;
  CHECK(job.copyProducerResultUnderLease(
      kGeneration, CopyToMappedDestination, &context) ==
      CpuSkinMtControllerResult::Consumed);
  CHECK(job.cancelTemplateMismatch(kGeneration) ==
        CpuSkinMtControllerResult::Deferred);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
  CHECK(job.noteUnlock(kGeneration, f.unlock) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::TemplateMismatch);
  CHECK(ledger.snapshot().deferredTemplateCancellations == 1u);
  CHECK(ledger.snapshot().closureHolds());
  return true;
}

bool TestDestructorClosesOutstandingNativeLease() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  {
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    const auto decision = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
    CHECK(decision.nativeBodyLease.valid());
  }
  const auto snapshot = ledger.snapshot();
  CHECK(snapshot.nativeBodyLeasesAbandoned == 1u);
  CHECK(snapshot.outerCancelled == 1u);
  CHECK(snapshot.liveJobs == 0u);
  CHECK(snapshot.closureHolds());
  return true;
}

bool TestMxcsrStatusDeltaContract() {
  const auto delta = CpuSkinMtCaptureMxcsrStatusDelta(
      kSafeMxcsr | 0x03u, kSafeMxcsr | 0x0au);
  CHECK(delta.valid);
  CHECK(delta.normalizedControl == kSafeMxcsr);
  CHECK(delta.statusBefore == 0x03u);
  CHECK(delta.statusAfter == 0x0au);
  CHECK(delta.raisedStatus == 0x08u);
  CHECK(delta.clearedStatus == 0x01u);
  CHECK(!CpuSkinMtCaptureMxcsrStatusDelta(
      kSafeMxcsr, kSafeMxcsr ^ 0x2000u).valid);
  CHECK(!CpuSkinMtCaptureMxcsrStatusDelta(
      kSafeMxcsr, kSafeMxcsr | 0x10000u).valid);
  return true;
}

bool TestEligibilityBoundaries() {
  ProofFixture minimum;
  CHECK(CpuSkinMtValidateFormat2Eligibility(
      minimum.eligibility()).eligible);
  CHECK(CpuSkinMtEligibilityMatchesFrozenKey(
      minimum.frozen, minimum.eligibility()));
  ProofFixture maximum(kCpuSkinMtFormat2MaxVertices);
  CHECK(CpuSkinMtValidateFormat2Eligibility(
      maximum.eligibility()).eligible);

  ProofFixture below(kCpuSkinMtFormat2MinVertices - 1u);
  CHECK(!CpuSkinMtValidateFormat2Eligibility(
      below.eligibility()).eligible);
  auto input = minimum.eligibility();
  input.path = CpuSkinMtControllerPath::Special;
  CHECK(!CpuSkinMtValidateFormat2Eligibility(input).eligible);
  input = minimum.eligibility();
  input.paletteSealedToken = 0u;
  CHECK(!CpuSkinMtValidateFormat2Eligibility(input).eligible);
  minimum.positions[0] = std::numeric_limits<float>::quiet_NaN();
  CHECK(!CpuSkinMtValidateFormat2Eligibility(
      minimum.eligibility()).eligible);
  return true;
}

bool TestKeyMismatchAndImmediateResetClosure() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  {
    CpuSkinMtControllerJob mismatch(f.frozen, kGeneration, ledger);
    CHECK(mismatch.submit(kGeneration) ==
          CpuSkinMtControllerResult::Applied);
    auto wrongOuter = f.outer;
    wrongOuter.sourceGeneration++;
    CHECK(mismatch.bindOuter(kGeneration, wrongOuter) ==
          CpuSkinMtControllerResult::KeyMismatch);
    CHECK(mismatch.cancelTemplateMismatch(kGeneration) ==
          CpuSkinMtControllerResult::Applied);

    CpuSkinMtControllerJob reset(f.frozen, kGeneration, ledger);
    CHECK(reset.submit(kGeneration) == CpuSkinMtControllerResult::Applied);
    CHECK(reset.cancelForReset(kGeneration, kGeneration + 1u) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(reset.activeGeneration() == kGeneration + 1u);
    CHECK(reset.submit(kGeneration) ==
          CpuSkinMtControllerResult::StaleGeneration);
  }
  const auto snapshot = ledger.snapshot();
  CHECK(snapshot.templateMismatch == 1u);
  CHECK(snapshot.resetCancelled == 1u);
  CHECK(snapshot.keyMismatchRejects == 1u);
  CHECK(snapshot.staleGenerationRejects == 1u);
  CHECK(snapshot.closureHolds());
  return true;
}

}  // namespace

int main() {
  struct TestCase {
    const char* name;
    bool (*run)();
  };

  const TestCase tests[] = {
      {"runtime gates", TestRuntimeGatesRemainClosed},
      {"destination-free producer", TestDestinationFreeProducerResultProof},
      {"publication is not consumption", TestPublicationIsNotConsumption},
      {"commit envelope/settlement",
       TestCopyCommitEnvelopeAndOuterSettlement},
      {"copy MXCSR status mismatch",
       TestCopyMxcsrStatusDeltaMismatchFailsClosed},
      {"native lease reset deferral",
       TestNativeLeaseDefersResetThroughOuterSettlement},
      {"native lease forgery", TestNativeLeaseRejectsForgeryAndDuplicateOwner},
      {"native outer cancel deferral",
       TestNativeCompletionThenOuterCancelDefers},
      {"native body/unlock race", TestNativeBodyUnlockRace},
      {"native reset/body race", TestNativeResetBodyRaceDefers},
      {"copy body/reset race", TestCopyBodyResetRaceDefers},
      {"lock contention lease", TestStateLockContentionIssuesNativeLease},
      {"producer/native race", TestProducerPublicationVersusNativeRace},
      {"single route race", TestSingleRouteClaimRace},
      {"unlock rejects copy", TestUnlockBeforeCopyRejectsWriteAndSettlesClaim},
      {"template cancel settlement", TestTemplateCancelWaitsForCopyUnlock},
      {"destructor lease closure", TestDestructorClosesOutstandingNativeLease},
      {"MXCSR status delta", TestMxcsrStatusDeltaContract},
      {"eligibility boundaries", TestEligibilityBoundaries},
      {"key/reset closure", TestKeyMismatchAndImmediateResetClosure},
  };

  for (const auto& test : tests) {
    if (!test.run()) {
      std::cerr << "FAIL: " << test.name << '\n';
      return 1;
    }
    std::cout << "PASS: " << test.name << '\n';
  }

  std::cout << "PASS: CPU-MT controller Phase 2A value contract\n";
  return 0;
}
