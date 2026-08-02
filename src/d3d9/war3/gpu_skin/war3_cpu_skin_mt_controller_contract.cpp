#include "war3_cpu_skin_mt_controller_contract.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace dxvk::war3::gpu_skin {
namespace {

template<typename Atomic>
uint64_t LoadRelaxed(const Atomic& value) noexcept {
  return value.load(std::memory_order_relaxed);
}

template<typename Atomic>
void Increment(Atomic& value) noexcept {
  value.fetch_add(1u, std::memory_order_relaxed);
}

template<typename Atomic>
void Decrement(Atomic& value) noexcept {
  value.fetch_sub(1u, std::memory_order_relaxed);
}

bool IsFiniteSpan(const float* values, size_t count) noexcept {
  if (values == nullptr)
    return false;
  for (size_t i = 0; i < count; i++) {
    if (!std::isfinite(values[i]))
      return false;
  }
  return true;
}

uint64_t HashBytes(const uint8_t* bytes, size_t size) noexcept {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

bool ByteCountMatches(size_t elementCount,
                      size_t elementSize,
                      uint32_t expectedBytes) noexcept {
  if (elementCount > std::numeric_limits<size_t>::max() / elementSize)
    return false;
  return elementCount * elementSize == expectedBytes;
}

uint32_t ExpectedOutputBytes(
    const CpuSkinMtControllerFrozenKey& frozen) noexcept {
  const uint64_t expected = uint64_t(frozen.source.vertexCount) *
      kCpuSkinMtFormat2OutputStride;
  if (expected == 0u || expected > std::numeric_limits<uint32_t>::max())
    return 0u;
  return static_cast<uint32_t>(expected);
}

bool MxcsrStatusDeltaConsistent(
    const CpuSkinMtControllerMxcsrStatusDelta& delta) noexcept {
  return delta.valid &&
      CpuSkinMtHasSafeMxcsrControl(delta.normalizedControl) &&
      (delta.statusBefore & ~kCpuSkinMtMxcsrStatusMask) == 0u &&
      (delta.statusAfter & ~kCpuSkinMtMxcsrStatusMask) == 0u &&
      delta.raisedStatus == (delta.statusAfter & ~delta.statusBefore) &&
      delta.clearedStatus == (delta.statusBefore & ~delta.statusAfter);
}

bool ProducerResultProofStructurallyComplete(
    const CpuSkinMtControllerProducerResultProof& proof) noexcept {
  return CpuSkinMtControllerFrozenKeyValid(proof.frozenKey) &&
      MxcsrStatusDeltaConsistent(proof.mxcsrStatusDelta) &&
      proof.resultSerial != 0u &&
      proof.producerGeneration ==
          proof.frozenKey.owner.producerGeneration &&
      proof.ownedBytesIdentity != 0u && proof.outputByteSize != 0u &&
      proof.outputByteSize == ExpectedOutputBytes(proof.frozenKey) &&
      proof.normalizedMxcsr == proof.mxcsrStatusDelta.normalizedControl &&
      proof.normalizedMxcsr == proof.frozenKey.palette.floatingPointControl &&
      proof.proofVersion == kCpuSkinMtProducerResultProofVersion;
}

}  // namespace

bool operator==(const CpuSkinMtControllerOwnerSessionProof& lhs,
                const CpuSkinMtControllerOwnerSessionProof& rhs) noexcept {
  return lhs.controllerInstanceGeneration == rhs.controllerInstanceGeneration &&
      lhs.producerGeneration == rhs.producerGeneration &&
      lhs.mapEpoch == rhs.mapEpoch && lhs.deviceEpoch == rhs.deviceEpoch &&
      lhs.bridgeResetGeneration == rhs.bridgeResetGeneration &&
      lhs.renderThreadId == rhs.renderThreadId;
}

bool operator==(const CpuSkinMtControllerFlushBindingProof& lhs,
                const CpuSkinMtControllerFlushBindingProof& rhs) noexcept {
  return lhs.frameTag == rhs.frameTag &&
      lhs.flushEpoch == rhs.flushEpoch && lhs.batchId == rhs.batchId &&
      lhs.renderablePart == rhs.renderablePart &&
      lhs.geosetData == rhs.geosetData &&
      lhs.candidateToken == rhs.candidateToken &&
      lhs.flushCandidateOrdinal == rhs.flushCandidateOrdinal &&
      lhs.layerIndex == rhs.layerIndex && lhs.path == rhs.path &&
      lhs.opaque == rhs.opaque;
}

bool operator==(const CpuSkinMtControllerImmutableSourceProof& lhs,
                const CpuSkinMtControllerImmutableSourceProof& rhs) noexcept {
  return lhs.snapshotIdentity == rhs.snapshotIdentity &&
      lhs.contentHash == rhs.contentHash &&
      lhs.sealedContentToken == rhs.sealedContentToken &&
      lhs.sourceGeneration == rhs.sourceGeneration &&
      lhs.layoutGeneration == rhs.layoutGeneration &&
      lhs.vertexCount == rhs.vertexCount &&
      lhs.matrixGroupCount == rhs.matrixGroupCount &&
      lhs.uvLayerCount == rhs.uvLayerCount &&
      lhs.maxGroupSlot == rhs.maxGroupSlot &&
      lhs.positions == rhs.positions && lhs.normals == rhs.normals &&
      lhs.groupSlots == rhs.groupSlots && lhs.uv0 == rhs.uv0 &&
      lhs.extra == rhs.extra && lhs.uv1 == rhs.uv1 &&
      lhs.positionStride == rhs.positionStride &&
      lhs.normalStride == rhs.normalStride &&
      lhs.groupSlotStride == rhs.groupSlotStride &&
      lhs.uv0Stride == rhs.uv0Stride &&
      lhs.extraStride == rhs.extraStride &&
      lhs.uv1Stride == rhs.uv1Stride &&
      lhs.positionByteCount == rhs.positionByteCount &&
      lhs.normalByteCount == rhs.normalByteCount &&
      lhs.groupSlotByteCount == rhs.groupSlotByteCount &&
      lhs.uv0ByteCount == rhs.uv0ByteCount;
}

bool operator==(const CpuSkinMtControllerPaletteProof& lhs,
                const CpuSkinMtControllerPaletteProof& rhs) noexcept {
  return lhs.palettePointer == rhs.palettePointer &&
      lhs.immutableBlobIdentity == rhs.immutableBlobIdentity &&
      lhs.paletteGeneration == rhs.paletteGeneration &&
      lhs.paletteWriteSerial == rhs.paletteWriteSerial &&
      lhs.paletteFrameTag == rhs.paletteFrameTag &&
      lhs.contentHash == rhs.contentHash &&
      lhs.sealedContentToken == rhs.sealedContentToken &&
      lhs.groupCount == rhs.groupCount && lhs.byteCount == rhs.byteCount &&
      lhs.floatingPointControl == rhs.floatingPointControl;
}

bool operator==(const CpuSkinMtControllerOuterBindingProof& lhs,
                const CpuSkinMtControllerOuterBindingProof& rhs) noexcept {
  return lhs.controllerInstanceGeneration ==
          rhs.controllerInstanceGeneration &&
      lhs.producerGeneration == rhs.producerGeneration &&
      lhs.mapEpoch == rhs.mapEpoch && lhs.deviceEpoch == rhs.deviceEpoch &&
      lhs.bridgeResetGeneration == rhs.bridgeResetGeneration &&
      lhs.frameTag == rhs.frameTag && lhs.flushEpoch == rhs.flushEpoch &&
      lhs.batchId == rhs.batchId &&
      lhs.sourceGeneration == rhs.sourceGeneration &&
      lhs.paletteGeneration == rhs.paletteGeneration &&
      lhs.sourceSealedContentToken == rhs.sourceSealedContentToken &&
      lhs.paletteSealedContentToken == rhs.paletteSealedContentToken &&
      lhs.dispatchEpoch == rhs.dispatchEpoch &&
      lhs.uploadEpoch == rhs.uploadEpoch &&
      lhs.gxDeviceD3d == rhs.gxDeviceD3d &&
      lhs.renderablePart == rhs.renderablePart &&
      lhs.geosetData == rhs.geosetData && lhs.positions == rhs.positions &&
      lhs.normals == rhs.normals && lhs.groupSlots == rhs.groupSlots &&
      lhs.uv0 == rhs.uv0 && lhs.extra == rhs.extra && lhs.uv1 == rhs.uv1 &&
      lhs.palettePointer == rhs.palettePointer &&
      lhs.uploadOrdinal == rhs.uploadOrdinal &&
      lhs.candidateToken == rhs.candidateToken &&
      lhs.flushCandidateOrdinal == rhs.flushCandidateOrdinal &&
      lhs.layerIndex == rhs.layerIndex &&
      lhs.vertexCount == rhs.vertexCount &&
      lhs.positionStride == rhs.positionStride &&
      lhs.normalStride == rhs.normalStride &&
      lhs.groupSlotStride == rhs.groupSlotStride &&
      lhs.uv0Stride == rhs.uv0Stride &&
      lhs.extraStride == rhs.extraStride &&
      lhs.uv1Stride == rhs.uv1Stride &&
      lhs.paletteGroupCount == rhs.paletteGroupCount &&
      lhs.skinMode == rhs.skinMode &&
      lhs.outputFormat == rhs.outputFormat && lhs.fvf == rhs.fvf &&
      lhs.outputStride == rhs.outputStride;
}

bool operator==(const CpuSkinMtControllerDestinationProof& lhs,
                const CpuSkinMtControllerDestinationProof& rhs) noexcept {
  return lhs.commonResource == rhs.commonResource &&
      lhs.comVertexBuffer == rhs.comVertexBuffer &&
      lhs.nativeD3DDevice == rhs.nativeD3DDevice &&
      lhs.mappedAllocation == rhs.mappedAllocation &&
      lhs.mappedAllocationBase == rhs.mappedAllocationBase &&
      lhs.mappedPointer == rhs.mappedPointer &&
      lhs.resourceGeneration == rhs.resourceGeneration &&
      lhs.realStorageGeneration == rhs.realStorageGeneration &&
      lhs.mappingStorageGeneration == rhs.mappingStorageGeneration &&
      lhs.mapAllocationGeneration == rhs.mapAllocationGeneration &&
      lhs.lockSerial == rhs.lockSerial &&
      lhs.mappedAllocationByteSize == rhs.mappedAllocationByteSize &&
      lhs.dispatchEpoch == rhs.dispatchEpoch &&
      lhs.uploadEpoch == rhs.uploadEpoch && lhs.offset == rhs.offset &&
      lhs.size == rhs.size && lhs.effectiveFlags == rhs.effectiveFlags &&
      lhs.lockDepth == rhs.lockDepth &&
      lhs.uploadOrdinal == rhs.uploadOrdinal &&
      lhs.outputFormat == rhs.outputFormat && lhs.fvf == rhs.fvf &&
      lhs.lockSucceeded == rhs.lockSucceeded &&
      lhs.lockActive == rhs.lockActive;
}

bool operator==(const CpuSkinMtControllerUnlockProof& lhs,
                const CpuSkinMtControllerUnlockProof& rhs) noexcept {
  return lhs.commonResource == rhs.commonResource &&
      lhs.comVertexBuffer == rhs.comVertexBuffer &&
      lhs.mappedAllocation == rhs.mappedAllocation &&
      lhs.mappedPointer == rhs.mappedPointer &&
      lhs.resourceGeneration == rhs.resourceGeneration &&
      lhs.mapAllocationGeneration == rhs.mapAllocationGeneration &&
      lhs.lockSerial == rhs.lockSerial &&
      lhs.unlockSerial == rhs.unlockSerial && lhs.result == rhs.result;
}

bool operator==(const CpuSkinMtControllerFrozenKey& lhs,
                const CpuSkinMtControllerFrozenKey& rhs) noexcept {
  return lhs.owner == rhs.owner && lhs.flush == rhs.flush &&
      lhs.source == rhs.source && lhs.palette == rhs.palette;
}

bool operator==(const CpuSkinMtControllerMxcsrStatusDelta& lhs,
                const CpuSkinMtControllerMxcsrStatusDelta& rhs) noexcept {
  return lhs.normalizedControl == rhs.normalizedControl &&
      lhs.statusBefore == rhs.statusBefore &&
      lhs.statusAfter == rhs.statusAfter &&
      lhs.raisedStatus == rhs.raisedStatus &&
      lhs.clearedStatus == rhs.clearedStatus && lhs.valid == rhs.valid;
}

bool operator==(const CpuSkinMtControllerProducerResultProof& lhs,
                const CpuSkinMtControllerProducerResultProof& rhs) noexcept {
  return lhs.frozenKey == rhs.frozenKey &&
      lhs.mxcsrStatusDelta == rhs.mxcsrStatusDelta &&
      lhs.resultSerial == rhs.resultSerial &&
      lhs.producerGeneration == rhs.producerGeneration &&
      lhs.outputContentHash == rhs.outputContentHash &&
      lhs.ownedBytesIdentity == rhs.ownedBytesIdentity &&
      lhs.outputByteSize == rhs.outputByteSize &&
      lhs.normalizedMxcsr == rhs.normalizedMxcsr &&
      lhs.proofVersion == rhs.proofVersion;
}

bool operator==(const CpuSkinMtControllerRenderCommitEnvelopeProof& lhs,
                const CpuSkinMtControllerRenderCommitEnvelopeProof& rhs)
    noexcept {
  return lhs.frozenKey == rhs.frozenKey && lhs.outer == rhs.outer &&
      lhs.destination == rhs.destination &&
      lhs.producerResult == rhs.producerResult &&
      lhs.commitSerial == rhs.commitSerial &&
      lhs.normalizedOwnerMxcsr == rhs.normalizedOwnerMxcsr &&
      lhs.proofVersion == rhs.proofVersion;
}

bool operator==(const CpuSkinMtControllerBodyCompletion& lhs,
                const CpuSkinMtControllerBodyCompletion& rhs) noexcept {
  return lhs.succeeded == rhs.succeeded &&
      lhs.mxcsrBefore == rhs.mxcsrBefore &&
      lhs.mxcsrAfter == rhs.mxcsrAfter;
}

uint32_t CpuSkinMtNormalizeMxcsrControl(uint32_t mxcsr) noexcept {
  return mxcsr & kCpuSkinMtMxcsrControlMask;
}

bool CpuSkinMtHasSafeMxcsrControl(uint32_t mxcsr) noexcept {
  return (mxcsr & ~kCpuSkinMtMxcsrControlMask) == 0u &&
      (mxcsr & kCpuSkinMtMxcsrExceptionMask) ==
          kCpuSkinMtMxcsrExceptionMask;
}

CpuSkinMtControllerMxcsrStatusDelta CpuSkinMtCaptureMxcsrStatusDelta(
    uint32_t before,
    uint32_t after) noexcept {
  CpuSkinMtControllerMxcsrStatusDelta result;
  const uint32_t allowed =
      kCpuSkinMtMxcsrControlMask | kCpuSkinMtMxcsrStatusMask;
  const uint32_t beforeControl = CpuSkinMtNormalizeMxcsrControl(before);
  const uint32_t afterControl = CpuSkinMtNormalizeMxcsrControl(after);
  result.normalizedControl = beforeControl;
  result.statusBefore = before & kCpuSkinMtMxcsrStatusMask;
  result.statusAfter = after & kCpuSkinMtMxcsrStatusMask;
  result.raisedStatus = result.statusAfter & ~result.statusBefore;
  result.clearedStatus = result.statusBefore & ~result.statusAfter;
  result.valid = (before & ~allowed) == 0u && (after & ~allowed) == 0u &&
      beforeControl == afterControl &&
      CpuSkinMtHasSafeMxcsrControl(beforeControl);
  return result;
}

bool CpuSkinMtControllerFrozenKeyValid(
    const CpuSkinMtControllerFrozenKey& key) noexcept {
  const auto& owner = key.owner;
  const auto& flush = key.flush;
  const auto& source = key.source;
  const auto& palette = key.palette;

  if (owner.controllerInstanceGeneration == 0u ||
      owner.producerGeneration == 0u || owner.mapEpoch == 0u ||
      owner.deviceEpoch == 0u || owner.bridgeResetGeneration == 0u ||
      owner.renderThreadId == 0u)
    return false;

  if (flush.frameTag == 0u || flush.flushEpoch == 0u ||
      flush.batchId == 0u || flush.renderablePart == 0u ||
      flush.geosetData == 0u || flush.candidateToken == 0u ||
      flush.path != CpuSkinMtControllerPath::Common || !flush.opaque)
    return false;

  if (source.snapshotIdentity == 0u || source.contentHash == 0u ||
      source.sealedContentToken == 0u || source.sourceGeneration == 0u ||
      source.layoutGeneration == 0u ||
      source.vertexCount < kCpuSkinMtFormat2MinVertices ||
      source.vertexCount > kCpuSkinMtFormat2MaxVertices ||
      source.matrixGroupCount == 0u ||
      source.matrixGroupCount > kCpuSkinMtFormat2MaxPaletteGroups ||
      source.uvLayerCount != 1u ||
      source.maxGroupSlot >= source.matrixGroupCount ||
      source.positions == 0u || source.normals == 0u ||
      source.groupSlots == 0u || source.uv0 == 0u ||
      source.extra != 0u || source.uv1 != 0u ||
      source.positionStride != 12u || source.normalStride != 12u ||
      source.groupSlotStride != 1u || source.uv0Stride != 8u ||
      source.extraStride != 0u || source.uv1Stride != 0u ||
      source.positionByteCount != source.vertexCount * 12u ||
      source.normalByteCount != source.vertexCount * 12u ||
      source.groupSlotByteCount != source.vertexCount ||
      source.uv0ByteCount != source.vertexCount * 8u)
    return false;

  if (palette.palettePointer == 0u ||
      palette.immutableBlobIdentity == 0u ||
      palette.paletteGeneration == 0u ||
      palette.paletteWriteSerial == 0u || palette.paletteFrameTag == 0u ||
      palette.contentHash == 0u || palette.sealedContentToken == 0u ||
      palette.groupCount != source.matrixGroupCount ||
      palette.byteCount != palette.groupCount * kCpuSkinMtPaletteMatrixBytes ||
      palette.paletteFrameTag != flush.frameTag ||
      !CpuSkinMtHasSafeMxcsrControl(palette.floatingPointControl))
    return false;

  return ExpectedOutputBytes(key) != 0u;
}

bool CpuSkinMtControllerOuterMatchesFrozen(
    const CpuSkinMtControllerFrozenKey& frozen,
    const CpuSkinMtControllerOuterBindingProof& outer) noexcept {
  if (!CpuSkinMtControllerFrozenKeyValid(frozen))
    return false;
  return outer.controllerInstanceGeneration ==
          frozen.owner.controllerInstanceGeneration &&
      outer.producerGeneration == frozen.owner.producerGeneration &&
      outer.mapEpoch == frozen.owner.mapEpoch &&
      outer.deviceEpoch == frozen.owner.deviceEpoch &&
      outer.bridgeResetGeneration == frozen.owner.bridgeResetGeneration &&
      outer.frameTag == frozen.flush.frameTag &&
      outer.flushEpoch == frozen.flush.flushEpoch &&
      outer.batchId == frozen.flush.batchId &&
      outer.sourceGeneration == frozen.source.sourceGeneration &&
      outer.paletteGeneration == frozen.palette.paletteGeneration &&
      outer.sourceSealedContentToken == frozen.source.sealedContentToken &&
      outer.paletteSealedContentToken == frozen.palette.sealedContentToken &&
      outer.dispatchEpoch != 0u && outer.uploadEpoch != 0u &&
      outer.gxDeviceD3d != 0u &&
      outer.renderablePart == frozen.flush.renderablePart &&
      outer.geosetData == frozen.flush.geosetData &&
      outer.positions == frozen.source.positions &&
      outer.normals == frozen.source.normals &&
      outer.groupSlots == frozen.source.groupSlots &&
      outer.uv0 == frozen.source.uv0 && outer.extra == frozen.source.extra &&
      outer.uv1 == frozen.source.uv1 &&
      outer.palettePointer == frozen.palette.palettePointer &&
      outer.candidateToken == frozen.flush.candidateToken &&
      outer.flushCandidateOrdinal == frozen.flush.flushCandidateOrdinal &&
      outer.layerIndex == frozen.flush.layerIndex &&
      outer.vertexCount == frozen.source.vertexCount &&
      outer.positionStride == frozen.source.positionStride &&
      outer.normalStride == frozen.source.normalStride &&
      outer.groupSlotStride == frozen.source.groupSlotStride &&
      outer.uv0Stride == frozen.source.uv0Stride &&
      outer.extraStride == frozen.source.extraStride &&
      outer.uv1Stride == frozen.source.uv1Stride &&
      outer.paletteGroupCount == frozen.palette.groupCount &&
      outer.skinMode == kCpuSkinMtFormat2SkinMode &&
      outer.outputFormat == kCpuSkinMtFormat2OutputFormat &&
      outer.fvf == kCpuSkinMtFormat2Fvf &&
      outer.outputStride == kCpuSkinMtFormat2OutputStride;
}

bool CpuSkinMtControllerDestinationValid(
    const CpuSkinMtControllerFrozenKey& frozen,
    const CpuSkinMtControllerOuterBindingProof& outer,
    const CpuSkinMtControllerDestinationProof& destination) noexcept {
  if (!CpuSkinMtControllerOuterMatchesFrozen(frozen, outer))
    return false;
  const uint32_t expectedSize = ExpectedOutputBytes(frozen);
  if (expectedSize == 0u || destination.commonResource == 0u ||
      destination.comVertexBuffer == 0u ||
      destination.nativeD3DDevice == 0u ||
      destination.mappedAllocation == 0u ||
      destination.mappedAllocationBase == 0u ||
      destination.mappedPointer == 0u ||
      destination.resourceGeneration == 0u ||
      destination.realStorageGeneration == 0u ||
      destination.mappingStorageGeneration == 0u ||
      destination.mapAllocationGeneration == 0u ||
      destination.lockSerial == 0u ||
      destination.mappedAllocationByteSize == 0u ||
      destination.dispatchEpoch != outer.dispatchEpoch ||
      destination.uploadEpoch != outer.uploadEpoch ||
      destination.uploadOrdinal != outer.uploadOrdinal ||
      !destination.lockSucceeded || !destination.lockActive ||
      destination.lockDepth != 1u ||
      destination.outputFormat != kCpuSkinMtFormat2OutputFormat ||
      destination.fvf != kCpuSkinMtFormat2Fvf ||
      destination.size != expectedSize ||
      destination.mappedPointer < destination.mappedAllocationBase)
    return false;

  const uintptr_t pointerOffset =
      destination.mappedPointer - destination.mappedAllocationBase;
  const uint64_t writeEnd =
      uint64_t(destination.offset) + uint64_t(destination.size);
  return pointerOffset == destination.offset &&
      writeEnd <= destination.mappedAllocationByteSize;
}

bool CpuSkinMtControllerUnlockMatchesDestination(
    const CpuSkinMtControllerDestinationProof& destination,
    const CpuSkinMtControllerUnlockProof& unlock) noexcept {
  return unlock.unlockSerial != 0u &&
      unlock.commonResource == destination.commonResource &&
      unlock.comVertexBuffer == destination.comVertexBuffer &&
      unlock.mappedAllocation == destination.mappedAllocation &&
      unlock.mappedPointer == destination.mappedPointer &&
      unlock.resourceGeneration == destination.resourceGeneration &&
      unlock.mapAllocationGeneration == destination.mapAllocationGeneration &&
      unlock.lockSerial == destination.lockSerial;
}

CpuSkinMtFormat2EligibilityResult CpuSkinMtValidateFormat2Eligibility(
    const CpuSkinMtFormat2EligibilityInput& input) noexcept {
  uint64_t failures = CpuSkinMtFormat2EligibilityNone;
  const auto fail = [&failures](CpuSkinMtFormat2EligibilityFailure bit) {
    failures |= static_cast<uint64_t>(bit);
  };

  if (input.path != CpuSkinMtControllerPath::Common)
    fail(CpuSkinMtFormat2EligibilityNotCommon);
  if (!input.opaque)
    fail(CpuSkinMtFormat2EligibilityNotOpaque);
  if (input.specialDispatch)
    fail(CpuSkinMtFormat2EligibilitySpecialDispatch);
  if (input.skinMode != kCpuSkinMtFormat2SkinMode)
    fail(CpuSkinMtFormat2EligibilitySkinMode);
  if (input.outputFormat != kCpuSkinMtFormat2OutputFormat)
    fail(CpuSkinMtFormat2EligibilityOutputFormat);
  if (input.fvf != kCpuSkinMtFormat2Fvf)
    fail(CpuSkinMtFormat2EligibilityFvf);
  if (input.outputStride != kCpuSkinMtFormat2OutputStride)
    fail(CpuSkinMtFormat2EligibilityOutputStride);
  if (input.vertexCount < kCpuSkinMtFormat2MinVertices ||
      input.vertexCount > kCpuSkinMtFormat2MaxVertices)
    fail(CpuSkinMtFormat2EligibilityVertexRange);
  if (input.positions == nullptr || input.normals == nullptr ||
      input.groupSlots == nullptr || input.uv0 == nullptr ||
      input.palette == nullptr)
    fail(CpuSkinMtFormat2EligibilityMissingStream);
  if (input.extra != 0u)
    fail(CpuSkinMtFormat2EligibilityUnexpectedExtra);
  if (input.uv1 != 0u)
    fail(CpuSkinMtFormat2EligibilityUnexpectedUv1);
  if (input.positionStride != 12u)
    fail(CpuSkinMtFormat2EligibilityPositionStride);
  if (input.normalStride != 12u)
    fail(CpuSkinMtFormat2EligibilityNormalStride);
  if (input.groupSlotStride != 1u)
    fail(CpuSkinMtFormat2EligibilityGroupStride);
  if (input.uv0Stride != 8u)
    fail(CpuSkinMtFormat2EligibilityUv0Stride);
  if (input.uvLayerCount != 1u)
    fail(CpuSkinMtFormat2EligibilityUvLayerCount);
  if (input.paletteGroupCount == 0u ||
      input.paletteGroupCount > kCpuSkinMtFormat2MaxPaletteGroups)
    fail(CpuSkinMtFormat2EligibilityPaletteRange);

  const size_t vertexCount = input.vertexCount;
  const size_t expectedPositionFloats = vertexCount * 3u;
  const size_t expectedNormalFloats = vertexCount * 3u;
  const size_t expectedGroups = vertexCount;
  const size_t expectedUv0Floats = vertexCount * 2u;
  const size_t expectedPaletteFloats =
      size_t(input.paletteGroupCount) * 12u;

  if (input.positionFloatCount != expectedPositionFloats)
    fail(CpuSkinMtFormat2EligibilityPositionCount);
  if (input.normalFloatCount != expectedNormalFloats)
    fail(CpuSkinMtFormat2EligibilityNormalCount);
  if (input.groupSlotCount != expectedGroups)
    fail(CpuSkinMtFormat2EligibilityGroupCount);
  if (input.uv0FloatCount != expectedUv0Floats)
    fail(CpuSkinMtFormat2EligibilityUv0Count);
  if (input.paletteFloatCount != expectedPaletteFloats)
    fail(CpuSkinMtFormat2EligibilityPaletteCount);

  if (input.groupSlots != nullptr &&
      input.groupSlotCount == expectedGroups &&
      input.paletteGroupCount <= kCpuSkinMtFormat2MaxPaletteGroups) {
    for (size_t i = 0; i < input.groupSlotCount; i++) {
      if (input.groupSlots[i] >= input.paletteGroupCount) {
        fail(CpuSkinMtFormat2EligibilityGroupSlotRange);
        break;
      }
    }
  }
  if (input.positions != nullptr &&
      input.positionFloatCount == expectedPositionFloats &&
      !IsFiniteSpan(input.positions, input.positionFloatCount))
    fail(CpuSkinMtFormat2EligibilityNonFinitePosition);
  if (input.normals != nullptr &&
      input.normalFloatCount == expectedNormalFloats &&
      !IsFiniteSpan(input.normals, input.normalFloatCount))
    fail(CpuSkinMtFormat2EligibilityNonFiniteNormal);
  if (input.palette != nullptr &&
      input.paletteFloatCount == expectedPaletteFloats &&
      !IsFiniteSpan(input.palette, input.paletteFloatCount))
    fail(CpuSkinMtFormat2EligibilityNonFinitePalette);
  if (input.immutableSourceSealedToken == 0u)
    fail(CpuSkinMtFormat2EligibilitySourceUnsealed);
  if (input.paletteSealedToken == 0u)
    fail(CpuSkinMtFormat2EligibilityPaletteUnsealed);

  const uint32_t frozenControl =
      CpuSkinMtNormalizeMxcsrControl(input.frozenMxcsr);
  const uint32_t currentControl =
      CpuSkinMtNormalizeMxcsrControl(input.currentMxcsr);
  if (!CpuSkinMtHasSafeMxcsrControl(frozenControl) ||
      !CpuSkinMtHasSafeMxcsrControl(currentControl))
    fail(CpuSkinMtFormat2EligibilityUnsafeMxcsr);
  if (frozenControl != currentControl)
    fail(CpuSkinMtFormat2EligibilityMxcsrMismatch);

  return CpuSkinMtFormat2EligibilityResult{
      failures, failures == CpuSkinMtFormat2EligibilityNone};
}

bool CpuSkinMtEligibilityMatchesFrozenKey(
    const CpuSkinMtControllerFrozenKey& key,
    const CpuSkinMtFormat2EligibilityInput& input) noexcept {
  const CpuSkinMtFormat2EligibilityResult eligibility =
      CpuSkinMtValidateFormat2Eligibility(input);
  if (!eligibility.eligible || !CpuSkinMtControllerFrozenKeyValid(key))
    return false;

  if (input.path != key.flush.path || input.opaque != key.flush.opaque ||
      input.skinMode != kCpuSkinMtFormat2SkinMode ||
      input.outputFormat != kCpuSkinMtFormat2OutputFormat ||
      input.fvf != kCpuSkinMtFormat2Fvf ||
      input.outputStride != kCpuSkinMtFormat2OutputStride ||
      input.vertexCount != key.source.vertexCount ||
      input.positionStride != key.source.positionStride ||
      input.normalStride != key.source.normalStride ||
      input.groupSlotStride != key.source.groupSlotStride ||
      input.uv0Stride != key.source.uv0Stride ||
      input.uvLayerCount != key.source.uvLayerCount ||
      input.paletteGroupCount != key.palette.groupCount ||
      input.extra != key.source.extra || input.uv1 != key.source.uv1 ||
      reinterpret_cast<uintptr_t>(input.positions) != key.source.positions ||
      reinterpret_cast<uintptr_t>(input.normals) != key.source.normals ||
      reinterpret_cast<uintptr_t>(input.groupSlots) !=
          key.source.groupSlots ||
      reinterpret_cast<uintptr_t>(input.uv0) != key.source.uv0 ||
      reinterpret_cast<uintptr_t>(input.palette) !=
          key.palette.palettePointer ||
      input.immutableSourceSealedToken != key.source.sealedContentToken ||
      input.paletteSealedToken != key.palette.sealedContentToken)
    return false;

  if (!ByteCountMatches(input.positionFloatCount, sizeof(float),
                        key.source.positionByteCount) ||
      !ByteCountMatches(input.normalFloatCount, sizeof(float),
                        key.source.normalByteCount) ||
      !ByteCountMatches(input.groupSlotCount, sizeof(uint8_t),
                        key.source.groupSlotByteCount) ||
      !ByteCountMatches(input.uv0FloatCount, sizeof(float),
                        key.source.uv0ByteCount) ||
      !ByteCountMatches(input.paletteFloatCount, sizeof(float),
                        key.palette.byteCount))
    return false;

  const uint32_t frozenControl =
      CpuSkinMtNormalizeMxcsrControl(input.frozenMxcsr);
  const uint32_t currentControl =
      CpuSkinMtNormalizeMxcsrControl(input.currentMxcsr);
  return frozenControl == key.palette.floatingPointControl &&
      currentControl == frozenControl;
}

std::shared_ptr<const CpuSkinMtControllerOwnedProducerResult>
CpuSkinMtControllerOwnedProducerResult::Create(
    const CpuSkinMtControllerFrozenKey& frozenKey,
    const CpuSkinMtFormat2EligibilityInput& eligibility,
    uint64_t resultSerial,
    std::vector<uint8_t> bytes,
    uint32_t producerMxcsrBefore,
    uint32_t producerMxcsrAfter) {
  const CpuSkinMtControllerMxcsrStatusDelta statusDelta =
      CpuSkinMtCaptureMxcsrStatusDelta(
          producerMxcsrBefore, producerMxcsrAfter);
  const uint32_t expectedBytes = ExpectedOutputBytes(frozenKey);
  if (resultSerial == 0u ||
      !CpuSkinMtEligibilityMatchesFrozenKey(frozenKey, eligibility) ||
      !statusDelta.valid ||
      statusDelta.normalizedControl !=
          frozenKey.palette.floatingPointControl ||
      bytes.empty() || bytes.size() > std::numeric_limits<uint32_t>::max() ||
      bytes.size() != expectedBytes)
    return {};

  return std::shared_ptr<const CpuSkinMtControllerOwnedProducerResult>(
      new CpuSkinMtControllerOwnedProducerResult(
          frozenKey, statusDelta, resultSerial, std::move(bytes)));
}

CpuSkinMtControllerOwnedProducerResult::
CpuSkinMtControllerOwnedProducerResult(
    const CpuSkinMtControllerFrozenKey& frozenKey,
    const CpuSkinMtControllerMxcsrStatusDelta& statusDelta,
    uint64_t resultSerial,
    std::vector<uint8_t> bytes) noexcept
    : m_bytes(std::move(bytes)) {
  m_proof.frozenKey = frozenKey;
  m_proof.mxcsrStatusDelta = statusDelta;
  m_proof.resultSerial = resultSerial;
  m_proof.producerGeneration = frozenKey.owner.producerGeneration;
  m_proof.outputContentHash = HashBytes(m_bytes.data(), m_bytes.size());
  m_proof.ownedBytesIdentity =
      reinterpret_cast<uintptr_t>(m_bytes.data());
  m_proof.outputByteSize = static_cast<uint32_t>(m_bytes.size());
  m_proof.normalizedMxcsr = statusDelta.normalizedControl;
  m_proof.proofVersion = kCpuSkinMtProducerResultProofVersion;
}

const CpuSkinMtControllerProducerResultProof&
CpuSkinMtControllerOwnedProducerResult::proof() const noexcept {
  return m_proof;
}

const uint8_t* CpuSkinMtControllerOwnedProducerResult::data() const noexcept {
  return m_bytes.data();
}

uint32_t CpuSkinMtControllerOwnedProducerResult::size() const noexcept {
  return static_cast<uint32_t>(m_bytes.size());
}

bool CpuSkinMtControllerOwnedProducerResult::proofComplete() const noexcept {
  return ProducerResultProofStructurallyComplete(m_proof) &&
      m_proof.ownedBytesIdentity ==
          reinterpret_cast<uintptr_t>(m_bytes.data()) &&
      m_proof.outputByteSize == m_bytes.size() &&
      m_proof.outputContentHash == HashBytes(m_bytes.data(), m_bytes.size());
}

bool CpuSkinMtControllerRenderCommitEnvelopeComplete(
    const CpuSkinMtControllerRenderCommitEnvelopeProof& proof) noexcept {
  return proof.commitSerial != 0u &&
      proof.commitSerial == proof.destination.lockSerial &&
      proof.proofVersion == kCpuSkinMtRenderCommitEnvelopeVersion &&
      CpuSkinMtControllerFrozenKeyValid(proof.frozenKey) &&
      CpuSkinMtControllerOuterMatchesFrozen(proof.frozenKey, proof.outer) &&
      CpuSkinMtControllerDestinationValid(
          proof.frozenKey, proof.outer, proof.destination) &&
      proof.producerResult.frozenKey == proof.frozenKey &&
      ProducerResultProofStructurallyComplete(proof.producerResult) &&
      proof.producerResult.outputByteSize == proof.destination.size &&
      proof.producerResult.normalizedMxcsr ==
          proof.normalizedOwnerMxcsr &&
      proof.normalizedOwnerMxcsr ==
          proof.frozenKey.palette.floatingPointControl;
}

CpuSkinMtControllerRenderCommitEnvelope
CpuSkinMtControllerRenderCommitEnvelope::MintAfterLock(
    const CpuSkinMtControllerFrozenKey& frozenKey,
    const CpuSkinMtControllerOuterBindingProof& outer,
    const CpuSkinMtControllerDestinationProof& destination,
    std::shared_ptr<const CpuSkinMtControllerOwnedProducerResult> result,
    uint64_t commitSerial,
    uint32_t ownerMxcsr) noexcept {
  CpuSkinMtControllerRenderCommitEnvelope envelope;
  const uint32_t normalizedOwnerMxcsr =
      CpuSkinMtNormalizeMxcsrControl(ownerMxcsr);
  if (!result || !result->proofComplete() || commitSerial == 0u ||
      result->proof().frozenKey != frozenKey ||
      !CpuSkinMtControllerDestinationValid(frozenKey, outer, destination) ||
      !CpuSkinMtHasSafeMxcsrControl(normalizedOwnerMxcsr) ||
      normalizedOwnerMxcsr != result->proof().normalizedMxcsr)
    return envelope;

  envelope.m_proof.frozenKey = frozenKey;
  envelope.m_proof.outer = outer;
  envelope.m_proof.destination = destination;
  envelope.m_proof.producerResult = result->proof();
  envelope.m_proof.commitSerial = commitSerial;
  envelope.m_proof.normalizedOwnerMxcsr = normalizedOwnerMxcsr;
  envelope.m_proof.proofVersion = kCpuSkinMtRenderCommitEnvelopeVersion;
  envelope.m_result = std::move(result);
  if (!CpuSkinMtControllerRenderCommitEnvelopeComplete(envelope.m_proof))
    return CpuSkinMtControllerRenderCommitEnvelope{};
  return envelope;
}

bool CpuSkinMtControllerRenderCommitEnvelope::valid() const noexcept {
  return m_result && m_result->proofComplete() &&
      m_result->proof() == m_proof.producerResult &&
      CpuSkinMtControllerRenderCommitEnvelopeComplete(m_proof);
}

const CpuSkinMtControllerRenderCommitEnvelopeProof&
CpuSkinMtControllerRenderCommitEnvelope::proof() const noexcept {
  return m_proof;
}

CpuSkinMtControllerNativeBodyLease::CpuSkinMtControllerNativeBodyLease(
    uint64_t controllerInstanceGeneration,
    uint64_t producerGeneration,
    uint64_t activeGeneration,
    uint64_t frameTag,
    uint64_t batchId,
    uint64_t lockSerial) noexcept
    : m_controllerInstanceGeneration(controllerInstanceGeneration),
      m_producerGeneration(producerGeneration),
      m_activeGeneration(activeGeneration),
      m_frameTag(frameTag),
      m_batchId(batchId),
      m_lockSerial(lockSerial) {
}

bool CpuSkinMtControllerNativeBodyLease::valid() const noexcept {
  return m_controllerInstanceGeneration != 0u &&
      m_producerGeneration != 0u && m_activeGeneration != 0u &&
      m_frameTag != 0u && m_batchId != 0u && m_lockSerial != 0u;
}

uint64_t CpuSkinMtControllerNativeBodyLease::activeGeneration()
    const noexcept {
  return m_activeGeneration;
}

uint64_t CpuSkinMtControllerNativeBodyLease::lockSerial() const noexcept {
  return m_lockSerial;
}

bool operator==(const CpuSkinMtControllerNativeBodyLease& lhs,
                const CpuSkinMtControllerNativeBodyLease& rhs) noexcept {
  return lhs.m_controllerInstanceGeneration ==
          rhs.m_controllerInstanceGeneration &&
      lhs.m_producerGeneration == rhs.m_producerGeneration &&
      lhs.m_activeGeneration == rhs.m_activeGeneration &&
      lhs.m_frameTag == rhs.m_frameTag && lhs.m_batchId == rhs.m_batchId &&
      lhs.m_lockSerial == rhs.m_lockSerial;
}

uint64_t CpuSkinMtControllerTerminalLedgerSnapshot::terminalJobs()
    const noexcept {
  return copiedNormal + originalNormal + originalFault + copyFault +
      templateMismatch + resetCancelled + outerCancelled;
}

bool CpuSkinMtControllerTerminalLedgerSnapshot::closureHolds()
    const noexcept {
  return jobsCreated == liveJobs + terminalJobs() &&
      producerResultPublications == liveProducerResults +
          producerResultConsumptions + producerResultAbandoned &&
      producerResultClaims == liveProducerClaims +
          producerResultConsumptions + producerClaimAbandoned &&
      copySelections == liveCopyJobs + copiedNormal + copyFault +
          copyCancelled &&
      nativeSelections == liveNativeJobs + originalNormal + originalFault +
          nativeCancelled &&
      nativeBodyLeases == liveNativeBodyLeases + nativeBodiesCompleted +
          nativeBodyLeasesAbandoned &&
      outerSettlementsOpened == copySelections + nativeSelections &&
      outerSettlementsOpened == liveOuterSettlements +
          outerSettlementsCompleted;
}

CpuSkinMtControllerTerminalLedgerSnapshot
CpuSkinMtControllerTerminalLedger::snapshot() const noexcept {
  CpuSkinMtControllerTerminalLedgerSnapshot result;
#define LOAD_COUNTER(name) result.name = LoadRelaxed(m_counters.name)
  LOAD_COUNTER(jobsCreated);
  LOAD_COUNTER(liveJobs);
  LOAD_COUNTER(producerResultPublications);
  LOAD_COUNTER(liveProducerResults);
  LOAD_COUNTER(producerResultClaims);
  LOAD_COUNTER(liveProducerClaims);
  LOAD_COUNTER(producerResultConsumptions);
  LOAD_COUNTER(producerResultAbandoned);
  LOAD_COUNTER(producerClaimAbandoned);
  LOAD_COUNTER(copySelections);
  LOAD_COUNTER(nativeSelections);
  LOAD_COUNTER(liveCopyJobs);
  LOAD_COUNTER(liveNativeJobs);
  LOAD_COUNTER(nativeBodyLeases);
  LOAD_COUNTER(liveNativeBodyLeases);
  LOAD_COUNTER(nativeBodiesCompleted);
  LOAD_COUNTER(nativeBodyLeasesAbandoned);
  LOAD_COUNTER(outerSettlementsOpened);
  LOAD_COUNTER(liveOuterSettlements);
  LOAD_COUNTER(outerSettlementsCompleted);
  LOAD_COUNTER(guardedCopies);
  LOAD_COUNTER(guardedCopyBodyFaults);
  LOAD_COUNTER(mxcsrStatusDeltasRecorded);
  LOAD_COUNTER(mxcsrStatusDeltaMismatches);
  LOAD_COUNTER(successfulUnlocks);
  LOAD_COUNTER(failedUnlocks);
  LOAD_COUNTER(deferredResetCancellations);
  LOAD_COUNTER(deferredOuterCancellations);
  LOAD_COUNTER(deferredTemplateCancellations);
  LOAD_COUNTER(copiedNormal);
  LOAD_COUNTER(originalNormal);
  LOAD_COUNTER(originalFault);
  LOAD_COUNTER(copyFault);
  LOAD_COUNTER(templateMismatch);
  LOAD_COUNTER(resetCancelled);
  LOAD_COUNTER(outerCancelled);
  LOAD_COUNTER(copyCancelled);
  LOAD_COUNTER(nativeCancelled);
  LOAD_COUNTER(unselectedCancelled);
  LOAD_COUNTER(staleGenerationRejects);
  LOAD_COUNTER(invalidTransitionRejects);
  LOAD_COUNTER(keyMismatchRejects);
  LOAD_COUNTER(producerNotReadyRejects);
  LOAD_COUNTER(lateProducerReadyRejects);
  LOAD_COUNTER(copyAfterUnlockRejects);
  LOAD_COUNTER(doubleTerminalRejects);
  LOAD_COUNTER(mxcsrMismatchRejects);
  LOAD_COUNTER(invalidNativeLeaseRejects);
#undef LOAD_COUNTER
  return result;
}

CpuSkinMtControllerJob::CpuSkinMtControllerJob(
    const CpuSkinMtControllerFrozenKey& frozen,
    uint64_t activeGeneration,
    CpuSkinMtControllerTerminalLedger& ledger) noexcept
    : m_frozen(frozen),
      m_activeGeneration(activeGeneration),
      m_ledger(&ledger) {
  m_activeGenerationAtomic.store(activeGeneration,
                                 std::memory_order_relaxed);
  if (activeGeneration == 0u ||
      activeGeneration != frozen.owner.controllerInstanceGeneration ||
      !CpuSkinMtControllerFrozenKeyValid(frozen))
    return;

  m_state = CpuSkinMtControllerState::Frozen;
  Increment(m_ledger->m_counters.jobsCreated);
  Increment(m_ledger->m_counters.liveJobs);
}

CpuSkinMtControllerJob::~CpuSkinMtControllerJob() noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state != CpuSkinMtControllerState::Invalid && !isTerminalLocked())
    finishLocked(CpuSkinMtControllerState::OuterCancelled,
                 CpuSkinMtControllerTerminal::OuterCancelled);
}

bool CpuSkinMtControllerJob::isTerminalLocked() const noexcept {
  return m_terminal != CpuSkinMtControllerTerminal::None;
}

CpuSkinMtControllerNativeBodyLease
CpuSkinMtControllerJob::makeNativeBodyLease() const noexcept {
  return CpuSkinMtControllerNativeBodyLease(
      m_frozen.owner.controllerInstanceGeneration,
      m_frozen.owner.producerGeneration,
      m_activeGenerationAtomic.load(std::memory_order_acquire),
      m_frozen.flush.frameTag,
      m_frozen.flush.batchId,
      m_destination.lockSerial);
}

bool CpuSkinMtControllerJob::nativeBodyLeaseMatchesLocked(
    const CpuSkinMtControllerNativeBodyLease& lease) const noexcept {
  return lease.valid() && lease == CpuSkinMtControllerNativeBodyLease(
      m_frozen.owner.controllerInstanceGeneration,
      m_frozen.owner.producerGeneration,
      m_activeGeneration,
      m_frozen.flush.frameTag,
      m_frozen.flush.batchId,
      m_destination.lockSerial);
}

void CpuSkinMtControllerJob::settleProducerConsumedLocked() noexcept {
  if (!m_producerResult || m_producerPublicationSettled)
    return;
  m_producerPublicationSettled = true;
  m_producerConsumed = true;
  Decrement(m_ledger->m_counters.liveProducerResults);
  Increment(m_ledger->m_counters.producerResultConsumptions);
  if (m_producerClaimed) {
    Decrement(m_ledger->m_counters.liveProducerClaims);
  }
}

void CpuSkinMtControllerJob::settleProducerAbandonedLocked() noexcept {
  if (m_producerResult && !m_producerPublicationSettled) {
    m_producerPublicationSettled = true;
    Decrement(m_ledger->m_counters.liveProducerResults);
    Increment(m_ledger->m_counters.producerResultAbandoned);
  }
  if (m_producerClaimed && !m_producerConsumed) {
    m_producerClaimed = false;
    Decrement(m_ledger->m_counters.liveProducerClaims);
    Increment(m_ledger->m_counters.producerClaimAbandoned);
  }
}

void CpuSkinMtControllerJob::materializeAtomicRouteLocked() noexcept {
  if (m_route != CpuSkinMtControllerKernelRoute::None)
    return;

  const CpuSkinMtControllerKernelRoute authority =
      m_routeAuthority.load(std::memory_order_acquire);
  if (authority == CpuSkinMtControllerKernelRoute::Native) {
    m_route = CpuSkinMtControllerKernelRoute::Native;
    m_state = CpuSkinMtControllerState::NativeBodyInProgress;
    m_outerSettlementOpen = true;
    m_nativeBodyLeaseLive = true;
    settleProducerAbandonedLocked();
    m_commitEnvelope = CpuSkinMtControllerRenderCommitEnvelope{};
    m_producerResult.reset();
  } else if (authority == CpuSkinMtControllerKernelRoute::Copy) {
    m_route = CpuSkinMtControllerKernelRoute::Copy;
  }
}

CpuSkinMtControllerKernelDecision
CpuSkinMtControllerJob::selectNativeWithoutWaiting(
    CpuSkinMtControllerKernelReason reason,
    bool stateLockAcquired) noexcept {
  CpuSkinMtControllerKernelRoute expected =
      CpuSkinMtControllerKernelRoute::None;
  if (m_routeAuthority.compare_exchange_strong(
          expected, CpuSkinMtControllerKernelRoute::Native,
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    m_kernelWindowOpen.store(false, std::memory_order_release);
    Increment(m_ledger->m_counters.nativeSelections);
    Increment(m_ledger->m_counters.liveNativeJobs);
    Increment(m_ledger->m_counters.nativeBodyLeases);
    Increment(m_ledger->m_counters.liveNativeBodyLeases);
    Increment(m_ledger->m_counters.outerSettlementsOpened);
    Increment(m_ledger->m_counters.liveOuterSettlements);
    return CpuSkinMtControllerKernelDecision{
        CpuSkinMtControllerResult::Applied,
        CpuSkinMtControllerKernelRoute::Native,
        reason,
        makeNativeBodyLease(),
        stateLockAcquired,
        true};
  }

  if (expected == CpuSkinMtControllerKernelRoute::Native ||
      expected == CpuSkinMtControllerKernelRoute::Copy) {
    return CpuSkinMtControllerKernelDecision{
        CpuSkinMtControllerResult::AlreadyApplied,
        expected,
        CpuSkinMtControllerKernelReason::AlreadyDecided,
        CpuSkinMtControllerNativeBodyLease{},
        stateLockAcquired,
        false};
  }

  return CpuSkinMtControllerKernelDecision{
      CpuSkinMtControllerResult::InvalidTransition,
      CpuSkinMtControllerKernelRoute::Cancelled,
      CpuSkinMtControllerKernelReason::WindowClosed,
      CpuSkinMtControllerNativeBodyLease{},
      stateLockAcquired,
      false};
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::rejectStaleLocked()
    noexcept {
  Increment(m_ledger->m_counters.staleGenerationRejects);
  return CpuSkinMtControllerResult::StaleGeneration;
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::rejectInvalidLocked()
    noexcept {
  Increment(m_ledger->m_counters.invalidTransitionRejects);
  return CpuSkinMtControllerResult::InvalidTransition;
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::rejectKeyLocked() noexcept {
  Increment(m_ledger->m_counters.keyMismatchRejects);
  return CpuSkinMtControllerResult::KeyMismatch;
}

CpuSkinMtControllerResult
CpuSkinMtControllerJob::rejectDoubleTerminalLocked() noexcept {
  Increment(m_ledger->m_counters.doubleTerminalRejects);
  return CpuSkinMtControllerResult::DoubleTerminal;
}

bool CpuSkinMtControllerJob::shouldDeferCancellationLocked() const noexcept {
  if (m_route == CpuSkinMtControllerKernelRoute::Native &&
      !m_bodyCompleted)
    return true;
  return m_route != CpuSkinMtControllerKernelRoute::None &&
      m_bodyCompleted && !m_unlockObserved;
}

CpuSkinMtControllerResult
CpuSkinMtControllerJob::queueDeferredCancellationLocked(
    CpuSkinMtControllerTerminal terminal,
    uint64_t nextGeneration) noexcept {
  if (m_deferredTerminal != CpuSkinMtControllerTerminal::None) {
    if (m_deferredTerminal == terminal &&
        (terminal != CpuSkinMtControllerTerminal::ResetCancelled ||
         m_pendingResetGeneration == nextGeneration))
      return CpuSkinMtControllerResult::AlreadyApplied;
    return rejectInvalidLocked();
  }

  m_deferredTerminal = terminal;
  m_pendingResetGeneration = nextGeneration;
  m_kernelWindowOpen.store(false, std::memory_order_release);
  if (terminal == CpuSkinMtControllerTerminal::ResetCancelled)
    Increment(m_ledger->m_counters.deferredResetCancellations);
  else if (terminal == CpuSkinMtControllerTerminal::OuterCancelled)
    Increment(m_ledger->m_counters.deferredOuterCancellations);
  else if (terminal == CpuSkinMtControllerTerminal::TemplateMismatch)
    Increment(m_ledger->m_counters.deferredTemplateCancellations);
  return CpuSkinMtControllerResult::Deferred;
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::finishLocked(
    CpuSkinMtControllerState state,
    CpuSkinMtControllerTerminal terminal) noexcept {
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
  if (terminal == CpuSkinMtControllerTerminal::None)
    return rejectInvalidLocked();

  materializeAtomicRouteLocked();
  m_kernelWindowOpen.store(false, std::memory_order_release);
  CpuSkinMtControllerKernelRoute expected =
      CpuSkinMtControllerKernelRoute::None;
  m_routeAuthority.compare_exchange_strong(
      expected, CpuSkinMtControllerKernelRoute::Cancelled,
      std::memory_order_acq_rel, std::memory_order_acquire);

  settleProducerAbandonedLocked();
  if (m_nativeBodyLeaseLive) {
    m_nativeBodyLeaseLive = false;
    Decrement(m_ledger->m_counters.liveNativeBodyLeases);
    Increment(m_ledger->m_counters.nativeBodyLeasesAbandoned);
  }
  if (m_outerSettlementOpen) {
    m_outerSettlementOpen = false;
    Decrement(m_ledger->m_counters.liveOuterSettlements);
    Increment(m_ledger->m_counters.outerSettlementsCompleted);
  }

  m_state = state;
  m_terminal = terminal;
  Decrement(m_ledger->m_counters.liveJobs);

  switch (terminal) {
    case CpuSkinMtControllerTerminal::CopiedNormal:
      Increment(m_ledger->m_counters.copiedNormal);
      break;
    case CpuSkinMtControllerTerminal::OriginalNormal:
      Increment(m_ledger->m_counters.originalNormal);
      break;
    case CpuSkinMtControllerTerminal::OriginalFault:
      Increment(m_ledger->m_counters.originalFault);
      break;
    case CpuSkinMtControllerTerminal::CopyFault:
      Increment(m_ledger->m_counters.copyFault);
      break;
    case CpuSkinMtControllerTerminal::TemplateMismatch:
      Increment(m_ledger->m_counters.templateMismatch);
      break;
    case CpuSkinMtControllerTerminal::ResetCancelled:
      Increment(m_ledger->m_counters.resetCancelled);
      break;
    case CpuSkinMtControllerTerminal::OuterCancelled:
      Increment(m_ledger->m_counters.outerCancelled);
      break;
    case CpuSkinMtControllerTerminal::None:
      break;
  }

  if (m_route == CpuSkinMtControllerKernelRoute::Copy) {
    Decrement(m_ledger->m_counters.liveCopyJobs);
    if (terminal != CpuSkinMtControllerTerminal::CopiedNormal &&
        terminal != CpuSkinMtControllerTerminal::CopyFault)
      Increment(m_ledger->m_counters.copyCancelled);
  } else if (m_route == CpuSkinMtControllerKernelRoute::Native) {
    Decrement(m_ledger->m_counters.liveNativeJobs);
    if (terminal != CpuSkinMtControllerTerminal::OriginalNormal &&
        terminal != CpuSkinMtControllerTerminal::OriginalFault)
      Increment(m_ledger->m_counters.nativeCancelled);
  } else {
    Increment(m_ledger->m_counters.unselectedCancelled);
  }

  if (terminal == CpuSkinMtControllerTerminal::ResetCancelled) {
    if (m_pendingResetGeneration > m_activeGeneration) {
      m_activeGeneration = m_pendingResetGeneration;
      m_activeGenerationAtomic.store(m_activeGeneration,
                                     std::memory_order_release);
    }
  }
  m_deferredTerminal = CpuSkinMtControllerTerminal::None;
  m_pendingResetGeneration = 0u;
  m_commitEnvelope = CpuSkinMtControllerRenderCommitEnvelope{};
  m_producerResult.reset();
  return CpuSkinMtControllerResult::Applied;
}

CpuSkinMtControllerResult
CpuSkinMtControllerJob::finalizeRouteAfterUnlockLocked() noexcept {
  if (!m_unlockObserved || !m_bodyCompleted)
    return CpuSkinMtControllerResult::Applied;

  if (m_deferredTerminal != CpuSkinMtControllerTerminal::None) {
    const CpuSkinMtControllerTerminal terminal = m_deferredTerminal;
    CpuSkinMtControllerState state = CpuSkinMtControllerState::OuterCancelled;
    if (terminal == CpuSkinMtControllerTerminal::ResetCancelled)
      state = CpuSkinMtControllerState::ResetCancelled;
    else if (terminal == CpuSkinMtControllerTerminal::TemplateMismatch)
      state = CpuSkinMtControllerState::TemplateMismatch;
    return finishLocked(state, terminal);
  }

  const bool unlockSucceeded = m_unlock.result >= 0;
  if (m_route == CpuSkinMtControllerKernelRoute::Copy) {
    if (m_bodySucceeded && unlockSucceeded)
      return finishLocked(CpuSkinMtControllerState::CopiedNormal,
                          CpuSkinMtControllerTerminal::CopiedNormal);
    return finishLocked(CpuSkinMtControllerState::CopyFault,
                        CpuSkinMtControllerTerminal::CopyFault);
  }
  if (m_route == CpuSkinMtControllerKernelRoute::Native) {
    if (m_bodySucceeded && unlockSucceeded)
      return finishLocked(CpuSkinMtControllerState::OriginalNormal,
                          CpuSkinMtControllerTerminal::OriginalNormal);
    return finishLocked(CpuSkinMtControllerState::OriginalFault,
                        CpuSkinMtControllerTerminal::OriginalFault);
  }
  return rejectInvalidLocked();
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::submit(
    uint64_t generation) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
  if (m_state == CpuSkinMtControllerState::Submitted)
    return CpuSkinMtControllerResult::AlreadyApplied;
  if (m_state != CpuSkinMtControllerState::Frozen)
    return rejectInvalidLocked();
  m_state = CpuSkinMtControllerState::Submitted;
  return CpuSkinMtControllerResult::Applied;
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::publishProducerResult(
    uint64_t generation,
    std::shared_ptr<const CpuSkinMtControllerOwnedProducerResult> result)
    noexcept {
  if (generation !=
      m_activeGenerationAtomic.load(std::memory_order_acquire)) {
    Increment(m_ledger->m_counters.staleGenerationRejects);
    return CpuSkinMtControllerResult::StaleGeneration;
  }
  const CpuSkinMtControllerKernelRoute initialAuthority =
      m_routeAuthority.load(std::memory_order_acquire);
  if (initialAuthority == CpuSkinMtControllerKernelRoute::Native ||
      initialAuthority == CpuSkinMtControllerKernelRoute::Cancelled) {
    Increment(m_ledger->m_counters.lateProducerReadyRejects);
    return CpuSkinMtControllerResult::LateProducerReady;
  }
  if (!result || !result->proofComplete()) {
    Increment(m_ledger->m_counters.keyMismatchRejects);
    return CpuSkinMtControllerResult::KeyMismatch;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  materializeAtomicRouteLocked();
  if (m_route == CpuSkinMtControllerKernelRoute::Native ||
      isTerminalLocked()) {
    Increment(m_ledger->m_counters.lateProducerReadyRejects);
    return CpuSkinMtControllerResult::LateProducerReady;
  }
  if (m_producerResult) {
    return m_producerResult == result
        ? CpuSkinMtControllerResult::AlreadyPublished
        : rejectKeyLocked();
  }
  if (m_state != CpuSkinMtControllerState::Submitted &&
      m_state != CpuSkinMtControllerState::BoundToOuter &&
      m_state != CpuSkinMtControllerState::AwaitingKernel)
    return rejectInvalidLocked();
  if (result->proof().frozenKey != m_frozen ||
      result->proof().producerGeneration !=
          m_frozen.owner.producerGeneration ||
      result->proof().outputByteSize != ExpectedOutputBytes(m_frozen))
    return rejectKeyLocked();

  m_producerResult = std::move(result);
  m_producerPublicationSettled = false;
  Increment(m_ledger->m_counters.producerResultPublications);
  Increment(m_ledger->m_counters.liveProducerResults);
  return CpuSkinMtControllerResult::Published;
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::bindOuter(
    uint64_t generation,
    const CpuSkinMtControllerOuterBindingProof& outer) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
  if (m_state == CpuSkinMtControllerState::BoundToOuter)
    return m_outer == outer ? CpuSkinMtControllerResult::AlreadyApplied
                            : rejectKeyLocked();
  if (m_state != CpuSkinMtControllerState::Submitted)
    return rejectInvalidLocked();
  if (!CpuSkinMtControllerOuterMatchesFrozen(m_frozen, outer))
    return rejectKeyLocked();
  m_outer = outer;
  m_state = CpuSkinMtControllerState::BoundToOuter;
  return CpuSkinMtControllerResult::Applied;
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::awaitKernel(
    uint64_t generation,
    const CpuSkinMtControllerDestinationProof& destination) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
  if (m_state == CpuSkinMtControllerState::AwaitingKernel)
    return m_destination == destination
        ? CpuSkinMtControllerResult::AlreadyApplied
        : rejectKeyLocked();
  if (m_state != CpuSkinMtControllerState::BoundToOuter)
    return rejectInvalidLocked();
  if (!CpuSkinMtControllerDestinationValid(
          m_frozen, m_outer, destination))
    return rejectKeyLocked();
  m_destination = destination;
  m_state = CpuSkinMtControllerState::AwaitingKernel;
  m_kernelWindowOpen.store(true, std::memory_order_release);
  return CpuSkinMtControllerResult::Applied;
}

CpuSkinMtControllerKernelDecision
CpuSkinMtControllerJob::trySelectKernelRoute(
    uint64_t generation,
    uint32_t currentMxcsr) noexcept {
  if (generation !=
      m_activeGenerationAtomic.load(std::memory_order_acquire)) {
    Increment(m_ledger->m_counters.staleGenerationRejects);
    return CpuSkinMtControllerKernelDecision{
        CpuSkinMtControllerResult::StaleGeneration,
        CpuSkinMtControllerKernelRoute::None,
        CpuSkinMtControllerKernelReason::WindowClosed,
        CpuSkinMtControllerNativeBodyLease{}, false, false};
  }
  if (!m_kernelWindowOpen.load(std::memory_order_acquire)) {
    Increment(m_ledger->m_counters.invalidTransitionRejects);
    return CpuSkinMtControllerKernelDecision{
        CpuSkinMtControllerResult::InvalidTransition,
        CpuSkinMtControllerKernelRoute::None,
        CpuSkinMtControllerKernelReason::WindowClosed,
        CpuSkinMtControllerNativeBodyLease{}, false, false};
  }

  std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    if (generation !=
            m_activeGenerationAtomic.load(std::memory_order_acquire) ||
        !m_kernelWindowOpen.load(std::memory_order_acquire)) {
      Increment(m_ledger->m_counters.invalidTransitionRejects);
      return CpuSkinMtControllerKernelDecision{
          CpuSkinMtControllerResult::InvalidTransition,
          CpuSkinMtControllerKernelRoute::None,
          CpuSkinMtControllerKernelReason::WindowClosed,
          CpuSkinMtControllerNativeBodyLease{}, false, false};
    }
    return selectNativeWithoutWaiting(
        CpuSkinMtControllerKernelReason::StateLockContended, false);
  }

  if (generation != m_activeGeneration || isTerminalLocked() ||
      !m_kernelWindowOpen.load(std::memory_order_acquire)) {
    return CpuSkinMtControllerKernelDecision{
        rejectInvalidLocked(), CpuSkinMtControllerKernelRoute::None,
        CpuSkinMtControllerKernelReason::WindowClosed,
        CpuSkinMtControllerNativeBodyLease{}, true, false};
  }
  const CpuSkinMtControllerKernelRoute authority =
      m_routeAuthority.load(std::memory_order_acquire);
  if (authority != CpuSkinMtControllerKernelRoute::None) {
    materializeAtomicRouteLocked();
    return CpuSkinMtControllerKernelDecision{
        authority == CpuSkinMtControllerKernelRoute::Cancelled
            ? CpuSkinMtControllerResult::InvalidTransition
            : CpuSkinMtControllerResult::AlreadyApplied,
        authority,
        authority == CpuSkinMtControllerKernelRoute::Cancelled
            ? CpuSkinMtControllerKernelReason::WindowClosed
            : CpuSkinMtControllerKernelReason::AlreadyDecided,
        CpuSkinMtControllerNativeBodyLease{}, true, false};
  }
  if (m_state != CpuSkinMtControllerState::AwaitingKernel)
    return CpuSkinMtControllerKernelDecision{
        rejectInvalidLocked(), CpuSkinMtControllerKernelRoute::None,
        CpuSkinMtControllerKernelReason::WindowClosed,
        CpuSkinMtControllerNativeBodyLease{}, true, false};

  if (!m_producerResult) {
    Increment(m_ledger->m_counters.producerNotReadyRejects);
    CpuSkinMtControllerKernelDecision decision =
        selectNativeWithoutWaiting(
            CpuSkinMtControllerKernelReason::ProducerNotReady, true);
    materializeAtomicRouteLocked();
    return decision;
  }

  const uint32_t normalizedCurrent =
      CpuSkinMtNormalizeMxcsrControl(currentMxcsr);
  if (!CpuSkinMtHasSafeMxcsrControl(normalizedCurrent) ||
      normalizedCurrent != m_producerResult->proof().normalizedMxcsr) {
    Increment(m_ledger->m_counters.mxcsrMismatchRejects);
    CpuSkinMtControllerKernelDecision decision =
        selectNativeWithoutWaiting(
            CpuSkinMtControllerKernelReason::MxcsrMismatch, true);
    materializeAtomicRouteLocked();
    return decision;
  }

  CpuSkinMtControllerRenderCommitEnvelope envelope =
      CpuSkinMtControllerRenderCommitEnvelope::MintAfterLock(
          m_frozen, m_outer, m_destination, m_producerResult,
          m_destination.lockSerial, currentMxcsr);
  if (!envelope.valid()) {
    CpuSkinMtControllerKernelDecision decision =
        selectNativeWithoutWaiting(
            CpuSkinMtControllerKernelReason::EnvelopeMismatch, true);
    materializeAtomicRouteLocked();
    return decision;
  }

  CpuSkinMtControllerKernelRoute expected =
      CpuSkinMtControllerKernelRoute::None;
  if (!m_routeAuthority.compare_exchange_strong(
          expected, CpuSkinMtControllerKernelRoute::Copy,
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    materializeAtomicRouteLocked();
    return CpuSkinMtControllerKernelDecision{
        expected == CpuSkinMtControllerKernelRoute::Cancelled
            ? CpuSkinMtControllerResult::InvalidTransition
            : CpuSkinMtControllerResult::AlreadyApplied,
        expected,
        expected == CpuSkinMtControllerKernelRoute::Cancelled
            ? CpuSkinMtControllerKernelReason::WindowClosed
            : CpuSkinMtControllerKernelReason::AlreadyDecided,
        CpuSkinMtControllerNativeBodyLease{}, true, false};
  }

  m_kernelWindowOpen.store(false, std::memory_order_release);
  m_route = CpuSkinMtControllerKernelRoute::Copy;
  m_state = CpuSkinMtControllerState::CommitClaimed;
  m_commitEnvelope = std::move(envelope);
  m_producerClaimed = true;
  m_outerSettlementOpen = true;
  Increment(m_ledger->m_counters.producerResultClaims);
  Increment(m_ledger->m_counters.liveProducerClaims);
  Increment(m_ledger->m_counters.copySelections);
  Increment(m_ledger->m_counters.liveCopyJobs);
  Increment(m_ledger->m_counters.outerSettlementsOpened);
  Increment(m_ledger->m_counters.liveOuterSettlements);
  return CpuSkinMtControllerKernelDecision{
      CpuSkinMtControllerResult::Applied,
      CpuSkinMtControllerKernelRoute::Copy,
      CpuSkinMtControllerKernelReason::ReadyProducerResult,
      CpuSkinMtControllerNativeBodyLease{}, true, true};
}

CpuSkinMtControllerResult
CpuSkinMtControllerJob::copyProducerResultUnderLease(
    uint64_t generation,
    CpuSkinMtControllerCopyCallback callback,
    void* context) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  if (m_unlockObserved) {
    Increment(m_ledger->m_counters.copyAfterUnlockRejects);
    return CpuSkinMtControllerResult::CopyAfterUnlock;
  }
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
  if (callback == nullptr)
    return CpuSkinMtControllerResult::MissingCopyCallback;
  if (m_state != CpuSkinMtControllerState::CommitClaimed ||
      m_route != CpuSkinMtControllerKernelRoute::Copy ||
      !m_producerResult || !m_producerResult->proofComplete() ||
      !m_commitEnvelope.valid() ||
      !(m_commitEnvelope.proof().producerResult ==
          m_producerResult->proof()))
    return rejectInvalidLocked();

  m_state = CpuSkinMtControllerState::CopyBodyInProgress;
  const CpuSkinMtControllerProducerResultView view{
      m_producerResult->data(), m_producerResult->size(),
      &m_producerResult->proof(), &m_commitEnvelope.proof()};
  m_bodyCompletion = callback(context, view, m_destination);
  m_bodyMxcsrStatusDelta = CpuSkinMtCaptureMxcsrStatusDelta(
      m_bodyCompletion.mxcsrBefore, m_bodyCompletion.mxcsrAfter);
  const bool statusMatches = m_bodyMxcsrStatusDelta.valid &&
      m_bodyMxcsrStatusDelta ==
          m_producerResult->proof().mxcsrStatusDelta;
  m_bodySucceeded = m_bodyCompletion.succeeded && statusMatches;
  m_bodyCompleted = true;
  m_state = CpuSkinMtControllerState::CopyAwaitingUnlock;
  settleProducerConsumedLocked();
  Increment(m_ledger->m_counters.guardedCopies);
  if (m_bodyMxcsrStatusDelta.valid)
    Increment(m_ledger->m_counters.mxcsrStatusDeltasRecorded);
  if (!statusMatches) {
    Increment(m_ledger->m_counters.mxcsrStatusDeltaMismatches);
  }
  if (!m_bodySucceeded)
    Increment(m_ledger->m_counters.guardedCopyBodyFaults);
  const CpuSkinMtControllerResult finalize =
      finalizeRouteAfterUnlockLocked();
  if (finalize != CpuSkinMtControllerResult::Applied)
    return finalize;
  return statusMatches ? CpuSkinMtControllerResult::Consumed
                       : CpuSkinMtControllerResult::MxcsrStatusDeltaMismatch;
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::completeNativeBody(
    uint64_t generation,
    const CpuSkinMtControllerNativeBodyLease& lease,
    const CpuSkinMtControllerBodyCompletion& completion) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  materializeAtomicRouteLocked();
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
  if (m_route != CpuSkinMtControllerKernelRoute::Native ||
      !nativeBodyLeaseMatchesLocked(lease)) {
    Increment(m_ledger->m_counters.invalidNativeLeaseRejects);
    return CpuSkinMtControllerResult::InvalidNativeLease;
  }
  if (m_bodyCompleted)
    return m_bodyCompletion == completion
        ? CpuSkinMtControllerResult::AlreadyApplied
        : rejectInvalidLocked();
  if (m_state != CpuSkinMtControllerState::NativeBodyInProgress)
    return rejectInvalidLocked();

  m_bodyCompletion = completion;
  m_bodyMxcsrStatusDelta = CpuSkinMtCaptureMxcsrStatusDelta(
      completion.mxcsrBefore, completion.mxcsrAfter);
  const bool statusValid = m_bodyMxcsrStatusDelta.valid &&
      m_bodyMxcsrStatusDelta.normalizedControl ==
          m_frozen.palette.floatingPointControl;
  m_bodySucceeded = completion.succeeded && statusValid;
  m_bodyCompleted = true;
  m_state = CpuSkinMtControllerState::NativeAwaitingUnlock;
  if (m_nativeBodyLeaseLive) {
    m_nativeBodyLeaseLive = false;
    Decrement(m_ledger->m_counters.liveNativeBodyLeases);
    Increment(m_ledger->m_counters.nativeBodiesCompleted);
  }
  if (m_bodyMxcsrStatusDelta.valid)
    Increment(m_ledger->m_counters.mxcsrStatusDeltasRecorded);
  if (!statusValid)
    Increment(m_ledger->m_counters.mxcsrStatusDeltaMismatches);
  const CpuSkinMtControllerResult finalize =
      finalizeRouteAfterUnlockLocked();
  if (finalize != CpuSkinMtControllerResult::Applied)
    return finalize;
  return statusValid ? CpuSkinMtControllerResult::Applied
                     : CpuSkinMtControllerResult::MxcsrStatusDeltaMismatch;
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::noteUnlock(
    uint64_t generation,
    const CpuSkinMtControllerUnlockProof& unlock) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  materializeAtomicRouteLocked();
  if (!CpuSkinMtControllerUnlockMatchesDestination(
          m_destination, unlock))
    return rejectKeyLocked();
  if (m_unlockObserved)
    return m_unlock == unlock ? CpuSkinMtControllerResult::AlreadyApplied
                              : rejectKeyLocked();

  m_unlock = unlock;
  m_unlockObserved = true;
  if (unlock.result >= 0)
    Increment(m_ledger->m_counters.successfulUnlocks);
  else
    Increment(m_ledger->m_counters.failedUnlocks);
  if (isTerminalLocked())
    return CpuSkinMtControllerResult::Applied;

  if (m_route == CpuSkinMtControllerKernelRoute::Copy) {
    if (!m_bodyCompleted) {
      m_bodyCompleted = true;
      m_bodySucceeded = false;
      m_bodyCompletion = CpuSkinMtControllerBodyCompletion{};
      m_state = CpuSkinMtControllerState::CopyAwaitingUnlock;
      settleProducerAbandonedLocked();
    }
    return finalizeRouteAfterUnlockLocked();
  }
  if (m_route == CpuSkinMtControllerKernelRoute::Native) {
    // Native is already protected by its issued body lease. Unlock-first is
    // recorded, but cannot fabricate a body result or terminate the job.
    return finalizeRouteAfterUnlockLocked();
  }
  return finishLocked(CpuSkinMtControllerState::OuterCancelled,
                      CpuSkinMtControllerTerminal::OuterCancelled);
}

CpuSkinMtControllerResult
CpuSkinMtControllerJob::cancelTemplateMismatch(
    uint64_t generation) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
  materializeAtomicRouteLocked();
  if (shouldDeferCancellationLocked())
    return queueDeferredCancellationLocked(
        CpuSkinMtControllerTerminal::TemplateMismatch, 0u);
  return finishLocked(CpuSkinMtControllerState::TemplateMismatch,
                      CpuSkinMtControllerTerminal::TemplateMismatch);
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::cancelOuter(
    uint64_t generation) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
  materializeAtomicRouteLocked();
  if (shouldDeferCancellationLocked())
    return queueDeferredCancellationLocked(
        CpuSkinMtControllerTerminal::OuterCancelled, 0u);
  return finishLocked(CpuSkinMtControllerState::OuterCancelled,
                      CpuSkinMtControllerTerminal::OuterCancelled);
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::cancelForReset(
    uint64_t generation,
    uint64_t nextGeneration) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  if (nextGeneration <= generation)
    return rejectInvalidLocked();
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
  materializeAtomicRouteLocked();
  if (shouldDeferCancellationLocked())
    return queueDeferredCancellationLocked(
        CpuSkinMtControllerTerminal::ResetCancelled, nextGeneration);
  m_pendingResetGeneration = nextGeneration;
  return finishLocked(CpuSkinMtControllerState::ResetCancelled,
                      CpuSkinMtControllerTerminal::ResetCancelled);
}

CpuSkinMtControllerState CpuSkinMtControllerJob::state() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_state;
}

CpuSkinMtControllerTerminal CpuSkinMtControllerJob::terminal() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_terminal;
}

uint64_t CpuSkinMtControllerJob::activeGeneration() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_activeGeneration;
}

bool CpuSkinMtControllerJob::producerResultPublished() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  return bool(m_producerResult);
}

bool CpuSkinMtControllerJob::producerResultConsumed() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_producerConsumed;
}

bool CpuSkinMtControllerJob::unlockObserved() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_unlockObserved;
}

CpuSkinMtControllerProducerResultProof
CpuSkinMtControllerJob::producerResultProof() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_producerResult ? m_producerResult->proof()
                          : CpuSkinMtControllerProducerResultProof{};
}

CpuSkinMtControllerRenderCommitEnvelopeProof
CpuSkinMtControllerJob::renderCommitEnvelopeProof() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_commitEnvelope.valid()
      ? m_commitEnvelope.proof()
      : CpuSkinMtControllerRenderCommitEnvelopeProof{};
}

CpuSkinMtControllerOuterSettlementSnapshot
CpuSkinMtControllerJob::outerSettlement() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  return CpuSkinMtControllerOuterSettlementSnapshot{
      m_route,
      m_deferredTerminal,
      m_bodyCompletion,
      m_bodyMxcsrStatusDelta,
      m_outerSettlementOpen,
      m_bodyCompleted,
      m_unlockObserved,
      bool(m_producerResult),
      m_producerClaimed,
      m_producerConsumed,
      m_nativeBodyLeaseLive,
      m_deferredTerminal != CpuSkinMtControllerTerminal::None,
      isTerminalLocked()};
}

}  // namespace dxvk::war3::gpu_skin
