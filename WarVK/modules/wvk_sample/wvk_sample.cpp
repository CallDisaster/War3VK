// =============================================================================
// wvk_sample · WarVK 内置模块示例
//
// 构建（32 位、动态库、导出 WarVKModuleInit）：
//   cl /LD /std:c++20 /O2 wvk_sample.cpp /Fe:wvk_sample.dll
// 或 meson:
//   shared_library('wvk_sample', 'wvk_sample.cpp',
//                  cpp_std='c++20', vs_module_defs:'wvk_sample.def')
//
// 分发：把 wvk_sample.dll 改名放进地图导入（warvk_modules\wvk_sample.dll），
//       或放到 <游戏目录>\WarVK\modules\wvk_sample.dll。
//
// 地图侧：
//   call WarVKModuleLoad("wvk_sample")
//   set n = WarVKModuleCallInt("wvk_sample.add", WarVKModuleArgInt(2) + WarVKModuleArgInt(3))
//   set r = WarVKModuleCallReal("wvk_sample.scale", WarVKModuleArgInt(3) + WarVKModuleArgReal(1.5))
// =============================================================================

#include "../../src/d3d9/war3/japi/warvk_module_abi.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

const WarVKModuleHostV1* g_host = nullptr;

void Log(int level, const char* text) {
  if (g_host && g_host->log) g_host->log(g_host->host, level, text);
}

// wvk_sample.add(int, int) -> int
int32_t __cdecl InvokeAdd(void*, const WarVKModuleArgV1* args, int32_t argCount,
                          WarVKModuleValueV1* out) {
  if (argCount != 2 || args[0].type != WARVK_ARG_INT ||
      args[1].type != WARVK_ARG_INT)
    return WARVK_MODULE_E_ARGUMENT;
  out->kind = WARVK_RESULT_INT;
  out->i = args[0].i + args[1].i;
  return WARVK_MODULE_OK;
}

// wvk_sample.scale(int, real) -> real
int32_t __cdecl InvokeScale(void*, const WarVKModuleArgV1* args, int32_t argCount,
                            WarVKModuleValueV1* out) {
  if (argCount != 2 || args[0].type != WARVK_ARG_INT ||
      args[1].type != WARVK_ARG_REAL)
    return WARVK_MODULE_E_ARGUMENT;
  out->kind = WARVK_RESULT_REAL;
  out->r = static_cast<float>(args[0].i) * args[1].r;
  return WARVK_MODULE_OK;
}

// wvk_sample.echo(text) -> text
int32_t __cdecl InvokeEcho(void*, const WarVKModuleArgV1* args, int32_t argCount,
                           WarVKModuleValueV1* out) {
  static std::string buffer;
  if (argCount != 1 || args[0].type != WARVK_ARG_TEXT)
    return WARVK_MODULE_E_ARGUMENT;
  buffer = args[0].s ? args[0].s : "";
  out->kind = WARVK_RESULT_TEXT;
  out->s = buffer.c_str();
  return WARVK_MODULE_OK;
}

// wvk_sample.log(text) -> void
int32_t __cdecl InvokeLog(void*, const WarVKModuleArgV1* args, int32_t argCount,
                          WarVKModuleValueV1* out) {
  if (argCount != 1 || args[0].type != WARVK_ARG_TEXT)
    return WARVK_MODULE_E_ARGUMENT;
  Log(0, args[0].s ? args[0].s : "(null)");
  out->kind = WARVK_RESULT_VOID;
  return WARVK_MODULE_OK;
}

// wvk_sample.gameVersion() -> int  （演示宿主服务）
int32_t __cdecl InvokeGameVersion(void*, const WarVKModuleArgV1*,
                                  int32_t argCount, WarVKModuleValueV1* out) {
  if (argCount != 0) return WARVK_MODULE_E_ARGUMENT;
  WarVKGameInfoV1 info{};
  if (g_host && g_host->getGameInfo)
    g_host->getGameInfo(g_host->host, &info);
  out->kind = WARVK_RESULT_INT;
  out->i = static_cast<int32_t>(info.gameVersion);
  return WARVK_MODULE_OK;
}

const WarVKModuleCommandV1 kCommands[] = {
    {"wvk_sample.add", "ii", WARVK_RESULT_INT, &InvokeAdd, nullptr},
    {"wvk_sample.scale", "ir", WARVK_RESULT_REAL, &InvokeScale, nullptr},
    {"wvk_sample.echo", "s", WARVK_RESULT_TEXT, &InvokeEcho, nullptr},
    {"wvk_sample.log", "s", WARVK_RESULT_VOID, &InvokeLog, nullptr},
    {"wvk_sample.gameVersion", "", WARVK_RESULT_INT, &InvokeGameVersion, nullptr},
};

void __cdecl Shutdown(void*) { Log(0, "wvk_sample shutdown"); }

const WarVKModuleDescV1 kDesc = {
    WARVK_MODULE_ABI_VERSION,
    "wvk_sample",
    "1.0.0",
    kCommands,
    static_cast<uint32_t>(sizeof(kCommands) / sizeof(kCommands[0])),
    &Shutdown,
    nullptr,
};

}  // namespace

extern "C" __declspec(dllexport) int32_t __cdecl WarVKModuleInit(
    const WarVKModuleHostV1* host, const WarVKModuleDescV1** outDesc) {
  if (!host || !outDesc) return WARVK_MODULE_E_ARGUMENT;
  if (host->abiVersion != WARVK_MODULE_ABI_VERSION ||
      host->structSize < sizeof(WarVKModuleHostV1))
    return WARVK_MODULE_E_ABI;

  g_host = host;
  *outDesc = &kDesc;
  Log(0, "wvk_sample init");
  return WARVK_MODULE_OK;
}
