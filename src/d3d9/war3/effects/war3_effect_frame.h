#pragma once

// What every effect executor receives besides its own record: the targets of
// the frame, the world camera, and the identity of the frame.
//
// An executor (war3_*_executor.h) is the single implementation of an effect.
// It depends on a DxvkDevice and these types only - not on D3D9DeviceEx, the
// War3 pipeline or Game.dll state - so the in-process device and the x64 host
// compile the same source and differ only in who fills this structure:
//   in process:  d3d9_war3_*.cpp adapts War3PipelineInput;
//   x64 host:    D3D9Backend adapts its own targets and the scene packet.

#include "war3_effect_scene.h"

#include "../../../dxvk/dxvk_cmdlist.h"
#include "../../../dxvk/dxvk_image.h"
#include "../../../util/util_matrix.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dxvk::war3::effects {

struct EffectCamera {
  bool valid = false;
  Matrix4 view;
  Matrix4 proj;
  Matrix4 viewProj;
  Matrix4 invViewProj;
  // The main world viewport inside the colour target, in pixels.
  uint32_t x = 0, y = 0, width = 0, height = 0;
  float minZ = 0.0f, maxZ = 1.0f;
  // Frame in which the matrices were captured.
  uint64_t frameSerial = 0;
};

struct EffectFrame {
  Rc<DxvkImageView> colorView;
  Rc<DxvkImageView> depthView;
  EffectCamera camera;
  // Monotonic frame number, map session and device generation. An executor
  // refuses a zero and uses them only to tell one run from the next.
  uint64_t frameSerial = 0;
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
};

// Optional observer of one run. beginRecording is called once, immediately
// before the first command of a run that records; a run that ends early never
// calls it. The in-process passes open their GPU timing scope here.
class EffectRecording {
public:
  virtual void beginRecording(const Rc<DxvkCommandList>& ctx) = 0;
protected:
  ~EffectRecording() = default;
};

// The composite matrices, as D3D9DeviceEx::War3RecordWorldCamera derives them.
inline void DeriveCameraMatrices(EffectCamera& camera) {
  camera.viewProj = camera.proj * camera.view;
  camera.invViewProj = inverse(camera.viewProj);
}

// The camera of a scene packet, for the frame the caller numbers.
inline EffectCamera CameraFromScene(const scene::Camera& record, uint64_t frameSerial) {
  EffectCamera camera;
  for (uint32_t row = 0; row < 4; ++row) {
    for (uint32_t column = 0; column < 4; ++column) {
      camera.view[row][column] = record.view[row * 4 + column];
      camera.proj[row][column] = record.projection[row * 4 + column];
    }
  }
  DeriveCameraMatrices(camera);
  camera.x = record.x; camera.y = record.y;
  camera.width = record.width; camera.height = record.height;
  camera.minZ = record.minZ; camera.maxZ = record.maxZ;
  camera.frameSerial = frameSerial;
  camera.valid = true;
  return camera;
}

// Which raw depth value the far plane clears to. The projection determines
// which NDC endpoint is far; viewport MinZ/MaxZ only map that endpoint into
// raw depth and may themselves be reversed, so the decision is made in view
// space first. False when the projection has no usable depth range.
inline bool InferFarClearRaw(const Matrix4& proj, float minZ, float maxZ, float& clearRaw) {
  const Matrix4 invProj = inverse(proj);
  const Vector4 endpoint0 = invProj * Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  const Vector4 endpoint1 = invProj * Vector4(0.0f, 0.0f, 1.0f, 1.0f);
  const auto finite = [](const Vector4& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
  };
  if (!finite(endpoint0) || !finite(endpoint1))
    return false;

  const auto endpointDistance = [](const Vector4& endpoint) {
    if (std::abs(endpoint.w) <= 1.0e-7f)
      return std::numeric_limits<float>::infinity();
    return std::abs(endpoint.z / endpoint.w);
  };

  const float distance0 = endpointDistance(endpoint0);
  const float distance1 = endpointDistance(endpoint1);
  const bool finite0 = std::isfinite(distance0);
  const bool finite1 = std::isfinite(distance1);
  if (!finite0 && !finite1)
    return false;

  if (finite0 && finite1 &&
      std::abs(distance0 - distance1) <=
          1.0e-5f * std::max({distance0, distance1, 1.0f}))
    return false;

  const bool farIsOne = distance1 > distance0;
  clearRaw = farIsOne ? maxZ : minZ;
  return std::isfinite(clearRaw);
}

inline bool MatrixFinite(const Matrix4& matrix) {
  for (uint32_t row = 0; row < 4; ++row)
    for (uint32_t column = 0; column < 4; ++column)
      if (!std::isfinite(matrix[row][column]))
        return false;
  return true;
}

inline VkImageMemoryBarrier2 ImageBarrier(
    DxvkImage* image, const VkImageSubresourceRange& range,
    VkImageLayout from, VkImageLayout to,
    VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess) {
  VkImageMemoryBarrier2 result = {
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
  result.srcStageMask = srcStages;
  result.srcAccessMask = srcAccess;
  result.dstStageMask = dstStages;
  result.dstAccessMask = dstAccess;
  result.oldLayout = from;
  result.newLayout = to;
  result.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  result.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  result.image = image->handle();
  result.subresourceRange = range;
  return result;
}

// A layout an executor can transition from and restore.
inline bool LayoutKnown(VkImageLayout layout) {
  return layout != VK_IMAGE_LAYOUT_UNDEFINED &&
         layout != VK_IMAGE_LAYOUT_PREINITIALIZED &&
         layout != VK_IMAGE_LAYOUT_MAX_ENUM;
}

} // namespace dxvk::war3::effects
