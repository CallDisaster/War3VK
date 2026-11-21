// Two-mode comparison of the shadow pass (phase 4c).
//
// One fixture, one sequence of D3D9 draws, scene records, caster records and
// stage markers, through either the original in-process renderer
// (--reference, its private IWarVKSceneEffectsFixture) or the x64 proxy
// (WarVKD3D9SetFrameScene / WarVKD3D9AnnotateNextDraw / WarVKD3D9MarkStage).
// In both the shadow pass is the same source, fed from the same records; the
// gate requires the two modes to agree on every frame and on the pass's own
// account of what it did.
//
// The world is drawn through the fixed-function transforms from object-space
// positions: a caster's geometry is the draw's own vertex and index buffer,
// so the vertices have to be what the pass can replay. The view and
// projection are the ones sent as the camera record.
//
// The sun is authored by hand (no celestial motion, a held lighting clock), so
// no clock or time of day takes part in what the frames look like - except in
// the two frames that follow the game's clock, which name their time.
//
// Lifecycle (phase 4c-3). What the pass kept of earlier frames must go when the
// x86 data layer says so, in both modes at the same frame:
//   - a ShadowFrame record whose tombstone serial or policy revision changed;
//   - a session reset (the map changed), which both modes take at a Present;
//   - a device Reset.
// Each is followed by a frame with an empty caster set: where the pass would
// hold its last shadow map, it has none. --taa-module-off runs the same frames
// with the shadow TAA module switched off, in the records and (in process) in
// the runtime profile, and adds a frame that asks for shadow TAA.
//
// More casters (phase 4c-2). A box drawn from dynamic buffers that are
// rewritten right after its draw, before BeforeUi: the pass shadows it as it
// was drawn, from the executor's copy. A box drawn alpha-tested through a
// striped texture: with a ShadowCasterAlpha record its shadow is striped as
// well, without one it is the whole box. Shadow TAA asked for in three frames
// before all others (its history images stay with the pass from then on): the
// second reads the first's history, and a new session leaves none, so the
// third draws as the first. With the module off the three draw as without.
// A point light above the boxes, first without shadows, then casting them.
// Box 1 outlined, its edges found on the screen, then drawn from its geometry.
// The volumetric light after the shadow pass (phase 4d): the sun's light
// shafts through the froxel grid, the same frame again (the first one's froxel
// history blended in), by the ray march, and with the lamp lighting the air.
// Its media (phase 4d-3): a fog volume around the boxes, scattering the
// light; the weather's haze and low snow, without a VolumetricLight record,
// and the same weather without them, which draws nothing. The lamp's contact
// shadows, traced through the depth.
#define wmain ExistingD3D9FixtureMain
#include "test_render_host_d3d9_parent.cpp"
#undef wmain
#include "../tools/render_host/process_memory_probe.h"
#include "../src/d3d9/war3/effects/war3_shadow_scene.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <vector>
namespace {
namespace lab=warvk::host::lab;
namespace scene=dxvk::war3::effects::scene;
constexpr DWORD ClearColor=0xff182028u,UiColor=0xffff00ffu;
constexpr UINT UiLeft=2,UiTop=2,UiSide=12,TimedFrames=12,Boxes=3;
// {5D6C0B8A-4F21-4E53-9A37-1C64E28F40B1}: src/d3d9/d3d9_war3_scene_effects_fixture.h
constexpr GUID FixtureIid={0x5d6c0b8a,0x4f21,0x4e53,{0x9a,0x37,0x1c,0x64,0xe2,0x8f,0x40,0xb1}};
// WarVKShadowFixtureRun and WarVKShadowFixtureAccount of that header.
struct ShadowRun {
  uint32_t casters=0,replayDraws=0,drawnCasters=0,cascades=0,cascadeDrawn[4]={};
  uint32_t mapExecuted=0,receiverExecuted=0,earlyReturn=0,inputReject=0,replayRejected=0;
  uint32_t completeMap=0,alphaPrepared=0,taaModule=0,taaMode=0,taaHistory=0,taaInvalidation=0,pointShadow=0,outline=0;
  uint32_t volumetric=0,volumetricComposite=0;
  int32_t volumetricBackend=-1;
  uint64_t volumetricBytes=0;
  uint32_t volumetricFogVolumes=0,volumetricWeather=0;
  float gameTime=0;
  uint64_t tombstoneSerial=0,stagePolicyRevision=0,resourceBytes=0;
};
struct ShadowAccount {
  uint64_t runs=0,mapRenders=0,receiverDraws=0,castersUsed=0,castersRefused=0,refusals[4]={},castersCopied=0,copiedBytes=0;
  uint64_t castersDrawn=0,replayRejected=0;
  uint64_t peakResourceBytes=0;
  ShadowRun last;
};
struct ISceneFixture:public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetFrameScene(UINT schema,const void* data,UINT bytes)=0;
  virtual HRESULT STDMETHODCALLTYPE MarkStage(UINT stage)=0;
  virtual HRESULT STDMETHODCALLTYPE DrawIndexedAnnotated(UINT schema,const void* data,UINT bytes,D3DPRIMITIVETYPE type,
    INT baseVertexIndex,UINT minVertexIndex,UINT numVertices,UINT startIndex,UINT primitiveCount)=0;
  virtual HRESULT STDMETHODCALLTYPE GetShadowAccount(ShadowAccount* account)=0;
  virtual HRESULT STDMETHODCALLTYPE RequestSessionReset(UINT reason,UINT generationLow,UINT generationHigh)=0;
  virtual HRESULT STDMETHODCALLTYPE AnnotateNextDraw(UINT schema,const void* data,UINT bytes)=0;
};
// War3RuntimeModule::ShadowTaa (src/d3d9/war3/core/war3_runtime_profile.h), and
// its name in DXVK_WAR3_DISABLE.
constexpr uint32_t ShadowTaaModule=1u<<9;
bool taaModuleOff=false,windowProcedureRestored=false,budgetFixture=false,budgetTemporal=false;
struct WorldVertex {float x,y,z;DWORD color;};
struct TexturedVertex {float x,y,z;DWORD color;float u,v;};
// The striped texture's side: two texel columns opaque, two transparent.
constexpr UINT StripeSide=8;
struct UiVertex {float x,y,z,w;DWORD color;};
struct Vec {float x,y,z;};
using Matrix=std::array<float,16>; // D3D row-vector convention, element (row, column) at row*4+column
struct Frame {const char* name;uint64_t hash;unsigned changed,uiPixels;};
struct Sample {const char* name;lab::ProcessMemory memory;};
std::vector<Frame> frames;std::vector<Sample> samples;
std::vector<uint8_t> world; // the frame without shadows, for the coverage counts
unsigned refusals=0,draws=0,casterDraws=0,presents=0,sessionResets=0,deviceResets=0;
double worldUs=0,shadowUs=0;
ShadowAccount account;
std::vector<ShadowRun> runLog; // in process: every run of the pass, in order

Vec Sub(Vec a,Vec b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec Cross(Vec a,Vec b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float Dot(Vec a,Vec b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec Unit(Vec a){const float n=std::sqrt(Dot(a,a));return {a.x/n,a.y/n,a.z/n};}
Matrix LookAt(Vec eye,Vec at,Vec up){
  const Vec z=Unit(Sub(at,eye)),x=Unit(Cross(up,z)),y=Cross(z,x);
  return {x.x,y.x,z.x,0, x.y,y.y,z.y,0, x.z,y.z,z.z,0, -Dot(x,eye),-Dot(y,eye),-Dot(z,eye),1};
}
Matrix Perspective(float fovY,float aspect,float zn,float zf){
  const float h=1/std::tan(fovY/2),w=h/aspect,q=zf/(zf-zn);
  return {w,0,0,0, 0,h,0,0, 0,0,q,1, 0,0,-q*zn,0};
}
constexpr Matrix Identity={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
// The unit box [-1,1] x [-1,1] x [0,1] placed at (cx, cy) on the ground.
Matrix Place(float cx,float cy,float half,float height){return {half,0,0,0, 0,half,0,0, 0,0,height,0, cx,cy,0,1};}
D3DMATRIX D3D(const Matrix& m){D3DMATRIX r;std::memcpy(&r,m.data(),sizeof(r));return r;}
void Probe(const char* name){
  const auto memory=lab::ProbeOwnProcessMemory();Require(memory.valid,"own process memory sample");
  samples.push_back({name,memory});
}
struct Box {float cx,cy,half,height;};
constexpr std::array<Box,Boxes> BoxList={{{-300,0,110,260},{250,150,110,180},{0,-350,90,320}}};
// What one frame sends. Casters: bit i marks box i; Ground the terrain quad.
enum : unsigned {Box0=1,Box1=2,Box2=4,AllBoxes=7,Ground=8};
// The sun of a frame: by hand, moved, without strength, following the game's
// clock, by hand with shadow TAA asked for, by hand with point lights and
// their shadows switched on, and with their contact shadows as well.
enum class Light {Base,Moved,Zero,Clock,Taa,Point,Contact,ContactHiz};
struct Plan {
  bool markers=true,lighting=false,frame=true,dynamicBox=false,ssaoSmaa=false;
  bool edgeScene=false, padded=false;
  // Box 0 textured and alpha-tested; alphaRecord: with its ShadowCasterAlpha record.
  bool alphaBox=false,alphaRecord=false;
  // A PointLights record with the lamp; pointShadow: the lamp casts shadows.
  bool pointLight=false,pointShadow=false;
  // An Outline record naming box 1: 1 its screen-space edges, 2 its geometry.
  unsigned outline=0;
  // A VolumetricLight record: 1 the froxel grid, 2 the ray march.
  unsigned volumetric=0;
  // A FogVolumes record: a box of fog around the boxes, and a sphere switched off.
  bool fog=false;
  // A Snow record without snowfall: 1 with haze and low snow, 2 without.
  unsigned weather=0;
  Light light=Light::Base;
  unsigned casters=0;
  int countDelta=0; // what the ShadowFrame record says beyond the casters sent
};
// The x86 data layer's facts a ShadowFrame record carries. The policy revision
// starts at 1 there, the tombstone serial at 0.
struct Facts {float time=12;uint64_t tombstoneSerial=0,stagePolicyRevision=1;};
struct Entry {
  using RecordFn=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT,const void*,UINT);
  using StageFn=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT);
  using ResetFn=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT,UINT,UINT);
  Com<ISceneFixture> local;RecordFn record=nullptr,annotate=nullptr;StageFn stage=nullptr;ResetFn reset=nullptr;
  IDirect3DDevice9* device=nullptr;
  // The map changed (1) or the game ended (2); both modes act on it at the next Present.
  HRESULT SessionReset(UINT reason,uint64_t generation){
    return local.get()?local->RequestSessionReset(reason,UINT(generation),UINT(generation>>32))
      :reset(device,reason,UINT(generation),UINT(generation>>32));
  }
  HRESULT Scene(UINT schema,const void* data,UINT bytes){
    return local.get()?local->SetFrameScene(schema,data,bytes):record(device,schema,data,bytes);
  }
  HRESULT Mark(scene::Stage value){return local.get()?local->MarkStage(UINT(value)):stage(device,UINT(value));}
  // A draw with a record: both modes refuse a bad record before drawing. An
  // alpha-tested caster's companion record follows the caster's own.
  HRESULT Caster(const void* data,UINT bytes,const scene::ShadowCasterAlpha* alpha,INT base,UINT min,UINT num,UINT start,UINT prims){
    const auto companion=alpha?scene::EncodeShadowCasterAlpha(*alpha):std::array<uint8_t,scene::ShadowCasterAlphaBytes>{};
    if(local.get()){
      if(alpha){const HRESULT hr=local->AnnotateNextDraw(scene::ShadowCasterAlphaSchema,companion.data(),UINT(companion.size()));if(FAILED(hr))return hr;}
      return local->DrawIndexedAnnotated(scene::ShadowCasterSchema,data,bytes,D3DPT_TRIANGLELIST,base,min,num,start,prims);
    }
    HRESULT hr=annotate(device,scene::ShadowCasterSchema,data,bytes);
    if(SUCCEEDED(hr)&&alpha)hr=annotate(device,scene::ShadowCasterAlphaSchema,companion.data(),UINT(companion.size()));
    return FAILED(hr)?hr:device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,base,min,num,start,prims);
  }
  // A companion record by itself, which both modes refuse when it is not one.
  HRESULT Alpha(const uint8_t* data,UINT bytes){
    return local.get()?local->AnnotateNextDraw(scene::ShadowCasterAlphaSchema,data,bytes)
      :annotate(device,scene::ShadowCasterAlphaSchema,data,bytes);
  }
};
void Run(Owner& o){
  o.stage="shadow-entry";
  Entry fx;fx.device=o.device.get();
  if(o.options.reference){
    void* raw=nullptr;Check(o.device->QueryInterface(FixtureIid,&raw),"in-process scene effects entry");
    *fx.local.put()=static_cast<ISceneFixture*>(raw);
  }else{
    fx.record=reinterpret_cast<Entry::RecordFn>(GetProcAddress(o.renderer,"WarVKD3D9SetFrameScene"));
    fx.annotate=reinterpret_cast<Entry::RecordFn>(GetProcAddress(o.renderer,"WarVKD3D9AnnotateNextDraw"));
    fx.stage=reinterpret_cast<Entry::StageFn>(GetProcAddress(o.renderer,"WarVKD3D9MarkStage"));
    fx.reset=reinterpret_cast<Entry::ResetFn>(GetProcAddress(o.renderer,"WarVKD3D9RequestSessionReset"));
    Require(fx.record&&fx.annotate&&fx.stage&&fx.reset,"scene exports");
  }
  o.stage="shadow-geometry";
  // Warcraft-like framing: Z up, the camera above and behind the origin.
  const Matrix view=budgetFixture?LookAt({0,-950,300},{0,0,50},{0,0,1}):LookAt({0,-1150,1050},{0,0,60},{0,0,1});
  const Matrix projection=Perspective(.9f,1,100,5000);
  // Ground: 4 vertices, 6 indices. Box: 24 vertices (4 a face), 36 indices.
  std::vector<WorldVertex> vertices={{-900,-900,0,0xff3a6b35},{900,-900,0,0xff3a6b35},{900,900,0,0xff3a6b35},{-900,900,0,0xff3a6b35}};
  std::vector<uint16_t> indices={0,1,2,0,2,3};
  const UINT groundVertices=4,groundIndices=6,boxVertices=24,boxIndices=36;
  auto face=[&](Vec a,Vec b,Vec c,Vec d,DWORD color){
    const uint16_t first=uint16_t(vertices.size()-groundVertices);
    for(Vec p:{a,b,c,d})vertices.push_back({p.x,p.y,p.z,color});
    for(uint16_t i:{0,1,2,0,2,3})indices.push_back(uint16_t(first+i));
  };
  face({-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1},0xffc8b090);
  face({-1,-1,0},{1,-1,0},{1,-1,1},{-1,-1,1},0xff9a7a58);
  face({1,-1,0},{1,1,0},{1,1,1},{1,-1,1},0xff8a6a48);
  face({1,1,0},{-1,1,0},{-1,1,1},{1,1,1},0xff7a5a38);
  face({-1,1,0},{-1,-1,0},{-1,-1,1},{-1,1,1},0xff826240);
  face({-1,-1,0},{-1,1,0},{1,1,0},{1,-1,0},0xff403020);
  Require(vertices.size()==groundVertices+boxVertices&&indices.size()==groundIndices+boxIndices,"fixture mesh");
  const DWORD fvf=D3DFVF_XYZ|D3DFVF_DIFFUSE,uiFvf=D3DFVF_XYZRHW|D3DFVF_DIFFUSE,texturedFvf=D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1;
  // The same mesh with texture coordinates, each box face spanning the
  // texture once.
  std::vector<TexturedVertex> textured;
  for(size_t i=0;i<vertices.size();++i){
    static constexpr float corner[4][2]={{0,1},{1,1},{1,0},{0,0}};
    const auto& v=vertices[i];const bool box=i>=groundVertices;
    textured.push_back({v.x,v.y,v.z,v.color,box?corner[(i-groundVertices)%4][0]:0.f,box?corner[(i-groundVertices)%4][1]:0.f});
  }
  const UINT texturedBytes=UINT(textured.size()*sizeof(TexturedVertex));
  auto fill=[&](IDirect3DVertexBuffer9* buffer,const void* bytes,UINT size,DWORD flags,const char* what){
    void* data=nullptr;Check(buffer->Lock(0,0,&data,flags),what);std::memcpy(data,bytes,size);Check(buffer->Unlock(),what);};
  const UINT vertexBytes=UINT(vertices.size()*sizeof(WorldVertex)),indexBytes=UINT(indices.size()*2);
  const float l=float(UiLeft),t=float(UiTop),r=float(UiLeft+UiSide),b=float(UiTop+UiSide);
  std::vector<UiVertex> ui;
  for(const auto& p:std::array<std::array<float,2>,6>{{{l,t},{r,t},{r,b},{l,t},{r,b},{l,b}}})ui.push_back({p[0]-.5f,p[1]-.5f,.5f,1,UiColor});
  Com<IDirect3DVertexBuffer9> dynamicVb,uiVb,texturedVb;Com<IDirect3DIndexBuffer9> dynamicIb;
  // White, its alpha in vertical stripes: texel columns 0-1 and 4-5 opaque,
  // 2-3 and 6-7 transparent. Managed, so it outlives a device Reset.
  Com<IDirect3DTexture9> stripes;
  Check(o.device->CreateTexture(StripeSide,StripeSide,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,stripes.put(),nullptr),"striped texture");
  {D3DLOCKED_RECT lock{};Check(stripes->LockRect(0,&lock,nullptr,0),"striped texture lock");
    for(UINT y=0;y<StripeSide;++y)for(UINT x=0;x<StripeSide;++x){
      const DWORD texel=((x>>1)&1)?0x00ffffffu:0xffffffffu;
      std::memcpy(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch+x*4,&texel,4);}
    Check(stripes->UnlockRect(0),"striped texture upload");}
  // The dynamic buffers' contents: the mesh, or what replaces it after the
  // caster draw - the boxes flat on the ground, every triangle degenerate.
  std::vector<WorldVertex> flatVertices=vertices;for(auto& v:flatVertices)v.z=0;
  const std::vector<uint16_t> degenerate(indices.size(),uint16_t(0));
  auto refill=[&](bool mesh){
    fill(dynamicVb.get(),(mesh?vertices:flatVertices).data(),vertexBytes,D3DLOCK_DISCARD,"dynamic VB rewrite");
    void* data=nullptr;Check(dynamicIb->Lock(0,0,&data,D3DLOCK_DISCARD),"dynamic IB rewrite lock");
    std::memcpy(data,(mesh?indices:degenerate).data(),indexBytes);Check(dynamicIb->Unlock(),"dynamic IB rewrite");
  };
  // Everything a device Reset takes away: the buffers, the readback surface
  // and the render state.
  auto resources=[&]{
    Check(o.device->CreateVertexBuffer(vertexBytes,D3DUSAGE_WRITEONLY,fvf,D3DPOOL_DEFAULT,o.vb.put(),nullptr),"world VB");
    fill(o.vb.get(),vertices.data(),vertexBytes,0,"world VB upload");
    Check(o.device->CreateIndexBuffer(indexBytes,D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_DEFAULT,o.ib.put(),nullptr),"world IB");
    {void* data=nullptr;Check(o.ib->Lock(0,0,&data,0),"world IB lock");std::memcpy(data,indices.data(),indexBytes);Check(o.ib->Unlock(),"world IB upload");}
    // The same mesh in dynamic buffers, rewritten around each draw from them.
    Check(o.device->CreateVertexBuffer(vertexBytes,D3DUSAGE_WRITEONLY|D3DUSAGE_DYNAMIC,fvf,D3DPOOL_DEFAULT,dynamicVb.put(),nullptr),"dynamic VB");
    fill(dynamicVb.get(),vertices.data(),vertexBytes,D3DLOCK_DISCARD,"dynamic VB upload");
    Check(o.device->CreateIndexBuffer(indexBytes,D3DUSAGE_WRITEONLY|D3DUSAGE_DYNAMIC,D3DFMT_INDEX16,D3DPOOL_DEFAULT,dynamicIb.put(),nullptr),"dynamic IB");
    {void* data=nullptr;Check(dynamicIb->Lock(0,0,&data,D3DLOCK_DISCARD),"dynamic IB lock");std::memcpy(data,indices.data(),indexBytes);Check(dynamicIb->Unlock(),"dynamic IB upload");}
    Check(o.device->CreateVertexBuffer(texturedBytes,D3DUSAGE_WRITEONLY,texturedFvf,D3DPOOL_DEFAULT,texturedVb.put(),nullptr),"textured VB");
    fill(texturedVb.get(),textured.data(),texturedBytes,0,"textured VB upload");
    Check(o.device->CreateVertexBuffer(UINT(ui.size()*sizeof(UiVertex)),D3DUSAGE_WRITEONLY,uiFvf,D3DPOOL_DEFAULT,uiVb.put(),nullptr),"ui VB");
    fill(uiVb.get(),ui.data(),UINT(ui.size()*sizeof(UiVertex)),0,"ui VB upload");
    Check(o.device->CreateOffscreenPlainSurface(Width,Height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,o.readback.put(),nullptr),"shadow readback");
    Check(o.device->SetRenderState(D3DRS_LIGHTING,FALSE),"scene unlit");
    Check(o.device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"scene cull");
    Check(o.device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL),"scene depth function");
    Check(o.device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1),"scene color op");
    Check(o.device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE),"scene color");
    Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1),"scene alpha op");
    Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE),"scene alpha");
    const D3DMATRIX v=D3D(view),p=D3D(projection);
    Check(o.device->SetTransform(D3DTS_VIEW,&v),"view transform");Check(o.device->SetTransform(D3DTS_PROJECTION,&p),"projection transform");
  };
  resources();
  // A static index buffer keeps the declared span live until BeforeUi. The
  // two 800000-byte VBs contain the same box as the 24-vertex control.
  // Each costs 600000 compact bytes. Either fits the 1 MiB host, both do not.
  constexpr UINT paddedCount=50000;
  std::array<Com<IDirect3DVertexBuffer9>,2> padded;
  if(budgetFixture){
    std::vector<WorldVertex> large(paddedCount);
    std::copy(vertices.begin()+groundVertices,vertices.end(),large.begin());
    for(auto& vb:padded){
      const UINT bytes=UINT(large.size()*sizeof(WorldVertex));
      Check(o.device->CreateVertexBuffer(bytes,D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,fvf,D3DPOOL_DEFAULT,vb.put(),nullptr),"pressure VB");
      fill(vb.get(),large.data(),bytes,D3DLOCK_DISCARD,"pressure VB upload");
    }
  }
  // Both modes take a session reset at a Present, and the in-process device a
  // device Reset too. The proxy's offscreen host presents nothing.
  auto present=[&]{
    const HRESULT hr=o.device->Present(nullptr,nullptr,nullptr,nullptr);
    Require(hr==D3D_OK||(!o.options.reference&&hr==E_NOTIMPL),"Present");++presents;
  };
  // A device Reset to the same mode: everything in the default pool goes
  // first, as D3D9 requires.
  auto resetDevice=[&]{
    Check(o.device->SetStreamSource(0,nullptr,0,0),"reset unbind stream");Check(o.device->SetIndices(nullptr),"reset unbind indices");
    dynamicVb.reset();dynamicIb.reset();uiVb.reset();texturedVb.reset();o.vb.reset();o.ib.reset();o.readback.reset();o.back.reset();o.target.reset();
    D3DPRESENT_PARAMETERS params{};
    params.BackBufferWidth=Width;params.BackBufferHeight=Height;params.BackBufferFormat=D3DFMT_A8R8G8B8;params.BackBufferCount=1;
    params.MultiSampleType=D3DMULTISAMPLE_NONE;params.SwapEffect=D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow=o.window.window.load(std::memory_order_acquire);params.Windowed=TRUE;
    params.EnableAutoDepthStencil=TRUE;params.AutoDepthStencilFormat=D3DFMT_D32F_LOCKABLE;
    params.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    Check(o.device->Reset(&params),"device Reset");++deviceResets;
    Check(o.device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,o.back.put()),"back buffer after Reset");
    Check(o.device->GetRenderTarget(0,o.target.put()),"render target after Reset");
    resources();
  };

  scene::Camera camera;camera.view=view;camera.projection=projection;
  camera.x=0;camera.y=0;camera.width=Width;camera.height=Height;camera.minZ=0;camera.maxZ=1;
  // The settings defaults with the sun authored by hand: a fixed direction and
  // strength, no celestial motion, no time colour grading, a held clock. The
  // cascades are 1024 texels: this is a 64-pixel picture.
  scene::Lighting lighting;
  lighting.celestialMotionEnabled=0;lighting.timeColorGradingEnabled=0;lighting.clockMode=1;
  lighting.sunDirection={.45f,.30f,-.84f,0};lighting.shadowStrength=.75f;lighting.csmShadowResolution=1024;
  scene::Lighting moved=lighting;moved.sunDirection={-.50f,.20f,-.84f,0};
  scene::Lighting zero=lighting;zero.shadowStrength=0;
  // The sun on its daily path, at the game's time of day (ShadowFrame).
  scene::Lighting clock=lighting;clock.celestialMotionEnabled=1;clock.clockMode=0;
  scene::Lighting taa=lighting;taa.shadowTaaEnabled=1;
  // Point lights and one point shadow, its cube 256 texels a side.
  scene::Lighting point=lighting;point.pointLightsEnabled=1;point.pointShadowEnabled=1;point.pointShadowMaxLights=1;
  point.pointShadowResolution=256;
  // Their contact shadows alone, without the cube shadows: a contact shadow
  // only adds occlusion where the cube's has none, and the cube's already
  // darkens all the rays find here. Traced in the receiver through the depth
  // (A0), and by default from the half-resolution depth pyramid the pass's
  // compute passes build first (A1).
  scene::Lighting contactHiz=point;contactHiz.pointShadowEnabled=0;contactHiz.pointRayShadowEnabled=1;
  scene::Lighting contact=contactHiz;contact.pointRayShadowHiZEnabled=0;
  auto lightOf=[&](Light light)->const scene::Lighting&{
    return light==Light::Moved?moved:light==Light::Zero?zero:light==Light::Clock?clock:light==Light::Taa?taa
      :light==Light::Point?point:light==Light::Contact?contact:light==Light::ContactHiz?contactHiz:lighting;};
  // A warm lamp above the middle of the boxes.
  scene::PointLight lamp;lamp.position={0,100,300};lamp.range=1400;lamp.color={1,.85f,.6f};lamp.intensity=3;
  Facts facts;
  const scene::Ssao ssao{6.0f,1.0f,0.02f,1.5f,0.15f,0.75f};
  const scene::Aa smaa{5,.75f,.166f,.0833f,.05f,32,16};
  auto caster=[&](unsigned index){
    scene::ShadowCaster c;
    if(index==Boxes){ // the ground, as the terrain
      c.world=Identity;c.boundsCenter={0,0,0};c.boundsRadius=1273;c.category=2;c.batchTag=0;c.stage=1;
      c.geometryKey=0x7e44a1ull;
    }else{
      const Box& box=BoxList[index];c.world=Place(box.cx,box.cy,box.half,box.height);
      c.boundsCenter={box.cx,box.cy,box.height/2};
      c.boundsRadius=std::sqrt(2*box.half*box.half+box.height*box.height/4);
      c.objectKind=2;c.category=3;c.batchTag=1;c.stage=11;
      c.rawcode=0x68626f78u;c.objectHandle=0x100u+index;c.part=0x1000u+index;c.batchHandle=0x100u+index;
      c.geometryKey=0xb0000ull+index;
    }
    c.flags=scene::ShadowCasterFlag::DepthWrite|scene::ShadowCasterFlag::DepthTest|scene::ShadowCasterFlag::BoundsIdentityProven;
    c.boundsProvenance=3;c.boundsGeneration=1;c.boundsAge=0;
    return c;
  };

  // One frame. The UI quad is drawn after BeforeUi with depth off: the pass
  // must not touch it.
  auto frame=[&](const Plan& plan){
    if(plan.markers){
      camera.frame=UINT(frames.size());const auto bytes=scene::EncodeCamera(camera);
      Check(fx.Scene(scene::CameraSchema,bytes.data(),UINT(bytes.size())),"camera record");
      if(plan.lighting){const auto p=scene::EncodeLighting(lightOf(plan.light));
        Check(fx.Scene(scene::LightingSchema,p.data(),UINT(p.size())),"lighting record");}
      if(plan.pointLight){scene::PointLights lights;lights.count=1;lights.lights[0]=lamp;
        lights.lights[0].shadowIntensity=plan.pointShadow?1.f:0.f;
        std::array<uint8_t,scene::PointLightsMaxBytes> p;const uint32_t bytes=scene::EncodePointLights(lights,p);
        Check(fx.Scene(scene::PointLightsSchema,p.data(),bytes),"point lights record");}
      if(plan.outline){scene::Outline o;o.useScreenSpace=plan.outline==1?1u:0u;o.showVisible=1;
        o.handleCount=1;o.handles[0]=0x101u; // box 1's batchHandle
        std::array<uint8_t,scene::OutlineMaxBytes> p;const uint32_t bytes=scene::EncodeOutline(o,p);
        Check(fx.Scene(scene::OutlineSchema,p.data(),bytes),"outline record");}
      // The settings' defaults, switched on, with the light's own sun map at
      // 512 texels (the default 2048 is for a screen, not 64 pixels).
      if(plan.volumetric){scene::VolumetricLight v;v.enabled=1;v.volumeSunResolution=512;
        if(plan.volumetric==2)v.quality=0; // War3VolumetricQuality::LegacyRayMarch
        const auto p=scene::EncodeVolumetricLight(v);
        Check(fx.Scene(scene::VolumetricLightSchema,p.data(),UINT(p.size())),"volumetric light record");}
      // A turned box of fog over the boxes, and a sphere of it switched off.
      if(plan.fog){scene::FogVolumes f;f.count=2;
        auto& box=f.volumes[0];box.shape=1;box.position={0,0,160};box.halfSize={520,520,170};
        box.rotationDegrees={0,0,30};box.density=.8f;box.edgeFeather=.2f;
        auto& off=f.volumes[1];off.shape=0;off.active=0;off.position={-300,0,130};off.halfSize={200,200,200};
        off.density=2;off.edgeFeather=0;
        std::array<uint8_t,scene::FogVolumesMaxBytes> p;const uint32_t bytes=scene::EncodeFogVolumes(f,p);
        Check(fx.Scene(scene::FogVolumesSchema,p.data(),bytes),"fog volumes record");}
      // The weather over the whole ground, without snowfall: the snow draws
      // nothing, the haze and low snow are the volumetric light's medium.
      if(plan.weather){scene::Snow s;s.haze=plan.weather==1?.6f:0.f;s.lowSnow=plan.weather==1?.5f:0.f;
        s.minX=-900;s.minY=-900;s.maxX=900;s.maxY=900;s.bottomZ=0;s.topZ=420;s.edgeFeather=.1f;
        s.timeSeconds=12.5;s.seed=7;s.id=1;
        const auto p=scene::EncodeSnow(s);Check(fx.Scene(scene::SnowSchema,p.data(),UINT(p.size())),"snow record");}
      if(plan.ssaoSmaa){const auto s=scene::EncodeSsao(ssao);Check(fx.Scene(scene::SsaoSchema,s.data(),UINT(s.size())),"ssao record");
        const auto a=scene::EncodeAa(smaa);Check(fx.Scene(scene::AaSchema,a.data(),UINT(a.size())),"aa record");}
    }
    Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,ClearColor,1,0),"scene clear");
    Check(o.device->BeginScene(),"scene begin");
    if(plan.markers)Check(fx.Mark(scene::Stage::WorldSceneEnter),"world scene enter");
    Check(o.device->SetRenderState(D3DRS_ZENABLE,TRUE),"world depth");Check(o.device->SetRenderState(D3DRS_ZWRITEENABLE,TRUE),"world depth write");
    Check(o.device->SetFVF(fvf),"world FVF");
    unsigned sent=0;
    // The companion record of an alpha-tested box: the texture of stage 0, a
    // threshold of one half, TEXCOORD0.
    scene::ShadowCasterAlpha cutout;cutout.stage=0;cutout.alphaRef=.5f;cutout.texcoord=0;
    auto draw=[&](bool marked,unsigned index,const Matrix& worldMatrix,INT base,UINT num,UINT start,UINT prims,bool alpha=false){
      const D3DMATRIX w=D3D(worldMatrix);Check(o.device->SetTransform(D3DTS_WORLD,&w),"world transform");
      if(marked){auto record=caster(index);
        record.world=worldMatrix;
        if(budgetFixture) {
          record.boundsCenter={worldMatrix[12],worldMatrix[13],worldMatrix[14]+worldMatrix[10]*.5f};
          record.boundsRadius=std::sqrt(worldMatrix[0]*worldMatrix[0]+worldMatrix[5]*worldMatrix[5]+
                                        .25f*worldMatrix[10]*worldMatrix[10]);
        }
        if(plan.edgeScene){
          // As in the game, the wire sphere carries no culling authority.
          record.boundsProvenance=0;record.boundsGeneration=0;
          record.boundsCenter={worldMatrix[12],worldMatrix[13],0};
        }
        const auto p=scene::EncodeShadowCaster(record);
        Check(fx.Caster(p.data(),UINT(p.size()),alpha?&cutout:nullptr,base,0,num,start,prims),"caster draw");++sent;++casterDraws;}
      else Check(o.device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,base,0,num,start,prims),"world draw");
      ++draws;
    };
    Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(WorldVertex)),"world stream");Check(o.device->SetIndices(o.ib.get()),"world indices");
    draw(plan.markers&&(plan.casters&Ground),Boxes,Identity,0,groundVertices,0,groundIndices/3);
    if(budgetFixture){
      // Submit the expensive far caster before the near one. The unmarked
      // draw always paints the same world geometry for the pixel controls.
      const std::array<Box,2> boxes={{{160,500,80,170},{-70,-490,50,110}}};
      for(unsigned i=0;i<boxes.size();++i){
        const auto& b=boxes[i];
        Check(o.device->SetStreamSource(0,padded[i].get(),0,sizeof(WorldVertex)),"pressure stream");
        draw(plan.markers&&(plan.casters&(1u<<i)),i,Place(b.cx,b.cy,b.half,b.height),0,
             plan.padded?paddedCount:boxVertices,groundIndices,boxIndices/3);
      }
    }else for(unsigned i=0;i<Boxes;++i){
      const bool dynamic=(plan.dynamicBox&&i==0)||(plan.edgeScene&&i<2),cut=plan.alphaBox&&i==0;
      if(dynamic){refill(true);Check(o.device->SetStreamSource(0,dynamicVb.get(),0,sizeof(WorldVertex)),"dynamic stream");Check(o.device->SetIndices(dynamicIb.get()),"dynamic indices");}
      // Alpha from the texture, tested against 128; the colour stays the vertices'.
      if(cut){Check(o.device->SetFVF(texturedFvf),"textured FVF");
        Check(o.device->SetStreamSource(0,texturedVb.get(),0,sizeof(TexturedVertex)),"textured stream");
        Check(o.device->SetTexture(0,stripes.get()),"striped texture bind");
        Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE),"texture alpha");
        Check(o.device->SetRenderState(D3DRS_ALPHAREF,0x80),"alpha reference");
        Check(o.device->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATEREQUAL),"alpha function");
        Check(o.device->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE),"alpha test on");}
      Box box=BoxList[i];
      // Before the sun moves, the snapped C1 upper Y edge is at
      // light-space Y=726. The base of this box straddles it (world Y=760).
      // The second dynamic box cannot reach any of the four maps.
      if(plan.edgeScene&&i==0)box={0,760,90,260};
      if(plan.edgeScene&&i==1)box.cx=100000;
      draw(plan.markers&&(plan.casters&(1u<<i)),i,Place(box.cx,box.cy,box.half,box.height),INT(groundVertices),boxVertices,groundIndices,boxIndices/3,
        cut&&plan.alphaRecord);
      if(cut){Check(o.device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE),"alpha test off");
        Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE),"vertex alpha");
        Check(o.device->SetTexture(0,nullptr),"striped texture unbind");
        Check(o.device->SetFVF(fvf),"world FVF");Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(WorldVertex)),"world stream");}
      // The caller's next use of its dynamic buffers, before BeforeUi: what
      // the pass replays is what the draw read, not this.
      if(dynamic){refill(false);Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(WorldVertex)),"world stream");Check(o.device->SetIndices(o.ib.get()),"world indices");}
    }
    if(plan.markers){
      Check(fx.Mark(scene::Stage::WorldSceneExit),"world scene exit");
      if(plan.lighting&&plan.frame){scene::ShadowFrame f;f.timeOfDay=facts.time;
        f.tombstoneSerial=facts.tombstoneSerial;f.stagePolicyRevision=facts.stagePolicyRevision;
        f.runtimeDisabledModules=taaModuleOff?ShadowTaaModule:0;f.casters=uint32_t(int(sent)+plan.countDelta);
        const auto p=scene::EncodeShadowFrame(f);Check(fx.Scene(scene::ShadowFrameSchema,p.data(),UINT(p.size())),"shadow frame record");}
      Check(fx.Mark(scene::Stage::BeforeUi),"before UI");
    }
    Check(o.device->SetRenderState(D3DRS_ZENABLE,FALSE),"ui depth");Check(o.device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE),"ui depth write");
    Check(o.device->SetStreamSource(0,uiVb.get(),0,sizeof(UiVertex)),"ui stream");Check(o.device->SetFVF(uiFvf),"ui FVF");
    Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,0,2),"ui draw");++draws;
    Check(o.device->EndScene(),"scene end");
    if(plan.markers)Check(fx.Mark(scene::Stage::FrameEnd),"frame end");
    // In process the pass's account of each run is read as it happens; the
    // x64 host keeps the same list and prints it when the session ends.
    if(fx.local.get()){
      const uint64_t before=account.runs;Check(fx.local->GetShadowAccount(&account),"shadow account");
      if(account.runs!=before){Require(account.runs==before+1,"more than one run of the pass in a frame");runLog.push_back(account.last);}
    }
  };
  auto read=[&](const char* name){
    Check(o.device->GetRenderTargetData(o.target.get(),o.readback.get()),"shadow readback copy");
    D3DLOCKED_RECT lock{};Check(o.readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"shadow readback lock");
    std::vector<uint8_t> image(Width*Height*4);
    for(UINT y=0;y<Height;++y)std::memcpy(image.data()+y*Width*4,static_cast<const uint8_t*>(lock.pBits)+y*lock.Pitch,Width*4);
    Check(o.readback->UnlockRect(),"shadow readback unlock");
    if(!name)return;
    if(world.empty())world=image;
    Frame result{name,Hash(image.data(),image.size()),0,0};
    for(UINT y=0;y<Height;++y)for(UINT x=0;x<Width;++x){
      const size_t at=(y*Width+x)*4;DWORD pixel=0;std::memcpy(&pixel,image.data()+at,4);
      const bool inUi=x>=UiLeft&&x<UiLeft+UiSide&&y>=UiTop&&y<UiTop+UiSide;
      if(inUi)result.uiPixels+=pixel==UiColor;
      else result.changed+=std::memcmp(image.data()+at,world.data()+at,4)!=0;
    }
    Require(result.uiPixels==UiSide*UiSide,"the shadow pass touched the UI drawn after BeforeUi");
    char file[64];std::snprintf(file,sizeof(file),"frame-%s.bgra",name);
    FILE* out=std::fopen(file,"wb");Require(out&&std::fwrite(image.data(),1,image.size(),out)==image.size()&&!std::fclose(out),"frame image file");
    frames.push_back(result);
  };
  auto refused=[&](HRESULT hr,const char* what){Require(hr==D3DERR_INVALIDCALL,what);++refusals;};
  auto plan=[](unsigned casters){Plan p;p.lighting=true;p.casters=casters;return p;};

  if(budgetFixture){
    o.stage="cascade-pressure";
    Plan p=plan(0);
    if(budgetTemporal) p.light=Light::Taa;
    frame(p);read("world");
    ++facts.stagePolicyRevision;p.casters=Box1;frame(p);read("near-only");
    ++facts.stagePolicyRevision;p.casters=Box0;frame(p);read("far-only");
    ++facts.stagePolicyRevision;p.casters=Box0|Box1;frame(p);read("complete");
    p.padded=true;
    for(const char* name:{"age1","age2","age3","age4","age5","age6","age7","age8","age9","age10"}){
      frame(p);read(name);
    }
    p.padded=false;frame(p);read("recovered");
    for(auto& vb:padded)vb.reset();
  }else{
  o.stage="shadow-world";
  {Plan p;p.markers=false;frame(p);}read("world");
  Require(frames[0].changed==0,"baseline differs from itself");
  Probe("world");

  o.stage="shadow-refusals";
  // Records the pass cannot read: both modes refuse them where they are made.
  {auto p=scene::EncodeLighting(lighting);
    refused(fx.Scene(scene::LightingSchema,p.data(),UINT(p.size())-4),"short lighting record accepted");
    auto bad=p;const uint32_t nan=0x7fc00000u;std::memcpy(bad.data()+4,&nan,4);
    refused(fx.Scene(scene::LightingSchema,bad.data(),UINT(bad.size())),"NaN sun direction accepted");
    bad=p;bad[0]=2;refused(fx.Scene(scene::LightingSchema,bad.data(),UINT(bad.size())),"lighting switch that is not 0 or 1 accepted");
    auto mode=lighting;mode.clockMode=3;const auto q=scene::EncodeLighting(mode);
    refused(fx.Scene(scene::LightingSchema,q.data(),UINT(q.size())),"unknown lighting clock accepted");}
  {scene::ShadowFrame f;f.timeOfDay=12;auto p=scene::EncodeShadowFrame(f);
    auto bad=p;bad[44]=1;refused(fx.Scene(scene::ShadowFrameSchema,bad.data(),UINT(bad.size())),"shadow frame with a reserved word accepted");
    f.omittedCasters=2;p=scene::EncodeShadowFrame(f);
    refused(fx.Scene(scene::ShadowFrameSchema,p.data(),UINT(p.size())),"omissions without a reason accepted");
    f.omittedCasters=0;f.timeOfDay=std::nanf("");p=scene::EncodeShadowFrame(f);
    refused(fx.Scene(scene::ShadowFrameSchema,p.data(),UINT(p.size())),"shadow frame without a time accepted");}
  {Check(o.device->BeginScene(),"refusal scene");Check(o.device->SetFVF(fvf),"refusal FVF");
    Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(WorldVertex)),"refusal stream");Check(o.device->SetIndices(o.ib.get()),"refusal indices");
    const auto good=scene::EncodeShadowCaster(caster(0));
    auto attempt=[&](const std::array<uint8_t,scene::ShadowCasterBytes>& p,UINT bytes,const char* what){
      refused(fx.Caster(p.data(),bytes,nullptr,INT(groundVertices),0,boxVertices,groundIndices,boxIndices/3),what);};
    attempt(good,scene::ShadowCasterBytes-4,"short caster record accepted");
    auto c=caster(0);c.mode=0;attempt(scene::EncodeShadowCaster(c),scene::ShadowCasterBytes,"caster without a mode accepted");
    c=caster(0);c.flags|=1u<<20;attempt(scene::EncodeShadowCaster(c),scene::ShadowCasterBytes,"caster with an unknown flag accepted");
    c=caster(0);c.world[5]=std::nanf("");attempt(scene::EncodeShadowCaster(c),scene::ShadowCasterBytes,"caster with a NaN transform accepted");
    c=caster(0);c.boundsRadius=-1;attempt(scene::EncodeShadowCaster(c),scene::ShadowCasterBytes,"caster with a negative radius accepted");
    // A companion record that names a ninth stage, or no threshold.
    scene::ShadowCasterAlpha a;auto bad=scene::EncodeShadowCasterAlpha(a);bad[0]=8;
    refused(fx.Alpha(bad.data(),UINT(bad.size())),"alpha record of a ninth stage accepted");
    bad=scene::EncodeShadowCasterAlpha(a);const uint32_t nan=0x7fc00000u;std::memcpy(bad.data()+4,&nan,4);
    refused(fx.Alpha(bad.data(),UINT(bad.size())),"alpha record without a threshold accepted");
    Check(o.device->EndScene(),"refusal scene end");}
  refused(fx.SessionReset(0,1),"session reset without a reason accepted");
  refused(fx.SessionReset(3,1),"session reset with an unknown reason accepted");
  refused(fx.SessionReset(1,0),"session reset to generation 0 accepted");

  o.stage="shadow-frames";
  // Shadow TAA asked for: the first frame has no history, the second reads
  // the first's. The map changed before the third (taken at the Present).
  uint64_t generation=0;
  {Plan p=plan(AllBoxes);p.light=Light::Taa;frame(p);read("taa-first");frame(p);read("taa-history");
    Check(fx.SessionReset(1,++generation),"session reset request");++sessionResets;present();
    frame(p);read("taa-session");}
  frame(plan(AllBoxes));read("shadow");
  frame(plan(AllBoxes));read("shadow-again");
  {Plan p=plan(Box2);p.edgeScene=true;frame(p);read("cascade-edge-control");
    p.casters=AllBoxes;frame(p);read("cascade-edge");}
  {Plan p=plan(AllBoxes);p.light=Light::Moved;frame(p);}read("sun-moved");
  // The first sun again. The cascades' light basis continues from the frame
  // before (d3d9_war3_csm.cpp), so after the sun moved its roll is not the
  // first frame's: this is the picture later frames of this sun repeat.
  frame(plan(AllBoxes));read("sun-back");
  // The flat ground as a terrain caster: drawn into the cascades, and it
  // shadows nothing.
  frame(plan(AllBoxes|Ground));read("terrain");
  frame(plan(Box0));read("one-caster");
  // No new shadow map is published from an empty caster set, nor from an
  // incomplete one: no ShadowFrame record, a count that names a caster that
  // never arrived. The last map stays.
  frame(plan(0));read("no-casters");
  {Plan p=plan(AllBoxes);p.frame=false;frame(p);}read("unsealed");
  {Plan p=plan(Box0|Box1);p.countDelta=1;frame(p);}read("short-count");
  // A box in dynamic buffers, rewritten after its draw: a complete set.
  {Plan p=plan(AllBoxes);p.dynamicBox=true;frame(p);}read("dynamic");
  // Box 0 striped by its alpha test: without the companion record its shadow
  // is the whole box, with it the stripes.
  {Plan p=plan(AllBoxes);p.alphaBox=true;frame(p);}read("alpha-off");
  {Plan p=plan(AllBoxes);p.alphaBox=p.alphaRecord=true;frame(p);}read("alpha");
  // The lamp lights the boxes, then casts their shadows too.
  {Plan p=plan(AllBoxes);p.light=Light::Point;p.pointLight=true;frame(p);}read("point-light");
  {Plan p=plan(AllBoxes);p.light=Light::Point;p.pointLight=p.pointShadow=true;frame(p);}read("point-shadow");
  // Box 1 outlined where it is seen and where it is hidden: its edges found
  // on the screen, then drawn from its geometry.
  {Plan p=plan(AllBoxes);p.outline=1;frame(p);}read("outline");
  {Plan p=plan(AllBoxes);p.outline=2;frame(p);}read("outline-geometry");
  // The volumetric light: the sun's shafts through the froxel grid, the same
  // frame again with the first one's history, by the ray march, and with the
  // lamp.
  {Plan p=plan(AllBoxes);p.volumetric=1;frame(p);}read("volumetric");
  {Plan p=plan(AllBoxes);p.volumetric=1;frame(p);}read("volumetric-again");
  {Plan p=plan(AllBoxes);p.volumetric=2;frame(p);}read("volumetric-march");
  {Plan p=plan(AllBoxes);p.light=Light::Point;p.pointLight=true;p.volumetric=1;frame(p);}read("volumetric-lamp");
  // Its media: the fog volume, then the weather with and without its haze
  // and low snow, no VolumetricLight record. Then the lamp's contact shadows,
  // its cube shadows switched off: traced in the receiver, then from the
  // depth pyramid.
  {Plan p=plan(AllBoxes);p.volumetric=1;p.fog=true;frame(p);}read("fog-volume");
  {Plan p=plan(AllBoxes);p.weather=1;frame(p);}read("weather-medium");
  {Plan p=plan(AllBoxes);p.weather=2;frame(p);}read("weather-clear");
  {Plan p=plan(AllBoxes);p.light=Light::Contact;p.pointLight=p.pointShadow=true;frame(p);}read("point-contact");
  {Plan p=plan(AllBoxes);p.light=Light::ContactHiz;p.pointLight=p.pointShadow=true;frame(p);}read("point-contact-hiz");
  {Plan p=plan(AllBoxes);p.light=Light::Zero;frame(p);}read("strength-zero");
  {Plan p=plan(AllBoxes);p.ssaoSmaa=true;frame(p);}read("shadow-ssao-smaa");
  // Records absent: the pass does not run and the world is unchanged.
  {Plan p;frame(p);}read("off");
  frame(plan(AllBoxes));read("shadow-return");
  auto at=[&](const char* name)->const Frame&{
    for(const auto& f:frames)if(std::strcmp(f.name,name)==0)return f;
    Require(false,"frame name");return frames[0];};
  Require(at("off").hash==frames[0].hash&&at("off").changed==0,"a frame without records differs from the world");
  Require(at("strength-zero").hash==frames[0].hash,"a sun without strength changed the world");
  Require(at("shadow").changed&&at("sun-moved").changed&&at("terrain").changed&&at("one-caster").changed,"the shadows changed nothing");
  Require(at("shadow-again").hash==at("shadow").hash,"the same casters and sun gave another picture");
  Require(at("sun-moved").hash!=at("shadow").hash&&at("one-caster").hash!=at("sun-back").hash,"shadow frames are not distinct");
  Require(at("terrain").hash==at("sun-back").hash,"the flat ground cast a shadow");
  for(const char* held:{"no-casters","unsealed","short-count"})
    Require(at(held).hash==at("one-caster").hash,"a new shadow map came from an empty or incomplete caster set");
  Require(at("dynamic").hash==at("sun-back").hash,"a caster in dynamic buffers did not shadow as it was drawn");
  Require(at("cascade-edge").hash!=at("cascade-edge-control").hash,"the edge caster cast no visible shadow");
  Require(at("alpha-off").hash!=at("sun-back").hash,"the alpha-tested box drew as the plain one");
  Require(at("alpha").hash!=at("alpha-off").hash,"the alpha test did not reach the shadow");
  Require(at("point-light").hash!=at("sun-back").hash,"the point light lit nothing");
  Require(at("point-shadow").hash!=at("point-light").hash,"the point light cast no shadow");
  Require(at("outline").hash!=at("sun-back").hash,"the screen-space outline drew nothing");
  Require(at("outline-geometry").hash!=at("sun-back").hash,"the geometry outline drew nothing");
  Require(at("volumetric").hash!=at("sun-back").hash,"the volumetric light drew nothing");
  Require(at("fog-volume").hash!=at("volumetric").hash,"the fog volume scattered nothing");
  Require(at("weather-clear").hash==at("sun-back").hash,"weather without snowfall, haze or low snow drew something");
  Require(at("weather-medium").hash!=at("weather-clear").hash,"the weather medium drew nothing");
  Require(at("point-contact").hash!=at("point-light").hash,"the contact shadows drew nothing");
  Require(at("shadow-ssao-smaa").hash!=at("sun-back").hash,"the effects after the shadows changed nothing");
  Require(at("shadow-return").hash==at("sun-back").hash,"a frame without records changed the shadows that followed");
  // A new session leaves no shadow TAA history; with the module off the
  // requests change nothing.
  Require(at("taa-session").hash==at("taa-first").hash,"shadow TAA history outlived its session");
  if(!taaModuleOff)Require(at("taa-history").hash!=at("taa-first").hash,"shadow TAA did not blend its history in");
  if(taaModuleOff)for(const char* asked:{"taa-first","taa-history","taa-session"})
    Require(at(asked).hash==at("shadow").hash,"shadow TAA ran with its module off");
  Probe("shadows");

  o.stage="shadow-lifecycle";
  // A caster was removed for good: the tombstone serial moved. The empty set
  // that follows has no map to hold; the full set renders a new one.
  facts.tombstoneSerial=1;
  frame(plan(0));read("tombstone-held");
  frame(plan(AllBoxes));read("tombstone-fresh");
  // A frame without the ShadowFrame record is incomplete, and the data
  // layer's facts are what they were: the map of the frame before stays.
  {Plan p=plan(AllBoxes);p.frame=false;frame(p);}read("facts-kept");
  // The stage policy changed.
  facts.stagePolicyRevision=2;
  frame(plan(0));read("policy-held");
  frame(plan(AllBoxes));read("policy-fresh");
  // The sun on its daily path at the game's time of day. The pass smooths
  // that time against the wall clock from the second frame of the game's
  // clock on, so each of these two is the first one after a held clock.
  facts.time=9;{Plan p=plan(AllBoxes);p.light=Light::Clock;frame(p);}read("time-9");
  facts.time=12;frame(plan(AllBoxes));read("clock-held");
  facts.time=15;{Plan p=plan(AllBoxes);p.light=Light::Clock;frame(p);}read("time-15");
  facts.time=12;frame(plan(AllBoxes));read("time-return");
  // The map changed. Until the Present the old session goes on, and an empty
  // set holds its map; after it nothing of the old map is left.
  Check(fx.SessionReset(1,++generation),"session reset request");++sessionResets;
  refused(fx.SessionReset(2,generation),"repeated session generation accepted");
  frame(plan(0));read("session-pending");
  present();
  frame(plan(0));read("session-held");
  frame(plan(AllBoxes));read("session-fresh");
  // More maps, the last one ending with the game: each starts from nothing
  // and the pass owns what it owned in the first.
  for(const char* name:{"map-2","map-3","map-4"}){
    Check(fx.SessionReset(std::strcmp(name,"map-4")?1u:2u,++generation),"session reset request");++sessionResets;
    present();
    frame(plan(AllBoxes));read(name);
  }
  // A device Reset. The in-process device hands its receiver the new device
  // epoch at the Present that follows.
  resetDevice();
  present();
  frame(plan(0));read("reset-held");
  frame(plan(AllBoxes));read("reset-fresh");
  // Shadow TAA asked for while its module is off: the pass draws as without.
  if(taaModuleOff){Plan p=plan(AllBoxes);p.light=Light::Taa;frame(p);read("taa-requested");}
  for(const char* ended:{"tombstone-held","policy-held","session-held","reset-held"})
    Require(at(ended).hash==frames[0].hash&&at(ended).changed==0,"a shadow map outlived what ended it");
  for(const char* fresh:{"tombstone-fresh","policy-fresh"})
    Require(at(fresh).hash==at("shadow-return").hash,"the shadows after a lifecycle change are not the ones before it");
  Require(at("facts-kept").hash==at("tombstone-fresh").hash,"a frame without the ShadowFrame record changed the data layer's facts");
  Require(at("time-9").changed&&at("time-15").changed&&at("time-9").hash!=at("time-15").hash
    &&at("time-9").hash!=at("time-return").hash&&at("time-15").hash!=at("time-return").hash,"the game's time of day did not move the sun");
  Require(at("session-pending").hash==at("time-return").hash,"a session ended before the Present that ends it");
  for(const char* fresh:{"session-fresh","map-2","map-3","map-4","reset-fresh"})
    Require(at(fresh).hash==at("time-return").hash,"a new session or device does not draw the shadows of the last one's casters");
  if(taaModuleOff)Require(at("taa-requested").hash==at("reset-fresh").hash,"shadow TAA ran with its module off");
  Probe("lifecycle");

  o.stage="shadow-timing";
  // Wall time per frame including its readback (the only sync both modes share).
  auto timed=[&](bool shadows){
    std::vector<double> us;
    for(UINT i=0;i<TimedFrames;++i){
      const auto begin=std::chrono::steady_clock::now();
      if(shadows)frame(plan(AllBoxes));else{Plan p;frame(p);}
      read(nullptr);
      us.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count());
    }
    std::sort(us.begin(),us.end());return us[us.size()/2];
  };
  worldUs=timed(false);shadowUs=timed(true);
  Probe("timed");
  }

  o.stage="shadow-release";
  Check(o.device->SetStreamSource(0,nullptr,0,0),"shadow unbind stream");Check(o.device->SetIndices(nullptr),"shadow unbind indices");
  fx.local.reset();dynamicVb.reset();dynamicIb.reset();uiVb.reset();texturedVb.reset();stripes.reset();
  o.vb.reset();o.ib.reset();o.readback.reset();o.back.reset();o.target.reset();
  Require(o.device.detach()->Release()==0,"shadow final device Release");
  Require(o.factory.detach()->Release()==0,"shadow final factory Release");
  // The first Present of the in-process renderer creates a Vulkan swap chain
  // on the window, and the system's opengl32.dll then subclasses the window
  // above the renderer's own window procedure hook. The renderer takes its
  // hook out only while it is the window's procedure, so the chain still leads
  // into the renderer after the device is gone. The game never unloads the
  // renderer; this fixture does, so the window first gets its class's
  // procedure back.
  {const HWND window=o.window.window.load(std::memory_order_acquire);
    const LONG_PTR own=GetClassLongPtrW(window,GCLP_WNDPROC);
    if(own&&GetWindowLongPtrW(window,GWLP_WNDPROC)!=own){SetWindowLongPtrW(window,GWLP_WNDPROC,own);windowProcedureRestored=true;}}
  Require(FreeLibrary(o.renderer)!=FALSE,"shadow renderer FreeLibrary");o.renderer=nullptr;
  o.window.stop();Probe("closed");
  o.normalTeardown=true;o.stage="complete";
}
}
int wmain(int argc,wchar_t** argv){
  Owner* owner=nullptr;
  try{
    // This fixture's own switch; the rest is the parent's command line.
    std::vector<wchar_t*> arguments;
    for(int i=0;i<argc;++i){if(i&&!std::wcscmp(argv[i],L"--taa-module-off"))taaModuleOff=true;
      else if(i&&!std::wcscmp(argv[i],L"--budget-fixture"))budgetFixture=true;
      else if(i&&!std::wcscmp(argv[i],L"--budget-temporal"))budgetTemporal=true;
      else arguments.push_back(argv[i]);}
    // In process the switch is the runtime profile's, read once when the
    // renderer first asks; the proxy has no profile and the record says it.
    if(taaModuleOff)Require(SetEnvironmentVariableW(L"DXVK_WAR3_DISABLE",L"shadow.taa")!=FALSE,"runtime profile switch");
    owner=new Owner;owner->options=ParseOptions(int(arguments.size()),arguments.data());
    Require(!owner->options.invalid&&!owner->options.present&&!owner->options.wsi,"shadow workload is offscreen normal only");
    CreateDevice(*owner);Run(*owner);
    std::printf("{\"schema\":\"warvk-d3d9-shadow-fixture/v2\",\"ok\":true,\"reference\":%s,\"taaModuleOff\":%s,"
      "\"draws\":%u,\"casterDraws\":%u,\"refusals\":%u,\"presents\":%u,\"sessionResets\":%u,\"deviceResets\":%u,"
      "\"timedFrames\":%u,\"worldFrameUs\":%.1f,\"shadowFrameUs\":%.1f,"
      "\"windowProcedureRestored\":%s,\"x86VulkanAbsent\":%s,\"normalTeardown\":true,\"frames\":[",
      JsonBool(owner->options.reference),JsonBool(taaModuleOff),draws,casterDraws,refusals,presents,sessionResets,deviceResets,
      TimedFrames,worldUs,shadowUs,JsonBool(windowProcedureRestored),
      JsonBool(owner->parentVulkanAbsent&&GetModuleHandleW(L"vulkan-1.dll")==nullptr));
    for(size_t i=0;i<frames.size();++i)
      std::printf("%s{\"name\":\"%s\",\"hash\":\"%016llx\",\"changed\":%u,\"uiPixels\":%u}",i?",":"",frames[i].name,
        static_cast<unsigned long long>(frames[i].hash),frames[i].changed,frames[i].uiPixels);
    std::printf("],\"samples\":[");
    for(size_t i=0;i<samples.size();++i){
      std::printf("%s{\"name\":\"%s\",\"memory\":",i?",":"",samples[i].name);lab::PrintProcessMemory(samples[i].memory);std::printf("}");
    }
    std::printf("],");
    // The in-process pass's account; the x64 host prints its own in its log.
    if(owner->options.reference){
      const auto& a=account;
      // One run, as the x64 host prints its own (d3d9_host_main.cpp).
      const auto printRun=[](const ShadowRun& s){
        uint32_t time=0;std::memcpy(&time,&s.gameTime,4);
        std::printf("{\"casters\":%u,\"replayDraws\":%u,\"drawnCasters\":%u,\"cascades\":%u,"
          "\"cascadeDrawn\":[%u,%u,%u,%u],\"mapExecuted\":%u,\"receiverExecuted\":%u,"
          "\"earlyReturn\":%u,\"inputReject\":%u,\"replayRejected\":%u,\"completeMap\":%u,"
          "\"alphaPrepared\":%u,"
          "\"taaModule\":%u,\"taaMode\":%u,\"taaHistory\":%u,\"taaInvalidation\":%u,\"pointShadow\":%u,\"outline\":%u,"
          "\"volumetric\":%u,\"volumetricBackend\":%d,\"volumetricComposite\":%u,\"volumetricBytes\":%llu,"
          "\"volumetricFogVolumes\":%u,\"volumetricWeather\":%u,"
          "\"gameTimeBits\":\"%08x\",\"tombstoneSerial\":%llu,"
          "\"stagePolicyRevision\":%llu,\"resourceBytes\":%llu}",
          s.casters,s.replayDraws,s.drawnCasters,s.cascades,s.cascadeDrawn[0],s.cascadeDrawn[1],
          s.cascadeDrawn[2],s.cascadeDrawn[3],s.mapExecuted,s.receiverExecuted,s.earlyReturn,s.inputReject,
          s.replayRejected,s.completeMap,s.alphaPrepared,s.taaModule,s.taaMode,s.taaHistory,s.taaInvalidation,
          s.pointShadow,s.outline,s.volumetric,s.volumetricBackend,s.volumetricComposite,
          static_cast<unsigned long long>(s.volumetricBytes),s.volumetricFogVolumes,s.volumetricWeather,time,
          static_cast<unsigned long long>(s.tombstoneSerial),static_cast<unsigned long long>(s.stagePolicyRevision),
          static_cast<unsigned long long>(s.resourceBytes));
      };
      std::printf("\"shadow\":{\"runs\":%llu,\"mapRenders\":%llu,\"receiverDraws\":%llu,\"castersUsed\":%llu,"
        "\"castersRefused\":%llu,\"refusedDynamic\":%llu,\"refusedLayout\":%llu,\"refusedRange\":%llu,"
        "\"castersCopied\":%llu,\"copiedBytes\":%llu,"
        "\"castersDrawn\":%llu,\"replayRejected\":%llu,\"peakResourceBytes\":%llu,\"last\":",
        static_cast<unsigned long long>(a.runs),static_cast<unsigned long long>(a.mapRenders),
        static_cast<unsigned long long>(a.receiverDraws),static_cast<unsigned long long>(a.castersUsed),
        static_cast<unsigned long long>(a.castersRefused),static_cast<unsigned long long>(a.refusals[1]),
        static_cast<unsigned long long>(a.refusals[2]),static_cast<unsigned long long>(a.refusals[3]),
        static_cast<unsigned long long>(a.castersCopied),static_cast<unsigned long long>(a.copiedBytes),
        static_cast<unsigned long long>(a.castersDrawn),static_cast<unsigned long long>(a.replayRejected),
        static_cast<unsigned long long>(a.peakResourceBytes));
      printRun(a.last);
      std::printf(",\"runLog\":[");
      for(size_t i=0;i<runLog.size();++i){if(i)std::printf(",");printRun(runLog[i]);}
      std::printf("]},");
    }
    std::printf("\"memoryMeasured\":true,\"gameTouched\":false}\n");std::fflush(stdout);
    delete owner;return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"shadow fixture failed stage=%s reason=%s\n",owner?owner->stage:"allocate",e.what());}
  ExitProcess(2);
}
