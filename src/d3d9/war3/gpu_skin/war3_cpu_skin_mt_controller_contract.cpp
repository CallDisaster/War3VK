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

}  // namespace

bool operator==(const CpuSkinMtControllerOwnerSessionProof& lhs,
                const CpuSkinMtControllerOwnerSessionProof& rhs) noexcept {
  return lhs.controllerInstanceGeneration == rhs.controllerInstanceGeneration &&
      lhs.producerGeneration == rhs.producerGeneration &&
      lhs.mapEpoch == rhs.mapEpoch &&
      lhs.deviceEpoch == rhs.deviceEpoch &&
      lhs.bridgeResetGeneration == rhs.bridgeResetGeneration &&
      lhs.renderThreadId == rhs.renderThreadId;
}

bool operator==(const CpuSkinMtControllerFlushBindingProof& lhs,
                const CpuSkinMtControllerFlushBindingProof& rhs) noexcept {
  return lhs.frameTag == rhs.frameTag &&
      lhs.flushEpoch == rhs.flushEpoch &&
      lhs.batchId == rhs.batchId &&
      lhs.renderablePart == rhs.renderablePart &&
      lhs.geosetData == rhs.geosetData &&
      lhs.candidateToken == rhs.candidateToken &&
      lhs.flushCandidateOrdinal == rhs.flushCandidateOrdinal &&
      lhs.layerIndex == rhs.layerIndex &&
      lhs.path == rhs.path &&
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
      lhs.positions == rhs.positions &&
      lhs.normals == rhs.normals &&
      lhs.groupSlots == rhs.groupSlots &&
      lhs.uv0 == rhs.uv0 &&
      lhs.extra == rhs.extra &&
      lhs.uv1 == rhs.uv1 &&
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
      lhs.groupCount == rhs.groupCount &&
      lhs.byteCount == rhs.byteCount &&
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
      lhs.geosetData == rhs.geosetData &&
      lhs.positions == rhs.positions &&
      lhs.normals == rhs.normals &&
      lhs.groupSlots == rhs.groupSlots &&
      lhs.uv0 == rhs.uv0 &&
      lhs.extra == rhs.extra &&
      lhs.uv1 == rhs.uv1 &&
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
      lhs.outputFormat == rhs.outputFormat &&
      lhs.fvf == rhs.fvf &&
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
      lhs.uploadEpoch == rhs.uploadEpoch &&
      lhs.offset == rhs.offset &&
      lhs.size == rhs.size &&
      lhs.effectiveFlags == rhs.effectiveFlags &&
      lhs.lockDepth == rhs.lockDepth &&
      lhs.uploadOrdinal == rhs.uploadOrdinal &&
      lhs.outputFormat == rhs.outputFormat &&
      lhs.fvf == rhs.fvf &&
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
      lhs.unlockSerial == rhs.unlockSerial &&
      lhs.result == rhs.result;
}

bool operator==(const CpuSkinMtControllerFrozenKey& lhs,
                const CpuSkinMtControllerFrozenKey& rhs) noexcept {
  return lhs.owner == rhs.owner && lhs.flush == rhs.flush &&
      lhs.source == rhs.source && lhs.palette == rhs.palette;
}

bool operator==(const CpuSkinMtControllerExactKey& lhs,
                const CpuSkinMtControllerExactKey& rhs) noexcept {
  return lhs.owner == rhs.owner && lhs.flush == rhs.flush &&
      lhs.source == rhs.source && lhs.palette == rhs.palette &&
      lhs.outer == rhs.outer && lhs.destination == rhs.destination;
}

bool operator==(const CpuSkinMtControllerOutputProof& lhs,
                const CpuSkinMtControllerOutputProof& rhs) noexcept {
  return lhs.exactKey == rhs.exactKey &&
      lhs.resultSerial == rhs.resultSerial &&
      lhs.producerGeneration == rhs.producerGeneration &&
      lhs.outputContentHash == rhs.outputContentHash &&
      lhs.ownedBytesIdentity == rhs.ownedBytesIdentity &&
      lhs.outputByteSize == rhs.outputByteSize &&
      lhs.normalizedMxcsr == rhs.normalizedMxcsr &&
      lhs.proofVersion == rhs.proofVersion;
}

uint32_t CpuSkinMtNormalizeMxcsrControl(uint32_t mxcsr) noexcept {
  return mxcsr & kCpuSkinMtMxcsrControlMask;
}

bool CpuSkinMtHasSafeMxcsrControl(uint32_t mxcsr) noexcept {
  return (mxcsr & ~kCpuSkinMtMxcsrControlMask) == 0u &&
      (mxcsr & kCpuSkinMtMxcsrExceptionMask) ==
          kCpuSkinMtMxcsrExceptionMask;
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

  return true;
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
      outer.uv0 == frozen.source.uv0 &&
      outer.extra == frozen.source.extra && outer.uv1 == frozen.source.uv1 &&
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

  const uint64_t expectedSize =
      uint64_t(frozen.source.vertexCount) * kCpuSkinMtFormat2OutputStride;
  if (expectedSize == 0u ||
      expectedSize > std::numeric_limits<uint32_t>::max())
    return false;

  if (destination.commonResource == 0u ||
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
      !destination.lockSucceeded ||
      !destination.lockActive || destination.lockDepth != 1u ||
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

bool CpuSkinMtControllerExactKeyComplete(
    const CpuSkinMtControllerExactKey& key) noexcept {
  const CpuSkinMtControllerFrozenKey frozen{
      key.owner, key.flush, key.source, key.palette};
  return CpuSkinMtControllerDestinationValid(
      frozen, key.outer, key.destination);
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

  if (input.groupSlots != nullptr &&
      input.groupSlotCount == expectedGroups &&
      input.paletteGroupCount > 0u &&
      input.paletteGroupCount <= kCpuSkinMtFormat2MaxPaletteGroups) {
    for (size_t i = 0; i < input.groupSlotCount; i++) {
      if (input.groupSlots[i] >= input.paletteGroupCount) {
        fail(CpuSkinMtFormat2EligibilityGroupSlotRange);
        break;
      }
    }
  }

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

bool CpuSkinMtEligibilityMatchesExactKey(
    const CpuSkinMtControllerExactKey& key,
    const CpuSkinMtFormat2EligibilityInput& input) noexcept {
  const CpuSkinMtFormat2EligibilityResult eligibility =
      CpuSkinMtValidateFormat2Eligibility(input);
  if (!eligibility.eligible || !CpuSkinMtControllerExactKeyComplete(key))
    return false;

  if (input.path != key.flush.path || input.opaque != key.flush.opaque ||
      input.skinMode != key.outer.skinMode ||
      input.outputFormat != key.outer.outputFormat ||
      input.fvf != key.outer.fvf ||
      input.outputStride != key.outer.outputStride ||
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
      input.immutableSourceSealedToken !=
          key.source.sealedContentToken ||
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

std::shared_ptr<const CpuSkinMtControllerOwnedOutput>
CpuSkinMtControllerOwnedOutput::Create(
    const CpuSkinMtControllerExactKey& exactKey,
    const CpuSkinMtFormat2EligibilityInput& eligibility,
    uint64_t resultSerial,
    std::vector<uint8_t> bytes) {
  if (resultSerial == 0u ||
      !CpuSkinMtEligibilityMatchesExactKey(exactKey, eligibility) ||
      bytes.empty() ||
      bytes.size() > std::numeric_limits<uint32_t>::max() ||
      bytes.size() != exactKey.destination.size)
    return {};

  return std::shared_ptr<const CpuSkinMtControllerOwnedOutput>(
      new CpuSkinMtControllerOwnedOutput(
          exactKey, CpuSkinMtNormalizeMxcsrControl(eligibility.frozenMxcsr),
          resultSerial, std::move(bytes)));
}

CpuSkinMtControllerOwnedOutput::CpuSkinMtControllerOwnedOutput(
    const CpuSkinMtControllerExactKey& exactKey,
    uint32_t normalizedMxcsr,
    uint64_t resultSerial,
    std::vector<uint8_t> bytes) noexcept
    : m_bytes(std::move(bytes)) {
  m_proof.exactKey = exactKey;
  m_proof.resultSerial = resultSerial;
  m_proof.producerGeneration = exactKey.owner.producerGeneration;
  m_proof.outputContentHash = HashBytes(m_bytes.data(), m_bytes.size());
  m_proof.ownedBytesIdentity =
      reinterpret_cast<uintptr_t>(m_bytes.data());
  m_proof.outputByteSize = static_cast<uint32_t>(m_bytes.size());
  m_proof.normalizedMxcsr = normalizedMxcsr;
  m_proof.proofVersion = kCpuSkinMtOutputProofVersion;
}

const CpuSkinMtControllerOutputProof&
CpuSkinMtControllerOwnedOutput::proof() const noexcept {
  return m_proof;
}

const uint8_t* CpuSkinMtControllerOwnedOutput::data() const noexcept {
  return m_bytes.data();
}

uint32_t CpuSkinMtControllerOwnedOutput::size() const noexcept {
  return static_cast<uint32_t>(m_bytes.size());
}

bool CpuSkinMtControllerOwnedOutput::proofComplete() const noexcept {
  if (!CpuSkinMtControllerExactKeyComplete(m_proof.exactKey) ||
      m_proof.resultSerial == 0u ||
      m_proof.producerGeneration !=
          m_proof.exactKey.owner.producerGeneration ||
      m_proof.ownedBytesIdentity == 0u ||
      m_proof.ownedBytesIdentity !=
          reinterpret_cast<uintptr_t>(m_bytes.data()) ||
      m_proof.outputByteSize == 0u ||
      m_proof.outputByteSize != m_bytes.size() ||
      m_proof.outputByteSize != m_proof.exactKey.destination.size ||
      m_proof.proofVersion != kCpuSkinMtOutputProofVersion ||
      !CpuSkinMtHasSafeMxcsrControl(m_proof.normalizedMxcsr))
    return false;

  return m_proof.outputContentHash ==
      HashBytes(m_bytes.data(), m_bytes.size());
}

uint64_t CpuSkinMtControllerTerminalLedgerSnapshot::terminalJobs()
    const noexcept {
  return copiedNormal + originalNormal + originalFault + copyFault +
      templateMismatch + resetCancelled + outerCancelled;
}

bool CpuSkinMtControllerTerminalLedgerSnapshot::closureHolds()
    const noexcept {
  return jobsCreated == liveJobs + terminalJobs() &&
      copySelections == liveCopyJobs + copiedNormal + copyFault +
          copyCancelled &&
      nativeSelections == liveNativeJobs + originalNormal + originalFault +
          nativeCancelled;
}

CpuSkinMtControllerTerminalLedgerSnapshot
CpuSkinMtControllerTerminalLedger::snapshot() const noexcept {
  CpuSkinMtControllerTerminalLedgerSnapshot result;
#define LOAD_COUNTER(name) result.name = LoadRelaxed(m_counters.name)
  LOAD_COUNTER(jobsCreated);
  LOAD_COUNTER(liveJobs);
  LOAD_COUNTER(producerReadyPublications);
  LOAD_COUNTER(copySelections);
  LOAD_COUNTER(nativeSelections);
  LOAD_COUNTER(liveCopyJobs);
  LOAD_COUNTER(liveNativeJobs);
  LOAD_COUNTER(guardedCopies);
  LOAD_COUNTER(guardedCopyBodyFaults);
  LOAD_COUNTER(successfulUnlocks);
  LOAD_COUNTER(failedUnlocks);
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
#undef LOAD_COUNTER
  return result;
}

CpuSkinMtControllerJob::CpuSkinMtControllerJob(
    const CpuSkinMtControllerFrozenKey& frozen,
    uint64_t activeGeneration,
    CpuSkinMtControllerTerminalLedger& ledger) noexcept
    : m_activeGeneration(activeGeneration), m_ledger(&ledger) {
  m_activeGenerationAtomic.store(activeGeneration,
                                 std::memory_order_relaxed);
  m_key.owner = frozen.owner;
  m_key.flush = frozen.flush;
  m_key.source = frozen.source;
  m_key.palette = frozen.palette;

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

void CpuSkinMtControllerJob::materializeAtomicRouteLocked() noexcept {
  if (m_route != CpuSkinMtControllerKernelRoute::None)
    return;

  const CpuSkinMtControllerKernelRoute authority =
      m_routeAuthority.load(std::memory_order_acquire);
  if (authority == CpuSkinMtControllerKernelRoute::Native) {
    m_route = CpuSkinMtControllerKernelRoute::Native;
    m_state = CpuSkinMtControllerState::NativeSelected;
    m_output.reset();
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
    Increment(m_ledger->m_counters.nativeSelections);
    Increment(m_ledger->m_counters.liveNativeJobs);
    return CpuSkinMtControllerKernelDecision{
        CpuSkinMtControllerResult::Applied,
        CpuSkinMtControllerKernelRoute::Native,
        reason,
        stateLockAcquired,
        true};
  }

  if (expected == CpuSkinMtControllerKernelRoute::Native ||
      expected == CpuSkinMtControllerKernelRoute::Copy) {
    return CpuSkinMtControllerKernelDecision{
        CpuSkinMtControllerResult::AlreadyApplied,
        expected,
        CpuSkinMtControllerKernelReason::AlreadyDecided,
        stateLockAcquired,
        false};
  }

  return CpuSkinMtControllerKernelDecision{
      CpuSkinMtControllerResult::InvalidTransition,
      CpuSkinMtControllerKernelRoute::Cancelled,
      CpuSkinMtControllerKernelReason::WindowClosed,
      stateLockAcquired,
      false};
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::rejectStaleLocked() noexcept {
  Increment(m_ledger->m_counters.staleGenerationRejects);
  return CpuSkinMtControllerResult::StaleGeneration;
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::rejectInvalidLocked() noexcept {
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

CpuSkinMtControllerResult CpuSkinMtControllerJob::finishLocked(
    CpuSkinMtControllerState state,
    CpuSkinMtControllerTerminal terminal) noexcept {
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();

  m_kernelWindowOpen.store(false, std::memory_order_release);
  CpuSkinMtControllerKernelRoute expected =
      CpuSkinMtControllerKernelRoute::None;
  m_routeAuthority.compare_exchange_strong(
      expected, CpuSkinMtControllerKernelRoute::Cancelled,
      std::memory_order_acq_rel, std::memory_order_acquire);
  materializeAtomicRouteLocked();

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
      return rejectInvalidLocked();
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

  m_output.reset();
  return CpuSkinMtControllerResult::Applied;
}

CpuSkinMtControllerResult
CpuSkinMtControllerJob::finalizeRouteAfterUnlockLocked() noexcept {
  if (!m_unlockObserved || !m_bodyCompleted)
    return CpuSkinMtControllerResult::Applied;

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

CpuSkinMtControllerResult CpuSkinMtControllerJob::bindOuter(
    uint64_t generation,
    const CpuSkinMtControllerOuterBindingProof& outer) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
  if (m_state == CpuSkinMtControllerState::BoundToOuter)
    return m_key.outer == outer ? CpuSkinMtControllerResult::AlreadyApplied
                                : rejectKeyLocked();
  if (m_state != CpuSkinMtControllerState::Submitted)
    return rejectInvalidLocked();
  const CpuSkinMtControllerFrozenKey frozen{
      m_key.owner, m_key.flush, m_key.source, m_key.palette};
  if (!CpuSkinMtControllerOuterMatchesFrozen(frozen, outer))
    return rejectKeyLocked();
  m_key.outer = outer;
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
    return m_key.destination == destination
        ? CpuSkinMtControllerResult::AlreadyApplied
        : rejectKeyLocked();
  if (m_state != CpuSkinMtControllerState::BoundToOuter)
    return rejectInvalidLocked();
  const CpuSkinMtControllerFrozenKey frozen{
      m_key.owner, m_key.flush, m_key.source, m_key.palette};
  if (!CpuSkinMtControllerDestinationValid(
          frozen, m_key.outer, destination))
    return rejectKeyLocked();
  m_key.destination = destination;
  m_state = CpuSkinMtControllerState::AwaitingKernel;
  m_kernelWindowOpen.store(true, std::memory_order_release);
  return CpuSkinMtControllerResult::Applied;
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::publishProducerReady(
    uint64_t generation,
    std::shared_ptr<const CpuSkinMtControllerOwnedOutput> output) noexcept {
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

  // The content hash is deliberately checked before taking the controller
  // mutex. The render-owner kernel decision must never wait behind this scan.
  if (!output || !output->proofComplete()) {
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
  if (m_state != CpuSkinMtControllerState::AwaitingKernel &&
      m_state != CpuSkinMtControllerState::ProducerReady)
    return rejectInvalidLocked();
  if (output->proof().exactKey != m_key ||
      output->proof().producerGeneration !=
          m_key.owner.producerGeneration ||
      output->proof().outputByteSize != m_key.destination.size)
    return rejectKeyLocked();
  if (m_output) {
    return m_output == output
        ? CpuSkinMtControllerResult::AlreadyApplied
        : rejectKeyLocked();
  }
  m_output = std::move(output);
  m_state = CpuSkinMtControllerState::ProducerReady;
  Increment(m_ledger->m_counters.producerReadyPublications);
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
        false,
        false};
  }
  if (!m_kernelWindowOpen.load(std::memory_order_acquire)) {
    Increment(m_ledger->m_counters.invalidTransitionRejects);
    return CpuSkinMtControllerKernelDecision{
        CpuSkinMtControllerResult::InvalidTransition,
        CpuSkinMtControllerKernelRoute::None,
        CpuSkinMtControllerKernelReason::WindowClosed,
        false,
        false};
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
          false,
          false};
    }
    return selectNativeWithoutWaiting(
        CpuSkinMtControllerKernelReason::StateLockContended, false);
  }

  if (generation != m_activeGeneration || isTerminalLocked() ||
      !m_kernelWindowOpen.load(std::memory_order_acquire)) {
    return CpuSkinMtControllerKernelDecision{
        rejectInvalidLocked(),
        CpuSkinMtControllerKernelRoute::None,
        CpuSkinMtControllerKernelReason::WindowClosed,
        true,
        false};
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
        true,
        false};
  }

  if (m_state != CpuSkinMtControllerState::AwaitingKernel &&
      m_state != CpuSkinMtControllerState::ProducerReady) {
    return CpuSkinMtControllerKernelDecision{
        rejectInvalidLocked(),
        CpuSkinMtControllerKernelRoute::None,
        CpuSkinMtControllerKernelReason::WindowClosed,
        true,
        false};
  }
  if (!m_output) {
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
      normalizedCurrent != m_output->proof().normalizedMxcsr) {
    Increment(m_ledger->m_counters.mxcsrMismatchRejects);
    CpuSkinMtControllerKernelDecision decision =
        selectNativeWithoutWaiting(
            CpuSkinMtControllerKernelReason::MxcsrMismatch, true);
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
        true,
        false};
  }

  m_route = CpuSkinMtControllerKernelRoute::Copy;
  m_state = CpuSkinMtControllerState::CommitClaimed;
  Increment(m_ledger->m_counters.copySelections);
  Increment(m_ledger->m_counters.liveCopyJobs);
  return CpuSkinMtControllerKernelDecision{
      CpuSkinMtControllerResult::Applied,
      CpuSkinMtControllerKernelRoute::Copy,
      CpuSkinMtControllerKernelReason::ReadyOwnedOutput,
      true,
      true};
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::copyOutputUnderLease(
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
      m_route != CpuSkinMtControllerKernelRoute::Copy || !m_output ||
      !m_output->proofComplete() ||
      m_output->proof().exactKey != m_key)
    return rejectInvalidLocked();

  m_state = CpuSkinMtControllerState::CopyInProgress;
  const CpuSkinMtControllerOutputView view{
      m_output->data(), m_output->size(), &m_output->proof()};

  // Deliberately execute the byte writer while holding m_mutex. A concurrent
  // matching Unlock/reset/cancel cannot linearize until the callback returns.
  m_bodySucceeded = callback(context, view, m_key.destination);
  m_bodyCompleted = true;
  m_state = CpuSkinMtControllerState::CopyAwaitingUnlock;
  Increment(m_ledger->m_counters.guardedCopies);
  if (!m_bodySucceeded)
    Increment(m_ledger->m_counters.guardedCopyBodyFaults);
  return CpuSkinMtControllerResult::Applied;
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::completeOriginalNormal(
    uint64_t generation) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  materializeAtomicRouteLocked();
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
  if (m_route != CpuSkinMtControllerKernelRoute::Native ||
      (m_state != CpuSkinMtControllerState::NativeSelected &&
       m_state != CpuSkinMtControllerState::NativeAwaitingUnlock))
    return rejectInvalidLocked();
  if (m_bodyCompleted)
    return m_bodySucceeded ? CpuSkinMtControllerResult::AlreadyApplied
                           : rejectInvalidLocked();
  m_bodyCompleted = true;
  m_bodySucceeded = true;
  m_state = CpuSkinMtControllerState::NativeAwaitingUnlock;
  return finalizeRouteAfterUnlockLocked();
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::completeOriginalFault(
    uint64_t generation) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  materializeAtomicRouteLocked();
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
  if (m_route != CpuSkinMtControllerKernelRoute::Native ||
      (m_state != CpuSkinMtControllerState::NativeSelected &&
       m_state != CpuSkinMtControllerState::NativeAwaitingUnlock))
    return rejectInvalidLocked();
  if (m_bodyCompleted)
    return !m_bodySucceeded ? CpuSkinMtControllerResult::AlreadyApplied
                            : rejectInvalidLocked();
  m_bodyCompleted = true;
  m_bodySucceeded = false;
  m_state = CpuSkinMtControllerState::NativeAwaitingUnlock;
  return finalizeRouteAfterUnlockLocked();
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::noteUnlock(
    uint64_t generation,
    const CpuSkinMtControllerUnlockProof& unlock) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  if (m_terminal == CpuSkinMtControllerTerminal::ResetCancelled)
    return rejectStaleLocked();
  materializeAtomicRouteLocked();
  if (!CpuSkinMtControllerUnlockMatchesDestination(
          m_key.destination, unlock))
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
    // Unlock won before the guarded callback. Mark the body failed now so a
    // later callback cannot write into an allocation whose lock is gone.
    if (!m_bodyCompleted) {
      m_bodyCompleted = true;
      m_bodySucceeded = false;
    }
    return finalizeRouteAfterUnlockLocked();
  }
  if (m_route == CpuSkinMtControllerKernelRoute::Native) {
    // The original kernel must report its synchronous body result before the
    // exact Unlock. An Unlock-first observation cannot later be upgraded by a
    // delayed success notification.
    if (!m_bodyCompleted) {
      m_bodyCompleted = true;
      m_bodySucceeded = false;
    }
    return finalizeRouteAfterUnlockLocked();
  }
  return finishLocked(CpuSkinMtControllerState::OuterCancelled,
                      CpuSkinMtControllerTerminal::OuterCancelled);
}

CpuSkinMtControllerResult CpuSkinMtControllerJob::cancelTemplateMismatch(
    uint64_t generation) noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation != m_activeGeneration)
    return rejectStaleLocked();
  if (isTerminalLocked())
    return rejectDoubleTerminalLocked();
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
  m_kernelWindowOpen.store(false, std::memory_order_release);
  m_activeGeneration = nextGeneration;
  m_activeGenerationAtomic.store(nextGeneration,
                                 std::memory_order_release);
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

bool CpuSkinMtControllerJob::producerReady() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  const CpuSkinMtControllerKernelRoute authority =
      m_routeAuthority.load(std::memory_order_acquire);
  return bool(m_output) &&
      authority != CpuSkinMtControllerKernelRoute::Native &&
      authority != CpuSkinMtControllerKernelRoute::Cancelled;
}

bool CpuSkinMtControllerJob::unlockObserved() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_unlockObserved;
}

CpuSkinMtControllerExactKey CpuSkinMtControllerJob::exactKey() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_key;
}

CpuSkinMtControllerOutputProof
CpuSkinMtControllerJob::outputProof() const noexcept {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_output ? m_output->proof() : CpuSkinMtControllerOutputProof{};
}

}  // namespace dxvk::war3::gpu_skin
