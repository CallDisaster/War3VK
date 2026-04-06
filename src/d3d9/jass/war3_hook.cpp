#include "war3_hook.h"

namespace hook {
	namespace detours {
		static hook_table_t hook_table;

		bool create(uintptr_t* jump_in, void* jump_des) {
            if (!jump_in || !*jump_in || !jump_des) {
                // LOG_ERROR("Invalid hook parameters");
                return false;
            }

            if (hook_table.find(jump_in) != hook_table.end()) {
                // Already hooked
                return false;
            }

            void* target = (void*)*jump_in;
            
            // Initialize MinHook if needed (usually safe to call multiple times or handle globally, 
			// but we can assume it's initialized or we init it here. 
			// MH_Initialize returns MH_OK if already initialized? No, it might error.
			// Let's assume we initialize it once elsewhere or check status.
			// Ideally we call MH_Initialize() in global init.
			// But for safety:
			static bool mh_inited = false;
			if (!mh_inited) {
				if (MH_Initialize() != MH_OK) {
					// handle error
				}
				mh_inited = true;
			}

            // Create hook
            // MH_CreateHook(pTarget, pDetour, ppOriginal)
            // ppOriginal will be set to the trampoline
            MH_STATUS status = MH_CreateHook(target, jump_des, (LPVOID*)jump_in);
            if (status != MH_OK) {
                // LOG_ERROR("MH_CreateHook failed");
                return false;
            }

            status = MH_EnableHook(target);
            if (status != MH_OK) {
                // LOG_ERROR("MH_EnableHook failed");
                // Cleanup?
                MH_RemoveHook(target);
                return false;
            }

            auto& info = hook_table[jump_in];
            info.jump_in = jump_in;
            info.jump_des = jump_des;
            info.original_target = target;

            return true;
		}

		bool remove(uintptr_t* jump_in) {
            auto it = hook_table.find(jump_in);
            if (it == hook_table.end()) {
                return false;
            }

            void* target = it->second.original_target;
            
            MH_DisableHook(target);
            MH_RemoveHook(target);
            
            // Restore original pointer if MinHook overwrote it?
            // Wait, MinHook overwrote *jump_in with Trampoline.
            // If we remove hook, MinHook restores the code.
            // But *jump_in (the variable) still points to the trampoline.
            // We should restore *jump_in to point to the original target (although the trampoline is freed?).
            // MH_RemoveHook invalidates the trampoline.
            // So we MUST restore *jump_in to `target`.
            *jump_in = (uintptr_t)target;

            hook_table.erase(it);
            return true;
		}

		void clear() {
            for (auto& pair : hook_table) {
                void* target = pair.second.original_target;
                MH_DisableHook(target);
                MH_RemoveHook(target);
                *pair.first = (uintptr_t)target;
            }
            hook_table.clear();
		}
	}
}
