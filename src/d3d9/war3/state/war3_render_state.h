#pragma once

#include <cstdint>
#include <atomic>

namespace dxvk::war3::state {

/**
 * @brief 渲染状态管理器
 * 
 * 集中管理魔兽争霸3的全局渲染状态、指针和环境标志。
 * 替代分散的全局变量，提供统一的访问点。
 */
class RenderState {
public:
    static RenderState& instance();

    // 禁止拷贝
    RenderState(const RenderState&) = delete;
    RenderState& operator=(const RenderState&) = delete;

    // =========================================================
    // 生命周期管理
    // =========================================================
    
    /**
     * @brief 标记一帧的开始
     * 通常在 EndScene 或 Present 调用后重置
     */
    void beginFrame();
    
    /**
     * @brief 标记一帧的结束
     */
    void endFrame();

    // =========================================================
    // 状态设置 (通常由 Hook 层调用)
    // =========================================================

    /**
     * @brief 更新游戏 World 对象指针 (this of RenderWorld)
     * @param ptr CGameWorld* 或类似结构
     */
    void setWorldPointer(void* ptr);

    /**
     * @brief 设置当前是否在游戏逻辑中 (而非菜单/加载)
     */
    void setIsInGame(bool value);

    /**
     * @brief 设置当前是否正在加载地图
     */
    void setIsLoading(bool value);

    // =========================================================
    // 状态查询 (供各模块使用)
    // =========================================================

    void* getWorldPointer() const;
    bool isInGame() const;
    bool isLoading() const;
    
    /**
     * @brief 获取当前帧序号
     * @return 这里的帧序号是自定义的渲染帧计数，非游戏逻辑帧
     */
    uint32_t getFrameIndex() const;

private:
    RenderState() = default;

    std::atomic<void*> m_worldPtr = { nullptr };
    std::atomic<bool> m_isInGame = { false };
    std::atomic<bool> m_isLoading = { false };
    std::atomic<uint32_t> m_frameIndex = { 0 };
};

} // namespace dxvk::war3::state
