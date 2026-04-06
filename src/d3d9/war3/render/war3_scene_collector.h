#pragma once

#include <cstdint>

namespace dxvk::war3::render {

/**
 * @brief 场景对象收集器
 * 
 * 负责在渲染帧开始时（Hook_WorldObjects_RenderGroup），
 * 遍历游戏内存中的对象列表，建立渲染对象(Entry)与逻辑对象(Unit/Handle)的映射关系。
 */
class SceneCollector {
public:
    /**
     * @brief 从 WorldObjects 渲染组收集对象
     * 对应原始函数：WorldObjects_RenderGroup (RVA 0x368E30)
     * 
     * @param gameWorldPtr 游戏 World 对象指针 (this)
     * @param groupIdx 渲染组索引:
     *                 0: WorldObjects (单位/可破坏物)
     *                 1: SelectionOverlay
     *                 2: Decorations (装饰物/特效)
     */
    static void CollectWorldObjects(void* gameWorldPtr, int groupIdx);
};

} // namespace dxvk::war3::render
