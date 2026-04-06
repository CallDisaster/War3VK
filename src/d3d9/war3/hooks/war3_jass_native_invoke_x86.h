#pragma once

#include <cstdint>

namespace dxvk::war3::hooks {

/**
 * @brief 查询当前构建是否支持 cdecl 参数打包调用器。
 * @return true 表示支持；false 表示仅可回退原始路径。
 */
bool IsCdeclPackedInvokeSupported();

/**
 * @brief 使用 cdecl 约定按参数数组调用目标函数。
 *
 * @param fn 目标函数地址。
 * @param args 参数数组（按逻辑顺序从第 1 参到第 N 参）。
 * @param count 参数个数。
 * @return 目标函数返回值（EAX）。
 */
int InvokeCdeclPacked(void *fn, const uint32_t *args, uint32_t count);

} // namespace dxvk::war3::hooks
