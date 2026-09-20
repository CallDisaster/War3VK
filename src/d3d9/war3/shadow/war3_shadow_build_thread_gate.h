#pragma once
#include <cstdint>

// 2026-09-17 上级裁定（线程修复方案 B 第一部分）：构建推进的所有者判定。
//
// 所有者身份来自 hook 生命周期观测到的主循环线程（GetMainLoopThreadId()），
// **绝不是"第一个请求线程"**：请求线程只表达需求，不能认领推进权。
// ownerThreadId == 0 表示所有者尚未建立（例如 Reset 之后尚未重新建立），
// ⇒ 一律拒绝推进；调用方只能安全排队或明确拒绝，不得消费构建。
// ownerThreadId != 0 且 currentThreadId != ownerThreadId ⇒ 非所有者同样只排队 / 拒绝。
//
// 该判定是纯函数，被抽到本轻量头（不依赖 d3d9 / 游戏状态 / 线程运行时），
// 供生产推进边界与宿主机边界测试共用同一份实现。
namespace dxvk::war3::shadow {

enum class ShadowBuildAdvanceDecision {
  Allow = 0,
  OwnerUnestablished = 1,
  NotOwner = 2,
};

inline ShadowBuildAdvanceDecision DecideShadowBuildAdvance(
    uint32_t ownerThreadId, uint32_t currentThreadId) {
  if (ownerThreadId == 0u)
    return ShadowBuildAdvanceDecision::OwnerUnestablished;
  if (currentThreadId != ownerThreadId)
    return ShadowBuildAdvanceDecision::NotOwner;
  return ShadowBuildAdvanceDecision::Allow;
}

} // namespace dxvk::war3::shadow
