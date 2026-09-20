#pragma once

#include <atomic>
#include <cstdint>

#include "war3_shadow_build_progress.h"
#include "war3_shadow_build_thread_gate.h"

// 2026-09-17 上级裁定（codex 01a02e0b）：
//   「9 个测试实际只调用线程判定函数，没有调用真实推进/Reset 边界」⇒ 需补**生产共用**的
//   推进/发布生命周期测试，覆盖未知所有者、两种入口、正常推进、取消/Reset、
//   旧工作不得重新发布，以及并发摘要读取。
// 因此把「所有者门 + 按值进度发布 + 工作代际守卫」收进本组件：运行时的两处推进入口与
// 宿主机测试使用**同一份实现**，测试覆盖的是真实边界逻辑而不是复制品。
//
// 约束：本组件自身不加锁（并发保护由调用方的 mutex 提供，与运行时一致）；不引入堆分配。

namespace dxvk::war3::shadow {

// 计数下沉：生产传进程级全局原子；测试传局部原子以断言拒绝分类。
// 上级明确：入口分类与拒绝原因**不是天然互斥的统计维度**，不得未经证明相加成「总拒绝」。
struct ShadowBuildAdvanceCounters {
  std::atomic<uint64_t>* offThreadRefused = nullptr;
  std::atomic<uint64_t>* ownerUnestablishedRefused = nullptr;
  std::atomic<uint64_t>* directAdvanceRefused = nullptr;
};

class ShadowBuildLifecycle {
 public:
  // 唯一一份消费权限检查：所有者未建立 / 非所有者一律拒绝（只保留安全请求）。
  // directEntry=true 表示调用来自真正的推进入口（ensureFrameBuiltForContract），
  // 该情形另计一份拒绝数，用于证明「直接调用底层方法也无法绕过」。
  bool consumeAllowed(bool directEntry, uint32_t ownerThreadId,
                      uint32_t currentThreadId,
                      const ShadowBuildAdvanceCounters& counters) {
    const ShadowBuildAdvanceDecision decision =
        DecideShadowBuildAdvance(ownerThreadId, currentThreadId);
    if (decision == ShadowBuildAdvanceDecision::Allow)
      return true;
    if (decision == ShadowBuildAdvanceDecision::OwnerUnestablished) {
      if (counters.ownerUnestablishedRefused != nullptr)
        counters.ownerUnestablishedRefused->fetch_add(
            1u, std::memory_order_relaxed);
    } else if (counters.offThreadRefused != nullptr) {
      counters.offThreadRefused->fetch_add(1u, std::memory_order_relaxed);
    }
    if (directEntry && counters.directAdvanceRefused != nullptr)
      counters.directAdvanceRefused->fetch_add(1u, std::memory_order_relaxed);
    return false;
  }

  // 注意：底层代际操作（Publish/Complete/Cancel）**不对外公开** ——
  // 唯一合法的对外路径是下面的 *IfCurrent 版本（必须携带工作身份 + 固定代际）。
  //
  // 开始 / 替换构建：绑定**工作对象身份**并分配新的工作代际（旧工作从此无法再发布）。
  // 上级裁定：工作对象与其代际必须成对携带；调用方必须在锁内成对取得后在后续成对使用。
  uint64_t beginBuild(const void* work, uint64_t frameSerial,
                      uint64_t publishRevision) {
    m_currentWork = work;
    return m_progress.BeginBuild(frameSerial, publishRevision);
  }
  // 当前工作身份是否仍是给定的 (work, generation) 对，**且该构建仍在进行中**。
  // 上级 03:58：终态（完成 / 取消）之后不得再被当作"当前工作"回写。
  bool isCurrent(const void* work, uint64_t generation) const {
    return work != nullptr && work == m_currentWork &&
           generation != 0u && generation == m_progress.generation() &&
           m_progress.building();
  }
  // 仅当 (work, generation) 仍是当前工作时才发布（旧工作不得回写）。
  bool publishIfCurrent(const void* work, uint64_t generation,
                        const ShadowBuildProgressValues& values) {
    if (!isCurrent(work, generation))
      return false;
    return m_progress.Publish(generation, values);
  }
  // 仅当 (work, generation) 仍是当前工作时才结束该代际。
  bool completeIfCurrent(const void* work, uint64_t generation) {
    if (!isCurrent(work, generation))
      return false;
    return m_progress.Complete(generation);
  }
  // 仅当 (work, generation) 仍是当前工作时才取消该代际（陈旧预览清理路径）。
  bool cancelIfCurrent(const void* work, uint64_t generation) {
    if (!isCurrent(work, generation))
      return false;
    return m_progress.Cancel(generation);
  }
  // 注意：底层代际门 publish / complete / cancel 只声明在下方 private 段
  //（唯一合法入口是携带 (work, generation) 的 *IfCurrent 版本）。
  // Reset（地图/设备切换）：代际 +1 ⇒ 在途旧块无法回写（与线程身份无关，两者是不同概念）。
  void reset() {
    m_currentWork = nullptr;
    m_progress.Reset();
  }

  bool building() const { return m_progress.building(); }
  bool hasValues() const { return m_progress.hasValues(); }
  uint64_t generation() const { return m_progress.generation(); }
  const ShadowBuildProgressValues& values() const { return m_progress.values(); }

 private:
  // 底层代际操作：只允许经 isCurrent 校验后的 *IfCurrent 入口调用（上级 03:58 要求）。
  bool publish(uint64_t generation, const ShadowBuildProgressValues& values) {
    return m_progress.Publish(generation, values);
  }
  bool complete(uint64_t generation) { return m_progress.Complete(generation); }
  bool cancel(uint64_t generation) { return m_progress.Cancel(generation); }

  const void* m_currentWork = nullptr;
  ShadowBuildProgressState m_progress;
};

} // namespace dxvk::war3::shadow
