#pragma once

#include "d3d9_war3_scene.h"
#include "d3d9_war3_settings.h"
#include "war3/render/war3_render_state.h"

#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_cmdlist.h"

#include <memory>
#include <vector>
#include <chrono>
#include <string>

namespace dxvk {

    /**
     * @brief War3 DXVK 渲染管线插入点
     *
     * 目前仅实现 BeforeUi（世界渲染结束、UI 开始前）。
     */
    enum class War3InsertionPoint : uint8_t {
        BeforeUi = 0,
        AfterUi  = 1,
        BeforePresent = 2,
    };

    /**
     * @brief 插入点输入：当前目标 RT/DS 的 DXVK 视图
     *
     * 只包含本次 pass 所需的最小信息，避免与 D3D9 状态耦合。
     */
    struct War3PipelineInput {
        Rc<DxvkImageView> colorView;
        Rc<DxvkImageView> depthView; // 预留
        War3FrameScene scene;        // 本帧捕获的世界数据（阴影/光照/后处理共享）
        uint32_t frameIndex = 0;     // Synchronization: 0..2 ring-buffer slot only
        // Monotonic D3D9 presentation-frame identity. Never use frameIndex for
        // publication/cache freshness: its 0/1/2 aliases repeat every three
        // frames and slot zero is a valid resource slot, not an invalid frame.
        uint64_t frameSerial = 0;
        const War3RenderSettings* settings = nullptr;
    };

    /**
     * @brief War3 DXVK 后处理/光照 pass 基类
     */
    class War3RenderPass {
    public:
        virtual ~War3RenderPass() = default;
        virtual War3InsertionPoint Point() const = 0;
        virtual void Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) = 0;
    };

    /**
     * @brief War3 渲染管线总控
     *
     * - 负责每帧插入点调度
     * - 管理注册的 RenderPass
     * - 不直接依赖 Hook/Debug 逻辑
     */
    class War3RenderPipeline {
    public:
        explicit War3RenderPipeline(const Rc<DxvkDevice>& device);
        ~War3RenderPipeline();

        void OnFrameStart();

        /**
         * @brief 通知一条 draw 的分类信息
         * @return true 表示需要立刻插入 BeforeUi pass
         */
        bool NotifyDraw(War3RenderLayer layer,
                        War3RenderState::StageCategory category,
                        War3BatchTag batchTag,
                        bool isUiBoundaryDraw);
        bool ForceBeforeUiInsertion();

        void Execute(War3InsertionPoint point,
                     const Rc<DxvkCommandList>& ctx,
                     const War3PipelineInput& input);

        struct PassEntry {
            std::string name;
            bool enabled = true;
            std::unique_ptr<War3RenderPass> pass;
        };

        void RegisterPass(const char* name, std::unique_ptr<War3RenderPass> pass, bool enabled = true);
        bool SetPassEnabled(const char* name, bool enabled);
        bool IsPassEnabled(const char* name) const;

        const War3RenderSettings& GetSettings() const { return m_settings; }
        War3RenderSettings& MutableSettings() { return m_settings; }

        bool HasInsertedBeforeUi() const { return m_insertedBeforeUi; }
        bool HasArmedBeforeUi() const { return m_armedBeforeUi; }

        // ====================================================================
        // 性能：快速旁路判定（用于“全部关闭”场景）
        // ====================================================================

        // 是否需要插入 BeforeUi（若为 false，设备侧可跳过所有分界检测与 beginExternalRendering）
        bool WantsBeforeUiInsertion() const { return m_wantsBeforeUiInsertion; }

        // 是否需要进行 Shadow/Outline 的 Draw 捕获（若为 false，设备侧可跳过 ShadowCapture 热路径）
        bool WantsShadowCapture() const { return m_wantsShadowCapture; }

    private:
        Rc<DxvkDevice> m_device;
        std::vector<PassEntry> m_passes;

        War3RenderSettings m_settings = { };

        bool m_insertedBeforeUi = false;
        bool m_armedBeforeUi = false;
        bool m_hadWorldDraw = false;
        float m_lastAutoExposure = 0.0f;
        bool m_hasAutoExposure = false;

        // 旁路开关（每帧 OnFrameStart 刷新）
        bool m_wantsBeforeUiInsertion = true;
        bool m_wantsShadowCapture = true;
        // 日夜循环状态已移动到 War3ShadowReceiverPass
    };

} // namespace dxvk
