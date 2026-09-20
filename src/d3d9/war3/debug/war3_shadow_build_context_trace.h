#pragma once

// M1 支撑项：从 d3d9_device.cpp 迁出的 shadow build-context 阶段计时设施。
// 迁出理由：语义模块中的 path blocker widget 指针探针需要进入这些阶段；该设施
// 本身与 D3D9 设备成员无关，只是渲染线程诊断（原为 device TU 内匿名命名空间单实例）。
// 在此头文件中它是 inline thread_local，全程序仍只有一个实例，行为与迁出前逐点
// 相同；device.cpp 的调用点无需改动。

#include "../../../util/util_time.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace dxvk {

enum class War3ShadowBuildContextPhase : uint8_t {
  SeedFromTls = 0u,
  RuntimeBridgeEntry,
  RuntimeBridgeModelInstance,
  RuntimeBridgeShadowObject,
  RuntimeBridgePose,
  RuntimeBridgeRenderObject,
  RuntimeBridgeFinalize,
  VisibleManifest,
  TagStageFallback,
  NativeHint,
  PathBlockerRawAndHandle,
  PathBlockerWorldCache,
  PathBlockerWorldNegativeCache,
  PathBlockerWorldMagicRead,
  PathBlockerWorldRawcodeRead,
  PathBlockerWorldWriteThrough,
  PathBlockerUnitWidget,
  Finalize,
  Count,
};

constexpr size_t kWar3ShadowBuildContextPhaseCount =
    static_cast<size_t>(War3ShadowBuildContextPhase::Count);

// Optional child timer for the sampled PathBlockerBuildContext call only.
// It deliberately shares the outer gate sample decision and sample weight, so
// its phase distribution can be normalized to the exact parent phase at frame
// flush without mixing in BuildShadowSemanticContext calls from other lanes.
class War3ShadowBuildContextRawTiming final {
public:
  War3ShadowBuildContextRawTiming(
      bool active, uint32_t sampleWeight, uint64_t qpcOverheadTicks,
      std::array<uint64_t, kWar3ShadowBuildContextPhaseCount>& ticks,
      std::array<uint32_t, kWar3ShadowBuildContextPhaseCount>& calls)
      : m_active(active),
        m_sampleWeight(std::max(1u, sampleWeight)),
        m_qpcOverheadTicks(qpcOverheadTicks),
        m_ticks(ticks),
        m_calls(calls) {
  }

  ~War3ShadowBuildContextRawTiming() {
    if (m_active)
      closeCurrent(dxvk::high_resolution_clock::get_counter());
  }

  War3ShadowBuildContextRawTiming(
      const War3ShadowBuildContextRawTiming&) = delete;
  War3ShadowBuildContextRawTiming& operator=(
      const War3ShadowBuildContextRawTiming&) = delete;

  inline void enter(War3ShadowBuildContextPhase phase) {
    if (!m_active)
      return;
    const int64_t now = dxvk::high_resolution_clock::get_counter();
    closeCurrent(now);
    m_phase = phase;
    m_begin = now;
    m_calls[static_cast<size_t>(phase)] += m_sampleWeight;
  }

private:
  inline void closeCurrent(int64_t now) {
    if (m_begin == 0 || m_phase == War3ShadowBuildContextPhase::Count)
      return;
    if (now > m_begin) {
      const size_t index = static_cast<size_t>(m_phase);
      const uint64_t elapsed = uint64_t(now - m_begin);
      const uint64_t corrected = elapsed > m_qpcOverheadTicks
          ? elapsed - m_qpcOverheadTicks : 0u;
      m_ticks[index] += corrected * m_sampleWeight;
    }
  }

  bool m_active = false;
  uint32_t m_sampleWeight = 1u;
  uint64_t m_qpcOverheadTicks = 0u;
  std::array<uint64_t, kWar3ShadowBuildContextPhaseCount>& m_ticks;
  std::array<uint32_t, kWar3ShadowBuildContextPhaseCount>& m_calls;
  War3ShadowBuildContextPhase m_phase = War3ShadowBuildContextPhase::Count;
  int64_t m_begin = 0;
};

inline thread_local War3ShadowBuildContextRawTiming*
    g_war3ShadowBuildContextRawTiming = nullptr;

class War3ShadowBuildContextTraceScope final {
public:
  explicit War3ShadowBuildContextTraceScope(
      War3ShadowBuildContextRawTiming* timing)
      : m_previous(g_war3ShadowBuildContextRawTiming) {
    g_war3ShadowBuildContextRawTiming = timing;
  }

  ~War3ShadowBuildContextTraceScope() {
    g_war3ShadowBuildContextRawTiming = m_previous;
  }

  War3ShadowBuildContextTraceScope(
      const War3ShadowBuildContextTraceScope&) = delete;
  War3ShadowBuildContextTraceScope& operator=(
      const War3ShadowBuildContextTraceScope&) = delete;

private:
  War3ShadowBuildContextRawTiming* m_previous = nullptr;
};

inline void War3EnterShadowBuildContextPhase(
    War3ShadowBuildContextPhase phase) {
  if (g_war3ShadowBuildContextRawTiming != nullptr)
    g_war3ShadowBuildContextRawTiming->enter(phase);
}

} // namespace dxvk
