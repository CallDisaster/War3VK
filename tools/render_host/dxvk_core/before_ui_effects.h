#pragma once
// The BeforeUi effect executors of the x64 host: the same sources the x86
// in-process passes wrap (src/d3d9/war3/effects), fed from the frame's scene
// packet instead of War3PipelineInput.
#include "../../../src/d3d9/war3/effects/war3_aa_executor.h"
#include "../../../src/d3d9/war3/effects/war3_bloom_executor.h"
#include "../../../src/d3d9/war3/effects/war3_snow_executor.h"
#include "../../../src/d3d9/war3/effects/war3_ssao_executor.h"

namespace warvk::host::dxvkcore {
namespace fx = dxvk::war3::effects;

struct BeforeUiStages {
  fx::SsaoStage ssao = fx::SsaoStage::Disabled;
  fx::SnowStage snow = fx::SnowStage::Disabled;
  fx::AaStage aa = fx::AaStage::Disabled;
  fx::BloomStage bloom = fx::BloomStage::Disabled;
  uint64_t resourceBytes = 0;
};

class BeforeUiEffects {
public:
  explicit BeforeUiEffects(const dxvk::Rc<dxvk::DxvkDevice>& device)
    : m_ssao(device), m_snow(device), m_aa(device), m_bloom(device) { }

  // Order is the in-process registration order (D3D9DeviceEx: "SSAO",
  // "Weather", "AA"): snow is drawn over the occluded world, not occluded
  // itself, and anti-aliased with it. Bloom and the tone map follow, as the
  // device's post-processing does after its BeforeUi passes.
  BeforeUiStages run(const dxvk::Rc<dxvk::DxvkCommandList>& list,
      const dxvk::Rc<dxvk::DxvkImageView>& color, const dxvk::Rc<dxvk::DxvkImageView>& depth,
      const fx::scene::FramePacket& packet, uint64_t frameSerial, uint64_t mapEpoch, uint64_t deviceEpoch) {
    fx::EffectFrame frame;
    frame.colorView = color; frame.depthView = depth;
    frame.frameSerial = frameSerial; frame.mapEpoch = mapEpoch; frame.deviceEpoch = deviceEpoch;
    if (packet.hasCamera) frame.camera = fx::CameraFromScene(packet.camera, frameSerial);
    BeforeUiStages stages;
    if (packet.hasSsao) stages.ssao = m_ssao.run(list, frame, packet.ssao);
    if (packet.hasSnow) {
      fx::SnowFrame snow; snow.snow = packet.snow;
      snow.frameSerial = frameSerial; snow.mapEpoch = mapEpoch; snow.deviceEpoch = deviceEpoch;
      stages.snow = m_snow.run(list, frame, snow).stage; m_snowOn = true;
    } else if (m_snowOn) {
      // The weather was turned off: its depth copy goes with it.
      m_snow.release(); m_snowOn = false;
    }
    if (packet.hasAa) stages.aa = m_aa.run(list, frame, packet.aa);
    if (packet.hasBloom) stages.bloom = m_bloom.run(list, frame, packet.bloom);
    stages.resourceBytes = m_ssao.resourceBytes() + m_snow.resourceBytes() +
      m_aa.resourceBytes() + m_bloom.resourceBytes();
    return stages;
  }

private:
  fx::SsaoExecutor m_ssao;
  fx::SnowExecutor m_snow;
  fx::AaExecutor m_aa;
  fx::BloomExecutor m_bloom;
  bool m_snowOn = false;
};
} // namespace warvk::host::dxvkcore
