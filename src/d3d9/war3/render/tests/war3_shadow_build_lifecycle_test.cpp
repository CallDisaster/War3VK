// 2026-09-17 上级裁定（codex 01a02e0b）：构建推进 / 发布生命周期的**生产共用**宿主机测试。
//
// 背景（上级原话要点）：此前的 9 个测试实际只调用线程判定函数
// （DecideShadowBuildAdvance），没有调用真实的推进 / 发布 / Reset 边界。本测试改为直接实例化
// 生产组件 dxvk::war3::shadow::ShadowBuildLifecycle（生产与测试**同一份实现**，
// 见 war3_shadow_build_lifecycle.h），并用**局部** std::atomic<uint64_t> 计数器驱动它，
// 因此覆盖的是真实边界逻辑，而不是判定的复制品。
//
// 2026-09-17 接线变更（主线程）：beginBuild 由 2 参改为 3 参
//   uint64_t beginBuild(const void* work, uint64_t frameSerial, uint64_t publishRevision)
// —— 开始/替换构建必须**绑定工作对象身份**并分配代际；随后一律以 (work, generation) 成对携带。
// 因此本测试的每个 beginBuild 调用都传入一个**测试内局部假工作对象**的地址（int workA/workB/…），
// 只用它的地址作为身份，不构造真实 ShadowValidationBuildWork。
//
// 覆盖范围：
//   C1 未知所有者（owner==0）：两种入口都被拒；ownerUnestablished 计数按调用次数递增，
//      direct 计数**只在直接入口**递增 ⇒ 两种入口分类不是天然互斥、不可相加成「总拒绝」。
//   C2 非所有者（owner=A, current=B）：off-thread 计数递增；入口 / 直接入口分别验证。
//   C3 正常所有者（owner=A, current=A）：两种入口都放行，三个计数都不变。
//   C4 正常推进：beginBuild(work, …) -> publishIfCurrent -> completeIfCurrent（工作身份 + 固定代际、
//      代际一致；building 期间 hasValues 为真）；**完成之后 isCurrent(work, gen) 必须为 false**。
//   C5 Reset 后旧工作不得重新发布（Reset 后 + 新 beginBuild 后都仍然失败）；取消之后 isCurrent 为 false。
//   C6 取消：cancelIfCurrent(work, gen) 之后同一个 (work, generation) 的 publish / complete / cancel
//      全部被拒，且 isCurrent(work, gen) 必须为 false（终态后不得再被当作当前工作回写）。
//   C7 替换构建：beginBuild(first) -> beginBuild(second) ⇒ 旧对被拒、新对成功；
//      完成之后 isCurrent 必须为 false。
//
// 2026-09-17 03:58 接线变更：底层代际门 publish / complete / cancel 已转 **private**，本测试因此
// 不再直接调用它们（那会编译失败），一律走公开的 publishIfCurrent / completeIfCurrent /
// cancelIfCurrent。理由：底层"只认代际"的状态机由 war3_shadow_build_progress_test.cpp 直接实例化
// ShadowBuildProgressState 逐入口覆盖，而本文件要证明的是运行时真正使用的 (work, generation) 成对入口。
//   C8 并发摘要读取：写者循环 publishIfCurrent（nextRecordIndex 递增），读者在 std::shared_mutex
//      保护下循环读 values()，断言读到的快照自洽（代际稳定、nextRecordIndex 单调不减、
//      frameSerial / publishRevision 与该代际一致、recordCount*8+chunkCount==nextRecordIndex）。
//   C9 **A/B 实际接线回归（Reset 路径，上级指定用例）**：A 开始 -> A 发布一次 -> A 暂停（不 complete）
//      -> reset() + 开始 B -> B 发布 -> A 尝试 publishIfCurrent/completeIfCurrent（都必须 false）
//      -> 断言 B 的全部公开状态逐字段不变 -> B 仍可正常 completeIfCurrent。另含「同一 work 换旧 token」
//      与 isCurrent(nullptr, gen) 的拒绝。
//   C10 **A/B 实际接线回归（替换路径）**：不 Reset，直接用 beginBuild(workB, …) 顶替在途 A；
//      A 的 publishIfCurrent/completeIfCurrent 同样必须 false，B 状态同样不变且可正常完成。
//   C11 空工作身份（beginBuild(nullptr, …)）不得被当作当前：isCurrent(nullptr, gen) 与
//      publishIfCurrent / completeIfCurrent / cancelIfCurrent 对 nullptr 与真实 work 一律 false。
//
// 不变量边界（不得越界宣称）：本测试只证明"该组件在给定调用序列下的返回值与摘要字段"，
// 不证明实机运行时已观察到唯一所有者推进，也不声称并发访问已消除竞争。
//
// 本测试只断言行为，不做字符串静态检查；每例打印一行，全部通过打印 SUMMARY，
// 任一失败非零退出。

#include "../../shadow/war3_shadow_build_lifecycle.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
// <mutex> 是 std::unique_lock 的**正式**归属头（libstdc++ 的 <shared_mutex> 不提供它）。
// 上级要求的 include 白名单（产品头 + <atomic>/<cstdint>/<cstdio>/<shared_mutex>/<thread>）
// 漏了这一项，而上级同时明确要求 C8 的写侧必须使用 unique_lock 才能与运行时同模式，
// 因此补入唯一一个必需的标准头；除此之外不引入任何头。
#include <mutex>
// 上级 03:58 裁定："接受固定次数测试，不要求凑满一秒；同时允许 <chrono> 用于计时或超时"。
// 因此这里用 <chrono> 实现运行器超时保护，并用 <cstdlib> 的 std::_Exit 在超时时立即终止。
#include <chrono>
#include <cstdlib>
#include <shared_mutex>
#include <thread>

namespace {

using dxvk::war3::shadow::ShadowBuildAdvanceCounters;
using dxvk::war3::shadow::ShadowBuildLifecycle;
using dxvk::war3::shadow::ShadowBuildProgressValues;

uint32_t g_failures = 0u;
uint32_t g_cases = 0u;

void Check(const char* label, bool ok) {
  ++g_cases;
  if (!ok)
    ++g_failures;
  std::printf("%s %s\n", label, ok ? "PASS" : "FAIL");
}

void CheckU64(const char* label, uint64_t actual, uint64_t expected) {
  ++g_cases;
  const bool ok = actual == expected;
  if (!ok)
    ++g_failures;
  std::printf("%s actual=%llu expected=%llu %s\n", label,
              static_cast<unsigned long long>(actual),
              static_cast<unsigned long long>(expected), ok ? "PASS" : "FAIL");
}

// 逐字段比较：一次发布所携带的全部字段必须整体相同（不得只比较其中几项而漏掉撕裂字段）。
bool SameValues(const ShadowBuildProgressValues& a,
                const ShadowBuildProgressValues& b) {
  return a.workGeneration == b.workGeneration &&
         a.frameSerial == b.frameSerial &&
         a.publishRevision == b.publishRevision &&
         a.nextRecordIndex == b.nextRecordIndex &&
         a.recordCount == b.recordCount && a.chunkCount == b.chunkCount &&
         a.totalBuildDurationUs == b.totalBuildDurationUs &&
         a.drawCount == b.drawCount;
}

// 局部计数器 + 指向它们的 ShadowBuildAdvanceCounters 视图。
// 生产侧传的是进程级全局原子；测试侧用局部原子，从而可以逐例断言拒绝分类。
struct LocalCounters {
  std::atomic<uint64_t> offThread{0u};
  std::atomic<uint64_t> ownerUnestablished{0u};
  std::atomic<uint64_t> directAdvance{0u};

  ShadowBuildAdvanceCounters View() {
    ShadowBuildAdvanceCounters counters;
    counters.offThreadRefused = &offThread;
    counters.ownerUnestablishedRefused = &ownerUnestablished;
    counters.directAdvanceRefused = &directAdvance;
    return counters;
  }
};

void RunUnknownOwnerCases() {
  // C1：所有者尚未建立（owner==0）。无论 currentThreadId 取什么（含 0）都必须拒绝：
  // owner==0 表示"还没有观测到主循环线程"，不得被解释成"自己是所有者"。
  LocalCounters counters;
  ShadowBuildLifecycle lifecycle;
  constexpr uint32_t kSomeThread = 0x1234u;

  const bool entryZero = lifecycle.consumeAllowed(
      /*directEntry=*/false, /*ownerThreadId=*/0u, /*currentThreadId=*/kSomeThread,
      counters.View());
  const bool directZero = lifecycle.consumeAllowed(
      /*directEntry=*/true, 0u, kSomeThread, counters.View());
  const bool entryZeroZero = lifecycle.consumeAllowed(
      /*directEntry=*/false, 0u, /*currentThreadId=*/0u, counters.View());
  const bool directZeroZero = lifecycle.consumeAllowed(
      /*directEntry=*/true, 0u, 0u, counters.View());
  const bool directZeroAgain = lifecycle.consumeAllowed(
      /*directEntry=*/true, 0u, kSomeThread, counters.View());

  Check("C1.unknown-owner-entry-denied", !entryZero);
  Check("C1.unknown-owner-direct-entry-denied", !directZero);
  Check("C1.unknown-owner-entry-current-zero-denied", !entryZeroZero);
  Check("C1.unknown-owner-direct-entry-current-zero-denied", !directZeroZero);
  Check("C1.unknown-owner-direct-entry-repeat-denied", !directZeroAgain);

  // 5 次调用全部落在 ownerUnestablished 分类：计数必须等于调用次数。
  const uint64_t ownerUnestablished = counters.ownerUnestablished.load();
  const uint64_t direct = counters.directAdvance.load();
  const uint64_t offThread = counters.offThread.load();
  CheckU64("C1.owner-unestablished-count-equals-call-count", ownerUnestablished, 5u);
  // direct 计数只在 directEntry==true 的 3 次调用上递增。
  CheckU64("C1.direct-count-only-on-direct-entry", direct, 3u);
  // 未知所有者不是"非所有者"：off-thread 分类必须保持不变。
  CheckU64("C1.off-thread-count-untouched-by-unknown-owner", offThread, 0u);
  // 分类不可相加：直接入口的拒绝同时计入 ownerUnestablished 与 direct，
  // 两者相加（5+3=8）大于真实拒绝次数（5）⇒ 不得相加成「总拒绝」。
  Check("C1.entry-classes-are-not-additive",
        ownerUnestablished + direct != 5u);
  std::printf(
      "C1.info refusals=%u ownerUnestablished=%llu direct=%llu sum=%llu\n",
      static_cast<unsigned>(5u),
      static_cast<unsigned long long>(ownerUnestablished),
      static_cast<unsigned long long>(direct),
      static_cast<unsigned long long>(ownerUnestablished + direct));
}

void RunNonOwnerCases() {
  // C2：owner 已建立（A），但当前线程是 B ⇒ 非所有者，只允许排队 / 拒绝。
  constexpr uint32_t kOwner = 0x1A2Bu;
  constexpr uint32_t kOther = 0x00FFu;

  {
    LocalCounters counters;
    ShadowBuildLifecycle lifecycle;
    const bool allowed = lifecycle.consumeAllowed(false, kOwner, kOther,
                                                 counters.View());
    Check("C2.non-owner-entry-denied", !allowed);
    CheckU64("C2.non-owner-entry-off-thread-count", counters.offThread.load(), 1u);
    CheckU64("C2.non-owner-entry-owner-unestablished-count",
             counters.ownerUnestablished.load(), 0u);
    CheckU64("C2.non-owner-entry-direct-count", counters.directAdvance.load(), 0u);
  }

  {
    LocalCounters counters;
    ShadowBuildLifecycle lifecycle;
    const bool allowed = lifecycle.consumeAllowed(true, kOwner, kOther,
                                                 counters.View());
    Check("C2.non-owner-direct-entry-denied", !allowed);
    CheckU64("C2.non-owner-direct-entry-off-thread-count",
             counters.offThread.load(), 1u);
    CheckU64("C2.non-owner-direct-entry-owner-unestablished-count",
             counters.ownerUnestablished.load(), 0u);
    CheckU64("C2.non-owner-direct-entry-direct-count",
             counters.directAdvance.load(), 1u);
  }

  {
    // 伪造 / 写坏的 owner id 不匹配任何当前线程，仍属非所有者。
    LocalCounters counters;
    ShadowBuildLifecycle lifecycle;
    const bool allowed = lifecycle.consumeAllowed(false, 0xFFFFFFFFu, kOwner,
                                                 counters.View());
    Check("C2.bogus-owner-entry-denied", !allowed);
    CheckU64("C2.bogus-owner-off-thread-count", counters.offThread.load(), 1u);
  }
}

void RunOwnerAllowedCases() {
  // C3：owner 已建立且 current==owner ⇒ 两种入口都放行，且不产生任何拒绝计数。
  constexpr uint32_t kOwner = 0x2C3Du;
  LocalCounters counters;
  ShadowBuildLifecycle lifecycle;
  const bool entryAllowed = lifecycle.consumeAllowed(false, kOwner, kOwner,
                                                    counters.View());
  const bool directAllowed = lifecycle.consumeAllowed(true, kOwner, kOwner,
                                                     counters.View());
  Check("C3.owner-entry-allowed", entryAllowed);
  Check("C3.owner-direct-entry-allowed", directAllowed);
  CheckU64("C3.allowed-off-thread-count-unchanged", counters.offThread.load(), 0u);
  CheckU64("C3.allowed-owner-unestablished-count-unchanged",
           counters.ownerUnestablished.load(), 0u);
  CheckU64("C3.allowed-direct-count-unchanged", counters.directAdvance.load(), 0u);
}

void RunNormalAdvanceCases() {
  // C4：正常推进 beginBuild(work, …) -> publishIfCurrent(work, gen, …) ->
  // completeIfCurrent(work, gen) —— 与运行时每块 / 完成调用**同一条接线**。
  // work 只是测试内的假工作对象，取地址作为工作身份。
  ShadowBuildLifecycle lifecycle;
  int work = 0;
  const uint64_t generation = lifecycle.beginBuild(&work, 0x11u, 0x22u);
  Check("C4.begin-build-generation-nonzero", generation != 0u);
  Check("C4.building-after-begin", lifecycle.building());
  Check("C4.generation-matches-begin-build", lifecycle.generation() == generation);
  Check("C4.is-current-after-begin", lifecycle.isCurrent(&work, generation));
  Check("C4.other-work-not-current", !lifecycle.isCurrent(nullptr, generation));
  Check("C4.has-values-false-before-first-publish", !lifecycle.hasValues());

  ShadowBuildProgressValues values;
  values.frameSerial = 0x11u;
  values.publishRevision = 0x22u;
  values.nextRecordIndex = 5u;
  values.recordCount = 0u;
  values.chunkCount = 1u;
  Check("C4.publish-accepted-for-current-generation",
        lifecycle.publishIfCurrent(&work, generation, values));
  Check("C4.has-values-true-while-building", lifecycle.hasValues());
  Check("C4.building-still-true-after-publish", lifecycle.building());
  Check("C4.values-work-generation-matches",
        lifecycle.values().workGeneration == generation);
  Check("C4.values-frame-serial-matches", lifecycle.values().frameSerial == 0x11u);
  Check("C4.values-publish-revision-matches",
        lifecycle.values().publishRevision == 0x22u);
  Check("C4.values-next-record-index-matches",
        lifecycle.values().nextRecordIndex == 5u);
  // 同一 work、错误代际：即使工作身份对得上，代际门也必须独立生效。
  Check("C4.stale-generation-same-work-publish-rejected",
        !lifecycle.publishIfCurrent(&work, generation + 1u, values));

  Check("C4.complete-accepted-for-current-generation",
        lifecycle.completeIfCurrent(&work, generation));
  Check("C4.building-false-after-complete", !lifecycle.building());
  Check("C4.has-values-false-after-complete", !lifecycle.hasValues());
  Check("C4.is-current-false-after-complete",
        !lifecycle.isCurrent(&work, generation));
  Check("C4.publish-after-complete-rejected",
        !lifecycle.publishIfCurrent(&work, generation, values));
  Check("C4.complete-twice-rejected",
        !lifecycle.completeIfCurrent(&work, generation));
  Check("C4.cancel-after-complete-rejected",
        !lifecycle.cancelIfCurrent(&work, generation));
}

void RunResetCases() {
  // C5：Reset（地图 / 设备切换）后代际 +1 ⇒ 在途旧工作不得重新发布。
  //
  // 2026-09-17 03:58 接线变更：底层代际门 publish / complete / cancel 已转 **private**，
  // 唯一合法入口是携带 (work, generation) 的 *IfCurrent 版本。因此本用例不再直接调用代际门，
  // 而是走运行时同一条接线（publishIfCurrent / completeIfCurrent / cancelIfCurrent）。
  // 底层"只认代际"的状态机由 war3_shadow_build_progress_test.cpp 直接实例化
  // ShadowBuildProgressState 覆盖（BeginBuild/Publish/Complete/Cancel/Reset 全覆盖）。
  ShadowBuildLifecycle lifecycle;
  int workOld = 0;
  int workNew = 0;
  ShadowBuildProgressValues oldValues;
  oldValues.frameSerial = 0x31u;
  oldValues.publishRevision = 0x41u;
  oldValues.nextRecordIndex = 9u;

  const uint64_t oldGeneration = lifecycle.beginBuild(&workOld, 0x31u, 0x41u);
  Check("C5.publish-accepted-before-reset",
        lifecycle.publishIfCurrent(&workOld, oldGeneration, oldValues));
  Check("C5.is-current-before-reset", lifecycle.isCurrent(&workOld, oldGeneration));
  lifecycle.reset();
  Check("C5.building-false-after-reset", !lifecycle.building());
  Check("C5.has-values-false-after-reset", !lifecycle.hasValues());
  Check("C5.publish-old-generation-after-reset-rejected",
        !lifecycle.publishIfCurrent(&workOld, oldGeneration, oldValues));
  Check("C5.complete-old-generation-after-reset-rejected",
        !lifecycle.completeIfCurrent(&workOld, oldGeneration));
  Check("C5.cancel-old-generation-after-reset-rejected",
        !lifecycle.cancelIfCurrent(&workOld, oldGeneration));
  // Reset 同时清空当前工作身份（m_currentWork = nullptr）：旧工作对也被拒。
  Check("C5.publish-if-current-old-work-after-reset-rejected",
        !lifecycle.publishIfCurrent(&workOld, oldGeneration, oldValues));
  Check("C5.complete-if-current-old-work-after-reset-rejected",
        !lifecycle.completeIfCurrent(&workOld, oldGeneration));
  Check("C5.is-current-old-work-after-reset-rejected",
        !lifecycle.isCurrent(&workOld, oldGeneration));

  const uint64_t newGeneration = lifecycle.beginBuild(&workNew, 0x32u, 0x42u);
  Check("C5.new-generation-differs-from-old", newGeneration != oldGeneration);
  Check("C5.publish-old-generation-after-new-begin-rejected",
        !lifecycle.publishIfCurrent(&workOld, oldGeneration, oldValues));
  Check("C5.publish-new-generation-accepted",
        lifecycle.publishIfCurrent(&workNew, newGeneration, oldValues));
  Check("C5.values-work-generation-is-new",
        lifecycle.values().workGeneration == newGeneration);
  // 接线路径：新工作 + 新代际被接受；旧工作（即使代际被错误地抬到新代际）被拒。
  Check("C5.publish-if-current-old-work-with-new-generation-rejected",
        !lifecycle.publishIfCurrent(&workOld, newGeneration, oldValues));
  // Reset 只提升工作代际，与线程身份无关（thread id 不参与代际判定）。
  Check("C5.generation-strictly-increased",
        lifecycle.generation() > oldGeneration);
  // 取消当前代际之后，isCurrent 必须为 false（终态后不得再被当作当前工作回写）。
  Check("C5.cancel-if-current-new-work-accepted",
        lifecycle.cancelIfCurrent(&workNew, newGeneration));
  Check("C5.is-current-false-after-cancel",
        !lifecycle.isCurrent(&workNew, newGeneration));
  Check("C5.publish-after-cancel-rejected",
        !lifecycle.publishIfCurrent(&workNew, newGeneration, oldValues));
}

void RunCancelCases() {
  // C6：取消当前代际后，同代际的 (work, generation) 不再是"当前工作"：
  // 发布 / 完成 / 再次取消都不得生效，且 isCurrent 必须为 false（终态后不得回写）。
  ShadowBuildLifecycle lifecycle;
  int work = 0;
  ShadowBuildProgressValues values;
  values.frameSerial = 0x51u;
  values.publishRevision = 0x61u;
  values.nextRecordIndex = 3u;

  const uint64_t generation = lifecycle.beginBuild(&work, 0x51u, 0x61u);
  Check("C6.publish-accepted-before-cancel",
        lifecycle.publishIfCurrent(&work, generation, values));
  Check("C6.is-current-before-cancel", lifecycle.isCurrent(&work, generation));
  Check("C6.cancel-accepted-for-current-generation",
        lifecycle.cancelIfCurrent(&work, generation));
  Check("C6.building-false-after-cancel", !lifecycle.building());
  Check("C6.is-current-false-after-cancel", !lifecycle.isCurrent(&work, generation));
  Check("C6.publish-after-cancel-rejected",
        !lifecycle.publishIfCurrent(&work, generation, values));
  Check("C6.complete-after-cancel-rejected",
        !lifecycle.completeIfCurrent(&work, generation));
  Check("C6.cancel-twice-rejected", !lifecycle.cancelIfCurrent(&work, generation));
  Check("C6.has-values-false-after-cancel", !lifecycle.hasValues());
  // Cancel(=Complete) 只结束该代际，不清空 m_values 原始结构：被取消代际的字段仍留在结构里，
  // 但 hasValues() 为假 ⇒ 读者必须先用 hasValues() 把关，不能直接信 values()。
  Check("C6.cancelled-generation-invisible-behind-has-values",
        !lifecycle.hasValues() &&
            lifecycle.values().workGeneration == generation);
  // 取消 / 完成后摘要不可见，读者回落到既有 lastStats（运行时 snapshot() 的回落路径）。
  ShadowBuildProgressValues fresh;
  fresh.frameSerial = 0x52u;
  fresh.publishRevision = 0x62u;
  Check("C6.publish-with-fresh-values-still-rejected",
        !lifecycle.publishIfCurrent(&work, generation, fresh));
  Check("C6.complete-with-current-pair-after-cancel-rejected",
        !lifecycle.completeIfCurrent(&work, generation));
}

void RunReplacementBuildCases() {
  // C7：替换在途构建（beginBuild -> beginBuild）⇒ 旧的 (work, generation) 立即失效，新对可发布。
  // 接线变更后只走公开入口：publishIfCurrent / completeIfCurrent / cancelIfCurrent。
  ShadowBuildLifecycle lifecycle;
  int workFirst = 0;
  int workSecond = 0;
  ShadowBuildProgressValues values;
  values.frameSerial = 0x71u;
  values.publishRevision = 0x81u;
  values.nextRecordIndex = 7u;

  const uint64_t firstGeneration = lifecycle.beginBuild(&workFirst, 0x71u, 0x81u);
  Check("C7.first-publish-accepted",
        lifecycle.publishIfCurrent(&workFirst, firstGeneration, values));

  const uint64_t secondGeneration = lifecycle.beginBuild(&workSecond, 0x72u, 0x82u);
  Check("C7.replacement-generation-differs", secondGeneration != firstGeneration);
  Check("C7.building-true-after-replacement", lifecycle.building());
  Check("C7.current-work-is-replacement", lifecycle.isCurrent(&workSecond, secondGeneration));
  Check("C7.first-work-not-current-after-replacement",
        !lifecycle.isCurrent(&workFirst, firstGeneration));
  Check("C7.has-values-false-until-replacement-publishes",
        !lifecycle.hasValues());
  Check("C7.publish-first-generation-rejected-after-replacement",
        !lifecycle.publishIfCurrent(&workFirst, firstGeneration, values));
  Check("C7.complete-first-generation-rejected-after-replacement",
        !lifecycle.completeIfCurrent(&workFirst, firstGeneration));
  Check("C7.cancel-first-generation-rejected-after-replacement",
        !lifecycle.cancelIfCurrent(&workFirst, firstGeneration));
  Check("C7.publish-replacement-generation-accepted",
        lifecycle.publishIfCurrent(&workSecond, secondGeneration, values));
  Check("C7.values-work-generation-is-replacement",
        lifecycle.values().workGeneration == secondGeneration);
  Check("C7.is-current-true-before-complete",
        lifecycle.isCurrent(&workSecond, secondGeneration));
  Check("C7.complete-if-current-accepted",
        lifecycle.completeIfCurrent(&workSecond, secondGeneration));
  Check("C7.is-current-false-after-complete",
        !lifecycle.isCurrent(&workSecond, secondGeneration));
  Check("C7.publish-after-complete-rejected",
        !lifecycle.publishIfCurrent(&workSecond, secondGeneration, values));
}

void RunAbWiringResetRegressionCases() {
  // C9：**A/B 实际接线回归（上级指定用例，Reset 路径）**。
  //   A 开始 -> A 发布一次 -> A 暂停（不 complete）
  //   -> reset() + 开始 B -> B 发布 valuesB
  //   -> A 尝试 publishIfCurrent / completeIfCurrent：必须都 false
  //   -> 断言 B 的全部公开状态**逐字段不变** -> B 仍可正常 completeIfCurrent
  // 这正是"旧工作不能回写"所需要的测试：不是只测 Publish(oldGeneration) 返回 false，
  // 而是走运行时的 (work, generation) 成对携带路径。
  constexpr uint64_t kFrameSerialA = 0xB1u;
  constexpr uint64_t kRevisionA = 0xB2u;
  constexpr uint64_t kFrameSerialB = 0xB3u;
  constexpr uint64_t kRevisionB = 0xB4u;
  constexpr uint64_t kChunkCountB = 5u;
  constexpr uint64_t kTotalDurationB = 4242u;
  constexpr uint64_t kDrawCountB = 7u;

  ShadowBuildLifecycle lifecycle;
  int workA = 0;
  int workB = 0;

  // --- A 开始（绑定工作身份并分配代际），A 发布一次。---
  const uint64_t genA = lifecycle.beginBuild(&workA, kFrameSerialA, kRevisionA);
  Check("C9.A-begin-build-generation-nonzero", genA != 0u);
  Check("C9.A-is-current-after-begin", lifecycle.isCurrent(&workA, genA));
  ShadowBuildProgressValues valuesA;
  valuesA.workGeneration = genA;
  valuesA.frameSerial = kFrameSerialA;
  valuesA.publishRevision = kRevisionA;
  valuesA.nextRecordIndex = 4u;
  valuesA.recordCount = 0u;
  valuesA.chunkCount = 4u;
  valuesA.totalBuildDurationUs = 111u;
  valuesA.drawCount = 2u;
  Check("C9.A-publish-accepted", lifecycle.publishIfCurrent(&workA, genA, valuesA));
  Check("C9.A-has-values-after-publish", lifecycle.hasValues());
  Check("C9.A-values-match-publish", SameValues(lifecycle.values(), valuesA));

  // --- A 暂停：不调用 complete；A 仍是"在途旧工作"。---
  Check("C9.A-building-while-suspended", lifecycle.building());
  Check("C9.A-still-current-while-suspended", lifecycle.isCurrent(&workA, genA));

  // --- Reset + 开始 B（B 获得新的工作身份与新代际），B 发布 valuesB。---
  lifecycle.reset();
  Check("C9.building-false-after-reset", !lifecycle.building());
  Check("C9.A-not-current-after-reset", !lifecycle.isCurrent(&workA, genA));
  const uint64_t genB = lifecycle.beginBuild(&workB, kFrameSerialB, kRevisionB);
  Check("C9.B-begin-build-generation-differs-from-A", genB != genA && genB != 0u);
  Check("C9.B-is-current-after-begin", lifecycle.isCurrent(&workB, genB));
  ShadowBuildProgressValues valuesB;
  valuesB.workGeneration = genB;
  valuesB.frameSerial = kFrameSerialB;
  valuesB.publishRevision = kRevisionB;
  valuesB.nextRecordIndex = 12u;
  valuesB.recordCount = 1u;
  valuesB.chunkCount = kChunkCountB;
  valuesB.totalBuildDurationUs = kTotalDurationB;
  valuesB.drawCount = kDrawCountB;
  Check("C9.B-publish-accepted", lifecycle.publishIfCurrent(&workB, genB, valuesB));

  // 断言 B 的公开状态"变动前"快照（后续所有 A 的尝试都不得改变它）。
  const uint64_t genBefore = lifecycle.generation();
  const bool hasValuesBefore = lifecycle.hasValues();
  const bool buildingBefore = lifecycle.building();
  const bool bCurrentBefore = lifecycle.isCurrent(&workB, genB);
  const ShadowBuildProgressValues valuesBefore = lifecycle.values();
  Check("C9.B-is-current-before-A-attempts", bCurrentBefore);

  // --- A 尝试发布 / 完成：必须是 false（旧工作不得回写）。---
  ShadowBuildProgressValues valuesA2;
  valuesA2.workGeneration = genA;
  valuesA2.frameSerial = kFrameSerialA;
  valuesA2.publishRevision = kRevisionA;
  valuesA2.nextRecordIndex = 999u;
  valuesA2.recordCount = 124u;
  valuesA2.chunkCount = 7u;
  valuesA2.totalBuildDurationUs = 999999u;
  valuesA2.drawCount = 999u;
  Check("C9.A-publish-after-B-rejected",
        !lifecycle.publishIfCurrent(&workA, genA, valuesA2));
  Check("C9.A-complete-after-B-rejected",
        !lifecycle.completeIfCurrent(&workA, genA));

  // --- 断言 B 的全部公开状态不变。---
  Check("C9.B-still-current-after-A-attempts", lifecycle.isCurrent(&workB, genB));
  CheckU64("C9.B-generation-unchanged", lifecycle.generation(), genBefore);
  Check("C9.B-generation-equals-B-begin", lifecycle.generation() == genB);
  Check("C9.B-building-unchanged", lifecycle.building() == buildingBefore);
  Check("C9.B-has-values-unchanged", lifecycle.hasValues() == hasValuesBefore);
  Check("C9.B-has-values-true", lifecycle.hasValues());
  // 逐字段等于发布时的 valuesB（含 workGeneration == genB）。
  const ShadowBuildProgressValues valuesAfter = lifecycle.values();
  Check("C9.B-values-unchanged-as-whole", SameValues(valuesAfter, valuesBefore));
  Check("C9.B-values-equal-published-valuesB", SameValues(valuesAfter, valuesB));
  CheckU64("C9.B-values-work-generation", valuesAfter.workGeneration, genB);
  CheckU64("C9.B-values-frame-serial", valuesAfter.frameSerial, kFrameSerialB);
  CheckU64("C9.B-values-publish-revision", valuesAfter.publishRevision, kRevisionB);
  CheckU64("C9.B-values-next-record-index", valuesAfter.nextRecordIndex, 12u);
  CheckU64("C9.B-values-record-count", valuesAfter.recordCount, 1u);
  CheckU64("C9.B-values-chunk-count", valuesAfter.chunkCount, kChunkCountB);
  CheckU64("C9.B-values-total-build-duration-us", valuesAfter.totalBuildDurationUs,
           kTotalDurationB);
  CheckU64("C9.B-values-draw-count", valuesAfter.drawCount, kDrawCountB);
  // A 的新值没有被写进去（否则 nextRecordIndex/drawCount 会是 999）。
  Check("C9.A-values-did-not-overwrite-B",
        valuesAfter.nextRecordIndex != valuesA2.nextRecordIndex &&
            valuesAfter.drawCount != valuesA2.drawCount);

  // --- 同一 work 但旧代际：对象身份正确也不能放行（防止"同一对象换 token"）。---
  Check("C9.same-work-stale-generation-publish-rejected",
        !lifecycle.publishIfCurrent(&workB, genA, valuesA2));
  Check("C9.same-work-stale-generation-complete-rejected",
        !lifecycle.completeIfCurrent(&workB, genA));
  Check("C9.same-work-stale-generation-is-current-rejected",
        !lifecycle.isCurrent(&workB, genA));
  // 反向错配（旧 work + 新代际）同样被拒。
  Check("C9.other-work-current-generation-publish-rejected",
        !lifecycle.publishIfCurrent(&workA, genB, valuesA2));
  Check("C9.other-work-current-generation-complete-rejected",
        !lifecycle.completeIfCurrent(&workA, genB));
  // 零代际不是"当前"。
  Check("C9.zero-generation-is-current-rejected", !lifecycle.isCurrent(&workB, 0u));
  Check("C9.zero-generation-publish-rejected",
        !lifecycle.publishIfCurrent(&workB, 0u, valuesA2));

  // --- 空工作身份不得被当作当前。---
  Check("C9.null-work-is-current-rejected", !lifecycle.isCurrent(nullptr, genB));
  Check("C9.null-work-publish-rejected",
        !lifecycle.publishIfCurrent(nullptr, genB, valuesA2));
  Check("C9.null-work-complete-rejected",
        !lifecycle.completeIfCurrent(nullptr, genB));

  // --- 以上的 A 尝试都没有破坏 B：B 仍可正常完成。---
  Check("C9.B-complete-if-current-accepted",
        lifecycle.completeIfCurrent(&workB, genB));
  Check("C9.B-building-false-after-complete", !lifecycle.building());
  Check("C9.B-has-values-false-after-complete", !lifecycle.hasValues());
  Check("C9.B-complete-if-current-twice-rejected",
        !lifecycle.completeIfCurrent(&workB, genB));
}

void RunAbWiringReplacementRegressionCases() {
  // C10：**A/B 实际接线回归（替换路径）**：不 Reset，直接用 beginBuild(&workB, …) 顶替在途 A。
  //   A 开始 -> A 发布一次 -> A 暂停 -> beginBuild(&workB, …) 开始 B -> B 发布
  //   -> A 尝试 publishIfCurrent / completeIfCurrent：必须都 false
  //   -> B 的全部公开状态不变 -> B 可正常 completeIfCurrent。
  constexpr uint64_t kFrameSerialA = 0xC1u;
  constexpr uint64_t kRevisionA = 0xC2u;
  constexpr uint64_t kFrameSerialB = 0xC3u;
  constexpr uint64_t kRevisionB = 0xC4u;

  ShadowBuildLifecycle lifecycle;
  int workA = 0;
  int workB = 0;

  const uint64_t genA = lifecycle.beginBuild(&workA, kFrameSerialA, kRevisionA);
  ShadowBuildProgressValues valuesA;
  valuesA.workGeneration = genA;
  valuesA.frameSerial = kFrameSerialA;
  valuesA.publishRevision = kRevisionA;
  valuesA.nextRecordIndex = 6u;
  valuesA.recordCount = 0u;
  valuesA.chunkCount = 6u;
  valuesA.totalBuildDurationUs = 222u;
  valuesA.drawCount = 3u;
  Check("C10.A-publish-accepted", lifecycle.publishIfCurrent(&workA, genA, valuesA));
  Check("C10.A-still-current-while-suspended", lifecycle.isCurrent(&workA, genA));

  // 替换：在途 A 未完成即开始 B（无 Reset）。
  const uint64_t genB = lifecycle.beginBuild(&workB, kFrameSerialB, kRevisionB);
  Check("C10.B-begin-build-generation-differs-from-A", genB != genA && genB != 0u);
  Check("C10.has-values-false-until-B-publishes", !lifecycle.hasValues());
  Check("C10.A-not-current-after-replacement", !lifecycle.isCurrent(&workA, genA));
  Check("C10.B-is-current-after-begin", lifecycle.isCurrent(&workB, genB));

  ShadowBuildProgressValues valuesB;
  valuesB.workGeneration = genB;
  valuesB.frameSerial = kFrameSerialB;
  valuesB.publishRevision = kRevisionB;
  valuesB.nextRecordIndex = 21u;
  valuesB.recordCount = 2u;
  valuesB.chunkCount = 5u;
  valuesB.totalBuildDurationUs = 555u;
  valuesB.drawCount = 9u;
  Check("C10.B-publish-accepted", lifecycle.publishIfCurrent(&workB, genB, valuesB));

  const uint64_t genBefore = lifecycle.generation();
  const ShadowBuildProgressValues valuesBefore = lifecycle.values();

  // A 的每块 / 完成尝试（用 A 自己的固定代际）。
  ShadowBuildProgressValues valuesA2;
  valuesA2.workGeneration = genA;
  valuesA2.frameSerial = kFrameSerialA;
  valuesA2.publishRevision = kRevisionA;
  valuesA2.nextRecordIndex = 777u;
  valuesA2.recordCount = 97u;
  valuesA2.chunkCount = 1u;
  valuesA2.totalBuildDurationUs = 777777u;
  valuesA2.drawCount = 777u;
  Check("C10.A-publish-after-B-rejected",
        !lifecycle.publishIfCurrent(&workA, genA, valuesA2));
  Check("C10.A-complete-after-B-rejected",
        !lifecycle.completeIfCurrent(&workA, genA));
  CheckU64("C10.B-generation-unchanged", lifecycle.generation(), genBefore);
  Check("C10.B-is-current-unchanged", lifecycle.isCurrent(&workB, genB));
  Check("C10.B-has-values-unchanged", lifecycle.hasValues());
  Check("C10.B-building-unchanged", lifecycle.building());
  Check("C10.B-values-unchanged-as-whole",
        SameValues(lifecycle.values(), valuesBefore));
  Check("C10.B-values-equal-published-valuesB",
        SameValues(lifecycle.values(), valuesB));
  CheckU64("C10.B-values-work-generation", lifecycle.values().workGeneration, genB);
  CheckU64("C10.B-values-frame-serial", lifecycle.values().frameSerial, kFrameSerialB);
  CheckU64("C10.B-values-publish-revision", lifecycle.values().publishRevision,
           kRevisionB);
  CheckU64("C10.B-values-next-record-index", lifecycle.values().nextRecordIndex, 21u);
  CheckU64("C10.B-values-record-count", lifecycle.values().recordCount, 2u);
  CheckU64("C10.B-values-chunk-count", lifecycle.values().chunkCount, 5u);
  CheckU64("C10.B-values-total-build-duration-us",
           lifecycle.values().totalBuildDurationUs, 555u);
  CheckU64("C10.B-values-draw-count", lifecycle.values().drawCount, 9u);
  Check("C10.same-work-stale-generation-publish-rejected",
        !lifecycle.publishIfCurrent(&workB, genA, valuesA2));
  Check("C10.B-complete-if-current-accepted",
        lifecycle.completeIfCurrent(&workB, genB));
  Check("C10.B-building-false-after-complete", !lifecycle.building());
}

void RunNullWorkIdentityCases() {
  // C11：空工作身份（beginBuild(nullptr, …)）不得被当作当前。
  // 代码级不变量：isCurrent 要求 work != nullptr 且 work == m_currentWork，
  // 因此 nullptr 永远不是"当前工作"，也就永远无法通过 publishIfCurrent / completeIfCurrent 回写。
  ShadowBuildLifecycle lifecycle;
  int work = 0;
  ShadowBuildProgressValues values;
  values.frameSerial = 0xE1u;
  values.publishRevision = 0xE2u;
  values.nextRecordIndex = 1u;

  const uint64_t generation = lifecycle.beginBuild(nullptr, 0xE1u, 0xE2u);
  Check("C11.generation-assigned-even-for-null-work", generation != 0u);
  Check("C11.null-work-is-current-rejected", !lifecycle.isCurrent(nullptr, generation));
  Check("C11.real-work-is-current-rejected-while-null-bound",
        !lifecycle.isCurrent(&work, generation));
  Check("C11.null-work-publish-rejected",
        !lifecycle.publishIfCurrent(nullptr, generation, values));
  Check("C11.null-work-complete-rejected",
        !lifecycle.completeIfCurrent(nullptr, generation));
  Check("C11.real-work-publish-rejected-while-null-bound",
        !lifecycle.publishIfCurrent(&work, generation, values));
  Check("C11.has-values-false", !lifecycle.hasValues());
  // 2026-09-17 03:58：底层代际门（只认代际、不认工作身份）已转 **private** ⇒ 运行时无法再
  // 直接调用它，组件对外只剩携带 (work, generation) 的 *IfCurrent 入口。
  // 因此"空工作身份不得回写"由**所有**公开入口共同保证；代际门本身（Publish/Complete/Cancel
  // 只认当前代际 + building）由 war3_shadow_build_progress_test.cpp 直接实例化
  // ShadowBuildProgressState 覆盖，本用例不再直接调用 private 成员。
  Check("C11.null-work-never-accepted-on-any-public-path",
        !lifecycle.publishIfCurrent(nullptr, generation, values) &&
            !lifecycle.publishIfCurrent(&work, generation, values) &&
            !lifecycle.completeIfCurrent(nullptr, generation) &&
            !lifecycle.completeIfCurrent(&work, generation) &&
            !lifecycle.cancelIfCurrent(nullptr, generation) &&
            !lifecycle.cancelIfCurrent(&work, generation));
  Check("C11.building-still-true-with-null-work", lifecycle.building());
}

void RunConcurrentSnapshotReadCases() {
  // C8：并发摘要读取。
  //
  // ShadowBuildLifecycle 自身**不加锁**（与运行时一致：并发保护由调用方的 mutex 提供）。
  // 因此本用例必须**模拟运行时的并发使用模式**：
  //   - 写者（构建推进线程）：std::unique_lock<std::shared_mutex> 内
  //     publishIfCurrent(&work, generation, values) —— 即运行时的**接线路径**
  //     （工作身份 + 固定代际成对携带），不是底层代际门 publish；
  //   - 读者（snapshot()/buildStateSnapshot()）：std::shared_lock<std::shared_mutex> 内
  //     按值拷贝 hasValues()/values()。
  // 这正是 ShadowValidationRuntime 中 m_mutex 的使用方式（见 war3_shadow_renderer_core.cpp
  // 的 publishBuildProgressLocked 调用点与 snapshot()/buildStateSnapshot()）。
  //
  // 计时说明（2026-09-17 上级 03:58 裁定）：**接受固定次数**，不要求凑满一秒；
  // <chrono> 已获准用于计时与超时。本用例用固定次数上界（写者 20 万次发布、读者 20 万次读取）
  // + 共同开始握手 + 交错计数 + 30 秒看门狗，并且**不做**"覆盖面不弱于 N 秒空转"一类比较。
  constexpr uint64_t kFrameSerial = 0x91u;
  constexpr uint64_t kPublishRevision = 0xA1u;
  constexpr uint64_t kPublishIterations = 200000u;
  constexpr uint64_t kReadIterations = 200000u;

  ShadowBuildLifecycle lifecycle;
  int work = 0;  // 假工作对象：工作身份在整个用例内固定不变（替换场景由 C7/C9/C10 覆盖）。
  std::shared_mutex mutex;
  std::atomic<uint64_t> publishAttempts{0u};
  std::atomic<uint64_t> publishAccepts{0u};
  std::atomic<uint64_t> publishRejects{0u};
  std::atomic<uint64_t> snapshotsRead{0u};
  std::atomic<uint64_t> snapshotFailures{0u};
  std::atomic<uint64_t> lastObservedRecordIndex{0u};
  // 上级 03:58 要求：C8 必须有**共同开始握手**、**证明读写实际交错**、以及**运行器超时保护**。
  std::atomic<uint32_t> readyThreads{0u};
  std::atomic<bool> startGate{false};
  std::atomic<bool> writerFinished{false};
  std::atomic<uint64_t> intermediateObservations{0u}; // 写者结束前读到的中间发布
  std::atomic<bool> testFinished{false};

  uint64_t generation = 0u;
  {
    // 初始发布：读者一开始就能看到"building + 有值"，避免读者空转。
    std::unique_lock<std::shared_mutex> lock(mutex);
    generation = lifecycle.beginBuild(&work, kFrameSerial, kPublishRevision);
    ShadowBuildProgressValues initial;
    initial.workGeneration = generation;
    initial.frameSerial = kFrameSerial;
    initial.publishRevision = kPublishRevision;
    initial.nextRecordIndex = 0u;
    initial.recordCount = 0u;
    initial.chunkCount = 0u;
    if (lifecycle.publishIfCurrent(&work, generation, initial))
      publishAccepts.fetch_add(1u, std::memory_order_relaxed);
  }

  std::thread writer([&]() {
    readyThreads.fetch_add(1u, std::memory_order_relaxed);
    while (!startGate.load(std::memory_order_acquire))
      std::this_thread::yield(); // 共同开始：两个线程都就绪后才放行，保证读写重叠。
    for (uint64_t i = 1u; i <= kPublishIterations; ++i) {
      // 同一次发布携带的全部字段自洽：nextRecordIndex == recordCount*8 + chunkCount。
      ShadowBuildProgressValues values;
      values.workGeneration = generation;
      values.frameSerial = kFrameSerial;
      values.publishRevision = kPublishRevision;
      values.nextRecordIndex = i;
      values.recordCount = i / 8u;
      values.chunkCount = i % 8u;
      std::unique_lock<std::shared_mutex> lock(mutex);
      publishAttempts.fetch_add(1u, std::memory_order_relaxed);
      // 写侧路径：publishIfCurrent（工作身份 + 固定代际）——与运行时 publishBuildProgressLocked 一致。
      if (lifecycle.publishIfCurrent(&work, generation, values))
        publishAccepts.fetch_add(1u, std::memory_order_relaxed);
      else
        publishRejects.fetch_add(1u, std::memory_order_relaxed);
    }
    writerFinished.store(true, std::memory_order_release);
  });

  std::thread reader([&]() {
    readyThreads.fetch_add(1u, std::memory_order_relaxed);
    while (!startGate.load(std::memory_order_acquire))
      std::this_thread::yield();
    uint64_t previousRecordIndex = 0u;
    for (uint64_t r = 0u; r < kReadIterations; ++r) {
      std::shared_lock<std::shared_mutex> lock(mutex);
      if (!lifecycle.hasValues()) {
        snapshotFailures.fetch_add(1u, std::memory_order_relaxed);
        continue;
      }
      // 锁内按值拷贝：锁外不得再触碰 values() 返回的引用。
      const ShadowBuildProgressValues snapshot = lifecycle.values();
      snapshotsRead.fetch_add(1u, std::memory_order_relaxed);
      // 1) 代际稳定。
      if (snapshot.workGeneration != generation)
        snapshotFailures.fetch_add(1u, std::memory_order_relaxed);
      // 2) frameSerial / publishRevision 与该代际一致。
      if (snapshot.frameSerial != kFrameSerial ||
          snapshot.publishRevision != kPublishRevision)
        snapshotFailures.fetch_add(1u, std::memory_order_relaxed);
      // 3) nextRecordIndex 单调不减。
      if (snapshot.nextRecordIndex < previousRecordIndex)
        snapshotFailures.fetch_add(1u, std::memory_order_relaxed);
      // 4) 快照自洽：所有字段来自**同一次** publish，不得出现撕裂组合。
      if (snapshot.recordCount * 8u + snapshot.chunkCount !=
          snapshot.nextRecordIndex)
        snapshotFailures.fetch_add(1u, std::memory_order_relaxed);
      previousRecordIndex = snapshot.nextRecordIndex;
      lastObservedRecordIndex.store(snapshot.nextRecordIndex,
                                    std::memory_order_relaxed);
      // 交错证据：写者尚未结束，且读到的是一条**中间**发布（非 0、非终值）。
      if (!writerFinished.load(std::memory_order_acquire) &&
          snapshot.nextRecordIndex != 0u &&
          snapshot.nextRecordIndex != kPublishIterations)
        intermediateObservations.fetch_add(1u, std::memory_order_relaxed);
    }
  });

  // 运行器超时保护：30 秒内未完成即视为测试失败（不是"跑得久的成功"）。
  std::thread watchdog([&]() {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(30);
    while (!testFinished.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        std::printf("C8.TIMEOUT watchdog fired after 30s\n");
        std::fflush(stdout);
        std::_Exit(2);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  // 共同开始：等待两个工作线程都就绪再放行。
  while (readyThreads.load(std::memory_order_acquire) < 2u)
    std::this_thread::yield();
  startGate.store(true, std::memory_order_release);
  writer.join();
  reader.join();
  testFinished.store(true, std::memory_order_release);
  watchdog.join();
  // 上级要求：必须证明读者观察到写者完成前的中间发布（不是两线程先后跑完也通过）。
  Check("C8.reader-observed-intermediate-publish",
        intermediateObservations.load() > 0u);
  Check("C8.writer-finished-after-reads",
        writerFinished.load(std::memory_order_acquire));

  CheckU64("C8.publish-attempts", publishAttempts.load(), kPublishIterations);
  CheckU64("C8.publish-accepts", publishAccepts.load(),
           kPublishIterations + 1u);
  CheckU64("C8.publish-rejects", publishRejects.load(), 0u);
  CheckU64("C8.snapshot-reads", snapshotsRead.load(), kReadIterations);
  CheckU64("C8.snapshot-self-consistency-failures", snapshotFailures.load(), 0u);

  // 结束后再读一次（确定性收尾）：终值必须属于当前代际且自洽。
  {
    std::shared_lock<std::shared_mutex> lock(mutex);
    Check("C8.final-has-values", lifecycle.hasValues());
    Check("C8.final-is-current", lifecycle.isCurrent(&work, generation));
    const ShadowBuildProgressValues finalValues = lifecycle.values();
    Check("C8.final-work-generation-matches",
          finalValues.workGeneration == generation);
    Check("C8.final-frame-serial-matches", finalValues.frameSerial == kFrameSerial);
    Check("C8.final-publish-revision-matches",
          finalValues.publishRevision == kPublishRevision);
    Check("C8.final-next-record-index-is-last-publish",
          finalValues.nextRecordIndex == kPublishIterations);
    Check("C8.final-snapshot-self-consistent",
          finalValues.recordCount * 8u + finalValues.chunkCount ==
              finalValues.nextRecordIndex);
    Check("C8.last-observed-index-monotonic-with-final",
          lastObservedRecordIndex.load() <= finalValues.nextRecordIndex);
  }
  std::printf("C8.info publishes=%llu reads=%llu lastObserved=%llu "
              "intermediateObservations=%llu\n",
              static_cast<unsigned long long>(publishAccepts.load()),
              static_cast<unsigned long long>(snapshotsRead.load()),
              static_cast<unsigned long long>(lastObservedRecordIndex.load()),
              static_cast<unsigned long long>(intermediateObservations.load()));
}

}  // namespace

int main() {
  RunUnknownOwnerCases();
  RunNonOwnerCases();
  RunOwnerAllowedCases();
  RunNormalAdvanceCases();
  RunResetCases();
  RunCancelCases();
  RunReplacementBuildCases();
  RunAbWiringResetRegressionCases();
  RunAbWiringReplacementRegressionCases();
  RunNullWorkIdentityCases();
  RunConcurrentSnapshotReadCases();

  if (g_failures != 0u) {
    std::printf("war3_shadow_build_lifecycle_test: FAIL %u/%u case(s)\n",
                static_cast<unsigned>(g_failures),
                static_cast<unsigned>(g_cases));
    return 1;
  }
  std::printf("SUMMARY: war3_shadow_build_lifecycle_test %u/%u case(s) passed\n",
              static_cast<unsigned>(g_cases), static_cast<unsigned>(g_cases));
  return 0;
}
