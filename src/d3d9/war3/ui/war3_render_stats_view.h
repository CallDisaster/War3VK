#pragma once
#include <cstdint>

namespace dxvk::war3::ui {
// No sampling at all while collapsed/hidden: the caller invokes this only
// inside Render Stats. Reset on device/UI shutdown; first tick may be zero.
class RenderStatsRefresh {
public:
  bool due(uint64_t nowMs) noexcept {
    if (m_valid && nowMs >= m_lastMs && nowMs - m_lastMs < 250u)
      return false;
    m_valid = true;
    m_lastMs = nowMs;
    return true;
  }
  void reset() noexcept { m_valid = false; m_lastMs = 0; }
private:
  bool m_valid = false;
  uint64_t m_lastMs = 0;
};
constexpr double BytesToMiB(uint64_t bytes) noexcept {
  return static_cast<double>(bytes) / (1024.0 * 1024.0);
}
constexpr float BudgetFraction(uint64_t bytes, uint64_t budget) noexcept {
  return budget == 0 ? 0.0f : bytes >= budget ? 1.0f
      : static_cast<float>(static_cast<double>(bytes) / static_cast<double>(budget));
}
}
