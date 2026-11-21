#pragma once

#include "d3d9_war3_pipeline.h"
#include "war3/effects/war3_snow_executor.h"

namespace dxvk {

class D3D9DeviceEx;

// Where the snow draw ended this frame. The draw itself is the shared executor
// (war3/effects/war3_snow_executor.h); this pass only adapts the pipeline input.
using War3WeatherDrawStage = war3::effects::SnowStage;

// Values describe command recording, not GPU completion or visual acceptance.
// No mapped GPU state is polled and no particle data is read back.
struct War3WeatherDiagnostics {
  war3::render::WeatherFrameSnapshot frozenInput = {};
  // Actual consumed camera/target domain, not the live JASS camera query.
  D3DVIEWPORT9 worldViewport = {};
  uint32_t depthWidth = 0;
  uint32_t depthHeight = 0;
  bool cameraFrameMatches = false;
  War3WeatherDrawStage stage = War3WeatherDrawStage::Disabled;
  uint32_t capacity = 16384;
  uint32_t activeParticles = 0; // CPU estimate before frustum/depth/region tests.
  // Current extra particle/image backing in this pass. Pipeline/sampler driver
  // allocations and Froxel textures are not included in this field.
  uint64_t resourceBytes = 0;
  uint64_t frameSerial = 0;
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
  int32_t id = 0;
  double timeSeconds = 0.0;
  bool submitted = false; // cmdDraw recorded; not a GPU-success assertion.
};

War3WeatherDiagnostics QueryWar3WeatherDiagnostics();
// Pure mapping, kept inline so diagnostics code needs no symbol from this pass.
inline const char* War3WeatherDrawStageName(War3WeatherDrawStage stage) noexcept {
  switch (stage) {
    case War3WeatherDrawStage::Disabled: return "disabled";
    case War3WeatherDrawStage::NoSnowfall: return "no-snowfall";
    case War3WeatherDrawStage::MissingTargets: return "missing-targets";
    case War3WeatherDrawStage::EpochMismatch: return "epoch-mismatch";
    case War3WeatherDrawStage::CameraUnavailable: return "camera-unavailable";
    case War3WeatherDrawStage::CameraInvalid: return "camera-invalid";
    case War3WeatherDrawStage::UnsupportedSamples: return "unsupported-msaa";
    case War3WeatherDrawStage::UnsupportedTarget: return "unsupported-color";
    case War3WeatherDrawStage::UnsupportedDepth: return "unsupported-depth";
    case War3WeatherDrawStage::DepthCopyBudget: return "depth-copy-budget";
    case War3WeatherDrawStage::DepthCopyUnavailable: return "depth-copy-unavailable";
    case War3WeatherDrawStage::UndefinedLayout: return "undefined-layout";
    case War3WeatherDrawStage::PipelineUnavailable: return "pipeline-unavailable";
    case War3WeatherDrawStage::DuplicateFrame: return "duplicate-frame";
    case War3WeatherDrawStage::CommandsRecorded: return "commands-recorded";
    case War3WeatherDrawStage::ResourceRetryWait: return "resource-retry-wait";
    case War3WeatherDrawStage::ResourceAllocationFailed: return "resource-allocation-failed";
    case War3WeatherDrawStage::ResourceHostAllocationFailed: return "resource-host-allocation-failed";
  }
  return "unknown";
}

class War3WeatherPass final : public War3RenderPass {
public:
  static constexpr uint32_t kMaxParticles = war3::effects::SnowExecutor::kMaxParticles;
  static constexpr uint64_t kMaxDepthCopyBytes = war3::effects::SnowExecutor::kMaxDepthCopyBytes;

  explicit War3WeatherPass(D3D9DeviceEx* parent);
  ~War3WeatherPass() override = default;

  War3InsertionPoint Point() const override {
    return War3InsertionPoint::BeforeUi;
  }
  void Run(const Rc<DxvkCommandList>& ctx,
           const War3PipelineInput& input) override;

private:
  war3::effects::SnowExecutor m_executor;
  bool m_wasEnabled = false;
};

} // namespace dxvk
