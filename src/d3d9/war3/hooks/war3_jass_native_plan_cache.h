#pragma once

#include "../core/war3_internal_test_config.h"

#include <array>
#include <cstdint>

namespace dxvk::war3::hooks {

/**
 * @brief GetTLSJassData 函数指针类型。
 */
using GetTlsJassDataFn = void *(__fastcall *)(void *);

/**
 * @brief RegFuncAddr2Handle 函数指针类型。
 */
using RegFuncAddr2HandleFn = uint32_t(__thiscall *)(void *, uint32_t);

/**
 * @brief ComputeHandleMemoryAddr 函数指针类型。
 */
using ComputeHandleMemoryAddrFn = uint32_t(__thiscall *)(void *, uint32_t);

/**
 * @brief Task-4 Native 调用辅助函数集合。
 */
struct NativeCallHelperFns {
  GetTlsJassDataFn getTlsJassData = nullptr;
  RegFuncAddr2HandleFn regFuncAddr2Handle = nullptr;
  ComputeHandleMemoryAddrFn computeHandleMemoryAddr = nullptr;
};

/**
 * @brief Native 参数转换操作类型。
 */
enum class NativeArgOp : uint8_t {
  DirectValue = 0,
  ZeroIfType3AndNotC,
  FuncAddrToHandle,
  HandleToMemoryAddr,
  RealByRefTemp,
  HandleSigSkipToSemicolon,
};

/**
 * @brief 单参数的计划条目。
 */
struct NativeArgPlan {
  NativeArgOp op = NativeArgOp::DirectValue;
  uint8_t sigChar = 0;
  uint8_t sigAdvance = 1;
  uint8_t reserved = 0;
};

/**
 * @brief Native 调用计划。
 */
struct NativeCallPlan {
  void *nativeEntry = nullptr;
  void *funcPtr = nullptr;
  const char *sigPtr = nullptr;
  uint32_t paramCount = 0;
  uint32_t retType = 0;
  uint32_t scratchCount = 0;
  std::array<NativeArgPlan, dxvk::war3::internal::kNativeJassNativeCallMaxArgs>
      argOps = {};
};

/**
 * @brief 配置 Native 调用辅助函数地址。
 * @param helpers 辅助函数集合。
 */
void ConfigureNativeCallHelperFns(const NativeCallHelperFns &helpers);

/**
 * @brief 清空 Native 调用计划缓存。
 */
void ResetNativeCallPlanCaches();

/**
 * @brief 记录一次 fast-path 回退。
 */
void RecordNativeCallFallback();

/**
 * @brief 从缓存构建或获取 Native 调用计划。
 *
 * @param vm JASS VM 指针（当前版本仅用于一致性保留）。
 * @param nativeEntry GetTLSJassData 返回的 native 元信息指针。
 * @param out 输出计划。
 * @return true 成功；false 表示需要回退原始路径。
 */
bool BuildOrGetNativeCallPlan(void *vm, void *nativeEntry, NativeCallPlan &out);

/**
 * @brief 执行 Native 调用快路径。
 *
 * @param vm JASS VM 指针。
 * @param plan 已构建的调用计划。
 * @param ret 输出返回值。
 * @return true 执行成功；false 表示需要回退原始路径。
 */
bool ExecuteNativeCallFast(void *vm, const NativeCallPlan &plan, int &ret);

} // namespace dxvk::war3::hooks
