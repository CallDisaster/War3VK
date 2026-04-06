#pragma once

#include "war3/render/war3_render_state.h"
#include <atomic>

namespace dxvk {

    /**
     * @brief 轻量级路径追踪占位/统计模块
     *
     * - 默认关闭，不影响现有渲染；启用后仅做批次统计与 UI 过滤，不替代光栅化。
     * - 后续可在此基础上接入真正的路径追踪实现（VK 计算/光追扩展），目前只做数据收集骨架。
     */
    class War3PathTracer {
    public:
        static War3PathTracer& Get();

        void SetEnabled(bool enabled);
        bool IsEnabled() const;

        // 每次有效的 Draw 调用上报当前分类信息（已过滤 UI）
        void RecordDraw(War3RenderLayer layer,
                        War3RenderState::StageCategory category,
                        War3BatchTag batchTag);

        // 统计接口（可用于调试）
        std::uint64_t GetRecordedCount() const { return m_totalRecords.load(std::memory_order_relaxed); }

    private:
        War3PathTracer() = default;
        std::atomic<bool> m_enabled{false};
        std::atomic<std::uint64_t> m_totalRecords{0};
        std::atomic<bool> m_loggedFirst{false};
    };

} // namespace dxvk
