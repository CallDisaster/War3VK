// war3_handle_resolver.h - Handle 解析模块
// 提供 jHandle <-> CUnit/CAgent 双向解析

#pragma once

#include <cstdint>

namespace dxvk::war3 {

// ============================================================================
// Handle 解析器
// ============================================================================

class HandleResolver {
public:
    static HandleResolver& instance();
    
    // 初始化（从 Game.dll 获取全局指针）
    bool initialize(uintptr_t gameDllBase);
    
    // 检查是否已初始化
    bool isInitialized() const { return m_initialized; }
    
    // 根据 CUnit* 查找 handleId
    // 返回 true 如果找到，outHandleId 为不含 0x100000 的 handleId
    bool findHandleByUnitPtr(void* unitPtr, uint32_t* outHandleId, void** outAgent = nullptr);
    
    // 根据 handleId 获取 CAgent 对象
    bool resolveHandle(uint32_t handleId, uint32_t typeHash, void** outObj);
    
    // 获取 JassHandleTable 基址
    uintptr_t getHandleTableBase() const;
    
    // 获取 CGameState 基址
    uintptr_t getGameStateBase() const;
    
    // 使用 CUnit+0x0C 作为 fallback handleId
    bool tryFallbackHandleId(void* unitPtr, uint32_t* outHandleId);
    
private:
    HandleResolver() = default;
    
    bool m_initialized = false;
    uintptr_t m_gameWar3PtrAddr = 0;  // Game.dll + 0xBE4238
};

// ============================================================================
// 便捷函数
// ============================================================================

// 从 CUnit* 获取完整 jHandle
inline uint32_t GetJHandleFromUnit(void* unitPtr) {
    uint32_t handleId = 0;
    if (HandleResolver::instance().findHandleByUnitPtr(unitPtr, &handleId, nullptr))
        return handleId | 0x100000;
    return 0;
}

// 检查对象是否为建筑
bool IsUnitBuilding(void* unitPtr);

// 获取对象 rawcode
uint32_t GetUnitRawcode(void* unitPtr);

// 获取对象类型描述
const char* GetObjectKindName(void* unitPtr, uint32_t agentTypeHash = 0);

} // namespace dxvk::war3
