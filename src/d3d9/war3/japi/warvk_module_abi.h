#pragma once

// =============================================================================
// WarVK 内置模块 ABI v1
//
// 用途：地图作者/编辑器把自己写的 32 位 DLL 随地图一起分发（"内置 DLL"），
//       运行时由 WarVK 载入，DLL 里的 C++ 函数通过 warvk:v1 协议被 JASS 调用。
//
// 与 MemHack 的区别：
//   - MemHack 靠 AI 脚本 + 内存写入 + 打补丁替换 JASS native；
//   - 本 ABI 不碰 JASS 原生表、不写游戏内存、不注入代码，
//     模块只登记"命令名 -> 函数指针"，调用走 WarVK 已有的字符串/哈希表协议。
//
// 稳定性约定：
//   - 本头文件只依赖 <stdint.h>，C 与 C++ 均可用；
//   - 结构体只允许在末尾追加字段，并通过 structSize 自描述；
//   - 版本不匹配时加载器必须拒绝加载，不允许"尽量兼容"。
// =============================================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WARVK_MODULE_ABI_VERSION 1u

/* 模块描述/命令表里的字符串一律 UTF-8，生命周期必须 >= 模块句柄存活期。 */

typedef enum WarVKModuleStatus {
  WARVK_MODULE_OK = 0,
  WARVK_MODULE_E_ABI = 1,        /* ABI 版本不符 */
  WARVK_MODULE_E_ARGUMENT = 2,   /* 参数类型/数量不符 */
  WARVK_MODULE_E_RANGE = 3,      /* 数值越界 */
  WARVK_MODULE_E_STATE = 4,      /* 未初始化 / 已关闭 */
  WARVK_MODULE_E_UNSUPPORTED = 5,/* 当前环境不支持 */
  WARVK_MODULE_E_INTERNAL = 6,   /* 模块内部错误 */
  WARVK_MODULE_E_DENIED = 7      /* 策略拒绝（未授权模块） */
} WarVKModuleStatus;

/* 参数类型字符，与 warvk:v1 线上标记一致：i int32 / r float / d int32(句柄) /
   b bool(0|1) / s 文本。 */
typedef enum WarVKModuleArgType {
  WARVK_ARG_INT = 0,
  WARVK_ARG_REAL = 1,
  WARVK_ARG_HANDLE = 2,
  WARVK_ARG_BOOL = 3,
  WARVK_ARG_TEXT = 4
} WarVKModuleArgType;

typedef struct WarVKModuleArgV1 {
  int32_t type;      /* WarVKModuleArgType */
  int32_t i;         /* INT / HANDLE / BOOL */
  float r;           /* REAL */
  const char* s;     /* TEXT（UTF-8，仅调用期间有效，不得长期保存） */
} WarVKModuleArgV1;

typedef enum WarVKModuleResultKind {
  WARVK_RESULT_VOID = 0,
  WARVK_RESULT_INT = 1,
  WARVK_RESULT_REAL = 2,
  WARVK_RESULT_TEXT = 3
} WarVKModuleResultKind;

typedef struct WarVKModuleValueV1 {
  int32_t kind;      /* WarVKModuleResultKind */
  int32_t i;
  float r;
  const char* s;     /* TEXT 必须是模块内静态/长生命周期字符串 */
} WarVKModuleValueV1;

/* --------------------------------------------------------------------------
 * 宿主服务：模块初始化时拿到，之后可长期持有（宿主生命周期 >= 模块）。
 * -------------------------------------------------------------------------- */

typedef struct WarVKGameInfoV1 {
  uint32_t gameVersion;     /* 例如 0x01070000 表示 1.27.0.0 */
  uint32_t isInGame;        /* 0/1 */
  uint32_t isReplay;        /* 0/1 */
  uint32_t mapEpoch;        /* 每次换图 +1，模块据此清理自己按地图存活的状态 */
} WarVKGameInfoV1;

typedef struct WarVKModuleHostV1 {
  uint32_t abiVersion;      /* 必须 == WARVK_MODULE_ABI_VERSION */
  uint32_t structSize;      /* sizeof(WarVKModuleHostV1) */

  void* host;

  /* 日志：level 0=info 1=warn 2=error。文本 UTF-8。 */
  void (__cdecl *log)(void* host, int32_t level, const char* text);

  /* 读取文件：先 MPQ（地图内导入）后本地。成功返回 0，
     *outData 由宿主分配，模块用 freeFile 归还。 */
  int32_t (__cdecl *readFile)(void* host, const char* path,
                              void** outData, uint32_t* outSize);
  void (__cdecl *freeFile)(void* host, void* data);

  int32_t (__cdecl *getGameInfo)(void* host, WarVKGameInfoV1* out);

  /* 掩码，见 war3_japi_v1.h 的 kFeature* 定义。 */
  uint32_t (__cdecl *getHostFeatureFlags)(void* host);
} WarVKModuleHostV1;

/* --------------------------------------------------------------------------
 * 命令登记
 *
 * name       必须是 "<模块名>.<函数名>"，全小写，只含 [a-z0-9._-]，最长 63 字节。
 * argTypes   参数类型串，例如 "ir" 表示 (int, real)；空串表示无参数；最长 16 个。
 * resultKind WarVKModuleResultKind。
 * invoke     调用约定统一 __cdecl；返回 WarVKModuleStatus。
 * user       原样回传，通常填模块自己的上下文指针。
 * -------------------------------------------------------------------------- */

typedef int32_t (__cdecl *WarVKModuleInvokeV1)(
    void* user,
    const WarVKModuleArgV1* args,
    int32_t argCount,
    WarVKModuleValueV1* out);

typedef struct WarVKModuleCommandV1 {
  const char* name;
  const char* argTypes;
  int32_t resultKind;
  WarVKModuleInvokeV1 invoke;
  void* user;
} WarVKModuleCommandV1;

typedef struct WarVKModuleDescV1 {
  uint32_t abiVersion;
  const char* name;        /* 模块名，与命令前缀一致 */
  const char* version;     /* 任意版本串，仅用于诊断 */
  const WarVKModuleCommandV1* commands;
  uint32_t commandCount;   /* 上限 256 */
  /* 可选：换图/卸载前回调，用于释放 GPU、文件、线程等资源。可为 NULL。 */
  void (__cdecl *shutdown)(void* moduleUser);
  void* moduleUser;
} WarVKModuleDescV1;

/* --------------------------------------------------------------------------
 * 模块唯一必须导出的符号
 *
 * 调用时机：宿主已完成 PE 校验、LoadLibrary、依赖解析，尚未对地图暴露任何命令。
 * 返回 WARVK_MODULE_OK 且 desc 合法才算加载成功；否则宿主立即 FreeLibrary。
 * desc 指向模块内的静态存储，必须长期有效。
 * -------------------------------------------------------------------------- */

typedef int32_t (__cdecl *WarVKModuleInitV1Fn)(
    const WarVKModuleHostV1* host,
    const WarVKModuleDescV1** outDesc);

#define WARVK_MODULE_INIT_EXPORT "WarVKModuleInit"

#ifdef __cplusplus
} /* extern "C" */
#endif
