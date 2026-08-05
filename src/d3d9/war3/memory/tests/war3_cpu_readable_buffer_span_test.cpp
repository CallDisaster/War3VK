#include "../war3_cpu_readable_buffer_span.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

namespace {

using namespace dxvk::war3::memory;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::cerr << __func__ << ':' << __LINE__                              \
                << ": CHECK failed: " #condition << '\n';                  \
      return false;                                                          \
    }                                                                        \
  } while (false)

War3CpuReadableBufferSpanInput ValidInput(
    const void* base, uint64_t allocationBytes, uint64_t offset,
    uint64_t bytes) {
  War3CpuReadableBufferSpanInput input = {};
  input.allocationBase = base;
  input.allocationBytes = allocationBytes;
  input.requestedOffset = offset;
  input.requestedBytes = bytes;
  input.ownerIdentity = 0x101u;
  input.identityGeneration = 11u;
  input.allocationGeneration = 13u;
  input.contentGeneration = 17u;
  input.cpuReadable = true;
  return input;
}

bool TestGuardPageAndExactAllocationRange() {
  SYSTEM_INFO systemInfo = {};
  GetSystemInfo(&systemInfo);
  const size_t pageBytes = systemInfo.dwPageSize;
  auto* pages = static_cast<uint8_t*>(VirtualAlloc(
      nullptr, pageBytes * 2u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  CHECK(pages != nullptr);

  DWORD oldProtection = 0u;
  CHECK(VirtualProtect(pages + pageBytes, pageBytes, PAGE_NOACCESS,
                       &oldProtection) != FALSE);
  std::memset(pages, 0x5a, pageBytes);

  const auto tail = BuildWar3CpuReadableBufferSpan(
      ValidInput(pages, pageBytes, pageBytes - 16u, 16u));
  CHECK(tail);
  uint8_t copy[16] = {};
  std::memcpy(copy, tail.data, sizeof(copy));
  CHECK(copy[0] == 0x5a && copy[15] == 0x5a);

  const auto crossesGuard = BuildWar3CpuReadableBufferSpan(
      ValidInput(pages, pageBytes, pageBytes - 8u, 16u));
  CHECK(!crossesGuard);
  CHECK(crossesGuard.rejectReason ==
        War3CpuReadableSpanRejectReason::LengthOutsideAllocation);
  CHECK(VirtualFree(pages, 0u, MEM_RELEASE) != FALSE);
  return true;
}

bool TestInvalidBindingOffsetAndLogicalLength() {
  uint8_t allocation[64] = {};
  const auto badBinding = BuildWar3CpuReadableBufferSpan(
      ValidInput(allocation, 512u * 1024u, 3u * 1024u * 1024u, 12u));
  CHECK(!badBinding);
  CHECK(badBinding.rejectReason ==
        War3CpuReadableSpanRejectReason::OffsetOutsideAllocation);

  const auto logicalLargerThanAllocation = BuildWar3CpuReadableBufferSpan(
      ValidInput(allocation, sizeof(allocation), 32u, 64u));
  CHECK(!logicalLargerThanAllocation);
  CHECK(logicalLargerThanAllocation.rejectReason ==
        War3CpuReadableSpanRejectReason::LengthOutsideAllocation);
  return true;
}

bool TestGenerationAndCpuReadabilityGates() {
  uint8_t allocation[32] = {};
  auto stale = ValidInput(allocation, sizeof(allocation), 0u, 16u);
  stale.allocationGeneration = 0u;
  const auto staleResult = BuildWar3CpuReadableBufferSpan(stale);
  CHECK(!staleResult);
  CHECK(staleResult.rejectReason ==
        War3CpuReadableSpanRejectReason::MissingGeneration);

  auto gpuOnly = ValidInput(allocation, sizeof(allocation), 0u, 16u);
  gpuOnly.cpuReadable = false;
  const auto gpuOnlyResult = BuildWar3CpuReadableBufferSpan(gpuOnly);
  CHECK(!gpuOnlyResult);
  CHECK(gpuOnlyResult.rejectReason ==
        War3CpuReadableSpanRejectReason::NotCpuReadable);
  return true;
}

bool TestCurrentUpBytesAndAddressOverflow() {
  uint8_t ownedUpBytes[48] = {};
  ownedUpBytes[9] = 0xa5u;
  const auto currentUp = BuildWar3CpuReadableBufferSpan(
      ValidInput(ownedUpBytes, sizeof(ownedUpBytes), 8u, 8u));
  CHECK(currentUp);
  CHECK(currentUp.data == ownedUpBytes + 8u);
  CHECK(currentUp.data[1] == 0xa5u);

  const uintptr_t nearEnd = std::numeric_limits<uintptr_t>::max() - 7u;
  const auto overflow = BuildWar3CpuReadableBufferSpan(ValidInput(
      reinterpret_cast<const void*>(nearEnd), 64u, 16u, 1u));
  CHECK(!overflow);
  CHECK(overflow.rejectReason ==
        War3CpuReadableSpanRejectReason::AddressOverflow);
  return true;
}

bool TestDiagnostics() {
  const auto diagnostics = QueryWar3CpuReadableSpanDiagnostics();
  CHECK(diagnostics.acceptedCount >= 2u);
  CHECK(diagnostics.rejectedCount >= 5u);
  CHECK(diagnostics.offsetOutsideAllocationRejectCount >= 1u);
  CHECK(diagnostics.lengthOutsideAllocationRejectCount >= 2u);
  CHECK(diagnostics.missingGenerationRejectCount >= 1u);
  CHECK(diagnostics.notCpuReadableRejectCount >= 1u);
  CHECK(diagnostics.addressOverflowRejectCount >= 1u);
  return true;
}

bool TestExactIndexVertexDomain() {
  const uint16_t indices16[] = {9u, 4u, 7u, 9u, 5u, 7u};
  const auto span16 = BuildWar3CpuReadableBufferSpan(
      ValidInput(indices16, sizeof(indices16), 0u, sizeof(indices16)));
  const auto domain16 = ComputeWar3ExactIndexVertexDomain(
      span16, 2u, 6u, -4, 16u);
  CHECK(domain16.valid);
  CHECK(domain16.firstVertex == 0u);
  CHECK(domain16.vertexCount == 6u);
  CHECK(domain16.minIndex == 4u);
  CHECK(domain16.maxIndex == 9u);

  const uint32_t indices32[] = {101u, 104u, 103u};
  const auto span32 = BuildWar3CpuReadableBufferSpan(
      ValidInput(indices32, sizeof(indices32), 0u, sizeof(indices32)));
  const auto domain32 = ComputeWar3ExactIndexVertexDomain(
      span32, 4u, 3u, -100, 8u);
  CHECK(domain32.valid);
  CHECK(domain32.firstVertex == 1u);
  CHECK(domain32.vertexCount == 4u);

  CHECK(!ComputeWar3ExactIndexVertexDomain(
      span16, 1u, 6u, 0, 16u).valid);
  CHECK(!ComputeWar3ExactIndexVertexDomain(
      span16, 2u, 7u, 0, 16u).valid);
  CHECK(!ComputeWar3ExactIndexVertexDomain(
      span16, 2u, 6u, -10, 16u).valid);
  CHECK(!ComputeWar3ExactIndexVertexDomain(
      span32, 4u, 3u, 0, 4u).valid);
  return true;
}

bool TestExactIndexVertexDomainBulkRead() {
  const uint16_t indices[] = {12u, 8u, 10u, 9u, 11u, 8u};
  War3ExactIndexDomainScanInput input = {};
  input.indices = BuildWar3CpuReadableBufferSpan(
      ValidInput(indices, sizeof(indices), 0u, sizeof(indices)));
  input.indexElementBytes = 2u;
  input.indexCount = 6u;
  input.baseVertex = -8;
  input.vertexCapacity = 16u;
  input.sourceHostCached = false;
  input.bulkReadEnabled = true;

  const auto before = QueryWar3CpuReadableSpanDiagnostics();
  const auto domain = ComputeWar3ExactIndexVertexDomainPrepared(input);
  const auto after = QueryWar3CpuReadableSpanDiagnostics();
  CHECK(domain.valid);
  CHECK(domain.firstVertex == 0u);
  CHECK(domain.vertexCount == 5u);
  CHECK(after.exactIndexDomainBulkReadCount ==
        before.exactIndexDomainBulkReadCount + 1u);
  CHECK(after.exactIndexDomainBulkReadBytes ==
        before.exactIndexDomainBulkReadBytes + sizeof(indices));
  CHECK(after.exactIndexDomainDirectReadCount ==
        before.exactIndexDomainDirectReadCount);
  return true;
}

}  // namespace

int main() {
  if (!TestGuardPageAndExactAllocationRange() ||
      !TestInvalidBindingOffsetAndLogicalLength() ||
      !TestGenerationAndCpuReadabilityGates() ||
      !TestCurrentUpBytesAndAddressOverflow() || !TestDiagnostics() ||
      !TestExactIndexVertexDomain() ||
      !TestExactIndexVertexDomainBulkRead())
    return 1;
  std::cout << "war3_cpu_readable_buffer_span_test: PASS\n";
  return 0;
}
