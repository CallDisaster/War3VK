#pragma once

#include "war3_shadow_backend.h"
#include "war3_shadow_runtime_contract.h"

#include "../../d3d9_include.h"
#include "../../../util/com/com_pointer.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dxvk::war3::shadow {

class NativeD3D9Backend final : public IShadowRenderBackend {
public:
  explicit NativeD3D9Backend(IDirect3DDevice9* device = nullptr)
    : m_device(device) {}

  void setDevice(IDirect3DDevice9* device);
  bool hasDevice() const { return m_device != nullptr; }
  uint64_t frameSerial() const { return m_frameSerial; }
  uint64_t submittedDrawCount() const { return m_submittedDrawCount; }
  uint64_t submittedRigidDrawCount() const { return m_submittedRigidDrawCount; }
  uint64_t submittedSkinnedDrawCount() const {
    return m_submittedSkinnedDrawCount;
  }
  uint64_t executedDrawCount() const { return m_executedDrawCount; }
  uint64_t executedRigidDrawCount() const { return m_executedRigidDrawCount; }
  uint64_t executedSkinnedDrawCount() const {
    return m_executedSkinnedDrawCount;
  }
  uint64_t executedFrameSerial() const { return m_executedFrameSerial; }
  uint64_t executeAttemptCount() const { return m_executeAttemptCount; }
  uint64_t executeSuccessCount() const { return m_executeSuccessCount; }
  uint64_t lastSuccessfulExecutedFrameSerial() const {
    return m_lastSuccessfulExecutedFrameSerial;
  }
  uint64_t lastSuccessfulExecutedDrawCount() const {
    return m_lastSuccessfulExecutedDrawCount;
  }
  uint64_t executeSkippedNoDeviceCount() const {
    return m_executeSkippedNoDeviceCount;
  }
  uint64_t executeSkippedNoDrawsCount() const {
    return m_executeSkippedNoDrawsCount;
  }
  uint64_t lastExecuteSubmittedDrawCount() const {
    return m_lastExecuteSubmittedDrawCount;
  }
  uint64_t lastExecuteFailedDrawCount() const {
    return m_lastExecuteFailedDrawCount;
  }
  uint64_t lastExecuteSubmittedRigidDrawCount() const {
    return m_lastExecuteSubmittedRigidDrawCount;
  }
  uint64_t lastExecuteSubmittedSkinnedDrawCount() const {
    return m_lastExecuteSubmittedSkinnedDrawCount;
  }
  uint64_t lastExecuteExecutedRigidDrawCount() const {
    return m_lastExecuteExecutedRigidDrawCount;
  }
  uint64_t lastExecuteExecutedSkinnedDrawCount() const {
    return m_lastExecuteExecutedSkinnedDrawCount;
  }
  size_t geometryCount() const { return m_geometryResources.size(); }
  size_t paletteCount() const { return m_paletteResources.size(); }
  size_t materialCount() const { return m_materialResources.size(); }
  void reset();
  bool executePreparedDraws();

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

public:
  struct GeometryResource {
    uint64_t cacheKey = 0;
    bool indexed = false;
    bool skinned = false;
    uint8_t explicitBlendCount = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t positionStride = 0;
    uint32_t blendStride = 0;
    ShadowPrimitiveTopology topology = ShadowPrimitiveTopology::TriangleList;
    Com<IDirect3DVertexBuffer9> positionBuffer;
    Com<IDirect3DIndexBuffer9> indexBuffer;
    Com<IDirect3DVertexBuffer9> blendBuffer;
  };

  struct PaletteResource {
    uint64_t cacheKey = 0;
    uint64_t matrixHash = 0;
    uint32_t matrixCount = 0;
    std::vector<Matrix4> matrices;
  };

  struct MaterialResource {
    uint64_t cacheKey = 0;
    ShadowMaterialSignature signature = {};
  };

  struct SubmittedDrawRecord {
    uint64_t frameSerial = 0;
    uint64_t geometryHandle = 0;
    uint64_t paletteHandle = 0;
    uint64_t materialHandle = 0;
    uint64_t modelKey = 0;
    uint32_t jHandle = 0;
    uint32_t rawcode = 0;
    bool skinned = false;
    bool indexed = false;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    ShadowPrimitiveTopology topology = ShadowPrimitiveTopology::TriangleList;
    Matrix4 worldTransform = Matrix4();
    std::vector<float> positions;
    std::vector<uint16_t> indices;
    std::vector<uint8_t> vertexGroupIndices;
    std::vector<std::array<float, 3>> explicitBlendWeights;
    std::vector<std::array<uint8_t, 4>> explicitBlendIndices;
    std::vector<Matrix4> runtimeGroupPalette;
  };

private:
  void ensureIdentityPaletteResource();

  IDirect3DDevice9* m_device = nullptr;
  uint64_t m_frameSerial = 0;
  uint64_t m_submittedDrawCount = 0;
  uint64_t m_submittedRigidDrawCount = 0;
  uint64_t m_submittedSkinnedDrawCount = 0;
  uint64_t m_executedDrawCount = 0;
  uint64_t m_executedRigidDrawCount = 0;
  uint64_t m_executedSkinnedDrawCount = 0;
  uint64_t m_executedFrameSerial = 0;
  uint64_t m_executeAttemptCount = 0;
  uint64_t m_executeSuccessCount = 0;
  uint64_t m_lastSuccessfulExecutedFrameSerial = 0;
  uint64_t m_lastSuccessfulExecutedDrawCount = 0;
  uint64_t m_executeSkippedNoDeviceCount = 0;
  uint64_t m_executeSkippedNoDrawsCount = 0;
  uint64_t m_lastExecuteSubmittedDrawCount = 0;
  uint64_t m_lastExecuteFailedDrawCount = 0;
  uint64_t m_lastExecuteSubmittedRigidDrawCount = 0;
  uint64_t m_lastExecuteSubmittedSkinnedDrawCount = 0;
  uint64_t m_lastExecuteExecutedRigidDrawCount = 0;
  uint64_t m_lastExecuteExecutedSkinnedDrawCount = 0;
  uint64_t m_nextHandleValue = 2;
  std::unordered_map<uint64_t, uint64_t> m_geometryHandleByKey;
  std::unordered_map<uint64_t, uint64_t> m_paletteHandleByKey;
  std::unordered_map<uint64_t, uint64_t> m_materialHandleByKey;
  std::unordered_map<uint64_t, GeometryResource> m_geometryResources;
  std::unordered_map<uint64_t, PaletteResource> m_paletteResources;
  std::unordered_map<uint64_t, MaterialResource> m_materialResources;
  std::vector<SubmittedDrawRecord> m_submittedDraws;
};

} // namespace dxvk::war3::shadow
