#pragma once

#include <cstddef>
#include <cstdint>

namespace dxvk::war3::memory {

enum class War3CpuReadableSpanRejectReason : uint8_t {
  None = 0u,
  NullBase,
  NotCpuReadable,
  MissingOwner,
  MissingGeneration,
  OffsetOutsideAllocation,
  LengthOutsideAllocation,
  AddressOverflow,
};

struct War3CpuReadableBufferSpan {
  const uint8_t* data = nullptr;
  uint64_t length = 0u;
  uintptr_t ownerIdentity = 0u;
  uint64_t identityGeneration = 0u;
  uint64_t allocationGeneration = 0u;
  uint64_t contentGeneration = 0u;
  War3CpuReadableSpanRejectReason rejectReason =
      War3CpuReadableSpanRejectReason::NullBase;

  explicit operator bool() const {
    return data != nullptr && length != 0u && ownerIdentity != 0u &&
           identityGeneration != 0u && allocationGeneration != 0u &&
           contentGeneration != 0u &&
           rejectReason == War3CpuReadableSpanRejectReason::None;
  }
};

struct War3CpuReadableBufferSpanInput {
  const void* allocationBase = nullptr;
  uint64_t allocationBytes = 0u;
  uint64_t requestedOffset = 0u;
  uint64_t requestedBytes = 0u;
  uintptr_t ownerIdentity = 0u;
  uint64_t identityGeneration = 0u;
  uint64_t allocationGeneration = 0u;
  uint64_t contentGeneration = 0u;
  bool cpuReadable = false;
};

struct War3CpuReadableSpanDiagnostics {
  uint64_t acceptedCount = 0u;
  uint64_t rejectedCount = 0u;
  uint64_t nullBaseRejectCount = 0u;
  uint64_t notCpuReadableRejectCount = 0u;
  uint64_t missingOwnerRejectCount = 0u;
  uint64_t missingGenerationRejectCount = 0u;
  uint64_t offsetOutsideAllocationRejectCount = 0u;
  uint64_t lengthOutsideAllocationRejectCount = 0u;
  uint64_t addressOverflowRejectCount = 0u;
  uint64_t lastRejectReason = 0u;
  uint64_t lastAllocationBytes = 0u;
  uint64_t lastRequestedOffset = 0u;
  uint64_t lastRequestedBytes = 0u;
  uint64_t lastOwnerIdentity = 0u;
  uint64_t lastIdentityGeneration = 0u;
  uint64_t lastAllocationGeneration = 0u;
  uint64_t lastContentGeneration = 0u;
};

// Exact index-domain proof used to compact a current-frame shadow freeze.
// The returned range is relative to the vertex slice consumed by the draw,
// after applying the draw's signed base-vertex offset.  A caller may trim a
// vertex stream only when this proof is valid and every referenced index lies
// inside the supplied vertex capacity.
struct War3ExactIndexVertexDomain {
  uint32_t firstVertex = 0u;
  uint32_t vertexCount = 0u;
  uint32_t minIndex = 0u;
  uint32_t maxIndex = 0u;
  bool valid = false;
};

War3CpuReadableBufferSpan BuildWar3CpuReadableBufferSpan(
    const War3CpuReadableBufferSpanInput& input) noexcept;
War3CpuReadableSpanDiagnostics QueryWar3CpuReadableSpanDiagnostics() noexcept;

War3ExactIndexVertexDomain ComputeWar3ExactIndexVertexDomain(
    const War3CpuReadableBufferSpan& indices, uint32_t indexElementBytes,
    uint32_t indexCount, int32_t baseVertex,
    uint32_t vertexCapacity) noexcept;

}  // namespace dxvk::war3::memory
