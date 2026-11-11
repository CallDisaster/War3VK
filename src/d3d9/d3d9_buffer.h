#pragma once

#include "d3d9_resource.h"

#include "d3d9_common_buffer.h"

namespace dxvk {

  template <typename... Type>
  class D3D9Buffer : public D3D9Resource<Type...> {

  public:

    D3D9Buffer(
            D3D9DeviceEx*      pDevice,
      const D3D9_BUFFER_DESC*  pDesc,
      const bool               Extended)
    : D3D9Resource<Type...> (pDevice, pDesc->Pool, Extended ),
      m_buffer              (pDevice, pDesc) {

    }

    HRESULT STDMETHODCALLTYPE Lock(
            UINT   OffsetToLock,
            UINT   SizeToLock,
            void** ppbData,
            DWORD  Flags) final {
#if defined(WARVK_D3D9_MIGRATION_CENSUS_DEV)
      // This public entry's own return address: the application's call site.
      m_buffer.CensusLock(OffsetToLock, SizeToLock, Flags, __builtin_return_address(0));
#endif
      return m_buffer.Lock(
        OffsetToLock,
        SizeToLock,
        ppbData,
        Flags,
        reinterpret_cast<uintptr_t>(this));
    }

    HRESULT STDMETHODCALLTYPE Unlock() final {
#if defined(WARVK_D3D9_MIGRATION_CENSUS_DEV)
      m_buffer.CensusUnlock();
#endif
      return m_buffer.Unlock(reinterpret_cast<uintptr_t>(this));
    }

    void STDMETHODCALLTYPE PreLoad() final {
      m_buffer.PreLoad();
    }

    D3D9CommonBuffer* GetCommonBuffer() {
      return &m_buffer;
    }

  protected:

    D3D9CommonBuffer m_buffer;

  };


  using D3D9VertexBufferBase = D3D9Buffer<IDirect3DVertexBuffer9>;
  class D3D9VertexBuffer final : public D3D9VertexBufferBase {

  public:

    D3D9VertexBuffer(
            D3D9DeviceEx*      pDevice,
      const D3D9_BUFFER_DESC*  pDesc,
      const bool               Extended);

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void** ppvObject);

    D3DRESOURCETYPE STDMETHODCALLTYPE GetType();

    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* pDesc);

  };

  using D3D9IndexBufferBase = D3D9Buffer<IDirect3DIndexBuffer9>;
  class D3D9IndexBuffer final : public D3D9IndexBufferBase {

  public:

    D3D9IndexBuffer(
            D3D9DeviceEx*      pDevice,
      const D3D9_BUFFER_DESC*  pDesc,
      const bool               Extended);

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void** ppvObject);

    D3DRESOURCETYPE STDMETHODCALLTYPE GetType();

    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* pDesc);

  };

  template <typename T>
  inline D3D9CommonBuffer* GetCommonBuffer(const T& pResource) {
    return pResource != nullptr ? pResource->GetCommonBuffer() : nullptr;
  }

}
