#pragma once

#include <atomic>

namespace dxvk::war3 {

// The native table may be rebuilt several times while loading one map. A
// completed MainRunner arms the next generation; its first native-table
// rebuild (or the next MainRunner entry when no rebuild was observed) closes
// that generation exactly once. This is a lifecycle signal, not proof that
// the map's path or Game.dll address changed.
class War3MapTransitionGate final {
public:
  bool onNativeTableRebuild() noexcept {
    return m_runningGeneration.exchange(false, std::memory_order_acq_rel);
  }

  bool onMainRunnerEnter() noexcept {
    return m_runningGeneration.exchange(false, std::memory_order_acq_rel);
  }

  void onMainRunnerComplete() noexcept {
    m_runningGeneration.store(true, std::memory_order_release);
  }

  void onGameExit() noexcept {
    m_runningGeneration.store(false, std::memory_order_release);
  }

private:
  std::atomic<bool> m_runningGeneration{false};
};

} // namespace dxvk::war3
