#include "war3_instance_buffer.h"
#include "../../d3d9_shader.h"
#include "../../d3d9_texture.h"
#include "../../d3d9_war3_debug.h"
#include "war3_render_types.h"

namespace dxvk {
namespace war3 {
namespace reimpl {

thread_local War3InstanceBuffer *War3InstanceBuffer::s_activeBuffer = nullptr;

War3InstanceBuffer *War3InstanceBuffer::Get(D3D9DeviceEx *device) {
  return device ? device->GetWar3InstanceBuffer() : nullptr;
}

void War3InstanceBuffer::SetActive(War3InstanceBuffer *buffer) {
  s_activeBuffer = buffer;
}

War3InstanceBuffer *War3InstanceBuffer::GetActive() { return s_activeBuffer; }

War3InstanceBuffer *War3InstanceBuffer::GetActive(D3D9DeviceEx *device) {
  return s_activeBuffer && s_activeBuffer->m_device == device ? s_activeBuffer : nullptr;
}

void War3InstanceBuffer::Bind(uint32_t streamIdx, uint32_t baseInstanceOffset) {
  if (m_buffer != nullptr && m_device) {
    // The per-device owner retains storage without retaining the device.
    uint32_t offsetInBytes = baseInstanceOffset * sizeof(InstanceData);
    m_device->SetStreamSource(streamIdx, m_buffer.ptr(), offsetInBytes,
                              sizeof(InstanceData));
  }
}

void War3InstanceBuffer::CaptureConstants(uint32_t startReg, const float *data,
                                          uint32_t count) {
  if (!m_lockedPtr || m_capturedInstanceCount >= m_maxBatchSize)
    return;
  // Map worldMatrix (c14-c17) -> worldMatrix[16]
  // Map color (c11) -> color[4]
  InstanceData &dest = m_lockedPtr[m_capturedInstanceCount];

  for (uint32_t i = 0; i < count; i++) {
    uint32_t reg = startReg + i;
    if (reg >= 14 && reg <= 17) {
      uint32_t offset = (reg - 14) * 4;
      memcpy(&dest.worldMatrix[offset], &data[i * 4], 4 * sizeof(float));
    } else if (reg == 11) {
      memcpy(dest.color, &data[i * 4], 4 * sizeof(float));
    }
  }
}

void War3InstanceBuffer::CaptureTextureState() {
  if (!m_device)
    return;
  for (uint32_t i = 0; i < 2;
       i++) { // Only track first 2 stages? Commonly used.
    Com<IDirect3DBaseTexture9> texture;
    m_device->GetTexture(i, &texture);
    // Retain storage before releasing either the old private or temporary
    // public owner; this is also safe when the texture did not change.
    TextureRefPrivate(texture.ptr(), true);
    TextureRefPrivate(m_capturedTextures[i], false);
    m_capturedTextures[i] = texture.ptr();
  }
}

bool War3InstanceBuffer::CheckTextureState() const {
  if (!m_device)
    return true;
  for (uint32_t i = 0; i < 2; i++) {
    IDirect3DBaseTexture9 *current = nullptr;
    m_device->GetTexture(i, &current);

    bool match = (current == m_capturedTextures[i]);

    if (current)
      current->Release(); // GetTexture AddsRef

    if (!match)
      return false;
  }
  return true;
}

void War3InstanceBuffer::RestoreTextureState() {
  if (!m_device)
    return;
  for (uint32_t i = 0; i < 2; i++) {
    m_device->SetTexture(i, m_capturedTextures[i]);
  }
}

void War3InstanceBuffer::OnLostDevice() {
  if (s_activeBuffer == this)
    s_activeBuffer = nullptr;
  if (m_buffer != nullptr && m_lockedPtr)
    m_buffer->Unlock();
  m_lockedPtr = nullptr;
  m_buffer = nullptr;
  m_currentOffset = m_activeAllocationStart = m_drawnCountThisAlloc = 0;
  m_capturedInstanceCount = m_maxBatchSize = 0;
  m_isFull = false;
  m_declCache.clear();
  for (auto &tex : m_capturedTextures) {
    TextureRefPrivate(tex, false);
    tex = nullptr;
  }
}

void War3InstanceBuffer::OnResetDevice() {
  if (m_buffer == nullptr) {
    Create(m_maxInstances > 0 ? m_maxInstances : 4096);
  }
}

void War3InstanceBuffer::AdvanceInstance() {
  m_capturedInstanceCount++;
  if (m_capturedInstanceCount > m_maxBatchSize) {
    // Should capture error?
    m_capturedInstanceCount = m_maxBatchSize;
  }
}

bool War3InstanceBuffer::IsFull() const {
  return m_capturedInstanceCount >= m_maxBatchSize;
}

War3InstanceBuffer::War3InstanceBuffer(D3D9DeviceEx *device)
    : m_device(device) { }

War3InstanceBuffer::~War3InstanceBuffer() {
  OnLostDevice();
}

HRESULT War3InstanceBuffer::Create(uint32_t maxInstances) {
  if (!m_device || !maxInstances || maxInstances > UINT32_MAX / sizeof(InstanceData))
    return D3DERR_INVALIDCALL;
  Com<IDirect3DVertexBuffer9> buffer;
  const uint32_t bytes = maxInstances * sizeof(InstanceData);
  if (FAILED(m_device->CreateVertexBuffer(
          bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
          D3DPOOL_DEFAULT, &buffer, nullptr))) {
    WAR3_RENDER_LOG("Failed to create War3InstanceBuffer\n");
    return E_FAIL;
  }

  OnLostDevice();
  m_buffer = static_cast<D3D9VertexBuffer*>(buffer.ptr());
  m_maxInstances = maxInstances;
  m_bufferSizeInBytes = bytes;
  return D3D_OK;
}

InstanceData *War3InstanceBuffer::Alloc(uint32_t count,
                                        uint32_t &outBaseOffset) {
  if (m_buffer == nullptr || m_lockedPtr || !count || count > m_maxInstances)
    return nullptr;

  // Simple ring buffer allocation
  // Check if wrap needed
  if (count > m_maxInstances - m_currentOffset) {
    m_currentOffset = 0; // DISCARD happens on Lock
  }

  outBaseOffset = m_currentOffset;
  void *ptr = nullptr;

  DWORD flags = D3DLOCK_NOOVERWRITE;
  if (m_currentOffset == 0) {
    flags = D3DLOCK_DISCARD;
    m_discardsThisFrame++;
  }

  uint32_t sizeToLock = count * sizeof(InstanceData);
  uint32_t offsetInBytes = m_currentOffset * sizeof(InstanceData);

  if (FAILED(m_buffer->Lock(offsetInBytes, sizeToLock, &ptr, flags))) {
    return nullptr;
  }

  m_currentOffset += count;
  m_allocsThisFrame++;

  // Reset batch state
  m_lockedPtr = static_cast<InstanceData *>(ptr);
  m_capturedInstanceCount = 0;
  m_maxBatchSize = count;

  // [Fix] Initialize sub-batch tracking
  m_activeAllocationStart = m_currentOffset - count;
  m_drawnCountThisAlloc = 0;

  return m_lockedPtr;
}

void War3InstanceBuffer::Unlock() {
  if (m_buffer != nullptr && m_lockedPtr) {
    m_buffer->Unlock();
    m_lockedPtr =
        nullptr; // Clear it so we don't accidentally write to it
  }
}

void War3InstanceBuffer::CaptureDrawParams(D3DPRIMITIVETYPE PrimitiveType,
                                           INT BaseVertexIndex,
                                           UINT MinVertexIndex,
                                           UINT NumVertices, UINT StartIndex,
                                           UINT PrimitiveCount) {
  m_drawParams = {PrimitiveType, BaseVertexIndex, MinVertexIndex,
                  NumVertices,   StartIndex,      PrimitiveCount};
}

void War3InstanceBuffer::ReplayDraw() {
  if (m_capturedInstanceCount == 0 || !m_device)
    return;

  // [Fix] Temporarily disable active buffer tracking to prevent
  // the DrawIndexedPrimitive hook from suppressing THIS draw call.
  War3InstanceBuffer *savedActive = s_activeBuffer;
  s_activeBuffer = nullptr;

  // Bind Instance Buffer to Stream 1
  // [Fix] Use accurate sub-batch offset
  uint32_t baseInstance = m_activeAllocationStart + m_drawnCountThisAlloc;

  this->Bind(1, baseInstance);

  // Setup HW Instancing
  m_device->SetStreamSourceFreq(
      0, (D3DSTREAMSOURCE_INDEXEDDATA | m_capturedInstanceCount));
  m_device->SetStreamSourceFreq(1, (D3DSTREAMSOURCE_INSTANCEDATA | 1));

  // [Patch] Switch Vertex Declaration
  IDirect3DVertexDeclaration9 *originalDecl = nullptr;
  IDirect3DVertexDeclaration9 *instancedDecl = nullptr;
  if (SUCCEEDED(m_device->GetVertexDeclaration(&originalDecl)) &&
      originalDecl) {
    instancedDecl = GetInstancedDeclaration(originalDecl);
    if (instancedDecl) {
      m_device->SetVertexDeclaration(instancedDecl);
    }
  }

  // [Dual Shader] Switch to Instanced Shader
  IDirect3DVertexShader9 *currentVS = nullptr;
  IDirect3DVertexShader9 *instancedVS = nullptr;

  if (SUCCEEDED(m_device->GetVertexShader(&currentVS)) && currentVS) {
    // Cast to DXVK implementation to access partner shader
    auto *dxvkShader = reinterpret_cast<dxvk::D3D9VertexShader *>(currentVS);
    instancedVS = dxvkShader->GetPartner();
  }

  if (instancedVS) {
    m_device->SetVertexShader(instancedVS);
  }

  // Draw
  m_device->DrawIndexedPrimitive(
      m_drawParams.PrimitiveType, m_drawParams.BaseVertexIndex,
      m_drawParams.MinVertexIndex, m_drawParams.NumVertices,
      m_drawParams.StartIndex, m_drawParams.PrimitiveCount);

  // Restore Original Shader
  if (instancedVS) {
    m_device->SetVertexShader(currentVS);
    instancedVS->Release();
  }

  if (currentVS) {
    currentVS->Release();
  }

  // Restore State
  m_device->SetStreamSourceFreq(0, 1);
  m_device->SetStreamSourceFreq(1, 1);

  // Restore Decl
  if (originalDecl) {
    m_device->SetVertexDeclaration(originalDecl);
    originalDecl->Release();
  }

  // [Fix] Restore active buffer tracking
  s_activeBuffer = savedActive;
}

void War3InstanceBuffer::FlushBatch() {
  if (m_capturedInstanceCount == 0 || !m_device)
    return;

  // Draw currently accumulated instances
  ReplayDraw();

  // [Fix] Update drawn count for next sub-batch calculations
  m_drawnCountThisAlloc += m_capturedInstanceCount;
  m_capturedInstanceCount = 0;
}

void War3InstanceBuffer::OnSetTexture(uint32_t stage,
                                      IDirect3DBaseTexture9 *texture) {
  if (s_activeBuffer != this || !m_lockedPtr)
    return;

  // Only track stage 0 and 1 for now (TeamColor is usually 1, Diffuse 0)
  if (stage >= 2)
    return;

  if (m_capturedTextures[stage] != texture) {
    // State changed within batch!
    // Flush what we have so far with OLD textures
    FlushBatch();

    // Update state to NEW texture
    TextureRefPrivate(texture, true);
    TextureRefPrivate(m_capturedTextures[stage], false);
    m_capturedTextures[stage] = texture;
  }
}

void War3InstanceBuffer::ResolveTextureChange() {
  // Deprecated/Replaced by OnSetTexture flushing
  FlushBatch();
}

IDirect3DVertexDeclaration9 *War3InstanceBuffer::GetInstancedDeclaration(
    IDirect3DVertexDeclaration9 *originalDecl) {
  const auto cached = m_declCache.find(originalDecl);
  if (cached != m_declCache.end())
    return cached->second.instanced.ptr();

  D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
  UINT numElements = 0;
  if (FAILED(originalDecl->GetDeclaration(elements, &numElements))) {
    return nullptr;
  }

  std::vector<D3DVERTEXELEMENT9> newElements;
  for (UINT i = 0; i < numElements; i++) {
    if (elements[i].Stream != 0xFF) { // Not END
      newElements.push_back(elements[i]);
    }
  }

  // Append Instance Data (Stream 1)
  WORD stream = 1;
  // worldMatrix (4 rows) - UsageIndex 1-4
  newElements.push_back({stream, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
                         D3DDECLUSAGE_TEXCOORD, 1});
  newElements.push_back({stream, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
                         D3DDECLUSAGE_TEXCOORD, 2});
  newElements.push_back({stream, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
                         D3DDECLUSAGE_TEXCOORD, 3});
  newElements.push_back({stream, 48, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
                         D3DDECLUSAGE_TEXCOORD, 4});
  // color - UsageIndex 5
  newElements.push_back({stream, 64, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
                         D3DDECLUSAGE_TEXCOORD, 5});
  // teamColorIndex + padding - UsageIndex 6 (total 16 bytes = float4)
  // .x = teamColorIndex, .yzw = padding (aligned to 16 bytes)
  newElements.push_back({stream, 80, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
                         D3DDECLUSAGE_TEXCOORD, 6});

  newElements.push_back(D3DDECL_END());

  Com<IDirect3DVertexDeclaration9> newDecl;
  if (FAILED(m_device->CreateVertexDeclaration(newElements.data(), &newDecl))) {
    return nullptr;
  }

  DeclarationEntry entry;
  entry.original = static_cast<D3D9VertexDecl*>(originalDecl);
  entry.instanced = static_cast<D3D9VertexDecl*>(newDecl.ptr());
  auto inserted = m_declCache.emplace(originalDecl, std::move(entry));
  return inserted.first->second.instanced.ptr();
}

} // namespace reimpl
} // namespace war3
} // namespace dxvk
