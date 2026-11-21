// war3_settings_resolver.h - from the author's settings to a frame's settings
//
// War3RenderSettings reach a frame in two steps that do not depend on where
// the frame is executed:
//   - once, the diagnostic environment overrides (DXVK_WAR3_SHADOW_*, ...);
//   - every frame, the pending edits of the settings mailbox, the lighting
//     clock, and the day/night ambient and exposure.
// The in-process device runs both from War3RenderPipeline. In proxy mode
// (platform/war3_proxy_mode.h) no pipeline exists and the data layer runs
// them for the records it sends to the x64 host. This is the one
// implementation of both.
#pragma once

#include "../../d3d9_war3_settings.h"

#include <chrono>
#include <cstdint>

namespace dxvk {

// d3d9_war3_pipeline.h
struct War3RenderSettingsMailbox;

// The DXVK_WAR3_* tuning overrides, over the defaults and the player's saved
// settings. DXVK_WAR3_TEST_POINT_LIGHT also creates its light here.
void ApplyWar3SettingsEnvironmentOverrides(War3RenderSettings& settings);

// The settings half of a frame start. Owner (render) thread only.
class War3SettingsFrameResolver {
public:
  // Takes the mailbox's pending settings when a newer revision waits, then
  // advances the lighting clock into dayNight.renderTimeHours.
  void BeginFrame(War3RenderSettingsMailbox& mailbox, War3RenderSettings& settings);
  // Day/night ambient and exposure. The resolved settings then become the
  // base of the next authoring edit, unless one arrived meanwhile.
  void FinishFrame(War3RenderSettingsMailbox& mailbox, War3RenderSettings& settings);

private:
  bool m_authoredExposureChanged = false;
  float m_lastAutoExposure = 0.0f;
  bool m_hasAutoExposure = false;
  uint64_t m_lightingClockRevision = 0u;
  float m_lightingClockTime01 = 0.5f;
  std::chrono::steady_clock::time_point m_lightingClockLastUpdate;
};

} // namespace dxvk
