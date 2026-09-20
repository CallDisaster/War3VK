// 2026-09-17 上级裁定（线程修复方案 B 第一部分）：构建推进所有者门的宿主机边界测试。
//
// 被测对象是产品源里的同一份 inline 判定
// （war3_shadow_build_thread_gate.h: DecideShadowBuildAdvance）。生产的两处推进入口
// （ShadowValidationRuntime::ensureLatestFrameBuilt 与 ensureFrameBuiltForContract）
// 都调用它，因此"显式管道 drain"与"隐式 hot-wait"共用同一条判定路径，直接调用底层
// 推进入口也无法绕过。
//
// 本测试只断言判定结果，不做字符串静态检查；每例打印一行，全部通过打印 SUMMARY，
// 任一失败非零退出。

#include "../../shadow/war3_shadow_build_thread_gate.h"

#include <cstdint>
#include <cstdio>

namespace {

using dxvk::war3::shadow::DecideShadowBuildAdvance;
using dxvk::war3::shadow::ShadowBuildAdvanceDecision;

uint32_t g_failures = 0u;
uint32_t g_cases = 0u;

const char* ToText(ShadowBuildAdvanceDecision decision) {
  switch (decision) {
    case ShadowBuildAdvanceDecision::Allow:
      return "Allow";
    case ShadowBuildAdvanceDecision::OwnerUnestablished:
      return "OwnerUnestablished";
    case ShadowBuildAdvanceDecision::NotOwner:
      return "NotOwner";
  }
  return "Unknown";
}

void Check(const char* label, uint32_t ownerThreadId, uint32_t currentThreadId,
           ShadowBuildAdvanceDecision expected) {
  const ShadowBuildAdvanceDecision actual =
      DecideShadowBuildAdvance(ownerThreadId, currentThreadId);
  ++g_cases;
  const bool ok = actual == expected;
  if (!ok)
    ++g_failures;
  std::printf("%s owner=0x%08X current=0x%08X -> %s expected=%s %s\n", label,
              static_cast<unsigned>(ownerThreadId),
              static_cast<unsigned>(currentThreadId), ToText(actual),
              ToText(expected), ok ? "PASS" : "FAIL");
}

}  // namespace

int main() {
  // 1. 所有者未建立：owner==0 一律 OwnerUnestablished；**不得**把 current==0
  //    当成"自己是所有者"而放行。
  Check("C1 owner-unestablished current-zero", 0u, 0u,
        ShadowBuildAdvanceDecision::OwnerUnestablished);
  Check("C1 owner-unestablished current-nonzero", 0u, 1234u,
        ShadowBuildAdvanceDecision::OwnerUnestablished);

  // 2. 正常所有者推进：owner 已建立且 current==owner。
  Check("C2 owner-advance", 0x1A2Bu, 0x1A2Bu,
        ShadowBuildAdvanceDecision::Allow);

  // 3. 管道线程 drain（显式开关形态）：命名管道分离线程不得推进。
  Check("C3 pipe-thread-drain", 0x1A2Bu, 0x00FFu,
        ShadowBuildAdvanceDecision::NotOwner);

  // 4. 隐式 hot-wait 形态：owner!=0 且 current!=owner，判定路径与 C3 相同。
  Check("C4 implicit-hot-wait", 0x1A2Bu, 0x2C3Du,
        ShadowBuildAdvanceDecision::NotOwner);

  // 5. 直接调用底层推进入口 ensureFrameBuiltForContract：与入口门共用同一条判定，
  //    因此不能通过绕过 ensureLatestFrameBuilt 消费构建。
  Check("C5 direct-lower-entry", 0x1A2Bu, 0x7F10u,
        ShadowBuildAdvanceDecision::NotOwner);

  // 6. 未知 / 错误所有者：伪造或写坏的非零 owner id 不匹配任何当前线程。
  Check("C6 bogus-owner", 0xFFFFFFFFu, 0x1A2Bu,
        ShadowBuildAdvanceDecision::NotOwner);

  // 7. Reset：reset 把所有者置 0 后，旧 owner id 的线程不得沿用推进权；
  //    必须先由 hook 生命周期重新建立所有者。
  uint32_t ownerThreadId = 0x1A2Bu;
  Check("C7 pre-reset-owner-advance", ownerThreadId, ownerThreadId,
        ShadowBuildAdvanceDecision::Allow);
  ownerThreadId = 0u;
  Check("C7 post-reset-owner-unestablished", ownerThreadId, 0x1A2Bu,
        ShadowBuildAdvanceDecision::OwnerUnestablished);

  if (g_failures != 0u) {
    std::printf("war3_shadow_build_thread_gate_test: FAIL %u/%u case(s)\n",
                static_cast<unsigned>(g_failures),
                static_cast<unsigned>(g_cases));
    return 1;
  }
  std::printf(
      "SUMMARY: war3_shadow_build_thread_gate_test %u/%u case(s) passed\n",
      static_cast<unsigned>(g_cases), static_cast<unsigned>(g_cases));
  return 0;
}
