#include "d3d9_war3_ssao.h"
#include "war3/effects/war3_effect_scene_input.h"
#include "d3d9_device.h"
#include "d3d9_war3_effect_adapter.h"

namespace dxvk {

    War3SsaoPass::War3SsaoPass(D3D9DeviceEx* device)
        : m_executor(device->GetDXVKDevice()) { }

    void War3SsaoPass::Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) {
        if (!input.settings)
            return;
        if (!input.settings->postFx.enabled || !input.settings->postFx.ssao.enabled)
            return;
        if (!input.colorView || !input.depthView)
            return;

        const war3::effects::scene::Ssao record =
            war3::effects::SsaoFromSettings(input.settings->postFx.ssao);
        m_executor.run(ctx, War3EffectFrame(input), record);
    }

} // namespace dxvk
