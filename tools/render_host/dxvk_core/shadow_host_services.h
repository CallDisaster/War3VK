#pragma once
// The shadow pass (src/d3d9/d3d9_war3_shadow*.cpp, d3d9_war3_csm.cpp) is one
// source built into both the x86 device and this host. In the device it reads
// a few facts of the x86 data layer and reports to its diagnostics through
// process-wide functions. Here the same functions are defined by
// shadow_host_services.cpp: diagnostics do nothing, and the facts are the ones
// the last ShadowFrame record carried, published before the pass runs. A frame
// without the record leaves them as they were: the data layer did not change
// because a record is missing.
#include <cstdint>
#include <array>

namespace warvk::host::dxvkcore::shadowhost {

// Before the first record they are what the x86 data layer starts with
// (war3_render_state.cpp g_gameTime, war3_shadow_lifecycle.cpp g_eventSerial
// and g_stagePolicyRevision).
struct DataLayerFacts {
  // War3RenderState::GetGameTime(): the game's time of day in hours; outside
  // 0..24 while the game has none.
  float gameTime = -1.0f;
  // war3::render::CurrentShadowCasterTombstoneSerial() and
  // CurrentShadowStagePolicyRevision(): a change invalidates what the pass
  // kept from earlier frames.
  uint64_t tombstoneSerial = 0, stagePolicyRevision = 1;
  // war3::runtime: the modules the x86 runtime profile disabled, one bit per
  // War3RuntimeModule.
  uint32_t runtimeDisabledModules = 0;
  // war3::state::RenderState::getFrameIndex().
  uint32_t frameIndex = 0;
};

// Host owner thread, before the pass runs. BeginFrame also advances the
// frame number the pass keys its once-per-frame uploads on.
void Publish(const DataLayerFacts& facts) noexcept;
void BeginFrame() noexcept;
DataLayerFacts Current() noexcept;

struct CasterBoundsCounts {
  uint32_t exact = 0, unknown = 0;
  std::array<uint32_t, 4> removed{};
  uint32_t complete = 0, held = 0, unavailable = 0;
  uint32_t omittedExact = 0, omittedUnknown = 0;
  uint64_t admittedBytes = 0;
  uint64_t casterVertexBytes = 0, casterIndexBytes = 0, casterIndexReusedBytes = 0;
  uint64_t timingId = 0, completedTimingId = 0;
  double shadowMapGpuUs = 0, shadowReceiverGpuUs = 0;
};

// Owner-thread handoff at Present, including frames without BeforeUi.
// The recorder copies these counts before it submits work to the writer.
void PublishPresentedBounds(const CasterBoundsCounts& counts) noexcept;
CasterBoundsCounts PresentedBounds() noexcept;

} // namespace warvk::host::dxvkcore::shadowhost
