// Incomplete, isolated x86 D3D9 COM profile. Never name/deploy this as the game
// d3d9.dll. Unsupported interfaces fail; windowed Reset follows native D3D9.
// WSI requires an explicit isolated profile and a separately pumping HWND owner.
#include "d3d9_unsupported.h"
#include "d3d9_connection.h"
#include "d3d9_draw_wire.h"
#include "d3d9_topology.h"
#include "d3d9_shader_tokens.h"
#include "d3d9_factory_wire.h"
#include "d3d9_declaration_wire.h"
#include "d3d9_ff_wire.h"
#include "d3d9_texture_wire.h"
#include "d3d9_sampler_wire.h"
#include "d3d9_effects_wire.h"
#include "d3d9_texture_stages_wire.h"
#include "d3d9_memory_probe.h"
#include "d3d9_residency.h"
#include "../../src/util/com/com_object.h"
#include <atomic>
#include <cmath>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

static_assert(sizeof(void*)==4,"This COM compatibility proxy is x86 only");
namespace warvk::host::d3d9proxy {
class Device;
class Factory;
class Surface;
class VertexBuffer;class IndexBuffer;class Texture;class VertexShader;class PixelShader;class Declaration;
class StateBlock;
struct LightState {D3DLIGHT9 value{};bool defined=false,enabled=false;};
// API state belongs to the x86 owner. Blocks retain private resource references;
// they never own a second host state machine or keep a Device/recording cycle.
struct CapturedState {
  CapturedState()=default;
  CapturedState(const CapturedState&)=delete;
  CapturedState& operator=(const CapturedState&)=delete;
  ~CapturedState();
  std::array<DWORD,256> render{};std::array<bool,256> renderMask{};
  d9::FixedFunctionState fixed{};
  std::array<bool,11> transformMask{};
  std::array<std::array<bool,11>,8> stageMask{};
  std::array<d9::SamplerState,8> samplers{};
  std::array<std::array<bool,d9::SamplerStateWords>,8> samplerMask{};
  std::array<std::array<float,128>,2> constants{};
  std::array<std::array<bool,32>,2> constantMask{};
  std::array<LightState,32> lights{};std::array<bool,32> enabledMask{};
  bool lightsMask=false,materialMask=false,viewportMask=false,scissorMask=false;
  bool vbMask=false,ibMask=false,vsMask=false,psMask=false,declMask=false,freqMask=false;
  D3DVIEWPORT9 viewport{};RECT scissor{};UINT offset=0,stride=0;
  VertexBuffer* vb=nullptr;IndexBuffer* ib=nullptr;VertexShader* vs=nullptr;
  PixelShader* ps=nullptr;Declaration* decl=nullptr;
  std::array<Texture*,8> textures{};std::array<bool,8> textureMask{};
};
static bool ReadSelf(const void* source,void* target,size_t bytes);
template<class I> class Child;
using VB=Child<IDirect3DVertexBuffer9>;
static d9::PresentationProfile PresenterProfile() {
  wchar_t value[64]{};
  const auto size=GetEnvironmentVariableW(L"WARVK_D3D9_PROFILE",value,64);
  if(!size)return d9::PresentationProfile::Offscreen;
  if(size<64&&std::wstring(value)==L"presenter-isolated-v1")return d9::PresentationProfile::IsolatedPresenter;
  if(size<64&&std::wstring(value)==L"presenter-readback-isolated-v1")return d9::PresentationProfile::IsolatedPresenterReadback;
  throw std::runtime_error("unknown D3D9 development profile");
}

static void Caps(D3DCAPS9& c) {
  c={};c.DeviceType=D3DDEVTYPE_HAL;c.VertexShaderVersion=D3DVS_VERSION(3,0);c.PixelShaderVersion=D3DPS_VERSION(3,0);
  c.MaxVertexShaderConst=32;c.MaxStreams=1;c.MaxStreamStride=256;
  c.MaxTextureWidth=c.MaxTextureHeight=d9::MaxTextureDimension;c.MaxSimultaneousTextures=d9::PixelTextureStages;c.NumSimultaneousRTs=1;
  c.MaxTextureBlendStages=d9::PixelTextureStages;
  c.MaxPrimitiveCount=d9::MaxDrawPrimitives;c.MaxVertexIndex=65535;c.TextureCaps=D3DPTEXTURECAPS_MIPMAP;
  c.ZCmpCaps=D3DPCMPCAPS_NEVER|D3DPCMPCAPS_LESS|D3DPCMPCAPS_EQUAL|D3DPCMPCAPS_LESSEQUAL
    |D3DPCMPCAPS_GREATER|D3DPCMPCAPS_NOTEQUAL|D3DPCMPCAPS_GREATEREQUAL|D3DPCMPCAPS_ALWAYS;
  c.SrcBlendCaps=c.DestBlendCaps=D3DPBLENDCAPS_ZERO|D3DPBLENDCAPS_ONE|D3DPBLENDCAPS_SRCCOLOR|D3DPBLENDCAPS_INVSRCCOLOR
    |D3DPBLENDCAPS_SRCALPHA|D3DPBLENDCAPS_INVSRCALPHA|D3DPBLENDCAPS_DESTALPHA|D3DPBLENDCAPS_INVDESTALPHA
    |D3DPBLENDCAPS_DESTCOLOR|D3DPBLENDCAPS_INVDESTCOLOR|D3DPBLENDCAPS_SRCALPHASAT;
  c.MaxAnisotropy=16;
  c.AlphaCmpCaps=D3DPCMPCAPS_NEVER|D3DPCMPCAPS_LESS|D3DPCMPCAPS_EQUAL|D3DPCMPCAPS_LESSEQUAL
    |D3DPCMPCAPS_GREATER|D3DPCMPCAPS_NOTEQUAL|D3DPCMPCAPS_GREATEREQUAL|D3DPCMPCAPS_ALWAYS;
  c.RasterCaps=D3DPRASTERCAPS_FOGVERTEX|D3DPRASTERCAPS_FOGTABLE|D3DPRASTERCAPS_FOGRANGE|D3DPRASTERCAPS_SCISSORTEST
    |D3DPRASTERCAPS_DEPTHBIAS;
  c.PrimitiveMiscCaps=D3DPMISCCAPS_SEPARATEALPHABLEND|D3DPMISCCAPS_CULLNONE|D3DPMISCCAPS_CULLCW|D3DPMISCCAPS_CULLCCW
    |D3DPMISCCAPS_BLENDOP;
  c.TextureFilterCaps=D3DPTFILTERCAPS_MINFPOINT|D3DPTFILTERCAPS_MINFLINEAR|D3DPTFILTERCAPS_MINFANISOTROPIC
    |D3DPTFILTERCAPS_MAGFPOINT|D3DPTFILTERCAPS_MAGFLINEAR|D3DPTFILTERCAPS_MAGFANISOTROPIC
    |D3DPTFILTERCAPS_MIPFPOINT|D3DPTFILTERCAPS_MIPFLINEAR;
  c.TextureAddressCaps=D3DPTADDRESSCAPS_WRAP|D3DPTADDRESSCAPS_MIRROR|D3DPTADDRESSCAPS_CLAMP|D3DPTADDRESSCAPS_BORDER|D3DPTADDRESSCAPS_MIRRORONCE;
  // These are the restricted DEV profile limits, not full D3D9 compatibility or
  // the physical adapter caps. Factory/format queries not negotiated fail.
}
static HRESULT NegotiatedCaps(Connection& connection,D3DCAPS9* output) {
  if(!output)return D3DERR_INVALIDCALL;
  d9::Header h;h.op=d9::Op::FactoryQuery;h.args[0]=uint32_t(d9::FactoryQuery::Caps);h.args[1]=D3DDEVTYPE_HAL;
  std::array<uint8_t,d9::FactoryCapsBytes> bytes{};
  const auto hr=connection.call(h,nullptr,bytes.data(),uint32_t(bytes.size()));
  if(hr!=D3D_OK)return hr;
  D3DCAPS9 physical{};
  if(!d9::DecodeCaps(bytes.data(),uint32_t(bytes.size()),physical)){connection.failClosed();return D3DERR_DEVICELOST;}
  // Actual backend capability intersected with implemented COM/wire semantics.
  // Broad physical FF/mipmap/format caps are never exposed by this limited proxy.
  D3DCAPS9 supported{};Caps(supported);
  supported.MaxTextureWidth=std::min(supported.MaxTextureWidth,physical.MaxTextureWidth);
  supported.MaxTextureHeight=std::min(supported.MaxTextureHeight,physical.MaxTextureHeight);
  supported.TextureCaps&=physical.TextureCaps;
  supported.TextureFilterCaps&=physical.TextureFilterCaps;supported.TextureAddressCaps&=physical.TextureAddressCaps;
  supported.MaxAnisotropy=std::min(supported.MaxAnisotropy,physical.MaxAnisotropy);
  supported.AlphaCmpCaps&=physical.AlphaCmpCaps;supported.RasterCaps&=physical.RasterCaps;
  supported.ZCmpCaps&=physical.ZCmpCaps;supported.SrcBlendCaps&=physical.SrcBlendCaps;supported.DestBlendCaps&=physical.DestBlendCaps;
  supported.PrimitiveMiscCaps&=physical.PrimitiveMiscCaps;
  supported.MaxVertexShaderConst=std::min(supported.MaxVertexShaderConst,physical.MaxVertexShaderConst);
  supported.MaxPrimitiveCount=std::min(supported.MaxPrimitiveCount,physical.MaxPrimitiveCount);
  supported.MaxVertexIndex=std::min(supported.MaxVertexIndex,physical.MaxVertexIndex);
  supported.MaxStreams=std::min(supported.MaxStreams,physical.MaxStreams);
  supported.MaxStreamStride=std::min(supported.MaxStreamStride,physical.MaxStreamStride);
  supported.NumSimultaneousRTs=std::min(supported.NumSimultaneousRTs,physical.NumSimultaneousRTs);
  supported.MaxSimultaneousTextures=std::min(supported.MaxSimultaneousTextures,physical.MaxSimultaneousTextures);
  supported.MaxTextureBlendStages=std::min(supported.MaxTextureBlendStages,physical.MaxTextureBlendStages);
  supported.VertexShaderVersion=std::min(supported.VertexShaderVersion,physical.VertexShaderVersion);
  supported.PixelShaderVersion=std::min(supported.PixelShaderVersion,physical.PixelShaderVersion);
  *output=supported;return D3D_OK;
}
// The implicit swap chain this profile implements, for CreateDevice and Reset:
// windowed, one DISCARD back buffer of A8R8G8B8 or X8R8G8B8, an auto depth
// buffer of D32F_LOCKABLE or D16 (both D32_SFLOAT, as in WarVK native), and the
// windowed intervals native accepts. A zero axis takes the client extent;
// presenting needs client == back buffer.
struct SwapChainMode {
  UINT width=0,height=0;D3DFORMAT format=D3DFMT_A8R8G8B8;uint32_t interval=0;
};
static bool ResolveSwapChain(const D3DPRESENT_PARAMETERS& p,HWND hwnd,bool presenter,SwapChainMode& mode) {
  if(!p.Windowed||(p.BackBufferFormat!=D3DFMT_A8R8G8B8&&p.BackBufferFormat!=D3DFMT_X8R8G8B8)
    ||p.BackBufferCount>1||p.MultiSampleType!=D3DMULTISAMPLE_NONE||p.MultiSampleQuality
    ||p.SwapEffect!=D3DSWAPEFFECT_DISCARD||!p.EnableAutoDepthStencil
    ||(p.AutoDepthStencilFormat!=D3DFMT_D32F_LOCKABLE&&p.AutoDepthStencilFormat!=D3DFMT_D16)
    ||p.Flags||p.FullScreen_RefreshRateInHz||(p.hDeviceWindow&&p.hDeviceWindow!=hwnd)
    ||(p.PresentationInterval!=D3DPRESENT_INTERVAL_DEFAULT&&p.PresentationInterval!=D3DPRESENT_INTERVAL_ONE
      &&p.PresentationInterval!=D3DPRESENT_INTERVAL_IMMEDIATE))return false;
  RECT client{};
  if(!GetClientRect(hwnd,&client)||client.left||client.top)return false;
  mode.width=p.BackBufferWidth?p.BackBufferWidth:UINT(client.right);
  mode.height=p.BackBufferHeight?p.BackBufferHeight:UINT(client.bottom);
  if(!d9::ValidTargetExtent(mode.width,mode.height)
    ||(presenter&&(UINT(client.right)!=mode.width||UINT(client.bottom)!=mode.height)))return false;
  mode.format=p.BackBufferFormat;
  // Offscreen sessions never present; WSI folds DEFAULT into ONE, as native does.
  mode.interval=!presenter?0:p.PresentationInterval==D3DPRESENT_INTERVAL_IMMEDIATE?d9::IntervalImmediate:d9::IntervalOne;
  return true;
}
template<class I> HRESULT Return(I* object,I** out) {
  if(!out)return D3DERR_INVALIDCALL;*out=object;if(object)object->AddRef();return D3D_OK;
}
template<class T> void Bind(T*& slot,T* value) {
  if(value)value->AddRefPrivate();if(slot)slot->ReleasePrivate();slot=value;
}

// The device window's WM_SIZE, as WarVK's window hook answers it
// (src/d3d9/d3d9_window.cpp): a windowed WM_SIZE never reaches the
// application. The window thread only records the client extent; the owner
// thread resets the swap chain at its next size-dependent call
// (Device::followWindow). The last device on a window owns its hook.
struct WindowSize {std::atomic<uint64_t> client{0},messages{0};};
struct WindowHook {WNDPROC previous=nullptr;bool unicode=false;WindowSize* size=nullptr;};
static std::mutex windowHooksMutex;
static std::unordered_map<HWND,WindowHook> windowHooks;
static LRESULT CALLBACK DeviceWindowProc(HWND window,UINT message,WPARAM wp,LPARAM lp){
  WindowHook hook;
  {std::lock_guard<std::mutex> lock(windowHooksMutex);const auto it=windowHooks.find(window);
    if(it!=windowHooks.end()) {
      if(message==WM_SIZE&&it->second.size) {
        it->second.size->messages.fetch_add(1,std::memory_order_relaxed);RECT client{};
        if(wp!=SIZE_MINIMIZED&&GetClientRect(window,&client)&&client.right>0&&client.bottom>0)
          it->second.size->client.store(uint64_t(client.right)<<32|uint32_t(client.bottom),std::memory_order_release);
      }
      hook=it->second;
      if(message==WM_NCDESTROY)windowHooks.erase(it);
    }}
  const bool unicode=hook.previous?hook.unicode:IsWindowUnicode(window)!=FALSE;
  if(!hook.previous||message==WM_SIZE)
    return unicode?DefWindowProcW(window,message,wp,lp):DefWindowProcA(window,message,wp,lp);
  return unicode?CallWindowProcW(hook.previous,window,message,wp,lp):CallWindowProcA(hook.previous,window,message,wp,lp);
}

// Unset keeps the legacy full x86 copies. A decimal KiB budget (0 included)
// requests host content authority; malformed values refuse device creation.
static bool BackingBudget(bool& enabled,uint64_t& budget) {
  wchar_t value[16]{};enabled=false;budget=0;
  const auto size=GetEnvironmentVariableW(L"WARVK_D3D9_X86_BACKING_KIB",value,16);
  if(!size)return true;
  if(size>7)return false;
  uint64_t kib=0;
  for(DWORD i=0;i<size;++i){if(value[i]<L'0'||value[i]>L'9')return false;kib=kib*10+uint64_t(value[i]-L'0');}
  if(kib>4194304)return false;
  enabled=true;budget=kib*1024;return true;
}

class Device final : public dxvk::ComObjectClamp<Unsupported<IDirect3DDevice9>> {
public:
  Device(Factory* f,HWND hwnd,DWORD flags,d9::PresentationProfile profile,const SwapChainMode& mode);
  ~Device();
  HRESULT initialize();
  HRESULT diagnosticCheckpoint(UINT phase,UINT ordinal,d9::memory::Checkpoint* output,UINT bytes);
  void accountPayload(d9::memory::Kind kind,uint64_t logical,uint64_t size,uint64_t capacity,bool add) noexcept {
    // Normal COM operations retain their existing owner contract. A foreign
    // final Release invalidates this diagnostic; it must not race plain data.
    if(!conn->isOwner()){memoryForeignOwner.store(true,std::memory_order_release);return;}
    if(add)memoryAccount.add(kind,logical,size,capacity);else memoryAccount.remove(kind,logical,size,capacity);
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
    if(!out)return E_POINTER;*out=nullptr;
    if(iid!=IID_IUnknown&&iid!=__uuidof(IDirect3DDevice9))return E_NOINTERFACE;
    *out=static_cast<IDirect3DDevice9*>(this);AddRef();return S_OK;
  }
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override {
    if(!conn->healthy())return D3DERR_DEVICELOST;return notReset?D3DERR_DEVICENOTRESET:D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS*) override;
  HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** out) override;
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* out) override {return NegotiatedCaps(*conn,out);}
  HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* out) override {
    if(!out)return D3DERR_INVALIDCALL;*out={0,D3DDEVTYPE_HAL,window,behavior};return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT,DWORD,DWORD,D3DPOOL,IDirect3DVertexBuffer9**,HANDLE*) override;
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DIndexBuffer9**,HANDLE*) override;
  HRESULT STDMETHODCALLTYPE CreateTexture(UINT,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DTexture9**,HANDLE*) override;
  HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9*,IDirect3DVertexDeclaration9**) override;
  HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD*,IDirect3DVertexShader9**) override;
  HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD*,IDirect3DPixelShader9**) override;
  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE,IDirect3DQuery9**) override;
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT,UINT,D3DFORMAT,D3DPOOL,IDirect3DSurface9**,HANDLE*) override;
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT,UINT,D3DBACKBUFFER_TYPE,IDirect3DSurface9**) override;
  HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD index,IDirect3DSurface9** out) override {return GetBackBuffer(index,0,D3DBACKBUFFER_TYPE_MONO,out);}
  HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD,IDirect3DSurface9*) override;
  HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9*,IDirect3DSurface9*) override;
  HRESULT STDMETHODCALLTYPE SetStreamSource(UINT,IDirect3DVertexBuffer9*,UINT,UINT) override;
  HRESULT STDMETHODCALLTYPE GetStreamSource(UINT,IDirect3DVertexBuffer9**,UINT*,UINT*) override;
  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9*) override;
  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9**) override;
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD,IDirect3DBaseTexture9*) override;
  HRESULT STDMETHODCALLTYPE GetTexture(DWORD,IDirect3DBaseTexture9**) override;
  HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9*) override;
  HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9**) override;
  HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9*) override;
  HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9**) override;
  HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9*) override;
  HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9**) override;
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD) override;
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD*) override;
  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE,const D3DMATRIX*) override;
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE,D3DMATRIX*) override;
  HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE,const D3DMATRIX*) override;
  HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9*) override;
  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9*) override;
  HRESULT STDMETHODCALLTYPE SetLight(DWORD,const D3DLIGHT9*) override;
  HRESULT STDMETHODCALLTYPE GetLight(DWORD,D3DLIGHT9*) override;
  HRESULT STDMETHODCALLTYPE LightEnable(DWORD,BOOL) override;
  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD,BOOL*) override;
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD,D3DTEXTURESTAGESTATETYPE,DWORD) override;
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD,D3DTEXTURESTAGESTATETYPE,DWORD*) override;
  HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE,IDirect3DStateBlock9**) override;
  HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
  HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9**) override;
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream,UINT setting) override {
    if(!allowed()||stream||setting!=1)return D3DERR_INVALIDCALL;
    if(recording)recording->freqMask=true;return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream,UINT* setting) override {
    if(stream||!setting)return D3DERR_INVALIDCALL;*setting=1;return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* v) override {
    followWindow();
    if(!allowed()||!v||v->X>width||v->Y>height||!v->Width||!v->Height||v->Width>width-v->X||v->Height>height-v->Y
      ||!std::isfinite(v->MinZ)||!std::isfinite(v->MaxZ)||v->MinZ<0||v->MaxZ>1||v->MinZ>v->MaxZ)return D3DERR_INVALIDCALL;
    if(recording){recording->viewport=*v;recording->viewportMask=true;}
    else draw.viewport=*v;return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* v) override {followWindow();if(!v)return D3DERR_INVALIDCALL;*v=draw.viewport;return D3D_OK;}
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE,DWORD) override;
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE,DWORD*) override;
  HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* value) override {
    followWindow();RECT copy{};if(!allowed()||!ReadSelf(value,&copy,sizeof(copy)))return D3DERR_INVALIDCALL;
    if(recording){recording->scissor=copy;recording->scissorMask=true;}
    else effects.scissor={copy.left,copy.top,copy.right,copy.bottom};return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* value) override {
    followWindow();if(!value)return D3DERR_INVALIDCALL;
    *value={effects.scissor[0],effects.scissor[1],effects.scissor[2],effects.scissor[3]};return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler,D3DSAMPLERSTATETYPE t,DWORD value) override {
    if(!allowed()||sampler>=d9::PixelTextureStages)return D3DERR_INVALIDCALL;
    if(!d9::ValidSamplerValue(t,value))return D3DERR_INVALIDCALL;
    if(recording){recording->samplers[sampler].values[t]=value;recording->samplerMask[sampler][t]=true;}
    else (sampler?textureStages.samplers[sampler-1]:samplerState).values[t]=value;return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD sampler,D3DSAMPLERSTATETYPE t,DWORD* value) override {
    if(sampler>=d9::PixelTextureStages||!value)return D3DERR_INVALIDCALL;
    if(uint32_t(t)==0||uint32_t(t)>=d9::SamplerStateWords)return D3DERR_INVALIDCALL;
    *value=(sampler?textureStages.samplers[sampler-1]:samplerState).values[t];return D3D_OK;
  }
  HRESULT constants(bool vs,bool set,UINT first,float* data,UINT count) {
    if(!allowed()||first>32||count>32-first||(count&&!data))return D3DERR_INVALIDCALL;
    auto& c=vs?draw.vertexFloatConstants:draw.pixelFloatConstants;
    if(set){for(UINT i=0;i<count*4;++i)if(!std::isfinite(data[i]))return D3DERR_INVALIDCALL;
      if(count){
        if(recording){auto& target=recording->constants[vs?0:1];std::memcpy(target.data()+first*4,data,count*16);
          for(UINT i=first;i<first+count;++i)recording->constantMask[vs?0:1][i]=true;}
        else std::memcpy(c.data()+first*4,data,count*16);
      }
    } else if(count)std::memcpy(data,c.data()+first*4,count*16);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT s,const float* p,UINT n) override {return constants(true,true,s,const_cast<float*>(p),n);}
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT s,float* p,UINT n) override {return constants(true,false,s,p,n);}
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT s,const float* p,UINT n) override {return constants(false,true,s,const_cast<float*>(p),n);}
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT s,float* p,UINT n) override {return constants(false,false,s,p,n);}
  HRESULT STDMETHODCALLTYPE BeginScene() override {followWindow();if(!allowed()||scene)return D3DERR_INVALIDCALL;scene=true;return D3D_OK;}
  HRESULT STDMETHODCALLTYPE EndScene() override {if(!allowed()||!scene)return D3DERR_INVALIDCALL;scene=false;return conn->endScene();}
  HRESULT STDMETHODCALLTYPE Clear(DWORD,const D3DRECT*,DWORD,D3DCOLOR,float,DWORD) override;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT) override;
  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE,UINT,UINT) override;
  HRESULT STDMETHODCALLTYPE Present(const RECT* src,const RECT* dst,HWND win,const RGNDATA* dirty) override {
    // A window resize is taken after the frame, where the game's own message
    // pump runs; a presenter host skips a frame drawn before it.
    const HRESULT hr=presentFrame(src,dst,win,dirty);followWindow();return hr;
  }
  HRESULT presentFrame(const RECT* src,const RECT* dst,HWND win,const RGNDATA* dirty) {
    if(conn->isOwner()&&(!conn->healthy()||notReset))return D3DERR_DEVICELOST;
    if(!allowed()||src||dst||win||dirty||scene)return D3DERR_INVALIDCALL;
    d9::Header h;h.op=d9::Op::Present;
    // A window resized ahead of its Reset: the host drops the frame and the
    // previous one stays on screen (native would stretch it).
    const auto skipped=[](HRESULT hr){return hr==HRESULT(d9::PresentSkipped)?D3D_OK:hr;};
    if(presented.empty())return skipped(conn->call(h));
    if(d9::PresentedInline(width,height))return skipped(conn->call(h,nullptr,presented.data(),uint32_t(presented.size())));
    // A larger frozen frame arrives in bands after the Present reply names it.
    // The frame number is published only once every band has arrived.
    std::array<uint8_t,8> reply{};
    HRESULT hr=conn->call(h,nullptr,reply.data(),uint32_t(reply.size()));if(hr!=D3D_OK)return skipped(hr);
    d9::P64(presented.data(),0);
    const uint64_t frame=d9::U64(reply.data());
    if(!frame){conn->failClosed();return D3DERR_DEVICELOST;}
    hr=readRows(d9::ReadbackSource::Presented,frame,presented.data()+8);if(hr!=D3D_OK)return hr;
    std::memcpy(presented.data(),reply.data(),8);return D3D_OK;
  }
  HRESULT readPresented(void* data,UINT size,UINT64* frame) const {
    if(!conn->isOwner())return D3DERR_INVALIDCALL;
    if(!conn->healthy())return D3DERR_DEVICELOST;
    if(presented.empty()||!data||size!=presented.size()-8||!frame||!d9::U64(presented.data()))return D3DERR_INVALIDCALL;
    std::memcpy(data,presented.data()+8,size);*frame=d9::U64(presented.data());return D3D_OK;
  }
  // Whole image when it fits one reply, else bands of whole rows, into a
  // tightly packed width*height*4 destination.
  HRESULT readRows(d9::ReadbackSource source,uint64_t frame,uint8_t* output) {
    const uint32_t pitch=width*4;
    if(uint64_t(pitch)*height<=d9::MaxReplyPayload) {
      d9::Header h;h.op=d9::Op::Readback;h.args[2]=uint32_t(source);h.args[3]=uint32_t(frame);h.args[4]=uint32_t(frame>>32);
      return conn->call(h,nullptr,output,pitch*height);
    }
    for(uint32_t first=0;first<height;) {
      const uint32_t rows=std::min(height-first,d9::ReadbackBandRows(width));
      d9::Header h;h.op=d9::Op::Readback;h.args[0]=first;h.args[1]=rows;h.args[2]=uint32_t(source);
      h.args[3]=uint32_t(frame);h.args[4]=uint32_t(frame>>32);
      const HRESULT hr=conn->call(h,nullptr,output+size_t(first)*pitch,rows*pitch);if(hr!=D3D_OK)return hr;
      first+=rows;
    }
    return D3D_OK;
  }
  UINT targetWidth() const {return width;}
  UINT targetHeight() const {return height;}
  D3DFORMAT backBufferFormat() const {return format;}
  // After a refused Reset only Release, TestCooperativeLevel and Reset proceed.
  bool allowed() const {return conn->isOwner()&&conn->healthy()&&!notReset;}
  // The window's own resize (DeviceWindowProc), taken by the owner thread at
  // its next size-dependent call outside a scene: the back buffer, viewport and
  // scissor rectangle, BeginScene, Clear, state blocks, and after Present.
  void followWindow(){
    if(windowSize.client.load(std::memory_order_acquire)&&!scene&&allowed())
      resizeToWindow(windowSize.client.exchange(0,std::memory_order_acq_rel));
  }
  // Native's losable count: DEFAULT-pool buffers, implicit back buffers and
  // state blocks. Reset succeeds only once the application released them all.
  uint32_t losable=0;
  d9::ResourceRef reserveResource(d9::Type type) {
    return allowed()?resources.reserve(type):d9::ResourceRef{};
  }
  void cancelResourceReservation(d9::ResourceRef ref,d9::Type type) noexcept {
    // Never recycle after a transport/terminal failure: the peer may already
    // own the resource even if no successful reply reached this process.
    if(!conn->healthy())return;
    if(!conn->isOwner()||!resources.cancelReservation(ref,type))conn->failClosed();
  }
  bool commitResourceReservation(d9::ResourceRef ref,d9::Type type) noexcept {
    if(!allowed()||!resources.commitReservation(ref,type)){conn->failClosed();return false;}
    return true;
  }
  bool resourceMatches(d9::ResourceRef ref,d9::Type type) const {
    return resources.matches(ref,type);
  }
  void destroyResource(d9::ResourceRef ref,d9::Type type) noexcept {
    // A previously latched remote terminal keeps its original classification.
    // It permits local COM cleanup, never a new IPC or a recycled handle.
    if(!conn->healthy())return;
    if(!conn->isOwner()||!resources.matches(ref,type)){conn->failClosed();return;}
    d9::Header h;h.op=d9::Op::Destroy;h.id=ref.id;h.generation=ref.generation;h.args[0]=uint32_t(type);
    const HRESULT hr=conn->call(h);
    if(hr!=D3D_OK){if(conn->healthy())conn->failClosed();return;}
    // For GPU resources the host acknowledges only after its production GPU,
    // resource-use and Presenter retirement. Query destruction only revokes
    // its CPU observation identity; it does not cancel pending GPU commands.
    if(!resources.destroy(ref,type))conn->failClosed();
  }
  std::shared_ptr<Connection> conn;
  Residency residency;
  // Ordered state: admitted without its reply in the asynchronous profile.
  HRESULT ordered(d9::Header h,const void* payload,bool passStaged=false) {
    return conn->asyncOrderedEnabled()?conn->enqueueOrdered(h,payload,passStaged):conn->call(h,payload);
  }
private:
  friend class StateBlock;
  std::unique_ptr<CapturedState> recording;
  void capture(CapturedState&,bool preserveOffset);
  HRESULT apply(const CapturedState&);
  void resetState();
  bool recordRender(D3DRENDERSTATETYPE key,DWORD value) {
    if(!recording)return false;
    recording->render[key]=value;recording->renderMask[key]=true;return true;
  }
  Factory* factory;
  HWND window;
  DWORD behavior;
  d9::PresentationProfile profile=d9::PresentationProfile::Offscreen;
  UINT width,height; // The back buffer extent; Reset changes it.
  D3DFORMAT format;uint32_t interval; // Back buffer format and the wire interval.
  bool notReset=false;
  bool markerSet=false;
  DWORD dither=FALSE;
  std::vector<uint8_t> presented; // Diagnostic profile only: frame number, then the image.
  std::wstring marker;
  WindowSize windowSize; // Written by the window thread through windowHooks.
  bool windowHooked=false;
  uint64_t windowResizes=0,windowResizesRefused=0;
  bool hookWindow();
  void unhookWindow() noexcept;
  void resizeToWindow(uint64_t client);
  d9::ResourceRegistry resources;
  bool scene=false;
  Surface* back=nullptr;
  dxvkcore::D3D9Backend::DrawState draw{};
  d9::FixedFunctionState fixed{};
  d9::SamplerState samplerState{},sentSamplerState{};bool samplerSent=false;
  d9::EffectsState effects{},sentEffects{};
  d9::TextureStages textureStages{},sentTextureStages{};
  HRESULT syncTextureStages();
  std::array<LightState,32> lights{};
  HRESULT submit(d9::DrawCommand& command) {
    const auto stageResult=syncTextureStages();if(stageResult!=D3D_OK)return stageResult;
    if(!(effects==sentEffects)) {
      d9::Header h;h.op=d9::Op::SetEffects;h.payloadBytes=d9::EffectsBytes;
      const auto bytes=d9::EncodeEffects(effects);const auto hr=ordered(h,bytes.data());
      if(hr!=D3D_OK){if(SUCCEEDED(hr))conn->failClosed();return FAILED(hr)?hr:D3DERR_DEVICELOST;}
      sentEffects=effects;
    }
    // Coalesce setters locally. The existing ordered call flushes the prior
    // owned draw prefix before publishing a changed sampler, never per setter.
    if(!samplerSent||samplerState.values!=sentSamplerState.values) {
      d9::Header stateHeader;stateHeader.op=d9::Op::SetSamplerState;stateHeader.payloadBytes=d9::SamplerStateBytes;
      const auto payload=d9::EncodeSamplerState(samplerState);const auto hr=ordered(stateHeader,payload.data());
      if(hr!=D3D_OK){if(SUCCEEDED(hr))conn->failClosed();return FAILED(hr)?hr:D3DERR_DEVICELOST;}
      sentSamplerState=samplerState;samplerSent=true;
    }
    if(vs&&ps)return conn->submitDraw(d9::EncodeDraw(command));
    fixed.lightCount=0;
    for(const auto& light:lights)if(light.enabled)fixed.lights[fixed.lightCount++]=light.value;
    // A batched refusal is terminal, so keep the host's fixed-state verdict
    // recoverable here, before anything is staged. Setters already validate.
    if(conn->fixedBatchingEnabled()&&!d9::ValidFixedFunction(fixed))return D3DERR_INVALIDCALL;
    return conn->submitFixedDraw(d9::EncodeFixedDraw(command,fixed));
  }
  // Private bindings keep resources alive without keeping the device alive
  // cyclically. External child refs keep their original COM device identity.
  class VertexBuffer* vb=nullptr;
  class IndexBuffer* ib=nullptr;
  class Texture* tex=nullptr;
  std::array<class Texture*,d9::AdditionalTextureStages> additionalTextures{};
  class VertexShader* vs=nullptr;
  class PixelShader* ps=nullptr;
  class Declaration* decl=nullptr;
  // Native m_fvfTable: one declaration per FVF while the device lives, so an
  // FVF switch rebinds instead of creating a host declaration. Bounded here;
  // past the bound SetFVF creates one as before.
  static constexpr size_t MaxFvfDeclarations=64;
  std::vector<class Declaration*> fvfDeclarations;
  uint64_t fvfEvictions=0;
  d9::memory::CpuAccount memoryAccount;
  std::atomic<bool> memoryForeignOwner{false};
  uint64_t lastMemorySequence=0;
};

// Local allocation may fail before any packet is sent. A transmitted create is
// uncertain until its exact reply arrives: only the host's two documented
// no-side-effect preflight refusals permit rollback. Unknown results (including
// S_FALSE), bad payloads, disconnects and backend OOM remain fail-closed.
class ResourceReservation final {
public:
  ResourceReservation(Device* parent,d9::Type type):parent(parent),type(type),ref(parent->reserveResource(type)){}
  ~ResourceReservation(){if(mayCancel&&ref.id)parent->cancelResourceReservation(ref,type);}
  ResourceReservation(const ResourceReservation&)=delete;
  ResourceReservation& operator=(const ResourceReservation&)=delete;
  HRESULT create(d9::Header h,const void* payload=nullptr) noexcept {
    if(!ref.id)return E_OUTOFMEMORY;
    h.id=ref.id;h.generation=ref.generation;
    mayCancel=false;
    const HRESULT hr=parent->conn->call(h,payload);
    if(hr==D3D_OK)
      return parent->commitResourceReservation(ref,type)?D3D_OK:D3DERR_DEVICELOST;
    if(parent->conn->healthy()&&(hr==D3DERR_INVALIDCALL||hr==D3DERR_OUTOFVIDEOMEMORY)) {
      mayCancel=true;return hr;
    }
    if(parent->conn->healthy())parent->conn->failClosed();
    return D3DERR_DEVICELOST;
  }
  Device* const parent;
  const d9::Type type;
  const d9::ResourceRef ref;
private:
  bool mayCancel=true;
};

template<class I> class Child : public dxvk::ComObjectClamp<Unsupported<I>> {
public:
  Child(Device* p,d9::ResourceRef r,d9::Type t):parent(p),ref(r),type(t){}
  ~Child(){if(remoteLive)parent->destroyResource(ref,type);}
  d9::Header command(d9::Op op) const {
    d9::Header h;h.op=op;h.id=ref.id;h.generation=ref.generation;return h;
  }
  bool resourceAllowed() const {
    if(!parent->allowed())return false;
    if(!remoteLive||!parent->resourceMatches(ref,type)){parent->conn->failClosed();return false;}
    return true;
  }
  bool publicReferencesZero() const noexcept {return this->m_refCount.load(std::memory_order_acquire)==0;}
  ULONG STDMETHODCALLTYPE AddRef() override {
    const uint32_t n=this->m_refCount++;
    if(!n){this->AddRefPrivate();parent->AddRef();}return n+1;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    uint32_t old=this->m_refCount.load();
    do{if(!old)return 0;}while(!this->m_refCount.compare_exchange_weak(old,old-1));
    if(old==1){auto* p=parent;this->ReleasePrivate();p->Release();}return old-1;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
    if(!out)return E_POINTER;*out=nullptr;
    bool supported=iid==IID_IUnknown||iid==__uuidof(I);
    if constexpr(std::is_base_of_v<IDirect3DResource9,I>)supported|=iid==__uuidof(IDirect3DResource9);
    if constexpr(std::is_base_of_v<IDirect3DBaseTexture9,I>)supported|=iid==__uuidof(IDirect3DBaseTexture9);
    if(!supported)return E_NOINTERFACE;
    *out=static_cast<I*>(this);AddRef();return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** out) override {return Return(static_cast<IDirect3DDevice9*>(parent),out);}
  Device* const parent;const d9::ResourceRef ref;const d9::Type type;bool remoteLive=false;
};

template<class I> class Buffer : public Child<I>, private ContentOwner {
public:
  Buffer(Device* d,d9::ResourceRef ref,uint32_t size,DWORD usage,bool index)
      :Child<I>(d,ref,index?d9::Type::IndexBuffer:d9::Type::VertexBuffer),extent(size),usage(usage){
    content.bytes=&bytes;content.owner=this;
    // A budgeted copy is created by its first Lock or draw, not by creation.
    if(!d->residency.enabled()){bytes.resize(size);d->residency.allocated(size);}
    account(true);++d->losable;
  }
  ~Buffer(){account(false);this->parent->residency.discard(content);--this->parent->losable;}
  // Local index validation reads this copy. An evicted one is restored first,
  // and the draw refreshes its place in the LRU order on release.
  HRESULT acquireContent() noexcept {
    if(bytes.empty()){const auto hr=restore(false);if(hr!=D3D_OK)return hr;}
    this->parent->residency.pin(content);return D3D_OK;
  }
  void releaseContent() noexcept {this->parent->residency.unpin(content);}
  void account(bool add) noexcept {
    if(add){accountedSize=bytes.size();accountedCapacity=bytes.capacity();}
    this->parent->accountPayload(this->type==d9::Type::IndexBuffer?d9::memory::Kind::IndexBuffer
      :d9::memory::Kind::VertexBuffer,extent,accountedSize,accountedCapacity,add);
  }
  void contentEvicted() noexcept override {account(false);account(true);}
  HRESULT STDMETHODCALLTYPE Lock(UINT offset,UINT size,void** out,DWORD flags) override {
    if(out)*out=nullptr;
    // NOSYSLOCK is a scheduling hint only, as in DXVK.
    if(!this->resourceAllowed()||!out||locked||offset>extent
      ||(flags&~DWORD(D3DLOCK_DISCARD|D3DLOCK_NOOVERWRITE|D3DLOCK_NOSYSLOCK))
      ||((flags&(D3DLOCK_DISCARD|D3DLOCK_NOOVERWRITE))&&!(usage&D3DUSAGE_DYNAMIC))
      ||((flags&D3DLOCK_DISCARD)&&(flags&D3DLOCK_NOOVERWRITE)))return D3DERR_INVALIDCALL;
    if(!size)size=extent-offset;
    if(!size||size>extent-offset||offset>initializedPrefix)return D3DERR_INVALIDCALL;
    if(bytes.empty()) {
      const auto hr=restore((flags&D3DLOCK_DISCARD)&&this->type==d9::Type::VertexBuffer);if(hr!=D3D_OK)return hr;
    }
    this->parent->residency.pin(content);
    // The GPU never reads this x86 mapping; Unlock sends the range in order.
    // The host waits for earlier GPU use only for a plain Lock: it writes a
    // NOOVERWRITE range directly and renames the storage on DISCARD.
    locked=true;lockOffset=offset;lockBytes=size;lockFlags=flags;*out=bytes.data()+offset;return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE Unlock() override {
    if(!this->resourceAllowed()||!locked)return D3DERR_INVALIDCALL;
    const bool discard=lockFlags&D3DLOCK_DISCARD,dynamic=discard||(lockFlags&D3DLOCK_NOOVERWRITE);
    for(uint32_t i=0;i<lockBytes;) {
      auto h=this->command(d9::Op::UploadBuffer);h.args[0]=lockOffset+i;
      // Only the first chunk discards; the rest land in the new storage.
      h.args[1]=uint32_t(discard&&!i?d9::UploadMode::Discard:dynamic?d9::UploadMode::NoOverwrite:d9::UploadMode::Ordered);
      h.payloadBytes=std::min(d9::MaxPayload,lockBytes-i);
      const HRESULT hr=this->parent->ordered(h,bytes.data()+lockOffset+i,h.args[1]==uint32_t(d9::UploadMode::NoOverwrite));
      if(FAILED(hr))return hr;
      if(hr!=D3D_OK){this->parent->conn->failClosed();return D3DERR_DEVICELOST;}
      initializedPrefix=std::max(initializedPrefix,lockOffset+i+h.payloadBytes);i+=h.payloadBytes;
    }
    locked=false;
    this->parent->residency.unpin(content);
    return D3D_OK;
  }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {return this->type==d9::Type::IndexBuffer?D3DRTYPE_INDEXBUFFER:D3DRTYPE_VERTEXBUFFER;}
  const uint32_t extent;
  std::vector<uint8_t> bytes;DWORD usage;bool locked=false;uint32_t lockOffset=0,lockBytes=0,initializedPrefix=0;DWORD lockFlags=0;
private:
  // Only an evicted buffer gets here. The host retains every byte of its
  // initialized prefix. After a vertex DISCARD the old contents are undefined,
  // and the WRITEONLY profile never lets the caller read them. Index content
  // is always restored: local validation must see the bytes the host draws.
  HRESULT restore(bool discard) noexcept {
    auto& residency=this->parent->residency;auto& conn=*this->parent->conn;
    try{bytes.resize(extent);}catch(...){return E_OUTOFMEMORY;}
    residency.allocated(extent);
    uint32_t calls=0;
    for(uint32_t at=0;!discard&&at<initializedPrefix;++calls) {
      const auto count=std::min(d9::MaxContentRead,initializedPrefix-at);
      auto h=this->command(d9::Op::ReadContent);h.args[1]=at;h.args[2]=count;
      if(conn.call(h,nullptr,bytes.data()+at,count)!=D3D_OK) {
        residency.discard(content);account(false);account(true);
        if(conn.healthy())conn.failClosed();return D3DERR_DEVICELOST;
      }
      at+=count;
    }
    if(discard)residency.discardMiss();else if(calls)residency.fetched(initializedPrefix,calls);
    account(false);account(true);return D3D_OK;
  }
  ResidentContent content;
  uint64_t accountedSize=0,accountedCapacity=0;
};
class VertexBuffer final:public Buffer<IDirect3DVertexBuffer9>{
public:VertexBuffer(Device*d,d9::ResourceRef ref,uint32_t n,DWORD u):Buffer(d,ref,n,u,false){}
  DWORD fvf=0;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* p) override{if(!p)return D3DERR_INVALIDCALL;*p={D3DFMT_VERTEXDATA,D3DRTYPE_VERTEXBUFFER,usage,D3DPOOL_DEFAULT,UINT(extent),fvf};return D3D_OK;}
};
class IndexBuffer final:public Buffer<IDirect3DIndexBuffer9>{
public:IndexBuffer(Device*d,d9::ResourceRef ref,uint32_t n,DWORD u):Buffer(d,ref,n,u,true){}
  HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* p) override{if(!p)return D3DERR_INVALIDCALL;*p={D3DFMT_INDEX16,D3DRTYPE_INDEXBUFFER,usage,D3DPOOL_DEFAULT,UINT(extent)};return D3D_OK;}
};
class Texture;
class TextureSurface final:public Unsupported<IDirect3DSurface9>{
public:
  Texture* texture=nullptr;UINT level=0;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override;
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** out) override;
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID iid,void** out) override;
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override{return D3DRTYPE_SURFACE;}
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* out) override;
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* out,const RECT* rect,DWORD flags) override;
  HRESULT STDMETHODCALLTYPE UnlockRect() override;
private:
  std::atomic<uint32_t> references{0};
};
class Texture final:public Child<IDirect3DTexture9>,private ContentOwner{
public:
  HRESULT checkedCall(const d9::Header& h,const void* data=nullptr) {
    const auto hr=parent->conn->call(h,data);
    if(SUCCEEDED(hr)&&hr!=D3D_OK){parent->conn->failClosed();return D3DERR_DEVICELOST;}return hr;
  }
  Texture(Device*d,d9::ResourceRef ref,const d9::TextureLayout& l):Child(d,ref,d9::Type::Texture),layout(l){
    for(UINT i=0;i<l.levels;++i){surfaces[i].texture=this;surfaces[i].level=i;content[i].bytes=&levelBytes[i];content[i].owner=this;}
    // A budgeted level copy is created by its first LockRect.
    if(!d->residency.enabled()){for(UINT i=0;i<l.levels;++i)levelBytes[i].resize(l.level[i].bytes);d->residency.allocated(l.bytes);}
    account(true);
  }
  ~Texture(){account(false);for(UINT i=0;i<layout.levels;++i)parent->residency.discard(content[i]);}
  void account(bool add) noexcept {
    if(add){accountedSize=accountedCapacity=0;
      for(const auto& level:levelBytes){accountedSize+=level.size();accountedCapacity+=level.capacity();}}
    parent->accountPayload(d9::memory::Kind::Texture,layout.bytes,accountedSize,accountedCapacity,add);
  }
  void contentEvicted() noexcept override {account(false);account(true);}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType()override{return D3DRTYPE_TEXTURE;}
  DWORD STDMETHODCALLTYPE GetLevelCount()override{return layout.levels;}
  DWORD STDMETHODCALLTYPE GetLOD()override{return lod;}
  DWORD STDMETHODCALLTYPE SetLOD(DWORD next)override{
    const auto old=lod;if(!resourceAllowed()||locked)return old;
    next=std::min<DWORD>(next,layout.levels-1);if(next==lod)return old;
    auto h=command(d9::Op::TextureLod);h.args[0]=next;
    if(checkedCall(h)==D3D_OK)lod=next;return old;
  }
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level,D3DSURFACE_DESC* p)override{
    if(level>=layout.levels||!p)return D3DERR_INVALIDCALL;const auto& s=layout.level[level];
    *p={D3DFORMAT(layout.format),D3DRTYPE_SURFACE,0,D3DPOOL_MANAGED,D3DMULTISAMPLE_NONE,0,s.width,s.height};return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level,IDirect3DSurface9** out)override{
    if(out)*out=nullptr;if(!resourceAllowed()||level>=layout.levels||!out)return D3DERR_INVALIDCALL;
    surfaces[level].AddRef();*out=&surfaces[level];return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE LockRect(UINT level,D3DLOCKED_RECT* p,const RECT* r,DWORD flags)override{
    if(!resourceAllowed()||level>=layout.levels||!p||(flags&~DWORD(D3DLOCK_READONLY))||locks[level].active)return D3DERR_INVALIDCALL;
    const auto& s=layout.level[level];RECT rect{0,0,LONG(s.width),LONG(s.height)};
    if(r&&!ReadSelf(r,&rect,sizeof(rect)))return D3DERR_INVALIDCALL;
    d9::TextureRegion selected;
    if(rect.left<0||rect.top<0||rect.right<=rect.left||rect.bottom<=rect.top
        ||!d9::MakeManagedLockRegion(layout,level,uint32_t(rect.left),uint32_t(rect.top),uint32_t(rect.right),uint32_t(rect.bottom),selected))
      return D3DERR_INVALIDCALL;
    if(levelBytes[level].empty()){const auto hr=restore(level);if(hr!=D3D_OK)return hr;}
    parent->residency.pin(content[level]);
    locks[level]={selected,true,bool(flags&D3DLOCK_READONLY)};++locked;
    p->Pitch=int(s.pitch);p->pBits=levelBytes[level].data()+d9::TextureByteOffset(layout,level,selected.x,selected.y)-s.offset;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT level)override{
    if(!resourceAllowed()||level>=layout.levels||!locks[level].active)return D3DERR_INVALIDCALL;
    auto& lock=locks[level];
    if(lock.readOnly){lock.active=false;--locked;parent->residency.unpin(content[level]);return D3D_OK;}
    if(revision==UINT32_MAX)return D3DERR_INVALIDCALL;
    auto patch=lock.region;const auto& s=layout.level[level];
    // Local backing is value-initialized. The first commit initializes the
    // whole level, including otherwise undefined texels outside a partial lock.
    if(!(initializedLevels&(1u<<level)))patch={level,0,0,s.width,s.height,s.bytes};
    auto h=command(d9::Op::BeginTextureUpload);h.args[0]=revision+1;h.args[1]=level;
    h.args[2]=patch.x;h.args[3]=patch.y;h.args[4]=patch.width;h.args[5]=patch.height;
    HRESULT hr=checkedCall(h);if(hr!=D3D_OK)return hr;++revision;
    std::array<uint8_t,d9::MaxPayload> payload{};
    const auto rowBytes=d9::TextureRowBytes(layout,patch.width);
    const auto origin=d9::TextureByteOffset(layout,level,patch.x,patch.y)-s.offset;
    const auto* source=levelBytes[level].data();
    for(uint32_t offset=0;offset<patch.bytes&&hr==D3D_OK;) {
      const auto count=std::min(d9::MaxPayload,patch.bytes-offset);uint32_t filled=0;
      while(filled<count) {
        const auto logical=offset+filled,row=logical/rowBytes,column=logical%rowBytes;
        const auto span=std::min(count-filled,rowBytes-column);
        std::memcpy(payload.data()+filled,source+origin+row*s.pitch+column,span);filled+=span;
      }
      h=command(d9::Op::TextureUploadChunk);h.args[0]=revision;h.args[1]=offset;h.payloadBytes=count;
      hr=checkedCall(h,payload.data());offset+=count;
    }
    if(hr==D3D_OK){h=command(d9::Op::CommitTextureUpload);h.args[0]=revision;hr=checkedCall(h);}
    if(hr!=D3D_OK){if(parent->conn->healthy()){h=command(d9::Op::AbortTextureUpload);h.args[0]=revision;(void)checkedCall(h);}return hr;}
    initializedLevels|=1u<<level;uploaded=initializedLevels==((1u<<layout.levels)-1);lock.active=false;--locked;
    parent->residency.unpin(content[level]);return D3D_OK;
  }
  struct LevelLock {d9::TextureRegion region;bool active=false,readOnly=false;};
  std::array<LevelLock,d9::MaxTextureLevels> locks{};
  std::array<TextureSurface,d9::MaxTextureLevels> surfaces{};
  d9::TextureLayout layout;
  uint32_t lod=0,revision=0,initializedLevels=0,locked=0;bool uploaded=false;
private:
  // An evicted initialized level is read back from the host's retained copy;
  // an uninitialized one starts value-initialized, as at creation.
  HRESULT restore(UINT level) noexcept {
    auto& residency=parent->residency;auto& bytes=levelBytes[level];const auto extent=layout.level[level].bytes;
    try{bytes.resize(extent);}catch(...){return E_OUTOFMEMORY;}
    residency.allocated(extent);
    uint32_t calls=0;
    for(uint32_t at=0;(initializedLevels&(1u<<level))&&at<extent;++calls) {
      const auto count=std::min(d9::MaxContentRead,extent-at);
      auto h=command(d9::Op::ReadContent);h.args[0]=level;h.args[1]=at;h.args[2]=count;
      if(parent->conn->call(h,nullptr,bytes.data()+at,count)!=D3D_OK) {
        residency.discard(content[level]);account(false);account(true);
        if(parent->conn->healthy())parent->conn->failClosed();return D3DERR_DEVICELOST;
      }
      at+=count;
    }
    if(calls)residency.fetched(extent,calls);
    account(false);account(true);return D3D_OK;
  }
  std::array<std::vector<uint8_t>,d9::MaxTextureLevels> levelBytes{};
  std::array<ResidentContent,d9::MaxTextureLevels> content{};
  uint64_t accountedSize=0,accountedCapacity=0;
};
// A subresource has its own public COM identity and borrows its container's
// private lifetime. It cannot keep a public device/container reference cycle.
ULONG TextureSurface::AddRef(){const auto n=references++;if(!n){texture->AddRefPrivate();texture->parent->AddRef();}return n+1;}
ULONG TextureSurface::Release(){
  uint32_t n=references.load();do{if(!n)return 0;}while(!references.compare_exchange_weak(n,n-1));
  if(n==1){auto* owner=texture->parent;texture->ReleasePrivate();owner->Release();}return n-1;
}
HRESULT TextureSurface::QueryInterface(REFIID iid,void** out){
  if(!out)return E_POINTER;*out=nullptr;
  if(iid!=IID_IUnknown&&iid!=__uuidof(IDirect3DResource9)&&iid!=__uuidof(IDirect3DSurface9))return E_NOINTERFACE;
  AddRef();*out=static_cast<IDirect3DSurface9*>(this);return S_OK;
}
HRESULT TextureSurface::GetDevice(IDirect3DDevice9** out){return texture->GetDevice(out);}
HRESULT TextureSurface::GetContainer(REFIID iid,void** out){return texture->QueryInterface(iid,out);}
HRESULT TextureSurface::GetDesc(D3DSURFACE_DESC* out){return texture->GetLevelDesc(level,out);}
HRESULT TextureSurface::LockRect(D3DLOCKED_RECT* out,const RECT* rect,DWORD flags){return texture->LockRect(level,out,rect,flags);}
HRESULT TextureSurface::UnlockRect(){return texture->UnlockRect(level);}
class Surface final:public Child<IDirect3DSurface9>{
public:
  Surface(Device*d,bool system):Child(d,d9::ResourceRef{},d9::Type::None),system(system),
    width(d->targetWidth()),height(d->targetHeight()),format(d->backBufferFormat()),
    bytes(system?size_t(width)*height*4:0){account(true);if(!system)++d->losable;}
  ~Surface(){account(false);if(!system)--parent->losable;}
  void account(bool add) noexcept {if(system)parent->accountPayload(d9::memory::Kind::SystemSurface,
    bytes.size(),bytes.size(),bytes.capacity(),add);}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType()override{return D3DRTYPE_SURFACE;}
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* p)override{
    if(!p)return D3DERR_INVALIDCALL;*p={format,D3DRTYPE_SURFACE,system?0u:DWORD(D3DUSAGE_RENDERTARGET),
      system?D3DPOOL_SYSTEMMEM:D3DPOOL_DEFAULT,D3DMULTISAMPLE_NONE,0,width,height};return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* p,const RECT* r,DWORD flags)override{
    if(!parent->allowed()||!system||!p||r||locked||(flags&~DWORD(D3DLOCK_READONLY)))return D3DERR_INVALIDCALL;
    locked=true;p->Pitch=INT(width*4);p->pBits=bytes.data();return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE UnlockRect()override{if(!parent->allowed()||!locked)return D3DERR_INVALIDCALL;locked=false;return D3D_OK;}
  bool system,locked=false;UINT width,height;D3DFORMAT format;std::vector<uint8_t> bytes;
};
template<class I>class Shader:public Child<I>{
public:
  Shader(Device*d,d9::ResourceRef ref,d9::Type t,std::vector<uint32_t> data):Child<I>(d,ref,t),tokens(std::move(data)){account(true);}
  ~Shader(){account(false);}
  void account(bool add) noexcept {this->parent->accountPayload(this->type==d9::Type::VertexShader
    ?d9::memory::Kind::VertexShader:d9::memory::Kind::PixelShader,
    tokens.size()*sizeof(uint32_t),tokens.size()*sizeof(uint32_t),tokens.capacity()*sizeof(uint32_t),add);}
  HRESULT STDMETHODCALLTYPE GetFunction(void* p,UINT* n)override{
    if(!n)return D3DERR_INVALIDCALL;const auto size=UINT(tokens.size()*4);
    if(!p){*n=size;return D3D_OK;}if(*n<size){*n=size;return D3DERR_INVALIDCALL;}
    std::memcpy(p,tokens.data(),size);*n=size;return D3D_OK;
  }
  std::vector<uint32_t>tokens;
};
class VertexShader final:public Shader<IDirect3DVertexShader9>{public:VertexShader(Device*d,d9::ResourceRef ref,std::vector<uint32_t> v):Shader(d,ref,d9::Type::VertexShader,std::move(v)){} };
class PixelShader final:public Shader<IDirect3DPixelShader9>{public:PixelShader(Device*d,d9::ResourceRef ref,std::vector<uint32_t> v):Shader(d,ref,d9::Type::PixelShader,std::move(v)){} };
class Declaration final:public Child<IDirect3DVertexDeclaration9>{
public:
  Declaration(Device*d,d9::ResourceRef ref,std::vector<D3DVERTEXELEMENT9> v):Child(d,ref,d9::Type::Declaration),elements(std::move(v)){account(true);}
  ~Declaration(){account(false);}
  void account(bool add) noexcept {parent->accountPayload(d9::memory::Kind::Declaration,
    elements.size()*sizeof(D3DVERTEXELEMENT9),elements.size()*sizeof(D3DVERTEXELEMENT9),elements.capacity()*sizeof(D3DVERTEXELEMENT9),add);}
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9* p,UINT* n)override{
    if(!n)return D3DERR_INVALIDCALL;const auto count=UINT(elements.size());
    if(!p){*n=count;return D3D_OK;}if(*n<count){*n=count;return D3DERR_INVALIDCALL;}
    std::memcpy(p,elements.data(),count*sizeof(*p));*n=count;return D3D_OK;
  }
  std::vector<D3DVERTEXELEMENT9> elements;
  DWORD fvf=0;
  uint32_t extent=0;
};
class Query final:public Child<IDirect3DQuery9>{
public:
  Query(Device*d,d9::ResourceRef ref):Child(d,ref,d9::Type::EventQuery){}
  D3DQUERYTYPE STDMETHODCALLTYPE GetType()override{return D3DQUERYTYPE_EVENT;}
  DWORD STDMETHODCALLTYPE GetDataSize()override{return sizeof(BOOL);}
  HRESULT STDMETHODCALLTYPE Issue(DWORD flags)override{
    if(!resourceAllowed()||flags!=D3DISSUE_END||revision==UINT64_MAX)return D3DERR_INVALIDCALL;
    const uint64_t nextRevision=revision+1;
    auto h=command(d9::Op::Issue);h.args[0]=uint32_t(nextRevision);h.args[1]=uint32_t(nextRevision>>32);
    std::array<uint8_t,16> p{};const auto hr=parent->conn->call(h,nullptr,p.data(),uint32_t(p.size()));
    if(FAILED(hr))return hr;
    const auto issuedRevision=d9::U64(p.data()),issuedSerial=d9::U64(p.data()+8);
    if(hr!=D3D_OK||issuedRevision!=nextRevision||!issuedSerial){parent->conn->failClosed();return D3DERR_DEVICELOST;}
    // A timeline serial may repeat with no intervening GPU submission. Only
    // this exact successful issue revision replaces this Query's observation.
    revision=issuedRevision;serial=issuedSerial;return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetData(void* p,DWORD n,DWORD flags)override{
    if(!resourceAllowed()||(p&&n!=sizeof(BOOL))||(!p&&n)||(flags&~DWORD(D3DGETDATA_FLUSH)))return D3DERR_INVALIDCALL;
    if(!serial){const auto hr=Issue(D3DISSUE_END);if(FAILED(hr))return hr;}
    auto h=command(d9::Op::Poll);h.args[0]=uint32_t(revision);h.args[1]=uint32_t(revision>>32);
    h.args[2]=uint32_t(serial);h.args[3]=uint32_t(serial>>32);
    if(parent->conn->asynchronousEvents()) {
      const auto hr=parent->conn->pollEvent(h);
      if(hr!=S_OK&&hr!=S_FALSE)return hr;
      const BOOL complete=hr==S_OK?TRUE:FALSE;
      if(p)std::memcpy(p,&complete,sizeof(complete));
      return hr;
    }
    std::array<uint8_t,4> data{};const auto hr=parent->conn->call(h,nullptr,data.data(),4);
    if(FAILED(hr))return hr;
    const uint32_t value=d9::U32(data.data());
    if(hr!=D3D_OK||value>1){parent->conn->failClosed();return D3DERR_DEVICELOST;}
    const BOOL complete=value?TRUE:FALSE;if(p)std::memcpy(p,&complete,4);
    return complete?S_OK:S_FALSE;
  }
  uint64_t revision=0,serial=0;
};

class Factory final:public dxvk::ComObjectClamp<Unsupported<IDirect3D9>>{
public:
  ~Factory() {
    // A metadata-only factory still closes its authenticated host normally.
    if(pending){d9::Header h;h.op=d9::Op::Finish;pending->call(h);}
  }
  std::shared_ptr<Connection> connection() {
    if(pending)return pending;
    if(auto result=active.lock())return result;
    pending=std::make_shared<Connection>(PresenterProfile()!=d9::PresentationProfile::Offscreen);
    return pending;
  }
  std::shared_ptr<Connection> acquireDeviceConnection() {
    auto result=pending?std::move(pending):std::make_shared<Connection>(PresenterProfile()!=d9::PresentationProfile::Offscreen);
    active=result;return result;
  }
  HRESULT query(d9::FactoryQuery kind,std::initializer_list<uint32_t> args,void* output=nullptr,uint32_t bytes=0) noexcept {
    try {
      d9::Header h;h.op=d9::Op::FactoryQuery;h.args[0]=uint32_t(kind);
      std::copy(args.begin(),args.end(),h.args.begin()+1);
      return connection()->call(h,nullptr,output,bytes);
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return D3DERR_NOTAVAILABLE;}
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{
    if(!out)return E_POINTER;*out=nullptr;
    if(iid!=IID_IUnknown&&iid!=__uuidof(IDirect3D9))return E_NOINTERFACE;
    *out=static_cast<IDirect3D9*>(this);AddRef();return S_OK;
  }
  UINT STDMETHODCALLTYPE GetAdapterCount()override{D3DCAPS9 c{};return GetDeviceCaps(0,D3DDEVTYPE_HAL,&c)==D3D_OK?1:0;}
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a,D3DDEVTYPE t,D3DCAPS9* p)override{
    if(a||t!=D3DDEVTYPE_HAL||!p)return D3DERR_INVALIDCALL;
    try{return NegotiatedCaps(*connection(),p);}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return D3DERR_NOTAVAILABLE;}
  }
  HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a,DWORD flags,D3DADAPTER_IDENTIFIER9* output) override {
    if(a||!output)return D3DERR_INVALIDCALL;
    std::array<uint8_t,d9::FactoryIdentifierBytes> bytes{};
    const auto hr=query(d9::FactoryQuery::Identifier,{flags},bytes.data(),uint32_t(bytes.size()));
    if(hr!=D3D_OK)return hr;
    if(!d9::DecodeIdentifier(bytes.data(),uint32_t(bytes.size()),*output)){
      if(auto c=active.lock())c->failClosed();if(pending)pending->failClosed();return D3DERR_DEVICELOST;
    }return D3D_OK;
  }
  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a,D3DFORMAT f) override {
    if(a)return 0;std::array<uint8_t,4> bytes{};
    return query(d9::FactoryQuery::ModeCount,{uint32_t(f)},bytes.data(),4)==D3D_OK?d9::U32(bytes.data()):0;
  }
  HRESULT mode(d9::FactoryQuery kind,std::initializer_list<uint32_t> args,D3DDISPLAYMODE* output) {
    if(!output)return D3DERR_INVALIDCALL;std::array<uint8_t,16> bytes{};
    const auto hr=query(kind,args,bytes.data(),16);if(hr!=D3D_OK)return hr;
    *output={d9::U32(bytes.data()),d9::U32(bytes.data()+4),d9::U32(bytes.data()+8),D3DFORMAT(d9::U32(bytes.data()+12))};
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a,D3DFORMAT f,UINT i,D3DDISPLAYMODE* p) override {
    return a?D3DERR_INVALIDCALL:mode(d9::FactoryQuery::EnumMode,{uint32_t(f),i},p);
  }
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a,D3DDISPLAYMODE* p) override {
    return a?D3DERR_INVALIDCALL:mode(d9::FactoryQuery::DisplayMode,{},p);
  }
  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override {
    // HMONITOR is obtained in the caller process, never serialized as a pointer.
    return a||!GetAdapterCount()?nullptr:MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a,D3DDEVTYPE t,D3DFORMAT display,D3DFORMAT back,BOOL windowed) override {
    if(a||t!=D3DDEVTYPE_HAL)return D3DERR_INVALIDCALL;
    if(windowed!=TRUE||(back!=D3DFMT_A8R8G8B8&&back!=D3DFMT_X8R8G8B8))return D3DERR_NOTAVAILABLE;
    return query(d9::FactoryQuery::DeviceType,{uint32_t(t),uint32_t(display),uint32_t(back),uint32_t(windowed)});
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a,D3DDEVTYPE t,D3DFORMAT display,DWORD usage,D3DRESOURCETYPE type,D3DFORMAT format) override {
    if(a||t!=D3DDEVTYPE_HAL)return D3DERR_INVALIDCALL;
    const bool texture=type==D3DRTYPE_TEXTURE&&(!usage||usage==D3DUSAGE_QUERY_SRGBREAD)&&d9::TextureFormatInfo(format).blockBytes;
    const bool target=type==D3DRTYPE_SURFACE&&((usage==D3DUSAGE_RENDERTARGET&&(format==D3DFMT_A8R8G8B8||format==D3DFMT_X8R8G8B8))
      ||(usage==D3DUSAGE_DEPTHSTENCIL&&(format==D3DFMT_D32F_LOCKABLE||format==D3DFMT_D16)));
    if(!texture&&!target)return D3DERR_NOTAVAILABLE;
    return query(d9::FactoryQuery::Format,{uint32_t(t),uint32_t(display),usage,uint32_t(type),uint32_t(format)});
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a,D3DDEVTYPE t,D3DFORMAT format,BOOL windowed,D3DMULTISAMPLE_TYPE samples,DWORD* quality) override {
    if(quality)*quality=0;if(a||t!=D3DDEVTYPE_HAL)return D3DERR_INVALIDCALL;
    if(windowed!=TRUE||samples!=D3DMULTISAMPLE_NONE||(format!=D3DFMT_A8R8G8B8&&format!=D3DFMT_X8R8G8B8
      &&format!=D3DFMT_D32F_LOCKABLE&&format!=D3DFMT_D16))return D3DERR_NOTAVAILABLE;
    std::array<uint8_t,4> bytes{};const auto hr=query(d9::FactoryQuery::Multisample,{uint32_t(t),uint32_t(format),1,uint32_t(samples)},bytes.data(),4);
    if(hr==D3D_OK&&quality)*quality=d9::U32(bytes.data());return hr;
  }
  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a,D3DDEVTYPE t,D3DFORMAT display,D3DFORMAT target,D3DFORMAT depth) override {
    if(a||t!=D3DDEVTYPE_HAL)return D3DERR_INVALIDCALL;
    if((target!=D3DFMT_A8R8G8B8&&target!=D3DFMT_X8R8G8B8)||(depth!=D3DFMT_D32F_LOCKABLE&&depth!=D3DFMT_D16))
      return D3DERR_NOTAVAILABLE;
    return query(d9::FactoryQuery::DepthMatch,{uint32_t(t),uint32_t(display),uint32_t(target),uint32_t(depth)});
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT a,D3DDEVTYPE t,D3DFORMAT,D3DFORMAT) override {
    return a||t!=D3DDEVTYPE_HAL?D3DERR_INVALIDCALL:D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE CreateDevice(UINT a,D3DDEVTYPE t,HWND hwnd,DWORD flags,D3DPRESENT_PARAMETERS* p,IDirect3DDevice9** out)override{
    if(out)*out=nullptr;
    DWORD pid=0;const DWORD windowThread=GetWindowThreadProcessId(hwnd,&pid);
    if(!out||!p||a||t!=D3DDEVTYPE_HAL||!IsWindow(hwnd)||pid!=GetCurrentProcessId()
      ||flags!=(D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE))return D3DERR_INVALIDCALL;
    try{
      const auto profile=PresenterProfile();
      // Windowed D3D9: the chosen extent is written back.
      SwapChainMode mode;
      if(!ResolveSwapChain(*p,hwnd,profile!=d9::PresentationProfile::Offscreen,mode))return D3DERR_INVALIDCALL;
      if(profile!=d9::PresentationProfile::Offscreen) {
        if(windowThread==GetCurrentThreadId())return D3DERR_INVALIDCALL;
        const auto desktop=d9::RequireIsolatedDesktop();
        if(d9::ObjectName(GetThreadDesktop(windowThread))!=d9::CurrentDesktop())return D3DERR_INVALIDCALL;
        (void)desktop;
      }
      auto device=std::make_unique<Device>(this,hwnd,flags,profile,mode);const auto hr=device->initialize();
      if(FAILED(hr))return hr;p->BackBufferWidth=mode.width;p->BackBufferHeight=mode.height;
      device->AddRef();*out=device.release();return D3D_OK;
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return D3DERR_NOTAVAILABLE;}
  }
private:
  std::shared_ptr<Connection> pending;
  std::weak_ptr<Connection> active;
};
Device::Device(Factory*f,HWND hwnd,DWORD flags,d9::PresentationProfile selected,const SwapChainMode& mode):
  conn(f->acquireDeviceConnection()),
  factory(f),window(hwnd),behavior(flags),profile(selected),width(mode.width),height(mode.height),
  format(mode.format),interval(mode.interval){
  if(profile==d9::PresentationProfile::IsolatedPresenterReadback)
    presented.resize(8+size_t(width)*height*4);
  if(!presented.empty())accountPayload(d9::memory::Kind::PresentedReadback,presented.size(),presented.size(),presented.capacity(),true);
  // D3D9 defaults: CCW culling; the viewport and the scissor rectangle cover the back buffer.
  factory->AddRef();draw.render.cullMode=D3DCULL_CCW;draw.viewport={0,0,width,height,0,1};
  effects.scissor={0,0,int32_t(width),int32_t(height)};
}
HRESULT Device::initialize(){
  bool retain=false;uint64_t budget=0;
  if(!BackingBudget(retain,budget))return D3DERR_NOTAVAILABLE;
  auto surface=std::make_unique<Surface>(this,false);
  d9::Header h;h.op=d9::Op::Initialize;h.args[0]=width;h.args[1]=height;
  h.args[6]=(retain?d9::InitializeRetainContent:0)|(conn->annotationsEnabled()?d9::InitializeAnnotations:0);
  h.args[7]=format==D3DFMT_X8R8G8B8?d9::OpaqueBackBuffer:0;
  if(profile!=d9::PresentationProfile::Offscreen) {
    marker=conn->windowMarker();
    if(GetPropW(window,marker.c_str())||!SetPropW(window,marker.c_str(),reinterpret_cast<HANDLE>(uintptr_t(1))))
      return D3DERR_INVALIDCALL;
    markerSet=true;
    h.args[2]=uint32_t(profile);
    h.args[3]=uint32_t(reinterpret_cast<uintptr_t>(window));
    h.args[4]=GetWindowThreadProcessId(window,nullptr);h.args[5]=interval;
  }
  // Every profile, as the original hooks every windowed swap chain.
  if(!hookWindow())return D3DERR_INVALIDCALL;
  const auto hr=conn->call(h);if(FAILED(hr))return hr;
  // Before any resource exists: budgeted copies need the host's retained ones.
  if(retain)residency.configure(budget);
  back=surface.release();back->AddRefPrivate();return D3D_OK;
}
bool Device::hookWindow(){
  std::lock_guard<std::mutex> lock(windowHooksMutex);
  const auto it=windowHooks.find(window);
  if(it!=windowHooks.end()){it->second.size=&windowSize;windowHooked=true;return true;}
  // The window thread waits on the lock, so no message misses the entry.
  auto& hook=windowHooks[window];hook.unicode=IsWindowUnicode(window)!=FALSE;hook.size=&windowSize;
  const auto proc=reinterpret_cast<LONG_PTR>(DeviceWindowProc);
  hook.previous=reinterpret_cast<WNDPROC>(hook.unicode?SetWindowLongPtrW(window,GWLP_WNDPROC,proc)
    :SetWindowLongPtrA(window,GWLP_WNDPROC,proc));
  if(!hook.previous){windowHooks.erase(window);return false;}
  windowHooked=true;return true;
}
void Device::unhookWindow() noexcept {
  if(!windowHooked)return;
  std::lock_guard<std::mutex> lock(windowHooksMutex);const auto it=windowHooks.find(window);
  if(it==windowHooks.end()||it->second.size!=&windowSize)return;
  const auto proc=reinterpret_cast<LONG_PTR>(DeviceWindowProc);
  const auto current=it->second.unicode?GetWindowLongPtrW(window,GWLP_WNDPROC):GetWindowLongPtrA(window,GWLP_WNDPROC);
  // A later subclass still calls through the entry, which keeps forwarding.
  if(current!=proc){it->second.size=nullptr;return;}
  const auto previous=reinterpret_cast<LONG_PTR>(it->second.previous);
  if(it->second.unicode)SetWindowLongPtrW(window,GWLP_WNDPROC,previous);else SetWindowLongPtrA(window,GWLP_WNDPROC,previous);
  windowHooks.erase(it);
}
Device::~Device(){
  unhookWindow();
  recording.reset(); // Destroy recorded resources before Finish closes the host.
  Bind(vb,static_cast<VertexBuffer*>(nullptr));Bind(ib,static_cast<IndexBuffer*>(nullptr));Bind(tex,static_cast<Texture*>(nullptr));
  for(auto& texture:additionalTextures)Bind(texture,static_cast<Texture*>(nullptr));
  Bind(vs,static_cast<VertexShader*>(nullptr));Bind(ps,static_cast<PixelShader*>(nullptr));Bind(decl,static_cast<Declaration*>(nullptr));
  const auto fvfCached=fvfDeclarations.size();
  for(auto& cached:fvfDeclarations)Bind(cached,static_cast<Declaration*>(nullptr));
  if(back)back->ReleasePrivate();
  if(!presented.empty())accountPayload(d9::memory::Kind::PresentedReadback,presented.size(),presented.size(),presented.capacity(),false);
  if(conn->healthy()&&(!conn->isOwner()||resources.liveCount()||resources.reservedCount()))conn->failClosed();
  // Cold, always printed; vertex-buffer and texture-level copies only.
  bool enabled=false;uint64_t budget=0;const auto r=residency.snapshot(budget,enabled);
  std::fprintf(stderr,"D3D9 backing-proxy: {\"schema\":\"warvk-d3d9-backing-proxy/v1\",\"enabled\":%s,\"budgetBytes\":%llu,"
    "\"residentBytes\":%llu,\"peakResidentBytes\":%llu,\"listedBytes\":%llu,\"peakPinnedBytes\":%llu,"
    "\"maxListedAfterTrim\":%llu,\"allocations\":%llu,\"evictions\":%llu,\"evictedBytes\":%llu,"
    "\"fetches\":%llu,\"fetchCalls\":%llu,\"fetchedBytes\":%llu,\"discardMisses\":%llu}\n",
    enabled?"true":"false",budget,r.residentBytes,r.peakResidentBytes,r.listedBytes,r.peakPinnedBytes,
    r.maxListedAfterTrim,r.allocations,r.evictions,r.evictedBytes,r.fetches,r.fetchCalls,r.fetchedBytes,r.discardMisses);
  std::fprintf(stderr,"D3D9 fvf-proxy: {\"schema\":\"warvk-d3d9-fvf-proxy/v1\",\"cached\":%llu,\"evictions\":%llu}\n",
    static_cast<unsigned long long>(fvfCached),static_cast<unsigned long long>(fvfEvictions));
  std::fprintf(stderr,"D3D9 window-proxy: {\"schema\":\"warvk-d3d9-window-proxy/v1\",\"hooked\":%s,\"sizeMessages\":%llu,"
    "\"resizes\":%llu,\"refused\":%llu}\n",windowHooked?"true":"false",
    static_cast<unsigned long long>(windowSize.messages.load()),static_cast<unsigned long long>(windowResizes),
    static_cast<unsigned long long>(windowResizesRefused));
  d9::Header h;h.op=d9::Op::Finish;conn->call(h);
  if(markerSet&&GetPropW(window,marker.c_str())==reinterpret_cast<HANDLE>(uintptr_t(1)))
    RemovePropW(window,marker.c_str());
  factory->Release();
}
HRESULT Device::diagnosticCheckpoint(UINT phase,UINT ordinal,d9::memory::Checkpoint* output,UINT bytes){
  using namespace d9::memory;
  if(!allowed()||!output||bytes!=sizeof(Checkpoint)||!ValidPhase(phase)
    ||(phase==uint32_t(Phase::DeviceReady)?ordinal!=0:ordinal==0))return D3DERR_INVALIDCALL;
  Checkpoint sample;sample.phase=phase;sample.ordinal=ordinal;
  sample.pid=GetCurrentProcessId();sample.thread=GetCurrentThreadId();
  try {
    sample.creation=lab::CreationTime(GetCurrentProcess());
    sample.logicalHandles=resources.liveCount();sample.reservedHandles=resources.reservedCount();
    const auto bound=[&](const auto* resource){if(resource){++sample.privateBindings;
      if(resource->publicReferencesZero())++sample.publicZeroBindings;}};
    bound(vb);bound(ib);bound(tex);bound(vs);bound(ps);bound(decl);
    for(const auto* texture:additionalTextures)bound(texture);
    sample.cpu=memoryAccount;
    if(memoryForeignOwner.load(std::memory_order_acquire))sample.cpu.valid=false;
    sample.connectionInlineBytes=sizeof(Connection);
    sample.fixedPacketPayloadBytes=uint64_t(commands::Capacity)*commands::MaxBatchBytes+2ull*sizeof(d9::Packet);
    sample.workerBatchPayloadBytes=commands::MaxBatchBytes;
    sample.tickBegin=GetTickCount64();sample.before=lab::ProbeOwnProcessMemory();sample.beforeEnd=GetTickCount64();
    d9::Header h;h.op=d9::Op::DiagnosticCheckpoint;h.args[0]=phase;h.args[1]=ordinal;
    HostWire encoded{};const HRESULT hr=conn->call(h,nullptr,encoded.data(),uint32_t(encoded.size()));
    if(hr!=D3D_OK)return hr;
    Connection::DiagnosticIdentity expected;
    if(!conn->diagnosticIdentity(expected)){conn->failClosed();return D3DERR_DEVICELOST;}
    sample.expectedHostPid=expected.childPid;sample.expectedHostCreation=expected.childCreation;
    sample.expectedNonceLow=expected.nonceLow;sample.expectedNonceHigh=expected.nonceHigh;
    sample.expectedSequence=expected.sequence;
    sample.afterBegin=GetTickCount64();sample.after=lab::ProbeOwnProcessMemory();sample.tickEnd=GetTickCount64();
    if(!DecodeHost(encoded,sample.host)||sample.host.phase!=phase||sample.host.ordinal!=ordinal
      ||sample.host.pid!=sample.expectedHostPid||sample.host.creation!=sample.expectedHostCreation
      ||sample.host.nonceLow!=sample.expectedNonceLow||sample.host.nonceHigh!=sample.expectedNonceHigh
      ||sample.host.sequence!=sample.expectedSequence
      ||sample.host.sequence<=lastMemorySequence||sample.host.tickBegin<sample.beforeEnd
      ||sample.host.tickEnd>sample.afterBegin){conn->failClosed();return D3DERR_DEVICELOST;}
    lastMemorySequence=sample.host.sequence;*output=sample;return D3D_OK;
  }catch(...){return E_FAIL;}
}
// DXVK's ResetState, as the WarVK reference does it: render, stage and sampler
// state go back to the creation defaults and the buffer and texture bindings
// are released (a null stream keeps its offset and stride). Shaders, the
// declaration, shader constants, transforms, the material and lights survive.
void Device::resetState(){
  Bind(vb,static_cast<VertexBuffer*>(nullptr));Bind(ib,static_cast<IndexBuffer*>(nullptr));Bind(tex,static_cast<Texture*>(nullptr));
  for(auto& texture:additionalTextures)Bind(texture,static_cast<Texture*>(nullptr));
  draw.render={};draw.render.cullMode=D3DCULL_CCW;draw.viewport={0,0,width,height,0,1};
  const auto transforms=fixed.transforms;const auto material=fixed.material;
  fixed={};fixed.transforms=transforms;fixed.material=material;
  samplerState={};effects={};effects.scissor={0,0,int32_t(width),int32_t(height)};
  textureStages={};dither=FALSE;scene=false;
}
HRESULT Device::Reset(D3DPRESENT_PARAMETERS* p){
  if(!conn->isOwner())return D3DERR_INVALIDCALL;
  if(!conn->healthy())return D3DERR_DEVICELOST;
  // Refused parameters (native validation, or this profile's limits) leave the
  // device and its state untouched; so does Reset while recording a block.
  SwapChainMode mode;
  try{if(!p||recording||!ResolveSwapChain(*p,window,profile!=d9::PresentationProfile::Offscreen,mode))return D3DERR_INVALIDCALL;}
  catch(...){return D3DERR_INVALIDCALL;}
  // From here native clears the state and releases its implicit surfaces
  // whether or not the Reset succeeds; the scene ends.
  resetState();
  if(back){back->ReleasePrivate();back=nullptr;}
  if(losable){notReset=true;return D3DERR_INVALIDCALL;}
  d9::Header h;h.op=d9::Op::Reset;h.args[0]=mode.width;h.args[1]=mode.height;h.args[5]=mode.interval;
  h.args[7]=mode.format==D3DFMT_X8R8G8B8?d9::OpaqueBackBuffer:0;
  HRESULT hr=conn->call(h);
  if(hr!=D3D_OK){if(SUCCEEDED(hr)){conn->failClosed();hr=D3DERR_DEVICELOST;}else if(conn->healthy())notReset=true;return hr;}
  width=mode.width;height=mode.height;format=mode.format;interval=mode.interval;
  try{
    if(!presented.empty()) {
      accountPayload(d9::memory::Kind::PresentedReadback,presented.size(),presented.size(),presented.capacity(),false);
      presented.assign(8+size_t(width)*height*4,0);
      accountPayload(d9::memory::Kind::PresentedReadback,presented.size(),presented.size(),presented.capacity(),true);
    }
    back=new Surface(this,false);back->AddRefPrivate();
  }catch(...){conn->failClosed();return D3DERR_DEVICELOST;}
  resetState();
  // The host keeps its last sampler, effects and stage tables: publish the
  // defaults now, so nothing the next draw compares against is stale.
  {d9::Header s;s.op=d9::Op::SetEffects;s.payloadBytes=d9::EffectsBytes;
    const auto bytes=d9::EncodeEffects(effects);hr=conn->call(s,bytes.data());}
  if(hr==D3D_OK){d9::Header s;s.op=d9::Op::SetSamplerState;s.payloadBytes=d9::SamplerStateBytes;
    const auto bytes=d9::EncodeSamplerState(samplerState);hr=conn->call(s,bytes.data());}
  if(hr==D3D_OK){d9::Header s;s.op=d9::Op::SetTextureStages;s.payloadBytes=d9::TextureStagesBytes;
    const auto bytes=d9::EncodeTextureStages(textureStages);hr=conn->call(s,bytes.data());}
  if(hr!=D3D_OK){if(conn->healthy())conn->failClosed();return D3DERR_DEVICELOST;}
  sentEffects=effects;sentSamplerState=samplerState;samplerSent=true;sentTextureStages=textureStages;
  // The application's extent wins over a window resize not yet taken.
  windowSize.client.store(0,std::memory_order_release);
  notReset=false;p->BackBufferWidth=width;p->BackBufferHeight=height;return D3D_OK;
}
// WarVK's WM_SIZE answer (D3D9DeviceEx::ResetSwapChain): the implicit swap
// chain takes the client extent with the device's format and interval. The
// back buffer and auto depth are new and RT0 is set again, so the viewport and
// the scissor rectangle cover it; every other state, binding and resource is
// kept. A refused extent keeps the old one, as the original only logs a failed
// ResetSwapChain; the next WM_SIZE tries again.
void Device::resizeToWindow(uint64_t client){
  const uint32_t w=uint32_t(client>>32),h=uint32_t(client);
  if(!client||(w==width&&h==height))return;
  if(!d9::ValidTargetExtent(w,h)){++windowResizesRefused;return;}
  d9::Header r;r.op=d9::Op::Reset;r.args[0]=w;r.args[1]=h;r.args[5]=interval;r.args[6]=d9::ResetKeepResources;
  r.args[7]=format==D3DFMT_X8R8G8B8?d9::OpaqueBackBuffer:0;
  const HRESULT hr=conn->call(r);
  if(hr!=D3D_OK){if(SUCCEEDED(hr))conn->failClosed();else if(conn->healthy())++windowResizesRefused;return;}
  width=w;height=h;
  try{
    if(!presented.empty()) {
      accountPayload(d9::memory::Kind::PresentedReadback,presented.size(),presented.size(),presented.capacity(),false);
      presented.assign(8+size_t(width)*height*4,0);
      accountPayload(d9::memory::Kind::PresentedReadback,presented.size(),presented.size(),presented.capacity(),true);
    }
    auto* next=new Surface(this,false);
    if(back)back->ReleasePrivate();back=next;back->AddRefPrivate();
  }catch(...){conn->failClosed();return;}
  draw.viewport={0,0,width,height,0,1};effects.scissor={0,0,int32_t(width),int32_t(height)};
  ++windowResizes;
}
HRESULT Device::GetDirect3D(IDirect3D9** p){return Return(static_cast<IDirect3D9*>(factory),p);}
template<class T,class I>static HRESULT MakeBuffer(Device* d,UINT n,DWORD usage,D3DPOOL pool,I** out,HANDLE* shared,bool index){
  if(out)*out=nullptr;
  if(!d->allowed()||!out||shared||!n||n>d9::MaxBuffer||pool!=D3DPOOL_DEFAULT
    ||(usage&~DWORD(D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY))||!(usage&D3DUSAGE_WRITEONLY)||(index&&(n&1)))return D3DERR_INVALIDCALL;
  try{ResourceReservation reservation(d,index?d9::Type::IndexBuffer:d9::Type::VertexBuffer);
    if(!reservation.ref.id)return E_OUTOFMEMORY;auto o=std::make_unique<T>(d,reservation.ref,n,usage);
    d9::Header h;h.op=d9::Op::CreateBuffer;h.args[0]=n;h.args[1]=index?1:0;h.args[2]=(usage&D3DUSAGE_DYNAMIC)?1:0;
    const auto hr=reservation.create(h);if(hr!=D3D_OK)return hr;
    o->remoteLive=true;o->AddRef();*out=o.release();return D3D_OK;
  }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return D3DERR_INVALIDCALL;}
}
HRESULT Device::CreateVertexBuffer(UINT n,DWORD u,DWORD f,D3DPOOL p,IDirect3DVertexBuffer9** o,HANDLE* s){
  const auto hr=MakeBuffer<VertexBuffer>(this,n,u,p,o,s,false);
  if(hr==D3D_OK)static_cast<VertexBuffer*>(*o)->fvf=f;
  return hr;
}
HRESULT Device::CreateIndexBuffer(UINT n,DWORD u,D3DFORMAT f,D3DPOOL p,IDirect3DIndexBuffer9** o,HANDLE* s){
  if(f!=D3DFMT_INDEX16){if(o)*o=nullptr;return D3DERR_INVALIDCALL;}return MakeBuffer<IndexBuffer>(this,n,u,p,o,s,true);
}
HRESULT Device::CreateTexture(UINT w,UINT h,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DTexture9** out,HANDLE* shared){
  if(out)*out=nullptr;
  d9::TextureLayout layout;
  if(!allowed()||!out||shared||!d9::MakeTextureLayout(w,h,levels,layout,format)||usage||pool!=D3DPOOL_MANAGED)return D3DERR_INVALIDCALL;
  try{ResourceReservation reservation(this,d9::Type::Texture);
    if(!reservation.ref.id)return E_OUTOFMEMORY;auto o=std::make_unique<Texture>(this,reservation.ref,layout);
    d9::Header hd;hd.op=d9::Op::CreateTexture2D;hd.args[0]=w;hd.args[1]=h;hd.args[2]=layout.levels;hd.args[3]=format;
    const auto hr=reservation.create(hd);if(hr!=D3D_OK)return hr;o->remoteLive=true;o->AddRef();*out=o.release();return D3D_OK;
  }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return D3DERR_INVALIDCALL;}
}
HRESULT Device::CreateOffscreenPlainSurface(UINT w,UINT h,D3DFORMAT f,D3DPOOL pool,IDirect3DSurface9** out,HANDLE* shared){
  // Only a GetRenderTargetData destination: exactly the back buffer extent and format.
  if(out)*out=nullptr;if(!allowed()||!out||shared||w!=width||h!=height||f!=format||pool!=D3DPOOL_SYSTEMMEM)return D3DERR_INVALIDCALL;
  try{auto p=new Surface(this,true);p->AddRef();*out=p;return D3D_OK;}catch(...){return E_OUTOFMEMORY;}
}
HRESULT Device::GetBackBuffer(UINT a,UINT b,D3DBACKBUFFER_TYPE t,IDirect3DSurface9** out){
  followWindow();if(out)*out=nullptr;
  if(a||b||t!=D3DBACKBUFFER_TYPE_MONO||!back)return D3DERR_INVALIDCALL;return Return(static_cast<IDirect3DSurface9*>(back),out);
}
HRESULT Device::SetRenderTarget(DWORD index,IDirect3DSurface9* s){return allowed()&&!index&&s==back?D3D_OK:D3DERR_INVALIDCALL;}
HRESULT Device::GetRenderTargetData(IDirect3DSurface9* src,IDirect3DSurface9* dst){
  auto target=dynamic_cast<Surface*>(dst);
  // A surface made before a Reset keeps its old extent and format.
  if(!allowed()||src!=back||!target||target->parent!=this||!target->system||target->locked
    ||target->width!=width||target->height!=height||target->format!=format)return D3DERR_INVALIDCALL;
  return readRows(d9::ReadbackSource::Target,0,target->bytes.data());
}
static bool ReadSelf(const void* source,void* target,size_t bytes){
  SIZE_T done=0;return source&&ReadProcessMemory(GetCurrentProcess(),source,target,bytes,&done)&&done==bytes;
}
HRESULT Device::SetTransform(D3DTRANSFORMSTATETYPE type,const D3DMATRIX* value) {
  const auto index=d9::FFTransformIndex(type);D3DMATRIX matrix{};
  if(!allowed()||index<0||!ReadSelf(value,&matrix,sizeof(matrix)))return D3DERR_INVALIDCALL;
  for(const auto& row:matrix.m)for(float v:row)if(!std::isfinite(v))return D3DERR_INVALIDCALL;
  if(recording){recording->fixed.transforms[index]=matrix;recording->transformMask[index]=true;}
  else fixed.transforms[index]=matrix;return D3D_OK;
}
HRESULT Device::GetTransform(D3DTRANSFORMSTATETYPE type,D3DMATRIX* value) {
  const auto index=d9::FFTransformIndex(type);if(index<0||!value)return D3DERR_INVALIDCALL;
  *value=fixed.transforms[index];return D3D_OK;
}
HRESULT Device::MultiplyTransform(D3DTRANSFORMSTATETYPE type,const D3DMATRIX* value) {
  const auto index=d9::FFTransformIndex(type);D3DMATRIX input{},result{};
  if(!allowed()||index<0||!ReadSelf(value,&input,sizeof(input)))return D3DERR_INVALIDCALL;
  // Matrix4 multiplication interprets the D3D rows as columns, as in native.
  const auto& old=fixed.transforms[index];
  for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
    for(unsigned k=0;k<4;++k)result.m[i][j]+=input.m[i][k]*old.m[k][j];
  // The original D3D9DeviceEx::MultiplyTransform deliberately bypasses its
  // recorder. Preserve that observable behavior; SetTransform is recorded.
  for(const auto& row:result.m)for(float v:row)if(!std::isfinite(v))return D3DERR_INVALIDCALL;
  fixed.transforms[index]=result;return D3D_OK;
}
HRESULT Device::SetMaterial(const D3DMATERIAL9* value) {
  D3DMATERIAL9 copy{};if(!allowed()||!ReadSelf(value,&copy,sizeof(copy))||!d9::FiniteMaterial(copy))return D3DERR_INVALIDCALL;
  if(recording){recording->fixed.material=copy;recording->materialMask=true;}
  else fixed.material=copy;return D3D_OK;
}
HRESULT Device::GetMaterial(D3DMATERIAL9* value) {if(!value)return D3DERR_INVALIDCALL;*value=fixed.material;return D3D_OK;}
HRESULT Device::SetLight(DWORD index,const D3DLIGHT9* value) {
  D3DLIGHT9 copy{};
  if(!allowed()||index>=lights.size()||!ReadSelf(value,&copy,sizeof(copy))||!d9::ValidLight(copy))return D3DERR_INVALIDCALL;
  auto& target=recording?recording->lights:lights;
  target[index].value=copy;target[index].defined=true;
  if(recording)recording->lightsMask=true;return D3D_OK;
}
HRESULT Device::GetLight(DWORD index,D3DLIGHT9* value) {
  if(!value||index>=lights.size()||!lights[index].defined)return D3DERR_INVALIDCALL;
  *value=lights[index].value;return D3D_OK;
}
HRESULT Device::LightEnable(DWORD index,BOOL enabled) {
  if(!allowed()||index>=lights.size())return D3DERR_INVALIDCALL;
  auto& target=recording?recording->lights:lights;
  if(enabled&&!target[index].enabled) {
    unsigned count=0;for(const auto& light:target)count+=light.enabled;
    if(count==8)return D3DERR_INVALIDCALL;
  }
  auto& light=target[index];
  if(!light.defined){light.value={};light.value.Type=D3DLIGHT_DIRECTIONAL;
    light.value.Diffuse={1,1,1,0};light.value.Direction={0,0,1};light.defined=true;}
  if(recording&&light.enabled!=(enabled!=FALSE)){
    recording->enabledMask[index]=true;recording->lightsMask=true;
  }
  light.enabled=enabled!=FALSE;return D3D_OK;
}
HRESULT Device::GetLightEnable(DWORD index,BOOL* value) {
  if(!value||index>=lights.size()||!lights[index].defined)return D3DERR_INVALIDCALL;
  *value=lights[index].enabled?128:0;return D3D_OK;
}
HRESULT Device::SetTextureStageState(DWORD stage,D3DTEXTURESTAGESTATETYPE type,DWORD value) {
  const auto index=d9::FFIndex(d9::FFStageStates,type);
  if(!allowed()||index<0||!d9::ValidFFStage(stage,type,value))return D3DERR_INVALIDCALL;
  if(recording){recording->fixed.stages[stage][index]=value;recording->stageMask[stage][index]=true;}
  else fixed.stages[stage][index]=value;return D3D_OK;
}
HRESULT Device::GetTextureStageState(DWORD stage,D3DTEXTURESTAGESTATETYPE type,DWORD* value) {
  const auto index=d9::FFIndex(d9::FFStageStates,type);
  if(!value||index<0||stage>=8)return D3DERR_INVALIDCALL;
  *value=fixed.stages[stage][index];return D3D_OK;
}
static bool ReadShader(const DWORD* data,std::vector<uint32_t>& words){
  if(!data)return false;words.clear();words.reserve(d3d9tokens::MaxWords);
  uint32_t token=0;if(!ReadSelf(data,&token,4))return false;words.push_back(token);
  size_t pos=1;
  while(pos<d3d9tokens::MaxWords){
    if(!ReadSelf(data+pos,&token,4))return false;words.push_back(token);++pos;
    if(token==0x0000ffffu)return d3d9tokens::Scan(words.data(),words.size()).accepted();
    const uint32_t n=(token&0xffffu)==0xfffeu?((token>>16)&0x7fff):((token>>24)&15);
    if(n>d3d9tokens::MaxWords-pos)return false;
    for(uint32_t i=0;i<n;++i){uint32_t arg;if(!ReadSelf(data+pos++,&arg,4))return false;words.push_back(arg);}
  }return false;
}
template<class T,class I>HRESULT MakeShader(Device*d,const DWORD* tokens,I** out,d9::Type type){
  if(out)*out=nullptr;if(!d->allowed()||!out)return D3DERR_INVALIDCALL;
  try{std::vector<uint32_t> words;if(!ReadShader(tokens,words))return D3DERR_INVALIDCALL;
    const bool vertex=type==d9::Type::VertexShader;
    if(d3d9tokens::Scan(words.data(),words.size()).stage!=(vertex?d3d9tokens::Stage::Vertex:d3d9tokens::Stage::Pixel))return D3DERR_INVALIDCALL;
    ResourceReservation reservation(d,type);if(!reservation.ref.id)return E_OUTOFMEMORY;
    auto o=std::make_unique<T>(d,reservation.ref,std::move(words));
    d9::Header h;h.op=d9::Op::CreateShader;h.args[0]=vertex?0:1;h.payloadBytes=uint32_t(o->tokens.size()*4);
    d9::Packet payload{};for(size_t i=0;i<o->tokens.size();++i)d9::P32(payload.data()+i*4,o->tokens[i]);
    const auto hr=reservation.create(h,payload.data());if(hr!=D3D_OK)return hr;o->remoteLive=true;o->AddRef();*out=o.release();return D3D_OK;
  }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return D3DERR_INVALIDCALL;}
}
HRESULT Device::CreateVertexShader(const DWORD*p,IDirect3DVertexShader9**o){return MakeShader<VertexShader>(this,p,o,d9::Type::VertexShader);}
HRESULT Device::CreatePixelShader(const DWORD*p,IDirect3DPixelShader9**o){return MakeShader<PixelShader>(this,p,o,d9::Type::PixelShader);}
HRESULT Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9* data,IDirect3DVertexDeclaration9** out){
  if(out)*out=nullptr;if(!allowed()||!out||!data)return D3DERR_INVALIDCALL;
  try{
    std::vector<D3DVERTEXELEMENT9> elems;
    for(size_t i=0;i<17;++i){D3DVERTEXELEMENT9 e;if(!ReadSelf(data+i,&e,sizeof(e)))return D3DERR_INVALIDCALL;elems.push_back(e);if(e.Stream==0xff)break;}
    const D3DVERTEXELEMENT9 end=D3DDECL_END();
    uint32_t extent=0;
    if(elems.empty()||std::memcmp(&elems.back(),&end,sizeof(end))
      ||!d9::ValidDeclaration(elems.data(),uint32_t(elems.size()-1),extent))return D3DERR_INVALIDCALL;
    ResourceReservation reservation(this,d9::Type::Declaration);if(!reservation.ref.id)return E_OUTOFMEMORY;
    auto o=std::make_unique<Declaration>(this,reservation.ref,std::move(elems));
    o->extent=extent;
    o->fvf=dxvk::FvfMapping::MapD3D9VertexElementsToFvf(o->elements.data(),uint32_t(o->elements.size()-1));
    std::array<uint8_t,d9::MaxDeclarationElements*d9::DeclarationElementBytes> p{};
    for(size_t i=0;i+1<o->elements.size();++i)d9::EncodeDeclarationElement(p.data()+i*8,o->elements[i]);
    d9::Header h;h.op=d9::Op::CreateDeclaration;h.payloadBytes=uint32_t(o->elements.size()-1)*8;
    const auto hr=reservation.create(h,p.data());if(hr!=D3D_OK)return hr;o->remoteLive=true;o->AddRef();*out=o.release();return D3D_OK;
  }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return D3DERR_INVALIDCALL;}
}
HRESULT Device::CreateQuery(D3DQUERYTYPE type,IDirect3DQuery9** out){
  if(out)*out=nullptr;if(!allowed()||type!=D3DQUERYTYPE_EVENT)return D3DERR_NOTAVAILABLE;
  if(!out)return D3D_OK;
  try{ResourceReservation reservation(this,d9::Type::EventQuery);if(!reservation.ref.id)return E_OUTOFMEMORY;
    auto q=std::make_unique<Query>(this,reservation.ref);d9::Header h;h.op=d9::Op::CreateQuery;
    const auto hr=reservation.create(h);if(hr!=D3D_OK)return hr;
    q->remoteLive=true;q->AddRef();*out=q.release();return D3D_OK;
  }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return D3DERR_INVALIDCALL;}
}

#define D9_BIND_METHOD(Set,Get,I,T,field) \
HRESULT Device::Set(I* p){auto q=dynamic_cast<T*>(p);if(!allowed()||(p&&(!q||q->parent!=this)))return D3DERR_INVALIDCALL; \
  if(q&&!q->resourceAllowed())return D3DERR_DEVICELOST; \
  if(recording){Bind(recording->field,q);recording->field##Mask=true;}else Bind(field,q);return allowed()?D3D_OK:D3DERR_DEVICELOST;} \
HRESULT Device::Get(I** p){return Return(static_cast<I*>(field),p);}
D9_BIND_METHOD(SetIndices,GetIndices,IDirect3DIndexBuffer9,IndexBuffer,ib)
D9_BIND_METHOD(SetVertexShader,GetVertexShader,IDirect3DVertexShader9,VertexShader,vs)
D9_BIND_METHOD(SetPixelShader,GetPixelShader,IDirect3DPixelShader9,PixelShader,ps)
D9_BIND_METHOD(SetVertexDeclaration,GetVertexDeclaration,IDirect3DVertexDeclaration9,Declaration,decl)
#undef D9_BIND_METHOD
HRESULT Device::SetFVF(DWORD fvf) {
  if(!allowed())return D3DERR_INVALIDCALL;
  // Native D3D9 accepts zero without changing the current declaration.
  if(!fvf)return D3D_OK;
  if(decl&&decl->fvf==fvf)return recording?SetVertexDeclaration(decl):D3D_OK;
  for(auto* cached:fvfDeclarations)if(cached->fvf==fvf)return SetVertexDeclaration(cached);
  const auto decoded=dxvk::DecodeFvf(fvf);
  std::array<D3DVERTEXELEMENT9,17> elements{};
  std::copy_n(decoded.elements.begin(),decoded.count,elements.begin());
  elements[decoded.count]=D3DDECL_END();
  IDirect3DVertexDeclaration9* value=nullptr;
  auto hr=CreateVertexDeclaration(elements.data(),&value);
  // The host has few declaration slots. Cached declarations that are neither
  // bound nor held by the caller give theirs back, then creation is retried once.
  if(hr==D3DERR_OUTOFVIDEOMEMORY) {
    const auto before=fvfDeclarations.size();
    for(auto& cached:fvfDeclarations)if(cached!=decl)Bind(cached,static_cast<Declaration*>(nullptr));
    fvfDeclarations.erase(std::remove(fvfDeclarations.begin(),fvfDeclarations.end(),nullptr),fvfDeclarations.end());
    fvfEvictions+=before-fvfDeclarations.size();
    if(fvfDeclarations.size()<before)hr=CreateVertexDeclaration(elements.data(),&value);
  }
  if(hr!=D3D_OK)return hr;
  auto* created=static_cast<Declaration*>(value);created->fvf=fvf;
  if(fvfDeclarations.size()<MaxFvfDeclarations){
    try{fvfDeclarations.push_back(nullptr);Bind(fvfDeclarations.back(),created);}catch(...){}
  }
  const auto result=SetVertexDeclaration(value);value->Release();return result;
}
HRESULT Device::GetFVF(DWORD* value) {
  if(!value)return D3DERR_INVALIDCALL;*value=decl?decl->fvf:0;return D3D_OK;
}
HRESULT Device::SetStreamSource(UINT stream,IDirect3DVertexBuffer9* p,UINT offset,UINT stride){
  auto q=dynamic_cast<VertexBuffer*>(p);
  if(!allowed()||stream||(p&&(!q||q->parent!=this||!stride||stride>256||offset>q->extent)))return D3DERR_INVALIDCALL;
  if(q&&!q->resourceAllowed())return D3DERR_DEVICELOST;
  if(recording){Bind(recording->vb,q);recording->offset=offset;recording->stride=stride;recording->vbMask=true;}
  else {Bind(vb,q);if(q){draw.streamOffset=offset;draw.stride=stride;}}
  return allowed()?D3D_OK:D3DERR_DEVICELOST;
}
HRESULT Device::GetStreamSource(UINT stream,IDirect3DVertexBuffer9** p,UINT* off,UINT* stride){
  if(stream||!p||!off||!stride)return D3DERR_INVALIDCALL;*off=draw.streamOffset;*stride=draw.stride;return Return(static_cast<IDirect3DVertexBuffer9*>(vb),p);
}
HRESULT Device::SetTexture(DWORD stage,IDirect3DBaseTexture9*p){
  auto q=dynamic_cast<Texture*>(p);if(!allowed()||stage>=d9::PixelTextureStages||(p&&(!q||q->parent!=this)))return D3DERR_INVALIDCALL;
  if(q&&!q->resourceAllowed())return D3DERR_DEVICELOST;
  if(recording){Bind(recording->textures[stage],q);recording->textureMask[stage]=true;}
  else Bind(stage?additionalTextures[stage-1]:tex,q);return allowed()?D3D_OK:D3DERR_DEVICELOST;
}
HRESULT Device::GetTexture(DWORD stage,IDirect3DBaseTexture9**p){
  if(stage>=d9::PixelTextureStages)return D3DERR_INVALIDCALL;
  return Return(static_cast<IDirect3DBaseTexture9*>(stage?additionalTextures[stage-1]:tex),p);
}
HRESULT Device::syncTextureStages() {
  bool changed=false;
  for(uint32_t i=0;i<d9::AdditionalTextureStages;++i) {
    const auto* texture=additionalTextures[i];
    if(texture&&(texture->locked||!texture->uploaded))return D3DERR_INVALIDCALL;
    if(texture&&!texture->resourceAllowed())return D3DERR_DEVICELOST;
    textureStages.textures[i]=texture?texture->ref:d9::ResourceRef{};
    changed|=textureStages.textures[i]!=sentTextureStages.textures[i]
      ||textureStages.samplers[i].values!=sentTextureStages.samplers[i].values;
  }
  if(!changed)return D3D_OK;
  d9::Header h;h.op=d9::Op::SetTextureStages;h.payloadBytes=d9::TextureStagesBytes;
  const auto bytes=d9::EncodeTextureStages(textureStages);const auto hr=ordered(h,bytes.data());
  if(hr!=D3D_OK){if(SUCCEEDED(hr))conn->failClosed();return FAILED(hr)?hr:D3DERR_DEVICELOST;}
  sentTextureStages=textureStages;return D3D_OK;
}
static uint32_t* RenderField(dxvkcore::D3D9Backend::RenderStates& r,D3DRENDERSTATETYPE t){
  switch(t){case D3DRS_ZENABLE:return &r.zEnable;case D3DRS_ZWRITEENABLE:return &r.zWriteEnable;case D3DRS_ZFUNC:return &r.zFunc;
    case D3DRS_CULLMODE:return &r.cullMode;case D3DRS_ALPHABLENDENABLE:return &r.alphaBlendEnable;case D3DRS_SRCBLEND:return &r.srcBlend;
    case D3DRS_DESTBLEND:return &r.destBlend;case D3DRS_BLENDOP:return &r.blendOp;case D3DRS_COLORWRITEENABLE:return &r.colorWriteMask;default:return nullptr;}
}
HRESULT Device::SetRenderState(D3DRENDERSTATETYPE t,DWORD v){
  if(!allowed())return D3DERR_INVALIDCALL;
  const auto effectIndex=d9::EffectIndex(t);
  if(effectIndex>=0){if(!d9::ValidEffect(t,v))return D3DERR_INVALIDCALL;if(!recordRender(t,v))effects.values[effectIndex]=v;return D3D_OK;}
  const auto fixedIndex=d9::FFIndex(d9::FFRenderStates,t);
  if(fixedIndex>=0){if(!d9::ValidFFRender(t,v))return D3DERR_INVALIDCALL;if(!recordRender(t,v))fixed.render[fixedIndex]=v;return D3D_OK;}
  if(t==D3DRS_STENCILENABLE||t==D3DRS_SRGBWRITEENABLE)return v==FALSE?D3D_OK:D3DERR_INVALIDCALL;
  // DXVK has no Vulkan dither; the value is kept for GetRenderState only.
  if(t==D3DRS_DITHERENABLE){if(v>1)return D3DERR_INVALIDCALL;if(!recordRender(t,v))dither=v;return D3D_OK;}
  auto field=RenderField(draw.render,t);if(!field)return E_NOTIMPL;
  bool valid=false;
  // The host's implemented domains: every compare/cull/blend factor up to
  // SRCALPHASAT and every blend op (BOTH*/BLENDFACTOR stay refused).
  switch(t){case D3DRS_ZENABLE:case D3DRS_ZWRITEENABLE:case D3DRS_ALPHABLENDENABLE:valid=v<=1;break;
    case D3DRS_ZFUNC:valid=v>=D3DCMP_NEVER&&v<=D3DCMP_ALWAYS;break;
    case D3DRS_CULLMODE:valid=v>=D3DCULL_NONE&&v<=D3DCULL_CCW;break;
    case D3DRS_SRCBLEND:case D3DRS_DESTBLEND:valid=v>=D3DBLEND_ZERO&&v<=D3DBLEND_SRCALPHASAT;break;
    case D3DRS_BLENDOP:valid=v>=D3DBLENDOP_ADD&&v<=D3DBLENDOP_MAX;break;case D3DRS_COLORWRITEENABLE:valid=v<=15;break;default:break;}
  if(!valid)return D3DERR_INVALIDCALL;if(!recordRender(t,v))*field=v;return D3D_OK;
}
HRESULT Device::GetRenderState(D3DRENDERSTATETYPE t,DWORD* p){
  if(!p)return D3DERR_INVALIDCALL;
  const auto effectIndex=d9::EffectIndex(t);
  if(effectIndex>=0){*p=effects.values[effectIndex];return D3D_OK;}
  const auto fixedIndex=d9::FFIndex(d9::FFRenderStates,t);
  if(fixedIndex>=0){*p=fixed.render[fixedIndex];return D3D_OK;}
  if(t==D3DRS_STENCILENABLE||t==D3DRS_SRGBWRITEENABLE){*p=FALSE;return D3D_OK;}
  if(t==D3DRS_DITHERENABLE){*p=dither;return D3D_OK;}
  auto field=RenderField(draw.render,t);if(!field)return E_NOTIMPL;*p=*field;return D3D_OK;
}
#include "d3d9_proxy_stateblock.h"

HRESULT Device::Clear(DWORD count,const D3DRECT* rects,DWORD flags,D3DCOLOR color,float depth,DWORD stencil){
  if(conn->isOwner()&&!conn->healthy())return D3DERR_DEVICELOST;
  followWindow();
  if(!allowed())return D3DERR_INVALIDCALL;
  // Same no-op/null-rectangle conventions as the original D3D9DeviceEx::Clear.
  if(!count&&rects)return D3D_OK;
  if((flags&~DWORD(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER))||stencil||!std::isfinite(depth)||depth<0||depth>1)
    return D3DERR_INVALIDCALL;
  if(!flags)return D3D_OK;
  if(!rects)count=0;
  if(count>d9::MaxClearRegions)return D3DERR_INVALIDCALL;
  std::array<uint8_t,d9::MaxClearRegions*d9::ClearRegionBytes> payload{};
  uint32_t used=0;
  const auto append=[&](d9::ClearRegion r) {
    if(effects.get(D3DRS_SCISSORTESTENABLE)) {
      const auto& clip=effects.scissor;
      const D3DVIEWPORT9 view{r.x,r.y,r.width,r.height,0,1};
      r=d9::IntersectClear(view,{clip[0],clip[1],clip[2],clip[3]});
    }
    if(r.width&&r.height)d9::EncodeClearRegion(payload.data()+(used++)*d9::ClearRegionBytes,r);
  };
  if(count)for(uint32_t i=0;i<count;++i) {
    D3DRECT r{};if(!ReadSelf(rects+i,&r,sizeof(r)))return D3DERR_INVALIDCALL;
    append(d9::IntersectClear(draw.viewport,r));
  } else append({draw.viewport.X,draw.viewport.Y,draw.viewport.Width,draw.viewport.Height});
  if(!used)return D3D_OK;
  d9::Header h;h.args[0]=color;h.args[1]=d9::FloatBits(depth);h.args[2]=flags;
  if(used==1&&d9::U32(payload.data())==0&&d9::U32(payload.data()+4)==0
    &&d9::U32(payload.data()+8)==width&&d9::U32(payload.data()+12)==height) {
    h.op=d9::Op::Clear;return conn->enqueueClear(h);
  }
  h.op=d9::Op::ClearRegions;h.args[3]=used;h.payloadBytes=used*d9::ClearRegionBytes;
  return conn->call(h,payload.data());
}
// Next-draw annotations belong to exactly one draw call, submitted or not.
struct NextDrawAnnotations {
  Connection& conn;
  ~NextDrawAnnotations(){if(conn.isOwner())conn.discardNextDrawAnnotations();}
};
HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type,UINT start,UINT prim){
  const NextDrawAnnotations scope{*conn};
  if(!allowed()||!scene||!vb||!decl||vb->locked||(tex&&(tex->locked||!tex->uploaded))
    ||!d9::ValidDrawTopology(type,prim)||!draw.stride||decl->extent>draw.stride
    ||uint64_t(draw.streamOffset)+(uint64_t(start)+d9::DrawElementCount(type,prim)-1)*draw.stride+decl->extent>vb->initializedPrefix)
    return D3DERR_INVALIDCALL;
  if(!vb->resourceAllowed()||(tex&&!tex->resourceAllowed())||(vs&&!vs->resourceAllowed())
    ||(ps&&!ps->resourceAllowed())||!decl->resourceAllowed())return D3DERR_DEVICELOST;
  d9::DrawCommand command;command.resources={vb->ref,{},tex?tex->ref:d9::ResourceRef{},vs?vs->ref:d9::ResourceRef{},ps?ps->ref:d9::ResourceRef{},decl->ref};command.state=draw;
  auto& s=command.state;s.baseVertexIndex=0;s.minVertexIndex=0;s.numVertices=uint32_t(d9::DrawElementCount(type,prim));
  s.startIndex=start;s.primitiveCount=prim;s.primitiveType=type;
  return submit(command);
}
HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type,INT base,UINT min,UINT num,UINT start,UINT prim){
  const NextDrawAnnotations scope{*conn};
  if(!allowed()||!scene||!vb||!ib||!decl||vb->locked||ib->locked||(tex&&(tex->locked||!tex->uploaded))
    ||!d9::ValidDrawTopology(type,prim)||base<0||!num||start>ib->initializedPrefix/2
    ||d9::DrawElementCount(type,prim)>ib->initializedPrefix/2-start
    ||min>65535||num>65536-min||!draw.stride||decl->extent>draw.stride)return D3DERR_INVALIDCALL;
  const auto indices=uint32_t(d9::DrawElementCount(type,prim));
  if(!vb->resourceAllowed()||!ib->resourceAllowed()||(tex&&!tex->resourceAllowed())
    ||(vs&&!vs->resourceAllowed())||(ps&&!ps->resourceAllowed())||!decl->resourceAllowed())return D3DERR_DEVICELOST;
  const auto acquired=ib->acquireContent();if(acquired!=D3D_OK)return acquired;
  bool indicesValid=true;
  for(UINT i=0;i<indices&&indicesValid;++i){uint16_t index;std::memcpy(&index,ib->bytes.data()+(start+i)*2,2);
    const uint64_t end=uint64_t(draw.streamOffset)+(uint64_t(base)+index)*draw.stride+decl->extent;
    indicesValid=index>=min&&index<uint64_t(min)+num&&end<=vb->initializedPrefix;}
  ib->releaseContent();if(!indicesValid)return D3DERR_INVALIDCALL;
  d9::DrawCommand command;command.resources={vb->ref,ib->ref,tex?tex->ref:d9::ResourceRef{},vs?vs->ref:d9::ResourceRef{},ps?ps->ref:d9::ResourceRef{},decl->ref};command.state=draw;
  auto& s=command.state;
  s.baseVertexIndex=base;s.minVertexIndex=min;s.numVertices=num;s.startIndex=start;s.primitiveCount=prim;s.primitiveType=type;
  return submit(command);
}
} // namespace warvk::host::d3d9proxy

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT version){
  if(version!=D3D_SDK_VERSION||GetModuleHandleW(L"Game.dll"))return nullptr;
  try{auto* f=new warvk::host::d3d9proxy::Factory();f->AddRef();return f;}catch(...){return nullptr;}
}
// Laboratory-only diagnostic, not a D3D9/JASS/game API. The returned bytes came
// from the actual acquired swapchain image, before presentation transferred its
// ownership. Local pointer arguments never cross the IPC boundary.
extern "C" HRESULT WINAPI WarVKD3D9ReadPresentedImage(IDirect3DDevice9* input,void* bytes,UINT size,UINT64* frame){
  auto* device=dynamic_cast<warvk::host::d3d9proxy::Device*>(input);
  return device?device->readPresented(bytes,size,frame):D3DERR_INVALIDCALL;
}
// Explicit cold laboratory observation. No GPU command/flush and no automatic
// sampling on normal APIs. The outer C2 reply still verifies nonce/sequence.
extern "C" HRESULT WINAPI WarVKD3D9DiagnosticCheckpoint(IDirect3DDevice9* input,UINT phase,UINT ordinal,
    warvk::host::d9::memory::Checkpoint* output,UINT bytes){
  auto* device=dynamic_cast<warvk::host::d3d9proxy::Device*>(input);
  return device?device->diagnosticCheckpoint(phase,ordinal,output,bytes):D3DERR_INVALIDCALL;
}
// x86 data-layer annotations, negotiated by WARVK_D3D9_ANNOTATIONS=1. Values
// only (d3d9_annotation_wire.h); a draw annotation applies to the next draw call.
extern "C" HRESULT WINAPI WarVKD3D9AnnotateNextDraw(IDirect3DDevice9* input,UINT schema,const void* data,UINT bytes){
  auto* device=dynamic_cast<warvk::host::d3d9proxy::Device*>(input);
  if(!device||schema>0xffff||bytes>0xffff)return D3DERR_INVALIDCALL;
  return device->conn->annotateNextDraw(uint16_t(schema),data,uint16_t(bytes));
}
extern "C" HRESULT WINAPI WarVKD3D9SetFrameScene(IDirect3DDevice9* input,UINT schema,const void* data,UINT bytes){
  auto* device=dynamic_cast<warvk::host::d3d9proxy::Device*>(input);
  return device?device->conn->frameScene(schema,data,bytes):D3DERR_INVALIDCALL;
}
extern "C" HRESULT WINAPI WarVKD3D9MarkStage(IDirect3DDevice9* input,UINT stage){
  auto* device=dynamic_cast<warvk::host::d3d9proxy::Device*>(input);
  return device?device->conn->stageMarker(stage):D3DERR_INVALIDCALL;
}
