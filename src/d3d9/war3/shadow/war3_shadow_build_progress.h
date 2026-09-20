#pragma once

#include <cstdint>

// 2026-09-17 上级裁定（codex 01a02e0b）：构建进度摘要必须
//   1) **按值发布、无可变别名**（不得只是指向同一工作对象的 shared_ptr<const>）；
//   2) **不要求每块新增堆分配**；
//   3) 帧号 / 发布 revision / 工作代际 / 统计**来自同一次发布**；
//   4) 开始、完成、取消、Reset、替换构建都要更新摘要；旧块不得在 Reset 后重新发布。
//
// 本文件是**纯状态机**（仅 POD + 代际守卫，无 d3d9/游戏依赖），生产路径与宿主机测试共用。
// 构建工作对象由所有者线程独占；摘要只在短临界区内按值写入，读者在共享锁下按值拷贝。

namespace dxvk::war3::shadow {

// 一次发布所携带的**同一份**值（不含 ShadowValidationFrameStats：那部分由运行时在同一临界区
// 一并发布，仍属同一次发布事件）。
struct ShadowBuildProgressValues {
  uint64_t workGeneration = 0u;
  uint64_t frameSerial = 0u;
  uint64_t publishRevision = 0u;
  uint64_t nextRecordIndex = 0u;
  uint64_t recordCount = 0u;
  uint64_t chunkCount = 0u;
  uint64_t totalBuildDurationUs = 0u;
  uint64_t drawCount = 0u;
};

class ShadowBuildProgressState {
 public:
  // 新构建（含替换在途构建）：代际 +1，清空旧值。返回值即本次构建的代际。
  uint64_t BeginBuild(uint64_t frameSerial, uint64_t publishRevision) {
    BumpGeneration();
    m_building = true;
    m_hasValues = false;
    m_values = ShadowBuildProgressValues{};
    m_values.workGeneration = m_generation;
    m_values.frameSerial = frameSerial;
    m_values.publishRevision = publishRevision;
    return m_generation;
  }
  // 仅接受「当前代际 + 正在构建」的发布；旧代际/已结束/未开始一律拒绝（返回 false）。
  bool Publish(uint64_t generation, const ShadowBuildProgressValues& values) {
    if (!m_building || generation == 0u || generation != m_generation)
      return false;
    m_values = values;
    m_values.workGeneration = generation;
    m_hasValues = true;
    return true;
  }
  // 完成 / 取消：只接受当前代际；结束后摘要不再对外可见（读者回落到既有 lastStats）。
  bool Complete(uint64_t generation) {
    if (!m_building || generation == 0u || generation != m_generation)
      return false;
    m_building = false;
    return true;
  }
  bool Cancel(uint64_t generation) { return Complete(generation); }
  // Reset（地图/设备切换）：代际 +1，任何在途旧块从此无法再发布。
  void Reset() {
    BumpGeneration();
    m_building = false;
    m_hasValues = false;
    m_values = ShadowBuildProgressValues{};
  }

  bool building() const { return m_building; }
  bool hasValues() const { return m_building && m_hasValues; }
  uint64_t generation() const { return m_generation; }
  const ShadowBuildProgressValues& values() const { return m_values; }

 private:
  void BumpGeneration() {
    ++m_generation;
    if (m_generation == 0u)
      ++m_generation; // 0 表示「尚未建立」，不得回绕到 0
  }

  uint64_t m_generation = 0u;
  bool m_building = false;
  bool m_hasValues = false;
  ShadowBuildProgressValues m_values = {};
};

} // namespace dxvk::war3::shadow
