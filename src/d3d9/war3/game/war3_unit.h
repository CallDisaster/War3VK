#pragma once

#include <cstdint>
#include "../core/war3_game_structs.h"
#include "../render/war3_render_objects.h" // for ObjectKind

namespace dxvk::war3::game {

/**
 * @brief CUnit 的面向对象封装
 * 提供安全的属性访问和类型判断方法
 */
class UnitWrapper {
public:
    /**
     * @brief 构造 UnitWrapper
     * @param ptr 指向 CUnit 结构的指针，可以为 nullptr
     */
    explicit UnitWrapper(void* ptr);

    /**
     * @brief 检查指针及其内存范围是否有效
     */
    bool IsValid() const;

    /**
     * @brief 获取原始指针
     */
    void* GetPtr() const { return m_ptr; }

    // ========================================================================
    // 基础属性访问 (SafeRead)
    // ========================================================================

    uint32_t GetRawcode() const;
    uint32_t GetFlags5C() const;
    uint32_t GetFlags60() const;
    void*    GetSprite() const;
    float    GetFlyHeight() const;

    // ========================================================================
    // 类型判断 (基于 Flags)
    // ========================================================================

    bool IsBuilding() const;
    bool IsUnit() const;
    bool IsDead() const;
    bool IsHero() const;    // 需访问更多偏移
    bool IsIllusion() const;
    bool IsSelectable() const;

    // ========================================================================
    // Handle 集成
    // ========================================================================

    /**
     * @brief 获取关联的 Handle ID (通过 HandleResolver)
     * @return handleId (不含 0x100000) 或 0
     */
    uint32_t GetHandleId() const;

    /**
     * @brief 获取完整的 Jass Handle
     * @return jHandle
     */
    uint32_t GetJassHandle() const;

    // ========================================================================
    // 高级功能
    // ========================================================================

    /**
     * @brief 确定对象类型 (Unit / Building / Destructible / Item)
     * @param agentType 可选，如果外部已读取 agentType，可传入以加速判断（特别是针对 Non-Unit 对象）
     */
    dxvk::war3::render::ObjectKind GetKind(uint32_t agentType = 0) const;

private:
    void* m_ptr = nullptr;
};

} // namespace dxvk::war3::game
