// 2026-09-17 上级裁定：构建进度摘要状态机的宿主机生命周期测试。
//
// 被测对象是产品源里的同一份实现
// （war3_shadow_build_progress.h: ShadowBuildProgressState）。生产路径的
// ShadowValidationRuntime 直接持有该状态机的**按值**成员 m_buildProgress，
// 因此本测试断言的就是生产边界（不是复制出来的模型）：
//   - BeginBuild 分配工作代际（+1，且不回绕到 0）；
//   - Publish 只接受「当前代际 + 正在构建」，旧代际 / 已结束 / 未开始一律拒绝且不写值；
//   - 开始 / 每块 / 完成 / 取消 / Reset / 替换构建都经由同一状态机更新摘要；
//   - 发布是**按值**的：调用方对象与状态机记录互不别名，每块不新增堆分配。
//
// 本测试只断言状态机行为，不做字符串静态检查；每例打印一行，全部通过打印 SUMMARY，
// 任一失败非零退出。并发压力通过**不能**替代这里的同步关系；本文件的断言范围仅限
// 状态机自身的同步语义，不对上层并发访问作任何结论。

#include "../../shadow/war3_shadow_build_progress.h"

#include <cstdint>
#include <cstdio>

namespace {

using dxvk::war3::shadow::ShadowBuildProgressState;
using dxvk::war3::shadow::ShadowBuildProgressValues;

uint32_t g_failures = 0u;
uint32_t g_cases = 0u;

void Check(const char* label, bool ok) {
  ++g_cases;
  if (!ok)
    ++g_failures;
  std::printf("%s %s\n", label, ok ? "PASS" : "FAIL");
}

void CheckEq(const char* label, uint64_t actual, uint64_t expected) {
  ++g_cases;
  const bool ok = actual == expected;
  if (!ok)
    ++g_failures;
  std::printf("%s actual=%llu expected=%llu %s\n", label,
              static_cast<unsigned long long>(actual),
              static_cast<unsigned long long>(expected), ok ? "PASS" : "FAIL");
}

// 只接受上面两个标准头，故不使用 <cstring> / <string>：逐字段构造即可。
ShadowBuildProgressValues MakeValues(uint64_t frameSerial,
                                     uint64_t publishRevision,
                                     uint64_t nextRecordIndex,
                                     uint64_t recordCount, uint64_t chunkCount,
                                     uint64_t drawCount,
                                     uint64_t totalBuildDurationUs) {
  ShadowBuildProgressValues values;
  values.frameSerial = frameSerial;
  values.publishRevision = publishRevision;
  values.nextRecordIndex = nextRecordIndex;
  values.recordCount = recordCount;
  values.chunkCount = chunkCount;
  values.drawCount = drawCount;
  values.totalBuildDurationUs = totalBuildDurationUs;
  return values;
}

// 断言「状态机当前值 == 期望值（除 workGeneration 外的全部字段）」。
// 用于证明一次被拒绝的 Publish 没有改写已发布摘要。
void CheckValuesMatch(const char* label, const ShadowBuildProgressValues& actual,
                      uint64_t frameSerial, uint64_t publishRevision,
                      uint64_t nextRecordIndex, uint64_t recordCount,
                      uint64_t chunkCount, uint64_t drawCount,
                      uint64_t totalBuildDurationUs) {
  const bool ok = actual.frameSerial == frameSerial &&
                  actual.publishRevision == publishRevision &&
                  actual.nextRecordIndex == nextRecordIndex &&
                  actual.recordCount == recordCount &&
                  actual.chunkCount == chunkCount &&
                  actual.drawCount == drawCount &&
                  actual.totalBuildDurationUs == totalBuildDurationUs;
  ++g_cases;
  if (!ok)
    ++g_failures;
  std::printf(
      "%s frame=%llu/%llu rev=%llu/%llu idx=%llu/%llu records=%llu/%llu "
      "chunks=%llu/%llu draws=%llu/%llu dur=%llu/%llu %s\n",
      label, static_cast<unsigned long long>(actual.frameSerial),
      static_cast<unsigned long long>(frameSerial),
      static_cast<unsigned long long>(actual.publishRevision),
      static_cast<unsigned long long>(publishRevision),
      static_cast<unsigned long long>(actual.nextRecordIndex),
      static_cast<unsigned long long>(nextRecordIndex),
      static_cast<unsigned long long>(actual.recordCount),
      static_cast<unsigned long long>(recordCount),
      static_cast<unsigned long long>(actual.chunkCount),
      static_cast<unsigned long long>(chunkCount),
      static_cast<unsigned long long>(actual.drawCount),
      static_cast<unsigned long long>(drawCount),
      static_cast<unsigned long long>(actual.totalBuildDurationUs),
      static_cast<unsigned long long>(totalBuildDurationUs),
      ok ? "PASS" : "FAIL");
}

// ---- 1. Begin -> Publish -> Complete 正常序列 ----
void Case1NormalSequence() {
  ShadowBuildProgressState state;
  CheckEq("C1 fresh generation is zero", state.generation(), 0u);
  Check("C1 fresh not building", !state.building());
  Check("C1 fresh has no published values", !state.hasValues());

  const uint64_t generation = state.BeginBuild(11u, 22u);
  Check("C1 begin allocates nonzero generation", generation != 0u);
  CheckEq("C1 begin returns the current generation", generation,
          state.generation());
  Check("C1 building after begin", state.building());
  // BeginBuild 只播种帧号/发布 revision/工作代际；第一次 Publish（生产里紧随
  // BeginBuild 同临界区调用）之前摘要还不对外可见。
  Check("C1 hasValues false until the first publish lands", !state.hasValues());
  CheckEq("C1 begin seeds frame serial", state.values().frameSerial, 11u);
  CheckEq("C1 begin seeds publish revision", state.values().publishRevision,
          22u);
  CheckEq("C1 begin seeds work generation", state.values().workGeneration,
          generation);

  const ShadowBuildProgressValues published =
      MakeValues(11u, 22u, 3u, 8u, 1u, 5u, 77u);
  Check("C1 publish accepted", state.Publish(generation, published));
  Check("C1 hasValues true while building", state.hasValues());
  CheckEq("C1 published work generation matches the build generation",
          state.values().workGeneration, generation);
  CheckValuesMatch("C1 published values match the same publish", state.values(),
                   11u, 22u, 3u, 8u, 1u, 5u, 77u);

  Check("C1 complete accepted", state.Complete(generation));
  Check("C1 not building after complete", !state.building());
  Check("C1 hasValues false after complete", !state.hasValues());
  CheckEq("C1 generation unchanged by complete", state.generation(),
          generation);
}

// ---- 2. Publish 代际不匹配被拒，且不写值 ----
void Case2StaleGenerationRejected() {
  ShadowBuildProgressState state;
  const uint64_t firstGeneration = state.BeginBuild(100u, 200u);
  Check("C2 first generation publish accepted",
        state.Publish(firstGeneration,
                      MakeValues(100u, 200u, 3u, 8u, 1u, 5u, 77u)));

  // 替换在途构建：新代际，旧代际 E 从此失效。
  const uint64_t secondGeneration = state.BeginBuild(101u, 201u);
  CheckEq("C2 replacement build bumps generation by one", secondGeneration,
          firstGeneration + 1u);
  Check("C2 current generation publish accepted",
        state.Publish(secondGeneration,
                      MakeValues(101u, 201u, 4u, 8u, 2u, 6u, 88u)));

  const ShadowBuildProgressValues stale =
      MakeValues(100u, 200u, 77u, 77u, 77u, 77u, 77777u);
  Check("C2 stale generation E publish rejected under E+1",
        !state.Publish(firstGeneration, stale));
  CheckValuesMatch("C2 rejected publish left values untouched", state.values(),
                   101u, 201u, 4u, 8u, 2u, 6u, 88u);
  CheckEq("C2 published work generation still current",
          state.values().workGeneration, secondGeneration);
  Check("C2 still building after rejected publish", state.building());
  Check("C2 current generation still publishes",
        state.Publish(secondGeneration,
                      MakeValues(101u, 201u, 5u, 8u, 3u, 6u, 99u)));
}

// ---- 3. Complete 之后 Publish 被拒（非 building）----
void Case3PublishAfterCompleteRejected() {
  ShadowBuildProgressState state;
  const uint64_t generation = state.BeginBuild(5u, 6u);
  Check("C3 publish accepted before complete",
        state.Publish(generation, MakeValues(5u, 6u, 2u, 4u, 1u, 3u, 40u)));
  Check("C3 complete accepted", state.Complete(generation));
  Check("C3 publish after complete rejected",
        !state.Publish(generation, MakeValues(5u, 6u, 4u, 4u, 2u, 9u, 999u)));
  CheckValuesMatch("C3 rejected publish after complete left values untouched",
                   state.values(), 5u, 6u, 2u, 4u, 1u, 3u, 40u);
  Check("C3 complete twice rejected", !state.Complete(generation));
  Check("C3 cancel after complete rejected", !state.Cancel(generation));
  Check("C3 hasValues stays false after complete", !state.hasValues());
}

// ---- 4. 未 Begin 时 Publish / Complete 被拒（generation == 0）----
void Case4WithoutBeginRejected() {
  ShadowBuildProgressState state;
  CheckEq("C4 fresh generation is zero", state.generation(), 0u);
  Check("C4 publish with generation 0 rejected",
        !state.Publish(0u, MakeValues(1u, 2u, 0u, 0u, 0u, 0u, 0u)));
  // 未 Begin 时即便猜到某个非零代际也不得放行（无 building 状态）。
  Check("C4 publish with guessed nonzero generation rejected",
        !state.Publish(1u, MakeValues(1u, 2u, 0u, 0u, 0u, 0u, 0u)));
  Check("C4 complete with generation 0 rejected", !state.Complete(0u));
  Check("C4 cancel with generation 0 rejected", !state.Cancel(0u));
  Check("C4 not building", !state.building());
  Check("C4 hasValues false", !state.hasValues());
  // 拒绝路径不得把状态改成 building。
  CheckEq("C4 generation still zero after rejected calls", state.generation(),
          0u);
}

// ---- 5. 替换构建：Begin(E) -> Begin(E+1)（在途），E 被拒 / E+1 成功 ----
void Case5ReplacementBuild() {
  ShadowBuildProgressState state;
  const uint64_t generationE = state.BeginBuild(71u, 72u);
  Check("C5 first build publish accepted",
        state.Publish(generationE, MakeValues(71u, 72u, 1u, 10u, 1u, 1u, 10u)));
  Check("C5 first build still building", state.building());

  // 在途替换：没有 Complete，直接 Begin 新构建。
  const uint64_t generationE1 = state.BeginBuild(81u, 82u);
  CheckEq("C5 replacement generation is E+1", generationE1, generationE + 1u);
  Check("C5 replacement resets hasValues until its first publish",
        !state.hasValues());
  CheckEq("C5 replacement seeds new frame serial", state.values().frameSerial,
          81u);
  Check("C5 old generation publish rejected after replacement",
        !state.Publish(generationE, MakeValues(71u, 72u, 9u, 10u, 9u, 9u, 90u)));
  Check("C5 new generation publish accepted",
        state.Publish(generationE1, MakeValues(81u, 82u, 2u, 10u, 1u, 2u, 20u)));
  CheckValuesMatch("C5 replacement values are the new build's", state.values(),
                   81u, 82u, 2u, 10u, 1u, 2u, 20u);
  CheckEq("C5 published work generation is E+1",
          state.values().workGeneration, generationE1);
}

// ---- 6. Reset 之后旧块不得再发布 ----
void Case6ResetRetiresInflightBlocks() {
  ShadowBuildProgressState state;
  const uint64_t oldGeneration = state.BeginBuild(31u, 32u);
  Check("C6 old build publish accepted",
        state.Publish(oldGeneration,
                      MakeValues(31u, 32u, 6u, 12u, 3u, 7u, 70u)));
  state.Reset();
  CheckEq("C6 reset bumps generation", state.generation(), oldGeneration + 1u);
  Check("C6 not building after reset", !state.building());
  Check("C6 no published values after reset", !state.hasValues());

  const ShadowBuildProgressValues stale =
      MakeValues(31u, 32u, 99u, 99u, 99u, 99u, 99999u);
  Check("C6 stale publish after reset rejected",
        !state.Publish(oldGeneration, stale));
  Check("C6 rejected stale publish did not make values visible",
        !state.hasValues());
  CheckValuesMatch("C6 reset cleared the old values", state.values(), 0u, 0u,
                   0u, 0u, 0u, 0u, 0u);

  // 新构建开始后，旧块仍然回写不了（旧代际在任何后续代际下都被拒）。
  const uint64_t newGeneration = state.BeginBuild(41u, 42u);
  // Reset 与 Begin 各 +1，所以新构建代际 = 旧构建代际 + 2。
  CheckEq("C6 new build generation is reset + 1 after the old build", newGeneration,
          oldGeneration + 2u);
  Check("C6 stale publish still rejected under the new build",
        !state.Publish(oldGeneration, stale));
  Check("C6 current build publish accepted",
        state.Publish(newGeneration, MakeValues(41u, 42u, 1u, 4u, 1u, 1u, 5u)));
  CheckValuesMatch("C6 new build values untouched by the stale block",
                   state.values(), 41u, 42u, 1u, 4u, 1u, 1u, 5u);
}

// ---- 7. 代际不回绕到 0：8 轮 Begin/Reset 严格递增且始终非零 ----
void Case7GenerationNeverWrapsToZero() {
  ShadowBuildProgressState state;
  uint64_t previous = state.generation();
  CheckEq("C7 initial generation is the sentinel zero", previous, 0u);
  uint32_t nonzeroBegins = 0u;
  uint32_t nonzeroResets = 0u;
  uint32_t monotonic = 0u;
  for (uint32_t round = 0u; round < 8u; ++round) {
    const uint64_t begun = state.BeginBuild(round + 1u, round + 100u);
    if (begun != 0u)
      ++nonzeroBegins;
    if (begun > previous)
      ++monotonic;
    previous = begun;
    state.Reset();
    const uint64_t afterReset = state.generation();
    if (afterReset != 0u)
      ++nonzeroResets;
    if (afterReset > previous)
      ++monotonic;
    previous = afterReset;
    std::printf("C7 round=%u begin=%llu afterReset=%llu %s\n", round,
                static_cast<unsigned long long>(begun),
                static_cast<unsigned long long>(afterReset),
                (begun != 0u && afterReset > begun) ? "PASS" : "FAIL");
    ++g_cases;
    if (!(begun != 0u && afterReset > begun))
      ++g_failures;
  }
  CheckEq("C7 all 8 begin generations nonzero", nonzeroBegins, 8u);
  CheckEq("C7 all 8 reset generations nonzero", nonzeroResets, 8u);
  CheckEq("C7 all 16 generation bumps strictly increasing", monotonic, 16u);
  Check("C7 final generation nonzero", state.generation() != 0u);
}

// ---- 8. 按值语义：发布与读取都不产生可变别名 ----
void Case8PublishAndReadAreByValue() {
  ShadowBuildProgressState state;
  const uint64_t generation = state.BeginBuild(42u, 43u);

  ShadowBuildProgressValues source = MakeValues(42u, 43u, 1u, 2u, 3u, 4u, 5u);
  Check("C8 publish accepted", state.Publish(generation, source));
  // (a) 发布按值拷贝：改写调用方对象不得影响已发布摘要。
  source.frameSerial = 999u;
  source.publishRevision = 999u;
  source.nextRecordIndex = 999u;
  source.recordCount = 999u;
  source.chunkCount = 999u;
  source.drawCount = 999u;
  source.totalBuildDurationUs = 999999u;
  CheckValuesMatch("C8 mutating the publisher object left values untouched",
                   state.values(), 42u, 43u, 1u, 2u, 3u, 4u, 5u);

  // (b) values() 的引用指向状态机自己的 POD，拷贝后改副本不影响原值。
  const ShadowBuildProgressValues& publishedRef = state.values();
  ShadowBuildProgressValues copy = state.values();
  copy.drawCount = 555u;
  copy.frameSerial = 556u;
  CheckEq("C8 mutating the copy changed only the copy", copy.drawCount, 555u);
  CheckEq("C8 copy mutation did not change the reference's drawCount",
          publishedRef.drawCount, 4u);
  CheckEq("C8 copy mutation did not change the reference's frameSerial",
          publishedRef.frameSerial, 42u);
  CheckValuesMatch("C8 state values unchanged after copy mutation",
                   state.values(), 42u, 43u, 1u, 2u, 3u, 4u, 5u);

  // (c) 两次 values() 拷贝互相之间也没有共享可变状态。
  ShadowBuildProgressValues first = state.values();
  ShadowBuildProgressValues second = state.values();
  first.recordCount = 111u;
  second.recordCount = 222u;
  CheckEq("C8 first copy is independent", first.recordCount, 111u);
  CheckEq("C8 second copy is independent", second.recordCount, 222u);
  CheckEq("C8 state record count is still 2", state.values().recordCount, 2u);

  // (d) 发布后再次发布仍然是复制语义（旧发布对象不别名新发布值）。
  ShadowBuildProgressValues next = MakeValues(43u, 44u, 9u, 9u, 9u, 9u, 9u);
  Check("C8 second publish accepted", state.Publish(generation, next));
  next.drawCount = 1000u;
  CheckEq("C8 second publish drawCount untouched", state.values().drawCount,
          9u);
  CheckEq("C8 second publish work generation still current",
          state.values().workGeneration, generation);
}

}  // namespace

int main() {
  Case1NormalSequence();
  Case2StaleGenerationRejected();
  Case3PublishAfterCompleteRejected();
  Case4WithoutBeginRejected();
  Case5ReplacementBuild();
  Case6ResetRetiresInflightBlocks();
  Case7GenerationNeverWrapsToZero();
  Case8PublishAndReadAreByValue();

  if (g_failures != 0u) {
    std::printf("war3_shadow_build_progress_test: FAIL %u/%u case(s)\n",
                static_cast<unsigned>(g_failures),
                static_cast<unsigned>(g_cases));
    return 1;
  }
  std::printf(
      "SUMMARY: war3_shadow_build_progress_test %u/%u case(s) passed\n",
      static_cast<unsigned>(g_cases), static_cast<unsigned>(g_cases));
  return 0;
}
