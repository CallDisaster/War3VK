#pragma once

#include <cstdint>

namespace dxvk::war3::render {

struct War3PinnedUploadRange {
  uint64_t offset = 0u;
  uint64_t length = 0u;
  bool valid = false;
};

// Converts a virtual-buffer-local range into a range relative to the exact
// backing allocation retained by the producer. The caller still verifies the
// VkBuffer handle and keeps the allocation alive until every consumer has
// finished using it.
constexpr War3PinnedUploadRange MakeWar3PinnedUploadRange(
    uint64_t allocationBytes, uint64_t localOffset,
    uint64_t requestedBytes) noexcept {
  War3PinnedUploadRange result = {};
  if (requestedBytes == 0u || localOffset > allocationBytes ||
      requestedBytes > allocationBytes - localOffset)
    return result;
  result.offset = localOffset;
  result.length = requestedBytes;
  result.valid = true;
  return result;
}

} // namespace dxvk::war3::render
