#include "../src/d3d9/war3/core/war3_map_transition_gate.h"

#include <cassert>

int main() {
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
  return 0;
}
