#include "war3_render_state.h"

#include <windows.h>

namespace dxvk::war3::state {

RenderState& RenderState::instance() {
    static RenderState* s_instance = new RenderState();
    return *s_instance;
}

void RenderState::beginFrame() {
    m_frameIndex.fetch_add(1, std::memory_order_relaxed);
    // Frame pace: one caller (the thread that presents), so plain members hold
    // the clock; the counters are atomic for the control plane's reads.
    LARGE_INTEGER now = {};
    if (!m_paceFrequency) {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        m_paceFrequency = frequency.QuadPart;
    }
    if (m_paceFrequency <= 0 || !QueryPerformanceCounter(&now))
        return;
    const int64_t last = m_paceLast;
    m_paceLast = now.QuadPart;
    if (last <= 0 || now.QuadPart <= last)
        return;
    const uint64_t us = uint64_t(now.QuadPart - last) * 1000000u / uint64_t(m_paceFrequency);
    const uint64_t bucket = us / PaceBucketUs;
    if (bucket < PaceBuckets) {
        m_paceBuckets[bucket].fetch_add(1, std::memory_order_relaxed);
    } else {
        m_paceOver.fetch_add(1, std::memory_order_relaxed);
        m_paceOverUs.fetch_add(us, std::memory_order_relaxed);
    }
    if (us > m_paceMaxUs.load(std::memory_order_relaxed))
        m_paceMaxUs.store(us, std::memory_order_relaxed);
    m_paceFrames.fetch_add(1, std::memory_order_relaxed);
}

void RenderState::readFramePace(FramePace& pace) const {
    pace.frames = m_paceFrames.load(std::memory_order_relaxed);
    pace.over = m_paceOver.load(std::memory_order_relaxed);
    pace.overUs = m_paceOverUs.load(std::memory_order_relaxed);
    pace.maxUs = m_paceMaxUs.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < PaceBuckets; ++i)
        pace.buckets[i] = m_paceBuckets[i].load(std::memory_order_relaxed);
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
