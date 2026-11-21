#pragma once

#include "d3d9_war3_pipeline.h"
#include "war3/effects/war3_aa_executor.h"

namespace dxvk {

    class D3D9DeviceEx;

    /**
     * @brief War3 Anti-Aliasing Pass
     *
     * 支持两种抗锯齿模式：
     * - FXAA：单 pass，快速但略微模糊
     * - SMAA：三 pass（边缘检测 + 混合权重 + 邻域混合），高质量
     *
     * 插入点：BeforeUi（阴影之后）。The passes themselves are the shared executor
     * (war3/effects/war3_aa_executor.h); this pass adapts the pipeline input.
     */
    class War3AAPass final : public War3RenderPass {
    public:
        explicit War3AAPass(D3D9DeviceEx* device);
        ~War3AAPass() override = default;

        War3InsertionPoint Point() const override { return War3InsertionPoint::BeforeUi; }
        void Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) override;

    private:
        war3::effects::AaExecutor m_executor;
    };

} // namespace dxvk
