#pragma once

#include <atomic>
#include <cstdint>

namespace dxvk::war3 {

// The world counts as live when its RenderScene finished in the frame being
// built or in the one presented just before it. Game logic (and with it every
// JASS thread) runs between a Present and the next world scene, so a runner
// entered mid-game always sees a distance of 0 or 1. A real map change shows
// loading-screen frames first, which have no world scene and push the
// distance past the slack.
inline constexpr uint64_t kWar3WorldLiveFrameSlack = 1u;

constexpr bool War3IsWorldSceneLive(bool everRendered, uint64_t frameSerial,
                                    uint64_t lastWorldSceneSerial) noexcept {
  return everRendered && frameSerial >= lastWorldSceneSerial &&
         frameSerial - lastWorldSceneSerial <= kWar3WorldLiveFrameSlack;
}

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

  // The runner is not only the map's `main`: 1.27a re-enters it whenever a
  // JASS thread resumes (melee AI scripts do so several times per second).
  // Such an entry happens while the world is still being drawn and is not a
  // map transition; it must neither request a reset nor disarm the fallback
  // for the real transition that follows later.
  bool onMainRunnerEnter(bool worldLive = false) noexcept {
    if (worldLive)
      return false;
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
