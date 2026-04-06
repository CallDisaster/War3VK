#pragma once

#include <cstdint>
#include "war3_unit.h"
#include "../core/war3_game_structs.h"

namespace dxvk::war3::game {

/**
 * @brief CAgentBaseAbs 的面向对象封装
 * 提供对游戏 Agent 对象的类型判断和 Unit 转换
 */
class AgentWrapper {
public:
    /**
     * @brief 构造 AgentWrapper
     * @param ptr 指向 CAgentBaseAbs 结构的指针
     */
    explicit AgentWrapper(void* ptr);

    /**
     * @brief 检查指针有效性
     */
    bool IsValid() const;

    /**
     * @brief 获取原始指针
     */
    void* GetPtr() const { return m_ptr; }

    // ========================================================================
    // 属性访问
    // ========================================================================

    /**
     * @brief 获取对象类型 FourCC (从偏移 0x10 读取)
     * @return 实际内存中的 FourCC (小端序)
     */
    uint32_t GetTypeFourCC() const;

    /**
     * @brief 获取关联的 Unit 指针
     */
    void* GetUnitPtr() const;

    // ========================================================================
    // 类型判断 (使用 _LE 常量)
    // ========================================================================

    bool IsUnit() const;
    bool IsDestructible() const;
    bool IsItem() const;

    // ========================================================================
    // 转换
    // ========================================================================

    /**
     * @brief 转换为 UnitWrapper
     * 获取内部的 UnitPtr 并创建 UnitWrapper 返回，如果不包含 UnitPtr 则返回无效的 wrapper
     */
    UnitWrapper AsUnit() const;

private:
    void* m_ptr = nullptr;
};

} // namespace dxvk::war3::game
