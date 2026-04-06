// war3_native_renderer_probe.h - Native Renderer 对照采样/统计模块
//
// 目标：
// - 在不替换游戏核心渲染逻辑的前提下，采集关键数据（RenderQueue/WorldObjects/ExecBatch 命中率）。
// - 为后续 RenderBatch_Submit / WorldObjects_RenderGroup 的“可回退重写”提供数据支撑。
//
// 开关：
// - 内部测试版本：由 `src/d3d9/war3/core/war3_internal_test_config.h` 编译期控制
// - 原设计（可回退）：DXVK_WAR3_NATIVE_RENDERER / DXVK_WAR3_NATIVE_RENDERER_REPORT_INTERVAL

#pragma once

#include <array>
#include <cstdint>

namespace dxvk::war3::render {

class NativeRendererProbe final {
public:
  static NativeRendererProbe& instance();

  // 注意：这是一个轻量的全局开关查询，用于热路径（Dispatch）快速分支。
  // 内部测试版本不依赖环境变量，直接返回编译期配置。
  static bool IsEnabled();

  // 帧边界（与 War3PerfMonitor 的 beginFrame/endFrame 对齐）
  void OnFrameBegin();
  void OnFrameEnd();

  // WorldObjects_RenderGroup 对象收集统计
  void OnWorldObjectsGroup(int groupIdx,
                           uint32_t listCount,
                           uint32_t keptCount,
                           uint32_t sceneNodeCount,
                           uint32_t handleCount,
                           uint32_t trackedHitCount,
                           bool filtered);

  // RenderQueue_FlushSortedItems 统计（可多次调用，Probe 内部会做“每帧只采样一次”的节流）
  struct RenderQueueSample {
    uint32_t numElements = 0;
    uint32_t sortedCount = 0;
    uint32_t sampled = 0;
    std::array<uint32_t, 4> flagsAnd3Counts = {};
  };
  void OnRenderQueueFlush(const RenderQueueSample& s);

  // ExecBatch/Dispatch 统计（热路径，仅在 Probe 启用时调用）
  void OnDispatch(bool isType3,
                  bool isWorldGroup,
                  bool hasSceneNode,
                  bool registryHit,
                  bool hasResolvedHandle);

private:
  NativeRendererProbe() = default;
};

} // namespace dxvk::war3::render
