#include "d3d9_war3_weather.h"
#include "war3/effects/war3_effect_scene_input.h"
#include "d3d9_device.h"
#include "d3d9_war3_effect_adapter.h"
#include "war3/tools/war3_perf_monitor.h"

#include <mutex>

namespace dxvk {
namespace {

std::mutex s_diagnosticsMutex;
War3WeatherDiagnostics s_diagnostics;

void PublishDiagnostics(const War3WeatherDiagnostics& value) {
  std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
  s_diagnostics = value;
}

// The executor's record: what the draw reads from the frozen snapshot.
war3::effects::SnowFrame SnowFrameOf(
    const war3::render::WeatherFrameSnapshot& weather) {
  war3::effects::SnowFrame frame;
  frame.snow = war3::effects::SnowFromWeather(weather);
  frame.frameSerial = weather.frameSerial;
  frame.mapEpoch = weather.mapEpoch;
  frame.deviceEpoch = weather.deviceEpoch;
  return frame;
}

// GPU timing covers exactly the recorded commands, as before the executor
// was shared: a run that ends early opens no scope.
struct SnowPerfScope final : war3::effects::EffectRecording {
  war3::War3PerfMonitor::ScopedSection section;
  void beginRecording(const Rc<DxvkCommandList>& ctx) override {
    section = war3::War3PerfMonitor::instance().scope("Weather/Snow", ctx);
  }
};

} // namespace

War3WeatherDiagnostics QueryWar3WeatherDiagnostics() {
  std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
  return s_diagnostics;
}

War3WeatherPass::War3WeatherPass(D3D9DeviceEx* parent)
: m_executor(parent->GetDXVKDevice()) { }

void War3WeatherPass::Run(const Rc<DxvkCommandList>& ctx,
                         const War3PipelineInput& input) {
  const auto& weather = input.weather;
  if (!weather.enabled || weather.id == 0) {
    // An unused weather system neither creates GPU objects nor takes the
    // diagnostics lock per frame. Publish only an actual enabled->off edge.
    if (m_wasEnabled) {
      War3WeatherDiagnostics disabled;
      disabled.frozenInput = weather;
      disabled.frameSerial = input.frameSerial;
      disabled.mapEpoch = input.mapEpoch;
      disabled.deviceEpoch = input.deviceEpoch;
      disabled.id = weather.id;
      disabled.timeSeconds = weather.timeSeconds;
      PublishDiagnostics(disabled);
      m_wasEnabled = false;
      m_executor.release();
    }
    return;
  }
  m_wasEnabled = true;
  War3WeatherDiagnostics diagnostics;
  diagnostics.frozenInput = weather;
  diagnostics.frameSerial = input.frameSerial;
  diagnostics.mapEpoch = input.mapEpoch;
  diagnostics.deviceEpoch = input.deviceEpoch;
  diagnostics.id = weather.id;
  diagnostics.timeSeconds = weather.timeSeconds;
  diagnostics.worldViewport = input.scene.worldCamera.viewport;
  diagnostics.cameraFrameMatches = input.scene.worldCamera.valid &&
    input.scene.worldCamera.frameSerial == input.frameSerial;

  SnowPerfScope perfScope;
  const auto result = m_executor.run(ctx, War3EffectFrame(input),
                                     SnowFrameOf(weather), &perfScope);
  diagnostics.stage = result.stage;
  diagnostics.depthWidth = result.depthWidth;
  diagnostics.depthHeight = result.depthHeight;
  diagnostics.resourceBytes = result.resourceBytes;
  diagnostics.activeParticles = result.activeParticles;
  diagnostics.submitted = result.submitted;
  PublishDiagnostics(diagnostics);
}

} // namespace dxvk
