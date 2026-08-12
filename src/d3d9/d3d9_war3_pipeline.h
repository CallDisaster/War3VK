#pragma once

#include "d3d9_war3_scene.h"
#include "d3d9_war3_settings.h"
#include "war3/render/war3_render_state.h"

#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_cmdlist.h"

#include <memory>
#include <mutex>
#include <vector>
#include <chrono>
#include <cstddef>
#include <string>

namespace dxvk::war3 {
    class War3SettingsWrite;
}

namespace dxvk {

    // Shared authoring mailbox. Scoped writers may outlive the active-device
    // publication they acquired it from, so this state must not be embedded
    // directly in War3RenderPipeline.
    struct War3RenderSettingsMailbox {
        mutable std::mutex mutex;
        War3RenderSettings pending = { };
        uint64_t pendingRevision = 0u;
        uint64_t appliedRevision = 0u;
        uint64_t pendingExposureRevision = 0u;
        uint64_t appliedExposureRevision = 0u;
    };

    // Per-command derived lighting. The queued command owns this object; the
    // shadow pass may resolve the authored day/night cycle and later passes
    // consume that result without mutating the immutable authored snapshot.
    struct War3FrameLightingState {
        Vector4 sunDirection = Vector4(0.0f, 0.0f, -1.0f, 0.0f);
        Vector4 sunColor = Vector4(1.0f, 1.0f, 1.0f, 0.0f);
        float renderTimeHours = 12.0f;
    };

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
        uint64_t mapEpoch = 0;
        uint64_t deviceEpoch = 0;
        // Immutable ownership is carried with the queued CS command. A raw
        // pointer into War3RenderPipeline would race the next JASS/UI update
        // and could observe a torn multi-field configuration.
        std::shared_ptr<const War3RenderSettings> settings;
        // Shared only by passes within this queued command. It never aliases
        // the render-owner or authoring copies.
        std::shared_ptr<War3FrameLightingState> lighting;
        // Safe feedback channel for derived day/night values. The expected
        // revision prevents a stale CS command from overwriting newer author
        // input.
        std::shared_ptr<War3RenderSettingsMailbox> settingsMailbox;
        uint64_t settingsRevision = 0u;
    };

    // UpdateRenderContext lives in a separate translation unit and walks the
    // scene vectors embedded in War3PipelineInput. Encode every layout that
    // participates in that walk in a link-time symbol so a stale object with
    // missing header dependencies cannot silently use an old vector element
    // stride. A mixed incremental build must fail to link instead of reading
    // past a caster during map startup.
    template <std::size_t InputSize,
              std::size_t InputAlignment,
              std::size_t SceneSize,
              std::size_t SceneAlignment,
              std::size_t CasterSize,
              std::size_t CasterAlignment,
              std::size_t ReplayBindingSize,
              std::size_t ReplayBindingAlignment>
    struct War3ShaderContextAbiTag { };

    using War3ShaderContextAbi = War3ShaderContextAbiTag<
        sizeof(War3PipelineInput),
        alignof(War3PipelineInput),
        sizeof(War3FrameScene),
        alignof(War3FrameScene),
        sizeof(War3ShadowCasterDraw),
        alignof(War3ShadowCasterDraw),
        sizeof(War3ShadowReplayBufferBinding),
        alignof(War3ShadowReplayBufferBinding)>;

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

        // Render-owner view. External/JASS writers must use the scoped editor
        // exposed by war3::GetMutableSettings().
        const War3RenderSettings& GetSettings() const { return m_settings; }
        void CaptureSettingsSnapshot(War3PipelineInput& input) const;
        std::shared_ptr<War3RenderSettingsMailbox> GetSettingsMailbox() const {
            return m_settingsMailbox;
        }
        bool CopyPendingSettings(War3RenderSettings& out) const {
            const auto mailbox = m_settingsMailbox;
            if (!mailbox)
                return false;
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            out = mailbox->pending;
            return true;
        }

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

        // m_settings is render-owner state. Cross-thread authoring edits are
        // serialized into the shared mailbox and become visible only at the
        // next OnFrameStart safe point.
        War3RenderSettings m_settings = { };
        std::shared_ptr<War3RenderSettingsMailbox> m_settingsMailbox =
            std::make_shared<War3RenderSettingsMailbox>();

        bool m_insertedBeforeUi = false;
        bool m_armedBeforeUi = false;
        bool m_hadWorldDraw = false;
        float m_lastAutoExposure = 0.0f;
        bool m_hasAutoExposure = false;
        uint64_t m_lightingClockRevision = 0u;
        float m_lightingClockTime01 = 0.5f;
        std::chrono::steady_clock::time_point m_lightingClockLastUpdate;

        // 旁路开关（每帧 OnFrameStart 刷新）
        bool m_wantsBeforeUiInsertion = true;
        bool m_wantsShadowCapture = true;
        // Direction/color consumption remains in War3ShadowReceiverPass; the
        // render-only authored clock advances here even when shadows are off.
    };

    // Allocation and destruction stay in the same translation unit as the
    // constructor. The ABI tag is part of the factory symbol, so a stale
    // caller compiled with an older pipeline/settings layout fails to link
    // instead of under-allocating the object and crashing in the constructor.
    template <std::size_t PipelineSize,
              std::size_t PipelineAlignment,
              std::size_t SettingsSize,
              std::size_t SettingsAlignment>
    struct War3RenderPipelineAbiTag { };

    using War3RenderPipelineAbi = War3RenderPipelineAbiTag<
        sizeof(War3RenderPipeline),
        alignof(War3RenderPipeline),
        sizeof(War3RenderSettings),
        alignof(War3RenderSettings)>;

    War3RenderPipeline* CreateWar3RenderPipeline(
        const Rc<DxvkDevice>& device,
        War3RenderPipelineAbi);

    void DestroyWar3RenderPipeline(
        War3RenderPipeline* pipeline,
        War3RenderPipelineAbi);

} // namespace dxvk

namespace war3shader::internal {

    // Implemented beside UpdateRenderContext. The ABI tag is deliberately a
    // value parameter so its layout constants become part of the symbol name.
    void ValidateWar3ShaderContextAbi(::dxvk::War3ShaderContextAbi) noexcept;

} // namespace war3shader::internal
