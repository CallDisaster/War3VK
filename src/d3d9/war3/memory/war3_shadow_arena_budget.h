#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace dxvk::war3::memory {

constexpr uint64_t kShadowArenaFixedResidentLimitBytes =
    1152ull * 1024ull * 1024ull;
constexpr uint64_t kShadowArenaMemoryReserveBytes =
    512ull * 1024ull * 1024ull;
constexpr uint32_t kShadowArenaBudgetFractionNumerator = 1u;
constexpr uint32_t kShadowArenaBudgetFractionDenominator = 4u;

struct ShadowArenaMemoryBudgetInput {
  bool extensionSupported = false;
  bool primaryDeviceLocalHeapFound = false;
  uint64_t heapSizeBytes = 0u;
  uint64_t heapBudgetBytes = 0u;
  uint64_t heapAllocatedBytes = 0u;
  uint64_t fixedResidentLimitBytes =
      kShadowArenaFixedResidentLimitBytes;
  uint64_t fixedReserveBytes = kShadowArenaMemoryReserveBytes;
};

struct ShadowArenaMemoryBudgetPolicy {
  bool supported = false;
  bool trusted = false;
  uint64_t heapSizeBytes = 0u;
  uint64_t heapBudgetBytes = 0u;
  uint64_t heapAllocatedBytes = 0u;
  uint64_t availableBytes = 0u;
  uint64_t proportionalLimitBytes = 0u;
  uint64_t reserveLimitBytes = 0u;
  uint64_t fixedResidentLimitBytes = 0u;
  uint64_t effectiveResidentLimitBytes = 0u;
};

/**
 * Resolves the maximum total logical Shadow Arena residency.
 *
 * A trustworthy VK_EXT_memory_budget snapshot clamps the legacy fixed cap by
 * both one quarter of currently available primary VRAM and the bytes left
 * after preserving a fixed 512 MiB reserve. Missing or malformed extension
 * data preserves the legacy cap; it never increases it. An over-budget but
 * otherwise valid snapshot is trustworthy pressure and therefore resolves to
 * zero new residency rather than being mistaken for unsupported data.
 */
inline ShadowArenaMemoryBudgetPolicy ResolveShadowArenaMemoryBudget(
    const ShadowArenaMemoryBudgetInput& input) noexcept {
  ShadowArenaMemoryBudgetPolicy result = {};
  result.supported = input.extensionSupported;
  result.heapSizeBytes = input.heapSizeBytes;
  result.heapBudgetBytes = input.heapBudgetBytes;
  result.heapAllocatedBytes = input.heapAllocatedBytes;
  result.fixedResidentLimitBytes = input.fixedResidentLimitBytes;
  result.effectiveResidentLimitBytes = input.fixedResidentLimitBytes;

  const bool saneSnapshot = input.extensionSupported &&
      input.primaryDeviceLocalHeapFound && input.heapSizeBytes != 0u &&
      input.heapBudgetBytes != 0u &&
      input.heapBudgetBytes <= input.heapSizeBytes;
  if (!saneSnapshot)
    return result;

  result.trusted = true;
  result.availableBytes = input.heapBudgetBytes > input.heapAllocatedBytes
      ? input.heapBudgetBytes - input.heapAllocatedBytes
      : 0u;
  result.proportionalLimitBytes =
      (result.availableBytes / kShadowArenaBudgetFractionDenominator) *
      kShadowArenaBudgetFractionNumerator;
  result.reserveLimitBytes = result.availableBytes > input.fixedReserveBytes
      ? result.availableBytes - input.fixedReserveBytes
      : 0u;
  result.effectiveResidentLimitBytes = std::min(
      {result.fixedResidentLimitBytes, result.proportionalLimitBytes,
       result.reserveLimitBytes});
  return result;
}

inline bool ShadowArenaCanGrowResident(
    uint64_t residentBytes, uint64_t growthBytes,
    uint64_t effectiveResidentLimitBytes) noexcept {
  return growthBytes <= effectiveResidentLimitBytes &&
      residentBytes <= effectiveResidentLimitBytes - growthBytes;
}

inline uint64_t ShadowArenaGrowthHeadroom(
    uint64_t residentBytes, uint64_t effectiveResidentLimitBytes) noexcept {
  return residentBytes < effectiveResidentLimitBytes
      ? effectiveResidentLimitBytes - residentBytes
      : 0u;
}

} // namespace dxvk::war3::memory
