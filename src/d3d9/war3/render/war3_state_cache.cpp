#include "war3_state_cache.h"
#include "../debug/war3_debug.h"

namespace dxvk {
namespace war3 {
namespace state {

War3StateCache::War3StateCache() {
    // 初始化所有缓存为无效
    for (uint32_t i = 0; i < kMaxStages; i++) {
        m_cache[i].valid = false;
        m_cache[i].stateHash = 0;
        m_cache[i].meshIndex = 0;
        m_cache[i].layerIndex = 0;
    }
    
    m_totalHits = 0;
    m_totalMisses = 0;
}

bool War3StateCache::TryApplyStateBlock(void* statePtr, uint32_t stageIndex, 
                                       uint32_t meshIndex, uint32_t layerIndex) {
    // 1. 计算前20字节哈希（魔兽比较的精确范围）
    const uint64_t stateHash = Hash20Bytes(statePtr);
    
    // 2. 检查缓存是否命中
    if (CheckCacheHit(stageIndex, stateHash, meshIndex, layerIndex)) {
        m_totalHits++;
        return false;  // 缓存命中，跳过状态应用
    }
    
    // 3. 缓存未命中，应用状态（调用D3D9原版实现）
    // 注意：这里我们仍然调用原版状态应用，只是缓存哈希避免重复
    // 实际的状态应用由D3D9Device::ApplyStateBlock处理
    m_totalMisses++;
    UpdateCache(stageIndex, stateHash, meshIndex, layerIndex);
    
    return true;  // 状态已应用
}

void War3StateCache::Reset() {
    // 重置所有缓存为无效
    InvalidateAll();
    
    // 重置统计（可选，保留用于每帧分析）
    // m_totalHits = 0;
    // m_totalMisses = 0;
    
    WAR3_LOG_DEBUG("War3StateCache: Reset\n");
}

} // namespace state
} // namespace war3
} // namespace dxvk
