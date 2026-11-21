#pragma once
#include <d3d9.h>
#include <atomic>
#include <cstdio>

// Mechanically transcribed public D3D9 COM signatures, not a fake device.
// Unsupported HRESULT methods fail visibly. Non-HRESULT methods have no
// failure channel: return the empty/unsupported value; no renderer side effect.
// This base is only used by the isolated, incomplete development DLL.
namespace warvk::host::d3d9proxy {
// First-wiring aid (phase 5.1): with the game profile variable set, every
// unsupported entry reports its first call on stderr, so a game run lists
// exactly which D3D9 entries it used that this proxy does not implement.
inline bool TraceUnsupported() noexcept {
  static const bool enabled=GetEnvironmentVariableW(L"WARVK_D3D9_GAME",nullptr,0)!=0;
  return enabled;
}
inline void NoteUnsupported(const char* entry,std::atomic<unsigned>& calls) noexcept {
  if(calls.fetch_add(1,std::memory_order_relaxed)==0&&TraceUnsupported()) {
    std::fprintf(stderr,"D3D9 game: unsupported %s\n",entry);std::fflush(stderr);
  }
}
#define WARVK_D3D9_UNSUPPORTED() \
  do{static std::atomic<unsigned> warvkCalls{0};::warvk::host::d3d9proxy::NoteUnsupported(__PRETTY_FUNCTION__,warvkCalls);}while(0)
template<class I> class Unsupported;
template<> class Unsupported<IDirect3D9> : public IDirect3D9 {
public:
  HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  UINT STDMETHODCALLTYPE GetAdapterCount() override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT iAdapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, WINBOOL bWindowed) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, WINBOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, struct IDirect3DDevice9** ppReturnedDeviceInterface) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
};
template<> class Unsupported<IDirect3DDevice9> : public IDirect3DDevice9 {
public:
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  HRESULT STDMETHODCALLTYPE EvictManagedResources() override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D9) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  void STDMETHODCALLTYPE SetCursorPosition(int X,int Y, DWORD Flags) override { WARVK_D3D9_UNSUPPORTED(); }
  WINBOOL STDMETHODCALLTYPE ShowCursor(WINBOOL bShow) override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Present(const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetDialogBoxMode(WINBOOL bEnableDialogs) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  void STDMETHODCALLTYPE SetGammaRamp(UINT swapchain_idx, DWORD flags, const D3DGAMMARAMP *ramp) override { WARVK_D3D9_UNSUPPORTED(); }
  void STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) override { WARVK_D3D9_UNSUPPORTED(); }
  HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, WINBOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, WINBOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9 *src_surface, const RECT *src_rect, IDirect3DSurface9 *dst_surface, const POINT *dst_point) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *src_surface, const RECT *src_rect, IDirect3DSurface9 *dst_surface, const RECT *dst_rect, D3DTEXTUREFILTERTYPE filter) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR color) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE BeginScene() override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EndScene() override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Clear(DWORD rect_count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *viewport) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pViewport) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *material) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pMaterial) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9 *light) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9*) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, WINBOOL Enable) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, WINBOOL* pEnable) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float *plane) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float* pPlane) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE BeginStateBlock() override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9 *clip_status) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* pClipStatus) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pNumPasses) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette_idx, const PALETTEENTRY *entries) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber,PALETTEENTRY* pEntries) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *rect) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pRect) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(WINBOOL bSoftware) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  WINBOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  float STDMETHODCALLTYPE GetNPatchMode() override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT primitive_count, const void *data, UINT stride) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT min_vertex_idx, UINT vertex_count, UINT primitive_count, const void *index_data, D3DFORMAT index_format, const void *data, UINT stride) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **declaration) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *byte_code, IDirect3DVertexShader9 **shader) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pShader) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppShader) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT reg_idx, const float *data, UINT count) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT reg_idx, const int *data, UINT count) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT reg_idx, const WINBOOL *data, UINT count) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister, WINBOOL* pConstantData, UINT BoolCount) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* OffsetInBytes, UINT* pStride) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Divider) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT* Divider) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIndexData) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIndexData) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *byte_code, IDirect3DPixelShader9 **shader) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pShader) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppShader) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT reg_idx, const float *data, UINT count) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT reg_idx, const int *data, UINT count) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT reg_idx, const WINBOOL *data, UINT count) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, WINBOOL* pConstantData, UINT BoolCount) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT handle, const float *segment_count, const D3DRECTPATCH_INFO *patch_info) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT handle, const float *segment_count, const D3DTRIPATCH_INFO *patch_info) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
};
template<> class Unsupported<IDirect3DVertexBuffer9> : public IDirect3DVertexBuffer9 {
public:
  HRESULT STDMETHODCALLTYPE GetDevice(struct IDirect3DDevice9** ppDevice) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, DWORD data_size, DWORD flags) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void* pData, DWORD* pSizeOfData) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  void STDMETHODCALLTYPE PreLoad() override { WARVK_D3D9_UNSUPPORTED(); }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_FORCE_DWORD; }
  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Unlock() override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* pDesc) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
};
template<> class Unsupported<IDirect3DIndexBuffer9> : public IDirect3DIndexBuffer9 {
public:
  HRESULT STDMETHODCALLTYPE GetDevice(struct IDirect3DDevice9** ppDevice) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, DWORD data_size, DWORD flags) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void* pData, DWORD* pSizeOfData) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  void STDMETHODCALLTYPE PreLoad() override { WARVK_D3D9_UNSUPPORTED(); }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_FORCE_DWORD; }
  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Unlock() override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* pDesc) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
};
template<> class Unsupported<IDirect3DTexture9> : public IDirect3DTexture9 {
public:
  HRESULT STDMETHODCALLTYPE GetDevice(struct IDirect3DDevice9** ppDevice) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, DWORD data_size, DWORD flags) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void* pData, DWORD* pSizeOfData) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  void STDMETHODCALLTYPE PreLoad() override { WARVK_D3D9_UNSUPPORTED(); }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_FORCE_DWORD; }
  DWORD STDMETHODCALLTYPE SetLOD(DWORD LODNew) override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  DWORD STDMETHODCALLTYPE GetLOD() override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  DWORD STDMETHODCALLTYPE GetLevelCount() override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_NONE; }
  void STDMETHODCALLTYPE GenerateMipSubLevels() override { WARVK_D3D9_UNSUPPORTED(); }
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9** ppSurfaceLevel) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *dirty_rect) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
};
template<> class Unsupported<IDirect3DSurface9> : public IDirect3DSurface9 {
public:
  HRESULT STDMETHODCALLTYPE GetDevice(struct IDirect3DDevice9** ppDevice) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, DWORD data_size, DWORD flags) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void* pData, DWORD* pSizeOfData) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  void STDMETHODCALLTYPE PreLoad() override { WARVK_D3D9_UNSUPPORTED(); }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_FORCE_DWORD; }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** ppContainer) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* pDesc) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UnlockRect() override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDC(HDC* phdc) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
};
template<> class Unsupported<IDirect3DVertexShader9> : public IDirect3DVertexShader9 {
public:
  HRESULT STDMETHODCALLTYPE GetDevice(struct IDirect3DDevice9** ppDevice) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFunction(void*, UINT* pSizeOfData) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
};
template<> class Unsupported<IDirect3DPixelShader9> : public IDirect3DPixelShader9 {
public:
  HRESULT STDMETHODCALLTYPE GetDevice(struct IDirect3DDevice9** ppDevice) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFunction(void*, UINT* pSizeOfData) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
};
template<> class Unsupported<IDirect3DVertexDeclaration9> : public IDirect3DVertexDeclaration9 {
public:
  HRESULT STDMETHODCALLTYPE GetDevice(struct IDirect3DDevice9** ppDevice) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9*, UINT* pNumElements) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
};
template<> class Unsupported<IDirect3DStateBlock9> : public IDirect3DStateBlock9 {
public:
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Capture() override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Apply() override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
};
template<> class Unsupported<IDirect3DQuery9> : public IDirect3DQuery9 {
public:
  HRESULT STDMETHODCALLTYPE GetDevice(struct IDirect3DDevice9** ppDevice) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return D3DQUERYTYPE_EVENT; }
  DWORD STDMETHODCALLTYPE GetDataSize() override { WARVK_D3D9_UNSUPPORTED(); return 0; }
  HRESULT STDMETHODCALLTYPE Issue(DWORD dwIssueFlags) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetData(void* pData, DWORD dwSize, DWORD dwGetDataFlags) override { WARVK_D3D9_UNSUPPORTED(); return E_NOTIMPL; }
};
} // namespace warvk::host::d3d9proxy

