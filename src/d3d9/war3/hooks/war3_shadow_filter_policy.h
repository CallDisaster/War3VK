#pragma once

#include <cstddef>
#include <cstdint>
#include "war3_hook_shadow.h"

namespace dxvk::war3::hooks::shadowfilter {

/**
 * @brief RegisterImage 来源枚举转文本。
 */
const char* ToString(ShadowRegisterSource source);

/**
 * @brief RegisterImage owner 类型转文本。
 */
const char* ToString(ShadowOwnerKind kind);

/**
 * @brief RegisterImage 统一策略入口。
 */
ShadowRegisterDecision DecideRegisterImage(const ShadowRegisterContext& ctx);

/**
 * @brief 安全读取 ASCII C 字符串。
 *
 * @param src 源字符串地址（来自游戏内存）。
 * @param dst 输出缓冲区。
 * @param dstSize 输出缓冲区大小（必须 >= 2）。
 * @return true 读取成功且得到非空字符串；false 表示读取失败或非 ASCII 可打印串。
 */
bool ReadAsciiCStringSafe(const char* src, char* dst, size_t dstSize);

/**
 * @brief 判断 Ubersplat key 是否命中阴影黑名单（大小写不敏感）。
 *
 * @param key 投影 key 字符串。
 * @return true 需要拦截；false 允许通过。
 */
bool IsBlockedShadowKey(const char* key);

/**
 * @brief 判断 FourCC 是否命中阴影黑名单。
 *
 * 兼容第二字符大小写差异（例如 `YTlc`/`Ytlc`）。
 *
 * @param fourcc 待检查 FourCC。
 * @return true 需要拦截；false 允许通过。
 */
bool IsBlockedFourCC(uint32_t fourcc);

/**
 * @brief 从阴影对象参数中提取 FourCC。
 *
 * 解析顺序：
 * 1. `obj + 0x30` 直接读取；
 * 2. 按 `CUnit` 读取；
 * 3. 按 `CAgent -> UnitPtr -> CUnit` 读取。
 *
 * @param obj 阴影对象指针。
 * @param outFourCC 输出 FourCC。
 * @return true 提取成功；false 提取失败。
 */
bool TryExtractShadowObjectFourCC(void* obj, uint32_t& outFourCC);

/**
 * @brief 低频记录投影 key 样本，用于逆向定位静态建筑阴影来源。
 *
 * @param key 采样 key。
 */
void RecordProjectorKeySample(const char* key);

}  // namespace dxvk::war3::hooks::shadowfilter
