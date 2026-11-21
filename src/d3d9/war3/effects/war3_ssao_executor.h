#pragma once

// Screen-space ambient occlusion: the one implementation of the BeforeUi
// composite, shared by the in-process pass (d3d9_war3_ssao.cpp) and the x64
// host. It copies the colour and depth targets, then draws one fullscreen
// triangle over the colour target that reads both copies.

#include "war3_effect_frame.h"

#include "../render/war3_owned_image_layout.h"
#include "../render/war3_tracked_vk_pipeline.h"

#include "../../../dxvk/dxvk_device.h"

namespace dxvk::war3::effects {

// Where a run ended. Append only.
enum class SsaoStage : uint32_t {
  Disabled = 0,
  MissingTargets,
  CameraUnavailable,
  UnsupportedSamples,
  UnsupportedTarget,
  UnsupportedDepth,
  UndefinedLayout,
  ResourceUnavailable,
  CommandsRecorded,
};

class SsaoExecutor {
public:
  explicit SsaoExecutor(Rc<DxvkDevice> device);

  // Records the copies and the draw into ctx, or leaves ctx untouched.
  SsaoStage run(const Rc<DxvkCommandList>& ctx, const EffectFrame& frame,
                const scene::Ssao& settings, EffectRecording* recording = nullptr);
  // Bytes of the two owned copies.
  uint64_t resourceBytes() const { return m_resourceBytes; }

private:
  bool ensurePipeline(VkFormat format);
  bool ensureCopies(VkExtent3D extent, VkFormat colorFormat, VkFormat depthFormat);

  Rc<DxvkDevice> m_device;
  Rc<DxvkSampler> m_linearSampler;
  const DxvkPipelineLayout* m_layout = nullptr;
  Rc<render::War3TrackedVkPipeline> m_pipeline;
  VkFormat m_pipelineFormat = VK_FORMAT_UNDEFINED;

  Rc<DxvkImage> m_colorCopy;
  Rc<DxvkImageView> m_colorCopyView;
  render::War3OwnedImageLayoutState m_colorCopyLayout;
  Rc<DxvkImage> m_depthCopy;
  Rc<DxvkImageView> m_depthCopyView;
  render::War3OwnedImageLayoutState m_depthCopyLayout;
  uint64_t m_resourceBytes = 0;
};

} // namespace dxvk::war3::effects
