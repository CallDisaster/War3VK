// Host-only proof for three small render contracts (1.22.03):
//  - the world-lighting scope that keeps WarVK's sun/ambient rewrites off
//    UI-owned 3D models (unit portrait, cursor, menu scenes);
//  - the BeforeUi boundary at the end of the main world scene, which holds in
//    paused frames too, and the frame-end fallback that must not run once
//    something was drawn after the world;
//  - the directional guide resolution of the volumetric shafts.
// No game, Vulkan device or window is involved.
#include "../src/d3d9/war3/render/war3_before_ui_boundary.h"
#include "../src/d3d9/war3/render/war3_volumetric_guide_divisor.h"
#include "../src/d3d9/war3/render/war3_world_lighting_scope.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace dxvk::war3::render;

static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

// ---------------------------------------------------------------------------
// World lighting scope
// ---------------------------------------------------------------------------

// The contract is usable in constant expressions: the device evaluates it on
// every SetLight/SetMaterial/SetRenderState(AMBIENT).
static_assert(War3WorldLightingActive(War3WorldLightingScope::Unscoped));
static_assert(War3WorldLightingActive(War3WorldLightingScope::World));
static_assert(!War3WorldLightingActive(War3WorldLightingScope::Outside));

// A model of the device: it stores the game's own value and derives the
// effective one from the scope, re-deriving only when the scope says so.
struct LightingModel {
  War3WorldLightingScope scope = War3WorldLightingScope::Unscoped;
  int pure = 0;        // what the game last set
  int effective = 0;   // what the fixed-function pipeline sees
  unsigned reapplies = 0;

  static int Rewrite(int pure) { return pure + 1000; }

  void Set(int value) {
    pure = value;
    effective = War3WorldLightingActive(scope) ? Rewrite(value) : value;
  }
  void Switch(bool inWorld) {
    const auto next = War3WorldLightingScopeFor(inWorld);
    const auto previous = scope;
    scope = next;
    if (War3WorldLightingScopeNeedsReapply(previous, next)) {
      ++reapplies;
      effective = War3WorldLightingActive(scope) ? Rewrite(pure) : pure;
    }
  }
};

static void TestLightingScopeTable() {
  CHECK(War3WorldLightingScopeFor(true) == War3WorldLightingScope::World);
  CHECK(War3WorldLightingScopeFor(false) == War3WorldLightingScope::Outside);

  const War3WorldLightingScope all[] = {
      War3WorldLightingScope::Unscoped, War3WorldLightingScope::World,
      War3WorldLightingScope::Outside};
  for (auto from : all) {
    for (auto to : all) {
      const bool flips =
          War3WorldLightingActive(from) != War3WorldLightingActive(to);
      CHECK(War3WorldLightingScopeNeedsReapply(from, to) == flips);
    }
    CHECK(!War3WorldLightingScopeNeedsReapply(from, from));
  }
  // The first world entry must not cost a reapply: nothing changes for a
  // device that was already rewriting.
  CHECK(!War3WorldLightingScopeNeedsReapply(War3WorldLightingScope::Unscoped,
                                            War3WorldLightingScope::World));
  CHECK(War3WorldLightingScopeNeedsReapply(War3WorldLightingScope::World,
                                           War3WorldLightingScope::Outside));
  CHECK(War3WorldLightingScopeNeedsReapply(War3WorldLightingScope::Outside,
                                           War3WorldLightingScope::World));
}

static void TestLightingHookNeverFires() {
  // Without the world-boundary hook the device keeps the old behaviour.
  LightingModel m;
  m.Set(7);
  CHECK(m.effective == 1007);
  m.Set(8);
  CHECK(m.effective == 1008);
  CHECK(m.reapplies == 0);
}

static void TestLightingFrame() {
  LightingModel m;
  m.Set(1);                 // loading screen, hook not seen yet
  CHECK(m.effective == 1001);

  m.Switch(true);           // world begins
  CHECK(m.reapplies == 0);
  CHECK(m.effective == 1001);
  m.Set(2);                 // the world's own light
  CHECK(m.effective == 1002);

  m.Switch(false);          // world ends: the same value turns pure again,
  CHECK(m.reapplies == 1);  // even though the game does not re-send it
  CHECK(m.effective == 2);
  m.Set(3);                 // the portrait's light is left alone
  CHECK(m.effective == 3);

  m.Switch(true);           // next frame: the portrait's value must not leak
  CHECK(m.reapplies == 2);  // into the world unrewritten
  CHECK(m.effective == 1003);
  m.Set(4);
  CHECK(m.effective == 1004);

  m.Switch(true);           // duplicate notifications are harmless
  CHECK(m.reapplies == 2);
  m.Switch(false);
  m.Switch(false);
  CHECK(m.reapplies == 3);
  CHECK(m.effective == 4);
}

// ---------------------------------------------------------------------------
// BeforeUi boundary at the end of the world scene
// ---------------------------------------------------------------------------

static_assert(kWar3BeforeUiWorldSceneEndTier == 10u);

static War3WorldSceneBoundaryInput Boundary(bool completed, bool active,
                                            bool inWorld, bool input,
                                            bool inserted) {
  War3WorldSceneBoundaryInput in;
  in.sceneCompleted = completed;
  in.sceneActive = active;
  in.inWorldThisFrame = inWorld;
  in.haveWorldInput = input;
  in.alreadyInserted = inserted;
  return in;
}

static void TestWorldSceneEndBoundary() {
  // The one accepting row: scene returned, world seen, inputs captured.
  CHECK(War3IsWorldSceneEndBoundary(Boundary(true, false, true, true, false)));

  // Every single missing condition rejects.
  CHECK(!War3IsWorldSceneEndBoundary(Boundary(false, false, true, true, false)));
  CHECK(!War3IsWorldSceneEndBoundary(Boundary(true, true, true, true, false)));
  CHECK(!War3IsWorldSceneEndBoundary(Boundary(true, false, false, true, false)));
  CHECK(!War3IsWorldSceneEndBoundary(Boundary(true, false, true, false, false)));
  CHECK(!War3IsWorldSceneEndBoundary(Boundary(true, false, true, true, true)));

  // Exhaustive: exactly one of the 32 rows accepts.
  unsigned accepted = 0;
  for (unsigned bits = 0; bits < 32u; ++bits) {
    if (War3IsWorldSceneEndBoundary(Boundary(bits & 1u, bits & 2u, bits & 4u,
                                             bits & 8u, bits & 16u)))
      ++accepted;
  }
  CHECK(accepted == 1);

  // A draw inside the scene never is the boundary, whatever the stages say:
  // a paused frame dispatches no stage 15/18/21 at all.
  CHECK(!War3IsWorldSceneEndBoundary(Boundary(false, true, true, true, false)));
  // A default input (hook never fired) keeps the older tiers in charge.
  CHECK(!War3IsWorldSceneEndBoundary(War3WorldSceneBoundaryInput{}));
}

static void TestFrameEndFallback() {
  // Interface hidden: nothing was drawn after the world, the fallback is the
  // only place the passes can run.
  CHECK(War3FrameEndFallbackAllowed(true, 0u, false));
  // Something was drawn after the world and the boundary did not commit:
  // running now would put the effects on top of it.
  CHECK(!War3FrameEndFallbackAllowed(true, 1u, false));
  CHECK(!War3FrameEndFallbackAllowed(true, 269u, false));
  // No scene hook: nothing is known, keep the previous behaviour.
  CHECK(War3FrameEndFallbackAllowed(false, 0u, false));
  CHECK(War3FrameEndFallbackAllowed(false, 269u, false));
  // The A/B switch restores the previous behaviour everywhere.
  CHECK(War3FrameEndFallbackAllowed(true, 269u, true));
}

// A model of one frame: draws arrive in order, the passes run once.
struct FrameModel {
  bool sceneCompleted = false;
  bool sceneActive = false;
  bool inWorld = false;
  bool inserted = false;
  unsigned drawsAfterWorld = 0; // drawn after the world without a commit

  void EnterScene() { sceneActive = true; sceneCompleted = false; inWorld = true; }
  void ExitScene() { sceneActive = false; sceneCompleted = true; }
  void Draw(bool canCommit = true) {
    if (!inserted && War3IsWorldSceneEndBoundary(
            Boundary(sceneCompleted, sceneActive, inWorld, true, inserted)) &&
        canCommit) {
      inserted = true; // passes run before this draw
      return;
    }
    if (!inserted && sceneCompleted && !sceneActive)
      ++drawsAfterWorld;
  }
  void Present() {
    if (!inserted &&
        War3FrameEndFallbackAllowed(sceneCompleted, drawsAfterWorld, false))
      inserted = true;
  }
};

static void TestFrameShapes() {
  { // running or paused frame with HUD: commit at the first draw after the world
    FrameModel f;
    f.EnterScene();
    for (int i = 0; i < 300; ++i) f.Draw();
    CHECK(!f.inserted);
    f.ExitScene();
    f.Draw();
    CHECK(f.inserted);
    for (int i = 0; i < 268; ++i) f.Draw();
    f.Present();
    CHECK(f.inserted);
    CHECK(f.drawsAfterWorld == 0);
  }
  { // interface hidden: nothing after the world, the fallback applies the passes
    FrameModel f;
    f.EnterScene();
    for (int i = 0; i < 300; ++i) f.Draw();
    f.ExitScene();
    f.Present();
    CHECK(f.inserted);
  }
  { // the commit was impossible: the fallback must stay out
    FrameModel f;
    f.EnterScene();
    f.ExitScene();
    for (int i = 0; i < 5; ++i) f.Draw(false);
    f.Present();
    CHECK(!f.inserted);
    CHECK(f.drawsAfterWorld == 5);
  }
}

// ---------------------------------------------------------------------------
// Volumetric directional guide resolution
// ---------------------------------------------------------------------------

static void TestGuideDivisor() {
  constexpr uint64_t kBudget = 100'000'000ull;
  constexpr uint32_t kHighDepth = 128u;
  constexpr uint32_t kMediumDepth = 64u;

  // 1080p High: half resolution fits (960x540x128 = 66.4M), as before.
  CHECK(War3SelectDirectionalGuideDivisor(1920, 1080, kHighDepth, 2, 4, kBudget) == 2);
  // 1440p High: half resolution is 117.9M cells; the old all-or-nothing rule
  // fell back to the effect resolution (1/8). A third fits (52.5M).
  CHECK(War3SelectDirectionalGuideDivisor(2560, 1440, kHighDepth, 2, 8, kBudget) == 3);
  // 4K High: 1/2 = 265M, 1/3 = 117.9M, 1/4 = 66.4M.
  CHECK(War3SelectDirectionalGuideDivisor(3840, 2160, kHighDepth, 2, 8, kBudget) == 4);
  // Medium at 1440p already fitted at half resolution.
  CHECK(War3SelectDirectionalGuideDivisor(2560, 1440, kMediumDepth, 2, 8, kBudget) == 2);

  // The chosen guide never exceeds the budget and is never coarser than the
  // effect; nothing finer than the target is ever returned.
  const uint32_t widths[] = {640, 1280, 1366, 1600, 1902, 1920, 2560, 3440, 3840, 5120, 7680};
  const uint32_t heights[] = {360, 720, 768, 900, 963, 1080, 1440, 2160, 2880, 4320};
  for (uint32_t w : widths) {
    for (uint32_t h : heights) {
      for (uint32_t effect : {4u, 8u, 16u}) {
        for (uint32_t depth : {kMediumDepth, kHighDepth}) {
          const uint32_t d = War3SelectDirectionalGuideDivisor(w, h, depth, 2, effect, kBudget);
          CHECK(d >= 2u && d <= effect);
          if (d < effect) {
            const uint64_t cells = uint64_t((w + d - 1) / d) * ((h + d - 1) / d) * depth;
            CHECK(cells <= kBudget);
            if (d > 2u) { // d is the finest that fits
              const uint32_t finer = d - 1u;
              const uint64_t finerCells =
                  uint64_t((w + finer - 1) / finer) * ((h + finer - 1) / finer) * depth;
              CHECK(finerCells > kBudget);
            }
          }
        }
      }
    }
  }

  // Degenerate requests fall back to the effect resolution.
  CHECK(War3SelectDirectionalGuideDivisor(1920, 1080, kHighDepth, 0, 4, kBudget) == 4);
  CHECK(War3SelectDirectionalGuideDivisor(1920, 1080, 0, 2, 4, kBudget) == 4);
  CHECK(War3SelectDirectionalGuideDivisor(1920, 1080, kHighDepth, 4, 4, kBudget) == 4);
  CHECK(War3SelectDirectionalGuideDivisor(1920, 1080, kHighDepth, 8, 4, kBudget) == 4);
  CHECK(War3SelectDirectionalGuideDivisor(1920, 1080, kHighDepth, 2, 4, 0) == 4);
  // No overflow at the largest extents the device can report.
  CHECK(War3SelectDirectionalGuideDivisor(0xFFFFFFFFu, 0xFFFFFFFFu, kHighDepth, 2, 8, kBudget) == 8);
}

int main() {
  TestLightingScopeTable();
  TestLightingHookNeverFires();
  TestLightingFrame();
  TestWorldSceneEndBoundary();
  TestFrameEndFallback();
  TestFrameShapes();
  TestGuideDivisor();
  std::printf("render_scope_contracts: %u checks passed\n", checks);
  return 0;
}
