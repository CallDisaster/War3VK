#pragma once

#include <cstdlib>
#include <cstdint>

namespace dxvk::war3::tools::evidence {

// Keep the external/explicit recorder contract unchanged.  The internal player
// candidate uses a deliberately smaller rolling window so that a 32-bit game
// can still enter asset-heavy maps while retaining the exact draw inputs needed
// to diagnose a short shadow fissure.  This is a capacity policy, not a lossy
// change to any captured draw or matrix representation.
struct RecorderProfile {
  uint32_t cpuEvents;
  uint32_t imagePreFrames;
  uint32_t imagePostFrames;
  uint32_t preMilliseconds;
  uint32_t inputSlots;
  bool highPressure;
};

inline constexpr RecorderProfile ExternalRecorderProfile{
  262144, 256, 4, 1000, 576, false
};

inline constexpr RecorderProfile InternalHighPressureRecorderProfile{
  65536, 96, 4, 1000, 224, true
};

inline constexpr RecorderProfile DefaultRecorderProfile(bool internalBuild) noexcept {
  return internalBuild ? InternalHighPressureRecorderProfile : ExternalRecorderProfile;
}

// Overrides may only shrink the SELECTED profile, not expand it to the
// external profile's limit. Reject malformed/overflowing values as a whole;
// atoi would silently accept e.g. "96garbage" and has no overflow contract.
inline uint32_t RecorderReducedFrameLimit(const char* text, uint32_t minimum,
    uint32_t selectedLimit) noexcept {
  if (!text || !*text) return selectedLimit;
  uint32_t value = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') return selectedLimit;
    const uint32_t digit = uint32_t(*p - '0');
    if (digit > selectedLimit || value > (selectedLimit - digit) / 10u)
      return selectedLimit;
    value = value * 10u + digit;
  }
  return value >= minimum ? value : selectedLimit;
}

inline RecorderProfile WithEnvOverrides(RecorderProfile profile) noexcept {
  profile.imagePreFrames = RecorderReducedFrameLimit(
      std::getenv("DXVK_WAR3_FRAME_EVIDENCE_PRE_FRAMES"), 4u, profile.imagePreFrames);
  profile.imagePostFrames = RecorderReducedFrameLimit(
      std::getenv("DXVK_WAR3_FRAME_EVIDENCE_POST_FRAMES"), 1u, profile.imagePostFrames);
  return profile;
}

} // namespace dxvk::war3::tools::evidence
