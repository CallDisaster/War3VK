#pragma once
#include <cstdint>
#include <functional>
#include <cstdlib>
#include "../core/war3_events.h"
#include "../core/war3_vftable_hook.h"
#include "../../util/log/log.h"
#include "../../jass/war3_jass_types.h"
#include "../../jass/war3_game.h"
#include "../render/war3_render_state.h"

namespace dxvk::war3 {

    /**
     * @brief 网络事件 ID 定义 (来自 MemHack)
     */
    enum NetEventId : uint32_t {
        NET_EVENT_PLAYER_LEAVE = 0x4009007A, // 玩家离开
        NET_EVENT_GAME_START   = 0x4009007E, // 游戏开始
        NET_EVENT_GAME_READY   = 0x4009007F, // 游戏就绪 (地图加载完成)
        NET_EVENT_GAME_LEAVE   = 0x40090081, // 游戏离开
        NET_EVENT_GAME_TICK    = 0x40090092, // 游戏 Tick
        NET_EVENT_GAME_IDLETICK= 0x40090093, // 游戏 Idle Tick
    };

    /**
     * @class NetEventHook
     * @brief 网络事件 Hook - 用于捕获游戏开始事件
     * 
     * 基于 MemHack 的 net_event_hook 实现。
     * Hook jass::get_instance(0xD) 返回对象的第6个虚函数，
     * 拦截 NET_EVENT_GAME_READY 事件来触发 OnGameStart。
     */
    class NetEventHook {
    public:
        // 网络事件处理函数类型 (thiscall)
        using OriginalHandler_t = uint32_t(__thiscall*)(void* thisPtr, uint32_t evt);
        
        /// 获取单例
        static NetEventHook& get() {
            static NetEventHook instance;
            return instance;
        }
        
        /// 设置运行时就绪标志（在第一次 JASS 函数执行时调用）
        void SetRuntimeReady() {
            if (!m_runtimeReady) {
                m_runtimeReady = true;
                OutputDebugStringA("[NetEventHook] SetRuntimeReady: JASS VM ready, game time fetch enabled\n");
            }
        }
        
        /// 查询运行时是否就绪
        bool IsRuntimeReady() const { return m_runtimeReady; }
        
        /// 初始化 Hook
        bool init() {
            if (m_initialized) return true;
            
            OutputDebugStringA("[NetEventHook] Initializing...\n");
            
            // 1. 获取网络实例 jass::get_instance(0xD)
            // 先检查函数指针是否有效
            if (!::pGameDLL) {
                OutputDebugStringA("[NetEventHook] Failed: pGameDLL is null\n");
                return false;
            }
            // 这里我们手动检查 getInstance 函数指针，假设它被导出了或者可访问
            // 但 jass::get_instance 内部会使用 jass::getInstance
            
            void* ins = jass::get_instance(0xD);
            if (!ins) {
                OutputDebugStringA("[NetEventHook] Failed: get_instance(0xD) returned null\n");
                return false;
            }
            
            char buf[256];
            snprintf(buf, sizeof(buf), "[NetEventHook] get_instance(0xD) = %p\n", ins);
            OutputDebugStringA(buf);
            
            // 检查内存可读性
            if (IsBadReadPtr(ins, 0x20)) {
                 OutputDebugStringA("[NetEventHook] Failed: ins is bad read ptr\n");
                 return false;
            }

            // 2. 通过偏移获取网络数据对象
            // ins + 0x10 -> data
            void* data = *(void**)((uintptr_t)ins + 0x10);
            if (!data) {
                OutputDebugStringA("[NetEventHook] Failed: data is null\n");
                return false;
            }
            
            if (IsBadReadPtr(data, 0x10)) {
                 OutputDebugStringA("[NetEventHook] Failed: data is bad read ptr\n");
                 return false;
            }
            
            // data + 0x8 -> net_data
            m_netData = *(void**)((uintptr_t)data + 0x8);
            if (!m_netData) {
                OutputDebugStringA("[NetEventHook] Failed: net_data is null\n");
                return false;
            }
            
            if (IsBadReadPtr(m_netData, 0x10)) {
                 OutputDebugStringA("[NetEventHook] Failed: m_netData is bad read ptr\n");
                 return false;
            }
            
            // net_data + 0x8 -> net_obs
            void* net_obs = (void*)((uintptr_t)m_netData + 0x8);
             if (IsBadReadPtr(net_obs, 0x8)) {
                 OutputDebugStringA("[NetEventHook] Failed: net_obs (ptr to ptr) is bad read ptr\n");
                 return false;
            }
            
            // net_obs + 0x4 -> osb_obj (要 Hook 的对象)
            // 注意: MemHack主要使用 pointer_calc, 意味着它是一个成员对象偏移，而不是指针
            m_osbObj = (void*)((uintptr_t)net_obs + 0x4);
            if (!m_osbObj) {
                OutputDebugStringA("[NetEventHook] Failed: osb_obj is null\n");
                return false;
            }
            
            snprintf(buf, sizeof(buf), "[NetEventHook] osb_obj = %p\n", m_osbObj);
            OutputDebugStringA(buf);
            
            // 3. Hook 虚函数表 (offset = 0x10, 第5个函数)
            // MemHack: vftable::create(osb_obj, 6, 0x10, handler)
            m_originalFunc = (OriginalHandler_t)VFTableHook::get().create(
                m_osbObj, 
                6,        // 虚函数表长度
                0x10,     // 偏移: 0x10 / 4 = 4 (第5个虚函数)
                (void*)&NetEventHook::Handler
            );
            
            if (!m_originalFunc) {
                OutputDebugStringA("[NetEventHook] Failed: VFTable hook failed\n");
                return false;
            }
            
            snprintf(buf, sizeof(buf), "[NetEventHook] Hook success! original = %p\n", m_originalFunc);
            OutputDebugStringA(buf);
            
            m_initialized = true;
            return true;
        }
        
        /// 清理
        void cleanup() {
            if (m_osbObj) {
                VFTableHook::get().remove(m_osbObj);
                m_osbObj = nullptr;
            }
            m_initialized = false;
            m_originalFunc = nullptr;
            m_runtimeReady = false;
        }
        
    private:
        NetEventHook() = default;
        
        /// Hook 处理函数 (静态，因为需要作为函数指针)
        static uint32_t __fastcall Handler(void* thisPtr, uint32_t /*edx*/, uint32_t evt) {
            auto& self = get();
            
            if (thisPtr && evt) {
                // 获取事件 ID (evt + 0x8)
                uint32_t eventId = *(uint32_t*)((uintptr_t)evt + 0x8);
                
                if (IsVerboseLogEnabled()) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "[NetEventHook] Event ID = %u\n", eventId);
                    OutputDebugStringA(buf);
                }
                
                switch (eventId) {
                case NET_EVENT_GAME_READY: {
                    if (IsVerboseLogEnabled())
                        OutputDebugStringA("[NetEventHook] NET_EVENT_GAME_READY detected!\n");

                    if (!self.m_runtimeReady) {
                        self.m_runtimeReady = true;
                        OutputDebugStringA("[NetEventHook] SetRuntimeReady: Game ready event, time fetch enabled\n");
                    }
                    
                    War3Events::get().fireOnGameStart();

                    uint32_t rv = self.m_originalFunc(thisPtr, evt);
                    return rv;
                }
                case NET_EVENT_GAME_TICK:
                case NET_EVENT_GAME_IDLETICK: {
                    if (self.m_netData && !IsBadReadPtr(self.m_netData, 0x1C74)) {
                        uint32_t tick = *(uint32_t*)((uintptr_t)self.m_netData + 0x1C70);
                        War3Events::get().fireOnGameTick(tick);
                    }
                    break;
                }
                case NET_EVENT_GAME_LEAVE: {
                    if (IsVerboseLogEnabled())
                        OutputDebugStringA("[NetEventHook] NET_EVENT_GAME_LEAVE detected!\n");
                    uint32_t rv = self.m_originalFunc(thisPtr, evt);
                    War3Events::get().fireOnGameExit();
                    War3Events::get().reset();
                    self.m_runtimeReady = false;
                    return rv;
                }
                    
                default:
                    break;
                }
            }
            
            // 调用原始函数
            return self.m_originalFunc(thisPtr, evt);
        }
        
        bool m_initialized = false;
        void* m_netData = nullptr;
        void* m_osbObj = nullptr;
        OriginalHandler_t m_originalFunc = nullptr;
        bool m_runtimeReady = false;

        static bool IsVerboseLogEnabled() {
            static int s_cached = -1;
            if (s_cached < 0) {
                const char* env = std::getenv("DXVK_WAR3_NETEVENT_LOG");
                s_cached = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
            }
            return s_cached > 0;
        }

        void InitializeGameRuntime() {
            // 游戏相关初始化统一放在地图就绪点
            ::war3_init();
            War3Events::get().fireOnJassReady();
            // 注：m_runtimeReady 在第一次 JASS 函数执行时已经设置
            OutputDebugStringA("[NetEventHook] InitializeGameRuntime: GAME_READY event processed\n");
        }
    };

} // namespace dxvk::war3
