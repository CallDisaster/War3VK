#include "war3_agent.h"
#include "../core/war3_memory.h"

namespace dxvk::war3::game {

namespace {
    bool IsKnownTypeFourCC(uint32_t value) {
        return value == AgentTypeFourCC::Unit_LE ||
               value == AgentTypeFourCC::Destructible_LE ||
               value == AgentTypeFourCC::Item_LE;
    }

    bool IsKnownTypeId(uint32_t value) {
        return value == AgentTypeFourCC::DestructibleID;
    }
}

AgentWrapper::AgentWrapper(void* ptr) : m_ptr(ptr) {}

bool AgentWrapper::IsValid() const {
    // 至少能读取 TypeFourCC (0x10)
    return m_ptr && IsReadableRangeFast(m_ptr, 0x14);
}

uint32_t AgentWrapper::GetTypeFourCC() const {
    if (!IsValid())
        return 0;
    
    // Agent 的 HashGroup (offset 0x0C) 包含两个 uint32:
    // - hash_0x0 (0x0C): 类型 ID，部分对象（如可破坏物）的类型值在此
    // - hash_0x4 (0x10): 类型 FourCC，部分对象（如单位）的类型值在此
    // 
    // 策略：
    // - 0x10 仅在明确是 +w3u/+w3d/+w3i 时才采信
    // - 0x0C 只接受已知的可破坏物 ID
    // 这样可以避免 CUnit 的 hash 值被误判为类型
    
    uint32_t typeAtOffset0C = 0;
    uint32_t typeAtOffset10 = 0;
    
    SafeReadU32Fast(m_ptr, CAgentOffsets::TypeHashId, typeAtOffset0C);   // 0x0C
    SafeReadU32Fast(m_ptr, CAgentOffsets::TypeFourCC, typeAtOffset10);   // 0x10
    
    if (IsKnownTypeFourCC(typeAtOffset10) && dxvk::war3::IsLikelyFourCC(typeAtOffset10)) {
        return typeAtOffset10;
    }

    if (IsKnownTypeId(typeAtOffset0C)) {
        return typeAtOffset0C;
    }

    if (IsKnownTypeFourCC(typeAtOffset0C) && dxvk::war3::IsLikelyFourCC(typeAtOffset0C)) {
        return typeAtOffset0C;
    }

    return 0;
}

void* AgentWrapper::GetUnitPtr() const {
    void* unitPtr = nullptr;
    // 需要能读到 0x54
    if (m_ptr && IsReadableRangeFast(m_ptr, 0x60)) { 
        SafeReadPtrFast(m_ptr, CAgentOffsets::UnitPtr, unitPtr);
    }
    return unitPtr;
}

bool AgentWrapper::IsUnit() const {
    return GetTypeFourCC() == AgentTypeFourCC::Unit_LE;
}

bool AgentWrapper::IsDestructible() const {
    uint32_t type = GetTypeFourCC();
    // 可破坏物的类型值可能是 FourCC 或类型 ID
    return type == AgentTypeFourCC::Destructible_LE || 
           type == AgentTypeFourCC::DestructibleID;
}

bool AgentWrapper::IsItem() const {
    return GetTypeFourCC() == AgentTypeFourCC::Item_LE;
}

UnitWrapper AgentWrapper::AsUnit() const {
    // 即使类型检查不通过，只要有 UnitPtr 也尝试转换
    return UnitWrapper(GetUnitPtr());
}

} // namespace dxvk::war3::game
