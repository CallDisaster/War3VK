#pragma once

#include "../../d3d9_buffer.h"
#include "../../d3d9_texture.h"

namespace dxvk::war3::imgui_owned {

// 仅供 WarVK 的 device-owned ImGui backend 使用。调用者必须是本模块的
// D3D9DeviceEx，并在初始化/渲染期间持有该 device；不是任意外部 D3D9 代理。
// public Get/QueryInterface/临时 stateblock 仍按原合同保活 device。
template<typename Implementation, typename Interface>
inline HRESULT RetainCreatedResource(HRESULT result, Interface** output) {
    if (SUCCEEDED(result) && output && *output) {
        auto* resource = static_cast<Implementation*>(*output);
        // 先取得 private owner，再交还 Create 的 public 引用；不可反序。
        resource->AddRefPrivate();
        resource->Release();
    }
    return result;
}

inline HRESULT CreateVertexBuffer(IDirect3DDevice9* device, UINT length,
        DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** output,
        HANDLE* sharedHandle) {
    const HRESULT result = device->CreateVertexBuffer(
        length, usage, fvf, pool, output, sharedHandle);
    return RetainCreatedResource<D3D9VertexBuffer>(result, output);
}

inline HRESULT CreateIndexBuffer(IDirect3DDevice9* device, UINT length,
        DWORD usage, D3DFORMAT format, D3DPOOL pool,
        IDirect3DIndexBuffer9** output, HANDLE* sharedHandle) {
    const HRESULT result = device->CreateIndexBuffer(
        length, usage, format, pool, output, sharedHandle);
    return RetainCreatedResource<D3D9IndexBuffer>(result, output);
}

inline HRESULT CreateTexture(IDirect3DDevice9* device, UINT width, UINT height,
        UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
        IDirect3DTexture9** output, HANDLE* sharedHandle) {
    const HRESULT result = device->CreateTexture(
        width, height, levels, usage, format, pool, output, sharedHandle);
    return RetainCreatedResource<D3D9Texture2D>(result, output);
}

inline void ReleaseVertexBuffer(IDirect3DVertexBuffer9* resource) {
    if (resource)
        static_cast<D3D9VertexBuffer*>(resource)->ReleasePrivate();
}

inline void ReleaseIndexBuffer(IDirect3DIndexBuffer9* resource) {
    if (resource)
        static_cast<D3D9IndexBuffer*>(resource)->ReleasePrivate();
}

inline void ReleaseTexture(IDirect3DTexture9* resource) {
    if (resource)
        static_cast<D3D9Texture2D*>(resource)->ReleasePrivate();
}

} // namespace dxvk::war3::imgui_owned
