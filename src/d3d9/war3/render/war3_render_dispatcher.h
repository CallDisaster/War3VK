// war3_render_dispatcher.h - 渲染分发阶段的状态桥接

#pragma once

#include <cstdint>

namespace dxvk {
    enum class War3RenderLayer : uint8_t;
}

namespace dxvk::war3::render {

// 负责把 Hook 的分发阶段状态收敛到 War3RenderState
class War3RenderDispatcher {
public:
    static War3RenderDispatcher& instance();

    // Dispatcher/Scene 提交阶段（用于下游 Draw 关联）
    int PushDispatcherStage(int stage);
    void PopDispatcherStage(int prevStage);

    // UI 分发入口（用于 UI 层级标记）
    War3RenderLayer BeginUiDispatch();
    void EndUiDispatch(War3RenderLayer prevLayer);

private:
    War3RenderDispatcher() = default;
};

} // namespace dxvk::war3::render
