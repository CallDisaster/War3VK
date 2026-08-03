#include "war3_cpu_readable_buffer_span.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>

namespace dxvk::war3::memory {

namespace {

constexpr size_t kRejectReasonCount =
    size_t(War3CpuReadableSpanRejectReason::AddressOverflow) + 1u;

std::atomic<uint64_t> g_acceptedCount{0u};
std::atomic<uint64_t> g_rejectedCount{0u};
std::array<std::atomic<uint64_t>, kRejectReasonCount> g_rejectCounts = {};
std::atomic<uint64_t> g_lastRejectReason{0u};
std::atomic<uint64_t> g_lastAllocationBytes{0u};
std::atomic<uint64_t> g_lastRequestedOffset{0u};
std::atomic<uint64_t> g_lastRequestedBytes{0u};
std::atomic<uint64_t> g_lastOwnerIdentity{0u};
std::atomic<uint64_t> g_lastIdentityGeneration{0u};
std::atomic<uint64_t> g_lastAllocationGeneration{0u};
std::atomic<uint64_t> g_lastContentGeneration{0u};

std::atomic<uint64_t> g_exactIndexDomainScannedBytes{0u};
std::atomic<uint64_t> g_exactIndexDomainNonHostCachedScanCount{0u};
std::atomic<uint64_t> g_exactIndexDomainNonHostCachedScannedBytes{0u};
std::atomic<uint64_t> g_exactIndexDomainBulkReadCount{0u};
std::atomic<uint64_t> g_exactIndexDomainBulkReadBytes{0u};
std::atomic<uint64_t> g_exactIndexDomainDirectReadCount{0u};
std::atomic<uint64_t> g_exactIndexDomainOversizeFallbackCount{0u};

constexpr size_t kExactIndexDomainBulkReadCapacity = 64u * 1024u;
alignas(64) thread_local std::array<uint8_t,
                                    kExactIndexDomainBulkReadCapacity>
    t_exactIndexDomainBulkRead = {};

void RecordExactIndexDomainScan(
    const War3ExactIndexDomainScanInput& input) noexcept {
  const uint64_t requiredBytes =
      uint64_t(input.indexElementBytes) * uint64_t(input.indexCount);
  g_exactIndexDomainScannedBytes.fetch_add(requiredBytes,
                                           std::memory_order_relaxed);
  if (!input.sourceHostCached) {
    g_exactIndexDomainNonHostCachedScanCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_exactIndexDomainNonHostCachedScannedBytes.fetch_add(
        requiredBytes, std::memory_order_relaxed);
  }
}

War3ExactIndexVertexDomain ComputeExactIndexDomainPreparedImpl(
    const War3ExactIndexDomainScanInput& input) noexcept {
  if (!input.indices ||
      (input.indexElementBytes != 2u && input.indexElementBytes != 4u) ||
      input.indexCount == 0u || input.vertexCapacity == 0u)
    return {};
  const uint64_t requiredBytes =
      uint64_t(input.indexElementBytes) * uint64_t(input.indexCount);
  if (requiredBytes > input.indices.length)
    return {};

  RecordExactIndexDomainScan(input);
  if (input.bulkReadEnabled && !input.sourceHostCached &&
      requiredBytes <= kExactIndexDomainBulkReadCapacity) {
    // Warcraft's WRITEONLY IB mappings are commonly write-combined.  Scalar
    // 16-bit loads from those pages are disproportionately expensive.  One
    // bounded sequential copy turns the subsequent min/max walk into cached
    // reads while preserving the exact validated source generation.
    std::memcpy(t_exactIndexDomainBulkRead.data(), input.indices.data,
                size_t(requiredBytes));
    auto staged = input.indices;
    staged.data = t_exactIndexDomainBulkRead.data();
    staged.length = requiredBytes;
    g_exactIndexDomainBulkReadCount.fetch_add(1u,
                                              std::memory_order_relaxed);
    g_exactIndexDomainBulkReadBytes.fetch_add(requiredBytes,
                                              std::memory_order_relaxed);
    return ComputeWar3ExactIndexVertexDomain(
        staged, input.indexElementBytes, input.indexCount,
        input.baseVertex, input.vertexCapacity);
  }

  if (input.bulkReadEnabled && !input.sourceHostCached &&
      requiredBytes > kExactIndexDomainBulkReadCapacity) {
    g_exactIndexDomainOversizeFallbackCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  g_exactIndexDomainDirectReadCount.fetch_add(1u,
                                              std::memory_order_relaxed);
  return ComputeWar3ExactIndexVertexDomain(
      input.indices, input.indexElementBytes, input.indexCount,
      input.baseVertex, input.vertexCapacity);
}

War3CpuReadableBufferSpan Reject(
    const War3CpuReadableBufferSpanInput& input,
    War3CpuReadableSpanRejectReason reason) noexcept {
  War3CpuReadableBufferSpan result = {};
  result.ownerIdentity = input.ownerIdentity;
  result.identityGeneration = input.identityGeneration;
  result.allocationGeneration = input.allocationGeneration;
  result.contentGeneration = input.contentGeneration;
  result.rejectReason = reason;
  g_rejectedCount.fetch_add(1u, std::memory_order_relaxed);
  const size_t reasonIndex = size_t(reason);
  if (reasonIndex < g_rejectCounts.size())
    g_rejectCounts[reasonIndex].fetch_add(1u, std::memory_order_relaxed);
  g_lastRejectReason.store(uint64_t(reason), std::memory_order_release);
  g_lastAllocationBytes.store(input.allocationBytes, std::memory_order_release);
  g_lastRequestedOffset.store(input.requestedOffset,
                              std::memory_order_release);
  g_lastRequestedBytes.store(input.requestedBytes,
                             std::memory_order_release);
  g_lastOwnerIdentity.store(uint64_t(input.ownerIdentity),
                            std::memory_order_release);
  g_lastIdentityGeneration.store(input.identityGeneration,
                                 std::memory_order_release);
  g_lastAllocationGeneration.store(input.allocationGeneration,
                                   std::memory_order_release);
  g_lastContentGeneration.store(input.contentGeneration,
                                std::memory_order_release);
  return result;
}

uint64_t RejectCount(War3CpuReadableSpanRejectReason reason) noexcept {
  const size_t index = size_t(reason);
  return index < g_rejectCounts.size()
      ? g_rejectCounts[index].load(std::memory_order_acquire) : 0u;
}

}  // namespace

War3CpuReadableBufferSpan BuildWar3CpuReadableBufferSpan(
    const War3CpuReadableBufferSpanInput& input) noexcept {
  if (input.allocationBase == nullptr)
    return Reject(input, War3CpuReadableSpanRejectReason::NullBase);
  if (!input.cpuReadable)
    return Reject(input, War3CpuReadableSpanRejectReason::NotCpuReadable);
  if (input.ownerIdentity == 0u)
    return Reject(input, War3CpuReadableSpanRejectReason::MissingOwner);
  if (input.identityGeneration == 0u ||
      input.allocationGeneration == 0u || input.contentGeneration == 0u)
    return Reject(input, War3CpuReadableSpanRejectReason::MissingGeneration);
  if (input.requestedOffset > input.allocationBytes)
    return Reject(input,
                  War3CpuReadableSpanRejectReason::OffsetOutsideAllocation);
  if (input.requestedBytes == 0u ||
      input.requestedBytes > input.allocationBytes - input.requestedOffset)
    return Reject(input,
                  War3CpuReadableSpanRejectReason::LengthOutsideAllocation);

  const uintptr_t base = reinterpret_cast<uintptr_t>(input.allocationBase);
  if (input.requestedOffset >
      uint64_t(std::numeric_limits<uintptr_t>::max() - base))
    return Reject(input, War3CpuReadableSpanRejectReason::AddressOverflow);

  War3CpuReadableBufferSpan result = {};
  result.data = reinterpret_cast<const uint8_t*>(
      base + uintptr_t(input.requestedOffset));
  result.length = input.requestedBytes;
  result.ownerIdentity = input.ownerIdentity;
  result.identityGeneration = input.identityGeneration;
  result.allocationGeneration = input.allocationGeneration;
  result.contentGeneration = input.contentGeneration;
  result.rejectReason = War3CpuReadableSpanRejectReason::None;
  g_acceptedCount.fetch_add(1u, std::memory_order_relaxed);
  return result;
}

War3CpuReadableSpanDiagnostics QueryWar3CpuReadableSpanDiagnostics() noexcept {
  War3CpuReadableSpanDiagnostics result = {};
  result.acceptedCount = g_acceptedCount.load(std::memory_order_acquire);
  result.rejectedCount = g_rejectedCount.load(std::memory_order_acquire);
  result.nullBaseRejectCount =
      RejectCount(War3CpuReadableSpanRejectReason::NullBase);
  result.notCpuReadableRejectCount =
      RejectCount(War3CpuReadableSpanRejectReason::NotCpuReadable);
  result.missingOwnerRejectCount =
      RejectCount(War3CpuReadableSpanRejectReason::MissingOwner);
  result.missingGenerationRejectCount =
      RejectCount(War3CpuReadableSpanRejectReason::MissingGeneration);
  result.offsetOutsideAllocationRejectCount =
      RejectCount(War3CpuReadableSpanRejectReason::OffsetOutsideAllocation);
  result.lengthOutsideAllocationRejectCount =
      RejectCount(War3CpuReadableSpanRejectReason::LengthOutsideAllocation);
  result.addressOverflowRejectCount =
      RejectCount(War3CpuReadableSpanRejectReason::AddressOverflow);
  result.lastRejectReason = g_lastRejectReason.load(std::memory_order_acquire);
  result.lastAllocationBytes =
      g_lastAllocationBytes.load(std::memory_order_acquire);
  result.lastRequestedOffset =
      g_lastRequestedOffset.load(std::memory_order_acquire);
  result.lastRequestedBytes =
      g_lastRequestedBytes.load(std::memory_order_acquire);
  result.lastOwnerIdentity =
      g_lastOwnerIdentity.load(std::memory_order_acquire);
  result.lastIdentityGeneration =
      g_lastIdentityGeneration.load(std::memory_order_acquire);
  result.lastAllocationGeneration =
      g_lastAllocationGeneration.load(std::memory_order_acquire);
  result.lastContentGeneration =
      g_lastContentGeneration.load(std::memory_order_acquire);
  result.exactIndexDomainScannedBytes =
      g_exactIndexDomainScannedBytes.load(std::memory_order_acquire);
  result.exactIndexDomainNonHostCachedScanCount =
      g_exactIndexDomainNonHostCachedScanCount.load(
          std::memory_order_acquire);
  result.exactIndexDomainNonHostCachedScannedBytes =
      g_exactIndexDomainNonHostCachedScannedBytes.load(
          std::memory_order_acquire);
  result.exactIndexDomainBulkReadCount =
      g_exactIndexDomainBulkReadCount.load(std::memory_order_acquire);
  result.exactIndexDomainBulkReadBytes =
      g_exactIndexDomainBulkReadBytes.load(std::memory_order_acquire);
  result.exactIndexDomainDirectReadCount =
      g_exactIndexDomainDirectReadCount.load(std::memory_order_acquire);
  result.exactIndexDomainOversizeFallbackCount =
      g_exactIndexDomainOversizeFallbackCount.load(
          std::memory_order_acquire);
  return result;
}

War3ExactIndexVertexDomain ComputeWar3ExactIndexVertexDomain(
    const War3CpuReadableBufferSpan& indices, uint32_t indexElementBytes,
    uint32_t indexCount, int32_t baseVertex,
    uint32_t vertexCapacity) noexcept {
  War3ExactIndexVertexDomain result = {};
  if (!indices || (indexElementBytes != 2u && indexElementBytes != 4u) ||
      indexCount == 0u || vertexCapacity == 0u)
    return result;

  const uint64_t requiredBytes =
      uint64_t(indexElementBytes) * uint64_t(indexCount);
  if (requiredBytes > indices.length)
    return result;

  uint32_t minIndex = std::numeric_limits<uint32_t>::max();
  uint32_t maxIndex = 0u;
  int64_t firstVertex = std::numeric_limits<int64_t>::max();
  int64_t lastVertex = -1;
  for (uint32_t i = 0u; i < indexCount; ++i) {
    uint32_t index = 0u;
    if (indexElementBytes == 2u) {
      uint16_t value = 0u;
      std::memcpy(&value, indices.data + size_t(i) * 2u, sizeof(value));
      index = uint32_t(value);
    } else {
      std::memcpy(&index, indices.data + size_t(i) * 4u, sizeof(index));
    }

    const int64_t vertex = int64_t(index) + int64_t(baseVertex);
    if (vertex < 0 || vertex >= int64_t(vertexCapacity))
      return {};
    minIndex = std::min(minIndex, index);
    maxIndex = std::max(maxIndex, index);
    firstVertex = std::min(firstVertex, vertex);
    lastVertex = std::max(lastVertex, vertex);
  }

  if (firstVertex < 0 || lastVertex < firstVertex ||
      uint64_t(lastVertex - firstVertex + 1) >
          uint64_t(std::numeric_limits<uint32_t>::max()))
    return result;

  result.firstVertex = uint32_t(firstVertex);
  result.vertexCount = uint32_t(lastVertex - firstVertex + 1);
  result.minIndex = minIndex;
  result.maxIndex = maxIndex;
  result.valid = result.vertexCount != 0u;
  return result;
}

War3ExactIndexVertexDomain ComputeWar3ExactIndexVertexDomainPrepared(
    const War3ExactIndexDomainScanInput& input) noexcept {
  return ComputeExactIndexDomainPreparedImpl(input);
}

}  // namespace dxvk::war3::memory
