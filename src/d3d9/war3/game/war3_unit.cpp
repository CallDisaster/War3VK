#include "war3_unit.h"
#include "../core/war3_memory.h"
#include "../handle/war3_handle_resolver.h"

namespace dxvk::war3::game {

UnitWrapper::UnitWrapper(void* ptr) : m_ptr(ptr) {}

bool UnitWrapper::IsValid() const {
    // 基础有效性检查：非空且前100字节可读
    return m_ptr && IsReadableRangeFast(m_ptr, 0x64);
}

uint32_t UnitWrapper::GetRawcode() const {
    uint32_t value = 0;
    if (IsValid()) {
        SafeReadU32Fast(m_ptr, CUnitOffsets::Rawcode, value);
    }
    return value;
}

uint32_t UnitWrapper::GetFlags5C() const {
    uint32_t value = 0;
    if (IsValid()) {
        SafeReadU32Fast(m_ptr, CUnitOffsets::Flags5C, value);
    }
    return value;
}

uint32_t UnitWrapper::GetFlags60() const {
    uint32_t value = 0;
    if (IsValid()) {
        SafeReadU32Fast(m_ptr, CUnitOffsets::Flags60, value);
    }
    return value;
}

void* UnitWrapper::GetSprite() const {
    void* sprite = nullptr;
    if (IsValid()) {
        SafeReadPtrFast(m_ptr, CUnitOffsets::Sprite, sprite);
    }
    return sprite;
}

float UnitWrapper::GetFlyHeight() const {
    float value = 0.0f;
    if (IsValid()) {
        SafeReadFast<float>(m_ptr, CUnitOffsets::FlyHeight, value);
    }
    return value;
}

bool UnitWrapper::IsBuilding() const {
    // 建筑标志在 0x5C
    return GetFlags5C() & UnitFlags5C::Building;
}

bool UnitWrapper::IsUnit() const {
    // 根据 flag 判断，非建筑即视为普通单位
    // 注意：如果有其他分类标志，应在此完善
    return IsValid() && !IsBuilding();
}

bool UnitWrapper::IsDead() const {
    return GetFlags5C() & UnitFlags5C::Dead;
}

bool UnitWrapper::IsHero() const {
    // TODO: 英雄标志通常在其他位置，或通过 Rawcode 判断
    // 暂时返回 false 或根据需求实现
    return false; 
}

bool UnitWrapper::IsIllusion() const {
    return GetFlags5C() & UnitFlags5C::Illusion;
}

bool UnitWrapper::IsSelectable() const {
    return GetFlags60() & UnitFlags60::Selectable;
}

uint32_t UnitWrapper::GetHandleId() const {
    if (!IsValid()) return 0;

    uint32_t handleId = 0;
    void* agent = nullptr;
    if (HandleResolver::instance().findHandleByUnitPtr(m_ptr, &handleId, &agent)) {
        return handleId;
    }
    return 0;
}

uint32_t UnitWrapper::GetJassHandle() const {
    uint32_t hId = GetHandleId();
    return hId ? MakeJHandle(hId) : 0;
}

render::ObjectKind UnitWrapper::GetKind(uint32_t agentType) const {
    if (!IsValid()) return render::ObjectKind::Unknown;

    // 过滤掉不认识的 agentType，避免把 CUnit 的 hash 误当类型
    const bool isKnownAgentType =
        agentType == AgentTypeFourCC::Unit_LE ||
        agentType == AgentTypeFourCC::Destructible_LE ||
        agentType == AgentTypeFourCC::DestructibleID ||
        agentType == AgentTypeFourCC::Item_LE;
    if (agentType != 0 && !isKnownAgentType) {
        agentType = 0;
    }

    // 如果传入了 agentType (来自 Agent+0x0C 或 0x10)，优先使用它判断非单位类型
    if (agentType != 0) {
        // 检查是否为可破坏物 (支持 FourCC 和类型 ID)
        if (agentType == AgentTypeFourCC::Destructible_LE || 
           agentType == AgentTypeFourCC::DestructibleID)
            return render::ObjectKind::Destructible;
        
        // 检查是否为物品
        if (agentType == AgentTypeFourCC::Item_LE)
            return render::ObjectKind::Item;
    }

    // ========================================================================
    // 可破坏物检测启发式（当 agentType 不可用时）
    // ========================================================================
    // 
    // 问题：可破坏物（如树）的内存布局与单位不同。
    // 当 HandleResolver 无法找到可破坏物的 Handle 时，agentType 为 0。
    // 此时 UnitWrapper 可能错误地将可破坏物解析为单位。
    //
    // 启发式方法：检查 flags 值是否"异常"
    // - 正常单位: flags 是位标志，通常较小（如 0x00000201, 0x00010000）
    // - 可破坏物: 由于内存布局不同，读取到的"flags"可能是浮点数或大随机值
    //   例如 0x3F800000 = 浮点数 1.0，0x41200000 = 浮点数 10.0
    //
    const uint32_t flags = GetFlags5C();
    
    // 检测浮点数特征值（IEEE 754 单精度浮点数）
    // 正常的小浮点数 (0.0 ~ 100.0) 的指数部分在 0x3F ~ 0x42 范围
    const uint32_t exponent = (flags >> 23) & 0xFF;
    if (exponent >= 0x3E && exponent <= 0x43 && (flags & 0x007FFFFF) != 0) {
        // 看起来像浮点数，很可能是可破坏物
        return render::ObjectKind::Destructible;
    }
    
    // 另一个启发式：如果 flags 的高 16 位太大（不像普通位标志）
    // 但排除合法的高位标志如 0x40000000 (Illusion)
    if ((flags & 0x80000000) != 0 || (flags > 0x7FFFFFFF)) {
        // 可能是可破坏物
        return render::ObjectKind::Destructible;
    }

    // ========================================================================
    // 正常的 Unit / Building 判定
    // ========================================================================
    
    if (agentType == 0 || agentType == AgentTypeFourCC::Unit_LE) {
        if (IsBuilding())
            return render::ObjectKind::Building;
        return render::ObjectKind::Unit;
    }

    return render::ObjectKind::Unknown;
}

} // namespace dxvk::war3::game
