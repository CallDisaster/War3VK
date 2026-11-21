#pragma once

#include "../platform/war3_proxy_mode.h"
#include "../platform/war3_proxy_overlay.h"

namespace dxvk::war3::imgui_proxy {

// Only for the proxy variant of the generated ImGui backend
// (war3_imgui_dx9_proxy.h). The device is the proxy front end's
// (tools/render_host/d3d9_proxy.cpp): its objects are not this module's
// classes, so the private reference is taken through the front end's
// WarVKD3D9RetainPrivate / WarVKD3D9ReleasePrivate (proxy::RetainPrivate).
// As in process, the panel's buffers and atlas then never keep the device
// alive: the game's last Release still destroys it, and the overlay frees them
// in the `released` callback, before the device is gone.
template<typename Interface>
inline HRESULT RetainCreatedResource(HRESULT result, Interface** output) {
  if (SUCCEEDED(result) && output && *output) {
    const HRESULT retained = proxy::RetainPrivate(*output);
    if (FAILED(retained)) {
      // Never keep a public reference: it would hold the device.
      (*output)->Release();
      *output = nullptr;
      result = retained;
    }
  }
  proxy::NoteOverlayResource(result);
  return result;
}

inline HRESULT CreateVertexBuffer(IDirect3DDevice9* device, UINT length,
    DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** output,
    HANDLE* sharedHandle) {
  const HRESULT result = device->CreateVertexBuffer(
      length, usage, fvf, pool, output, sharedHandle);
  return RetainCreatedResource(result, output);
}

inline HRESULT CreateIndexBuffer(IDirect3DDevice9* device, UINT length,
    DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DIndexBuffer9** output, HANDLE* sharedHandle) {
  const HRESULT result = device->CreateIndexBuffer(
      length, usage, format, pool, output, sharedHandle);
  return RetainCreatedResource(result, output);
}

// The front end takes textures only as MANAGED with no usage (DYNAMIC +
// DEFAULT is refused). The atlas is written through LockRect either way.
inline HRESULT CreateTexture(IDirect3DDevice9* device, UINT width, UINT height,
    UINT levels, DWORD /*usage*/, D3DFORMAT format, D3DPOOL /*pool*/,
    IDirect3DTexture9** output, HANDLE* sharedHandle) {
  const HRESULT result = device->CreateTexture(
      width, height, levels, 0, format, D3DPOOL_MANAGED, output, sharedHandle);
  return RetainCreatedResource(result, output);
}

inline void ReleaseVertexBuffer(IDirect3DVertexBuffer9* resource) {
  if (resource)
    proxy::ReleasePrivate(resource);
}

inline void ReleaseIndexBuffer(IDirect3DIndexBuffer9* resource) {
  if (resource)
    proxy::ReleasePrivate(resource);
}

inline void ReleaseTexture(IDirect3DTexture9* resource) {
  if (resource)
    proxy::ReleasePrivate(resource);
}

} // namespace dxvk::war3::imgui_proxy
