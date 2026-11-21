#pragma once

// Post-processing chain: bloom (prefilter, five downsamples, five additive
// upsamples) and the composite with exposure and an optional ACES tone map.
// Shared by the in-process pass (d3d9_war3_bloom.cpp) and the x64 host. It
// replaces the D3D9-API implementation (war3/render/war3_post_process.cpp),
// whose embedded HLSL it ports (subprojects/war3fx/shaders/war3_bloom.frag),
// including its unconditional sRGB conversion: useSrgb is carried, not read,
// as it was not read there either. All images keep the target's 8-bit
// format, as the D3D9 render targets did.

#include "war3_effect_frame.h"

#include "../render/war3_owned_image_layout.h"
#include "../render/war3_tracked_vk_pipeline.h"

#include "../../../dxvk/dxvk_device.h"

#include <array>

namespace dxvk::war3::effects {

// Where a run ended. Append only.
enum class BloomStage : uint32_t {
  Disabled = 0,
  MissingTargets,
  UnsupportedSamples,
  UnsupportedTarget,
  UndefinedLayout,
  ResourceUnavailable,
  CommandsRecorded,
};

class BloomExecutor {
public:
  // Level 0 has the target's size, each further level half of the previous.
  static constexpr uint32_t kLevels = 6;

  explicit BloomExecutor(Rc<DxvkDevice> device);

  // Records the chain into ctx, or no command at all.
  BloomStage run(const Rc<DxvkCommandList>& ctx, const EffectFrame& frame,
                 const scene::Bloom& settings, EffectRecording* recording = nullptr);
  // Bytes of the scene copy and the bloom levels.
  uint64_t resourceBytes() const { return m_resourceBytes; }

private:
  bool ensurePipelines(VkFormat format);
  bool ensureTargets(VkExtent3D extent, VkFormat copyFormat, VkFormat levelFormat);

  Rc<DxvkDevice> m_device;
  Rc<DxvkSampler> m_linearSampler;
  Rc<DxvkSampler> m_pointSampler;
  const DxvkPipelineLayout* m_layout = nullptr;
  Rc<render::War3TrackedVkPipeline> m_drawPipeline;  // no blending
  Rc<render::War3TrackedVkPipeline> m_addPipeline;   // additive (upsample)
  VkFormat m_pipelineFormat = VK_FORMAT_UNDEFINED;

  Rc<DxvkImage> m_sceneCopy;
  Rc<DxvkImageView> m_sceneCopyView;
  render::War3OwnedImageLayoutState m_sceneCopyLayout;
  std::array<Rc<DxvkImage>, kLevels> m_levels;
  std::array<Rc<DxvkImageView>, kLevels> m_levelViews;
  std::array<render::War3OwnedImageLayoutState, kLevels> m_levelLayouts;
  uint64_t m_resourceBytes = 0;
};

} // namespace dxvk::war3::effects
