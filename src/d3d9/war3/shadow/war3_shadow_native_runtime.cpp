#include "war3_shadow_native_runtime.h"

namespace dxvk::war3::shadow {

NativeD3D9BackendRuntime& NativeD3D9BackendRuntime::instance() {
  static NativeD3D9BackendRuntime s_instance;
  return s_instance;
}

namespace {

void FillNativeBackendExecutionSummary(NativeD3D9BackendSummary& summary,
                                       const NativeD3D9Backend& backend) {
  summary.executedFrameSerial = backend.executedFrameSerial();
  summary.executedDrawCount = backend.executedDrawCount();
  summary.executedRigidDrawCount = backend.executedRigidDrawCount();
  summary.executedSkinnedDrawCount = backend.executedSkinnedDrawCount();
  summary.executeAttemptCount = backend.executeAttemptCount();
  summary.executeSuccessCount = backend.executeSuccessCount();
  summary.lastSuccessfulExecutedFrameSerial =
      backend.lastSuccessfulExecutedFrameSerial();
  summary.lastSuccessfulExecutedDrawCount =
      backend.lastSuccessfulExecutedDrawCount();
  summary.executeSkippedNoDeviceCount = backend.executeSkippedNoDeviceCount();
  summary.executeSkippedNoDrawsCount = backend.executeSkippedNoDrawsCount();
  summary.lastExecuteSubmittedDrawCount =
      backend.lastExecuteSubmittedDrawCount();
  summary.lastExecuteFailedDrawCount = backend.lastExecuteFailedDrawCount();
  summary.lastExecuteSubmittedRigidDrawCount =
      backend.lastExecuteSubmittedRigidDrawCount();
  summary.lastExecuteSubmittedSkinnedDrawCount =
      backend.lastExecuteSubmittedSkinnedDrawCount();
  summary.lastExecuteExecutedRigidDrawCount =
      backend.lastExecuteExecutedRigidDrawCount();
  summary.lastExecuteExecutedSkinnedDrawCount =
      backend.lastExecuteExecutedSkinnedDrawCount();
  summary.geometryCount = backend.geometryCount();
  summary.paletteCount = backend.paletteCount();
  summary.materialCount = backend.materialCount();
}

} // namespace

void NativeD3D9BackendRuntime::setDevice(IDirect3DDevice9* device) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_backend.setDevice(device);
  m_lastFrameSerial = 0u;
  m_lastSourcePublishRevision = 0u;
  m_lastSummary = {};
  m_lastSummary.hasDevice = m_backend.hasDevice();
}

void NativeD3D9BackendRuntime::reset() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_backend.reset();
  m_lastFrameSerial = 0u;
  m_lastSourcePublishRevision = 0u;
  m_lastSummary = {};
  m_lastSummary.hasDevice = m_backend.hasDevice();
}

bool NativeD3D9BackendRuntime::buildLatestFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_backend.hasDevice()) {
    m_lastSummary.hasDevice = false;
    return false;
  }

  const auto frame =
      ShadowValidationRuntime::instance().snapshotRenderableFrameShared();
  if (!frame || frame->frameSerial == 0u || frame->draws.empty()) {
    m_lastSummary = {};
    m_lastSummary.hasDevice = true;
    return false;
  }

  if (frame->frameSerial == m_lastFrameSerial &&
      frame->sourcePublishRevision == m_lastSourcePublishRevision) {
    m_lastSummary.hasDevice = true;
    return m_lastSummary.submittedDrawCount != 0u;
  }

  m_backend.beginFrame(frame->frameSerial);
  m_core.submitFrame(*frame, m_backend);
  m_backend.endFrame();

  m_lastFrameSerial = frame->frameSerial;
  m_lastSourcePublishRevision = frame->sourcePublishRevision;
  m_lastSummary.frameSerial = frame->frameSerial;
  m_lastSummary.sourcePublishRevision = frame->sourcePublishRevision;
  m_lastSummary.submittedDrawCount = m_backend.submittedDrawCount();
  m_lastSummary.submittedRigidDrawCount = m_backend.submittedRigidDrawCount();
  m_lastSummary.submittedSkinnedDrawCount =
      m_backend.submittedSkinnedDrawCount();
  FillNativeBackendExecutionSummary(m_lastSummary, m_backend);
  m_lastSummary.hasDevice = true;
  return m_lastSummary.submittedDrawCount != 0u;
}

bool NativeD3D9BackendRuntime::executePreparedFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_backend.hasDevice()) {
    m_lastSummary.hasDevice = false;
    return false;
  }

  const bool ok = m_backend.executePreparedDraws();
  FillNativeBackendExecutionSummary(m_lastSummary, m_backend);
  m_lastSummary.hasDevice = true;
  return ok;
}

NativeD3D9BackendSummary NativeD3D9BackendRuntime::snapshot() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  NativeD3D9BackendSummary summary = m_lastSummary;
  summary.hasDevice = m_backend.hasDevice();
  if (summary.hasDevice) {
    FillNativeBackendExecutionSummary(summary, m_backend);
  }
  return summary;
}

} // namespace dxvk::war3::shadow
