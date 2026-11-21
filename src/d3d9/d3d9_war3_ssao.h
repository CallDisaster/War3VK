#pragma once

#include "d3d9_war3_pipeline.h"
#include "war3/effects/war3_ssao_executor.h"

namespace dxvk {

    class D3D9DeviceEx;

    /**
     * @brief War3 SSAO Pass（内置）
     *
     * The composite itself is the shared executor
     * (war3/effects/war3_ssao_executor.h); this pass adapts the pipeline input.
     */
    class War3SsaoPass final : public War3RenderPass {
    public:
        explicit War3SsaoPass(D3D9DeviceEx* device);
        ~War3SsaoPass() override = default;

        War3InsertionPoint Point() const override { return War3InsertionPoint::BeforeUi; }
        void Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) override;

    private:
        war3::effects::SsaoExecutor m_executor;
    };

} // namespace dxvk
