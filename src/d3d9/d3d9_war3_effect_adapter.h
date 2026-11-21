#pragma once

// In-process side of the effect executors (war3/effects): turns the frame the
// War3 pipeline captured into the executor's input. The x64 host fills the
// same structure from its own targets and the scene packet.

#include "d3d9_war3_pipeline.h"
#include "war3/effects/war3_effect_frame.h"

namespace dxvk {

inline war3::effects::EffectCamera War3EffectCamera(const War3WorldCameraState& camera) {
  war3::effects::EffectCamera result;
  result.valid = camera.valid;
  result.view = camera.view;
  result.proj = camera.proj;
  result.viewProj = camera.viewProj;
  result.invViewProj = camera.invViewProj;
  result.x = camera.viewport.X;
  result.y = camera.viewport.Y;
  result.width = camera.viewport.Width;
  result.height = camera.viewport.Height;
  result.minZ = camera.viewport.MinZ;
  result.maxZ = camera.viewport.MaxZ;
  result.frameSerial = camera.frameSerial;
  return result;
}

inline war3::effects::EffectFrame War3EffectFrame(const War3PipelineInput& input) {
  war3::effects::EffectFrame frame;
  frame.colorView = input.colorView;
  frame.depthView = input.depthView;
  frame.camera = War3EffectCamera(input.scene.worldCamera);
  frame.frameSerial = input.frameSerial;
  frame.mapEpoch = input.mapEpoch;
  frame.deviceEpoch = input.deviceEpoch;
  return frame;
}

} // namespace dxvk
