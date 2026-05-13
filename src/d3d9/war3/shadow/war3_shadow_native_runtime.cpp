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
  summary.geometryRejectCount = backend.geometryRejectCount();
  summary.paletteRejectCount = backend.paletteRejectCount();
  summary.materialRejectCount = backend.materialRejectCount();
  summary.submitRejectCount = backend.submitRejectCount();
}

} // namespace

void NativeD3D9BackendRuntime::setDevice(IDirect3DDevice9* device) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_backend.setDevice(device);
  m_lastFrameSerial = 0u;
  m_lastSourcePublishRevision = 0u;
  m_lastSummary = {};
  m_lastSummary.hasDevice = m_backend.hasDevice();
  m_requestedCanonicalFrameSerial = 0u;
  m_canonicalFrame = {};
  m_lastCanonicalPublishCountConsumed = 0u;
  m_canonicalPublishCount = 0u;
  m_canonicalFramePublishCount = 0u;
  m_canonicalPublishRejectNotReadyCount = 0u;
  m_canonicalPublishRejectNoPositionsCount = 0u;
}

void NativeD3D9BackendRuntime::reset() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_backend.reset();
  m_lastFrameSerial = 0u;
  m_lastSourcePublishRevision = 0u;
  m_lastSummary = {};
  m_lastSummary.hasDevice = m_backend.hasDevice();
  m_requestedCanonicalFrameSerial = 0u;
  m_canonicalFrame = {};
  m_lastCanonicalPublishCountConsumed = 0u;
  m_canonicalPublishCount = 0u;
  m_canonicalFramePublishCount = 0u;
  m_canonicalPublishRejectNotReadyCount = 0u;
  m_canonicalPublishRejectNoPositionsCount = 0u;
}

void NativeD3D9BackendRuntime::beginCanonicalFrame(uint64_t frameSerial) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_requestedCanonicalFrameSerial == frameSerial)
    return;
  m_requestedCanonicalFrameSerial = frameSerial;
}

void NativeD3D9BackendRuntime::publishCanonicalDraw(
    const render::CanonicalShadowDrawItem& item) {
  if (!item.readyForShadowConsumer()) {
    ++m_canonicalPublishRejectNotReadyCount;
    return;
  }

  auto& mesh = item.instance.mesh;
  const auto& positions = mesh.effectivePositionVec();
  if (mesh.vertexCount == 0u ||
      positions.size() < size_t(mesh.vertexCount) * 3u) {
    ++m_canonicalPublishRejectNoPositionsCount;
    return;
  }

  CanonicalPreparedDraw draw = {};
  draw.item = item;
  draw.positions = positions;
  if (mesh.useIndices) {
    const uint16_t* indexData = mesh.effectiveIndexData();
    if (indexData == nullptr || mesh.effectiveIndexCount == 0u)
      return;
    draw.indices.assign(indexData, indexData + mesh.effectiveIndexCount);
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_requestedCanonicalFrameSerial == 0u)
    return;
  if (m_canonicalFrame.frameSerial != m_requestedCanonicalFrameSerial) {
    m_canonicalFrame.frameSerial = m_requestedCanonicalFrameSerial;
    m_canonicalFrame.draws.clear();
    m_canonicalFramePublishCount = 0u;
    m_lastCanonicalPublishCountConsumed = 0u;
  }
  m_canonicalFrame.draws.emplace_back(std::move(draw));
  ++m_canonicalPublishCount;
  ++m_canonicalFramePublishCount;
}

void NativeD3D9BackendRuntime::publishCanonicalDrawPrepared(
    const render::CanonicalShadowDrawItem& item,
    const std::vector<float>& positions,
    const uint16_t* indexData,
    uint32_t indexCount) {
  if (!item.readyForShadowConsumer()) {
    ++m_canonicalPublishRejectNotReadyCount;
    return;
  }

  auto& mesh = item.instance.mesh;
  const uint32_t availableVertexCount =
      uint32_t(positions.size() / 3u);
  if (availableVertexCount == 0u) {
    ++m_canonicalPublishRejectNoPositionsCount;
    return;
  }

  CanonicalPreparedDraw draw = {};
  draw.item = item;
  draw.item.instance.mesh.vertexCount =
      std::min(mesh.vertexCount != 0u ? mesh.vertexCount : availableVertexCount,
               availableVertexCount);
  draw.positions = positions;
  if (mesh.useIndices) {
    if (indexData != nullptr && indexCount != 0u) {
      draw.indices.assign(indexData, indexData + indexCount);
    }
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_requestedCanonicalFrameSerial == 0u)
    return;
  if (m_canonicalFrame.frameSerial != m_requestedCanonicalFrameSerial) {
    m_canonicalFrame.frameSerial = m_requestedCanonicalFrameSerial;
    m_canonicalFrame.draws.clear();
    m_canonicalFramePublishCount = 0u;
    m_lastCanonicalPublishCountConsumed = 0u;
  }
  m_canonicalFrame.draws.emplace_back(std::move(draw));
  ++m_canonicalPublishCount;
  ++m_canonicalFramePublishCount;
}

bool NativeD3D9BackendRuntime::buildCanonicalFrame(
    ShadowSubmissionFrame& outFrame) {
  if (m_canonicalFrame.frameSerial == 0u || m_canonicalFrame.draws.empty())
    return false;

  outFrame = {};
  outFrame.frameSerial = m_canonicalFrame.frameSerial;
  outFrame.sourcePublishRevision = m_canonicalFrame.frameSerial;
  outFrame.draws.reserve(m_canonicalFrame.draws.size());

  for (const auto& canonicalDraw : m_canonicalFrame.draws) {
    const auto& item = canonicalDraw.item;
    const auto& identity = item.instance.identity;
    const auto& mesh = item.instance.mesh;
    const auto& material = item.instance.material;
    const auto& skin = item.instance.skin;
    const auto& world = item.instance.worldTransform;

    ShadowDrawPacket packet = {};
    packet.renderable.worldObjectEntry = identity.worldObjectEntry;
    packet.renderable.sceneNode = identity.sceneNode;
    packet.renderable.unitPtr = identity.unitPtr;
    packet.renderable.renderablePart = identity.renderablePart;
    packet.renderable.payload = identity.renderablePart;
    packet.renderable.meshData = identity.meshDataPtr;
    packet.renderable.runtimeModelPtr = identity.runtimeModelPtr;
    packet.renderable.modelResourcePtr = identity.modelResourcePtr;
    packet.renderable.runtimeGeosetPtr = identity.runtimeGeosetPtr;
    packet.renderable.runtimeGeosetDataPtr = identity.runtimeGeosetDataPtr;
    packet.renderable.modelKey = identity.modelKey;
    packet.renderable.jHandle = identity.jHandle;
    packet.renderable.rawcode = identity.rawcode;
    packet.renderable.layerIndex = material.layerIndex;
    packet.renderable.objectKind = identity.objectKind;
    packet.renderable.frameSerial = identity.frameSerial;
    packet.renderable.transparentType = material.transparentType;
    packet.renderable.queueKind =
        material.queueKind != 0u
            ? render::VisibleRenderableQueueKind::Transparent
            : render::VisibleRenderableQueueKind::MainQueue;

    packet.resource.modelResourcePtr = identity.modelResourcePtr;
    packet.resource.modelKey = identity.modelKey;
    packet.resource.geosetIndex = mesh.geosetIndex;
    packet.resource.vertexCount = mesh.vertexCount;
    packet.resource.primitiveRecordCount = mesh.primitiveRecordCount;
    packet.resource.explicitBlendCount = skin.explicitBlendCount;
    packet.resource.contentHash = mesh.contentHash;
    packet.resource.topology = mesh.topology;
    packet.resource.ownedPositions = canonicalDraw.positions;
    packet.resource.positions = &packet.resource.ownedPositions;
    if (!canonicalDraw.indices.empty()) {
      packet.resource.ownedIndices = canonicalDraw.indices;
      packet.resource.indices = &packet.resource.ownedIndices;
    }
    if (!skin.groupSlots.empty()) {
      packet.resource.ownedVertexGroupIndices = skin.groupSlots;
      packet.resource.vertexGroupIndices =
          &packet.resource.ownedVertexGroupIndices;
    }
    if (!skin.explicitBlendWeights.empty() &&
        !skin.explicitBlendIndices.empty()) {
      packet.resource.ownedVertexBlendWeights = skin.explicitBlendWeights;
      packet.resource.ownedVertexBlendIndices = skin.explicitBlendIndices;
      packet.resource.vertexBlendWeights =
          &packet.resource.ownedVertexBlendWeights;
      packet.resource.vertexBlendIndices =
          &packet.resource.ownedVertexBlendIndices;
    }

    packet.material.signatureHash = material.signatureHash;
    packet.material.alphaMode = material.alphaMode;
    packet.material.alphaCutoutRef = material.alphaCutoutRef;
    packet.material.blendOrDrawMode = material.blendOrDrawMode;
    packet.material.layerIndex = material.layerIndex;
    packet.material.queueKind = material.queueKind;
    packet.material.transparentType = material.transparentType;

    packet.path = skin.skinned ? ShadowDrawPath::Skinned : ShadowDrawPath::Rigid;
    packet.maxVertexGroupSlot = skin.maxVertexGroupSlot;
    packet.matrixGroupsUseAveraging = skin.matrixGroupsUseAveraging;
    if (!skin.palette.empty()) {
      packet.hasRuntimeGroupPalette = true;
      packet.runtimeGroupPalette = skin.palette;
      packet.runtimeGroupPaletteHash = skin.paletteHash;
    }

    if (world.valid) {
      packet.pose.hasWorldTransform = true;
      packet.pose.worldTransform = world.matrix;
    }

    outFrame.draws.emplace_back(std::move(packet));
    auto& outPacket = outFrame.draws.back();
    outPacket.resource.positions = &outPacket.resource.ownedPositions;
    outPacket.resource.indices =
        outPacket.resource.ownedIndices.empty()
            ? nullptr
            : &outPacket.resource.ownedIndices;
    outPacket.resource.vertexGroupIndices =
        outPacket.resource.ownedVertexGroupIndices.empty()
            ? nullptr
            : &outPacket.resource.ownedVertexGroupIndices;
    outPacket.resource.vertexBlendWeights =
        outPacket.resource.ownedVertexBlendWeights.empty()
            ? nullptr
            : &outPacket.resource.ownedVertexBlendWeights;
    outPacket.resource.vertexBlendIndices =
        outPacket.resource.ownedVertexBlendIndices.empty()
            ? nullptr
            : &outPacket.resource.ownedVertexBlendIndices;
  }

  return !outFrame.draws.empty();
}

bool NativeD3D9BackendRuntime::buildLatestFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_backend.hasDevice()) {
    m_lastSummary.hasDevice = false;
    return false;
  }

  ShadowSubmissionFrame canonicalFrame = {};
  const bool hasCanonicalFrame = buildCanonicalFrame(canonicalFrame);

  const auto frame =
      hasCanonicalFrame
          ? std::shared_ptr<const ShadowSubmissionFrame>{}
          : ShadowValidationRuntime::instance().snapshotRenderableFrameShared();
  if (!hasCanonicalFrame &&
      (!frame || frame->frameSerial == 0u || frame->draws.empty())) {
    m_lastSummary.hasDevice = true;
    return false;
  }

  const uint64_t frameSerial =
      hasCanonicalFrame ? canonicalFrame.frameSerial : frame->frameSerial;
  const uint64_t sourcePublishRevision =
      hasCanonicalFrame ? canonicalFrame.sourcePublishRevision
                        : frame->sourcePublishRevision;

  if (frameSerial == m_lastFrameSerial &&
      sourcePublishRevision == m_lastSourcePublishRevision &&
      (!hasCanonicalFrame ||
       m_canonicalFramePublishCount == m_lastCanonicalPublishCountConsumed)) {
    m_lastSummary.hasDevice = true;
    return m_lastSummary.submittedDrawCount != 0u;
  }

  m_backend.beginFrame(frameSerial);
  if (hasCanonicalFrame)
    m_core.submitFrame(canonicalFrame, m_backend);
  else
    m_core.submitFrame(*frame, m_backend);
  m_backend.endFrame();

  m_lastFrameSerial = frameSerial;
  m_lastSourcePublishRevision = sourcePublishRevision;
  m_lastCanonicalPublishCountConsumed =
      hasCanonicalFrame ? m_canonicalFramePublishCount : 0u;
  m_lastSummary.frameSerial = frameSerial;
  m_lastSummary.sourcePublishRevision = sourcePublishRevision;
  m_lastSummary.submittedDrawCount = m_backend.submittedDrawCount();
  m_lastSummary.submittedRigidDrawCount = m_backend.submittedRigidDrawCount();
  m_lastSummary.submittedSkinnedDrawCount =
      m_backend.submittedSkinnedDrawCount();
  m_lastSummary.usedCanonicalFrame = hasCanonicalFrame;
  m_lastSummary.canonicalDrawCount =
      hasCanonicalFrame ? canonicalFrame.draws.size() : 0u;
  m_lastSummary.canonicalFrameSerial =
      hasCanonicalFrame ? canonicalFrame.frameSerial : 0u;
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
  if (summary.canonicalFrameSerial == 0u && m_canonicalFrame.frameSerial != 0u) {
    summary.canonicalFrameSerial = m_canonicalFrame.frameSerial;
  }
  if (summary.canonicalDrawCount == 0u && !m_canonicalFrame.draws.empty()) {
    summary.canonicalDrawCount = m_canonicalFrame.draws.size();
  }
  summary.canonicalPublishCount = m_canonicalFramePublishCount;
  summary.canonicalPublishRejectNotReadyCount =
      m_canonicalPublishRejectNotReadyCount;
  summary.canonicalPublishRejectNoPositionsCount =
      m_canonicalPublishRejectNoPositionsCount;
  if (summary.hasDevice) {
    FillNativeBackendExecutionSummary(summary, m_backend);
  }
  return summary;
}

} // namespace dxvk::war3::shadow
