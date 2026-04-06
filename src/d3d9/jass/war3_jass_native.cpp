#include "war3_jass_native.h"
#include "war3_game.h"
#include "war3_hook.h"

namespace jass {
	namespace native {
		// 函数指针
		static uintptr_t cjStart;
		static uintptr_t cjEnd;
		static uintptr_t aiStart;
		static uintptr_t aiEnd;
		static uintptr_t addNativeFunction;
		static uintptr_t getNativeFuncStruct;

		// 函数表
		static native_map_t map_common;
		static native_map_t map_ai;

        void init_native_func();
        
        // 获取原生函数
		void* read_native_func(const string& name) {
            auto func = get_native_func(name);
            return func ? func->func.func_addr : nullptr;
        }

		// 添加原生函数
		void add_native_func(void* addr, const char* name, const char* info) {
            if (addNativeFunction)
			    call_fast<void>(addNativeFunction, addr, name, info);
		}

		// 获取原生函数结构
		NativeFuncStruct* get_native_func(const char* func_name) {
			return getNativeFuncStruct ? call_fast<NativeFuncStruct*>(getNativeFuncStruct, func_name) : nullptr;
		}

		// 获取原生函数结构
		NativeFuncStruct* get_native_func(const string& func_name) {
			return get_native_func(func_name.c_str());
		}

		// 获取cj函数表
		const native_map_t& get_map_common() {
			return map_common;
		}

		// 获取ai函数表
		const native_map_t& get_map_ai() {
			return map_ai;
		}
        
        void init_native_func() {
            if (cjStart && cjEnd && aiStart && aiEnd) {
                // cj function map
                for (size_t ptr = cjStart; (uint32_t)ptr <= cjEnd; ptr += 0x14) {
                    const char* name = *(const char**)(ptr);
                    void* addr = *(void**)(ptr + 0x5);
                    if (name) map_common[name] = addr;
                }

                // ai function map
                for (size_t ptr = aiStart; (uint32_t)ptr <= aiEnd; ptr += 0x14) {
                    const char* name = *(const char**)(ptr);
                    void* addr = *(void**)(ptr + 0x5);
                    if (name) map_ai[name] = addr;
                }
            }
        }
    }
}

using namespace jass::native;

void jass_native_init() {
	if (pGameDLL) {
		switch (game_version)
		{
		case(0x24e):
			cjStart = pGameDLL + 0x3D4B6B;
			cjEnd	= pGameDLL + 0x3DA683;
			aiStart = pGameDLL + 0x2E48B6;
			aiEnd	= pGameDLL + 0x2E5252;

			addNativeFunction	= pGameDLL + 0;
			getNativeFuncStruct = pGameDLL + 0;
			break;
		case(0x26a):
			cjStart = pGameDLL + 0x3D402B;
			cjEnd	= pGameDLL + 0x3D9B43;
			aiStart = pGameDLL + 0x2E3D96;
			aiEnd	= pGameDLL + 0x2E4732;

			addNativeFunction	= pGameDLL + 0;
			getNativeFuncStruct = pGameDLL + 0;
			break;
		case(0x27a):
			cjStart = pGameDLL + 0x1E9A5B;
			cjEnd	= pGameDLL + 0x1EF573;
			aiStart = pGameDLL + 0x892DC6;
			aiEnd	= pGameDLL + 0x893762;

			addNativeFunction	= pGameDLL + 0x7E3710;
			
            getNativeFuncStruct = pGameDLL + 0x7E2FE0; 
			break;
		default:
			return;
		}
		// 初始化原生函数
		init_native_func();
	}
}
