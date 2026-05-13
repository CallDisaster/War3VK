#pragma once

#include "war3_shadow_backend_native_d3d9.h"
#include "war3_shadow_renderer_core.h"
#include "../render/war3_canonical_draw.h"

#include <cstdint>
#include <mutex>

namespace dxvk::war3::shadow {

struct NativeD3D9BackendSummary {
  uint64_t frameSerial = 0;
  uint64_t sourcePublishRevision = 0;
  uint64_t submittedDrawCount = 0;
  uint64_t submittedRigidDrawCount = 0;
  uint64_t submittedSkinnedDrawCount = 0;
  uint64_t executedFrameSerial = 0;
  uint64_t executedDrawCount = 0;
  uint64_t executedRigidDrawCount = 0;
  uint64_t executedSkinnedDrawCount = 0;
  uint64_t executeAttemptCount = 0;
  uint64_t executeSuccessCount = 0;
  uint64_t lastSuccessfulExecutedFrameSerial = 0;
  uint64_t lastSuccessfulExecutedDrawCount = 0;
  uint64_t executeSkippedNoDeviceCount = 0;
  uint64_t executeSkippedNoDrawsCount = 0;
  uint64_t lastExecuteSubmittedDrawCount = 0;
  uint64_t lastExecuteFailedDrawCount = 0;
  uint64_t lastExecuteSubmittedRigidDrawCount = 0;
  uint64_t lastExecuteSubmittedSkinnedDrawCount = 0;
  uint64_t lastExecuteExecutedRigidDrawCount = 0;
  uint64_t lastExecuteExecutedSkinnedDrawCount = 0;
  uint64_t geometryCount = 0;
  uint64_t paletteCount = 0;
  uint64_t materialCount = 0;
  bool usedCanonicalFrame = false;
  uint64_t canonicalDrawCount = 0;
  uint64_t canonicalFrameSerial = 0;
  uint64_t canonicalPublishCount = 0;
  uint64_t canonicalPublishRejectNotReadyCount = 0;
  uint64_t canonicalPublishRejectNoPositionsCount = 0;
  uint64_t geometryRejectCount = 0;
  uint64_t paletteRejectCount = 0;
  uint64_t materialRejectCount = 0;
  uint64_t submitRejectCount = 0;
  bool hasDevice = false;
};

class NativeD3D9BackendRuntime {
public:
  static NativeD3D9BackendRuntime& instance();

  void beginCanonicalFrame(uint64_t frameSerial);
  void publishCanonicalDraw(const render::CanonicalShadowDrawItem& item);
  void publishCanonicalDrawPrepared(
      const render::CanonicalShadowDrawItem& item,
      const std::vector<float>& positions,
      const uint16_t* indexData,
      uint32_t indexCount);
  void setDevice(IDirect3DDevice9* device);
  void reset();
  bool buildLatestFrame();
  bool executePreparedFrame();
  NativeD3D9BackendSummary snapshot() const;

private:
  struct CanonicalPreparedDraw {
    render::CanonicalShadowDrawItem item = {};
    std::vector<float> positions;
    std::vector<uint16_t> indices;
  };

  struct CanonicalPreparedFrame {
    uint64_t frameSerial = 0;
    std::vector<CanonicalPreparedDraw> draws;
  };

  bool buildCanonicalFrame(ShadowSubmissionFrame& outFrame);

  NativeD3D9BackendRuntime() = default;

  mutable std::mutex m_mutex;
  NativeD3D9Backend m_backend;
  ShadowRendererCore m_core = {};
  NativeD3D9BackendSummary m_lastSummary = {};
  uint64_t m_lastFrameSerial = 0;
  uint64_t m_lastSourcePublishRevision = 0;
  uint64_t m_lastCanonicalPublishCountConsumed = 0;
  uint64_t m_requestedCanonicalFrameSerial = 0;
  CanonicalPreparedFrame m_canonicalFrame = {};
  uint64_t m_canonicalPublishCount = 0;
  uint64_t m_canonicalFramePublishCount = 0;
  uint64_t m_canonicalPublishRejectNotReadyCount = 0;
  uint64_t m_canonicalPublishRejectNoPositionsCount = 0;
};

} // namespace dxvk::war3::shadow
