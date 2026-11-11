#pragma once

#include "d3d9_war3_pipeline.h"
#include "war3/render/war3_owned_image_layout.h"
#include "war3/render/war3_tracked_vk_pipeline.h"
#include "war3/render/war3_weather_resource_retry.h"

namespace dxvk {

class D3D9DeviceEx;

enum class War3WeatherDrawStage : uint32_t {
  Disabled = 0,
  NoSnowfall,
  MissingTargets,
  EpochMismatch,
  CameraUnavailable,
  CameraInvalid,
  UnsupportedSamples,
  UnsupportedTarget,
  UnsupportedDepth,
  DepthCopyBudget,
  DepthCopyUnavailable,
  UndefinedLayout,
  PipelineUnavailable,
  DuplicateFrame,
  CommandsRecorded,
  ResourceRetryWait,
  ResourceAllocationFailed,
  ResourceHostAllocationFailed,
};

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
const char* War3WeatherDrawStageName(War3WeatherDrawStage stage) noexcept;

class War3WeatherPass final : public War3RenderPass {
public:
  static constexpr uint32_t kMaxParticles = 16384;
  static constexpr uint64_t kMaxDepthCopyBytes = 32u * 1024u * 1024u;

  explicit War3WeatherPass(D3D9DeviceEx* parent);
  ~War3WeatherPass() override = default;

  War3InsertionPoint Point() const override {
    return War3InsertionPoint::BeforeUi;
  }
  void Run(const Rc<DxvkCommandList>& ctx,
           const War3PipelineInput& input) override;

private:
  bool ensurePipeline(VkFormat format);
  bool ensureDepthCopy(VkExtent3D extent, VkFormat format);
  void copyDepth(const Rc<DxvkCommandList>& ctx,
                 const Rc<DxvkImageView>& source,
                 const VkImageSubresourceRange& sourceRange,
                 VkImageLayout sourceOriginal);
  void clearDepthCopy();
  Rc<DxvkDevice> m_device;
  const DxvkPipelineLayout* m_layout = nullptr;
  Rc<war3::render::War3TrackedVkPipeline> m_pipeline;
  VkFormat m_pipelineFormat = VK_FORMAT_UNDEFINED;
  Rc<DxvkSampler> m_depthSampler;
  Rc<DxvkImageView> m_depthSampleView;
  Rc<DxvkImage> m_depthCopy;
  Rc<DxvkImageView> m_depthCopyView;
  war3::render::War3OwnedImageLayoutState m_depthCopyLayout;
  uint64_t m_depthCopyBytes = 0;
  war3::render::WeatherResourceRetry m_resourceRetry;
  bool m_wasEnabled = false;
  uint64_t m_lastFrameSerial = 0;
  uint64_t m_lastMapEpoch = 0;
  uint64_t m_lastDeviceEpoch = 0;
};

} // namespace dxvk
