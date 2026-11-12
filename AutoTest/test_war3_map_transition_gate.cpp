#include "../src/d3d9/war3/core/war3_map_transition_gate.h"

#include <cassert>
#include <cstdint>

int main() {
  using dxvk::war3::War3IsWorldSceneLive;
  using dxvk::war3::kWar3WorldLiveFrameSlack;

  dxvk::war3::War3MapTransitionGate gate;

  // First process launch: native initialization is not a map transition.
  assert(!gate.onNativeTableRebuild());
  assert(!gate.onNativeTableRebuild());
  assert(!gate.onMainRunnerEnter());
  gate.onMainRunnerComplete();

  // A -> B: three native-table calls must retire the A session only once.
  assert(gate.onNativeTableRebuild());
  assert(!gate.onNativeTableRebuild());
  assert(!gate.onNativeTableRebuild());
  assert(!gate.onMainRunnerEnter());
  gate.onMainRunnerComplete();

  // B -> A: a missing native-table callback still has a runner fallback.
  assert(gate.onMainRunnerEnter());
  assert(!gate.onMainRunnerEnter());
  gate.onMainRunnerComplete();

  // An explicit leave disarms the fallback; no second retirement is needed.
  gate.onGameExit();
  assert(!gate.onNativeTableRebuild());
  assert(!gate.onMainRunnerEnter());
  gate.onMainRunnerComplete();
  assert(gate.onNativeTableRebuild());

  // --- JASS threads resuming mid-game -----------------------------------
  // 1.27a re-enters the runner for every resumed thread (melee AI scripts
  // do it several times per second). While the world is live none of these
  // entries may request a reset, however many there are.
  {
    dxvk::war3::War3MapTransitionGate live;
    assert(!live.onMainRunnerEnter(false)); // the map's own main
    live.onMainRunnerComplete();
    for (int i = 0; i < 1000; ++i) {
      assert(!live.onMainRunnerEnter(true));
      live.onMainRunnerComplete();
    }
    // ...and they must not have disarmed the real transition that follows:
    // either signal still closes the generation exactly once.
    assert(live.onNativeTableRebuild());
    assert(!live.onMainRunnerEnter(false));
    live.onMainRunnerComplete();

    for (int i = 0; i < 1000; ++i) {
      assert(!live.onMainRunnerEnter(true));
      live.onMainRunnerComplete();
    }
    // B -> A without a native-table callback: the loading screen ended the
    // live window, so the runner fallback still fires once.
    assert(live.onMainRunnerEnter(false));
    assert(!live.onMainRunnerEnter(false));
    live.onMainRunnerComplete();

    // A live-world entry never consumes an explicit leave either.
    live.onGameExit();
    assert(!live.onMainRunnerEnter(true));
    assert(!live.onMainRunnerEnter(false));
  }

  // --- what "live" means -------------------------------------------------
  // Never rendered a world scene: nothing is live (hook absent, main menu).
  assert(!War3IsWorldSceneLive(false, 0u, 0u));
  assert(!War3IsWorldSceneLive(false, 100u, 100u));
  // Logic runs after the Present that followed the world scene (distance 1)
  // or, with several world scenes per presented frame, in the same frame.
  assert(War3IsWorldSceneLive(true, 100u, 100u));
  assert(War3IsWorldSceneLive(true, 101u, 100u));
  // A single loading-screen (or full-screen menu) frame ends the window.
  assert(!War3IsWorldSceneLive(true, 102u, 100u));
  assert(!War3IsWorldSceneLive(true, 5000u, 100u));
  for (uint64_t d = 0u; d < 64u; ++d) {
    assert(War3IsWorldSceneLive(true, 1000u + d, 1000u) ==
           (d <= kWar3WorldLiveFrameSlack));
  }
  // A stamp from the future is inconsistent, not live.
  assert(!War3IsWorldSceneLive(true, 99u, 100u));
  // No wrap-around surprise at the top of the range.
  assert(War3IsWorldSceneLive(true, UINT64_MAX, UINT64_MAX));
  assert(War3IsWorldSceneLive(true, UINT64_MAX, UINT64_MAX - 1u));
  assert(!War3IsWorldSceneLive(true, UINT64_MAX, 0u));
  static_assert(War3IsWorldSceneLive(true, 8u, 7u));
  static_assert(!War3IsWorldSceneLive(true, 9u, 7u));
  return 0;
}
