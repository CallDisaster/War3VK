// war3_model_hook.h - 魔兽模型加载探针与路径重映射
#pragma once

#include <cstdint>

namespace dxvk {
namespace war3 {
namespace model {

// 初始化模型 Hook（仅在 1.27a 偏移下验证）
void Init(uintptr_t gameBase);

// 关闭/重置状态（当前仅占位）
void Shutdown();

// 查询是否已启用模型 Hook
bool IsActive();

// 查询高成本的 pose / matrix 运行时 Hook 是否启用
bool IsPoseHookEnabled();

} // namespace model
} // namespace war3
} // namespace dxvk
