#pragma once

#include "war3_gpu_requirements.h"

#include <cstdint>
#include <string>

namespace dxvk {
class DxvkAdapter;
}

namespace dxvk::war3::platform {

/**
 * @brief 开局时按显存选好的档位，以及档位落到的具体设置。
 *
 * 只管“固定占用、不会自己随显存预算缩放”的设置。阴影 arena 和
 * Stage11 快照已经按驱动报告的预算增长，不在这里重复限制。
 */
struct VramPreset {
  gpureq::VramTier tier = gpureq::VramTier::High;
  std::string source;              ///< "auto"、"env" 或 "ini"
  std::string adapterName;
  uint64_t deviceLocalBytes = 0;   ///< 最大的 DEVICE_LOCAL 堆
  uint64_t budgetBytes = 0;        ///< VK_EXT_memory_budget 给出的预算，0 表示未知
  bool integrated = false;
  uint32_t csmResolutionCap = 4096;
};

/**
 * @brief 在创建 War3 渲染管线之前调用一次。重复调用无效。
 *
 * 玩家覆盖顺序：环境变量 DXVK_WAR3_VRAM_PRESET，其次游戏目录
 * WarVK\warvk.ini 的 [Graphics] VramPreset=auto|low|medium|high。
 */
void InitVramPreset(const DxvkAdapter& adapter);

/** 尚未初始化时返回 nullptr。 */
const VramPreset* CurrentVramPreset();

} // namespace dxvk::war3::platform
