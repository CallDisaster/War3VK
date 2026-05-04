#include "war3_shadow_backend_native_d3d9.h"

#include "war3_shadow_renderer_core.h"

#include "../../d3d9_war3_hook.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../core/war3_semantic_shadow_gate.h"
#include "../debug/war3_debug.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>

namespace dxvk::war3::shadow {

namespace {

bool NativeExecuteVerboseEnabled() {
  return dxvk::war3::internal::kNativeRendererHookVerboseLogging ||
         dxvk::war3::internal::
             IsNativeRendererHostExecuteValidationRuntimeEnabled();
}

constexpr uint64_t kIdentityPaletteHandle = 1u;

struct NativeBlendVertex {
  float weights[3];
  uint8_t indices[4];
};

uint64_t HashMatrixComponents(uint64_t hash, const Matrix4& matrix) {
  for (uint32_t row = 0u; row < 4u; ++row) {
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[row].x));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[row].y));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[row].z));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[row].w));
  }
  return hash;
}

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
  hash = bit::fnv1a_iter(hash, uint32_t(packet.path));

  if (packet.path == ShadowDrawPath::Skinned && !packet.runtimeGroupPalette.empty()) {
    hash = bit::fnv1a_iter(hash, uint32_t(packet.runtimeGroupPalette.size()));
    if (packet.pose.matrixHash != 0u) {
      hash = bit::fnv1a_iter(hash, packet.pose.matrixHash);
    } else {
      for (const auto& matrix : packet.runtimeGroupPalette)
        hash = HashMatrixComponents(hash, matrix);
    }
    return hash != 0u ? hash : 1u;
  }

  if (packet.pose.hasWorldTransform) {
    hash = HashMatrixComponents(hash, packet.pose.worldTransform);
    return hash != 0u ? hash : 1u;
  }

  if (!packet.pose.matrixPalette.empty()) {
    if (packet.pose.matrixHash != 0u)
      hash = bit::fnv1a_iter(hash, packet.pose.matrixHash);
    else
      hash = HashMatrixComponents(hash, packet.pose.matrixPalette[0]);
    hash = bit::fnv1a_iter(hash, uint32_t(packet.pose.matrixPalette.size()));
    return hash != 0u ? hash : 1u;
  }

  return kIdentityPaletteHandle;
}

uint64_t MakeMaterialCacheKey(const ShadowDrawPacket& packet) {
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, packet.material.signatureHash);
  hash = bit::fnv1a_iter(hash, uint32_t(packet.material.alphaMode));
  hash =
      bit::fnv1a_iter(hash, bit::cast<uint32_t>(packet.material.alphaCutoutRef));
  return hash != 0u ? hash : 1u;
}

template <typename T>
bool TryCopyPacketVector(const std::vector<T>* source,
                         std::vector<T>& out,
                         size_t requiredCount = 0u,
                         size_t maxCount = 1u << 20) {
  out.clear();
  if (source == nullptr)
    return requiredCount == 0u;
  if (!dxvk::war3::IsReadableRange(source, sizeof(*source)))
    return false;

  const size_t count = source->size();
  if (count < requiredCount || count > maxCount)
    return false;
  if (count == 0u)
    return true;

  const T* data = source->data();
  if (data == nullptr ||
      !dxvk::war3::IsReadableRange(data, count * sizeof(T))) {
    return false;
  }

  out.assign(data, data + count);
  return true;
}

template <typename T>
bool IsPacketVectorReadable(const std::vector<T>* source,
                            size_t requiredCount = 0u,
                            size_t maxCount = 1u << 20) {
  if (source == nullptr)
    return requiredCount == 0u;
  if (!dxvk::war3::IsReadableRange(source, sizeof(*source)))
    return false;
  const size_t count = source->size();
  if (count < requiredCount || count > maxCount)
    return false;
  if (count == 0u)
    return true;
  const T* data = source->data();
  return data != nullptr &&
         dxvk::war3::IsReadableRange(data, count * sizeof(T));
}

template <typename TBuffer>
bool UploadLockedBuffer(TBuffer* buffer, const void* data, size_t bytes) {
  if (buffer == nullptr || bytes == 0u)
    return false;

  void* dst = nullptr;
  if (FAILED(buffer->Lock(0u, UINT(bytes), &dst, 0u)) || dst == nullptr)
    return false;

  std::memcpy(dst, data, bytes);
  buffer->Unlock();
  return true;
}

bool CreateStaticVertexBuffer(IDirect3DDevice9* device,
                              const void* data,
                              size_t bytes,
                              Com<IDirect3DVertexBuffer9>& outBuffer) {
  outBuffer = nullptr;
  if (device == nullptr || data == nullptr || bytes == 0u ||
      bytes > size_t((std::numeric_limits<UINT>::max)())) {
    return false;
  }

  IDirect3DVertexBuffer9* buffer = nullptr;
  HRESULT hr =
      device->CreateVertexBuffer(UINT(bytes), D3DUSAGE_WRITEONLY, 0u,
                                 D3DPOOL_DEFAULT, &buffer, nullptr);
  if (FAILED(hr) || buffer == nullptr) {
    hr = device->CreateVertexBuffer(UINT(bytes), D3DUSAGE_WRITEONLY, 0u,
                                    D3DPOOL_MANAGED, &buffer, nullptr);
  }
  if (FAILED(hr) || buffer == nullptr)
    return false;

  const bool ok = UploadLockedBuffer(buffer, data, bytes);
  outBuffer = buffer;
  buffer->Release();
  if (!ok) {
    outBuffer = nullptr;
    return false;
  }

  return true;
}

bool CreateStaticIndexBuffer(IDirect3DDevice9* device,
                             const void* data,
                             size_t bytes,
                             Com<IDirect3DIndexBuffer9>& outBuffer) {
  outBuffer = nullptr;
  if (device == nullptr || data == nullptr || bytes == 0u ||
      bytes > size_t((std::numeric_limits<UINT>::max)())) {
    return false;
  }

  IDirect3DIndexBuffer9* buffer = nullptr;
  HRESULT hr = device->CreateIndexBuffer(UINT(bytes), D3DUSAGE_WRITEONLY,
                                         D3DFMT_INDEX16, D3DPOOL_DEFAULT,
                                         &buffer, nullptr);
  if (FAILED(hr) || buffer == nullptr) {
    hr = device->CreateIndexBuffer(UINT(bytes), D3DUSAGE_WRITEONLY,
                                   D3DFMT_INDEX16, D3DPOOL_MANAGED, &buffer,
                                   nullptr);
  }
  if (FAILED(hr) || buffer == nullptr)
    return false;

  const bool ok = UploadLockedBuffer(buffer, data, bytes);
  outBuffer = buffer;
  buffer->Release();
  if (!ok) {
    outBuffer = nullptr;
    return false;
  }

  return true;
}

uint32_t ResolveVertexCount(const ShadowDrawPacket& packet,
                            const std::vector<float>& positions) {
  const uint32_t positionVertexCount = uint32_t(positions.size() / 3u);
  if (packet.resource.vertexCount == 0u)
    return positionVertexCount;
  if (positionVertexCount == 0u)
    return packet.resource.vertexCount;
  return (std::min)(packet.resource.vertexCount, positionVertexCount);
}

Matrix4 ResolveWorldTransform(const ShadowDrawPacket& packet) {
  if (packet.pose.hasWorldTransform)
    return packet.pose.worldTransform;
  if (!packet.pose.matrixPalette.empty())
    return packet.pose.matrixPalette[0];
  return Matrix4();
}

bool ResolvePositionStream(const ShadowDrawPacket& packet,
                           std::vector<float>& outPositions) {
  if (!TryCopyPacketVector(packet.resource.positions, outPositions,
                           0u, size_t(16u) * 1024u * 1024u)) {
    return false;
  }
  if (!outPositions.empty())
    return true;

  if (packet.resource.dynamicPositionStream == nullptr ||
      packet.resource.dynamicPositionStride < 12u ||
      packet.resource.vertexCount == 0u) {
    return false;
  }

  const size_t stride = size_t(packet.resource.dynamicPositionStride);
  const size_t lastOffset =
      size_t(packet.resource.vertexCount - 1u) * stride + sizeof(float) * 3u;
  if (!dxvk::war3::IsReadableRange(packet.resource.dynamicPositionStream,
                                   lastOffset)) {
    return false;
  }

  outPositions.resize(size_t(packet.resource.vertexCount) * 3u);
  const auto* srcBase =
      reinterpret_cast<const uint8_t*>(packet.resource.dynamicPositionStream);
  for (uint32_t i = 0u; i < packet.resource.vertexCount; ++i) {
    std::memcpy(outPositions.data() + size_t(i) * 3u,
                srcBase + size_t(i) * stride, sizeof(float) * 3u);
  }
  return !outPositions.empty();
}

bool ResolveIndexStream(const ShadowDrawPacket& packet,
                        std::vector<uint16_t>& outIndices) {
  if (packet.resource.ownedDynamicIndices != nullptr &&
      !packet.resource.ownedDynamicIndices->empty()) {
    outIndices = *packet.resource.ownedDynamicIndices;
    return true;
  }

  if (packet.resource.dynamicIndexStream != nullptr &&
      packet.resource.dynamicIndexCount != 0u) {
    if (!dxvk::war3::IsReadableRange(
            packet.resource.dynamicIndexStream,
            size_t(packet.resource.dynamicIndexCount) * sizeof(uint16_t))) {
      return false;
    }

    outIndices.assign(packet.resource.dynamicIndexStream,
                      packet.resource.dynamicIndexStream +
                          packet.resource.dynamicIndexCount);
    return !outIndices.empty();
  }

  if (!TryCopyPacketVector(packet.resource.indices, outIndices,
                           0u, size_t(32u) * 1024u * 1024u)) {
    return false;
  }
  return !outIndices.empty();
}

bool ResolveBlendStream(const ShadowDrawPacket& packet,
                        uint32_t vertexCount,
                        std::vector<NativeBlendVertex>& outBlendVertices,
                        std::vector<std::array<uint8_t, 4>>& outBlendIndices,
                        uint32_t& outBlendStride,
                        uint8_t& outExplicitBlendCount) {
  outBlendVertices.clear();
  outBlendIndices.clear();
  outBlendStride = 0u;
  outExplicitBlendCount = 0u;

  if (packet.path != ShadowDrawPath::Skinned)
    return true;

  if (!IsPacketVectorReadable(packet.resource.vertexBlendWeights,
                              0u, size_t(16u) * 1024u * 1024u) ||
      !IsPacketVectorReadable(packet.resource.vertexBlendIndices,
                              0u, size_t(16u) * 1024u * 1024u) ||
      !IsPacketVectorReadable(packet.resource.vertexGroupIndices,
                              0u, size_t(16u) * 1024u * 1024u)) {
    return false;
  }

  const auto& packetWeights = packet.resource.vertexBlendWeightVec();
  const auto& packetIndices = packet.resource.vertexBlendIndexVec();
  const auto& packetGroups = packet.resource.vertexGroupIndexVec();
  const bool hasExplicitBlendContract =
      packet.resource.explicitBlendCount != 0u &&
      packetWeights.size() >= size_t(vertexCount) &&
      packetIndices.size() >= size_t(vertexCount);

  if (hasExplicitBlendContract) {
    outExplicitBlendCount = packet.resource.explicitBlendCount;
    outBlendStride = sizeof(NativeBlendVertex);
    outBlendVertices.resize(vertexCount);
    for (uint32_t i = 0u; i < vertexCount; ++i) {
      outBlendVertices[i].weights[0] = packetWeights[i][0];
      outBlendVertices[i].weights[1] = packetWeights[i][1];
      outBlendVertices[i].weights[2] = packetWeights[i][2];
      outBlendVertices[i].indices[0] = packetIndices[i][0];
      outBlendVertices[i].indices[1] = packetIndices[i][1];
      outBlendVertices[i].indices[2] = packetIndices[i][2];
      outBlendVertices[i].indices[3] = packetIndices[i][3];
    }
    return true;
  }

  if (packetGroups.size() < size_t(vertexCount))
    return false;

  outBlendStride = sizeof(std::array<uint8_t, 4>);
  outBlendIndices.resize(vertexCount);
  for (uint32_t i = 0u; i < vertexCount; ++i)
    outBlendIndices[i] = {packetGroups[i], 0u, 0u, 0u};
  return true;
}

Vector4 TransformPoint(const Matrix4& matrix, float x, float y, float z) {
  return matrix * Vector4(x, y, z, 1.0f);
}

D3DMATRIX ToD3dMatrix(const Matrix4& matrix) {
  D3DMATRIX result = {};
  std::memcpy(&result, &matrix, sizeof(result));
  return result;
}

D3DPRIMITIVETYPE ResolveD3dPrimitiveType(ShadowPrimitiveTopology topology) {
  switch (topology) {
  case ShadowPrimitiveTopology::TriangleStrip:
    return D3DPT_TRIANGLESTRIP;
  case ShadowPrimitiveTopology::TriangleFan:
    return D3DPT_TRIANGLEFAN;
  case ShadowPrimitiveTopology::LineList:
    return D3DPT_LINELIST;
  case ShadowPrimitiveTopology::LineStrip:
    return D3DPT_LINESTRIP;
  case ShadowPrimitiveTopology::TriangleList:
  default:
    return D3DPT_TRIANGLELIST;
  }
}

UINT ResolvePrimitiveCount(ShadowPrimitiveTopology topology,
                           bool indexed,
                           uint32_t vertexCount,
                           uint32_t indexCount) {
  const uint32_t elementCount = indexed ? indexCount : vertexCount;
  switch (topology) {
  case ShadowPrimitiveTopology::TriangleStrip:
  case ShadowPrimitiveTopology::TriangleFan:
    return elementCount >= 3u ? UINT(elementCount - 2u) : 0u;
  case ShadowPrimitiveTopology::LineList:
    return UINT(elementCount / 2u);
  case ShadowPrimitiveTopology::LineStrip:
    return elementCount >= 2u ? UINT(elementCount - 1u) : 0u;
  case ShadowPrimitiveTopology::TriangleList:
  default:
    return UINT(elementCount / 3u);
  }
}

bool CaptureStateBlock(IDirect3DDevice9* device,
                       Com<IDirect3DStateBlock9>& outStateBlock) {
  outStateBlock = nullptr;
  if (device == nullptr)
    return false;

  IDirect3DStateBlock9* stateBlock = nullptr;
  if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) ||
      stateBlock == nullptr) {
    return false;
  }

  if (FAILED(stateBlock->Capture())) {
    stateBlock->Release();
    return false;
  }

  outStateBlock = stateBlock;
  stateBlock->Release();
  return true;
}

bool PrepareSkinnedWorldPositions(const NativeD3D9Backend::SubmittedDrawRecord& record,
                                  std::vector<float>& outPositions) {
  outPositions.clear();
  if (record.positions.empty() || record.runtimeGroupPalette.empty())
    return false;

  const size_t vertexCount = record.positions.size() / 3u;
  if (vertexCount == 0u)
    return false;

  outPositions.resize(record.positions.size());
  const auto transformSingleGroup = [&](size_t vertexIndex,
                                        uint8_t groupIndex) {
    if (groupIndex >= record.runtimeGroupPalette.size())
      return false;

    const size_t base = vertexIndex * 3u;
    const Vector4 transformed =
        TransformPoint(record.runtimeGroupPalette[groupIndex],
                       record.positions[base + 0u], record.positions[base + 1u],
                       record.positions[base + 2u]);
    outPositions[base + 0u] = transformed.x;
    outPositions[base + 1u] = transformed.y;
    outPositions[base + 2u] = transformed.z;
    return true;
  };

  const bool hasExplicitBlend =
      record.explicitBlendWeights.size() >= vertexCount &&
      record.explicitBlendIndices.size() >= vertexCount;
  if (hasExplicitBlend) {
    for (size_t vertexIndex = 0u; vertexIndex < vertexCount; ++vertexIndex) {
      const size_t base = vertexIndex * 3u;
      const Vector4 source(record.positions[base + 0u], record.positions[base + 1u],
                           record.positions[base + 2u], 1.0f);
      const auto& weights = record.explicitBlendWeights[vertexIndex];
      const auto& indices = record.explicitBlendIndices[vertexIndex];
      const float w0 = weights[0];
      const float w1 = weights[1];
      const float w2 = weights[2];
      const float w3 = (std::max)(0.0f, 1.0f - (w0 + w1 + w2));

      Vector4 blended(0.0f);
      float appliedWeight = 0.0f;
      const auto applyWeightedMatrix = [&](uint8_t paletteIndex, float weight) {
        if (weight <= 0.0f || paletteIndex >= record.runtimeGroupPalette.size())
          return;
        blended += weight * (record.runtimeGroupPalette[paletteIndex] * source);
        appliedWeight += weight;
      };

      applyWeightedMatrix(indices[0], w0);
      applyWeightedMatrix(indices[1], w1);
      applyWeightedMatrix(indices[2], w2);
      applyWeightedMatrix(indices[3], w3);

      if (appliedWeight <= 0.0f) {
        if (!transformSingleGroup(vertexIndex, indices[0]))
          return false;
        continue;
      }

      outPositions[base + 0u] = blended.x;
      outPositions[base + 1u] = blended.y;
      outPositions[base + 2u] = blended.z;
    }
    return true;
  }

  if (record.vertexGroupIndices.size() < vertexCount)
    return false;

  for (size_t vertexIndex = 0u; vertexIndex < vertexCount; ++vertexIndex) {
    if (!transformSingleGroup(vertexIndex, record.vertexGroupIndices[vertexIndex]))
      return false;
  }
  return true;
}

bool DrawPreparedRigidRecord(IDirect3DDevice9* device,
                             const NativeD3D9Backend::GeometryResource& geometry,
                             const NativeD3D9Backend::SubmittedDrawRecord& record) {
  if (device == nullptr || geometry.positionBuffer == nullptr) {
    static std::atomic<bool> s_logged{false};
    if (NativeExecuteVerboseEnabled() &&
        !s_logged.exchange(true, std::memory_order_relaxed)) {
      WAR3_LOG_WARN(
          "[NativeShadow] rigid execute skipped: device=%p positionBuffer=%p\n",
          device, geometry.positionBuffer.ptr());
    }
    return false;
  }

  const UINT primitiveCount = ResolvePrimitiveCount(
      geometry.topology, geometry.indexed, geometry.vertexCount, geometry.indexCount);
  if (primitiveCount == 0u) {
    static std::atomic<bool> s_logged{false};
    if (NativeExecuteVerboseEnabled() &&
        !s_logged.exchange(true, std::memory_order_relaxed)) {
      WAR3_LOG_WARN(
          "[NativeShadow] rigid execute skipped: primitiveCount=0 topology=%u "
          "indexed=%d vertexCount=%u indexCount=%u\n",
          uint32_t(geometry.topology), geometry.indexed ? 1 : 0,
          geometry.vertexCount, geometry.indexCount);
    }
    return false;
  }

  device->SetVertexShader(nullptr);
  device->SetPixelShader(nullptr);
  device->SetTexture(0, nullptr);
  device->SetRenderState(D3DRS_LIGHTING, FALSE);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
  device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
  device->SetFVF(D3DFVF_XYZ);

  const D3DMATRIX world = ToD3dMatrix(record.worldTransform);
  device->SetTransform(D3DTS_WORLD, &world);
  device->SetStreamSource(0, geometry.positionBuffer.ptr(), 0u,
                          geometry.positionStride);

  bool ok = false;
  if (geometry.indexed) {
    if (geometry.indexBuffer == nullptr) {
      static std::atomic<bool> s_logged{false};
      if (NativeExecuteVerboseEnabled() &&
          !s_logged.exchange(true, std::memory_order_relaxed)) {
        WAR3_LOG_WARN(
            "[NativeShadow] rigid execute skipped: missing index buffer "
            "vertexCount=%u indexCount=%u\n",
            geometry.vertexCount, geometry.indexCount);
      }
      return false;
    }
    device->SetIndices(geometry.indexBuffer.ptr());
  }

  const bool previousShadowPass = dxvk::War3Hook::IsInShadowPass();
  dxvk::War3Hook::SetShadowPass(true);

  HRESULT hr = D3D_OK;
  if (geometry.indexed) {
    hr = device->DrawIndexedPrimitive(ResolveD3dPrimitiveType(geometry.topology),
                                      0, 0, geometry.vertexCount, 0,
                                      primitiveCount);
    ok = SUCCEEDED(hr);
  } else {
    hr = device->DrawPrimitive(ResolveD3dPrimitiveType(geometry.topology), 0u,
                               primitiveCount);
    ok = SUCCEEDED(hr);
  }

  dxvk::War3Hook::SetShadowPass(previousShadowPass);
  if (!ok) {
    static std::atomic<bool> s_logged{false};
    if (NativeExecuteVerboseEnabled() &&
        !s_logged.exchange(true, std::memory_order_relaxed)) {
      WAR3_LOG_WARN(
          "[NativeShadow] rigid draw failed hr=0x%08X topology=%u indexed=%d "
          "primitiveCount=%u vertexCount=%u indexCount=%u\n",
          unsigned(hr), uint32_t(geometry.topology), geometry.indexed ? 1 : 0,
          primitiveCount, geometry.vertexCount, geometry.indexCount);
    }
  }
  return ok;
}

bool DrawPreparedSkinnedRecord(IDirect3DDevice9* device,
                               const NativeD3D9Backend::SubmittedDrawRecord& record) {
  if (device == nullptr) {
    static std::atomic<bool> s_logged{false};
    if (NativeExecuteVerboseEnabled() &&
        !s_logged.exchange(true, std::memory_order_relaxed)) {
      WAR3_LOG_WARN("[NativeShadow] skinned execute skipped: device is null\n");
    }
    return false;
  }

  std::vector<float> skinnedPositions;
  if (!PrepareSkinnedWorldPositions(record, skinnedPositions)) {
    static std::atomic<bool> s_logged{false};
    if (NativeExecuteVerboseEnabled() &&
        !s_logged.exchange(true, std::memory_order_relaxed)) {
      WAR3_LOG_WARN(
          "[NativeShadow] skinned prepare failed: positions=%zu groups=%zu "
          "explicitWeights=%zu explicitIndices=%zu palette=%zu\n",
          record.positions.size() / 3u, record.vertexGroupIndices.size(),
          record.explicitBlendWeights.size(),
          record.explicitBlendIndices.size(),
          record.runtimeGroupPalette.size());
    }
    return false;
  }

  const uint32_t vertexCount = uint32_t(skinnedPositions.size() / 3u);
  const bool indexed = !record.indices.empty();
  const uint32_t indexCount =
      indexed ? uint32_t(record.indices.size()) : 0u;
  const UINT primitiveCount = ResolvePrimitiveCount(record.topology, indexed,
                                                    vertexCount, indexCount);
  if (primitiveCount == 0u) {
    static std::atomic<bool> s_logged{false};
    if (NativeExecuteVerboseEnabled() &&
        !s_logged.exchange(true, std::memory_order_relaxed)) {
      WAR3_LOG_WARN(
          "[NativeShadow] skinned execute skipped: primitiveCount=0 "
          "topology=%u indexed=%d vertexCount=%u indexCount=%u\n",
          uint32_t(record.topology), indexed ? 1 : 0, vertexCount, indexCount);
    }
    return false;
  }

  device->SetVertexShader(nullptr);
  device->SetPixelShader(nullptr);
  device->SetTexture(0, nullptr);
  device->SetRenderState(D3DRS_LIGHTING, FALSE);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
  device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
  device->SetFVF(D3DFVF_XYZ);

  const Matrix4 identityMatrix = Matrix4();
  const D3DMATRIX identity = ToD3dMatrix(identityMatrix);
  device->SetTransform(D3DTS_WORLD, &identity);

  const bool previousShadowPass = dxvk::War3Hook::IsInShadowPass();
  dxvk::War3Hook::SetShadowPass(true);

  bool ok = false;
  HRESULT hr = D3D_OK;
  if (indexed) {
    hr = device->DrawIndexedPrimitiveUP(
        ResolveD3dPrimitiveType(record.topology), 0, vertexCount, primitiveCount,
        record.indices.data(), D3DFMT_INDEX16, skinnedPositions.data(),
        UINT(sizeof(float) * 3u));
    ok = SUCCEEDED(hr);
  } else {
    hr = device->DrawPrimitiveUP(ResolveD3dPrimitiveType(record.topology),
                                 primitiveCount, skinnedPositions.data(),
                                 UINT(sizeof(float) * 3u));
    ok = SUCCEEDED(hr);
  }

  dxvk::War3Hook::SetShadowPass(previousShadowPass);
  if (!ok) {
    static std::atomic<bool> s_logged{false};
    if (NativeExecuteVerboseEnabled() &&
        !s_logged.exchange(true, std::memory_order_relaxed)) {
      WAR3_LOG_WARN(
          "[NativeShadow] skinned draw failed hr=0x%08X topology=%u indexed=%d "
          "primitiveCount=%u vertexCount=%u indexCount=%u\n",
          unsigned(hr), uint32_t(record.topology), indexed ? 1 : 0,
          primitiveCount, vertexCount, indexCount);
    }
  }
  return ok;
}

} // namespace

void NativeD3D9Backend::setDevice(IDirect3DDevice9* device) {
  if (m_device == device)
    return;

  m_device = device;
  reset();
}

void NativeD3D9Backend::reset() {
  m_frameSerial = 0u;
  m_submittedDrawCount = 0u;
  m_submittedRigidDrawCount = 0u;
  m_submittedSkinnedDrawCount = 0u;
  m_executedDrawCount = 0u;
  m_executedRigidDrawCount = 0u;
  m_executedSkinnedDrawCount = 0u;
  m_executedFrameSerial = 0u;
  m_executeAttemptCount = 0u;
  m_executeSuccessCount = 0u;
  m_lastSuccessfulExecutedFrameSerial = 0u;
  m_lastSuccessfulExecutedDrawCount = 0u;
  m_executeSkippedNoDeviceCount = 0u;
  m_executeSkippedNoDrawsCount = 0u;
  m_lastExecuteSubmittedDrawCount = 0u;
  m_lastExecuteFailedDrawCount = 0u;
  m_lastExecuteSubmittedRigidDrawCount = 0u;
  m_lastExecuteSubmittedSkinnedDrawCount = 0u;
  m_lastExecuteExecutedRigidDrawCount = 0u;
  m_lastExecuteExecutedSkinnedDrawCount = 0u;
  m_nextHandleValue = 2u;
  m_geometryHandleByKey.clear();
  m_paletteHandleByKey.clear();
  m_materialHandleByKey.clear();
  m_geometryResources.clear();
  m_paletteResources.clear();
  m_materialResources.clear();
  m_submittedDraws.clear();
  ensureIdentityPaletteResource();
}

void NativeD3D9Backend::ensureIdentityPaletteResource() {
  if (m_paletteResources.find(kIdentityPaletteHandle) != m_paletteResources.end())
    return;

  PaletteResource identity = {};
  identity.cacheKey = kIdentityPaletteHandle;
  identity.matrixHash = 0u;
  identity.matrixCount = 0u;
  m_paletteHandleByKey.emplace(kIdentityPaletteHandle, kIdentityPaletteHandle);
  m_paletteResources.emplace(kIdentityPaletteHandle, std::move(identity));
}

void NativeD3D9Backend::beginFrame(uint64_t frameSerial) {
  ensureIdentityPaletteResource();
  m_frameSerial = frameSerial;
  m_submittedDrawCount = 0u;
  m_submittedRigidDrawCount = 0u;
  m_submittedSkinnedDrawCount = 0u;
  m_executedDrawCount = 0u;
  m_executedRigidDrawCount = 0u;
  m_executedSkinnedDrawCount = 0u;
  m_executedFrameSerial = 0u;
  m_submittedDraws.clear();
}

bool NativeD3D9Backend::ensureGeometry(const ShadowDrawPacket& packet,
                                       ShadowGeometryHandle& outHandle) {
  outHandle.value = 0u;
  if (m_device == nullptr)
    return false;

  const uint64_t key = MakeGeometryCacheKey(packet);
  if (const auto it = m_geometryHandleByKey.find(key);
      it != m_geometryHandleByKey.end()) {
    outHandle.value = it->second;
    return true;
  }

  std::vector<float> positions;
  if (!ResolvePositionStream(packet, positions))
    return false;

  const uint32_t vertexCount = ResolveVertexCount(packet, positions);
  if (vertexCount == 0u || positions.size() < size_t(vertexCount) * 3u)
    return false;

  std::vector<uint16_t> indices;
  const bool indexed = ResolveIndexStream(packet, indices);
  if (indexed && indices.empty())
    return false;

  std::vector<NativeBlendVertex> blendVertices;
  std::vector<std::array<uint8_t, 4>> blendIndices;
  uint32_t blendStride = 0u;
  uint8_t explicitBlendCount = 0u;
  if (!ResolveBlendStream(packet, vertexCount, blendVertices, blendIndices,
                          blendStride, explicitBlendCount)) {
    return false;
  }

  GeometryResource resource = {};
  resource.cacheKey = key;
  resource.indexed = indexed;
  resource.skinned = packet.path == ShadowDrawPath::Skinned;
  resource.explicitBlendCount = explicitBlendCount;
  resource.vertexCount = vertexCount;
  resource.indexCount = indexed ? uint32_t(indices.size()) : 0u;
  resource.positionStride = sizeof(float) * 3u;
  resource.blendStride = blendStride;
  resource.topology = packet.resource.topology;

  if (!CreateStaticVertexBuffer(m_device, positions.data(),
                                positions.size() * sizeof(float),
                                resource.positionBuffer)) {
    return false;
  }

  if (indexed &&
      !CreateStaticIndexBuffer(m_device, indices.data(),
                               indices.size() * sizeof(uint16_t),
                               resource.indexBuffer)) {
    return false;
  }

  if (!blendVertices.empty() &&
      !CreateStaticVertexBuffer(m_device, blendVertices.data(),
                                blendVertices.size() * sizeof(blendVertices[0]),
                                resource.blendBuffer)) {
    return false;
  }

  if (!blendIndices.empty() &&
      !CreateStaticVertexBuffer(m_device, blendIndices.data(),
                                blendIndices.size() * sizeof(blendIndices[0]),
                                resource.blendBuffer)) {
    return false;
  }

  const uint64_t handleValue = m_nextHandleValue++;
  m_geometryHandleByKey.emplace(key, handleValue);
  m_geometryResources.emplace(handleValue, std::move(resource));
  outHandle.value = handleValue;
  return true;
}

bool NativeD3D9Backend::ensurePalette(const ShadowDrawPacket& packet,
                                      ShadowPaletteHandle& outHandle) {
  outHandle.value = 0u;
  ensureIdentityPaletteResource();

  const uint64_t key = MakePaletteCacheKey(packet);
  if (key == kIdentityPaletteHandle) {
    outHandle.value = kIdentityPaletteHandle;
    return true;
  }

  if (const auto it = m_paletteHandleByKey.find(key);
      it != m_paletteHandleByKey.end()) {
    outHandle.value = it->second;
    return true;
  }

  PaletteResource resource = {};
  resource.cacheKey = key;
  resource.matrixHash = packet.pose.matrixHash;

  if (packet.path == ShadowDrawPath::Skinned && !packet.runtimeGroupPalette.empty()) {
    resource.matrices = packet.runtimeGroupPalette;
  } else if (packet.pose.hasWorldTransform) {
    resource.matrices.push_back(packet.pose.worldTransform);
  } else if (!packet.pose.matrixPalette.empty()) {
    resource.matrices.push_back(packet.pose.matrixPalette[0]);
  }

  if (resource.matrices.empty()) {
    outHandle.value = kIdentityPaletteHandle;
    return true;
  }

  resource.matrixCount = uint32_t(resource.matrices.size());
  const uint64_t handleValue = m_nextHandleValue++;
  m_paletteHandleByKey.emplace(key, handleValue);
  m_paletteResources.emplace(handleValue, std::move(resource));
  outHandle.value = handleValue;
  return true;
}

bool NativeD3D9Backend::ensureMaterial(const ShadowDrawPacket& packet,
                                       ShadowMaterialHandle& outHandle) {
  outHandle.value = 0u;
  const uint64_t key = MakeMaterialCacheKey(packet);
  if (const auto it = m_materialHandleByKey.find(key);
      it != m_materialHandleByKey.end()) {
    outHandle.value = it->second;
    return true;
  }

  MaterialResource resource = {};
  resource.cacheKey = key;
  resource.signature = packet.material;

  const uint64_t handleValue = m_nextHandleValue++;
  m_materialHandleByKey.emplace(key, handleValue);
  m_materialResources.emplace(handleValue, std::move(resource));
  outHandle.value = handleValue;
  return true;
}

bool NativeD3D9Backend::submitDraw(const ShadowDrawPacket& packet,
                                   const ShadowGeometryHandle& geometryHandle,
                                   const ShadowPaletteHandle& paletteHandle,
                                   const ShadowMaterialHandle& materialHandle) {
  if (m_device == nullptr || geometryHandle.value == 0u ||
      paletteHandle.value == 0u || materialHandle.value == 0u) {
    return false;
  }

  const auto geometryIt = m_geometryResources.find(geometryHandle.value);
  const auto paletteIt = m_paletteResources.find(paletteHandle.value);
  const auto materialIt = m_materialResources.find(materialHandle.value);
  if (geometryIt == m_geometryResources.end() ||
      paletteIt == m_paletteResources.end() ||
      materialIt == m_materialResources.end()) {
    return false;
  }

  // Late-inject/native execution will bind and draw these uploaded resources in
  // a later step. This backend now owns real native D3D9 resources and records
  // backend-agnostic submissions instead of acting as a handle-only stub.
  SubmittedDrawRecord record = {};
  record.frameSerial = m_frameSerial;
  record.geometryHandle = geometryHandle.value;
  record.paletteHandle = paletteHandle.value;
  record.materialHandle = materialHandle.value;
  record.modelKey =
      packet.renderable.modelKey != 0u ? packet.renderable.modelKey
                                       : packet.resource.modelKey;
  record.jHandle = packet.renderable.jHandle;
  record.rawcode = packet.renderable.rawcode;
  record.skinned = packet.path == ShadowDrawPath::Skinned;
  record.indexed = geometryIt->second.indexed;
  record.vertexCount = geometryIt->second.vertexCount;
  record.indexCount = geometryIt->second.indexCount;
  record.topology = geometryIt->second.topology;
  record.worldTransform = ResolveWorldTransform(packet);
  ResolvePositionStream(packet, record.positions);
  ResolveIndexStream(packet, record.indices);
  if (packet.path == ShadowDrawPath::Skinned) {
    if (!TryCopyPacketVector(packet.resource.vertexGroupIndices,
                             record.vertexGroupIndices,
                             0u, size_t(16u) * 1024u * 1024u) ||
        !TryCopyPacketVector(packet.resource.vertexBlendWeights,
                             record.explicitBlendWeights,
                             0u, size_t(16u) * 1024u * 1024u) ||
        !TryCopyPacketVector(packet.resource.vertexBlendIndices,
                             record.explicitBlendIndices,
                             0u, size_t(16u) * 1024u * 1024u)) {
      return false;
    }
    record.runtimeGroupPalette = packet.runtimeGroupPalette;
  }

  m_submittedDraws.emplace_back(std::move(record));
  ++m_submittedDrawCount;
  if (packet.path == ShadowDrawPath::Skinned)
    ++m_submittedSkinnedDrawCount;
  else
    ++m_submittedRigidDrawCount;
  return true;
}

void NativeD3D9Backend::endFrame() {
}

bool NativeD3D9Backend::executePreparedDraws() {
  ++m_executeAttemptCount;
  m_lastExecuteSubmittedDrawCount = m_submittedDraws.size();
  m_lastExecuteSubmittedRigidDrawCount = 0u;
  m_lastExecuteSubmittedSkinnedDrawCount = 0u;
  for (const auto& record : m_submittedDraws) {
    if (record.skinned)
      ++m_lastExecuteSubmittedSkinnedDrawCount;
    else
      ++m_lastExecuteSubmittedRigidDrawCount;
  }
  m_lastExecuteFailedDrawCount = 0u;
  m_lastExecuteExecutedRigidDrawCount = 0u;
  m_lastExecuteExecutedSkinnedDrawCount = 0u;
  m_executedDrawCount = 0u;
  m_executedRigidDrawCount = 0u;
  m_executedSkinnedDrawCount = 0u;
  m_executedFrameSerial = 0u;

  if (m_device == nullptr) {
    ++m_executeSkippedNoDeviceCount;
    return false;
  }

  if (m_submittedDraws.empty()) {
    ++m_executeSkippedNoDrawsCount;
    return false;
  }

  Com<IDirect3DStateBlock9> stateBlock;
  CaptureStateBlock(m_device, stateBlock);

  uint64_t executedDraws = 0u;
  uint64_t executedRigidDraws = 0u;
  uint64_t executedSkinnedDraws = 0u;
  for (const auto& record : m_submittedDraws) {
    bool executed = false;
    const auto geometryIt = m_geometryResources.find(record.geometryHandle);
    if (!record.skinned && geometryIt != m_geometryResources.end()) {
      executed = DrawPreparedRigidRecord(m_device, geometryIt->second, record);
    } else if (record.skinned) {
      executed = DrawPreparedSkinnedRecord(m_device, record);
    }

    if (executed) {
      ++executedDraws;
      if (record.skinned)
        ++executedSkinnedDraws;
      else
        ++executedRigidDraws;
    }
  }
  if (stateBlock != nullptr)
    stateBlock->Apply();

  m_executedDrawCount = executedDraws;
  m_executedRigidDrawCount = executedRigidDraws;
  m_executedSkinnedDrawCount = executedSkinnedDraws;
  m_lastExecuteExecutedRigidDrawCount = executedRigidDraws;
  m_lastExecuteExecutedSkinnedDrawCount = executedSkinnedDraws;
  m_executedFrameSerial = executedDraws != 0u ? m_frameSerial : 0u;
  m_lastExecuteFailedDrawCount =
      m_submittedDraws.size() >= executedDraws
          ? uint64_t(m_submittedDraws.size()) - executedDraws
          : 0u;
  if (executedDraws != 0u)
  {
    ++m_executeSuccessCount;
    m_lastSuccessfulExecutedFrameSerial = m_frameSerial;
    m_lastSuccessfulExecutedDrawCount = executedDraws;
  }
  if (NativeExecuteVerboseEnabled() && executedDraws == 0u) {
    static std::atomic<uint64_t> s_lastLoggedFrame{0u};
    const uint64_t previous =
        s_lastLoggedFrame.exchange(m_frameSerial, std::memory_order_relaxed);
    if (previous != m_frameSerial) {
      WAR3_LOG_WARN(
          "[NativeShadow] executePreparedDraws produced 0 draws frame=%llu "
          "submitted=%zu geometry=%zu palette=%zu material=%zu\n",
          static_cast<unsigned long long>(m_frameSerial),
          m_submittedDraws.size(), m_geometryResources.size(),
          m_paletteResources.size(), m_materialResources.size());
    }
  }
  return executedDraws != 0u;
}

} // namespace dxvk::war3::shadow
