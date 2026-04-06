#pragma once

#include <cstdint>

namespace dxvk {
namespace war3 {
namespace state {

/**
 * @brief War3专用状态缓存
 *
 * 魔兽争霸3的RenderQueue在状态切换时频繁调用GxDevice_ApplyStateBlock，
 * DXVK会将D3D9状态转换为Vulkan流水线对象。如果相同状态重复应用，
 * 会浪费GPU时间重建流水线。
 *
 * 优化策略：
 * 1. 缓存D3D9状态的前20字节哈希（魔兽比较的精确范围）
 * 2. 每次应用前先检查缓存，命中则跳过
 * 3. 支持多纹理stage（魔兽最多16个stage）
 */
class War3StateCache {
public:
    War3StateCache();
    
    /**
     * @brief 尝试应用状态块
     * @param statePtr D3D9状态块指针（36字节）
     * @param stageIndex 纹理stage索引（0-15）
     * @param meshIndex 模型索引
     * @param layerIndex 层索引
     * @return true表示状态已应用，false表示被缓存跳过
     */
    bool TryApplyStateBlock(void* statePtr, uint32_t stageIndex, 
                         uint32_t meshIndex, uint32_t layerIndex);
    
    /**
     * @brief 重置缓存（每帧开始时调用）
     */
    void Reset();
    
    /**
     * @brief 获取缓存统计信息（用于性能分析）
     */
    void GetStats(uint64_t& hits, uint64_t& misses) const {
        hits = m_totalHits;
        misses = m_totalMisses;
    }
    
private:
    static constexpr uint32_t kMaxStages = 16;
    
    // 状态缓存项
    struct CacheEntry {
        uint64_t stateHash;      // 前20字节哈希
        uint32_t meshIndex;      // 模型索引
        uint32_t layerIndex;      // 层索引
        bool valid;              // 是否有效
    };
    
    CacheEntry m_cache[kMaxStages];
    
    // 统计
    uint64_t m_totalHits = 0;
    uint64_t m_totalMisses = 0;
    
    /**
     * @brief 计算前20字节的FNV-1a哈希
     * 
     * 使用FNV-1a是因为：
     * 1. 快速计算（简单乘法和异或）
     * 2. 低哈希冲突率（FNV的分布较好）
     * 3. 与魔兽原版memcmp行为一致（只比较前20字节）
     */
    static uint64_t Hash20Bytes(const void* ptr) {
        const uint8_t* data = static_cast<const uint8_t*>(ptr);
        uint64_t hash = 1469598103934665603ULL;  // FNV offset basis
        
        for (int i = 0; i < 20; i++) {
            hash ^= data[i];
            hash *= 1099511628211ULL;  // FNV prime
        }
        
        return hash;
    }
    
    /**
     * @brief 检查缓存是否命中
     */
    bool CheckCacheHit(uint32_t stageIndex, uint64_t stateHash, 
                    uint32_t meshIndex, uint32_t layerIndex) {
        if (stageIndex >= kMaxStages)
            return false;
        
        CacheEntry& entry = m_cache[stageIndex];
        
        if (!entry.valid)
            return false;
        
        // 精确匹配：哈希 + meshIndex + layerIndex
        return (entry.stateHash == stateHash &&
                entry.meshIndex == meshIndex &&
                entry.layerIndex == layerIndex);
    }
    
    /**
     * @brief 更新缓存
     */
    void UpdateCache(uint32_t stageIndex, uint64_t stateHash,
                  uint32_t meshIndex, uint32_t layerIndex) {
        if (stageIndex >= kMaxStages)
            return;
        
        CacheEntry& entry = m_cache[stageIndex];
        entry.stateHash = stateHash;
        entry.meshIndex = meshIndex;
        entry.layerIndex = layerIndex;
        entry.valid = true;
    }
    
    /**
     * @brief 使缓存无效（用于状态强制刷新）
     */
    void InvalidateCache(uint32_t stageIndex) {
        if (stageIndex < kMaxStages)
            m_cache[stageIndex].valid = false;
    }
    
    /**
     * @brief 使所有缓存无效
     */
    void InvalidateAll() {
        for (uint32_t i = 0; i < kMaxStages; i++)
            m_cache[i].valid = false;
    }
};

} // namespace state
} // namespace war3
} // namespace dxvk