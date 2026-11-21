#pragma once

#include "d3d9_war3_pipeline.h"
#include "war3/render/war3_tracked_vk_pipeline.h"

#include <array>
#include <chrono>

namespace dxvk {

  class War3ShadowReceiverPass;

  class War3WaterSurfacePass {
  public:
    War3WaterSurfacePass(const Rc<DxvkDevice>& device, const War3ShadowReceiverPass* shadow);
    void Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input);
    uint64_t ResourceBytes() const;

  private:
    bool preparePipeline(VkFormat format, VkSampleCountFlagBits samples);

    Rc<DxvkDevice> m_device;
    const War3ShadowReceiverPass* m_shadow;
    std::chrono::steady_clock::time_point m_waveTick = std::chrono::steady_clock::now();
    double m_waveSeconds = 0.0;
    Rc<DxvkSampler> m_pointSampler, m_colorSampler;
    const DxvkPipelineLayout* m_layout = nullptr;
    Rc<war3::render::War3TrackedVkPipeline> m_pipeline;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;
    // Frame-local uniform updates. Barriers also cover repeated serials and slot reuse.
    std::array<Rc<DxvkBuffer>, 24> m_uniforms;
  };

}
