// war3_render_dispatcher.cpp - 渲染分发阶段的状态桥接实现

#include "war3_render_dispatcher.h"
#include "war3_render_state.h"

namespace dxvk::war3::render {

War3RenderDispatcher& War3RenderDispatcher::instance() {
    static War3RenderDispatcher s_instance;
    return s_instance;
}

int War3RenderDispatcher::PushDispatcherStage(int stage) {
    const int prev = War3RenderState::GetDispatcherStage();
    War3RenderState::SetDispatcherStage(stage);
    return prev;
}

void War3RenderDispatcher::PopDispatcherStage(int prevStage) {
    War3RenderState::SetDispatcherStage(prevStage);
}

War3RenderLayer War3RenderDispatcher::BeginUiDispatch() {
    War3RenderState::OnUiDispatch();
    const War3RenderLayer prev = War3RenderState::CurrentLayer();
    War3RenderState::PushUiLayer();
    return prev;
}

void War3RenderDispatcher::EndUiDispatch(War3RenderLayer prevLayer) {
    War3RenderState::PopLayer(prevLayer);
}

} // namespace dxvk::war3::render
