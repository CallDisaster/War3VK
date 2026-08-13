#include "../war3_cpu_readable_buffer_span.h"
#include "../war3_coherent_up_index_trim_contract.h"
#include "../war3_coherent_real_index_trim_contract.h"
#include "../war3_current_up_shadow_replay_contract.h"
#include "../war3_exact_index_domain_observer_cache.h"

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

bool TestExactIndexDomainRebase() {
  const uint16_t indices16[] = {9u, 4u, 7u, 9u, 5u, 7u};
  const auto span16 = BuildWar3CpuReadableBufferSpan(
      ValidInput(indices16, sizeof(indices16), 0u, sizeof(indices16)));
  uint16_t rebased16[6] = {};
  CHECK(RebaseWar3ExactIndexDomain(
      span16, 2u, 6u, 4u, 9u, rebased16, sizeof(rebased16)));
  const uint16_t expected16[] = {5u, 0u, 3u, 5u, 1u, 3u};
  CHECK(std::memcmp(rebased16, expected16, sizeof(expected16)) == 0);

  const uint32_t indices32[] = {101u, 104u, 103u};
  const auto span32 = BuildWar3CpuReadableBufferSpan(
      ValidInput(indices32, sizeof(indices32), 0u, sizeof(indices32)));
  uint32_t rebased32[3] = {};
  CHECK(RebaseWar3ExactIndexDomain(
      span32, 4u, 3u, 101u, 104u, rebased32, sizeof(rebased32)));
  const uint32_t expected32[] = {0u, 3u, 2u};
  CHECK(std::memcmp(rebased32, expected32, sizeof(expected32)) == 0);

  CHECK(!RebaseWar3ExactIndexDomain(
      span16, 2u, 6u, 5u, 9u, rebased16, sizeof(rebased16)));
  CHECK(!RebaseWar3ExactIndexDomain(
      span16, 2u, 6u, 4u, 9u, rebased16, sizeof(rebased16) - 1u));
  return true;
}

bool TestCoherentCurrentUpIndexTrimContract() {
  War3CoherentUpIndexTrimInput input = {};
  input.indexed = true;
  input.currentPositionUpload = true;
  input.currentIndexUpload = true;
  input.samePinnedAllocation = true;
  input.hasPositionBytes = true;
  input.hasIndexBytes = true;
  input.positionUploadBytes = 16u * 32u;
  input.positionSliceBytes = 16u * 32u;
  input.positionStride = 16u;
  input.indexUploadBytes = 12u;
  input.indexSliceBytes = 12u;
  input.indexElementBytes = 2u;
  input.indexCount = 6u;
  input.firstIndex = 0u;
  const auto accepted = EvaluateWar3CoherentUpIndexTrim(input);
  CHECK(accepted);
  CHECK(accepted.indexBytes == 12u);
  CHECK(accepted.positionCapacity == 32u);

  auto rejected = input;
  rejected.samePinnedAllocation = false;
  CHECK(!EvaluateWar3CoherentUpIndexTrim(rejected));
  rejected = input;
  rejected.firstIndex = 1u;
  CHECK(EvaluateWar3CoherentUpIndexTrim(rejected).rejectReason ==
        War3CoherentUpIndexTrimRejectReason::NonZeroUploadedFirstIndex);
  rejected = input;
  rejected.indexUploadBytes = 10u;
  CHECK(EvaluateWar3CoherentUpIndexTrim(rejected).rejectReason ==
        War3CoherentUpIndexTrimRejectReason::IndexRangeOutsideUpload);
  rejected = input;
  rejected.positionUploadBytes = 15u;
  CHECK(EvaluateWar3CoherentUpIndexTrim(rejected).rejectReason ==
        War3CoherentUpIndexTrimRejectReason::PositionRangeOutsideUpload);

  CHECK(ParseWar3CoherentUpIndexTrimMode(0u) ==
        War3CoherentUpIndexTrimMode::Off);
  CHECK(ParseWar3CoherentUpIndexTrimMode(1u) ==
        War3CoherentUpIndexTrimMode::Observe);
  CHECK(ParseWar3CoherentUpIndexTrimMode(2u) ==
        War3CoherentUpIndexTrimMode::Consume);
  CHECK(ParseWar3CoherentUpIndexTrimMode(3u) ==
        War3CoherentUpIndexTrimMode::Off);
  return true;
}

bool TestCurrentUpPositionReplayContract() {
  War3CurrentUpPositionReplayInput input = {};
  input.currentPositionUpload = true;
  input.hasPinnedAllocation = true;
  input.hasUploadBuffer = true;
  input.sameBuffer = true;
  input.uploadOffset = 4096u;
  input.uploadLength = 16384u;
  input.replayOffset = 6144u;
  input.replayLength = 8192u;
  const auto accepted = EvaluateWar3CurrentUpPositionReplay(input);
  CHECK(accepted);
  CHECK(accepted.replayOffset == 6144u);
  CHECK(accepted.replayLength == 8192u);

  auto rejected = input;
  rejected.sameBuffer = false;
  CHECK(EvaluateWar3CurrentUpPositionReplay(rejected).rejectReason ==
        War3CurrentUpPositionReplayRejectReason::BufferMismatch);
  rejected = input;
  rejected.replayOffset = 2048u;
  CHECK(EvaluateWar3CurrentUpPositionReplay(rejected).rejectReason ==
        War3CurrentUpPositionReplayRejectReason::RangeOutsideUpload);
  rejected = input;
  rejected.replayOffset = UINT64_MAX - 4u;
  rejected.replayLength = 16u;
  CHECK(EvaluateWar3CurrentUpPositionReplay(rejected).rejectReason ==
        War3CurrentUpPositionReplayRejectReason::RangeOutsideUpload);

  CHECK(ParseWar3CurrentUpShadowReplayMode(0u) ==
        War3CurrentUpShadowReplayMode::Off);
  CHECK(ParseWar3CurrentUpShadowReplayMode(1u) ==
        War3CurrentUpShadowReplayMode::Observe);
  CHECK(ParseWar3CurrentUpShadowReplayMode(2u) ==
        War3CurrentUpShadowReplayMode::Consume);
  CHECK(ParseWar3CurrentUpShadowReplayMode(3u) ==
        War3CurrentUpShadowReplayMode::Off);
  return true;
}

bool TestCoherentCurrentRealIndexTrimContract() {
  War3CoherentRealIndexTrimInput input = {};
  input.indexedTerrain = true;
  input.dynamicRealPosition = true;
  input.currentPositionSpan = true;
  input.currentIndexSpan = true;
  input.positionSpanBytes = 512u * 1024u;
  input.positionStride = 32u;
  input.indexSpanBytes = 12u;
  input.indexElementBytes = 2u;
  input.indexCount = 6u;
  const auto accepted = EvaluateWar3CoherentRealIndexTrim(input);
  CHECK(accepted);
  CHECK(accepted.indexBytes == 12u);
  CHECK(accepted.positionCapacity == 16384u);

  auto rejected = input;
  rejected.vertexBlendEnabled = true;
  CHECK(EvaluateWar3CoherentRealIndexTrim(rejected).rejectReason ==
        War3CoherentRealIndexTrimRejectReason::NotRigidOpaque);
  rejected = input;
  rejected.alphaTestEnabled = true;
  CHECK(EvaluateWar3CoherentRealIndexTrim(rejected).rejectReason ==
        War3CoherentRealIndexTrimRejectReason::NotRigidOpaque);
  rejected = input;
  rejected.dynamicRealPosition = false;
  CHECK(EvaluateWar3CoherentRealIndexTrim(rejected).rejectReason ==
        War3CoherentRealIndexTrimRejectReason::NotDynamicRealPosition);
  rejected = input;
  rejected.currentPositionSpan = false;
  CHECK(EvaluateWar3CoherentRealIndexTrim(rejected).rejectReason ==
        War3CoherentRealIndexTrimRejectReason::MissingCurrentPositionSpan);
  rejected = input;
  rejected.currentIndexSpan = false;
  CHECK(EvaluateWar3CoherentRealIndexTrim(rejected).rejectReason ==
        War3CoherentRealIndexTrimRejectReason::MissingCurrentIndexSpan);
  rejected = input;
  rejected.indexSpanBytes = 10u;
  CHECK(EvaluateWar3CoherentRealIndexTrim(rejected).rejectReason ==
        War3CoherentRealIndexTrimRejectReason::IndexRangeOutsideSpan);

  CHECK(ParseWar3CoherentRealIndexTrimMode(0u) ==
        War3CoherentRealIndexTrimMode::Off);
  CHECK(ParseWar3CoherentRealIndexTrimMode(1u) ==
        War3CoherentRealIndexTrimMode::Observe);
  CHECK(ParseWar3CoherentRealIndexTrimMode(2u) ==
        War3CoherentRealIndexTrimMode::Consume);
  CHECK(ParseWar3CoherentRealIndexTrimMode(3u) ==
        War3CoherentRealIndexTrimMode::Off);
  return true;
}

War3ExactIndexDomainObserverKey ObserverKey(uint64_t contentGeneration) {
  War3ExactIndexDomainObserverKey key = {};
  key.mapEpoch = 3u;
  key.deviceEpoch = 5u;
  key.ownerIdentity = 0x1010u;
  key.spanDataIdentity = 0x2020u;
  key.identityGeneration = 7u;
  key.allocationGeneration = 11u;
  key.contentGeneration = contentGeneration;
  key.spanLength = 24u;
  key.indexElementBytes = 2u;
  key.indexCount = 12u;
  key.baseVertex = -4;
  key.vertexCapacity = 256u;
  return key;
}

bool TestExactIndexDomainObserverCacheIdentity() {
  War3ExactIndexDomainObserverCache<1u, 16u> cache;
  const auto key = ObserverKey(13u);
  War3ExactIndexVertexDomain domain = {};
  domain.firstVertex = 2u;
  domain.vertexCount = 9u;
  domain.minIndex = 6u;
  domain.maxIndex = 14u;
  domain.valid = true;
  CHECK(cache.store(key, domain) ==
        War3ExactIndexDomainObserverStore::Inserted);

  War3ExactIndexVertexDomain observed = {};
  CHECK(cache.lookup(key, observed) ==
        War3ExactIndexDomainObserverLookup::Hit);
  CHECK(observed.valid && observed.firstVertex == domain.firstVertex &&
        observed.vertexCount == domain.vertexCount &&
        observed.minIndex == domain.minIndex &&
        observed.maxIndex == domain.maxIndex);

  auto expectMiss = [&](War3ExactIndexDomainObserverKey changed) {
    War3ExactIndexVertexDomain ignored = {};
    return cache.lookup(changed, ignored) !=
        War3ExactIndexDomainObserverLookup::Hit;
  };
  auto changed = key;
  changed.mapEpoch++;
  CHECK(expectMiss(changed));
  changed = key;
  changed.deviceEpoch++;
  CHECK(expectMiss(changed));
  changed = key;
  changed.ownerIdentity++;
  CHECK(expectMiss(changed));
  changed = key;
  changed.spanDataIdentity++;
  CHECK(expectMiss(changed));
  changed = key;
  changed.identityGeneration++;
  CHECK(expectMiss(changed));
  changed = key;
  changed.allocationGeneration++;
  CHECK(expectMiss(changed));
  changed = key;
  changed.contentGeneration++;
  CHECK(expectMiss(changed));
  changed = key;
  changed.spanLength += 2u;
  CHECK(expectMiss(changed));
  changed = key;
  changed.indexElementBytes = 4u;
  CHECK(expectMiss(changed));
  changed = key;
  changed.indexCount++;
  CHECK(expectMiss(changed));
  changed = key;
  changed.baseVertex++;
  CHECK(expectMiss(changed));
  changed = key;
  changed.vertexCapacity++;
  CHECK(expectMiss(changed));

  auto invalid = key;
  invalid.mapEpoch = 0u;
  CHECK(cache.lookup(invalid, observed) ==
        War3ExactIndexDomainObserverLookup::InvalidKey);
  CHECK(cache.store(invalid, domain) ==
        War3ExactIndexDomainObserverStore::InvalidKey);
  return true;
}

bool TestExactIndexDomainObserverCacheDeterministicReplacement() {
  War3ExactIndexDomainObserverCache<1u, 2u> cache;
  const auto first = ObserverKey(101u);
  const auto second = ObserverKey(103u);
  const auto third = ObserverKey(107u);
  War3ExactIndexVertexDomain domain = {};
  domain.valid = true;
  domain.vertexCount = 1u;

  CHECK(cache.store(first, domain) ==
        War3ExactIndexDomainObserverStore::Inserted);
  CHECK(cache.store(second, domain) ==
        War3ExactIndexDomainObserverStore::Inserted);
  War3ExactIndexVertexDomain observed = {};
  CHECK(cache.lookup(first, observed) ==
        War3ExactIndexDomainObserverLookup::Hit);
  CHECK(cache.store(third, domain) ==
        War3ExactIndexDomainObserverStore::Replaced);
  CHECK(cache.lookup(first, observed) ==
        War3ExactIndexDomainObserverLookup::Hit);
  CHECK(cache.lookup(third, observed) ==
        War3ExactIndexDomainObserverLookup::Hit);
  CHECK(cache.lookup(second, observed) ==
        War3ExactIndexDomainObserverLookup::MissCollision);

  War3ExactIndexVertexDomain invalidDomain = {};
  const auto invalidDomainKey = ObserverKey(109u);
  CHECK(cache.store(invalidDomainKey, invalidDomain) ==
        War3ExactIndexDomainObserverStore::Replaced);
  observed.valid = true;
  CHECK(cache.lookup(invalidDomainKey, observed) ==
        War3ExactIndexDomainObserverLookup::Hit);
  CHECK(!observed.valid);
  return true;
}

}  // namespace

int main() {
  if (!TestGuardPageAndExactAllocationRange() ||
      !TestInvalidBindingOffsetAndLogicalLength() ||
      !TestGenerationAndCpuReadabilityGates() ||
      !TestCurrentUpBytesAndAddressOverflow() || !TestDiagnostics() ||
      !TestExactIndexVertexDomain() ||
      !TestExactIndexVertexDomainBulkRead() ||
      !TestExactIndexDomainRebase() ||
      !TestCoherentCurrentUpIndexTrimContract() ||
      !TestCurrentUpPositionReplayContract() ||
      !TestCoherentCurrentRealIndexTrimContract() ||
      !TestExactIndexDomainObserverCacheIdentity() ||
      !TestExactIndexDomainObserverCacheDeterministicReplacement())
    return 1;
  std::cout << "war3_cpu_readable_buffer_span_test: PASS\n";
  return 0;
}
