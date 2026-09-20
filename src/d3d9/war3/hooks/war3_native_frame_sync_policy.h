#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace dxvk::war3::hooks {
// Experimental native caller policy, not a D3D LockRect implementation.
// Unknown argument values, nested calls and real pixel consumers stay native.
constexpr bool CanElideNativeFrameSync(bool enabled, bool recordingOwner,
    bool readable, bool nested, int32_t request, uint32_t capture) noexcept {
  return enabled && recordingOwner && readable && !nested &&
         request == 1 && capture == 0;
}
// Normalize only frozen IMAGE_REL_BASED_HIGHLOW operands in a local COPY.
// Full-function digest validation follows; this never writes loaded code.
inline bool NormalizeNativeFrameCode(uint8_t* copy, size_t size, uint32_t delta,
    const size_t* offsets, size_t count) noexcept {
  if (!copy || size<4 || (!offsets && count)) return false;
  for (size_t i=0;i<count;++i) {
    if (offsets[i]>size-4 || (i && offsets[i]<offsets[i-1]+4)) return false;
    uint32_t value=0;
    std::memcpy(&value,copy+offsets[i],4);
    value-=delta;
    std::memcpy(copy+offsets[i],&value,4);
  }
  return true;
}
}
