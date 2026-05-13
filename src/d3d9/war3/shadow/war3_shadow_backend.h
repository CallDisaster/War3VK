#pragma once

#include <cstdint>

namespace dxvk::war3::shadow {

enum class ShadowPrimitiveTopology : uint8_t {
  TriangleList = 0,
  TriangleStrip = 1,
  TriangleFan = 2,
  LineList = 3,
  LineStrip = 4,
};

enum class ShadowAlphaMode : uint8_t {
  Opaque = 0,
  Cutout = 1,
  AlphaBlend = 2,
};

struct ShadowGeometryHandle {
  uint64_t value = 0;
};

struct ShadowPaletteHandle {
  uint64_t value = 0;
};

struct ShadowMaterialHandle {
  uint64_t value = 0;
};

struct ShadowMaterialSignature {
  uint64_t signatureHash = 0;
  ShadowAlphaMode alphaMode = ShadowAlphaMode::Opaque;
  float alphaCutoutRef = 0.5f;
  uint32_t blendOrDrawMode = 0;
  uint32_t layerIndex = 0;
  uint32_t queueKind = 0;
  uint32_t transparentType = 0;
  bool layerContractResolved = false;
  uint32_t layerContractMeshIndex = 0;
  uint32_t layerContractLayerCount = 0;

  bool valid() const {
    return signatureHash != 0;
  }
};

struct ShadowDrawPacket;

class IShadowRenderBackend {
public:
  virtual ~IShadowRenderBackend() = default;

  virtual void beginFrame(uint64_t frameSerial) = 0;
  virtual bool ensureGeometry(const ShadowDrawPacket& packet,
                              ShadowGeometryHandle& outHandle) = 0;
  virtual bool ensurePalette(const ShadowDrawPacket& packet,
                             ShadowPaletteHandle& outHandle) = 0;
  virtual bool ensureMaterial(const ShadowDrawPacket& packet,
                              ShadowMaterialHandle& outHandle) = 0;
  virtual bool submitDraw(const ShadowDrawPacket& packet,
                          const ShadowGeometryHandle& geometryHandle,
                          const ShadowPaletteHandle& paletteHandle,
                          const ShadowMaterialHandle& materialHandle) = 0;
  virtual void endFrame() = 0;
};

} // namespace dxvk::war3::shadow
