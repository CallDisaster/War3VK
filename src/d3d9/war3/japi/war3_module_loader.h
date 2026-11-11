#pragma once

// =============================================================================
// WarVK 内置模块加载器（宿主侧）
//
// 职责：
//   1. 从地图导入（MPQ）或本地目录解析模块 DLL 字节；
//   2. PE/ABI/策略校验后 LoadLibrary，调用 WarVKModuleInit；
//   3. 把模块登记的命令接进 warvk:v1 字符串协议；
//   4. 换图 / JASS VM 重建时统一卸载。
//
// 刻意不做的事：
//   - 不修改 JASS 原生表，不写游戏内存，不注入线程；
//   - 不支持 64 位模块（war3 是 32 位进程）；
//   - 不对未在允许清单内的地图内模块放行（见策略说明）。
// =============================================================================

#include <cstdint>
#include <string_view>

#include "war3_japi_v1.h"

namespace dxvk::war3::japi::modules {

// 只做前缀判断，不解析参数；供 Dispatch 前置分流使用，开销 O(模块数)。
bool IsModulePayload(std::string_view payload) noexcept;

// 完整处理一条模块命令，返回与 warvk:v1 语义一致的 Reply。
// 非模块命令返回 Reply{consumed=false}，调用方应继续走原协议。
Reply Dispatch(Carrier carrier, std::string_view payload) noexcept;

// 换图 / JASS VM 重建：卸载全部模块并递增 mapEpoch。
void ResetAuthorState() noexcept;

// 诊断用。
uint32_t LoadedCount() noexcept;
const char* LastErrorText() noexcept;

} // namespace dxvk::war3::japi::modules
