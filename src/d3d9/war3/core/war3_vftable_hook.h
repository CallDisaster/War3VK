#pragma once
#include <windows.h>
#include <cstdint>
#include <unordered_map>

namespace dxvk::war3 {

    /**
     * @class VFTableHook
     * @brief 虚函数表 Hook 工具类
     * 
     * 通过复制虚函数表并替换指定函数指针来实现 Hook。
     * 基于 MemHack 的 hook::vftable 实现。
     */
    class VFTableHook {
    public:
        struct HookInfo {
            void* object;        // 被 Hook 的对象
            void** realVtable;   // 原始虚函数表
            void* memory;        // 分配的内存 (包含 RTTI + 虚函数表)
            void** fakeVtable;   // 假的虚函数表 (指向 memory + 4)
        };
        
        /// 获取单例
        static VFTableHook& get() {
            // [FIX] Use leaky singleton to avoid destructor execution on process exit.
            // Destructor calls clear() which writes to game objects that might already be freed, causing crash.
            static VFTableHook* instance = new VFTableHook();
            return *instance;
        }
        
        /**
         * @brief 创建虚函数表 Hook
         * @param object 目标对象
         * @param length 虚函数表长度 (函数数量)
         * @param offset 要 Hook 的函数在表中的字节偏移 (index * 4)
         * @param detour Hook 函数
         * @return 原始函数指针
         */
        void* create(void* object, uint32_t length, uint32_t offset, void* detour) {
            if (!object || !detour) return nullptr;
            
            HookInfo* info = nullptr;
            auto it = m_hookTable.find(object);
            
            if (it == m_hookTable.end()) {
                info = new HookInfo();
                m_hookTable[object] = info;
                
                info->object = object;
                info->realVtable = *(void***)object;
                
                // 分配内存: RTTI (4 bytes) + 虚函数表 (length * 4 bytes)
                info->memory = new void*[length + 1];
                
                // 复制原始表 (包括 RTTI，它在 vtable 之前的 4 字节)
                memcpy(info->memory, 
                       (void*)((uintptr_t)info->realVtable - 4), 
                       4 * (length + 1));
                
                // fakeVtable 指向 memory + 4 (跳过 RTTI)
                info->fakeVtable = (void**)((uintptr_t)info->memory + 4);
                
                // 修改对象的虚函数表指针
                *(void***)(object) = info->fakeVtable;
            } else {
                info = it->second;
            }
            
            // 替换指定位置的函数指针
            uint32_t index = offset / 4;
            info->fakeVtable[index] = detour;
            
            // 返回原始函数
            return info->realVtable[index];
        }
        
        /**
         * @brief 获取原始函数指针
         * @param object 被 Hook 的对象
         * @param offset 函数在表中的字节偏移
         * @return 原始函数指针，失败返回 nullptr
         */
        void* getOriginal(void* object, uint32_t offset) {
            auto it = m_hookTable.find(object);
            if (it == m_hookTable.end()) return nullptr;
            return it->second->realVtable[offset / 4];
        }
        
        /**
         * @brief 移除 Hook
         * @param object 被 Hook 的对象
         */
        void remove(void* object) {
            auto it = m_hookTable.find(object);
            if (it == m_hookTable.end()) return;
            
            HookInfo* info = it->second;
            if (info) {
                // 恢复原始虚函数表
                *(void***)object = info->realVtable;
                
                delete[] (void**)info->memory;
                delete info;
            }
            m_hookTable.erase(it);
        }
        
        /// 清除所有 Hook
        void clear() {
            for (auto& pair : m_hookTable) {
                HookInfo* info = pair.second;
                if (info) {
                    *(void***)pair.first = info->realVtable;
                    delete[] (void**)info->memory;
                    delete info;
                }
            }
            m_hookTable.clear();
        }
        
    private:
        VFTableHook() = default;
        ~VFTableHook() { clear(); }
        
        std::unordered_map<void*, HookInfo*> m_hookTable;
    };

} // namespace dxvk::war3
