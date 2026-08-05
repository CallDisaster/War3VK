#pragma once

#include "war3_shadow_backend.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace dxvk::war3::shadow {

class IDxvkValidationHost {
public:
  virtual ~IDxvkValidationHost() = default;

  virtual bool shouldSubmitDraw(const ShadowDrawPacket& packet,
                                bool unitsOnly) const = 0;
  virtual bool submitDrawPacket(const ShadowDrawPacket& packet) = 0;
  virtual uint32_t normalizeHandle(uint32_t handle) const = 0;
};

class DxvkValidationBackend final : public IShadowRenderBackend {
public:
  void configureHost(IDxvkValidationHost* host, bool unitsOnly,
                     bool trackSubmittedIdentities = true);
  void beginFrame(uint64_t frameSerial) override;
  bool ensureGeometry(const ShadowDrawPacket& packet,
                      ShadowGeometryHandle& outHandle) override;
  bool ensurePalette(const ShadowDrawPacket& packet,
                     ShadowPaletteHandle& outHandle) override;
  bool ensureMaterial(const ShadowDrawPacket& packet,
                      ShadowMaterialHandle& outHandle) override;
  bool submitDraw(const ShadowDrawPacket& packet,
                  const ShadowGeometryHandle& geometryHandle,
                  const ShadowPaletteHandle& paletteHandle,
                  const ShadowMaterialHandle& materialHandle) override;
  void endFrame() override;

  uint64_t frameSerial() const { return m_frameSerial; }
  uint64_t submittedDrawCount() const { return m_submittedDrawCount; }
  const std::unordered_set<uint32_t>& submittedHandles() const {
    return m_submittedHandles;
  }
  const std::unordered_set<void*>& submittedWorldObjectEntries() const {
    return m_submittedWorldObjectEntries;
  }
  const std::unordered_set<void*>& submittedSceneNodes() const {
    return m_submittedSceneNodes;
  }
  const std::unordered_set<void*>& submittedRuntimeModels() const {
    return m_submittedRuntimeModels;
  }

private:
  bool shouldSubmitPacket(const ShadowDrawPacket& packet) const;
  void noteSubmittedIdentity(const ShadowDrawPacket& packet);

  IDxvkValidationHost* m_host = nullptr;
  bool m_unitsOnly = false;
  bool m_trackSubmittedIdentities = true;
  uint64_t m_frameSerial = 0;
  uint64_t m_submittedDrawCount = 0;
  uint64_t m_nextHandleValue = 1;
  std::unordered_map<uint64_t, uint64_t> m_geometryHandles;
  std::unordered_map<uint64_t, uint64_t> m_paletteHandles;
  std::unordered_map<uint64_t, uint64_t> m_materialHandles;
  std::unordered_set<uint32_t> m_submittedHandles;
  std::unordered_set<void*> m_submittedWorldObjectEntries;
  std::unordered_set<void*> m_submittedSceneNodes;
  std::unordered_set<void*> m_submittedRuntimeModels;
};

} // namespace dxvk::war3::shadow
