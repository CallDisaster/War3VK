#include "war3_frame_graph.h"

namespace dxvk::war3::render {

War3FrameGraphPlan::War3FrameGraphPlan() {
  // BeforeUi: Pass 前触发世界渲染闭环和后处理开始事件。
  m_beforeUiPrePass = {
      war3shader::RenderEventID::WORLD_RENDER_BEGIN,
      war3shader::RenderEventID::WORLD_RENDER_END,
      war3shader::RenderEventID::POST_PROCESS_BEGIN,
  };

  // BeforeUi: Pass 后触发后处理结束与 UI 开始事件。
  m_beforeUiPostPass = {
      war3shader::RenderEventID::POST_PROCESS_END,
      war3shader::RenderEventID::UI_RENDER_BEGIN,
  };

  // BeforePresent: 帧结束事件。
  m_beforePresentPostPass = {
      war3shader::RenderEventID::FRAME_END,
  };
}

const War3FrameGraphPlan& War3FrameGraphPlan::Default() {
  static const War3FrameGraphPlan kDefaultPlan;
  return kDefaultPlan;
}

const std::vector<war3shader::RenderEventID>& War3FrameGraphPlan::Events(
    FrameGraphDispatchStage stage) const {
  switch (stage) {
  case FrameGraphDispatchStage::BeforeUiPrePass:
    return m_beforeUiPrePass;
  case FrameGraphDispatchStage::BeforeUiPostPass:
    return m_beforeUiPostPass;
  case FrameGraphDispatchStage::BeforePresentPostPass:
    return m_beforePresentPostPass;
  default:
    return m_beforePresentPostPass;
  }
}

} // namespace dxvk::war3::render

