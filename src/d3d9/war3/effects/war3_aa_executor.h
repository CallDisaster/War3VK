#pragma once

// Anti-aliasing: the one implementation of the BeforeUi AA pass, shared by the
// in-process pass (d3d9_war3_aa.cpp) and the x64 host. It copies the colour
// target and redraws it from the copy, either with FXAA (one pass) or with SMAA
// (edge detection, blend weights, neighbourhood blending).

#include "war3_effect_frame.h"

#include "../render/war3_owned_image_layout.h"
#include "../render/war3_tracked_vk_pipeline.h"

#include "../../../dxvk/dxvk_device.h"

namespace dxvk::war3::effects {

// Where a run ended. Append only.
enum class AaStage : uint32_t {
  Disabled = 0,
  MissingTargets,
  UnsupportedSamples,
  UnsupportedTarget,
  UndefinedLayout,
  ResourceUnavailable,
  FxaaRecorded,
  SmaaRecorded,
};

class AaExecutor {
public:
  explicit AaExecutor(Rc<DxvkDevice> device);

  // Records the copy and the passes into ctx, or no frame command at all.
  // SMAA falls back to FXAA for a frame whose lookup tables are not ready.
  AaStage run(const Rc<DxvkCommandList>& ctx, const EffectFrame& frame,
              const scene::Aa& settings, EffectRecording* recording = nullptr);
  // Bytes of the owned images (colour copy, SMAA targets and lookup tables).
  uint64_t resourceBytes() const;

private:
  struct Pass {
    const DxvkPipelineLayout* layout = nullptr;
    Rc<render::War3TrackedVkPipeline> pipeline;
    VkFormat format = VK_FORMAT_UNDEFINED;
  };

  bool ensureSamplers();
  bool ensurePipelines(VkFormat format, bool smaa);
  bool ensureTargets(VkExtent3D extent, VkFormat copyFormat, bool smaa);
  bool createSmaaLookupTextures(const Rc<DxvkCommandList>& ctx);

  Rc<DxvkDevice> m_device;
  Rc<DxvkSampler> m_linearSampler;
  Rc<DxvkSampler> m_pointSampler;

  Pass m_fxaa;
  Pass m_smaaEdge;
  Pass m_smaaBlend;
  Pass m_smaaNeighbor;

  // Copy of the target: the input of every pass.
  Rc<DxvkImage> m_colorCopy;
  Rc<DxvkImageView> m_colorCopyView;
  render::War3OwnedImageLayoutState m_colorCopyLayout;

  // SMAA intermediates.
  Rc<DxvkImage> m_edgesTex;   // RG8
  Rc<DxvkImageView> m_edgesView;
  render::War3OwnedImageLayoutState m_edgesLayout;
  Rc<DxvkImage> m_blendTex;   // RGBA8
  Rc<DxvkImageView> m_blendView;
  render::War3OwnedImageLayoutState m_blendLayout;

  // SMAA lookup tables, uploaded once.
  Rc<DxvkImage> m_areaTex;    // 160x560, RG8
  Rc<DxvkImageView> m_areaView;
  render::War3OwnedImageLayoutState m_areaLayout;
  Rc<DxvkImage> m_searchTex;  // 64x16, R8
  Rc<DxvkImageView> m_searchView;
  render::War3OwnedImageLayoutState m_searchLayout;
  bool m_lookupTablesCreated = false;
};

} // namespace dxvk::war3::effects
