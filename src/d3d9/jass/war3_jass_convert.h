#pragma once
#include "war3_types.h"
#include "war3_jass_types.h"
#include "war3_game.h"
#include <string>

struct CStringRep;
struct RCString;
struct OPCode;

namespace jass {
  namespace convert {
    jString __fastcall to_jString(const std::string& str);
    jString __fastcall to_jString(const char* str);
    const char* __fastcall to_CString(jString sh);

    jCode __fastcall to_jCode(OPCode* code);
    OPCode* __fastcall to_opcode(jCode code);

    const CStringRep* to_CStringRep(jString sh);
    const RCString* to_RCString(jString sh);

    jHandle __fastcall to_handle(void* obj, uint32_t inherit = 0);
    jHandle __fastcall inquire_handle(void* obj, uint32_t inherit = 0);

    template <typename R>
    inline R to_object(jHandle handle, uint32_t inherit = 0) {
      if (!jass::is_handle_valid(handle))
        return (R)0;

      auto& table = game_state->handle_table;
      const uint32_t index = handle - 0x100000;
      if (index >= table.size)
        return (R)0;

      R res = (R)table.handle_array[index].object;
      if (inherit && res && !jass::check_inherit((void*)res, inherit))
        return (R)0;
      return res;
    }

    // 类型转字符串（调试用）
    std::string type_to_str(uint32_t type);
  }
}

void jass_convert_init();
