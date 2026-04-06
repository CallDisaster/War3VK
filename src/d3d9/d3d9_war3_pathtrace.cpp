#include "d3d9_war3_pathtrace.h"
#include "d3d9_war3_debug.h"
#include <cstdio>

namespace dxvk {

    War3PathTracer& War3PathTracer::Get() {
        static War3PathTracer inst;
        return inst;
    }

    void War3PathTracer::SetEnabled(bool enabled) {
        m_enabled.store(enabled, std::memory_order_relaxed);
        if (enabled && !m_loggedFirst.exchange(true, std::memory_order_relaxed)) {
            WAR3_RENDER_LOG("DXVK War3PathTracer: enabled (stub collector, no raster override)\n");
        }
    }

    bool War3PathTracer::IsEnabled() const {
        return m_enabled.load(std::memory_order_relaxed);
    }

    void War3PathTracer::RecordDraw(War3RenderLayer layer,
                                    War3RenderState::StageCategory category,
                                    War3BatchTag batchTag) {
        if (!IsEnabled()) return;

        // 过滤 UI/后处理：路径追踪不记录 HUD/UI 与 PostProcess 批次
        if (layer == War3RenderLayer::UI || category == War3RenderState::StageCategory::PostProcess) {
            return;
        }

        // 记录计数（后续可扩展为写入命令缓冲/AS）
        auto count = m_totalRecords.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 5 || (count % 1000 == 0)) {
            WAR3_RENDER_LOG(
                "DXVK War3PathTracer: record #%llu layer=%d cat=%d batchTag=%d\n",
                static_cast<unsigned long long>(count),
                static_cast<int>(layer),
                static_cast<int>(category),
                static_cast<int>(batchTag));
        }
    }

} // namespace dxvk
