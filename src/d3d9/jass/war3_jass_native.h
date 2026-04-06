#pragma once
#include "war3_types.h"
#include "war3_game_struct.h"
#include <unordered_map>

struct RCString;

namespace jass {
	// 接口函数
	namespace native {
		using native_map_t = std::unordered_map<string, void*>;

		// 获取原生函数
		void*				read_native_func(const string& name);
		// 添加原生函数
		void				add_native_func(void* addr, const char* name, const char* info);
		// 获取原生函数结构
		NativeFuncStruct*   get_native_func(const char* func_name);
		// 获取原生函数结构
		NativeFuncStruct*   get_native_func(const string& func_name);
		
		// Hook原生函数 (Modify function pointer in table)
		template <typename R, class...Args>
		void* __fastcall				hook_native_func(const char* name, R(__cdecl* func)(Args...)) {
			NativeFuncStruct* jass_func_ptr = get_native_func(name);
			void* origin = nullptr;
			if (jass_func_ptr && func) {
				NativeFuncStruct& jass_func = *jass_func_ptr;
				origin = jass_func.func.func_addr;
				jass_func.func.func_addr = (void*)func;
			}
			return origin;
		}

		// 获取cj函数表
		const native_map_t&				get_map_common();
		// 获取ai函数表
		const native_map_t&				get_map_ai();
	}
}

void jass_native_init();
