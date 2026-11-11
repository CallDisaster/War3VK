#pragma once

#include <cstdint>

namespace dxvk::war3::render {

// Only resource-creation inputs belong here. Author parameters, weather ids,
// revisions and ordinary frame changes must not defeat a failed-key cooldown.
struct WeatherResourceRetryKey {
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t colorFormat = 0;
  uint32_t depthFormat = 0;
};

// One owner-thread slot, not a resource owner or a cross-thread coordinator.
// A different key discards the remembered failure; returning to an earlier key
// is a new attempt. A caller must clear() after successful resource creation.
class WeatherResourceRetry {
public:
  using Key = WeatherResourceRetryKey;
  static constexpr uint64_t kCooldownFrames = 120;

  bool ready(const Key& key, uint64_t frameSerial) noexcept {
    if (!m_hasFailure)
      return true;
    if (!sameKey(key, m_failedKey)) {
      clear();
      return true;
    }
    // Compare before subtraction: rollback and uint64 wrap cannot make the
    // elapsed interval look huge. Near UINT64_MAX this deliberately fails
    // closed until clear() or a resource-key change, rather than wrapping.
    return frameSerial >= m_failedFrame
        && frameSerial - m_failedFrame >= kCooldownFrames;
  }

  void failed(const Key& key, uint64_t frameSerial) noexcept {
    if (!m_hasFailure || !sameKey(key, m_failedKey)) {
      m_failedKey = key;
      m_failedFrame = frameSerial;
      m_hasFailure = true;
      return;
    }
    // A retry that genuinely fails restarts its cooldown. A backward report
    // cannot shorten an already remembered failure interval.
    if (frameSerial > m_failedFrame)
      m_failedFrame = frameSerial;
  }

  void clear() noexcept {
    m_failedKey = {};
    m_failedFrame = 0;
    m_hasFailure = false;
  }

private:
  static bool sameKey(const Key& a, const Key& b) noexcept {
    return a.mapEpoch == b.mapEpoch
        && a.deviceEpoch == b.deviceEpoch
        && a.width == b.width
        && a.height == b.height
        && a.colorFormat == b.colorFormat
        && a.depthFormat == b.depthFormat;
  }

  Key m_failedKey = {};
  uint64_t m_failedFrame = 0;
  bool m_hasFailure = false;
};

} // namespace dxvk::war3::render
