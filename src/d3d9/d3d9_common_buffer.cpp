#include "d3d9_common_buffer.h"

#include "d3d9_device.h"
#include "d3d9_util.h"
#include "war3/gpu_skin/war3_gpu_skin_native_bridge.h"

#include <atomic>

namespace dxvk {

  D3D9CommonBuffer::D3D9CommonBuffer(
          D3D9DeviceEx*      pDevice,
    const D3D9_BUFFER_DESC*  pDesc)
    : m_parent ( pDevice ), m_desc ( *pDesc ),
      m_mapMode(DetermineMapMode(pDevice->GetOptions())) {
    static std::atomic<uint64_t> s_war3BufferGeneration{0u};
    m_war3IdentityGeneration =
        s_war3BufferGeneration.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (m_war3IdentityGeneration == 0u) {
      m_war3IdentityGeneration =
          s_war3BufferGeneration.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    m_buffer = CreateBuffer();
    if (m_mapMode == D3D9_COMMON_BUFFER_MAP_MODE_BUFFER)
      m_stagingBuffer = CreateStagingBuffer();

    m_allocation = GetMapBuffer()->storage();

    const auto allocationBytes = [](const Rc<DxvkBuffer>& buffer) {
      const Rc<DxvkResourceAllocation> storage =
          buffer != nullptr ? buffer->storage() : nullptr;
      return storage != nullptr
          ? uint64_t(storage->getMemoryInfo().size)
          : 0u;
    };
    const uint64_t realBytes = allocationBytes(m_buffer);
    const uint64_t stagingBytes = allocationBytes(m_stagingBuffer);
    const uint64_t cpuAddressBytes = m_stagingBuffer != nullptr
        ? stagingBytes
        : (m_mapMode == D3D9_COMMON_BUFFER_MAP_MODE_DIRECT
            ? realBytes : 0u);
    war3::resource_census::ResourceRegistration census = {};
    census.resourceClass = m_desc.Type == D3DRTYPE_INDEXBUFFER
        ? war3::resource_census::ResourceClass::IndexBuffer
        : war3::resource_census::ResourceClass::VertexBuffer;
    census.pool = static_cast<uint32_t>(m_desc.Pool);
    census.usage = m_desc.Usage;
    census.mapMode = static_cast<uint32_t>(m_mapMode);
    census.subresourceCount = 1u;
    census.logicalBytes = m_desc.Size;
    census.deviceAllocationBytes = realBytes;
    census.hostBackingLogicalBytes = cpuAddressBytes;
    census.hostMappedLogicalBytes = cpuAddressBytes;
    census.duplicateHostBackingLogicalBytes = stagingBytes;
    census.dynamic = (m_desc.Usage & D3DUSAGE_DYNAMIC) != 0u;
    census.writeOnly = (m_desc.Usage & D3DUSAGE_WRITEONLY) != 0u;
    census.hasDeviceCopy = m_buffer != nullptr;
    census.deviceReadbackCapable =
        m_desc.Type == D3DRTYPE_VERTEXBUFFER && m_buffer != nullptr;
    // D3D9CommonBuffer 的 host backing 来自 Vulkan resource allocation，
    // 不属于 D3D9Memory chunk，必须明确排除在 chunk binding 闭合之外。
    census.hostBinding.bindingClass = cpuAddressBytes != 0u
        ? war3::resource_census::HostBackingBindingClass::
              ExternalHostAllocation
        : war3::resource_census::HostBackingBindingClass::None;
    m_war3ResourceCensus = war3::resource_census::Register(census);

    if (m_desc.Pool != D3DPOOL_DEFAULT)
      m_dirtyRange = D3D9Range(0, m_desc.Size);
  }

  D3D9CommonBuffer::~D3D9CommonBuffer() {
    if (m_desc.Type == D3DRTYPE_VERTEXBUFFER &&
        m_desc.Pool == D3DPOOL_DEFAULT &&
        (m_desc.Usage & D3DUSAGE_DYNAMIC) != 0u &&
        War3GpuSkinNativeTracked()) {
      war3::gpu_skin::NotifyNativeVertexResourceRetired(
          reinterpret_cast<uintptr_t>(this), m_war3IdentityGeneration);
    }
    if (m_desc.Pool == D3DPOOL_DEFAULT)
      m_parent->DecrementLosableCounter();
  }


  HRESULT D3D9CommonBuffer::Lock(
          UINT   OffsetToLock,
          UINT   SizeToLock,
          void** ppbData,
          DWORD  Flags,
     uintptr_t   OwnerIdentity) {
    return m_parent->LockBuffer(
      this,
      OffsetToLock,
      SizeToLock,
      ppbData,
      Flags,
      OwnerIdentity);
  }


  HRESULT D3D9CommonBuffer::Unlock(uintptr_t OwnerIdentity) {
    return m_parent->UnlockBuffer(this, OwnerIdentity);
  }


  HRESULT D3D9CommonBuffer::ValidateBufferProperties(const D3D9_BUFFER_DESC* pDesc, const bool IsExtended) {
    if (unlikely(pDesc->Size == 0))
      return D3DERR_INVALIDCALL;

    // Neither vertex nor index buffers can be created in D3DPOOL_SCRATCH
    // or in D3DPOOL_MANAGED with D3DUSAGE_DYNAMIC. On extended devices,
    // D3DPOOL_MANAGED can not be used at all, regardless of usage flags.
    if (unlikely(pDesc->Pool == D3DPOOL_SCRATCH
             || (pDesc->Pool == D3DPOOL_MANAGED && (IsExtended ||
                                                    pDesc->Usage & D3DUSAGE_DYNAMIC))))
      return D3DERR_INVALIDCALL;

    // D3DUSAGE_AUTOGENMIPMAP, D3DUSAGE_DEPTHSTENCIL and D3DUSAGE_RENDERTARGET
    // are not permitted on index or vertex buffers.
    if (unlikely((pDesc->Usage & D3DUSAGE_AUTOGENMIPMAP)
              || (pDesc->Usage & D3DUSAGE_DEPTHSTENCIL)
              || (pDesc->Usage & D3DUSAGE_RENDERTARGET)))
      return D3DERR_INVALIDCALL;

    return D3D_OK;
  }


  void D3D9CommonBuffer::PreLoad() {
    if (IsPoolManaged(m_desc.Pool)) {
      auto lock = m_parent->LockDevice();

      if (NeedsUpload())
        m_parent->FlushBuffer(this);
    }
  }

  
  D3D9_COMMON_BUFFER_MAP_MODE D3D9CommonBuffer::DetermineMapMode(const D3D9Options* options) const {
    if (m_desc.Pool != D3DPOOL_DEFAULT)
      return D3D9_COMMON_BUFFER_MAP_MODE_BUFFER;

    // CSGO keeps vertex buffers locked across multiple frames and writes to it. It uses them for drawing without unlocking first.
    // Tests show that D3D9 DEFAULT + USAGE_DYNAMIC behaves like a directly mapped buffer even when unlocked.
    // DEFAULT + WRITEONLY does not behave like a directly mapped buffer EXCEPT if its locked at the moment.
    // TODO: Work around that by adding option that maps WRITEONLY directly or disables the staging buffer for buffer uploads.

    if (!(m_desc.Usage & D3DUSAGE_DYNAMIC))
      return D3D9_COMMON_BUFFER_MAP_MODE_BUFFER;

    if (!options->allowDirectBufferMapping)
      return D3D9_COMMON_BUFFER_MAP_MODE_BUFFER;

    return D3D9_COMMON_BUFFER_MAP_MODE_DIRECT;
  }


  Rc<DxvkBuffer> D3D9CommonBuffer::CreateBuffer() const {
    DxvkBufferCreateInfo  info;
    info.size   = m_desc.Size;
    info.usage  = 0;
    info.stages = 0;
    info.access = 0;

    VkMemoryPropertyFlags memoryFlags = 0;

    if (m_desc.Type == D3DRTYPE_VERTEXBUFFER) {
      info.usage  |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
      // GPU 蒙皮 Dual 模式会把真实上传后的 VB 与 compute 输出比较。无论使用直接
      // 映射还是 staging 映射，都必须让真实顶点缓冲可被复制读取；usage 在创建后不可变。
      info.usage  |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      info.stages |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT
                  |  VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
                  |  VK_ACCESS_TRANSFER_READ_BIT;

      if (m_parent->SupportsSWVP()) {
        info.usage  |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.stages |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
        info.access |= VK_ACCESS_SHADER_WRITE_BIT;
      }
    }
    else if (m_desc.Type == D3DRTYPE_INDEXBUFFER) {
      // Stage11 exact shadow capture may need a same-command-stream snapshot
      // when an index buffer has no CPU-readable current allocation.  Buffer
      // usage is immutable, so every real IB must advertise transfer-read at
      // creation time even though the common path only binds it for indexing.
      info.usage  |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                  |  VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      info.stages |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT
                  |  VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access |= VK_ACCESS_INDEX_READ_BIT
                  |  VK_ACCESS_TRANSFER_READ_BIT;
    }

    if (m_mapMode == D3D9_COMMON_BUFFER_MAP_MODE_DIRECT) {
      info.stages |= VK_PIPELINE_STAGE_HOST_BIT
                  |  VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.usage  |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      info.access |= VK_ACCESS_HOST_WRITE_BIT
                  |  VK_ACCESS_TRANSFER_READ_BIT;

      memoryFlags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                  |  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

      if ((m_desc.Usage & D3DUSAGE_WRITEONLY) == 0
        || DoPerDrawUpload()
        || m_parent->CanOnlySWVP()
        || m_parent->GetOptions()->cachedWriteOnlyBuffers) {
        // Never use uncached memory on devices that support SWVP because we might end up reading from it.

        info.access |= VK_ACCESS_HOST_READ_BIT;
        memoryFlags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      } else {
        memoryFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      }
    }
    else {
      info.stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.usage  |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.access |= VK_ACCESS_TRANSFER_WRITE_BIT;

      memoryFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }

    return m_parent->GetDXVKDevice()->createBuffer(info, memoryFlags);
  }


  Rc<DxvkBuffer> D3D9CommonBuffer::CreateStagingBuffer() const {
    DxvkBufferCreateInfo  info;
    info.size   = m_desc.Size;
    info.stages = VK_PIPELINE_STAGE_HOST_BIT
                | VK_PIPELINE_STAGE_TRANSFER_BIT;

    info.usage  = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    info.access = VK_ACCESS_HOST_WRITE_BIT
                | VK_ACCESS_TRANSFER_READ_BIT;

    if (!(m_desc.Usage & D3DUSAGE_WRITEONLY))
      info.access |= VK_ACCESS_HOST_READ_BIT;

    VkMemoryPropertyFlags memoryFlags = 
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

    return m_parent->GetDXVKDevice()->createBuffer(info, memoryFlags);
  }

}
