#pragma once

#include <cstdint>
#include "../render/war3_render_state.h"

namespace dxvk::war3::hooks {

/**
 * @brief 将渲染 stage 映射为 `War3BatchTag`。
 *
 * profile 语义：
 * - `0`：实测映射（当前默认）；
 * - `1`：传统映射（用于 A/B 对比）。
 *
 * @param stage 渲染阶段号。
 * @param profile 映射配置档位。
 * @return 对应的批次标签。
 */
War3BatchTag MapStageToTag(int stage, uint32_t profile);

/**
 * @brief 判断是否应抑制 stage 推断标签，改由 groupIdx 标签生效。
 *
 * @param stage 渲染阶段号。
 * @param profile 映射配置档位。
 * @param tagWorldByGroupIdx 是否启用 groupIdx 精确标记。
 * @return true 表示应抑制 stage 标签推断。
 */
bool ShouldSuppressStageTagByGroupMode(int stage, uint32_t profile,
                                       bool tagWorldByGroupIdx);

/**
 * @brief 判断标签是否属于“需要桥接对象追踪”的世界对象类。
 *
 * @param tag 待判断标签。
 * @return true 表示该标签需要桥接对象追踪。
 */
bool IsWorldBridgeTag(War3BatchTag tag);

} // namespace dxvk::war3::hooks

