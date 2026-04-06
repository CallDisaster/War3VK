#pragma once

#include "../../war3_shader_api.h"

#include <vector>

namespace dxvk::war3::render {

/**
 * @brief 帧图事件分发阶段。
 *
 * 说明：
 * - `BeforeUiPrePass`：BeforeUi 插入点执行 Pass 前；
 * - `BeforeUiPostPass`：BeforeUi 插入点执行 Pass 后；
 * - `BeforePresentPostPass`：BeforePresent 插入点执行 Pass 后。
 */
enum class FrameGraphDispatchStage : uint8_t {
  BeforeUiPrePass = 0,
  BeforeUiPostPass = 1,
  BeforePresentPostPass = 2,
};

/**
 * @brief War3 轻量帧图事件计划。
 *
 * 该计划只负责“事件序列”编排，不改变已有渲染提交路径，
 * 用于将事件分发逻辑从 `d3d9_war3_pipeline.cpp` 解耦。
 */
class War3FrameGraphPlan final {
public:
  /**
   * @brief 获取默认事件计划（只读单例）。
   */
  static const War3FrameGraphPlan& Default();

  /**
   * @brief 获取指定阶段的事件序列。
   */
  const std::vector<war3shader::RenderEventID>& Events(
      FrameGraphDispatchStage stage) const;

private:
  War3FrameGraphPlan();

  std::vector<war3shader::RenderEventID> m_beforeUiPrePass;
  std::vector<war3shader::RenderEventID> m_beforeUiPostPass;
  std::vector<war3shader::RenderEventID> m_beforePresentPostPass;
};

} // namespace dxvk::war3::render

