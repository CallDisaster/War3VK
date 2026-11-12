#pragma once

#include <cstdint>

namespace dxvk::war3::render {

// Where the BeforeUi passes (shadow receiver, volumetric light, SSAO, bloom,
// colour grading...) are inserted into the game's frame.
//
// A 1.27a in-game frame is:
//
//   UiDispatch  WorldRenderScene{ world stages }  HUD / portrait / menus
//
// The older boundary tiers recognise the end of the world from its stages:
// "main-screen stage 21 completed and stage 15 reached". A paused game (the
// single-player game menu, PauseGame) renders the same scene with the stages
// flagged off-screen and without stages 15/18/21, so none of those tiers
// fired; the passes then ran from the frame-end fallback in Present, after
// the menu and the HUD had been drawn into the same target, and landed on top
// of them.
//
// The hooked WorldRenderScene returning is the one signal that holds in both
// kinds of frame: every draw after it is not world content.
constexpr uint32_t kWar3BeforeUiWorldSceneEndTier = 10u;

struct War3WorldSceneBoundaryInput {
  bool sceneCompleted = false;   // the hook saw WorldRenderScene return
  bool sceneActive = false;      // still inside WorldRenderScene
  bool inWorldThisFrame = false; // a world stage was dispatched this frame
  bool haveWorldInput = false;   // world RT0, depth and camera are captured
  bool alreadyInserted = false;  // BeforeUi already ran this frame
};

// True for a draw that must be preceded by the BeforeUi passes because the
// main world scene is complete.
constexpr bool War3IsWorldSceneEndBoundary(
    const War3WorldSceneBoundaryInput& in) noexcept {
  return in.sceneCompleted && !in.sceneActive && in.inWorldThisFrame &&
         in.haveWorldInput && !in.alreadyInserted;
}

// The frame-end fallback (Present) runs the passes when no draw followed the
// world, e.g. a cinematic with the interface hidden. It must not run once
// something has been drawn after the world scene: that content would end up
// under the effects. Without the scene hook nothing is known and the old
// behaviour is kept. `legacyOverride` is the A/B switch
// DXVK_WAR3_BEFOREUI_FRAME_END_AFTER_UI=1.
constexpr bool War3FrameEndFallbackAllowed(bool sceneCompleted,
                                           uint32_t drawsAfterWorldScene,
                                           bool legacyOverride) noexcept {
  return legacyOverride || !sceneCompleted || drawsAfterWorldScene == 0u;
}

} // namespace dxvk::war3::render
