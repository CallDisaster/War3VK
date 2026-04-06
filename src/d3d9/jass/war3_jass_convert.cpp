#include "war3_jass_convert.h"
#include "war3_game.h"
#include <unordered_map>

namespace jass {
  namespace convert {
    static uintptr_t tojString = 0;
    static uintptr_t jHandleToCObject = 0;
    static uintptr_t CObjectTojHandle = 0;
    static uintptr_t jHandleToCUnit = 0;
    static uintptr_t jHandleToCGroup = 0;
    static uintptr_t jHandleToCPlayerWar3 = 0;
    static uintptr_t jHandleToCTriggerWar3 = 0;

    static const std::unordered_map<uint32_t, std::string> kTypeName = {
      { '+dlb', "对话框按钮" },
      { '+dlg', "对话框" },
      { '+w3d', "可破坏物" },
      { '+EIP', "特效" },
      { '+EIm', "特效" },
      { '+EIf', "特效" },
      { 'vevt', "事件" },
      { 'tmvt', "事件" },
      { 'tmet', "事件" },
      { 'gfvt', "事件" },
      { 'devt', "事件" },
      { 'bevt', "事件" },
      { 'gevt', "事件" },
      { '+rev', "事件" },
      { '+tev', "事件" },
      { 'pevt', "事件" },
      { 'alvt', "事件" },
      { 'psvt', "事件" },
      { 'pcvt', "事件" },
      { 'wdvt', "事件" },
      { 'usvt', "事件" },
      { 'uevt', "事件" },
      { 'rgvt', "事件" },
      { '+flt', "过滤器" },
      { '+fgm', "可见度修正器" },
      { '+frc', "玩家组" },
      { '+grp', "单位组" },
      { 'ghth', "哈希表" },
      { 'item', "物品" },
      { '+loc', "点" },
      { '+mdb', "多面板" },
      { '+ply', "玩家" },
      { '+rct', "矩形区域" },
      { '+agr', "不规则区域" },
      { '+snd', "声音" },
      { '+tmr', "计时器" },
      { '+tid', "计时器窗口" },
      { '+trg', "触发器" },
      { '+tac', "触发器动作" },
      { 'tcnd', "触发器条件" },
      { '+w3u', "单位" },
      { 'pool', "单位池" },
      { 'ipol', "物品池" },
      { '+mbi', "多面板项目" },
    };
  }
}

using namespace jass;
using namespace jass::convert;

static inline bool is_handle_valid_local(jHandle h) {
  return jass::is_handle_valid(h);
}

jString __fastcall convert::to_jString(const std::string& str) {
  if (!tojString || str.empty())
    return 0;
  return call_fast<jString>(tojString, str.c_str());
}

jString __fastcall convert::to_jString(const char* str) {
  if (!tojString || !str || !*str)
    return 0;
  return call_fast<jString>(tojString, str);
}

const char* __fastcall convert::to_CString(jString sh) {
  const auto* rep = to_CStringRep(sh);
  if (!rep || !rep->str)
    return "";
  return rep->str;
}

jCode __fastcall convert::to_jCode(OPCode* code) {
  auto* jvm = jass::get_main_thread();
  return jvm ? jvm->to_jCode(code) : 0;
}

OPCode* __fastcall convert::to_opcode(jCode code) {
  auto* jvm = jass::get_main_thread();
  return jvm ? jvm->to_opcode(code) : nullptr;
}

const CStringRep* convert::to_CStringRep(jString sh) {
  auto* jvm = jass::get_main_thread();
  if (!jvm)
    return nullptr;
  auto* table = jvm->get_string_table();
  if (!table || sh >= table->size || !table->arr)
    return nullptr;
  return table->arr[sh].str.string_rep;
}

const RCString* convert::to_RCString(jString sh) {
  auto* jvm = jass::get_main_thread();
  if (!jvm)
    return nullptr;
  auto* table = jvm->get_string_table();
  if (!table || sh >= table->size || !table->arr)
    return nullptr;
  return &table->arr[sh].str;
}

jHandle __fastcall convert::to_handle(void* obj, uint32_t inherit) {
  if (!obj || (inherit && !jass::check_inherit(obj, inherit)))
    return 0;

  if (CObjectTojHandle) {
    return call_fast<jHandle>(CObjectTojHandle, jass::get_data_node(), obj, 0);
  }

  return inquire_handle(obj, inherit);
}

jHandle __fastcall convert::inquire_handle(void* obj, uint32_t inherit) {
  if (!obj || !game_state || (inherit && !jass::check_inherit(obj, inherit)))
    return 0;

  const auto& table = game_state->handle_table;
  for (uint32_t i = 0; i < table.size; ++i) {
    if (table.handle_array[i].object == obj)
      return 0x100000 + i;
  }
  return 0;
}

std::string convert::type_to_str(uint32_t type) {
  auto it = kTypeName.find(type);
  if (it != kTypeName.end())
    return it->second;

  char buf[5] = {};
  buf[0] = static_cast<char>(type & 0xFF);
  buf[1] = static_cast<char>((type >> 8) & 0xFF);
  buf[2] = static_cast<char>((type >> 16) & 0xFF);
  buf[3] = static_cast<char>((type >> 24) & 0xFF);
  return std::string(buf);
}

void jass_convert_init() {
  if (!pGameDLL)
    return;

  switch (game_version) {
    case 0x24e:
      tojString             = pGameDLL + 0x3BB560;
      jHandleToCObject      = pGameDLL + 0x428B90;
      CObjectTojHandle      = pGameDLL + 0x4317C0;
      jHandleToCUnit        = pGameDLL + 0x3BE7F0;
      jHandleToCGroup       = pGameDLL + 0x0;
      jHandleToCPlayerWar3  = pGameDLL + 0x0;
      jHandleToCTriggerWar3 = pGameDLL + 0x0;
      break;
    case 0x26a:
      tojString             = pGameDLL + 0x3BAA20;
      jHandleToCObject      = pGameDLL + 0x428050;
      CObjectTojHandle      = pGameDLL + 0x430C80;
      jHandleToCUnit        = pGameDLL + 0x3BDCB0;
      jHandleToCGroup       = pGameDLL + 0x3BEA30;
      jHandleToCPlayerWar3  = pGameDLL + 0x3BD4D0;
      jHandleToCTriggerWar3 = pGameDLL + 0x3BDEF0;
      break;
    case 0x27a:
      tojString             = pGameDLL + 0x1DA520;
      jHandleToCObject      = pGameDLL + 0x268380;
      CObjectTojHandle      = pGameDLL + 0x2651D0;
      jHandleToCUnit        = pGameDLL + 0x1D1550;
      jHandleToCGroup       = pGameDLL + 0x1CFB10;
      jHandleToCPlayerWar3  = pGameDLL + 0x1D03D0;
      jHandleToCTriggerWar3 = pGameDLL + 0x1D1410;
      break;
    default:
      break;
  }
}
