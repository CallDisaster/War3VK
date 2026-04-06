#pragma once
#include <functional>
#include <vector>
#include <mutex>
#include "../tools/war3_diagnostics_hub.h"

namespace dxvk::war3 {

    /**
     * @class War3Events
     * @brief 游戏事件分发器 - 用于在安全时机触发回调
     * 
     * 主要事件:
     * - OnJassReady: Jass 引擎初始化完成，可以安全调用 Jass/Storm API
     * - OnGameStart: 游戏开始 (地图加载完成，进入游戏)，用于初始化地图相关功能
     */
    class War3Events {
    public:
        using Callback = std::function<void()>;
        using TickCallback = std::function<void(uint32_t)>;
        
        /// 获取单例
        static War3Events& get() {
            static War3Events instance;
            return instance;
        }
        
        // ==========================================
        // OnJassReady 事件
        // Jass 引擎初始化完成，Storm.dll 可以安全调用
        // ==========================================
        
        /// 注册 OnJassReady 回调 (仅触发一次)
        void registerOnJassReady(Callback cb) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_jassReady) {
                if (cb) cb();
            } else {
                m_onJassReadyCallbacks.push_back(std::move(cb));
            }
        }
        
        /// 触发 OnJassReady 事件 (由 Hook 调用)
        void fireOnJassReady() {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_jassReady) return;
            m_jassReady = true;
            
            for (auto& cb : m_onJassReadyCallbacks) {
                if (cb) cb();
            }
            m_onJassReadyCallbacks.clear();
        }
        
        /// Jass 是否已就绪
        bool isJassReady() const { return m_jassReady; }
        
        // ==========================================
        // OnGameStart 事件
        // 地图加载完成，进入游戏 (可安全访问 MPQ/Storm)
        // ==========================================
        
        /// 注册 OnGameStart 回调 (仅触发一次)
        void registerOnGameStart(Callback cb) {
            std::lock_guard<std::mutex> lock(m_mutex);
            
            // DEBUG
            char buf[256];
            snprintf(buf, sizeof(buf), "[War3Events] registerOnGameStart called, m_gameStarted=%d\n", 
                     m_gameStarted ? 1 : 0);
            OutputDebugStringA(buf);
            
            if (m_gameStarted) {
                OutputDebugStringA("[War3Events] Already started, calling cb immediately\n");
                if (cb) cb();
            } else {
                OutputDebugStringA("[War3Events] Queuing callback\n");
                m_onGameStartCallbacks.push_back(std::move(cb));
            }
        }
        
        /// 触发 OnGameStart 事件 (由 Hook 调用)
        void fireOnGameStart() {
            std::lock_guard<std::mutex> lock(m_mutex);
            
            // DEBUG: 记录调用
            OutputDebugStringA("[War3Events] fireOnGameStart called\n");
            char buf[256];
            snprintf(buf, sizeof(buf), "[War3Events] m_gameStarted=%d, callbacks=%zu\n", 
                     m_gameStarted ? 1 : 0, m_onGameStartCallbacks.size());
            OutputDebugStringA(buf);
            
            if (m_gameStarted) return;
            m_gameStarted = true;
            dxvk::war3::tools::ExportRuntimeStatusSnapshot("War3Events/OnGameStart", 0);
            
            OutputDebugStringA("[War3Events] Firing OnGameStart callbacks...\n");
            for (auto& cb : m_onGameStartCallbacks) {
                if (cb) cb();
            }
            m_onGameStartCallbacks.clear();
            OutputDebugStringA("[War3Events] OnGameStart complete\n");
        }
        
        /// 游戏是否已开始
        bool isGameStarted() const { return m_gameStarted; }

        // ==========================================
        // OnGameTick 事件
        // 游戏 Tick（来自 NetEvent）
        // ==========================================

        /// 注册 OnGameTick 回调
        void registerOnGameTick(TickCallback cb) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (cb) {
                m_onGameTickCallbacks.push_back(std::move(cb));
            }
        }

        /// 触发 OnGameTick 事件
        void fireOnGameTick(uint32_t stamp) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastGameTick = stamp;
            for (auto& cb : m_onGameTickCallbacks) {
                if (cb) cb(stamp);
            }
        }

        /// 获取最后一次 Tick 时间戳
        uint32_t getLastGameTickStamp() const { return m_lastGameTick; }

        // ==========================================
        // OnGameExit 事件
        // 游戏退出（离开地图）
        // ==========================================

        /// 注册 OnGameExit 回调 (可能多次触发)
        void registerOnGameExit(Callback cb) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (cb) {
                m_onGameExitCallbacks.push_back(std::move(cb));
            }
        }

        /// 触发 OnGameExit 事件
        void fireOnGameExit() {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& cb : m_onGameExitCallbacks) {
                if (cb) cb();
            }
        }
        
        /// 重置状态 (游戏退出时调用)
        void reset() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_jassReady = false;
            m_gameStarted = false;
            m_onJassReadyCallbacks.clear();
            m_onGameStartCallbacks.clear();
            m_onGameTickCallbacks.clear();
            m_onGameExitCallbacks.clear();
            m_lastGameTick = 0;
            dxvk::war3::tools::ResetRuntimeReadySignals();
            dxvk::war3::tools::ExportRuntimeStatusSnapshot("War3Events/Reset", 0);
        }
        
    private:
        War3Events() = default;
        
        std::mutex m_mutex;
        bool m_jassReady = false;
        bool m_gameStarted = false;
        uint32_t m_lastGameTick = 0;
        std::vector<Callback> m_onJassReadyCallbacks;
        std::vector<Callback> m_onGameStartCallbacks;
        std::vector<TickCallback> m_onGameTickCallbacks;
        std::vector<Callback> m_onGameExitCallbacks;
    };

} // namespace dxvk::war3
