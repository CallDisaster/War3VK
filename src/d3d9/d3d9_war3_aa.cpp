#include "d3d9_war3_aa.h"
#include "war3/effects/war3_effect_scene_input.h"
#include "d3d9_device.h"
#include "d3d9_war3_effect_adapter.h"
#include "war3/tools/war3_perf_monitor.h"

namespace dxvk {

    namespace {
        // GPU timing covers exactly the recorded commands: a run that ends
        // early opens no scope.
        struct AaPerfScope final : war3::effects::EffectRecording {
            war3::War3PerfMonitor::ScopedSection section;
            void beginRecording(const Rc<DxvkCommandList>& ctx) override {
                section = war3::War3PerfMonitor::instance().scope("PostFX/AA", ctx);
            }
        };
    }

    War3AAPass::War3AAPass(D3D9DeviceEx* device)
        : m_executor(device->GetDXVKDevice()) { }

    void War3AAPass::Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) {
        if (!input.settings || !input.settings->postFx.enabled)
            return;
        const auto& aa = input.settings->postFx.aa;
        if (aa.mode == War3AAMode::None || !input.colorView)
            return;

        const war3::effects::scene::Aa record = war3::effects::AaFromSettings(aa);
        AaPerfScope perfScope;
        m_executor.run(ctx, War3EffectFrame(input), record, &perfScope);
    }

} // namespace dxvk
