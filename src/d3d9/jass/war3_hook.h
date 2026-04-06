#pragma once
#include "war3_types.h"
#include "../../minhook/include/MinHook.h"
#include <unordered_map>

namespace hook {
	// detours replacement using MinHook
	namespace detours {
		using hook_info_t = struct _Hook_Info {
			uintptr_t*	jump_in;
			void*		jump_des;
            void*       original_target; // Store original target for Enable/Disable/Remove
		};
		using hook_table_t = std::unordered_map<uintptr_t*, hook_info_t>;

		bool create(uintptr_t* jump_in, void* jump_des);
		bool remove(uintptr_t* jump_in);
		void clear();
	}
}
