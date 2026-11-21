#pragma once

// Snow weather: the one implementation of the BeforeUi particle draw, shared
// by the in-process pass (d3d9_war3_weather.cpp) and the x64 host.

#include "war3_effect_frame.h"

#include "../render/war3_owned_image_layout.h"
#include "../render/war3_tracked_vk_pipeline.h"
#include "../render/war3_weather_resource_retry.h"

#include "../../../dxvk/dxvk_device.h"

namespace dxvk::war3::effects {

// Where a run ended. Values are part of the diagnostics surface: append only.
enum class SnowStage : uint32_t {
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

// One frame of snow: scene::Snow plus the frame its producer captured it in.
// In process the three identities come from the weather runtime's snapshot
// and must equal the frame's; a scene packet is one frame by construction.
struct SnowFrame {
  scene::Snow snow;
  uint64_t frameSerial = 0;
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
};

// Describes command recording, not GPU completion or visual acceptance.
struct SnowResult {
  SnowStage stage = SnowStage::Disabled;
  uint32_t depthWidth = 0;
  uint32_t depthHeight = 0;
  uint32_t activeParticles = 0; // CPU estimate before frustum/depth/region tests.
  uint64_t resourceBytes = 0;   // the owned depth copy, when one is needed
  bool submitted = false;       // cmdDraw recorded
};

class SnowExecutor {
public:
  static constexpr uint32_t kMaxParticles = 16384;
  static constexpr uint64_t kMaxDepthCopyBytes = 32u * 1024u * 1024u;

  explicit SnowExecutor(Rc<DxvkDevice> device);

  // Records the draw into ctx, or leaves ctx untouched and says why not.
  SnowResult run(const Rc<DxvkCommandList>& ctx, const EffectFrame& frame,
                 const SnowFrame& snow, EffectRecording* recording = nullptr);
  // The effect was turned off: drop the per-target resources.
  void release();
  uint64_t resourceBytes() const { return m_depthCopyBytes; }

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
  Rc<render::War3TrackedVkPipeline> m_pipeline;
  VkFormat m_pipelineFormat = VK_FORMAT_UNDEFINED;
  Rc<DxvkSampler> m_depthSampler;
  Rc<DxvkImageView> m_depthSampleView;
  Rc<DxvkImage> m_depthCopy;
  Rc<DxvkImageView> m_depthCopyView;
  render::War3OwnedImageLayoutState m_depthCopyLayout;
  uint64_t m_depthCopyBytes = 0;
  render::WeatherResourceRetry m_resourceRetry;
  uint64_t m_lastFrameSerial = 0;
  uint64_t m_lastMapEpoch = 0;
  uint64_t m_lastDeviceEpoch = 0;
};

} // namespace dxvk::war3::effects
