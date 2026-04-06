#pragma once

#include "../../d3d9_device.h"
#include <cstdint>
#include <d3d9.h>
#include <unordered_map>

namespace dxvk {
namespace war3 {
namespace reimpl {

// Instance Data Structure (Stream 1)
// Must match the vertex shader input declaration layout
struct InstanceData {
  // m4x4 World Matrix (Transposed or not? War3 uses row-major in memory
  // usually, but shader expects m4x4) We will copy exactly what is in the
  // game's matrix array.
  float worldMatrix[16]; // 64 bytes

  // Per-instance color (frequently used for team color or fading)
  // Maps to a float4 in shader
  float color[4]; // 16 bytes

  // Team Color Index (0-23) for Texture2DArray sampling
  float teamColorIndex; // 4 bytes
  float padding[3];     // 12 bytes for alignment
}; // Total: 96 bytes

class War3InstanceBuffer {
public:
  War3InstanceBuffer(D3D9DeviceEx *device);
  ~War3InstanceBuffer();

  // Initialize the buffer
  // Initialize the buffer
  HRESULT Create(uint32_t maxInstances = 65536);

  // Allocates space for 'count' instances and returns a pointer to write to
  // Returns nullptr if failed
  InstanceData *Alloc(uint32_t count, uint32_t &outBaseOffset);

  // Must be called after Alloc() once writing is done
  void Unlock();

  // Capture System
  // State Tracking for Batch Breaking
  void CaptureTextureState();
  // Returns true if textures match captured state
  bool CheckTextureState() const;
  void RestoreTextureState();
  // Handles texture mismatch by flushing current batch (minus last item) and
  // starting new one
  void ResolveTextureChange();

  // Lifecycle
  void OnLostDevice();
  void OnResetDevice();

  static void SetActive(War3InstanceBuffer *buffer);
  static War3InstanceBuffer *GetActive();
  static War3InstanceBuffer *Get(D3D9DeviceEx *device);

  // State-Aware Batching Hooks
  // Called by SetTexture hook to detect state changes within a batch
  void OnSetTexture(uint32_t stage, IDirect3DBaseTexture9 *texture);
  // Flushes the current accumulated batch (excluding the current item being
  // processed if any)
  void FlushBatch();

  void Bind(uint32_t streamIdx, uint32_t baseInstanceOffset);

  void CaptureConstants(uint32_t startReg, const float *data, uint32_t count);
  void CaptureDrawParams(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex,
                         UINT MinVertexIndex, UINT NumVertices, UINT StartIndex,
                         UINT PrimitiveCount);
  void ReplayDraw();
  void AdvanceInstance();
  bool IsFull() const;
  bool m_isFull = false;

  // Captured state to detect changes
  IDirect3DBaseTexture9 *m_capturedTextures[8] = {nullptr};

private:
  struct DrawParams {
    D3DPRIMITIVETYPE PrimitiveType;
    INT BaseVertexIndex;
    UINT MinVertexIndex;
    UINT NumVertices;
    UINT StartIndex;
    UINT PrimitiveCount;
  } m_drawParams;

  static thread_local War3InstanceBuffer *s_activeBuffer;
  static thread_local InstanceData *s_activeLockedPtr;
  uint32_t m_capturedInstanceCount = 0;
  uint32_t m_maxBatchSize = 0;

  // Resets the buffer at the start of the frame (optional, mostly for tracking)
  void OnFrameStart();

  IDirect3DVertexBuffer9 *GetBuffer() const { return m_buffer; }
  uint32_t GetStride() const { return sizeof(InstanceData); }

private:
  D3D9DeviceEx *m_device = nullptr;
  IDirect3DVertexBuffer9 *m_buffer = nullptr;

  uint32_t m_maxInstances = 0;
  uint32_t m_currentOffset = 0;
  uint32_t m_bufferSizeInBytes = 0;

  // Tracking for sub-batches
  uint32_t m_activeAllocationStart = 0; // Start offset of current Alloc
  uint32_t m_drawnCountThisAlloc =
      0; // How many instances flushed so far in this Alloc

  // Statistics
  uint32_t m_allocsThisFrame = 0;
  uint32_t m_discardsThisFrame = 0;

  // Vertex Declaration Cache
  IDirect3DVertexDeclaration9 *
  GetInstancedDeclaration(IDirect3DVertexDeclaration9 *originalDecl);
  std::unordered_map<IDirect3DVertexDeclaration9 *,
                     IDirect3DVertexDeclaration9 *>
      m_declCache;
};

} // namespace reimpl
} // namespace war3
} // namespace dxvk
