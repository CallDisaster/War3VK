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

  CpuSkinMtControllerExactKey exactKey() const {
    return CpuSkinMtControllerExactKey{
        frozen.owner, frozen.flush, frozen.source, frozen.palette,
        outer, destination};
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
    input.frozenMxcsr = kSafeMxcsr;
    input.currentMxcsr = kSafeMxcsr;
    return input;
  }

  std::shared_ptr<const CpuSkinMtControllerOwnedOutput> output(
      uint64_t serial = 1u,
      uint8_t fill = 0x5au) const {
    return CpuSkinMtControllerOwnedOutput::Create(
        exactKey(), eligibility(), serial,
        std::vector<uint8_t>(destination.size, fill));
  }
};

bool PrepareAwaiting(CpuSkinMtControllerJob& job,
                     const ProofFixture& fixture) {
  CHECK(job.submit(kGeneration) == CpuSkinMtControllerResult::Applied);
  CHECK(job.bindOuter(kGeneration, fixture.outer) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.awaitKernel(kGeneration, fixture.destination) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(CpuSkinMtControllerExactKeyComplete(job.exactKey()));
  return true;
}

bool SelectCopyRoute(CpuSkinMtControllerJob& job,
                     uint32_t currentMxcsr = kSafeMxcsr) {
  const CpuSkinMtControllerKernelDecision decision =
      job.trySelectKernelRoute(kGeneration, currentMxcsr);
  CHECK(decision.result == CpuSkinMtControllerResult::Applied);
  CHECK(decision.route == CpuSkinMtControllerKernelRoute::Copy);
  CHECK(decision.reason ==
        CpuSkinMtControllerKernelReason::ReadyOwnedOutput);
  CHECK(decision.stateLockAcquired);
  CHECK(decision.selectedNow);
  return true;
}

bool SelectNativeRoute(
    CpuSkinMtControllerJob& job,
    CpuSkinMtControllerKernelReason expectedReason =
        CpuSkinMtControllerKernelReason::ProducerNotReady) {
  const CpuSkinMtControllerKernelDecision decision =
      job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
  CHECK(decision.result == CpuSkinMtControllerResult::Applied);
  CHECK(decision.route == CpuSkinMtControllerKernelRoute::Native);
  CHECK(decision.reason == expectedReason);
  CHECK(decision.selectedNow);
  return true;
}

bool HasFailure(const CpuSkinMtFormat2EligibilityResult& result,
                CpuSkinMtFormat2EligibilityFailure failure) {
  return (result.failureMask & static_cast<uint64_t>(failure)) != 0u;
}

struct CopyContext {
  uint32_t calls = 0;
  bool bodyResult = true;
  uint8_t expectedFill = 0x5au;
};

bool CopyToMappedDestination(
    void* opaque,
    const CpuSkinMtControllerOutputView& output,
    const CpuSkinMtControllerDestinationProof& destination) noexcept {
  auto* context = static_cast<CopyContext*>(opaque);
  context->calls++;
  if (output.bytes == nullptr || output.proof == nullptr ||
      output.byteSize != destination.size ||
      output.proof->ownedBytesIdentity !=
          reinterpret_cast<uintptr_t>(output.bytes) ||
      output.proof->outputByteSize != output.byteSize ||
      output.bytes[0] != context->expectedFill)
    return false;
  std::memcpy(reinterpret_cast<void*>(destination.mappedPointer),
              output.bytes, output.byteSize);
  return context->bodyResult;
}

struct BlockingCopyContext {
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::atomic<uint32_t>* sequence = nullptr;
  uint32_t writeOrder = 0;
};

bool BlockingCopy(
    void* opaque,
    const CpuSkinMtControllerOutputView& output,
    const CpuSkinMtControllerDestinationProof& destination) noexcept {
  auto* context = static_cast<BlockingCopyContext*>(opaque);
  context->entered.store(true, std::memory_order_release);
  while (!context->release.load(std::memory_order_acquire))
    std::this_thread::yield();
  std::memcpy(reinterpret_cast<void*>(destination.mappedPointer),
              output.bytes, output.byteSize);
  context->writeOrder =
      context->sequence->fetch_add(1u, std::memory_order_acq_rel) + 1u;
  return true;
}

bool TestRuntimeGatesRemainClosed() {
  CHECK(!kCpuSkinMtControllerRuntimeIntegrated);
  CHECK(!kCpuSkinMtControllerNativeParityProven);
  CHECK(!kCpuSkinMtControllerConsumeEnabled);
  CHECK(!kCpuSkinMtControllerProductionDefault);
  return true;
}

bool TestExactKeyAndOutputAuthority() {
  ProofFixture f;
  const auto exact = f.exactKey();
  CHECK(CpuSkinMtControllerFrozenKeyValid(f.frozen));
  CHECK(CpuSkinMtControllerOuterMatchesFrozen(f.frozen, f.outer));
  CHECK(CpuSkinMtControllerDestinationValid(
      f.frozen, f.outer, f.destination));
  CHECK(CpuSkinMtControllerUnlockMatchesDestination(
      f.destination, f.unlock));
  CHECK(CpuSkinMtControllerExactKeyComplete(exact));
  CHECK(CpuSkinMtEligibilityMatchesExactKey(exact, f.eligibility()));

  const auto output = f.output(17u, 0x6bu);
  CHECK(output);
  CHECK(output->proofComplete());
  CHECK(output->proof().exactKey == exact);
  CHECK(output->proof().resultSerial == 17u);
  CHECK(output->proof().producerGeneration ==
        exact.owner.producerGeneration);
  CHECK(output->proof().ownedBytesIdentity ==
        reinterpret_cast<uintptr_t>(output->data()));
  CHECK(output->proof().outputByteSize == exact.destination.size);
  CHECK(output->proof().normalizedMxcsr == kSafeMxcsr);
  CHECK(output->proof().proofVersion == kCpuSkinMtOutputProofVersion);

  auto badEligibility = f.eligibility();
  badEligibility.positions++;
  CHECK(!CpuSkinMtEligibilityMatchesExactKey(exact, badEligibility));
  CHECK(!CpuSkinMtControllerOwnedOutput::Create(
      exact, badEligibility, 19u,
      std::vector<uint8_t>(f.destination.size, 0u)));
  badEligibility = f.eligibility();
  badEligibility.currentMxcsr ^= 0x2000u;
  CHECK(!CpuSkinMtEligibilityMatchesExactKey(exact, badEligibility));
  badEligibility = f.eligibility();
  badEligibility.paletteSealedToken++;
  CHECK(!CpuSkinMtEligibilityMatchesExactKey(exact, badEligibility));
  CHECK(!CpuSkinMtControllerOwnedOutput::Create(
      exact, f.eligibility(), 0u,
      std::vector<uint8_t>(f.destination.size, 0u)));
  CHECK(!CpuSkinMtControllerOwnedOutput::Create(
      exact, f.eligibility(), 21u,
      std::vector<uint8_t>(f.destination.size - 1u, 0u)));
  return true;
}

bool TestOwnedOutputLifetimeAndUnlockCommit() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
  CHECK(PrepareAwaiting(job, f));

  auto output = f.output();
  CHECK(output);
  std::weak_ptr<const CpuSkinMtControllerOwnedOutput> weak = output;
  CHECK(job.publishProducerReady(kGeneration, output) ==
        CpuSkinMtControllerResult::Applied);
  output.reset();
  CHECK(!weak.expired());
  CHECK(job.producerReady());
  CHECK(job.outputProof().exactKey == f.exactKey());
  CHECK(SelectCopyRoute(job));

  CopyContext copy;
  CHECK(job.copyOutputUnderLease(kGeneration, CopyToMappedDestination,
                                 &copy) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(copy.calls == 1u);
  CHECK(job.state() == CpuSkinMtControllerState::CopyAwaitingUnlock);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
  CHECK(!weak.expired());
  CHECK(f.mapped[f.destination.offset] == copy.expectedFill);

  CHECK(job.noteUnlock(kGeneration, f.unlock) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::CopiedNormal);
  CHECK(weak.expired());
  const auto snapshot = ledger.snapshot();
  CHECK(snapshot.jobsCreated == 1u);
  CHECK(snapshot.producerReadyPublications == 1u);
  CHECK(snapshot.copySelections == 1u);
  CHECK(snapshot.guardedCopies == 1u);
  CHECK(snapshot.successfulUnlocks == 1u);
  CHECK(snapshot.copiedNormal == 1u);
  CHECK(snapshot.closureHolds());
  return true;
}

bool TestOutputMismatchAndCommitMxcsr() {
  {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(SelectNativeRoute(job));
    CHECK(job.completeOriginalNormal(kGeneration) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.noteUnlock(kGeneration, f.unlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(ledger.snapshot().producerNotReadyRejects == 1u);
    CHECK(ledger.snapshot().closureHolds());
  }

  {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    auto changedExact = f.exactKey();
    changedExact.destination.lockSerial++;
    const auto wrong = CpuSkinMtControllerOwnedOutput::Create(
        changedExact, f.eligibility(), 31u,
        std::vector<uint8_t>(f.destination.size, 0x11u));
    CHECK(wrong);
    CHECK(job.publishProducerReady(kGeneration, wrong) ==
          CpuSkinMtControllerResult::KeyMismatch);

    const auto output = f.output(33u);
    const auto duplicateValue = f.output(34u);
    CHECK(job.publishProducerReady(kGeneration, output) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.publishProducerReady(kGeneration, output) ==
          CpuSkinMtControllerResult::AlreadyApplied);
    CHECK(job.publishProducerReady(kGeneration, duplicateValue) ==
          CpuSkinMtControllerResult::KeyMismatch);
    const CpuSkinMtControllerKernelDecision mismatch =
        job.trySelectKernelRoute(kGeneration, kSafeMxcsr ^ 0x2000u);
    CHECK(mismatch.result == CpuSkinMtControllerResult::Applied);
    CHECK(mismatch.route == CpuSkinMtControllerKernelRoute::Native);
    CHECK(mismatch.reason ==
          CpuSkinMtControllerKernelReason::MxcsrMismatch);
    CHECK(job.completeOriginalNormal(kGeneration) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.noteUnlock(kGeneration, f.unlock) ==
          CpuSkinMtControllerResult::Applied);
    const auto snapshot = ledger.snapshot();
    CHECK(snapshot.keyMismatchRejects == 2u);
    CHECK(snapshot.mxcsrMismatchRejects == 1u);
    CHECK(snapshot.originalNormal == 1u);
    CHECK(snapshot.closureHolds());
  }

  {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(job.publishProducerReady(kGeneration, f.output()) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(SelectCopyRoute(job, kSafeMxcsr | 0x21u));
    CHECK(job.cancelOuter(kGeneration) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(ledger.snapshot().copyCancelled == 1u);
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestKernelWindowContentionFallsBackImmediately() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
  CHECK(PrepareAwaiting(job, f));
  auto output = f.output();
  std::weak_ptr<const CpuSkinMtControllerOwnedOutput> weak = output;
  CHECK(job.publishProducerReady(kGeneration, output) ==
        CpuSkinMtControllerResult::Applied);
  output.reset();
  CHECK(!weak.expired());

  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::thread holder([&] {
    CpuSkinMtControllerJobTestPeer::holdStateMutex(
        job, entered, release);
  });
  while (!entered.load(std::memory_order_acquire))
    std::this_thread::yield();

  // This call is intentionally made by the thread that must later release the
  // holder. Any blocking mutex acquisition would deadlock this test.
  const CpuSkinMtControllerKernelDecision decision =
      job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
  CHECK(decision.result == CpuSkinMtControllerResult::Applied);
  CHECK(decision.route == CpuSkinMtControllerKernelRoute::Native);
  CHECK(decision.reason ==
        CpuSkinMtControllerKernelReason::StateLockContended);
  CHECK(!decision.stateLockAcquired);
  CHECK(decision.selectedNow);
  CHECK(!release.load(std::memory_order_acquire));

  release.store(true, std::memory_order_release);
  holder.join();
  CHECK(job.publishProducerReady(kGeneration, f.output(2u)) ==
        CpuSkinMtControllerResult::LateProducerReady);
  CHECK(job.completeOriginalNormal(kGeneration) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(weak.expired());
  CHECK(job.noteUnlock(kGeneration, f.unlock) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::OriginalNormal);
  const auto snapshot = ledger.snapshot();
  CHECK(snapshot.copySelections == 0u);
  CHECK(snapshot.nativeSelections == 1u);
  CHECK(snapshot.lateProducerReadyRejects == 1u);
  CHECK(snapshot.closureHolds());
  return true;
}

bool TestCopyFaultsAreUnlockFinalized() {
  {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(job.publishProducerReady(kGeneration, f.output()) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(SelectCopyRoute(job));
    CopyContext copy;
    CHECK(job.copyOutputUnderLease(kGeneration, CopyToMappedDestination,
                                   &copy) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
    auto failedUnlock = f.unlock;
    failedUnlock.result = -1;
    CHECK(job.noteUnlock(kGeneration, failedUnlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::CopyFault);
    CHECK(ledger.snapshot().failedUnlocks == 1u);
    CHECK(ledger.snapshot().copyFault == 1u);
    CHECK(ledger.snapshot().closureHolds());
  }

  {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(job.publishProducerReady(kGeneration, f.output()) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(SelectCopyRoute(job));
    CopyContext copy;
    copy.bodyResult = false;
    CHECK(job.copyOutputUnderLease(kGeneration, CopyToMappedDestination,
                                   &copy) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
    CHECK(job.noteUnlock(kGeneration, f.unlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::CopyFault);
    CHECK(ledger.snapshot().guardedCopyBodyFaults == 1u);
    CHECK(ledger.snapshot().copyFault == 1u);
    CHECK(ledger.snapshot().closureHolds());
  }

  return true;
}

bool TestNativeCompletionRequiresUnlock() {
  {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(SelectNativeRoute(job));
    const auto late = f.output(2u);
    CHECK(job.publishProducerReady(kGeneration, late) ==
          CpuSkinMtControllerResult::LateProducerReady);
    CHECK(!job.producerReady());
    CHECK(job.completeOriginalNormal(kGeneration) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
    CHECK(job.noteUnlock(kGeneration, f.unlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::OriginalNormal);
    CHECK(ledger.snapshot().lateProducerReadyRejects == 1u);
    CHECK(ledger.snapshot().closureHolds());
  }

  {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(SelectNativeRoute(job));
    CHECK(job.noteUnlock(kGeneration, f.unlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::OriginalFault);
    CHECK(job.completeOriginalNormal(kGeneration) ==
          CpuSkinMtControllerResult::DoubleTerminal);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::OriginalFault);
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestNativeFailureClassification() {
  {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(SelectNativeRoute(job));
    CHECK(job.completeOriginalNormal(kGeneration) ==
          CpuSkinMtControllerResult::Applied);
    auto failedUnlock = f.unlock;
    failedUnlock.result = -7;
    CHECK(job.noteUnlock(kGeneration, failedUnlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::OriginalFault);
    CHECK(ledger.snapshot().originalFault == 1u);
    CHECK(ledger.snapshot().closureHolds());
  }

  {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(SelectNativeRoute(job));
    CHECK(job.completeOriginalFault(kGeneration) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
    CHECK(job.noteUnlock(kGeneration, f.unlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::OriginalFault);
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestNativeBodyVersusUnlockRace() {
  for (uint32_t iteration = 0; iteration < 300u; iteration++) {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(SelectNativeRoute(job));

    std::atomic<bool> start{false};
    CpuSkinMtControllerResult bodyResult =
        CpuSkinMtControllerResult::InvalidTransition;
    CpuSkinMtControllerResult unlockResult =
        CpuSkinMtControllerResult::InvalidTransition;
    std::thread body([&] {
      while (!start.load(std::memory_order_acquire)) { }
      bodyResult = job.completeOriginalNormal(kGeneration);
    });
    std::thread unlock([&] {
      while (!start.load(std::memory_order_acquire)) { }
      unlockResult = job.noteUnlock(kGeneration, f.unlock);
    });
    start.store(true, std::memory_order_release);
    body.join();
    unlock.join();

    CHECK(unlockResult == CpuSkinMtControllerResult::Applied);
    const CpuSkinMtControllerTerminal terminal = job.terminal();
    CHECK(terminal == CpuSkinMtControllerTerminal::OriginalNormal ||
          terminal == CpuSkinMtControllerTerminal::OriginalFault);
    if (terminal == CpuSkinMtControllerTerminal::OriginalNormal) {
      CHECK(bodyResult == CpuSkinMtControllerResult::Applied);
    } else {
      CHECK(bodyResult == CpuSkinMtControllerResult::DoubleTerminal);
    }
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestUnlockBeforeCopyRejectsAllWrites() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
  CHECK(PrepareAwaiting(job, f));
  CHECK(job.publishProducerReady(kGeneration, f.output()) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(SelectCopyRoute(job));
  CHECK(job.noteUnlock(kGeneration, f.unlock) ==
        CpuSkinMtControllerResult::Applied);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::CopyFault);
  CopyContext copy;
  CHECK(job.copyOutputUnderLease(kGeneration, CopyToMappedDestination,
                                 &copy) ==
        CpuSkinMtControllerResult::CopyAfterUnlock);
  CHECK(copy.calls == 0u);
  CHECK(f.mapped[f.destination.offset] == 0xccu);
  CHECK(job.terminal() == CpuSkinMtControllerTerminal::CopyFault);
  CHECK(ledger.snapshot().copyAfterUnlockRejects == 1u);
  CHECK(ledger.snapshot().closureHolds());
  return true;
}

bool TestCancelBeforeCopyRejectsAllWrites() {
  for (uint32_t route = 0; route < 2u; route++) {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(job.publishProducerReady(kGeneration, f.output()) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(SelectCopyRoute(job));
    if (route == 0u) {
      CHECK(job.cancelForReset(kGeneration, kGeneration + 1u) ==
            CpuSkinMtControllerResult::Applied);
    } else {
      CHECK(job.cancelOuter(kGeneration) ==
            CpuSkinMtControllerResult::Applied);
    }
    CopyContext copy;
    const CpuSkinMtControllerResult copyResult =
        job.copyOutputUnderLease(kGeneration, CopyToMappedDestination, &copy);
    CHECK(copyResult == (route == 0u
        ? CpuSkinMtControllerResult::StaleGeneration
        : CpuSkinMtControllerResult::DoubleTerminal));
    CHECK(copy.calls == 0u);
    CHECK(f.mapped[f.destination.offset] == 0xccu);
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestGuardedCopyLinearizesBeforeUnlock() {
  for (uint32_t iteration = 0; iteration < 100u; iteration++) {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(job.publishProducerReady(kGeneration, f.output()) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(SelectCopyRoute(job));

    std::atomic<uint32_t> sequence{0u};
    BlockingCopyContext copy;
    copy.sequence = &sequence;
    CpuSkinMtControllerResult copyResult =
        CpuSkinMtControllerResult::InvalidTransition;
    CpuSkinMtControllerResult unlockResult =
        CpuSkinMtControllerResult::InvalidTransition;
    std::atomic<bool> unlockAttempted{false};
    uint32_t unlockOrder = 0u;

    std::thread copyThread([&] {
      copyResult = job.copyOutputUnderLease(
          kGeneration, BlockingCopy, &copy);
    });
    while (!copy.entered.load(std::memory_order_acquire))
      std::this_thread::yield();

    std::thread unlockThread([&] {
      unlockAttempted.store(true, std::memory_order_release);
      unlockResult = job.noteUnlock(kGeneration, f.unlock);
      unlockOrder = sequence.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    });
    while (!unlockAttempted.load(std::memory_order_acquire))
      std::this_thread::yield();
    copy.release.store(true, std::memory_order_release);
    copyThread.join();
    unlockThread.join();

    CHECK(copyResult == CpuSkinMtControllerResult::Applied);
    CHECK(unlockResult == CpuSkinMtControllerResult::Applied);
    CHECK(copy.writeOrder != 0u);
    CHECK(copy.writeOrder < unlockOrder);
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::CopiedNormal);
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestGuardedCopyLinearizesBeforeResetOrCancel() {
  for (uint32_t iteration = 0; iteration < 100u; iteration++) {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(job.publishProducerReady(kGeneration, f.output()) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(SelectCopyRoute(job));

    std::atomic<uint32_t> sequence{0u};
    BlockingCopyContext copy;
    copy.sequence = &sequence;
    CpuSkinMtControllerResult copyResult =
        CpuSkinMtControllerResult::InvalidTransition;
    CpuSkinMtControllerResult cancelResult =
        CpuSkinMtControllerResult::InvalidTransition;
    std::atomic<bool> cancelAttempted{false};
    uint32_t cancelOrder = 0u;

    std::thread copyThread([&] {
      copyResult = job.copyOutputUnderLease(
          kGeneration, BlockingCopy, &copy);
    });
    while (!copy.entered.load(std::memory_order_acquire))
      std::this_thread::yield();

    std::thread cancelThread([&] {
      cancelAttempted.store(true, std::memory_order_release);
      if ((iteration & 1u) == 0u) {
        cancelResult = job.cancelForReset(
            kGeneration, kGeneration + 1u);
      } else {
        cancelResult = job.cancelOuter(kGeneration);
      }
      cancelOrder = sequence.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    });
    while (!cancelAttempted.load(std::memory_order_acquire))
      std::this_thread::yield();
    copy.release.store(true, std::memory_order_release);
    copyThread.join();
    cancelThread.join();

    CHECK(copyResult == CpuSkinMtControllerResult::Applied);
    CHECK(cancelResult == CpuSkinMtControllerResult::Applied);
    CHECK(copy.writeOrder < cancelOrder);
    const CpuSkinMtControllerTerminal expected = (iteration & 1u) == 0u
        ? CpuSkinMtControllerTerminal::ResetCancelled
        : CpuSkinMtControllerTerminal::OuterCancelled;
    CHECK(job.terminal() == expected);
    const CpuSkinMtControllerResult postCancelUnlock =
        job.noteUnlock(job.activeGeneration(), f.unlock);
    CHECK(postCancelUnlock == ((iteration & 1u) == 0u
        ? CpuSkinMtControllerResult::StaleGeneration
        : CpuSkinMtControllerResult::Applied));
    CHECK(job.terminal() == expected);
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestClaimVersusNativeRace() {
  for (uint32_t iteration = 0; iteration < 200u; iteration++) {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    CHECK(job.publishProducerReady(kGeneration, f.output()) ==
          CpuSkinMtControllerResult::Applied);

    std::atomic<bool> start{false};
    CpuSkinMtControllerKernelDecision first;
    CpuSkinMtControllerKernelDecision second;
    std::thread firstOwner([&] {
      while (!start.load(std::memory_order_acquire)) { }
      first = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
    });
    std::thread secondOwner([&] {
      while (!start.load(std::memory_order_acquire)) { }
      second = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
    });
    start.store(true, std::memory_order_release);
    firstOwner.join();
    secondOwner.join();
    CHECK(first.route == second.route);
    CHECK(first.route == CpuSkinMtControllerKernelRoute::Copy ||
          first.route == CpuSkinMtControllerKernelRoute::Native);
    CHECK(first.selectedNow != second.selectedNow);
    CHECK((first.result == CpuSkinMtControllerResult::Applied) !=
          (second.result == CpuSkinMtControllerResult::Applied));

    if (first.route == CpuSkinMtControllerKernelRoute::Copy) {
      CopyContext context;
      CHECK(job.copyOutputUnderLease(kGeneration, CopyToMappedDestination,
                                     &context) ==
            CpuSkinMtControllerResult::Applied);
    } else {
      CHECK(job.completeOriginalNormal(kGeneration) ==
            CpuSkinMtControllerResult::Applied);
    }
    CHECK(job.terminal() == CpuSkinMtControllerTerminal::None);
    CHECK(job.noteUnlock(kGeneration, f.unlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == (first.route ==
                                      CpuSkinMtControllerKernelRoute::Copy
        ? CpuSkinMtControllerTerminal::CopiedNormal
        : CpuSkinMtControllerTerminal::OriginalNormal));
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestProducerVersusNativeRaceAndLateOutput() {
  for (uint32_t iteration = 0; iteration < 200u; iteration++) {
    ProofFixture f;
    CpuSkinMtControllerTerminalLedger ledger;
    CpuSkinMtControllerJob job(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(job, f));
    const auto output = f.output();

    std::atomic<bool> start{false};
    CpuSkinMtControllerResult readyResult =
        CpuSkinMtControllerResult::InvalidTransition;
    CpuSkinMtControllerKernelDecision decision;
    std::thread ready([&] {
      while (!start.load(std::memory_order_acquire)) { }
      readyResult = job.publishProducerReady(kGeneration, output);
    });
    std::thread owner([&] {
      while (!start.load(std::memory_order_acquire)) { }
      decision = job.trySelectKernelRoute(kGeneration, kSafeMxcsr);
    });
    start.store(true, std::memory_order_release);
    ready.join();
    owner.join();

    CHECK(decision.result == CpuSkinMtControllerResult::Applied);
    CHECK(decision.selectedNow);
    CHECK(decision.route == CpuSkinMtControllerKernelRoute::Copy ||
          decision.route == CpuSkinMtControllerKernelRoute::Native);
    CHECK(readyResult == CpuSkinMtControllerResult::Applied ||
          readyResult == CpuSkinMtControllerResult::LateProducerReady);
    if (decision.route == CpuSkinMtControllerKernelRoute::Copy) {
      CHECK(readyResult == CpuSkinMtControllerResult::Applied);
      CopyContext context;
      CHECK(job.copyOutputUnderLease(kGeneration, CopyToMappedDestination,
                                     &context) ==
            CpuSkinMtControllerResult::Applied);
    } else {
      CHECK(!job.producerReady());
      CHECK(job.publishProducerReady(kGeneration, output) ==
            CpuSkinMtControllerResult::LateProducerReady);
      CHECK(job.completeOriginalNormal(kGeneration) ==
            CpuSkinMtControllerResult::Applied);
    }
    CHECK(job.noteUnlock(kGeneration, f.unlock) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(job.terminal() == (decision.route ==
                                      CpuSkinMtControllerKernelRoute::Copy
        ? CpuSkinMtControllerTerminal::CopiedNormal
        : CpuSkinMtControllerTerminal::OriginalNormal));
    CHECK(ledger.snapshot().closureHolds());
  }
  return true;
}

bool TestKeyMismatchResetAndDestructorClosure() {
  ProofFixture f;
  CpuSkinMtControllerTerminalLedger ledger;
  {
    CpuSkinMtControllerJob mismatch(f.frozen, kGeneration, ledger);
    CHECK(PrepareAwaiting(mismatch, f));
    auto wrongUnlock = f.unlock;
    wrongUnlock.mapAllocationGeneration++;
    CHECK(mismatch.noteUnlock(kGeneration, wrongUnlock) ==
          CpuSkinMtControllerResult::KeyMismatch);
    CHECK(!mismatch.unlockObserved());
    CHECK(mismatch.cancelTemplateMismatch(kGeneration) ==
          CpuSkinMtControllerResult::Applied);

    CpuSkinMtControllerJob reset(f.frozen, kGeneration, ledger);
    CHECK(reset.submit(kGeneration) == CpuSkinMtControllerResult::Applied);
    CHECK(reset.cancelForReset(kGeneration, kGeneration + 1u) ==
          CpuSkinMtControllerResult::Applied);
    CHECK(reset.activeGeneration() == kGeneration + 1u);
    CHECK(reset.submit(kGeneration) ==
          CpuSkinMtControllerResult::StaleGeneration);

    CpuSkinMtControllerJob abandoned(f.frozen, kGeneration, ledger);
    CHECK(abandoned.submit(kGeneration) ==
          CpuSkinMtControllerResult::Applied);
  }
  const auto snapshot = ledger.snapshot();
  CHECK(snapshot.templateMismatch == 1u);
  CHECK(snapshot.resetCancelled == 1u);
  CHECK(snapshot.outerCancelled == 1u);
  CHECK(snapshot.keyMismatchRejects == 1u);
  CHECK(snapshot.staleGenerationRejects == 1u);
  CHECK(snapshot.liveJobs == 0u);
  CHECK(snapshot.closureHolds());
  return true;
}

bool TestEligibilityBoundariesAndShape() {
  ProofFixture minimum;
  CHECK(CpuSkinMtValidateFormat2Eligibility(
      minimum.eligibility()).eligible);
  ProofFixture maximum(kCpuSkinMtFormat2MaxVertices);
  CHECK(CpuSkinMtValidateFormat2Eligibility(
      maximum.eligibility()).eligible);

  ProofFixture below(kCpuSkinMtFormat2MinVertices - 1u);
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(
                       below.eligibility()),
                   CpuSkinMtFormat2EligibilityVertexRange));
  ProofFixture above(kCpuSkinMtFormat2MaxVertices + 1u);
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(
                       above.eligibility()),
                   CpuSkinMtFormat2EligibilityVertexRange));

  auto input = minimum.eligibility();
  input.path = CpuSkinMtControllerPath::Special;
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(input),
                   CpuSkinMtFormat2EligibilityNotCommon));
  input = minimum.eligibility();
  input.opaque = false;
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(input),
                   CpuSkinMtFormat2EligibilityNotOpaque));
  input = minimum.eligibility();
  input.specialDispatch = true;
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(input),
                   CpuSkinMtFormat2EligibilitySpecialDispatch));
  input = minimum.eligibility();
  input.outputStride++;
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(input),
                   CpuSkinMtFormat2EligibilityOutputStride));
  input = minimum.eligibility();
  input.positionFloatCount--;
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(input),
                   CpuSkinMtFormat2EligibilityPositionCount));
  input = minimum.eligibility();
  input.paletteSealedToken = 0u;
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(input),
                   CpuSkinMtFormat2EligibilityPaletteUnsealed));
  return true;
}

bool TestEligibilityFiniteGroupAndMxcsr() {
  ProofFixture f;
  f.groups[0] = static_cast<uint8_t>(f.frozen.palette.groupCount);
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(f.eligibility()),
                   CpuSkinMtFormat2EligibilityGroupSlotRange));
  f.groups[0] = 0u;
  f.positions[0] = std::numeric_limits<float>::quiet_NaN();
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(f.eligibility()),
                   CpuSkinMtFormat2EligibilityNonFinitePosition));
  f.positions[0] = 1.0f;
  f.normals[0] = std::numeric_limits<float>::infinity();
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(f.eligibility()),
                   CpuSkinMtFormat2EligibilityNonFiniteNormal));
  f.normals[0] = 0.5f;
  f.palette[0] = -std::numeric_limits<float>::infinity();
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(f.eligibility()),
                   CpuSkinMtFormat2EligibilityNonFinitePalette));
  f.palette[0] = 1.0f;

  auto input = f.eligibility();
  input.frozenMxcsr = 0u;
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(input),
                   CpuSkinMtFormat2EligibilityUnsafeMxcsr));
  input = f.eligibility();
  input.currentMxcsr ^= 0x2000u;
  CHECK(HasFailure(CpuSkinMtValidateFormat2Eligibility(input),
                   CpuSkinMtFormat2EligibilityMxcsrMismatch));
  input = f.eligibility();
  input.frozenMxcsr |= 0x01u;
  input.currentMxcsr |= 0x21u;
  CHECK(CpuSkinMtValidateFormat2Eligibility(input).eligible);
  CHECK(CpuSkinMtEligibilityMatchesExactKey(f.exactKey(), input));
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
      {"exact/output authority", TestExactKeyAndOutputAuthority},
      {"owned output lifetime", TestOwnedOutputLifetimeAndUnlockCommit},
      {"output/MXCSR mismatch", TestOutputMismatchAndCommitMxcsr},
      {"kernel lock contention",
       TestKernelWindowContentionFallsBackImmediately},
      {"copy fault finalization", TestCopyFaultsAreUnlockFinalized},
      {"native unlock pending", TestNativeCompletionRequiresUnlock},
      {"native fault classification", TestNativeFailureClassification},
      {"native body/unlock race", TestNativeBodyVersusUnlockRace},
      {"unlock rejects copy", TestUnlockBeforeCopyRejectsAllWrites},
      {"cancel rejects copy", TestCancelBeforeCopyRejectsAllWrites},
      {"copy/unlock race", TestGuardedCopyLinearizesBeforeUnlock},
      {"copy/cancel race", TestGuardedCopyLinearizesBeforeResetOrCancel},
      {"claim/native race", TestClaimVersusNativeRace},
      {"producer/native race", TestProducerVersusNativeRaceAndLateOutput},
      {"key/reset/destructor", TestKeyMismatchResetAndDestructorClosure},
      {"eligibility shape", TestEligibilityBoundariesAndShape},
      {"eligibility finite/MXCSR", TestEligibilityFiniteGroupAndMxcsr},
  };

  for (const auto& test : tests) {
    if (!test.run()) {
      std::cerr << "FAIL: " << test.name << '\n';
      return 1;
    }
    std::cout << "PASS: " << test.name << '\n';
  }

  std::cout << "PASS: CPU-MT controller Phase 1 value contract\n";
  return 0;
}
