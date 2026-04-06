// war3_native_renderer_probe.cpp - Native Renderer 对照采样/统计模块实现

#include "war3_native_renderer_probe.h"

#include "../../d3d9_war3_debug.h"
#include "../core/war3_internal_test_config.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>

namespace dxvk::war3::render {

namespace {

struct WorldGroupCounters {
  uint64_t calls = 0;
  uint64_t listCount = 0;
  uint64_t keptCount = 0;
  uint64_t sceneNodeCount = 0;
  uint64_t handleCount = 0;
  uint64_t trackedHitCount = 0;
  uint64_t filteredOutCount = 0;
  uint64_t filteredCalls = 0;
};

struct DispatchCounters {
  uint64_t commonCalls = 0;
  uint64_t specialCalls = 0;
  uint64_t worldCalls = 0;
  uint64_t hasSceneNode = 0;
  uint64_t registryHit = 0;
  uint64_t resolvedHandle = 0;
};

struct QueueCounters {
  uint64_t flushCalls = 0;
  uint32_t maxNumElements = 0;
  uint32_t maxSortedCount = 0;

  // 每帧会多次触发 Flush（约 9 次/帧），为了让统计更有代表性，采样选择“本帧
  // sortedCount 最大”的那次 Flush，而不是固定采第一笔。
  uint32_t bestSortedCount = 0;
  uint32_t bestSampled = 0;
  std::array<uint32_t, 4> bestFlagsAnd3Counts = {};

  // 窗口统计（跨帧累加）
  uint64_t sampledFrames = 0;
  uint64_t sampleTotal = 0;
  std::array<uint64_t, 4> flagsAnd3Counts = {};
};

struct FrameCounters {
  std::array<WorldGroupCounters, 9> groups = {};
  DispatchCounters dispatch = {};
  QueueCounters queue = {};
};

} // namespace

class NativeRendererProbeState {
public:
  void ensureInitialized() {
    if (m_initialized)
      return;

    // 内部测试版本：不依赖环境变量，直接使用编译期配置。
    m_reportIntervalFrames = (std::max)(
        1u, dxvk::war3::internal::kNativeRendererProbeReportIntervalFrames);
    m_initialized = true;
  }

  void onFrameBegin() { m_frame = FrameCounters{}; }

  void onFrameEnd() {
    m_windowFrames++;

    // 累加统计窗口
    for (size_t i = 0; i < m_frame.groups.size(); i++) {
      auto& w = m_window.groups[i];
      const auto& f = m_frame.groups[i];
      w.calls += f.calls;
      w.listCount += f.listCount;
      w.keptCount += f.keptCount;
      w.sceneNodeCount += f.sceneNodeCount;
      w.handleCount += f.handleCount;
      w.trackedHitCount += f.trackedHitCount;
      w.filteredOutCount += f.filteredOutCount;
      w.filteredCalls += f.filteredCalls;
    }

    m_window.dispatch.commonCalls += m_frame.dispatch.commonCalls;
    m_window.dispatch.specialCalls += m_frame.dispatch.specialCalls;
    m_window.dispatch.worldCalls += m_frame.dispatch.worldCalls;
    m_window.dispatch.hasSceneNode += m_frame.dispatch.hasSceneNode;
    m_window.dispatch.registryHit += m_frame.dispatch.registryHit;
    m_window.dispatch.resolvedHandle += m_frame.dispatch.resolvedHandle;

    m_window.queue.flushCalls += m_frame.queue.flushCalls;
    m_window.queue.maxNumElements = (std::max)(m_window.queue.maxNumElements,
                                               m_frame.queue.maxNumElements);
    m_window.queue.maxSortedCount = (std::max)(m_window.queue.maxSortedCount,
                                               m_frame.queue.maxSortedCount);
    if (m_frame.queue.bestSampled != 0) {
      m_window.queue.sampledFrames += 1;
      m_window.queue.sampleTotal += m_frame.queue.bestSampled;
      for (size_t i = 0; i < 4; i++) {
        m_window.queue.flagsAnd3Counts[i] +=
            static_cast<uint64_t>(m_frame.queue.bestFlagsAnd3Counts[i]);
      }
    }

    if (m_windowFrames >= m_reportIntervalFrames) {
      reportAndReset();
    }
  }

  void onWorldObjectsGroup(int groupIdx,
                           uint32_t listCount,
                           uint32_t keptCount,
                           uint32_t sceneNodeCount,
                           uint32_t handleCount,
                           uint32_t trackedHitCount,
                           bool filtered) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(m_frame.groups.size()))
      return;

    auto& g = m_frame.groups[static_cast<size_t>(groupIdx)];
    g.calls++;
    g.listCount += listCount;
    g.keptCount += keptCount;
    g.sceneNodeCount += sceneNodeCount;
    g.handleCount += handleCount;
    g.trackedHitCount += trackedHitCount;
    if (filtered) {
      g.filteredCalls++;
      if (listCount > keptCount)
        g.filteredOutCount += (listCount - keptCount);
    }
  }

  void onRenderQueueFlush(const NativeRendererProbe::RenderQueueSample& s) {
    m_frame.queue.flushCalls++;
    m_frame.queue.maxNumElements =
        (std::max)(m_frame.queue.maxNumElements, s.numElements);
    m_frame.queue.maxSortedCount =
        (std::max)(m_frame.queue.maxSortedCount, s.sortedCount);

    if (s.sampled == 0)
      return;

    const bool better =
        (s.sortedCount > m_frame.queue.bestSortedCount) ||
        (s.sortedCount == m_frame.queue.bestSortedCount &&
         s.sampled > m_frame.queue.bestSampled);
    if (!better)
      return;

    m_frame.queue.bestSortedCount = s.sortedCount;
    m_frame.queue.bestSampled = s.sampled;
    for (size_t i = 0; i < 4; i++) {
      m_frame.queue.bestFlagsAnd3Counts[i] = s.flagsAnd3Counts[i];
    }
  }

  void onDispatch(bool isType3,
                  bool isWorldGroup,
                  bool hasSceneNode,
                  bool registryHit,
                  bool hasResolvedHandle) {
    if (isType3)
      m_frame.dispatch.specialCalls++;
    else
      m_frame.dispatch.commonCalls++;

    if (isWorldGroup)
      m_frame.dispatch.worldCalls++;
    if (hasSceneNode)
      m_frame.dispatch.hasSceneNode++;
    if (registryHit)
      m_frame.dispatch.registryHit++;
    if (hasResolvedHandle)
      m_frame.dispatch.resolvedHandle++;
  }

private:
  void reportAndReset() {
    // 仅输出关键数字，避免刷屏。需要更细的数据可再加开关或导出为 JSON。
    war3dbg::Print(
        "DXVK NativeRendererProbe: window=%" PRIu64
        " frames (interval=%u)\n",
        m_windowFrames, m_reportIntervalFrames);

    for (size_t i = 0; i < m_window.groups.size(); i++) {
      const auto& g = m_window.groups[i];
      if (g.calls == 0)
        continue;

      const double keepRatio =
          (g.listCount > 0) ? (double(g.keptCount) / double(g.listCount)) : 0.0;
      const double sceneRatio =
          (g.keptCount > 0)
              ? (double(g.sceneNodeCount) / double(g.keptCount))
              : 0.0;
      const double handleRatio =
          (g.keptCount > 0) ? (double(g.handleCount) / double(g.keptCount)) : 0.0;

      war3dbg::Print(
          "  Group[%zu]: calls=%" PRIu64
          " list=%" PRIu64
          " kept=%" PRIu64
          " keep=%.2f%% scene=%.2f%% handle=%.2f%% filteredCalls=%" PRIu64
          " filteredOut=%" PRIu64 "\n",
          i, g.calls, g.listCount, g.keptCount, keepRatio * 100.0,
          sceneRatio * 100.0, handleRatio * 100.0, g.filteredCalls,
          g.filteredOutCount);
    }

    const auto& d = m_window.dispatch;
    const uint64_t totalDispatch = d.commonCalls + d.specialCalls;
    if (totalDispatch != 0) {
      const double worldRatio =
          double(d.worldCalls) / double(totalDispatch);
      const double sceneRatio =
          double(d.hasSceneNode) / double(totalDispatch);
      const double hitRatio =
          (d.hasSceneNode != 0) ? (double(d.registryHit) / double(d.hasSceneNode))
                                : 0.0;
      const double handleRatio =
          double(d.resolvedHandle) / double(totalDispatch);

      war3dbg::Print(
          "  Dispatch: total=%" PRIu64 " common=%" PRIu64 " special=%" PRIu64
          " world=%.2f%% scene=%.2f%% hit(scene)=%.2f%% handle=%.2f%%\n",
          totalDispatch, d.commonCalls, d.specialCalls, worldRatio * 100.0,
          sceneRatio * 100.0, hitRatio * 100.0, handleRatio * 100.0);
    }

    const auto& q = m_window.queue;
    if (q.flushCalls != 0) {
      war3dbg::Print(
          "  RenderQueue: flushCalls=%" PRIu64
          " maxElements=%u maxSorted=%u sampledFrames=%" PRIu64
          " sampleTotal=%" PRIu64 "\n",
          q.flushCalls, q.maxNumElements, q.maxSortedCount, q.sampledFrames,
          q.sampleTotal);
      if (q.sampleTotal != 0) {
        const double inv = 1.0 / double(q.sampleTotal);
        war3dbg::Print(
            "    flags&3: [0]=%.2f%% [1]=%.2f%% [2]=%.2f%% [3]=%.2f%%\n",
            double(q.flagsAnd3Counts[0]) * inv * 100.0,
            double(q.flagsAnd3Counts[1]) * inv * 100.0,
            double(q.flagsAnd3Counts[2]) * inv * 100.0,
            double(q.flagsAnd3Counts[3]) * inv * 100.0);
      }
    }

    // 重置窗口统计
    m_window = FrameCounters{};
    m_windowFrames = 0;
  }

private:
  bool m_initialized = false;
  uint32_t m_reportIntervalFrames = 300;

  FrameCounters m_frame = {};
  FrameCounters m_window = {};
  uint64_t m_windowFrames = 0;
};

static NativeRendererProbeState g_probeState;

NativeRendererProbe& NativeRendererProbe::instance() {
  static NativeRendererProbe* s_instance = new NativeRendererProbe();
  return *s_instance;
}

bool NativeRendererProbe::IsEnabled() {
  static std::atomic<bool> s_logged{false};
  const bool enabled = dxvk::war3::internal::kNativeRendererProbeEnabled;

  // 一次性日志：用于确认当前编译版本是否启用了 Probe（不依赖环境变量）。
  if (!s_logged.exchange(true, std::memory_order_relaxed)) {
    war3dbg::Print(
        "DXVK NativeRendererProbe: enabled=%d (built-in) interval=%u\n",
        enabled ? 1 : 0,
        dxvk::war3::internal::kNativeRendererProbeReportIntervalFrames);
  }

  return enabled;
}

void NativeRendererProbe::OnFrameBegin() {
  if (!IsEnabled())
    return;
  g_probeState.ensureInitialized();
  g_probeState.onFrameBegin();
}

void NativeRendererProbe::OnFrameEnd() {
  if (!IsEnabled())
    return;
  g_probeState.ensureInitialized();
  g_probeState.onFrameEnd();
}

void NativeRendererProbe::OnWorldObjectsGroup(int groupIdx,
                                              uint32_t listCount,
                                              uint32_t keptCount,
                                              uint32_t sceneNodeCount,
                                              uint32_t handleCount,
                                              uint32_t trackedHitCount,
                                              bool filtered) {
  if (!IsEnabled())
    return;
  g_probeState.onWorldObjectsGroup(groupIdx, listCount, keptCount, sceneNodeCount,
                                  handleCount, trackedHitCount, filtered);
}

void NativeRendererProbe::OnRenderQueueFlush(const RenderQueueSample& s) {
  if (!IsEnabled())
    return;
  g_probeState.onRenderQueueFlush(s);
}

void NativeRendererProbe::OnDispatch(bool isType3,
                                     bool isWorldGroup,
                                     bool hasSceneNode,
                                     bool registryHit,
                                     bool hasResolvedHandle) {
  if (!IsEnabled())
    return;
  g_probeState.onDispatch(isType3, isWorldGroup, hasSceneNode, registryHit,
                          hasResolvedHandle);
}

} // namespace dxvk::war3::render
