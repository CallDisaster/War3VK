#include "war3_render_state.h"

namespace dxvk::war3::state {

RenderState& RenderState::instance() {
    static RenderState* s_instance = new RenderState();
    return *s_instance;
}

void RenderState::beginFrame() {
    m_frameIndex.fetch_add(1, std::memory_order_relaxed);
}

void RenderState::endFrame() {
    // 可以在这里重置某些每帧状态，目前暂时为空
}

void RenderState::setWorldPointer(void* ptr) {
    m_worldPtr.store(ptr, std::memory_order_relaxed);
}

void RenderState::setIsInGame(bool value) {
    m_isInGame.store(value, std::memory_order_relaxed);
}

void RenderState::setIsLoading(bool value) {
    m_isLoading.store(value, std::memory_order_relaxed);
}

void* RenderState::getWorldPointer() const {
    return m_worldPtr.load(std::memory_order_relaxed);
}

bool RenderState::isInGame() const {
    return m_isInGame.load(std::memory_order_relaxed);
}

bool RenderState::isLoading() const {
    return m_isLoading.load(std::memory_order_relaxed);
}

uint32_t RenderState::getFrameIndex() const {
    return m_frameIndex.load(std::memory_order_relaxed);
}

} // namespace dxvk::war3::state
