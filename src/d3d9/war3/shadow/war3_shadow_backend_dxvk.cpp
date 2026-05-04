#include "war3_shadow_backend_dxvk.h"

#include "war3_shadow_renderer_core.h"

#include <functional>

namespace dxvk::war3::shadow {

namespace {

uint64_t MakeGeometryCacheKey(const ShadowDrawPacket& packet) {
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, packet.resource.contentHash);
  hash = bit::fnv1a_iter(hash, packet.renderable.modelKey);
  hash = bit::fnv1a_iter(hash, uint64_t(packet.resource.geosetIndex));
  hash = bit::fnv1a_iter(hash, packet.material.signatureHash);
  hash = bit::fnv1a_iter(hash, uint32_t(packet.material.alphaMode));
  return hash != 0u ? hash : 1u;
}

uint64_t MakePaletteCacheKey(const ShadowDrawPacket& packet) {
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, packet.pose.matrixHash);
  hash = bit::fnv1a_iter(hash, uint64_t(packet.pose.matrixCount));
  hash = bit::fnv1a_iter(hash, uint32_t(packet.path));
  return hash != 0u ? hash : 1u;
}

uint64_t MakeMaterialCacheKey(const ShadowDrawPacket& packet) {
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, packet.material.signatureHash);
  hash = bit::fnv1a_iter(hash, uint32_t(packet.material.alphaMode));
  hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(packet.material.alphaCutoutRef));
  return hash != 0u ? hash : 1u;
}

} // namespace

void DxvkValidationBackend::configureHost(IDxvkValidationHost* host,
                                          bool unitsOnly,
                                          bool trackSubmittedIdentities) {
  m_host = host;
  m_unitsOnly = unitsOnly;
  m_trackSubmittedIdentities = trackSubmittedIdentities && host != nullptr;
}

bool DxvkValidationBackend::shouldSubmitPacket(
    const ShadowDrawPacket& packet) const {
  return m_host == nullptr || m_host->shouldSubmitDraw(packet, m_unitsOnly);
}

void DxvkValidationBackend::noteSubmittedIdentity(
    const ShadowDrawPacket& packet) {
  if (m_host == nullptr || !m_trackSubmittedIdentities)
    return;

  if (packet.renderable.jHandle != 0u)
    m_submittedHandles.insert(m_host->normalizeHandle(packet.renderable.jHandle));
  if (packet.renderable.worldObjectEntry != nullptr)
    m_submittedWorldObjectEntries.insert(packet.renderable.worldObjectEntry);
  if (packet.renderable.sceneNode != nullptr)
    m_submittedSceneNodes.insert(packet.renderable.sceneNode);
  if (packet.renderable.runtimeModelPtr != nullptr)
    m_submittedRuntimeModels.insert(packet.renderable.runtimeModelPtr);
}

void DxvkValidationBackend::beginFrame(uint64_t frameSerial) {
  m_frameSerial = frameSerial;
  m_submittedDrawCount = 0;
  if (m_trackSubmittedIdentities || !m_submittedHandles.empty())
    m_submittedHandles.clear();
  if (m_trackSubmittedIdentities || !m_submittedWorldObjectEntries.empty())
    m_submittedWorldObjectEntries.clear();
  if (m_trackSubmittedIdentities || !m_submittedSceneNodes.empty())
    m_submittedSceneNodes.clear();
  if (m_trackSubmittedIdentities || !m_submittedRuntimeModels.empty())
    m_submittedRuntimeModels.clear();
}

bool DxvkValidationBackend::ensureGeometry(const ShadowDrawPacket& packet,
                                           ShadowGeometryHandle& outHandle) {
  if (!shouldSubmitPacket(packet))
    return false;

  if (m_host != nullptr) {
    outHandle.value = 1u;
    return true;
  }

  const uint64_t key = MakeGeometryCacheKey(packet);
  auto [it, inserted] = m_geometryHandles.try_emplace(key, 0u);
  if (inserted || it->second == 0u)
    it->second = m_nextHandleValue++;
  outHandle.value = it->second;
  return outHandle.value != 0u;
}

bool DxvkValidationBackend::ensurePalette(const ShadowDrawPacket& packet,
                                          ShadowPaletteHandle& outHandle) {
  if (m_host != nullptr) {
    outHandle.value = 1u;
    return true;
  }

  if (packet.path == ShadowDrawPath::Rigid && !packet.pose.hasWorldTransform &&
      packet.pose.matrixHash == 0u && packet.pose.matrixCount == 0u) {
    outHandle.value = 1u;
    return true;
  }

  const uint64_t key = MakePaletteCacheKey(packet);
  auto [it, inserted] = m_paletteHandles.try_emplace(key, 0u);
  if (inserted || it->second == 0u)
    it->second = m_nextHandleValue++;
  outHandle.value = it->second;
  return outHandle.value != 0u;
}

bool DxvkValidationBackend::ensureMaterial(const ShadowDrawPacket& packet,
                                           ShadowMaterialHandle& outHandle) {
  if (m_host != nullptr) {
    outHandle.value = 1u;
    return true;
  }

  const uint64_t key = MakeMaterialCacheKey(packet);
  auto [it, inserted] = m_materialHandles.try_emplace(key, 0u);
  if (inserted || it->second == 0u)
    it->second = m_nextHandleValue++;
  outHandle.value = it->second;
  return outHandle.value != 0u;
}

bool DxvkValidationBackend::submitDraw(const ShadowDrawPacket& packet,
                                       const ShadowGeometryHandle& geometryHandle,
                                       const ShadowPaletteHandle& paletteHandle,
                                       const ShadowMaterialHandle& materialHandle) {
  if (geometryHandle.value == 0 || paletteHandle.value == 0 ||
      materialHandle.value == 0) {
    return false;
  }

  if (m_host != nullptr && !m_host->submitDrawPacket(packet))
    return false;

  noteSubmittedIdentity(packet);
  ++m_submittedDrawCount;
  return true;
}

void DxvkValidationBackend::endFrame() {
}

} // namespace dxvk::war3::shadow
