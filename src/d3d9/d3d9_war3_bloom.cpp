#include "d3d9_war3_bloom.h"
#include "war3/effects/war3_effect_scene_input.h"
#include "d3d9_device.h"
#include "war3/tools/war3_perf_monitor.h"

namespace dxvk {

namespace {
// GPU timing covers exactly the recorded commands.
struct BloomPerfScope final : war3::effects::EffectRecording {
  war3::War3PerfMonitor::ScopedSection section;
  void beginRecording(const Rc<DxvkCommandList>& ctx) override {
    section = war3::War3PerfMonitor::instance().scope("PostFX/Bloom", ctx);
  }
};
} // namespace

War3BloomPass::War3BloomPass(D3D9DeviceEx* device)
: m_executor(device->GetDXVKDevice()) { }

war3::effects::scene::Bloom War3BloomPass::Record(const War3PostFxSettings& settings) {
  return war3::effects::BloomFromSettings(settings);
}

war3::effects::BloomStage War3BloomPass::Run(const Rc<DxvkCommandList>& ctx,
                                             const Rc<DxvkImageView>& color,
                                             const war3::effects::scene::Bloom& record) {
  war3::effects::EffectFrame frame;
  frame.colorView = color;
  BloomPerfScope perfScope;
  return m_executor.run(ctx, frame, record, &perfScope);
}

} // namespace dxvk
