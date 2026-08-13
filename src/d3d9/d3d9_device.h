#pragma once

#include "../dxvk/dxvk_cs.h"
#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_staging.h"


#include "d3d9_adapter.h"
#include "d3d9_constant_buffer.h"
#include "d3d9_constant_set.h"
#include "d3d9_cursor.h"
#include "d3d9_format.h"
#include "d3d9_include.h"
#include "d3d9_mem.h"
#include "d3d9_multithread.h"


#include "d3d9_state.h"

#include "d3d9_options.h"

#include "../dxso/dxso_modinfo.h"
#include "../dxso/dxso_module.h"
#include "../dxso/dxso_options.h"
#include "../dxso/dxso_util.h"


#include "d3d9_fixed_function.h"
#include "d3d9_swvp_emu.h"

#include "d3d9_interop.h"
#include "d3d9_on_12.h"
#include "d3d9_spec_constants.h"


#include "d3d9_bridge.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_set>


#include "d3d9_war3_shadow.h"
#include "d3d9_war3_ssao.h"
#include "war3/shadow/war3_shadow_backend_dxvk.h"
#include "war3/render/war3_shadow_generation_backed_stream.h"
#include "war3/render/war3_stage11_snapshot_page_policy.h"
#include "war3/gpu_skin/war3_persistent_gpu_package_d3d9_observe_owner.h"
#include "war3/gpu_skin/war3_persistent_gpu_package_stage11_observe_adapter.h"
#include <type_traits>
#include <unordered_map>
#include <vector>


namespace dxvk {
namespace war3 {
class War3Material;
namespace shadow {
struct ShadowDrawPacket;
struct ShadowSubmissionFrame;
}
namespace render {
enum class ObjectKind : uint8_t;
struct CurrentDrawContractRecord;
struct CurrentDrawAuthoritativeSample;
}
namespace gpu_skin {
class War3GpuSkinCompute;
class War3GpuSkinManager;
struct FlushRequest;
struct GpuSkinHostSubmitResult;
struct GpuSkinNativeBypassHostRequest;
struct GpuSkinPendingBatch;
struct GpuSkinResolvedDraw;
struct NativeCpuRewriteOutputProof;
struct NativeDipObservation;
struct NativeFlushObservation;
struct NativeUploadObservation;
struct NativeVertexOutputProof;
}
}
} // namespace dxvk
#include "d3d9_war3_aa.h"

#include "../util/util_flush.h"

// War3 渲染增强：帧捕获数据结构
#include "../util/util_lru.h"
#include "d3d9_war3_scene.h"
#include "war3/render/war3_frame_hash_index.h"


namespace dxvk {

class War3RenderPipeline;
class War3PostProcess;

// Draw-time vertex snapshots belong to one object instance and one logical
// draw slice. Model parts can be shared by many units, and one part/layer can
// still publish several geoset/palette payloads in a frame. Omitting either
// identity lets a later unit overwrite the VB/IB/world tuple consumed by an
// earlier unit and emit a one-frame triangle towards the world origin.
struct War3DrawTimeVBCacheKey {
  uint64_t mapEpoch = 0u;
  void* instanceIdentity = nullptr;
  void* meshPayloadPtr = nullptr;
  void* renderablePart = nullptr;
  uint32_t jHandle = 0u;
  uint32_t layerIndex = 0u;
  uint32_t payloadWord108 = 0u;
  uint32_t payloadWord11C = 0u;

  bool operator==(const War3DrawTimeVBCacheKey& other) const noexcept {
    return mapEpoch == other.mapEpoch &&
           instanceIdentity == other.instanceIdentity &&
           meshPayloadPtr == other.meshPayloadPtr &&
           renderablePart == other.renderablePart &&
           jHandle == other.jHandle &&
           layerIndex == other.layerIndex &&
           payloadWord108 == other.payloadWord108 &&
           payloadWord11C == other.payloadWord11C;
  }
};

struct War3DrawTimeVBCacheKeyHash {
  size_t operator()(const War3DrawTimeVBCacheKey& key) const noexcept {
    size_t hash = std::hash<uint64_t>{}(key.mapEpoch);
    hash ^= std::hash<uintptr_t>{}(
        reinterpret_cast<uintptr_t>(key.instanceIdentity));
    hash ^= std::hash<uintptr_t>{}(
                reinterpret_cast<uintptr_t>(key.meshPayloadPtr)) +
            size_t(0x9e3779b9u) + (hash << 6u) + (hash >> 2u);
    hash ^= std::hash<uintptr_t>{}(
                reinterpret_cast<uintptr_t>(key.renderablePart)) +
            size_t(0x9e3779b9u) + (hash << 6u) + (hash >> 2u);
    hash ^= std::hash<uint32_t>{}(key.jHandle) +
            size_t(0x9e3779b9u) + (hash << 6u) + (hash >> 2u);
    hash ^= std::hash<uint32_t>{}(key.layerIndex) +
            size_t(0x9e3779b9u) + (hash << 6u) + (hash >> 2u);
    hash ^= std::hash<uint32_t>{}(key.payloadWord108) +
            size_t(0x9e3779b9u) + (hash << 6u) + (hash >> 2u);
    hash ^= std::hash<uint32_t>{}(key.payloadWord11C) +
            size_t(0x9e3779b9u) + (hash << 6u) + (hash >> 2u);
    return hash;
  }
};

// A verified anonymous LOSBlocker rejection must also outrank a short-lived
// CurrentDraw/packet representation captured before the exact Stage11 draw.
// That older representation can carry a different instance or payload
// generation, but it still names the same native model part, mesh payload,
// and layer. Keep this deliberately weaker key isolated to the narrow marker
// rejection path; it must never authorize geometry reuse.
struct War3DrawTimeAnonymousMarkerSliceKey {
  void* renderablePart = nullptr;
  void* meshPayloadPtr = nullptr;
  uint32_t layerIndex = 0u;

  bool operator==(
      const War3DrawTimeAnonymousMarkerSliceKey& other) const noexcept {
    return renderablePart == other.renderablePart &&
           meshPayloadPtr == other.meshPayloadPtr &&
           layerIndex == other.layerIndex;
  }
};

struct War3DrawTimeAnonymousMarkerSliceKeyHash {
  size_t operator()(
      const War3DrawTimeAnonymousMarkerSliceKey& key) const noexcept {
    size_t hash = std::hash<uintptr_t>{}(
        reinterpret_cast<uintptr_t>(key.renderablePart));
    hash ^= std::hash<uintptr_t>{}(
                reinterpret_cast<uintptr_t>(key.meshPayloadPtr)) +
            size_t(0x9e3779b9u) + (hash << 6u) + (hash >> 2u);
    hash ^= std::hash<uint32_t>{}(key.layerIndex) +
            size_t(0x9e3779b9u) + (hash << 6u) + (hash >> 2u);
    return hash;
  }
};

class D3D9InterfaceEx;
class D3D9SwapChainEx;
class D3D9CommonTexture;
class D3D9CommonBuffer;
class D3D9CommonShader;
class D3D9ShaderModuleSet;
class D3D9Initializer;
class D3D9Query;
class D3D9StateBlock;
class D3D9FormatHelper;
class D3D9UserDefinedAnnotation;

enum class D3D9DeviceDirtyFlag : uint32_t {
  Framebuffer,
  ClipPlanes,
  DepthStencilState,
  BlendState,
  RasterizerState,
  DepthBias,
  AlphaTestState,
  InputLayout,
  ViewportScissor,
  MultiSampleState,
  VertexBuffers,
  IndexBuffer,

  FogState,
  FogColor,
  FogDensity,
  FogScale,
  FogEnd,

  FFVertexData,
  FFVertexBlend,
  FFVertexShader,
  FFPixelShader,
  FFViewport,
  FFPixelData,
  SharedPixelShaderData,
  DepthBounds,
  PointScale,

  SpecializationEntries,
};

using D3D9DeviceDirtyFlags = Flags<D3D9DeviceDirtyFlag>;

enum class D3D9DeviceLostState {
  Ok = 0,
  Lost = 1,
  NotReset = 2,
};

struct D3D9DrawInfo {
  uint32_t vertexCount;
  uint32_t instanceCount;
};

struct D3D9BufferSlice {
  DxvkBufferSlice slice = {};
  void *mapPtr = nullptr;
};

struct D3D9TextureSlotTracking {
  /* Pixel shaders can access 16 textures/samplers.
   * Then there's 1 dmap texture/sampler.
   * Vertex shaders can use 4 textures/samplers.
   * So unless otherwise noted most bitmasks use 21 bits
   * and each bit is one texture/sampler slot.
   * See RemapSamplerState(), IsPSSampler(), IsVSSampler() in d3d9_util.h */

  /** Whether the format of the texture currently bound to each slot is a format
   * that gets fetched in comparison mode. */
  uint32_t depth = 0;

  /** If a depth texture format isn't supported, we fall back to D32F.
   * We'll need to clamp the reference value if the original format was an unorm
   * format. This tracks the texture/sampler slots for which this kind of
   * adjusting needs to be done. */
  uint32_t drefClamp = 0;

  /** Used to store the type of each bound pixel shader texture.
   * This is used to generate fixed function shader code
   * and for PS 1.1 shaders which do not provide this information in the shader
   * bytecode. SM 1.1 and fixed function doesn't allow sampling textures in the
   * VS, so we only need the 16 PS slots. There's 3 texture types, so every
   * texture/sampler slot uses 2 bits. */
  uint32_t textureType = 0;

  /** Whether the type of the texture currently bound to each slot matches the
   * texture type that the shader expects */
  uint32_t mismatchingTextureType = 0;

  /** Whether projected texture lookup is enabled for each texture/sampler slot.
   * This is only used for generating fixed function shaders. */
  uint32_t projected = 0;

  /** Whether sampler slots whose state has been changed and bindings in the
   * backend need to be updated */
  uint32_t samplerStateDirty = 0;

  /** Whether Fetch 4 is enabled for a sampler slot.
   * This just means the application enabled it using the sampler state.
   * It does not mean Fetch 4 is actually active, as that depends on other
   * factors such as the sampling mode and the texture format. */
  uint32_t fetch4SamplerState = 0;

  /** Whether Fetch 4 is active. */
  uint32_t fetch4 = 0;

  /** Whether the texture bound to a slot has been changed and bindings in the
   * backend need to be updated */
  uint32_t textureDirty = 0;

  /** Whether the texture bound to a slot has D3DUSAGE_RENDERTARGET */
  uint32_t rtUsage = 0;

  /** Whether the texture bound to a slot has D3DUSAGE_DEPTHSTENCIL */
  uint32_t dsUsage = 0;

  /** Whether the texture bound to a slot is also bound as a render target
   * and the render target is actually used for writing. */
  uint32_t unresolvableHazardRT = 0;

  /** Whether the texture bound to a slot is also bound as the depth stencil
   * surface and depth stencil surface is actually used for depth testing. */
  uint32_t unresolvableHazardDS = 0;

  /** Whether the texture bound to a slot is also bound as a render target */
  uint32_t hazardRT = 0;

  /** Whether the texture bound to a slot is also bound as the depth stencil
   * view */
  uint32_t hazardDS = 0;

  /** Whether there's a texture bound to a slot */
  uint32_t bound = 0;

  /** Whether there's a texture bound to a slot that needs to be uploaded at
   * draw time */
  uint32_t needsUpload = 0;

  /** Whether there's a texture bound to a slot that needs to have its mip maps
   * generated */
  uint32_t needsMipGen = 0;
};

struct D3D9RTSlotTracking {
  /* D3D9 allows rendering to 4 render targets at the same time.
   * So all RT bit masks only use the first 4 bits. */

  /** Whether a render target is a D3D9Texture rather than just a D3D9Surface.
   * Textures can be bound for sampling so there can be a feedback loop if this
   * RT is also bound for sampling. */
  uint8_t canBeSampled = 0;

  /** Whether the alpha channel of the format of this RT needs to be manually
   * handled as part of the blend state. */
  uint8_t hasAlphaSwizzle = 0;
};

struct D3D9VBSlotTracking {
  /* D3D9 allows using 16 vertex buffers ('streams'). */

  /** Whether there's a vertex buffer bound to the slot */
  uint16_t bound = 0;

  /** Whether the vertex buffer at each slot needs to be uploaded at draw time
   */
  uint16_t needsUpload = 0;

  /** Whether the vertex buffer for each slot gets copied at draw time to act
   * like DrawUP */
  uint16_t uploadPerDraw = 0;

  /** Whether instancing is enabled for each slot */
  uint16_t instanced = 0;
};

struct War3ShadowLifecycleDiagnostics {
  uint64_t requestedResetSerial = 0u;
  uint64_t appliedResetSerial = 0u;
  uint64_t currentMapEpoch = 0u;
  uint64_t appliedFrameSerial = 0u;
  uint64_t quarantinedRetireSerial = 0u;
  uint64_t completedRetireSerial = 0u;
  uint64_t retiredSessionCount = 0u;
  uint64_t retiredSessionEntryCount = 0u;
  uint64_t retiredSessionAllocatorBytes = 0u;
  uint64_t retiredSessionCachedGpuLogicalBytes = 0u;
  uint64_t retiredSessionCpuOwnedBytes = 0u;
  uint64_t retiredSessionOldestRetireSerial = 0u;
  uint64_t retiredSessionCollectedCount = 0u;
  uint64_t retiredLastMapEpoch = 0u;
  uint64_t pendingProducerRejectCount = 0u;
  uint32_t transitionState = 0u; // 0=ready, 1=requested, 2=quarantined
  uint32_t producerReady = 0u;
};

class D3D9DeviceEx final : public ComObjectClamp<IDirect3DDevice9Ex> {
  constexpr static uint32_t DefaultFrameLatency = 3;
  constexpr static uint32_t MaxFrameLatency = 20;

  constexpr static uint32_t MinFlushIntervalUs = 750;
  constexpr static uint32_t IncFlushIntervalUs = 250;
  constexpr static uint32_t MaxPendingSubmits = 6;

  constexpr static uint32_t NullStreamIdx = caps::MaxStreams;

  constexpr static VkDeviceSize StagingBufferSize = 4ull << 20;

  friend class D3D9SwapChainEx;
  friend struct D3D9WindowContext;
  friend class D3D9ConstantBuffer;
  friend class D3D9UserDefinedAnnotation;
  friend class DxvkD3D8Bridge;
  friend D3D9VkInteropDevice;

public:
  D3D9DeviceEx(D3D9InterfaceEx *pParent, D3D9Adapter *pAdapter,
               D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
               Rc<DxvkDevice> dxvkDevice);

  ~D3D9DeviceEx();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject);

  HRESULT STDMETHODCALLTYPE TestCooperativeLevel();

  UINT STDMETHODCALLTYPE GetAvailableTextureMem();

  HRESULT STDMETHODCALLTYPE EvictManagedResources();

  HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9 **ppD3D9);

  HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9 *pCaps);

  HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain,
                                           D3DDISPLAYMODE *pMode);

  HRESULT STDMETHODCALLTYPE
  GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters);

  HRESULT STDMETHODCALLTYPE SetCursorProperties(
      UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap);

  void STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags);

  BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow);

  HRESULT STDMETHODCALLTYPE
  CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters,
                            IDirect3DSwapChain9 **ppSwapChain);

  HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain,
                                         IDirect3DSwapChain9 **pSwapChain);

  UINT STDMETHODCALLTYPE GetNumberOfSwapChains();

  HRESULT STDMETHODCALLTYPE
  Reset(D3DPRESENT_PARAMETERS *pPresentationParameters);

  HRESULT STDMETHODCALLTYPE Present(const RECT *pSourceRect,
                                    const RECT *pDestRect,
                                    HWND hDestWindowOverride,
                                    const RGNDATA *pDirtyRegion);

  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain, UINT iBackBuffer,
                                          D3DBACKBUFFER_TYPE Type,
                                          IDirect3DSurface9 **ppBackBuffer);

  HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain,
                                            D3DRASTER_STATUS *pRasterStatus);

  HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs);

  void STDMETHODCALLTYPE SetGammaRamp(UINT iSwapChain, DWORD Flags,
                                      const D3DGAMMARAMP *pRamp);

  void STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp);

  HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels,
                                          DWORD Usage, D3DFORMAT Format,
                                          D3DPOOL Pool,
                                          IDirect3DTexture9 **ppTexture,
                                          HANDLE *pSharedHandle);

  HRESULT STDMETHODCALLTYPE CreateVolumeTexture(
      UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
      D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture,
      HANDLE *pSharedHandle);

  HRESULT STDMETHODCALLTYPE CreateCubeTexture(
      UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
      IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle);

  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(
      UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
      IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle);

  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(
      UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
      IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle);

  HRESULT STDMETHODCALLTYPE CreateRenderTarget(
      UINT Width, UINT Height, D3DFORMAT Format,
      D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable,
      IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle);

  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(
      UINT Width, UINT Height, D3DFORMAT Format,
      D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard,
      IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle);

  HRESULT STDMETHODCALLTYPE UpdateSurface(
      IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect,
      IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint);

  HRESULT STDMETHODCALLTYPE
  UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture,
                IDirect3DBaseTexture9 *pDestinationTexture);

  HRESULT STDMETHODCALLTYPE GetRenderTargetData(
      IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface);

  HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain,
                                               IDirect3DSurface9 *pDestSurface);

  HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *pSourceSurface,
                                        const RECT *pSourceRect,
                                        IDirect3DSurface9 *pDestSurface,
                                        const RECT *pDestRect,
                                        D3DTEXTUREFILTERTYPE Filter);

  HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *pSurface,
                                      const RECT *pRect, D3DCOLOR Color);

  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(
      UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
      IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle);

  HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex,
                                            IDirect3DSurface9 *pRenderTarget);

  HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex,
                                            IDirect3DSurface9 **ppRenderTarget);

  HRESULT STDMETHODCALLTYPE
  SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil);

  HRESULT STDMETHODCALLTYPE
  GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface);

  HRESULT STDMETHODCALLTYPE BeginScene();

  HRESULT STDMETHODCALLTYPE EndScene();

  HRESULT STDMETHODCALLTYPE Clear(DWORD Count, const D3DRECT *pRects,
                                  DWORD Flags, D3DCOLOR Color, float Z,
                                  DWORD Stencil);

  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State,
                                         const D3DMATRIX *pMatrix);

  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State,
                                         D3DMATRIX *pMatrix);

  HRESULT STDMETHODCALLTYPE MultiplyTransform(
      D3DTRANSFORMSTATETYPE TransformState, const D3DMATRIX *pMatrix);

  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *pViewport);

  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *pViewport);

  HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *pMaterial);

  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *pMaterial);

  HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, const D3DLIGHT9 *pLight);

  HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9 *pLight);

  HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable);

  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL *pEnable);

  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, const float *pPlane);

  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float *pPlane);

  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State,
                                           DWORD Value);

  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State,
                                           DWORD *pValue);

  HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type,
                                             IDirect3DStateBlock9 **ppSB);

  HRESULT STDMETHODCALLTYPE BeginStateBlock();

  HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **ppSB);

  HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9 *pClipStatus);

  HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *pClipStatus);

  HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage,
                                       IDirect3DBaseTexture9 **ppTexture);

  HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage,
                                       IDirect3DBaseTexture9 *pTexture);

  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage,
                                                 D3DTEXTURESTAGESTATETYPE Type,
                                                 DWORD *pValue);

  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage,
                                                 D3DTEXTURESTAGESTATETYPE Type,
                                                 DWORD Value);

  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler,
                                            D3DSAMPLERSTATETYPE Type,
                                            DWORD *pValue);

  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler,
                                            D3DSAMPLERSTATETYPE Type,
                                            DWORD Value);

  HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *pNumPasses);

  HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber,
                                              const PALETTEENTRY *pEntries);

  HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber,
                                              PALETTEENTRY *pEntries);

  HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber);

  HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber);

  HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *pRect);

  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *pRect);

  HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware);

  BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing();

  HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments);

  float STDMETHODCALLTYPE GetNPatchMode();

  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType,
                                          UINT StartVertex,
                                          UINT PrimitiveCount);

  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(
      D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
      UINT NumVertices, UINT StartIndex, UINT PrimitiveCount);

  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,
                                            UINT PrimitiveCount,
                                            const void *pVertexStreamZeroData,
                                            UINT VertexStreamZeroStride);

  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(
      D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices,
      UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat,
      const void *pVertexStreamZeroData, UINT VertexStreamZeroStride);

  HRESULT STDMETHODCALLTYPE
  ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
                  IDirect3DVertexBuffer9 *pDestBuffer,
                  IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags);

  HRESULT STDMETHODCALLTYPE
  CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements,
                          IDirect3DVertexDeclaration9 **ppDecl);

  HRESULT STDMETHODCALLTYPE
  SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl);

  HRESULT STDMETHODCALLTYPE
  GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl);

  HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF);

  HRESULT STDMETHODCALLTYPE GetFVF(DWORD *pFVF);

  HRESULT STDMETHODCALLTYPE
  CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader);

  HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *pShader);

  HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **ppShader);

  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT StartRegister,
                                                     const float *pConstantData,
                                                     UINT Vector4fCount);

  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister,
                                                     float *pConstantData,
                                                     UINT Vector4fCount);

  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT StartRegister,
                                                     const int *pConstantData,
                                                     UINT Vector4iCount);

  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister,
                                                     int *pConstantData,
                                                     UINT Vector4iCount);

  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT StartRegister,
                                                     const BOOL *pConstantData,
                                                     UINT BoolCount);

  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister,
                                                     BOOL *pConstantData,
                                                     UINT BoolCount);

  HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber,
                                            IDirect3DVertexBuffer9 *pStreamData,
                                            UINT OffsetInBytes, UINT Stride);

  HRESULT STDMETHODCALLTYPE
  GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData,
                  UINT *pOffsetInBytes, UINT *pStride);

  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber,
                                                UINT Setting);

  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber,
                                                UINT *pSetting);

  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *pIndexData);

  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **ppIndexData);

  HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *pFunction,
                                              IDirect3DPixelShader9 **ppShader);

  HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *pShader);

  HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **ppShader);

  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister,
                                                    const float *pConstantData,
                                                    UINT Vector4fCount);

  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister,
                                                    float *pConstantData,
                                                    UINT Vector4fCount);

  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister,
                                                    const int *pConstantData,
                                                    UINT Vector4iCount);

  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister,
                                                    int *pConstantData,
                                                    UINT Vector4iCount);

  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister,
                                                    const BOOL *pConstantData,
                                                    UINT BoolCount);

  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister,
                                                    BOOL *pConstantData,
                                                    UINT BoolCount);

  HRESULT STDMETHODCALLTYPE
  DrawRectPatch(UINT Handle, const float *pNumSegs,
                const D3DRECTPATCH_INFO *pRectPatchInfo);

  HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, const float *pNumSegs,
                                         const D3DTRIPATCH_INFO *pTriPatchInfo);

  HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle);

  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type,
                                        IDirect3DQuery9 **ppQuery);

  // Ex Methods

  HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT width, UINT height,
                                                     float *rows,
                                                     float *columns);

  HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9 *pSrc,
                                         IDirect3DSurface9 *pDst,
                                         IDirect3DVertexBuffer9 *pSrcRectDescs,
                                         UINT NumRects,
                                         IDirect3DVertexBuffer9 *pDstRectDescs,
                                         D3DCOMPOSERECTSOP Operation,
                                         int Xoffset, int Yoffset);

  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *pPriority);

  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority);

  HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT iSwapChain);

  HRESULT STDMETHODCALLTYPE CheckResourceResidency(
      IDirect3DResource9 **pResourceArray, UINT32 NumResources);

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency);

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency);

  HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND hDestinationWindow);

  HRESULT STDMETHODCALLTYPE PresentEx(const RECT *pSourceRect,
                                      const RECT *pDestRect,
                                      HWND hDestWindowOverride,
                                      const RGNDATA *pDirtyRegion,
                                      DWORD dwFlags);

  HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(
      UINT Width, UINT Height, D3DFORMAT Format,
      D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable,
      IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage);

  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(
      UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
      IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage);

  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(
      UINT Width, UINT Height, D3DFORMAT Format,
      D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard,
      IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage);

  HRESULT STDMETHODCALLTYPE
  ResetEx(D3DPRESENT_PARAMETERS *pPresentationParameters,
          D3DDISPLAYMODEEX *pFullscreenDisplayMode);

  HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT iSwapChain,
                                             D3DDISPLAYMODEEX *pMode,
                                             D3DDISPLAYROTATION *pRotation);

  HRESULT STDMETHODCALLTYPE
  CreateAdditionalSwapChainEx(D3DPRESENT_PARAMETERS *pPresentationParameters,
                              const D3DDISPLAYMODEEX *pFullscreenDisplayMode,
                              IDirect3DSwapChain9 **ppSwapChain);

  /**
   * @brief Sets the given sampler state
   *
   * @param StateSampler Sampler index (according to our internal way of storing
   * samplers)
   * @param Type Sampler state type to change
   * @param Value State value
   */
  HRESULT SetStateSamplerState(DWORD StateSampler, D3DSAMPLERSTATETYPE Type,
                               DWORD Value);

  /**
   * @brief Sets the given sampler texture
   *
   * @param StateSampler Sampler index (according to our internal way of storing
   * samplers)
   * @param pTexture Texture to use
   */
  HRESULT SetStateTexture(DWORD StateSampler, IDirect3DBaseTexture9 *pTexture);

  /**
   * @brief Sets the transform for the given sampler
   *
   * @param idx Sampler index (according to our internal way of storing
   * samplers)
   * @param pMatrix Transform matrix
   */
  HRESULT SetStateTransform(uint32_t idx, const D3DMATRIX *pMatrix);

  /**
   * @brief Sets the fixed function texture processing state
   *
   * @param Stage Sampler index (according to our internal way of storing
   * samplers)
   * @param Type Fixed function texture stage type
   * @param Value Value for the state
   */
  HRESULT SetStateTextureStageState(DWORD Stage,
                                    D3D9TextureStageStateTypes Type,
                                    DWORD Value);

  VkPipelineStageFlags GetEnabledShaderStages() const {
    return m_dxvkDevice->getShaderPipelineStages();
  }

  /**
   * \brief Returns whether the Vulkan device supports the required features for
   * ProcessVertices
   */
  bool SupportsSWVP();

  bool SupportsVCacheQuery() const;

  const Rc<DxvkDevice> &GetDXVKDevice() const { return m_dxvkDevice; }

  bool IsExtended();

  HWND GetWindow();

  const Rc<DxvkDevice> &GetDXVKDevice() { return m_dxvkDevice; }

  D3D9_VK_FORMAT_MAPPING LookupFormat(D3D9Format Format) const;

  const DxvkFormatInfo *UnsupportedFormatInfo(D3D9Format Format) const;

  bool WaitForResource(const DxvkPagedResource &Resource,
                       uint64_t SequenceNumber, DWORD MapFlags);

  /**
   * \brief Locks a subresource of an image
   *
   * \param [in] Subresource The subresource of the image to lock
   * \param [out] pLockedBox The returned locked box of the image, containing
   * data ptr and strides
   * \param [in] pBox The region of the subresource to lock. This offsets the
   * returned data ptr
   * \param [in] Flags The D3DLOCK_* flags to lock the image with
   * \returns \c D3D_OK if the parameters are valid or D3DERR_INVALIDCALL if it
   * fails.
   */
  HRESULT LockImage(D3D9CommonTexture *pResource, UINT Face, UINT Mip,
                    D3DLOCKED_BOX *pLockedBox, const D3DBOX *pBox, DWORD Flags);

  uint32_t CalcImageLockOffset(uint32_t SlicePitch, uint32_t RowPitch,
                               const DxvkFormatInfo *FormatInfo,
                               const D3DBOX *pBox);

  /**
   * \brief Unlocks a subresource of an image
   *
   * Passthrough to device unlock.
   * \param [in] Subresource The subresource of the image to unlock
   * \returns \c D3D_OK if the parameters are valid or D3DERR_INVALIDCALL if it
   * fails.
   */
  HRESULT UnlockImage(D3D9CommonTexture *pResource, UINT Face, UINT MipLevel);

  /**
   * \brief Uploads the given texture subresource from its local system memory
   * copy.
   */
  HRESULT FlushImage(D3D9CommonTexture *pResource, UINT Subresource);

  /**
   * \brief Copies the given part of a texture from the local system memory copy
   * of the source texture to the image of the destination texture.
   */
  void UpdateTextureFromBuffer(D3D9CommonTexture *pDestTexture,
                               D3D9CommonTexture *pSrcTexture,
                               UINT DestSubresource, UINT SrcSubresource,
                               VkOffset3D SrcOffset, VkExtent3D SrcExtent,
                               VkOffset3D DestOffset);

  void EmitGenerateMips(D3D9CommonTexture *pResource);

  HRESULT LockBuffer(D3D9CommonBuffer *pResource, UINT OffsetToLock,
                     UINT SizeToLock, void **ppbData, DWORD Flags,
                     uintptr_t OwnerIdentity);

  /**
   * \brief Uploads the given buffer from its local system memory copy.
   */
  HRESULT FlushBuffer(D3D9CommonBuffer *pResource);

  HRESULT UnlockBuffer(D3D9CommonBuffer *pResource,
                       uintptr_t OwnerIdentity);

  /**
   * @brief Uploads data from D3DPOOL_SYSMEM + D3DUSAGE_DYNAMIC buffers and
   * binds the temporary buffers.
   *
   * @param FirstVertexIndex The first vertex
   * @param NumVertices The number of vertices that are accessed. If this is 0,
   * the vertex buffer binding will not be modified.
   * @param FirstIndex The first index
   * @param NumIndices The number of indices that will be drawn. If this is 0,
   * the index buffer binding will not be modified.
   */
  void UploadPerDrawData(UINT &FirstVertexIndex, UINT NumVertices,
                         UINT &FirstIndex, UINT NumIndices,
                         INT &BaseVertexIndex, bool *pDynamicVBOs,
                         bool *pDynamicIBO);

  void SetupFPU();

  int64_t DetermineInitialTextureMemory();

  void CreateConstantBuffers();

  void SynchronizeCsThread(uint64_t SequenceNumber);

  void Flush();
  void FlushAndSync9On12();

  void BeginFrame(Rc<DxvkLatencyTracker> LatencyTracker, uint64_t FrameId);
  void EndFrame(Rc<DxvkLatencyTracker> LatencyTracker);

  void UpdateActiveRTs(uint32_t index);

  template <uint32_t Index> void UpdateAnyColorWrites();

  void UpdateTextureBitmasks(uint32_t index, DWORD combinedUsage);

  void UpdateActiveHazardsRT(uint32_t texMask);

  void UpdateActiveHazardsDS(uint32_t texMask);

  void EmitFeedbackLoopBarriers();

  void UpdateActiveFetch4(uint32_t stateSampler);

  /**
   * @brief Sets the mismatching texture type bits for all samplers if
   * necessary.
   *
   * This function will check all samplers the shader uses and set the  set the
   * mismatching texture type bit for the given sampler if it does not match the
   * texture type expected by the respective shader.
   *
   * It will *not* unset the bit if the texture type does match.
   *
   * @param stateSampler Sampler index (according to our internal way of storing
   * samplers)
   */

  /**
   * @brief Sets the mismatching texture type bits for all samplers if
   * necessary.
   *
   * This function will check all samplers the shader uses and set the  set the
   * mismatching texture type bit for the given sampler if it does not match the
   * texture type expected by the shader.
   *
   * @param shader The shader
   * @param shaderSamplerMask Mask of all samplers that the shader uses
   * (according to our internal way of storing samplers)
   * @param shaderSamplerOffset First index of the shader's samplers according
   * to our internal way of storing samplers. Used to transform the sampler
   * indices that are relative to the entire pipeline to ones relative to the
   * shader.
   */
  void UpdateTextureTypeMismatchesForShader(const D3D9CommonShader *shader,
                                            uint32_t shaderSamplerMask,
                                            uint32_t shaderSamplerOffset);

  /**
   * @brief Sets the mismatching texture type bit for the given sampler.
   *
   * This function will set the mismatching texture type bit for the given
   * sampler if it does not match the texture type expected by the respective
   * shader.
   *
   * It will *not* unset the bit if the texture type does match.
   *
   * @param stateSampler Sampler index (according to our internal way of storing
   * samplers)
   */
  void UpdateTextureTypeMismatchesForTexture(uint32_t stateSampler);

  void UploadManagedTexture(D3D9CommonTexture *pResource);

  void UploadManagedTextures(uint32_t mask);

  void GenerateTextureMips(uint32_t mask);

  void MarkTextureMipsDirty(D3D9CommonTexture *pResource);

  void MarkTextureMipsUnDirty(D3D9CommonTexture *pResource);

  void MarkTextureUploaded(D3D9CommonTexture *pResource);

  void UpdatePointMode(bool pointList);

  void UpdateFog();

  void BindFramebuffer();

  void BindViewportAndScissor();

  inline bool IsNVDepthBoundsTestEnabled() {
    // NVDB is not supported by D3D8
    if (unlikely(m_isD3D8Compatible))
      return false;

    return m_state.renderStates[D3DRS_ADAPTIVETESS_X] ==
           uint32_t(D3D9Format::NVDB);
  }

  void UpdateAlphaToCoverangeAndAlphaTest();

  inline bool IsZTestEnabled() {
    return m_state.renderStates[D3DRS_ZENABLE] &&
           m_state.depthStencil != nullptr;
  }

  void BindMultiSampleState();

  void BindBlendState();

  void BindBlendFactor();

  void BindDepthStencilState();

  void BindDepthStencilReference();

  void BindRasterizerState();

  void BindDepthBias();

  inline void
  UploadSoftwareConstantSet(const D3D9ShaderConstantsVSSoftware &Src,
                            const D3D9ConstantLayout &Layout);

  inline void *CopySoftwareConstants(D3D9ConstantBuffer &dstBuffer,
                                     const void *src, uint32_t size);

  template <DxsoProgramType ShaderStage, typename HardwareLayoutType,
            typename SoftwareLayoutType, typename ShaderType>
  inline void UploadConstantSet(const SoftwareLayoutType &Src,
                                const D3D9ConstantLayout &Layout,
                                const ShaderType &Shader);

  template <DxsoProgramType ShaderStage> void UploadConstants();

  void UpdateClipPlanes();

  /**
   * \brief Updates the push constant data at the given offset with data from
   * the specified pointer.
   *
   * \param Offset Offset at which the push constant data gets written.
   * \param Length Length of the push constant data to write.
   * \param pData Push constant data
   */
  template <uint32_t Offset, uint32_t Length>
  void UpdatePushConstant(const void *pData);

  /**
   * \brief Updates the specified push constant based on the device state.
   *
   * \param Item Render state push constant to update
   */
  template <D3D9RenderStateItem Item> void UpdatePushConstant();

  void BindSampler(DWORD Sampler);

  void BindTexture(DWORD SamplerSampler);

  void UnbindTextures(uint32_t mask);

  void UndirtySamplers(uint32_t mask);

  void UndirtyTextures(uint32_t usedMask);

  void MarkTextureBindingDirty(IDirect3DBaseTexture9 *texture);

  HRESULT STDMETHODCALLTYPE SetRenderTargetInternal(
      DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget);

  D3D9DrawInfo GenerateDrawInfo(D3DPRIMITIVETYPE PrimitiveType,
                                UINT PrimitiveCount, UINT InstanceCount);

  uint32_t GetInstanceCount() const;

  void PrepareDraw(D3DPRIMITIVETYPE PrimitiveType, bool UploadVBOs,
                   bool UploadIBOs);

  void EnsureSamplerLimit();

  template <DxsoProgramType ShaderStage>
  void BindShader(const D3D9CommonShader *pShaderModule);

  template <DxsoProgramType ShaderStage> void BindFFUbershader();

  void BindInputLayout();

  void BindVertexBuffer(UINT Slot, D3D9VertexBuffer *pBuffer, UINT Offset,
                        UINT Stride);

  void BindIndices();

  D3D9DeviceLock LockDevice() { return m_multithread.AcquireLock(); }

  const D3D9Options *GetOptions() const { return &m_d3d9Options; }

  Direct3DState9 *GetRawState() { return &m_state; }

  void Begin(D3D9Query *pQuery);
  void End(D3D9Query *pQuery);

  void SetVertexBoolBitfield(uint32_t idx, uint32_t mask, uint32_t bits);
  void SetPixelBoolBitfield(uint32_t idx, uint32_t mask, uint32_t bits);

  void ConsiderFlush(GpuFlushType FlushType);

  bool ChangeReportedMemory(int64_t delta) {
    if (IsExtended())
      return true;

    int64_t availableMemory = m_availableMemory.fetch_add(delta);

    return !m_d3d9Options.memoryTrackTest || availableMemory >= delta;
  }

  void ResolveZ();

  void TransitionImage(D3D9CommonTexture *pResource, VkImageLayout NewLayout);

  void TransformImage(D3D9CommonTexture *pResource,
                      const VkImageSubresourceRange *pSubresources,
                      VkImageLayout OldLayout, VkImageLayout NewLayout);

  const D3D9ConstantLayout &GetVertexConstantLayout() {
    return m_consts[DxsoProgramType::VertexShader].layout;
  }
  const D3D9ConstantLayout &GetPixelConstantLayout() {
    return m_consts[DxsoProgramType::PixelShader].layout;
  }

  void ResetState(D3DPRESENT_PARAMETERS *pPresentationParameters);
  HRESULT ResetSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters,
                         D3DDISPLAYMODEEX *pFullscreenDisplayMode);

  HRESULT InitialReset(D3DPRESENT_PARAMETERS *pPresentationParameters,
                       D3DDISPLAYMODEEX *pFullscreenDisplayMode);
  bool TryRemapWindowedLogicalViewport(const D3DVIEWPORT9& viewport,
                                       D3DVIEWPORT9& remappedViewport) const;
  bool TryRemapWindowedLogicalScissor(const RECT& rect,
                                      RECT& remappedRect) const;
  VkExtent2D GetFrameMaxViewportExtent() const {
    return VkExtent2D{m_frameMaxViewportRight, m_frameMaxViewportBottom};
  }
  void ResetFrameMaxViewportExtent() {
    m_frameMaxViewportRight = 0u;
    m_frameMaxViewportBottom = 0u;
  }

  /**
   * \brief Returns the allocator used for unmappable system memory texture data
   */
  D3D9MemoryAllocator *GetAllocator() { return &m_memoryAllocator; }

  /**
   * \brief Gets the pointer of the system memory copy of the texture
   *
   * Also tracks the texture if it is unmappable.
   */
  void *MapTexture(D3D9CommonTexture *pTexture, UINT Subresource);

  /**
   * \brief Moves the texture to the front of the LRU list of mapped textures
   */
  void TouchMappedTexture(D3D9CommonTexture *pTexture);

  /**
   * \brief Removes the texture from the LRU list of mapped textures
   */
  void RemoveMappedTexture(D3D9CommonTexture *pTexture);

  /**
   * \brief Returns whether the device is currently recording a StateBlock
   */
  bool ShouldRecord() const { return m_recorder != nullptr; }

  bool IsD3D8Compatible() const { return m_isD3D8Compatible; }

  // Device Lost
  bool IsDeviceLost() const {
    return m_deviceLostState != D3D9DeviceLostState::Ok ||
           IsVulkanDeviceLostFailStop();
  }

  /**
   * \brief Tests the irreversible Vulkan device-loss latch
   *
   * Unlike the legacy D3D9 focus-loss state, a lost Vulkan logical device
   * cannot be recovered by Reset. This query is lock-free so command emission
   * can stop as soon as the submission queue publishes VK_ERROR_DEVICE_LOST.
   */
  bool IsVulkanDeviceLostFailStop() const {
    return m_vkDeviceLostFailStop.load(std::memory_order_acquire) ||
           (m_dxvkDevice != nullptr &&
            m_dxvkDevice->getDeviceStatus() == VK_ERROR_DEVICE_LOST);
  }

  /**
   * \brief Latches and records the first Vulkan device-loss observation
   *
   * \param origin Stable diagnostic label for the observing front-end path.
   * \returns true if the Vulkan logical device is lost.
   */
  bool CheckVulkanDeviceLostFailStop(const char* origin);

  void NotifyFullscreen(HWND window, bool fullscreen);
  void NotifyWindowActivated(HWND window, bool activated);

  /**
   * \brief Increases the amount of D3DPOOL_DEFAULT resources that block a
   * device reset
   */
  void IncrementLosableCounter() { m_losableResourceCounter++; }

  /**
   * \brief Decreases the amount of D3DPOOL_DEFAULT resources that block a
   * device reset
   */
  void DecrementLosableCounter() { m_losableResourceCounter--; }

  /**
   * \brief Returns whether the device is configured to only support vertex
   * processing.
   */
  bool CanOnlySWVP() const {
    return m_behaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING;
  }

  /**
   * \brief Returns whether the device can be set to do software vertex
   * processing. It may also be set up to only support software vertex
   * processing.
   */
  bool CanSWVP() const {
    return m_behaviorFlags & (D3DCREATE_MIXED_VERTEXPROCESSING |
                              D3DCREATE_SOFTWARE_VERTEXPROCESSING);
  }

  /**
   * \brief Returns whether or not the device is currently set to do software
   * vertex processing.
   */
  bool IsSWVP() const { return m_isSWVP; }

  /**
   * \brief Returns the number of vertex shader modules generated for fixed
   * function state.
   */
  UINT GetFixedFunctionVSCount() const { return m_ffModules.GetVSCount(); }

  /**
   * \brief Returns the number of fragment shader modules generated for fixed
   * function state.
   */
  UINT GetFixedFunctionFSCount() const { return m_ffModules.GetFSCount(); }

  /**
   * \brief Returns the number of shader modules generated for ProcessVertices.
   */
  UINT GetSWVPShaderCount() const { return m_swvpEmulator.GetShaderCount(); }

  void InjectCsChunk(DxvkCsChunkRef &&Chunk, bool Synchronize);

  template <typename Fn> void InjectCs(Fn &&Command) {
    if (unlikely(IsVulkanDeviceLostFailStop()))
      return;

    auto chunk = AllocCsChunk();
    chunk->push(std::move(Command));

    InjectCsChunk(std::move(chunk), false);
  }

  DxvkCsChunkRef AllocCsChunk() {
    DxvkCsChunk *chunk = m_csChunkPool.allocChunk(DxvkCsChunkFlag::SingleUse);
    return DxvkCsChunkRef(chunk, &m_csChunkPool);
  }

  bool Is9On12Device() const { return m_d3d9On12Args.Enable9On12; }

private:
  template <bool AllowFlush = true, typename Cmd> void EmitCs(Cmd &&command) {
    if (unlikely(IsVulkanDeviceLostFailStop()))
      return;

    if (unlikely(!m_csChunk->push(command))) {
      EmitCsChunk(std::move(m_csChunk));
      m_csChunk = AllocCsChunk();

      if constexpr (AllowFlush)
        ConsiderFlush(GpuFlushType::ImplicitWeakHint);

      m_csChunk->push(command);
    }
  }

  void EmitCsChunk(DxvkCsChunkRef &&chunk);

  void FlushCsChunk() {
    if (unlikely(IsVulkanDeviceLostFailStop())) {
      // Drop CPU-side commands which can no longer be submitted. Releasing the
      // chunk here also releases resource references retained by those lambdas.
      m_csChunk = AllocCsChunk();
      return;
    }

    if (likely(!m_csChunk->empty())) {
      EmitCsChunk(std::move(m_csChunk));
      m_csChunk = AllocCsChunk();
    }
  }

  /**
   * \brief Queries current reset counter
   * Used for the deferred surface creation workaround.
   * (Device Reset detection for D3D9SwapChainEx::Present)
   */
  uint32_t GetResetCounter() { return m_resetCtr; }

  template <bool Synchronize9On12> void ExecuteFlush();

  void DetermineConstantLayouts(bool canSWVP);

  // War3 渲染管线插入点检测（BeforeUi）
  void War3MaybeInsertBeforeUi(bool forceFrameEnd = false);

  // Applies a coalesced map-session reset only from the Present render-thread
  // boundary. Returns true when the current Arena generation was sealed and
  // its dedicated completion signal was emitted by this call.
  bool War3ApplyShadowMapEpochResetAtPresent(uint64_t retireSerial);
  void War3ResetShadowSessionState(uint64_t retireSerial);
  void War3CollectRetiredShadowSessions(uint64_t completedSerial);
  void War3RefreshRetiredShadowSessionDiagnostics();

  // War3：捕获世界相机与投影（用于 CSM/后处理）
  void War3RecordWorldCamera();

  // War3：判断当前 viewport 是否为“主世界视口”
  // 用于避免在头像/小地图等小 viewport 中误捕获相机，导致阴影随视角乱跳。
  bool War3IsLikelyMainWorldViewport() const;

  struct War3ShadowDeclInfo;

  // War3：为 fixed-function vertex blending 捕获当前帧的 world matrix palette
  // 说明：
  // - D3D9 顶点混合会在 draw 前更新 D3DTS_WORLDMATRIX(i)；
  // - shadow caster 重放时必须使用当时那一套
  // palette，否则会出现“阴影残缺/错位/乱飞”。
  uint32_t War3GetOrCreateShadowMatrixPalette();
  uint32_t War3GetOrCreateShadowMatrixPaletteFromData(
      const Matrix4* matrices,
      uint32_t matrixCount,
      uint64_t knownHash = 0u);

  // War3：缓存顶点声明解析结果，减少逐 draw 扫描开销
  const War3ShadowDeclInfo &War3GetShadowDeclInfo(D3D9VertexDecl *decl);

  // War3：捕获可投影阴影的 indexed
  // draw（Terrain/Units/Buildings/Destructibles）
  void War3TryCaptureShadowCasterDrawIndexed(D3DPRIMITIVETYPE PrimitiveType,
                                             INT BaseVertexIndex,
                                             UINT MinVertexIndex,
                                             UINT NumVertices,
                                             UINT StartIndex, UINT IndexCount,
                                             bool DynamicSysmemVBOs,
                                             bool DynamicSysmemIBO,
                                             const war3::gpu_skin::GpuSkinResolvedDraw*
                                                 gpuSkinResolved);

  // War3：捕获可投影阴影的 non-indexed draw（主要用于 Terrain 等）
  void War3TryCaptureShadowCasterDrawNonIndexed(D3DPRIMITIVETYPE PrimitiveType,
                                                UINT StartVertex,
                                                UINT VertexCount,
                                                bool DynamicSysmemVBOs);

  // War3：材质覆盖（自定义 HLSL）辅助结构
  struct War3ConstRange {
    uint32_t start = 0;
    uint32_t count = 0;
  };

  struct War3MaterialOverrideBackup {
    Com<D3D9VertexShader, false> prevVs;
    Com<D3D9PixelShader, false> prevPs;
    std::vector<std::pair<D3DRENDERSTATETYPE, DWORD>> renderStates;
    std::vector<Vector4> vsConsts;
    std::vector<Vector4> psConsts;
    std::vector<War3ConstRange> vsRanges;
    std::vector<War3ConstRange> psRanges;
  };

  enum class War3MaterialKind : uint8_t {
    World = 0,
    Outline = 1,
    PostProcess = 2,
  };

  bool War3ShouldOverrideWorldMaterial() const;
  bool War3ShouldOverridePostProcessMaterial() const;
  bool War3ShouldDrawOutline() const;
  bool War3ShouldDrawDebugOverlay() const;
  bool War3ApplyMaterialOverride(war3::War3Material *material,
                                 War3MaterialKind kind,
                                 War3MaterialOverrideBackup &backup);
  void War3RestoreMaterialOverride(const War3MaterialOverrideBackup &backup);
  void War3UpdateMaterialUniforms(war3::War3Material *material,
                                  War3MaterialKind kind);
  void War3DrawDebugOverlayTriangle();

public:
  // War3：获取渲染管线设置（供 ImGui/调试使用）
  War3RenderPipeline *GetWar3Pipeline() const { return m_war3Pipeline; }

  // GPU skin hooks are process-lifetime, while the device and map epochs are
  // explicit. Disabled mode never allocates a manager or GPU resources.
  void War3AttachGpuSkinNativeBridge(uintptr_t gameBase);
  // Process-global CPU semantic state has no device owner. This shared reset
  // is safe for a new-device handoff and for map unload with no active device;
  // it deliberately does not touch any GPU-owned resource.
  static uint64_t War3ResetCpuSemanticMapSession(
      uint64_t* outTombstoneSerial = nullptr);
  void War3RequestShadowMapEpochReset();
  War3ShadowLifecycleDiagnostics QueryWar3ShadowLifecycleDiagnostics() const;
  bool War3ResetGpuSkinBridgeForTest(bool deviceEpoch);
  bool War3LogGpuSkinDiagnosticsForTest(
      bool requireQuiescent = false, bool* outQuiescent = nullptr);

private:
  static bool War3GpuSkinQueryFlushRequest(
      void* userData,
      const war3::gpu_skin::NativeFlushObservation& observation,
      war3::gpu_skin::FlushRequest* request);
  static war3::gpu_skin::GpuSkinHostSubmitResult
  War3GpuSkinSubmitFlushBatch(
      void* userData,
      const war3::gpu_skin::GpuSkinPendingBatch& batch);
  static bool War3GpuSkinPreflightNativeBypass(
      void* userData,
      const war3::gpu_skin::GpuSkinNativeBypassHostRequest& request,
      war3::gpu_skin::NativeVertexOutputProof* outputProof);
  static bool War3GpuSkinResolveNativeCpuRewriteOutputProof(
      void* userData,
      const war3::gpu_skin::NativeUploadObservation& observation,
      war3::gpu_skin::NativeCpuRewriteOutputProof* outputProof);
  enum class War3GpuSkinDipDisposition : uint8_t {
    Unresolved = 0u,
    ProvenCpuOnly = 1u,
  };
  war3::gpu_skin::GpuSkinResolvedDraw War3NotifyGpuSkinDip(
      D3DPRIMITIVETYPE primitiveType, INT baseVertexIndex,
      UINT minVertexIndex, UINT numVertices, UINT startIndex,
      UINT primitiveCount, uint32_t flags, bool outlineRequested = false,
      War3GpuSkinDipDisposition* disposition = nullptr);
  void War3ScheduleGpuSkinParity(
      const war3::gpu_skin::GpuSkinResolvedDraw& resolved,
      INT baseVertexIndex);
  void War3PollGpuSkinParity();
  void War3LogGpuSkinDiagnostics(bool force);
  void War3RetireGpuSkinFrameBatches();
  void War3ResetGpuSkinMapEpoch();
  void War3ResetGpuSkinDeviceEpoch();
  void War3RetryGpuSkinDeviceRebind();
  void War3RequestShadowDeviceEpochTransition(uint64_t deviceEpoch);
  bool War3ApplyShadowDeviceEpochTransitionAtPresent(uint64_t retireSerial);
  void War3QuarantineShadowSessionAtPresent(uint64_t retireSerial);
  void War3InvalidateShadowReceiverEpochOnCs(uint64_t mapEpoch,
                                             uint64_t deviceEpoch);
  bool War3GpuSkinDeviceReady() const;

  /**
   * \brief Allocates buffer memory for DrawPrimitiveUp draws
   */
  D3D9BufferSlice AllocUPBuffer(VkDeviceSize size);

  /**
   * \brief Allocates buffer memory for resource uploads
   */
  D3D9BufferSlice AllocStagingBuffer(VkDeviceSize size);

  /**
   * \brief Waits until the amount of used staging memory is below a certain
   * threshold.
   */
  void ThrottleAllocation();

  DxvkStagingBufferStats GetStagingMemoryStatistics() const;

  HRESULT CreateShaderModule(D3D9CommonShader *pShaderModule, uint32_t *pLength,
                             VkShaderStageFlagBits ShaderStage,
                             const DWORD *pShaderBytecode,
                             const DxsoModuleInfo *pModuleInfo);

  inline uint32_t GetUPDataSize(uint32_t vertexCount, uint32_t stride) {
    return vertexCount * stride;
  }

  inline uint32_t GetUPBufferSize(uint32_t vertexCount, uint32_t stride) {
    return (vertexCount - 1) * stride +
           std::max(m_state.vertexDecl->GetSize(0), stride);
  }

  /**
   * \brief Writes data to the given pointer and zeroes any access buffer space
   */
  inline void FillUPVertexBuffer(void *buffer, const void *userData,
                                 uint32_t dataSize, uint32_t bufferSize) {
    uint8_t *data = reinterpret_cast<uint8_t *>(buffer);
    // Don't copy excess data if we don't end up needing it.
    dataSize = std::min(dataSize, bufferSize);
    std::memcpy(data, userData, dataSize);
    // Pad out with 0 to make buffer range checks happy
    // Some games have components out of range in the vertex decl
    // that they don't read from the shader.
    // My tests show that these are read back as 0 always if out of range of
    // the dataSize.
    //
    // So... make the actual buffer the range that satisfies the range of the
    // vertex declaration and pad with 0s outside of it.
    if (dataSize < bufferSize)
      std::memset(data + dataSize, 0, bufferSize - dataSize);
  }

  // So we don't do OOB.
  template <DxsoProgramType ProgramType, D3D9ConstantType ConstantType>
  inline static constexpr uint32_t DetermineSoftwareRegCount() {
    constexpr bool isVS = ProgramType == DxsoProgramType::VertexShader;

    switch (ConstantType) {
    default:
    case D3D9ConstantType::Float:
      return isVS ? caps::MaxFloatConstantsSoftware
                  : caps::MaxSM3FloatConstantsPS;
    case D3D9ConstantType::Int:
      return isVS ? caps::MaxOtherConstantsSoftware : caps::MaxOtherConstants;
    case D3D9ConstantType::Bool:
      return isVS ? caps::MaxOtherConstantsSoftware : caps::MaxOtherConstants;
    }
  }

  // So we don't copy more than we need.
  template <DxsoProgramType ProgramType, D3D9ConstantType ConstantType>
  inline uint32_t DetermineHardwareRegCount() const {
    const auto &layout = m_consts[ProgramType].layout;

    switch (ConstantType) {
    default:
    case D3D9ConstantType::Float:
      return layout.floatCount;
    case D3D9ConstantType::Int:
      return layout.intCount;
    case D3D9ConstantType::Bool:
      return layout.boolCount;
    }
  }

  inline uint32_t GetFrameLatency() { return m_frameLatency; }

  template <DxsoProgramType ProgramType, D3D9ConstantType ConstantType,
            typename T>
  HRESULT SetShaderConstants(UINT StartRegister, const T *pConstantData,
                             UINT Count);

  template <DxsoProgramType ProgramType, D3D9ConstantType ConstantType,
            typename T>
  HRESULT GetShaderConstants(UINT StartRegister, T *pConstantData, UINT Count) {
    auto GetHelper = [&](const auto &set) {
      const uint32_t regCountHardware =
          DetermineHardwareRegCount<ProgramType, ConstantType>();
      constexpr uint32_t regCountSoftware =
          DetermineSoftwareRegCount<ProgramType, ConstantType>();

      if (StartRegister + Count > regCountSoftware)
        return D3DERR_INVALIDCALL;

      Count = UINT(std::max<INT>(
          std::clamp<INT>(Count + StartRegister, 0, regCountHardware) -
              INT(StartRegister),
          0));

      if (Count == 0)
        return D3D_OK;

      if (pConstantData == nullptr)
        return D3DERR_INVALIDCALL;

      if constexpr (ConstantType == D3D9ConstantType::Float) {
        const float *source = set->fConsts[StartRegister].data;
        const size_t size = Count * sizeof(Vector4);

        std::memcpy(pConstantData, source, size);
      } else if constexpr (ConstantType == D3D9ConstantType::Int) {
        const int *source = set->iConsts[StartRegister].data;
        const size_t size = Count * sizeof(Vector4i);

        std::memcpy(pConstantData, source, size);
      } else {
        for (uint32_t i = 0; i < Count; i++) {
          const uint32_t constantIdx = StartRegister + i;
          const uint32_t arrayIdx = constantIdx / 32;
          const uint32_t bitIdx = constantIdx % 32;

          const uint32_t bit = (1u << bitIdx);

          bool constValue = set->bConsts[arrayIdx] & bit;
          pConstantData[i] = constValue ? TRUE : FALSE;
        }
      }

      return D3D_OK;
    };

    return ProgramType == DxsoProgramTypes::VertexShader
               ? GetHelper(m_state.vsConsts)
               : GetHelper(m_state.psConsts);
  }

  void UpdateFixedFunctionVS();

  void UpdateFixedFunctionPS();

  void ApplyPrimitiveType(DxvkContext *pContext, D3DPRIMITIVETYPE PrimType);

  bool UseProgrammableVS();

  bool UseProgrammablePS();

  uint32_t GetAlphaTestPrecision();

  void BindAlphaTestState();

  void UpdateAlphaTestSpec(VkCompareOp alphaOp, uint32_t precision);
  void UpdateVertexBoolSpec(uint32_t value);
  void UpdatePixelBoolSpec(uint32_t value);
  void UpdatePixelShaderSamplerSpec(uint32_t types, uint32_t fetch4);
  void UpdateCommonSamplerSpec(uint32_t boundMask, uint32_t depthMask,
                               uint32_t drefMask, uint32_t projections);
  void UpdatePointModeSpec(uint32_t mode);
  void UpdateFogModeSpec(bool fogEnabled, D3DFOGMODE vertexFogMode,
                         D3DFOGMODE pixelFogMode);

  D3D9FFShaderKeyVS BuildFFKeyVS(D3D9FF_VertexBlendMode vertexBlendMode,
                                 bool indexedVertexBlend) const;
  D3D9FFShaderKeyFS BuildFFKeyFS() const;

  void BindSpecConstants();

  void TrackBufferMappingBufferSequenceNumber(D3D9CommonBuffer *pResource);

  void TrackTextureMappingBufferSequenceNumber(D3D9CommonTexture *pResource,
                                               UINT Subresource);

  uint64_t GetCurrentSequenceNumber();

  /**
   * \brief Will unmap the least recently used textures if the amount of mapped
   * texture memory exceeds a threshold.
   */
  void UnmapTextures();

  /**
   * \brief Get the swapchain that was used the most recently for presenting
   * Has to be externally synchronized.
   */
  D3D9SwapChainEx *GetMostRecentlyUsedSwapchain() {
    return m_mostRecentlyUsedSwapchain;
  }

  /**
   * \brief Set the swapchain that was used the most recently for presenting
   * Has to be externally synchronized.
   */
  void SetMostRecentlyUsedSwapchain(D3D9SwapChainEx *swapchain) {
    m_mostRecentlyUsedSwapchain = swapchain;
  }

  /**
   * @brief Reset the most recently swapchain back to the implicit one
   * Has to be externally synchronized.
   */
  void ResetMostRecentlyUsedSwapchain() {
    m_mostRecentlyUsedSwapchain = m_implicitSwapchain.ptr();
  }

  bool IsTextureBoundAsAttachment(const D3D9CommonTexture *pTexture) const {
    if (unlikely(pTexture->IsRenderTarget())) {
      for (uint32_t i = 0u; i < m_state.renderTargets.size(); i++) {
        if (m_state.renderTargets[i] == nullptr)
          continue;

        auto texInfo = m_state.renderTargets[i]->GetCommonTexture();
        if (unlikely(texInfo == pTexture)) {
          return true;
        }
      }
    } else if (unlikely(pTexture->IsDepthStencil() &&
                        m_state.depthStencil != nullptr)) {
      auto texInfo = m_state.depthStencil->GetCommonTexture();
      if (unlikely(texInfo == pTexture)) {
        return true;
      }
    }
    return false;
  }

  inline bool HasRenderTargetBound(uint32_t Index) const {
    return m_state.renderTargets[Index] != nullptr &&
           !m_state.renderTargets[Index]->IsNull();
  }

  inline D3D9ShaderMasks VSShaderMasks() const {
    return m_state.vertexShader != nullptr
               ? m_state.vertexShader->GetCommonShader()->GetShaderMask()
               : D3D9ShaderMasks();
  }

  inline D3D9ShaderMasks PSShaderMasks() const {
    return m_state.pixelShader != nullptr
               ? m_state.pixelShader->GetCommonShader()->GetShaderMask()
               : FixedFunctionMask;
  }

  GpuFlushType GetMaxFlushType() const;

  bool ValidateSharedTexture(HANDLE handle, D3DRESOURCETYPE type,
                             const D3D9_COMMON_TEXTURE_DESC &textureDesc) const;

  bool ValidateSharedBuffer(HANDLE handle,
                            const dxvk::D3D9_BUFFER_DESC &bufferDesc) const;

  bool HasFormatsUnlocked() const { return m_unlockAdditionalFormats; }

  Com<D3D9InterfaceEx> m_parent;
  D3DDEVTYPE m_deviceType;
  HWND m_window;
  WORD m_behaviorFlags;

  D3D9Adapter *m_adapter;
  Rc<DxvkDevice> m_dxvkDevice;

  D3D9MemoryAllocator m_memoryAllocator;

  // Second memory allocator used for D3D9 shader bytecode.
  // Most games never access the stored bytecode, so putting that
  // into the same chunks as texture memory would waste address space.
  D3D9MemoryAllocator m_shaderAllocator;

  uint32_t m_frameLatency = DefaultFrameLatency;

  D3D9Initializer *m_initializer = nullptr;
  D3D9FormatHelper *m_converter = nullptr;
  War3RenderPipeline *m_war3Pipeline = nullptr;
  std::unique_ptr<war3::gpu_skin::War3GpuSkinManager>
      m_war3GpuSkinManager;
  std::unique_ptr<war3::gpu_skin::War3GpuSkinCompute>
      m_war3GpuSkinCompute;
  war3::gpu_skin::War3PersistentGpuPackageStage11ObserveAdapter
      m_war3PersistentPackageStage11ObserveAdapter;
  std::unique_ptr<
      war3::gpu_skin::War3PersistentGpuPackageD3D9ObserveOwner>
      m_war3PersistentPackageD3D9ObserveOwner;
  uint64_t m_war3PersistentPackageIndexScanFrameSerial = 0u;
  uint64_t m_war3PersistentPackageIndexProofBytesThisFrame = 0u;
  uint64_t m_war3PersistentPackageProofTicksThisFrame = 0u;
  uint64_t m_war3PersistentPackagePositionHashBytesThisFrame = 0u;
  uint64_t m_war3PersistentPackageCaptureIndexScans = 0u;
  uint64_t m_war3PersistentPackageCaptureIndexScanBytes = 0u;
  uint64_t m_war3PersistentPackageCaptureIndexScanTicks = 0u;
  uint64_t m_war3PersistentPackageCapturePositionCopies = 0u;
  uint64_t m_war3PersistentPackageCapturePositionCopyBytes = 0u;
  uint64_t m_war3PersistentPackageCapturePositionCopyTicks = 0u;
  uint64_t m_war3PersistentPackageCaptureContentHashBytes = 0u;
  uint64_t m_war3PersistentPackageCaptureContentHashTicks = 0u;
  uint64_t m_war3PersistentPackageCaptureProofBudgetRejected = 0u;
  Rc<sync::Fence> m_war3ShadowArenaFence;
  std::atomic<uint64_t> m_war3ShadowMapResetRequestedSerial { 0u };
  uint64_t m_war3ShadowMapResetAppliedSerial = 0u;
  uint64_t m_war3ShadowMapResetAppliedFrameSerial = 0u;
  uint64_t m_war3ShadowArenaQuarantinedRetireSerial = 0u;
  std::atomic<bool> m_war3ShadowSessionReady { true };
  // A D3D9 Reset keeps the Warcraft map alive but publishes a new logical
  // device epoch after GPU-skin rebind. Receiver state belongs to the CS
  // thread, so the render owner records the committed epoch here and applies
  // it at the next Present boundary before producers may reopen.
  std::atomic<uint64_t> m_war3ShadowDeviceEpochRequested { 1u };
  std::atomic<uint64_t> m_war3ShadowDeviceEpochApplied { 1u };
  // Reset/ResetEx closes admission before GPU-skin rebind begins. This stays
  // true across failed retries and is cleared only by the Present transaction
  // that publishes the committed receiver/device tuple.
  std::atomic<bool> m_war3ShadowDeviceRebindPending { false };
  std::atomic<uint64_t> m_war3ShadowDiagAppliedResetSerial { 0u };
  std::atomic<uint64_t> m_war3ShadowDiagCurrentMapEpoch { 0u };
  std::atomic<uint64_t> m_war3ShadowDiagAppliedFrameSerial { 0u };
  std::atomic<uint64_t> m_war3ShadowDiagQuarantinedRetireSerial { 0u };
  std::atomic<uint64_t> m_war3ShadowDiagCompletedRetireSerial { 0u };
  std::atomic<uint64_t> m_war3ShadowDiagRetiredSessionCount { 0u };
  std::atomic<uint64_t> m_war3ShadowDiagRetiredSessionEntryCount { 0u };
  std::atomic<uint64_t> m_war3ShadowDiagRetiredSessionAllocatorBytes { 0u };
  std::atomic<uint64_t> m_war3ShadowDiagRetiredSessionCachedGpuLogicalBytes {
    0u };
  std::atomic<uint64_t> m_war3ShadowDiagRetiredSessionCpuOwnedBytes { 0u };
  std::atomic<uint64_t> m_war3ShadowDiagRetiredSessionOldestRetireSerial {
    0u };
  std::atomic<uint64_t> m_war3ShadowDiagRetiredSessionCollectedCount { 0u };
  std::atomic<uint64_t> m_war3ShadowDiagRetiredLastMapEpoch { 0u };
  std::atomic<uint64_t> m_war3ShadowDiagPendingProducerRejectCount { 0u };
  std::atomic<uint32_t> m_war3ShadowDiagTransitionState { 0u };
  Rc<DxvkFence> m_war3GpuSkinFence;
  uint64_t m_war3GpuSkinFenceValue = 0u;
  uint64_t m_war3GpuSkinMapEpoch = 1u;
  uint64_t m_war3GpuSkinDeviceEpoch = 1u;
  enum class War3GpuSkinDeviceBindingState : uint8_t {
    Ready,
    RebindPending,
  };
  War3GpuSkinDeviceBindingState m_war3GpuSkinDeviceBindingState =
      War3GpuSkinDeviceBindingState::Ready;
  uint64_t m_war3GpuSkinPendingDeviceEpoch = 0u;
  uint64_t m_war3GpuSkinDeviceRebindAttempts = 0u;
  uint64_t m_war3GpuSkinDeviceRebindFailures = 0u;
  std::vector<uint64_t> m_war3GpuSkinFrameBatchIds;
  struct War3GpuSkinD3D9IndexTicketState {
    D3D9CommonBuffer* commonResource = nullptr;
    uintptr_t comIndexBuffer = 0u;
    uint64_t resourceGeneration = 0u;
    uint64_t ticketGeneration = 0u;
    uint32_t offset = 0u;
    uint32_t size = 0u;
    uint32_t flags = 0u;
    uint32_t indexFormat = 0u;
    bool contentsValidated = false;
    bool unlockNotified = false;
    bool setIndicesNotified = false;

    explicit operator bool() const {
      return commonResource != nullptr && comIndexBuffer != 0u &&
             resourceGeneration != 0u && ticketGeneration != 0u;
    }
  };
  War3GpuSkinD3D9IndexTicketState m_war3GpuSkinD3D9IndexTicket;
  struct War3GpuSkinParityReadback {
    Rc<DxvkBuffer> buffer;
    Rc<DxvkFence> fence;
    uint64_t fenceValue = 0u;
    uint64_t token = 0u;
    uint32_t byteCount = 0u;
    uint32_t gpuOffset = 0u;
    uint32_t allocationBytes = 0u;
    uint32_t stride = 0u;
    uint32_t outputFormat = 0u;
  };
  std::vector<War3GpuSkinParityReadback> m_war3GpuSkinParityReadbacks;
  uint64_t m_war3GpuSkinParityReadbackBytes = 0u;
  uint64_t m_war3GpuSkinParitySkippedBudget = 0u;
  uint64_t m_war3GpuSkinParitySkippedSource = 0u;
  uint64_t m_war3GpuSkinParitySamples = 0u;
  uint64_t m_war3GpuSkinParityMatches = 0u;
  uint64_t m_war3GpuSkinParityMismatches = 0u;
  std::array<uint64_t, 6> m_war3GpuSkinParitySamplesByFormat = {};
  std::array<uint64_t, 6> m_war3GpuSkinParityMatchesByFormat = {};
  std::array<uint64_t, 6> m_war3GpuSkinParityMismatchesByFormat = {};
  uint64_t m_war3GpuSkinP3Hits = 0u;
  uint64_t m_war3GpuSkinP3Rejects = 0u;
  uint64_t m_war3GpuSkinP3MainDrawsSubmitted = 0u;
  uint64_t m_war3GpuSkinP3OutlineDrawsSubmitted = 0u;
  uint64_t m_war3GpuSkinP3OutlineSameSliceSubmitted = 0u;
  uint64_t m_war3GpuSkinP3OutlineSliceMismatches = 0u;
  uint64_t m_war3GpuSkinVsMainRouteAttempts = 0u;
  uint64_t m_war3GpuSkinVsMainInputRejects = 0u;
  uint64_t m_war3GpuSkinVsMainStateRejects = 0u;
  uint64_t m_war3GpuSkinVsMainDrawsSubmitted = 0u;
  uint64_t m_war3GpuSkinVsMainBindingsCleared = 0u;
  uint64_t m_war3GpuSkinVsShadowCaptureAttempts = 0u;
  uint64_t m_war3GpuSkinVsShadowCaptureInputRejects = 0u;
  uint64_t m_war3GpuSkinVsShadowCaptureStateRejects = 0u;
  uint64_t m_war3GpuSkinVsShadowCaptureCommitted = 0u;
  uint64_t m_war3GpuSkinP3Restores = 0u;
  uint64_t m_war3GpuSkinP3RestoreRebinds = 0u;
  uint64_t m_war3GpuSkinP3RestoreOverlaps = 0u;
  bool m_war3GpuSkinP3RestorePending = false;
  uint64_t m_war3GpuSkinP2ResolvedInputs = 0u;
  uint64_t m_war3GpuSkinP2ResolvedStage11Inputs = 0u;
  uint64_t m_war3GpuSkinP2SemanticGateInputs = 0u;
  uint64_t m_war3GpuSkinP2SemanticContextInputs = 0u;
  uint64_t m_war3GpuSkinP2IdentityMatchInputs = 0u;
  uint64_t m_war3GpuSkinP2BackingHits = 0u;
  uint64_t m_war3GpuSkinP2BackingRejects = 0u;
  uint64_t m_war3GpuSkinP2BackingFallbacks = 0u;
  uint64_t m_war3GpuSkinP2SkippedCpuCopyBytes = 0u;
  // P4 Shadow 承诺必须晚于可复用 IB/UV scratch 的只读 warm gate。
  // 终门计数用于证明授权后不再发生不可逆 consumer suppression。
  uint64_t m_war3GpuSkinP4ShadowPreflightIndexRejects = 0u;
  uint64_t m_war3GpuSkinP4ShadowPreflightUvRejects = 0u;
  uint64_t m_war3GpuSkinP4ShadowFinalPositionRejects = 0u;
  uint64_t m_war3GpuSkinP4ShadowFinalIndexRejects = 0u;
  uint64_t m_war3GpuSkinP4ShadowFinalUvRejects = 0u;
  uint64_t m_war3GpuSkinP4ShadowFinalCommitRejects = 0u;
  uint64_t m_war3GpuSkinP4ShadowCommits = 0u;
  uint64_t m_war3GpuSkinDiagnosticFrames = 0u;
  War3FrameScene m_war3Scene;
  // Reused render-thread-only SoA storage for the generation-sealed compact
  // control plane. It never owns geometry, alpha payloads, or palette bytes.
  War3CompactWorkTable m_war3CompactWorkTable;
  // 本帧场景已被 rotate（move 进 pipeline input）的帧序号。direct-only 模式
  // 下 EndFrame flush 用它判定 BeforeUi 是否已消费本帧场景：命中则跳过重复
  // 的全量 populate 固定开销；未命中（菜单/过场等 BeforeUi 漏检帧）仍走完整
  // 兜底路径。以 rotate 事实为键，而非 populate 成功——见 2026-07-26 分析。
  uint64_t m_war3SceneRotatedFrameSerial = 0u;
  struct War3ShadowDeclInfo {
    bool hasPosition = false;
    D3DDECLTYPE posType = D3DDECLTYPE_UNUSED;
    uint32_t posCompCount = 0;
    VkFormat posFormat = VK_FORMAT_UNDEFINED;
    uint32_t posStream = 0;
    uint32_t posOffset = 0;

    bool hasBlendWeight = false;
    D3DDECLTYPE weightType = D3DDECLTYPE_UNUSED;
    VkFormat weightFormat = VK_FORMAT_UNDEFINED;
    uint32_t weightStream = 0;
    uint32_t weightOffset = 0;

    bool hasBlendIndex = false;
    D3DDECLTYPE indexType = D3DDECLTYPE_UNUSED;
    VkFormat indexFormat = VK_FORMAT_UNDEFINED;
    uint32_t indexStream = 0;
    uint32_t indexOffset = 0;
  };
  std::unordered_map<D3D9VertexDecl *, War3ShadowDeclInfo>
      m_war3ShadowDeclCache;
  size_t m_shadowCasterReserveHint = 0;
  size_t m_shadowPaletteReserveHint = 0;
  dxvk::war3::render::War3FrameHashIndex m_war3ShadowPaletteHashIndex;
  struct War3SemanticPaletteCacheEntry {
    const void* runtimeModelPtr = nullptr;
    const void* paletteData = nullptr;
    uint64_t matrixHash = 0u;
    uint64_t worldHash = 0u;
    uint32_t matrixCount = 0u;
    uint8_t objectKind = 0u;
    bool composedWorldPalette = false;
    uint32_t paletteIndex = 0u;
    std::vector<Matrix4> composedPalette;
  };
  std::vector<War3SemanticPaletteCacheEntry> m_war3SemanticPaletteCache;
  // Frame-local retained hash nodes avoid allocating one unordered-map node
  // per palette every frame. Hash hits still require the complete semantic
  // comparison below, so collision and identity behavior remain unchanged.
  dxvk::war3::render::War3FrameHashIndex
      m_war3SemanticPaletteCacheHashIndex;
  // War3：当 D3D9 走 UploadPerDrawData（SYSTEMMEM|DYNAMIC/UP 路径）时，
  // 实际 draw 绑定的 VB/IB 会被替换为 UP buffer 的子切片，且底层 VkBuffer
  // 可能被 invalidate。 为了让 shadow caster drawlist 在 BeforeUi
  // 重放时仍能读取到“当时那一次 draw 的真实数据”， 需要在 capture 时固化本次
  // UploadPerDrawData 的绑定信息。
  struct War3PerDrawUploadInfo {
    Rc<DxvkResourceAllocation> storage;
    std::array<DxvkBufferSlice, caps::MaxStreams> vbSlices = {};
    std::array<uint32_t, caps::MaxStreams> vbStrides = {};
    std::array<bool, caps::MaxStreams> vbValid = {};
    // Exact CPU address of the bytes written into each per-draw UP slice.
    // DxvkBufferSlice names the virtual buffer and only resolves its backing
    // on the command-stream thread; these pointers are pinned by storage and
    // therefore name the same DISCARD generation that the main draw consumes.
    std::array<const void*, caps::MaxStreams> vbUploadBytes = {};
    std::array<uint32_t, caps::MaxStreams> vbUploadLength = {};
    std::array<const void*, caps::MaxStreams> vbSourceBase = {};
    std::array<uint32_t, caps::MaxStreams> vbSourceOffset = {};
    std::array<uint32_t, caps::MaxStreams> vbSourceLength = {};
    std::array<uint32_t, caps::MaxStreams> vbSourceElementCount = {};
    std::array<uint32_t, caps::MaxStreams> vbSourceElementStride = {};
    std::array<uint32_t, caps::MaxStreams> vbSourceElementSize = {};
    std::array<uint64_t, caps::MaxStreams> vbSourceSequence = {};
    std::array<uintptr_t, caps::MaxStreams> vbSourceResource = {};
    std::array<uint64_t, caps::MaxStreams> vbSourceIdentityGeneration = {};
    std::array<uint64_t, caps::MaxStreams> vbSourceContentGeneration = {};
    std::array<bool, caps::MaxStreams> vbSourceValid = {};
    DxvkBufferSlice ibSlice;
    VkIndexType ibType = VK_INDEX_TYPE_UINT32;
    bool ibValid = false;
    Rc<DxvkResourceAllocation> ibStorage;
    // Exact CPU address of the index bytes copied into the per-draw upload
    // allocation.  DxvkBufferSlice::mapPtr() follows the virtual buffer's
    // current storage and can therefore observe a different DISCARD
    // generation until the queued invalidate command executes.  This pointer
    // is pinned by ibStorage and identifies the same allocation that the main
    // draw binds.
    const void* ibUploadBytes = nullptr;
    uint32_t ibUploadLength = 0u;
    uintptr_t ibSourceResource = 0u;
    uint64_t ibSourceIdentityGeneration = 0u;
    uint64_t ibSourceSequence = 0u;
    uint64_t ibSourceContentGeneration = 0u;
    uint32_t ibSourceOffset = 0u;
    uint32_t ibSourceLength = 0u;
    bool ibSourceValid = false;
  };
  War3PerDrawUploadInfo m_war3PerDrawUpload;
  // 上一帧/最近一次“可解析透视投影”的世界相机快照（用于兜底，避免偶发捕获失败导致整帧无阴影）。
  War3WorldCameraState m_war3LastGoodCamera;
  // War3 BeforeUi 插入点需要使用“世界渲染结束时”的 RT/DS。
  // UI/HUD 往往会在 draw 前切换
  // depthStencil（甚至置空），因此在世界阶段先缓存一份。
  Com<D3D9Surface, false> m_war3LastWorldRt0;
  Com<D3D9Surface, false> m_war3LastWorldDs;
  uint64_t m_war3BestWorldViewportArea = 0u;
  // 世界相机捕获优先级：用于避免被地形 overlay /
  // 水面等“非主相机”透视矩阵覆盖（导致阴影随镜头乱飞）
  uint32_t m_war3BestWorldCameraTier = 0u;
  Com<IDirect3DVertexDeclaration9> m_war3DebugOverlayDecl;
  bool m_war3DebugOverlayDrawn = false;
  // 本帧是否已经进入 UI 绘制（用于避免后处理污染 UI）
  bool m_war3UiDrawSeenThisFrame = false;

  D3D9FFShaderModuleSet m_ffModules;
  D3D9SWVPEmulator m_swvpEmulator;

  Com<D3D9StateBlock, false> m_recorder;

  Rc<D3D9ShaderModuleSet> m_shaderModules;

  D3D9ConstantBuffer m_vsClipPlanes;

  D3D9ConstantBuffer m_vsFixedFunction;
  D3D9ConstantBuffer m_vsVertexBlend;
  D3D9ConstantBuffer m_psFixedFunction;
  D3D9ConstantBuffer m_psShared;
  D3D9ConstantBuffer m_specBuffer;

  Rc<DxvkBuffer> m_upBuffer;
  VkDeviceSize m_upBufferOffset = 0ull;
  void *m_upBufferMapPtr = nullptr;
  // 当前 UP buffer 对应的 backing allocation（wrap/invalidate 时会更换），用于
  // War3 捕获固定 VkBuffer
  Rc<DxvkResourceAllocation> m_upBufferAllocation;

  DxvkStagingBuffer m_stagingBuffer;
  Rc<sync::Fence> m_stagingBufferFence;
  VkDeviceSize m_stagingMemorySignaled = 0ull;

  VkDeviceSize m_discardMemoryCounter = 0u;
  VkDeviceSize m_discardMemoryOnFlush = 0u;

  D3D9Cursor m_cursor;

  Com<D3D9Surface, false> m_autoDepthStencil;

  Com<D3D9SwapChainEx, false> m_implicitSwapchain;

  const D3D9Options m_d3d9Options;
  DxsoOptions m_dxsoOptions;

  std::unordered_map<DWORD, Com<D3D9VertexDecl, false>> m_fvfTable;

  D3D9Multithread m_multithread;
  D3D9InputAssemblyState m_iaState;

  D3D9DeviceDirtyFlags m_dirty;

  D3D9TextureSlotTracking m_textureSlotTracking;

  D3D9RTSlotTracking m_rtSlotTracking;

  D3D9VBSlotTracking m_vbSlotTracking;

  D3D9SpecializationInfo m_specInfo = D3D9SpecializationInfo();

  bool m_isSWVP;
  bool m_isD3D8Compatible;
  bool m_ffZTest = false;

  // the enablement of below features is tracked independently
  // of render states both due to complexity and to avoid
  // incurring overhead on all render state changes
  bool m_alphaTestEnabled = false;
  // vendor hack state tracking
  bool m_atocEnabled = false;
  bool m_nvdbEnabled = false;

  bool m_inScene = false;
  bool m_validSampleMask = false;

  VkImageLayout m_hazardLayout = VK_IMAGE_LAYOUT_GENERAL;

  bool m_usingGraphicsPipelines = false;
  uint32_t m_resetCtr = 0u;

  DxvkDepthBiasRepresentation m_depthBiasRepresentation = {
      VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT, false};
  float m_depthBiasScale = 0.0f;

  uint32_t m_robustSSBOAlignment = 1;
  uint32_t m_robustUBOAlignment = 1;
  uint32_t m_frameMaxViewportRight = 0u;
  uint32_t m_frameMaxViewportBottom = 0u;

  D3D9ConstantSets m_consts[DxsoProgramTypes::Count];

  D3D9UserDefinedAnnotation *m_annotation = nullptr;

  D3D9ViewportInfo m_viewportInfo;

  DxvkCsChunkPool m_csChunkPool;
  DxvkCsThread m_csThread;
  DxvkCsChunkRef m_csChunk;
  uint64_t m_csSeqNum = 0ull;

  Rc<sync::Fence> m_submissionFence;
  uint64_t m_submissionId = 0ull;
  DxvkSubmitStatus m_submitStatus;

  uint64_t m_flushSeqNum = 0ull;
  GpuFlushTracker m_flushTracker;

  std::atomic<int64_t> m_availableMemory = {0};

  D3D9DeviceLostState m_deviceLostState = D3D9DeviceLostState::Ok;
  std::atomic<bool> m_vkDeviceLostFailStop{false};
  std::atomic<bool> m_vkDeviceLostBaseIncidentReady{false};
  HWND m_fullscreenWindow = NULL;
  std::atomic<uint32_t> m_losableResourceCounter = {0};

  D3D9SwapChainEx *m_mostRecentlyUsedSwapchain = nullptr;

#ifdef D3D9_ALLOW_UNMAPPING
  lru_list<D3D9CommonTexture *> m_mappedTextures;
#endif

  // m_state should be declared last (i.e. freed first), because it
  // references objects that can call back into the device when freed.
  Direct3DState9 m_state;

  D3D9VkInteropDevice m_d3d9Interop;
  D3D9ON12_ARGS m_d3d9On12Args = {};
  D3D9On12 m_d3d9On12;
  DxvkD3D8Bridge m_d3d8Bridge;

  // Sampler statistics
  constexpr static uint32_t SamplerCountBits = 12u;
  constexpr static uint64_t SamplerCountMask = (1u << SamplerCountBits) - 1u;

  uint64_t m_samplerBindCount = 0u;

  uint64_t m_lastSamplerLiveCount = 0u;
  uint64_t m_lastSamplerBindCount = 0u;

  // Written by CS thread
  alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> m_lastSamplerStats = {0u};

  // War3 Shadow Buffer Allocator (Freezer)
  struct War3ShadowBufferAllocator {
    std::vector<Rc<DxvkBuffer>> chunks;
    Rc<DxvkBuffer> currentChunk;
    VkDeviceSize currentOffset = 0;
    // 4MB chunks for efficient freeze allocation
    static constexpr VkDeviceSize ChunkSize = 4 * 1024 * 1024;

    void Reset() {
      chunks.clear();
      currentChunk = nullptr;
      currentOffset = 0;
    }
  };
  struct War3ShadowMappedBufferAllocator {
    std::vector<Rc<DxvkBuffer>> chunks;
    Rc<DxvkBuffer> currentChunk;
    void* currentMapPtr = nullptr;
    VkDeviceSize currentOffset = 0;
    static constexpr VkDeviceSize ChunkSize = 4 * 1024 * 1024;

    void Reset() {
      chunks.clear();
      currentChunk = nullptr;
      currentMapPtr = nullptr;
      currentOffset = 0;
    }
  };
  enum class War3FrameFreezeStreamType : uint8_t {
    Position = 0u,
    Blend,
    Uv,
    Index,
  };
  struct War3FrameFreezeKey {
    DxvkBuffer* sourceBuffer = nullptr;
    VkDeviceSize sourceOffset = 0;
    VkDeviceSize sourceLength = 0;
    VkDeviceSize size = 0;
    uint32_t sourceElementStride = 0u;
    uint32_t sourceElementSize = 0u;
    uintptr_t allocationIdentity = 0u;
    uintptr_t sourceOwner = 0u;
    uint64_t identityGeneration = 0u;
    uint64_t allocationGeneration = 0u;
    uint64_t contentGeneration = 0u;
    uint64_t mapEpoch = 0u;
    uint64_t frameSerial = 0u;
    War3FrameFreezeStreamType streamType =
        War3FrameFreezeStreamType::Position;

    bool operator==(const War3FrameFreezeKey& other) const {
      return sourceBuffer == other.sourceBuffer &&
             sourceOffset == other.sourceOffset &&
             sourceLength == other.sourceLength && size == other.size &&
             sourceElementStride == other.sourceElementStride &&
             sourceElementSize == other.sourceElementSize &&
             allocationIdentity == other.allocationIdentity &&
             sourceOwner == other.sourceOwner &&
             identityGeneration == other.identityGeneration &&
             allocationGeneration == other.allocationGeneration &&
             contentGeneration == other.contentGeneration &&
             mapEpoch == other.mapEpoch &&
             frameSerial == other.frameSerial &&
             streamType == other.streamType;
    }
  };
  struct War3FrameFreezeKeyHash {
    size_t operator()(const War3FrameFreezeKey& key) const {
      size_t hash = std::hash<DxvkBuffer*>()(key.sourceBuffer);
      const auto mix = [&](size_t value) {
        hash ^= value + size_t(0x9e3779b9u) + (hash << 6) + (hash >> 2);
      };
      mix(std::hash<VkDeviceSize>()(key.sourceOffset));
      mix(std::hash<VkDeviceSize>()(key.sourceLength));
      mix(std::hash<VkDeviceSize>()(key.size));
      mix(std::hash<uint32_t>()(key.sourceElementStride));
      mix(std::hash<uint32_t>()(key.sourceElementSize));
      mix(std::hash<uintptr_t>()(key.allocationIdentity));
      mix(std::hash<uintptr_t>()(key.sourceOwner));
      mix(std::hash<uint64_t>()(key.identityGeneration));
      mix(std::hash<uint64_t>()(key.allocationGeneration));
      mix(std::hash<uint64_t>()(key.contentGeneration));
      mix(std::hash<uint64_t>()(key.mapEpoch));
      mix(std::hash<uint64_t>()(key.frameSerial));
      mix(std::hash<uint8_t>()(uint8_t(key.streamType)));
      return hash;
    }
  };
  struct War3FrameFreezeEntry {
    Rc<DxvkBuffer> frozenBuffer;
    DxvkResourceBufferInfo frozenInfo = {};
    uint64_t sourceBytes = 0u;
  };
  using War3ShadowGeometryRegistryKey = War3ShadowGeometryKey;
  struct War3ShadowGeometryRegistryKeyHash {
    size_t operator()(const War3ShadowGeometryRegistryKey& key) const {
      const size_t h1 = std::hash<uint64_t>()(key.sourceHash);
      const size_t h2 = std::hash<uint64_t>()(key.layoutHash);
      const size_t h3 = std::hash<uint32_t>()(static_cast<uint32_t>(key.mode));
      return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2)) ^
             (h3 + 0x9e3779b9u + (h2 << 6) + (h2 >> 2));
    }
  };
  struct War3ShadowGeometryRegistryEntry {
    uint32_t geometryId = 0;
    uint32_t instances = 0;
    bool instanceable = false;
  };
  struct War3ShadowFrozenGeometryCacheEntry {
    War3ShadowCasterDraw drawTemplate = {};
    uint64_t totalFreezeBytes = 0;
  };
  struct War3ShadowPersistentUpload {
    DxvkBufferSlice slice;
    const void* hostData = nullptr;
    VkDeviceSize bytes = 0;
    VkBufferUsageFlags usage = 0;
    const char* debugName = nullptr;
    // Static S1 geometry already lives in a stable GPU buffer. Retaining that
    // source slice avoids allocating and copying another device-local buffer
    // for every bridge/ramp tile while preserving the same strong lifetime.
    bool retainSource = false;
  };
  enum class War3ShadowPersistentCreateFailure : uint8_t {
    None = 0,
    Capacity,
    PositionBufferCreate,
    IndexBufferCreate,
    BlendBufferCreate,
    UvBufferCreate,
    RegistryInsert,
    Other,
  };
  struct War3ShadowPersistentDiagnosticsFrame {
    // These reject counters refine the legacy
    // persistentRejectCreateOrBudget bucket and therefore count only the
    // ShadowCapture caller that publishes that legacy bucket.
    uint64_t rejectCapacity = 0;
    uint64_t rejectPositionBufferCreate = 0;
    uint64_t rejectIndexBufferCreate = 0;
    uint64_t rejectBlendBufferCreate = 0;
    uint64_t rejectUvBufferCreate = 0;
    uint64_t rejectRegistryInsert = 0;
    uint64_t rejectOther = 0;

    // Creation/GC telemetry covers every caller of the shared persistent
    // geometry helper during the completed Present-to-Present interval.
    uint64_t createAttempts = 0;
    uint64_t bytesNeededTotal = 0;
    uint64_t bytesNeededMax = 0;
    uint64_t bytesNeededLast = 0;
    uint64_t forceGcRequests = 0;
    uint64_t forceGcNoBytesFreed = 0;
    uint64_t forceGcStillInsufficient = 0;
    uint64_t forceGcBytesFreed = 0;
    uint64_t capacityRejectAllCallers = 0;
    uint64_t capacityFastReject = 0;
    uint64_t expiryTokensPopped = 0;
    uint64_t expiryTokensRequeued = 0;
    uint64_t expiryStaleTokens = 0;
    uint64_t expiryAgeEvictions = 0;

    // Present-sampled S1 early-cache gauges. logicalReferencedBytes is a
    // conservative sum of the buffer ranges referenced by every entry. It is
    // intentionally not a unique-allocation or resident-memory measurement.
    uint64_t s1EarlyEntryCount = 0;
    uint64_t s1EarlyPersistentBackedCount = 0;
    uint64_t s1EarlyPersistentReverseIndexCount = 0;
    uint64_t s1EarlyFallbackBackedCount = 0;
    uint64_t s1EarlyLogicalReferencedBytes = 0;

    // Per-interval publication closure for accepted S1 early-cache hits.
    // BuildShadowReplayDraws consumes only shadowInstances/shadowFallbacks, so
    // every accepted hit must publish exactly one canonical replay record in
    // addition to retaining the compatibility shadowCasters entry.
    uint64_t s1EarlyAcceptedHitCount = 0;
    uint64_t s1EarlyReplayPublishedCount = 0;
    uint64_t s1EarlyReplayInstanceCount = 0;
    uint64_t s1EarlyReplayFallbackCount = 0;
    // 命中但源指纹不匹配（key 碰撞或 VB 指针复用）而被淘汰的次数。
    // 非零即证明 early key 碰撞在真实运行中发生过。
    uint64_t s1EarlySourceMismatchEvictCount = 0;

    // Observe-only generation proof for dynamic S1 terrain. These counters do
    // not authorize cache consumption; they establish whether the exact owner,
    // allocation, content generation and source ranges remain identical on
    // adjacent frames before a later persistent-promotion candidate is built.
    uint64_t s1GenerationProofEntryCount = 0;
    uint64_t s1GenerationProofEligibleCount = 0;
    uint64_t s1GenerationProofFirstCount = 0;
    uint64_t s1GenerationProofSameFrameCount = 0;
    uint64_t s1GenerationProofAdvancedCount = 0;
    uint64_t s1GenerationProofChangedCount = 0;
    uint64_t s1GenerationProofStaleRestartCount = 0;
    uint64_t s1GenerationProofPromotionReadyCount = 0;
    uint64_t s1GenerationProofCapacityRejectCount = 0;
  };
  struct War3ShadowPersistentGeometryEntry {
    War3ShadowGeometryRegistryKey key = {};
    War3ShadowPersistentGeometry geometry = {};
    uint64_t totalBytes = 0;
    uint64_t lastSeenFrame = 0;
  };
  struct War3ShadowPersistentExpiryEntry {
    uint64_t lastSeenFrame = 0;
    uint32_t geometryId = 0;
  };
  struct War3ShadowPersistentExpiryCompare {
    bool operator()(const War3ShadowPersistentExpiryEntry& a,
                    const War3ShadowPersistentExpiryEntry& b) const {
      return a.lastSeenFrame > b.lastSeenFrame;
    }
  };
  struct War3SemanticDirectCasterContractKey {
    uint64_t mapEpoch = 0u;
    uint64_t identityKey = 0;
    uint64_t sceneNode = 0;
    uint64_t renderablePart = 0;
    uint64_t meshData = 0;

    bool operator==(const War3SemanticDirectCasterContractKey& other) const {
      return mapEpoch == other.mapEpoch &&
             identityKey == other.identityKey &&
             meshData == other.meshData;
    }

    bool valid() const {
      return mapEpoch != 0u && identityKey != 0 && meshData != 0;
    }
  };
  struct War3SemanticDirectCasterContractKeyHash {
    size_t operator()(const War3SemanticDirectCasterContractKey& key) const {
      const size_t h0 = std::hash<uint64_t>()(key.mapEpoch);
      const size_t h1 = std::hash<uint64_t>()(key.identityKey) ^
          (h0 + 0x9e3779b9u + (h0 << 6) + (h0 >> 2));
      const size_t h2 = std::hash<uint64_t>()(key.meshData);
      return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2));
    }
  };
  struct War3SemanticDirectCasterContractState {
    uint64_t paletteHash = 0;
    uint64_t groupHash = 0;
    uint64_t stableGroupHash = 0;
    uint64_t stream1Ptr = 0;
    uint64_t geometrySourceHash = 0;
    uint64_t lastSeenFrame = 0;
    Matrix4 palette0 = Matrix4();
    bool hasPalette0 = false;
  };
  using War3SemanticDirectCasterContractMap =
      std::unordered_map<War3SemanticDirectCasterContractKey,
                         War3SemanticDirectCasterContractState,
                         War3SemanticDirectCasterContractKeyHash>;
  using War3FrameFreezeCatalog =
      std::unordered_map<War3FrameFreezeKey, War3FrameFreezeEntry,
                         War3FrameFreezeKeyHash>;
  using War3ShadowGeometryRegistry =
      std::unordered_map<War3ShadowGeometryRegistryKey,
                         War3ShadowGeometryRegistryEntry,
                         War3ShadowGeometryRegistryKeyHash>;
  using War3ShadowFrozenGeometryCache =
      std::unordered_map<War3ShadowGeometryRegistryKey,
                         War3ShadowFrozenGeometryCacheEntry,
                         War3ShadowGeometryRegistryKeyHash>;
  using War3ShadowPersistentGeometryMap =
      std::unordered_map<uint32_t, War3ShadowPersistentGeometryEntry>;
  // Stage13 contains static bridge/ramp world objects whose native visibility
  // batch may disappear before their projected shadow has left the camera.
  // Retain a bounded CPU copy of the exact referenced vertex sequence. Stale
  // entries are uploaded into the current frame's shared mapped freeze arena;
  // no frame-ring Rc is ever retained across frames and no per-object Vulkan
  // buffer is created.
  struct War3Stage13RetainedCasterEntry {
    War3ShadowCasterDraw draw = {};
    std::vector<unsigned char> positionBytes;
    uint64_t contentHash = 0u;
    uint64_t sourceIdentityHash = 0u;
    uint64_t worldMatrixHash = 0u;
    uint64_t materialHash = 0u;
    uint64_t layoutHash = 0u;
    uint64_t lastSeenFrame = 0u;
  };
  using War3Stage13RetainedCasterMap =
      std::unordered_map<War3ShadowGeometryRegistryKey,
                         War3Stage13RetainedCasterEntry,
                         War3ShadowGeometryRegistryKeyHash>;
  std::array<War3ShadowBufferAllocator, 3> m_war3ShadowAllocators;
  std::array<War3ShadowMappedBufferAllocator, 3> m_war3ShadowMappedAllocators;
  std::array<War3ShadowFrozenGeometryCache, 3>
      m_war3ShadowFrozenGeometryCaches;
  War3FrameFreezeCatalog m_war3FrameFreezeCatalog;
  uint64_t m_war3FrameFreezeCatalogSerial = 0u;
  uint64_t m_war3FrameFreezeUniqueSourceBytes = 0u;
  uint64_t m_war3FrameFreezeDuplicateBytesSaved = 0u;
  War3ShadowGeometryRegistry m_war3ShadowGeometryRegistry;
  War3ShadowPersistentGeometryMap m_war3ShadowPersistentGeometries;
  War3Stage13RetainedCasterMap m_war3Stage13RetainedCasters;
  // Every live geometry has one authoritative scheduled age token. Hits update
  // the map entry only; when an old token reaches the heap head, GC either
  // retires the still-idle geometry or requeues its newer lastSeenFrame.
  // Emergency budget repair may leave a short-lived stale token for an entry
  // it erased; the normal head walk discards that token without double
  // accounting. This preserves the max-age contract without scanning the
  // entire ~512 MiB registry every Present.
  std::priority_queue<War3ShadowPersistentExpiryEntry,
                      std::vector<War3ShadowPersistentExpiryEntry>,
                      War3ShadowPersistentExpiryCompare>
      m_war3ShadowPersistentExpiryQueue;
  uint32_t m_war3NextShadowGeometryId = 1;
  uint64_t m_war3ShadowPersistentFrameSerial = 0;
  uint64_t m_war3ShadowPersistentLastGcFrameSerial = ~uint64_t(0);
  uint64_t m_war3SemanticSceneLastCaptureFrameSerial = 0;
  uint64_t m_war3SemanticSceneLastCapturePublishRevision = 0;
  uint64_t m_war3SemanticSceneLastCapturedVisibleFrameSerial = 0;
  uint64_t m_war3SemanticSceneLastCoverageRecoveryCaptureFrameSerial = 0;
  uint64_t m_war3SemanticSceneLastPoseOnlyCaptureFrameSerial = 0;
  uint64_t m_war3SemanticSceneLastSteadyBuildFrameSerial = 0;
  uint64_t m_war3SemanticSceneLastZeroSubmitFrameSerial = 0;
  uint64_t m_war3SemanticSceneLastZeroSubmitPublishRevision = 0;
  uint64_t m_war3SemanticSceneLastSuccessfulSubmitFrameSerial = 0;
  uint64_t m_war3SemanticSceneLastSuccessfulSubmitPublishRevision = 0;
  std::shared_ptr<const war3::shadow::ShadowSubmissionFrame>
      m_war3SemanticSceneLastReusableFrame;
  uint64_t m_war3SemanticDrawTimePoseFrameSerial = 0;
  uint64_t m_war3SemanticDrawTimePoseDirtyFrameSerial = 0;
  uint64_t m_war3SemanticLastMatrixPublisherPoseRevision = 0;
  std::vector<uint64_t> m_war3SemanticDrawTimePoseKeys;
  struct War3Stage11SnapshotPage {
    uint64_t id = 0u;
    Rc<DxvkBuffer> buffer;
    VkDeviceSize capacity = 0u;
    VkDeviceSize used = 0u;
  };
  enum class War3Stage11SnapshotAllocationResult : uint8_t {
    Success,
    PageCreateBudget,
    ResidentCapacity,
    AllocationFailure,
    InvalidRange,
  };
  std::vector<std::shared_ptr<War3Stage11SnapshotPage>>
      m_war3Stage11SnapshotPages;
  uint64_t m_war3Stage11SnapshotNextPageId = 1u;
  uint64_t m_war3Stage11SnapshotResidentBytes = 0u;
  uint64_t m_war3Stage11SnapshotReclaimedPages = 0u;
  // Phase 7.55 v4：draw-time VB position cache（GPU copy 自有 buffer 版本）。
  // ring buffer 问题：保存 Rc<DxvkBuffer> 引用不够——War3 后续 draw 会覆盖
  // 同一 buffer 的不同 offset，cache 里的引用 read 时拿到的是错乱数据。
  // 解决方案：capture 时用 copyBuffer 把这个 draw 实际使用的 vertex range
  // 拷贝到我们自己的 device-local buffer。以后再有 draw 覆盖原 buffer 也
  // 不影响我们的 buffer。
  struct War3DrawTimeVBEntry {
    uint64_t mapEpoch = 0u;
    void* renderablePart = nullptr;
    uint32_t layerIndex = 0u;
    // Canonical CurrentDraw logical-slice identity. These values are copied
    // from the same authoritative record used to construct the map key and
    // are rechecked by every consumer before a cached draw is published.
    uint32_t payloadWord108 = 0u;
    uint32_t payloadWord11C = 0u;
    void* instanceIdentity = nullptr;
    void* meshPayloadPtr = nullptr;
    uint32_t contractJHandle = 0u;
    // Phase 7.92：capture 时保存 sceneNode，让 producer/fast-append 路径能
    // 从 entry 直接读到世界位置用于 CSM cascade cull。
    void* sceneNode = nullptr;
    void* unitPtr = nullptr;
    void* worldObjectEntry = nullptr;
    int16_t producerStage = -1;
    // 我们自有 GPU buffer（device-local），存 capture 帧的 vertex range bytes。
    // 包含完整 stride 的 vertex 数据；shader 用 positionStride/positionOffset
    // 读取 xyz。
    Rc<DxvkBuffer> positionBuffer;
    Rc<DxvkResourceAllocation> positionPinnedAllocation;
    std::shared_ptr<War3Stage11SnapshotPage> positionSnapshotPage;
    VkDeviceSize positionSnapshotOffset = 0u;
    DxvkResourceBufferInfo positionInfo = {};
    uint32_t positionStride = 0u;
    uint32_t positionOffset = 0u;
    VkFormat positionFormat = VK_FORMAT_R32G32B32_SFLOAT;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    uint32_t vertexCount = 0u;
    // capture 时存好的 Vulkan vertexOffset 值，consume 端直接用。
    // 含义：buffer 索引 0 应映射到原 VB 索引哪个位置的偏移修正。
    //   - wrap path（vRangeStart=0）：vertexOffset = BaseVertexIndex
    //     （让 final_idx = IB_value + BaseVertexIndex 与 D3D9 一致）
    //   - standard path（vRangeStart=BaseVertexIndex+MinVertexIndex）：
    //     vertexOffset = -MinVertexIndex（IB 值 v ∈ [Min, Min+Num)，
    //     映射到 buffer 索引 v - Min；GPU final = (v - Min) + Min = v ≈ buffer
    //     索引 v - Min，与 D3D9 GPU 读 BaseVI+v 等价）
    int32_t consumeVertexOffset = 0;
    // index buffer（未 rebase；index 值仍指向原 vertex 编号空间）
    Rc<DxvkBuffer> indexBuffer;
    Rc<DxvkResourceAllocation> indexPinnedAllocation;
    std::shared_ptr<War3Stage11SnapshotPage> indexSnapshotPage;
    VkDeviceSize indexSnapshotOffset = 0u;
    DxvkResourceBufferInfo indexInfo = {};
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    uint32_t indexCount = 0u;
    uint32_t firstIndex = 0u;
    bool indexed = false;
    // War3's DrawIndexedPrimitive MinVertexIndex/NumVertices pair is only a
    // hint in several model paths.  Exact Stage11 capture therefore records
    // the domain actually referenced by the current IB whenever the bytes
    // are CPU-readable.  When they are not, the only safe alternative is a
    // bounded copy of the complete bound vertex domain.
    uint32_t actualIndexMin = 0u;
    uint32_t actualIndexMax = 0u;
    bool actualIndexDomainKnown = false;
    bool fullVertexDomainFallback = false;
    bool indexHintMismatch = false;
    // True only after every mandatory backing resource for the original draw
    // has been captured. In particular, an indexed source must never be
    // reinterpreted as a non-indexed draw when its IB copy is deferred or
    // fails; doing so can emit unrelated triangles that cover large screen
    // regions.
    bool captureComplete = false;
    // Optional Observe-only content proof. It is sealed from the same exact
    // current-frame CPU spans and generations as this Arena capture, but owns
    // no GPU slice and cannot authorize a renderer mutation.
    war3::gpu_skin::PersistentGpuPackageCurrentDrawProof
        persistentPackageCurrentDrawProof = {};
    // Cross-frame reuse is authorized only by the exact D3D9 source owner and
    // its allocation/content generations.  These proofs never accept the
    // historical pointer/fingerprint shortcut.
    war3::render::War3ShadowGenerationBackedStreamProof positionSourceProof = {};
    war3::render::War3ShadowGenerationBackedStreamProof uvSourceProof = {};
    war3::render::War3ShadowGenerationBackedStreamProof indexSourceProof = {};

    bool MatchesKey(const War3DrawTimeVBCacheKey& key) const {
      return mapEpoch == key.mapEpoch &&
             instanceIdentity == key.instanceIdentity &&
             meshPayloadPtr == key.meshPayloadPtr &&
             renderablePart == key.renderablePart &&
             contractJHandle == key.jHandle &&
             layerIndex == key.layerIndex &&
             payloadWord108 == key.payloadWord108 &&
             payloadWord11C == key.payloadWord11C;
    }

    bool HasCompleteBacking() const {
      return captureComplete &&
             vertexCount != 0u &&
             positionBuffer != nullptr &&
             positionInfo.buffer != VK_NULL_HANDLE &&
             positionInfo.size != 0u &&
             (!indexed ||
              (indexBuffer != nullptr &&
               indexInfo.buffer != VK_NULL_HANDLE &&
               indexInfo.size != 0u &&
               indexCount != 0u));
    }

    bool HasCompleteAlphaPayload() const {
      if (!alphaTestEnabled || diffuseTexture == nullptr ||
          uvFormat == VK_FORMAT_UNDEFINED || uvStride == 0u ||
          uvOffset >= uvStride) {
        return false;
      }

      if (uvSharesPositionBuffer) {
        return positionBuffer != nullptr &&
               positionInfo.buffer != VK_NULL_HANDLE &&
               positionInfo.size != 0u;
      }

      return uvBuffer != nullptr &&
             uvInfo.buffer != VK_NULL_HANDLE &&
             uvInfo.size != 0u;
    }
    // capture 时的 D3DTS_WORLD 矩阵。
    // 动态单位（已 CPU skin）：通常是 identity，顶点已是世界空间。
    // 静态建筑（未 skin）：是该模型的 world 变换矩阵，顶点是模型本地空间。
    // consume 时直接用这个矩阵作为 draw.worldMatrix，避免误把本地空间顶点
    // 当世界空间，让静态建筑阴影跑到世界中心。
    Matrix4 capturedWorldMatrix = {};
    // UV buffer（用于 alpha-test shadow）。
    // 注意：可能与 positionBuffer 不在同一个 stream。capture 时如果 UV stream
    // 与 position stream 相同，复用 positionBuffer；否则单独 GPU copy 一份。
    Rc<DxvkBuffer> uvBuffer;
    Rc<DxvkResourceAllocation> uvPinnedAllocation;
    std::shared_ptr<War3Stage11SnapshotPage> uvSnapshotPage;
    VkDeviceSize uvSnapshotOffset = 0u;
    DxvkResourceBufferInfo uvInfo = {};
    uint32_t uvStride = 0u;
    uint32_t uvOffset = 0u;
    VkFormat uvFormat = VK_FORMAT_UNDEFINED;
    bool uvSharesPositionBuffer = false;
    VkDeviceSize uvCapacity = 0u;
    // capture 时的 alpha test / blend / texture 状态。
    // bypass capture 路径不会进入 legacy ShadowCapture 后半段，所以 alpha test
    // 信息必须在 v4 自己读取。
    bool alphaTestEnabled = false;
    bool alphaBlendEnabled = false;
    float alphaRef = 0.5f;
    Rc<DxvkImageView> diffuseTexture;
    // 已分配的 buffer 容量（bytes），用于复用避免反复 createBuffer。
    VkDeviceSize positionCapacity = 0u;
    VkDeviceSize indexCapacity = 0u;
    uint64_t frameSerial = 0u;
    // Observe-only cost prediction: ordinal of this full cache key's capture
    // within frameSerial, and the ordinal which the previous exact Stage11
    // submission selected. Neither field authorizes geometry reuse.
    uint32_t packageCaptureOrdinal = 0u;
    uint32_t packageLastSubmittedCaptureOrdinal = 0u;
    uint64_t submittedFrameSerial = 0u;
    // Written only by the exact current-frame Stage11 producer after the
    // entry has passed its geometry/visibility ownership gates. Generic and
    // lease paths must never write this marker.
    uint64_t exactOwnerFrameSerial = 0u;
    // Unlike exactOwnerFrameSerial, this is written only after the exact
    // current-frame caster was actually published.  Lifecycle/core code uses
    // it as positive live evidence; blocker/alpha fail-closed decisions must
    // never refresh object liveness.
    uint64_t exactSubmittedFrameSerial = 0u;
    uint32_t rawcode = 0u;
    uint32_t jHandle = 0u;
    bool pathBlocker = false;
    bool pathBlockerGeometryMarker = false;
    // ObjectKind can be inherited from stale TLS.  Only a current contract
    // plus live unit-object evidence may set this bit.
    bool unitIdentityProven = false;
    // GPU-skin output pages are dynamic, manager-retired storage. They are
    // valid only for this capture frame and must never enter static reuse.
    bool gpuSkinLeaseBacked = false;
    // VS-S1 与 compute output 共用同一帧 consumer fence；这里只保留
    // generation-pinned 静态输入和 palette storage 的值语义/强引用。
    War3GpuSkinDrawInput gpuSkinInput = {};
    war3::render::ObjectKind objectKind =
        static_cast<war3::render::ObjectKind>(0);
    // 2026-05-30 问题2（桥/斜坡卡顿）：静态几何标记。
    // 桥/斜坡/建筑/装饰物/可破坏物是静态几何——CPU skin 之后顶点不再变化，
    // worldMatrix 也固定。它们离开视野后被 16 帧 TTL 淘汰 + GPU buffer 释放，
    // 再次进入视野时必须重新 createBuffer + copyBuffer（vkAllocateMemory 同步
    // 阻塞主线程）→ 复现"看到桥/斜坡就卡，过一会好，离开再回来又卡"。
    // 标记为静态后用更长 TTL（近似常驻），再次进入视野直接 O(1) 复用已有
    // GPU buffer，不再 createBuffer。受 cache 总字节上限约束做 LRU 淘汰。
    bool isStaticGeometry = false;
    // 上次被消费端（producer/fast-append/consumer）实际复用的帧号。
    // 静态几何 LRU 淘汰时按此排序，保证"最近还在看的桥/斜坡"优先保留。
    uint64_t lastAccessFrameSerial = 0u;
    // 该 entry 持有的 GPU buffer 总字节（pos + uv + idx），用于字节上限统计。
    uint64_t ownedGpuBytes = 0u;
    // Phase 7.70：同帧重复捕获去重指纹。
    // 同一 renderablePart 在一帧里常被多次 draw（sub-mesh、layer pass、补光），
    // 每次都会重做 EmitCs(copyBuffer)。如果数据来源（VB/IB slice + range）没变，
    // 我们已经把该范围拷贝到自有 buffer，第二次进来只需更新 alpha-test / 世界
    // 矩阵 / 纹理这些便宜的状态，跳过 GPU copy 命令。指纹覆盖：
    //   - position 源 buffer 指针、偏移、本次 range（start/count/stride）
    //   - UV 源（若与 position 不同 stream）
    //   - index 源（若 indexed）
    //   - 一个简单 mix-hash，不持久化跨帧
    uint64_t lastCaptureFingerprint = 0u;
  };
  std::unordered_map<War3DrawTimeVBCacheKey, War3DrawTimeVBEntry,
                     War3DrawTimeVBCacheKeyHash>
      m_war3DrawTimeVBCache;
#if defined(WARVK_ENABLE_SHADOW_OBSERVERS_DEV) && \
    WARVK_ENABLE_SHADOW_OBSERVERS_DEV
  struct War3Stage11AllocationObserverProofHash {
    size_t operator()(
        const war3::render::War3ShadowGenerationBackedStreamProof& proof)
        const noexcept {
      size_t h = size_t(proof.ownerIdentity);
      const auto fold = [&h](uint64_t value) {
        h ^= size_t(value) + size_t(0x9e3779b9u) + (h << 6u) + (h >> 2u);
      };
      fold(proof.identityGeneration);
      fold(proof.allocationGeneration);
      fold(proof.contentGeneration);
      fold(proof.sourceOffset);
      fold(proof.sourceLength);
      fold(proof.elementStride);
      fold(proof.elementSize);
      fold(proof.mapEpoch);
      fold(proof.deviceEpoch);
      fold(static_cast<uint8_t>(proof.streamKind));
      return h;
    }
  };
  struct War3Stage11AllocationObserverProofEqual {
    bool operator()(
        const war3::render::War3ShadowGenerationBackedStreamProof& lhs,
        const war3::render::War3ShadowGenerationBackedStreamProof& rhs)
        const noexcept {
      return lhs.matches(rhs);
    }
  };
  static constexpr size_t kWar3Stage11AllocationObserverMaxProofs = 65536u;
  std::unordered_set<
      war3::render::War3ShadowGenerationBackedStreamProof,
      War3Stage11AllocationObserverProofHash,
      War3Stage11AllocationObserverProofEqual>
      m_war3Stage11AllocationObserverProofs;
  uint64_t m_war3Stage11AllocationObserverFrameSerial = 0u;
#endif
  // A positive current-frame rejection is an owner decision even when no VB
  // entry was published.  It prevents generic reconstruction and historical
  // packet leases from resurrecting a blocker after an early capture gate.
  std::unordered_set<War3DrawTimeVBCacheKey, War3DrawTimeVBCacheKeyHash>
      m_war3DrawTimeExactRejectedKeys;
  uint64_t m_war3DrawTimeExactRejectedFrameSerial = 0u;
  // Producer completeness is intentionally independent from the exact reject
  // owner set above: blocker/transparent fail-closed rejections protect
  // representation ownership, but do not by themselves prove a required
  // caster was omitted.
  std::unordered_set<War3DrawTimeVBCacheKey, War3DrawTimeVBCacheKeyHash>
      m_war3RequiredCasterOmissionKeys;
  uint64_t m_war3RequiredCasterOmissionFrameSerial = 0u;
  // Short-lived terminal witnesses for the verified anonymous 4v/6i marker
  // only. The value is the last exact-proof frame. This weaker slice identity
  // exists solely to stop a prior CurrentDraw/Grace representation from
  // resurrecting the rejected part; it never authorizes geometry reuse.
  std::unordered_map<War3DrawTimeAnonymousMarkerSliceKey, uint64_t,
                     War3DrawTimeAnonymousMarkerSliceKeyHash>
      m_war3DrawTimeAnonymousMarkerRejectedSlices;
  uint64_t m_war3DrawTimeVBCacheLastCleanFrame = 0u;
  uint64_t m_war3DrawTimeVBCacheStaticOverCapFrameCount = 0u;
  // S1 地形 legacy capture 分帧复用：period>1 时 off 帧注入上一批 stash，
  // 避免每帧数千 tile 全量 freeze（实测 ~6ms/帧）。
  std::vector<War3ShadowCasterDraw> m_war3S1TerrainCasterStash;
  uint64_t m_war3S1TerrainStashBuiltFrameSerial = 0u;
  uint64_t m_war3S1TerrainStashCaptureFrameSerial = 0u;
  // Phase A：S1 early cache（入口 O(1) 命中，跳过整条 ShadowCapture 热路径）。
  // key = worldMatrix + draw layout；value 持有已 freeze 的 GPU buffer。
  // period 保持 1：每帧只提交当前可见 tile；命中只省 re-copy/decl 解析。
  struct War3S1TerrainEarlyEntry {
    War3ShadowCasterDraw draw;
    uint64_t lastSeenFrame = 0u;
    // Non-zero entries alias a geometry owned by the persistent registry.
    // Early hits must validate and refresh that backing entry before use.
    uint32_t persistentGeometryId = 0u;
    uint64_t logicalReferencedBytes = 0u;
    // 冻结时的顶点/索引源身份（VB/IB 指针+offset+stride+draw 参数）。
    // early key 只含 worldMatrix+几何规模：identity world + 相同顶点数的
    // 不同 tile 会 key 碰撞，错误重放另一块地形的冻结几何。命中时必须
    // 比对源指纹，不匹配即淘汰重建，绝不重放。
    uint64_t sourceFingerprint = 0u;
  };
  using War3S1TerrainEarlyCache =
      std::unordered_map<uint64_t, War3S1TerrainEarlyEntry>;
  // Resources removed from the live map session remain strongly referenced
  // until the dedicated shadow-Arena completion fence proves that every
  // command recorded before the reset boundary has finished. This is the
  // ownership counterpart to Arena quarantine; clearing these containers at
  // map-unload time would still allow a command list to name freed backing.
  struct War3RetiredShadowSession {
    uint64_t mapEpoch = 0u;
    uint64_t retireSerial = 0u;
    // Census fields are computed once when the live containers move into this
    // fence-owned record. GPU logical bytes can contain aliases and are not a
    // unique residency measurement; allocator bytes are reported separately.
    uint64_t entryCount = 0u;
    uint64_t allocatorBytes = 0u;
    uint64_t cachedGpuLogicalBytes = 0u;
    uint64_t cpuOwnedBytes = 0u;
    std::array<War3ShadowBufferAllocator, 3> shadowAllocators;
    std::array<War3ShadowMappedBufferAllocator, 3> shadowMappedAllocators;
    std::array<War3ShadowFrozenGeometryCache, 3> frozenGeometryCaches;
    War3FrameFreezeCatalog frameFreezeCatalog;
    War3ShadowGeometryRegistry geometryRegistry;
    War3ShadowPersistentGeometryMap persistentGeometries;
    War3Stage13RetainedCasterMap stage13RetainedCasters;
    std::priority_queue<War3ShadowPersistentExpiryEntry,
                        std::vector<War3ShadowPersistentExpiryEntry>,
                        War3ShadowPersistentExpiryCompare>
        persistentExpiryQueue;
    std::unordered_map<War3DrawTimeVBCacheKey, War3DrawTimeVBEntry,
                       War3DrawTimeVBCacheKeyHash>
        drawTimeVbCache;
    std::vector<War3ShadowCasterDraw> s1TerrainCasterStash;
    War3S1TerrainEarlyCache s1TerrainEarlyCache;
  };
  std::vector<War3RetiredShadowSession> m_war3RetiredShadowSessions;
  War3S1TerrainEarlyCache m_war3S1TerrainEarlyCache;
  struct War3S1GenerationProofObservationEntry {
    dxvk::war3::render::War3ShadowGenerationStabilityState state = {};
    uint64_t lastSeenFrame = 0u;
  };
  std::unordered_map<uint64_t, War3S1GenerationProofObservationEntry>
      m_war3S1GenerationProofObservations;
  dxvk::war3::render::War3ShadowGenerationObservationClock
      m_war3S1GenerationProofObservationClock = {};
  uint64_t m_war3S1GenerationProofLastGcFrame = 0u;
  // One persistent geometry can back multiple S1 tiles because the persistent
  // key may omit worldMatrix while the early key includes it. Keep a one-to-many
  // reverse index so retiring a persistent geometry also releases every strong
  // Rc reference retained by its early aliases.
  std::unordered_multimap<uint32_t, uint64_t>
      m_war3S1TerrainEarlyKeysByPersistentGeometryId;
  uint64_t m_war3S1TerrainEarlyCacheLastGcFrame = 0u;
  uint64_t m_war3S1TerrainEarlyPersistentBackedCount = 0u;
  uint64_t m_war3S1TerrainEarlyFallbackBackedCount = 0u;
  uint64_t m_war3S1TerrainEarlyLogicalReferencedBytes = 0u;
  // Phase 7.123：per-frame GPU buffer alloc 预算，限制单帧 capture 端创建多少
  // 新的 device-local buffer。当一批新 caster 同时进入视野（如桥/斜坡/装饰物
  // 集中区域）时，原本会一次性触发数十次 createBuffer + EmitCs(copyBuffer)，
  // 阻塞主线程产生首帧暴降。预算耗尽后剩余的 cache miss 会被推到下一帧。
  // 视觉上 caster 的 shadow 第一帧缺失，第二帧才出现，但帧时长不再尖刺。
  uint32_t m_war3DrawTimeVBCacheAllocBudgetThisFrame = 0u;
  uint64_t m_war3DrawTimeVBCacheAllocBudgetFrame = 0u;
  // Phase 7.123：每帧因预算耗尽而被推迟的 cache miss 计数（诊断用）。
  uint32_t m_war3DrawTimeVBCacheBudgetDeferredCount = 0u;
  bool m_war3SemanticSceneLastZeroSubmitUnitsOnly = true;
  bool m_war3SemanticSceneLastZeroSubmitNativeValidation = false;
  bool m_war3SemanticSceneLastSuccessfulSubmitUnitsOnly = true;
  bool m_war3SemanticSceneLastSuccessfulSubmitNativeValidation = false;
  // Phase 7.1: 帧间 identity churn 追踪
  uint64_t m_war3SemanticDirectPrevIdentityHash = 0;
  // Phase 7.2: 帧间 contract 稳定性 churn 追踪
  uint64_t m_war3SemanticDirectPrevPaletteHash = 0;
  uint64_t m_war3SemanticDirectPrevGroupHash = 0;
  uint64_t m_war3SemanticDirectPrevStableGroupHash = 0;
  uint64_t m_war3SemanticDirectPrevStream1Ptr = 0;
  uint64_t m_war3SemanticDirectPrevGeometrySourceHash = 0;
  uint64_t m_war3SemanticDirectPrevSceneNode = 0;
  uint64_t m_war3SemanticDirectPrevRenderablePart = 0;
  uint64_t m_war3SemanticDirectPrevMeshData = 0;
  std::vector<uint64_t> m_war3SemanticDirectPrevSubmittedIdentityKeys;
  std::vector<uint64_t> m_war3SemanticDirectPrevSubmittedObjectIdentityKeys;
  std::vector<uint64_t> m_war3SemanticDirectPrevSubmittedPartIdentityKeys;
  std::unordered_map<uint64_t, uint64_t>
      m_war3SemanticDirectSelectionLeaseLastSeen;
  struct War3SemanticDirectPartPacketLeaseState;
  std::unique_ptr<War3SemanticDirectPartPacketLeaseState>
      m_war3SemanticDirectPartPacketLeaseState;
  uint64_t m_war3ShadowTombstoneSerialSeen = 0u;
  War3SemanticDirectCasterContractMap m_war3SemanticDirectCasterContracts;
  bool m_war3SemanticSceneLastReusableUnitsOnly = true;
  bool m_war3SemanticSceneLastReusableNativeValidation = false;
  bool m_war3SemanticSceneLastSuccessfulSubmitComplete = false;
  uint64_t m_war3ShadowPersistentBytesUsed = 0;
  uint64_t m_war3ShadowPersistentBytesEvicted = 0;
  War3ShadowPersistentDiagnosticsFrame
      m_war3ShadowPersistentDiagnosticsFrame = {};
  uint64_t m_war3ShadowFallbackBudgetCapBytes = 0;
  uint64_t m_war3ShadowFallbackBudgetUsedBytes = 0;
  bool m_war3ShadowFallbackBudgetExceeded = false;

  Rc<DxvkBuffer> War3AllocFreezeBuffer(VkDeviceSize size,
                                       VkDeviceSize &outOffset,
                                       bool hostVisible = false,
                                       void** outMapPtr = nullptr);
  bool War3DrainShadowCasterTombstones();
  War3ShadowSemanticContext War3BuildShadowSemanticContext(
      const dxvk::war3::render::RenderObjectInfo* currentObj) const;
  War3ShadowReplayMode War3ClassifyShadowReplayMode(
      bool vertexBlendEnabled, bool vertexBlendIndexed) const;
  bool War3CanPromoteShadowPersistentGeometry(
      const War3ShadowSemanticContext& semantic, War3ShadowReplayMode mode,
      bool objectCaster, bool indexed, bool captureAlphaTest,
      bool alphaBlendEnabled, bool dynamicSysmemVBOs, bool dynamicSysmemIBO,
      bool posDynamic, bool blendDynamic, bool ibDynamic, uint32_t blendBinding,
      const Rc<DxvkBuffer>& posStorage,
      const Rc<DxvkBuffer>& blendStorage,
      const Rc<DxvkBuffer>& indexStorage) const;
  bool War3TryPublishSemanticDrawTimePose();
  bool War3CreateShadowPersistentBuffer(
                                        const War3ShadowPersistentUpload& upload,
                                        Rc<DxvkBuffer>& outStorage,
                                        DxvkResourceBufferInfo& outInfo);
  bool War3TryFindShadowPersistentGeometry(
      const War3ShadowGeometryRegistryKey& key,
      uint32_t& outGeometryId,
      const War3ShadowPersistentGeometry*& outGeometry);
  bool War3FindOrCreateShadowPersistentGeometry(
      const War3ShadowGeometryRegistryKey& key,
      const War3ShadowPersistentGeometry& candidate,
      const std::array<War3ShadowPersistentUpload, 4>& uploads,
      uint32_t& outGeometryId,
      const War3ShadowPersistentGeometry*& outGeometry,
      bool& outCreatedNew);
  bool War3CreateShadowPersistentGeometryAfterMiss(
      const War3ShadowGeometryRegistryKey& key,
      const War3ShadowPersistentGeometry& candidate,
      const std::array<War3ShadowPersistentUpload, 4>& uploads,
      uint32_t& outGeometryId,
      const War3ShadowPersistentGeometry*& outGeometry,
      bool& outCreatedNew,
      War3ShadowPersistentCreateFailure* outFailure = nullptr);
  void War3GcShadowPersistentGeometry();
  void War3StoreS1TerrainEarlyCacheEntry(
      uint64_t key, const War3ShadowCasterDraw& draw,
      uint32_t persistentGeometryId, uint64_t sourceFingerprint);
  uint64_t War3ComputeS1TerrainSourceFingerprint(
      bool indexed, INT baseVertexIndex, UINT minVertexIndex,
      UINT startVal) const;
  void War3EraseS1TerrainEarlyCacheEntry(uint64_t key);
  void War3EraseS1TerrainEarlyAliasesForPersistentGeometry(
      uint32_t persistentGeometryId);
  void War3GcS1TerrainEarlyCache();
  void War3GcS1GenerationProofObservations();
  War3Stage11SnapshotAllocationResult War3AllocateStage11Snapshot(
      VkDeviceSize requiredBytes,
      std::shared_ptr<War3Stage11SnapshotPage>& outPage,
      VkDeviceSize& outOffset, VkDeviceSize& outCapacity);
  void War3CollectUnusedStage11SnapshotPages();
  void War3ResetStage11SnapshotPages();
  void War3TryCaptureShadowCaster(D3DPRIMITIVETYPE PrimitiveType,
                                  INT BaseVertexIndex, UINT MinVertexIndex,
                                  UINT NumVertices, UINT StartVal,
                                  UINT CountVal, bool indexed,
                                  bool DynamicSysmemVBOs,
                                  bool DynamicSysmemIBO,
                                  const war3::gpu_skin::GpuSkinResolvedDraw*
                                      gpuSkinResolved);
  bool War3CaptureShadowDrawMetadata(
      D3DPRIMITIVETYPE primitiveType, INT baseVertexIndex,
      UINT minVertexIndex, UINT numVertices, UINT startVal, UINT countVal,
      bool indexed, bool dynamicSysmemVbos,
      const War3ShadowSemanticContext& semantic, int stage,
      War3RenderState::StageCategory category, War3BatchTag batchTag);
  bool War3TryAppendSemanticShadowPacket(
      const dxvk::war3::shadow::ShadowDrawPacket& packet);
  bool War3TryAppendSemanticShadowPacket(
      const dxvk::war3::shadow::ShadowDrawPacket& packet,
      const dxvk::war3::render::CurrentDrawAuthoritativeSample*
          directCurrentDrawSample);
  // Phase 7.30 Step A probe：多接一个"是否来自 core stale-pose restore"的标记，
  // 提交端 palette 探针据此把 LargeDelta 归因到 stale→live 过渡帧。
  // 默认重载保持旧语义，新增参数默认 false。
  bool War3TryAppendSemanticShadowPacket(
      const dxvk::war3::shadow::ShadowDrawPacket& packet,
      const dxvk::war3::render::CurrentDrawAuthoritativeSample*
          directCurrentDrawSample,
      bool fromStalePoseRestore);
  bool War3TryAppendSemanticShadowPacket(
      const dxvk::war3::shadow::ShadowDrawPacket& packet,
      const dxvk::war3::render::CurrentDrawAuthoritativeSample*
          directCurrentDrawSample,
      bool fromStalePoseRestore,
      bool currentFrameExactOwnerPrefiltered);
  void War3MarkDrawTimeExactRejectedCurrentFrame(
      const War3DrawTimeVBCacheKey& key);
  void War3RecordRequiredCasterOmission(
      const War3DrawTimeVBCacheKey& key,
      War3RequiredCasterOmissionReason reason);
  void War3RecordRequiredCasterOmission(
      War3RequiredCasterOmissionReason reason);
  void War3SealShadowProducerCompleteness(War3FrameScene& scene,
                                          uint64_t frameSerial,
                                          uint64_t mapEpoch,
                                          uint64_t deviceEpoch) const;
  bool War3DrawTimeExactRejectedCurrentFrame(
      const War3DrawTimeVBCacheKey& key) const;
  void War3RememberDrawTimeAnonymousMarkerRejection(
      const War3DrawTimeVBCacheKey& key);
  bool War3DrawTimeAnonymousMarkerRejectionActive(
      void* renderablePart, void* meshPayloadPtr,
      uint32_t layerIndex) const;
  uint32_t War3TryPopulateDrawTimeSemanticProducer(
      std::vector<dxvk::war3::render::CurrentDrawContractRecord>&
          exactSubmittedManifestRecords);
  void War3ObservePersistentPackageStage11Evidence(
      const War3DrawTimeVBCacheKey& key,
      const War3DrawTimeVBEntry& entry, int16_t exactProducerStage,
      bool blockerClassified,
      war3::gpu_skin::War3PersistentGpuPackageStage11ObserveAdapter::Mode
          requestedMode) noexcept;
  void War3ObservePersistentPackageD3D9Owner(
      const War3DrawTimeVBCacheKey& key,
      const War3DrawTimeVBEntry& entry,
      const war3::gpu_skin::
          War3PersistentGpuPackageStage11ObserveAdapter::Evidence&
              evidence) noexcept;
  uint32_t War3GetOrCreateSemanticShadowPalette(
      const dxvk::war3::shadow::ShadowDrawPacket& packet,
      dxvk::war3::render::ObjectKind resolvedObjectKind,
      const Matrix4* overrideMatrices = nullptr,
      uint32_t overrideMatrixCount = 0u,
      uint64_t overrideMatrixHash = 0u);
  // Phase 7.1: 将两处 direct current-draw loop 合并为 object-grouped submit helper
  uint32_t War3TryPopulateDirectCurrentDrawGrouped(
      bool readyOnly,
      bool unitsOnly,
      uint64_t currentVisibleFrameSerial,
      uint64_t currentDrawMinVisibleFrameSerial,
      const std::vector<dxvk::war3::render::CurrentDrawContractRecord>&
          exactSubmittedManifestRecords);
  uint32_t War3TryPopulateSemanticShadowScene(
      bool unitsOnly,
      bool executeNativeBackendValidation = false);
  void War3ResetShadowAllocator() {
    // Map unload may be requested before the Present safe point reaches this
    // helper (including through the non-Ex Present wrapper). Never rewind a
    // legacy/fallback generation while the old session is quarantined; the
    // transition moves all backing into the fence-retired session instead.
    if (!m_war3ShadowSessionReady.load(std::memory_order_acquire) ||
        m_war3ShadowMapResetRequestedSerial.load(std::memory_order_acquire) !=
            m_war3ShadowMapResetAppliedSerial ||
        m_war3ShadowDeviceEpochRequested.load(std::memory_order_acquire) !=
            m_war3ShadowDeviceEpochApplied.load(std::memory_order_acquire) ||
        m_war3ShadowDeviceRebindPending.load(std::memory_order_acquire))
      return;
    // Reset the allocator for the NEXT frame (to be used in next BeginFrame
    // cycle)
    m_war3ShadowAllocators[(m_war3FrameIndex + 1) % 3].Reset();
    m_war3ShadowMappedAllocators[(m_war3FrameIndex + 1) % 3].Reset();
    m_war3ShadowFrozenGeometryCaches[(m_war3FrameIndex + 1) % 3].clear();
    const uint64_t nextFrameSerial = m_war3ShadowPersistentFrameSerial + 1u;
    if (m_war3FrameFreezeCatalogSerial != nextFrameSerial) {
      m_war3FrameFreezeCatalog.clear();
      m_war3FrameFreezeCatalogSerial = nextFrameSerial;
      m_war3FrameFreezeUniqueSourceBytes = 0u;
      m_war3FrameFreezeDuplicateBytesSaved = 0u;
    }
  }

  // War3 Shadow
  War3ShadowReceiverPass *m_shadowReceiverPass = nullptr;
  War3SsaoPass *m_ssaoPass = nullptr;
  War3AAPass *m_aaPass = nullptr;
  War3PostProcess *m_war3PostProcess = nullptr;
  dxvk::war3::shadow::DxvkValidationBackend m_war3SemanticDxvkBackend;
  Com<IDirect3DVertexShader9> m_shadowFakeVS;
  Com<IDirect3DPixelShader9> m_shadowFakePS;

  bool m_unlockAdditionalFormats = false;

  // War3 Ambient Tracking
  DWORD m_pureGameAmbient = 0x00000000;

public:
  DWORD GetPureGameAmbient() const { return m_pureGameAmbient; }
  War3ShadowReceiverPass* GetWar3ShadowReceiverPass() const {
    return m_shadowReceiverPass;
  }
  uint32_t War3PopulateSemanticShadowSceneForValidation(
      bool unitsOnly,
      bool executeNativeBackendValidation = false) {
    return War3TryPopulateSemanticShadowScene(
        unitsOnly, executeNativeBackendValidation);
  }
  bool War3ExecuteSemanticShadowSceneForValidation(
      bool unitsOnly,
      bool executeNativeBackendValidation = false);
  bool War3SubmitSemanticShadowPacketForBackend(
      const dxvk::war3::shadow::ShadowDrawPacket& packet) {
    return War3TryAppendSemanticShadowPacket(packet);
  }

  // War3 Frame Index (0-2) for Ring Buffer Synchronization
  uint32_t m_war3FrameIndex = 0;
};

} // namespace dxvk
