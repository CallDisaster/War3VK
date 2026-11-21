// Two-mode comparison of the BeforeUi effect executors (phases 4a and 4b:
// SSAO, weather snow, FXAA/SMAA, bloom and tone map).
//
// One fixture, one sequence of D3D9 draws, scene records and stage markers,
// through either the original in-process renderer (--reference, its private
// IWarVKSceneEffectsFixture) or the x64 proxy (WarVKD3D9SetFrameScene /
// WarVKD3D9MarkStage). Every frame is read back, written to frame-<name>.bgra
// and hashed; the gate requires the two modes to agree byte for byte.
//
// The world is drawn from pre-transformed vertices that this fixture projects
// with the same view and projection matrices it sends as the camera record, so
// the depth buffer and the camera agree as they do in the game.
//
// With DXVK_WAR3_SCENE_EFFECTS_FIXTURE_LEGACY_BLOOM=1 the in-process arm runs
// the earlier D3D9-API bloom on bloom records instead of the executor: the
// gate measures the port against it and does not require equality.
//
// --depth-stencil (in-process reference only) binds a D24S8 surface instead
// of the D32F one both modes share. The x64 host has one depth format, so
// this variant has no remote counterpart: it runs the executors on a
// combined depth+stencil image, which the shared format never does.
#define wmain ExistingD3D9FixtureMain
#include "test_render_host_d3d9_parent.cpp"
#undef wmain
#include "../tools/render_host/process_memory_probe.h"
#include "../src/d3d9/war3/effects/war3_effect_scene.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <vector>
namespace {
namespace lab=warvk::host::lab;
namespace scene=dxvk::war3::effects::scene;
constexpr DWORD ClearColor=0xff182028u,UiColor=0xffff00ffu;
constexpr UINT UiLeft=2,UiTop=2,UiSide=12,TimedFrames=24;
// The effects of one frame.
enum : unsigned {Ssao=1,Snow=2,Fxaa=4,Smaa=8,Bloom=16,Tone=32,All=Ssao|Snow|Smaa|Bloom};
// {5D6C0B8A-4F21-4E53-9A37-1C64E28F40B1}: src/d3d9/d3d9_war3_scene_effects_fixture.h
constexpr GUID FixtureIid={0x5d6c0b8a,0x4f21,0x4e53,{0x9a,0x37,0x1c,0x64,0xe2,0x8f,0x40,0xb1}};
struct ISceneFixture:public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetFrameScene(UINT schema,const void* data,UINT bytes)=0;
  virtual HRESULT STDMETHODCALLTYPE MarkStage(UINT stage)=0;
};
struct Screen {float x,y,z,w;DWORD color;};
struct Vec {float x,y,z;};
using Matrix=std::array<float,16>; // D3D row-vector convention, element (row, column) at row*4+column
struct Frame {const char* name;uint64_t hash;unsigned changed,uiPixels;};
struct Sample {const char* name;lab::ProcessMemory memory;};
std::vector<Frame> frames;std::vector<Sample> samples;
std::vector<uint8_t> world; // the frame without effects, for the coverage counts
unsigned refusals=0,refusalsSkipped=0,worldVertices=0,draws=0;
bool depthStencil=false;
double worldUs=0,effectsUs=0;

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
// v * View * Projection, then the viewport: the D3D9 fixed-function path.
Screen Project(const Matrix& view,const Matrix& projection,Vec p,DWORD color){
  auto mul=[](const Matrix& m,const std::array<float,4>& v){std::array<float,4> r{};
    for(int c=0;c<4;++c)for(int k=0;k<4;++k)r[c]+=v[k]*m[k*4+c];return r;};
  const auto clip=mul(projection,mul(view,{p.x,p.y,p.z,1}));
  const float rhw=1/clip[3];
  return {(clip[0]*rhw*.5f+.5f)*Width,(.5f-clip[1]*rhw*.5f)*Height,clip[2]*rhw,rhw,color};
}
void Probe(const char* name){
  const auto memory=lab::ProbeOwnProcessMemory();Require(memory.valid,"own process memory sample");
  samples.push_back({name,memory});
}
struct Effects {
  using RecordFn=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT,const void*,UINT);
  using StageFn=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT);
  Com<ISceneFixture> local;RecordFn record=nullptr;StageFn stage=nullptr;IDirect3DDevice9* device=nullptr;
  HRESULT Scene(UINT schema,const void* data,UINT bytes){
    return local.get()?local->SetFrameScene(schema,data,bytes):record(device,schema,data,bytes);
  }
  HRESULT Mark(scene::Stage value){return local.get()?local->MarkStage(UINT(value)):stage(device,UINT(value));}
};
void Run(Owner& o){
  o.stage="scene-effects-entry";
  Effects fx;fx.device=o.device.get();
  if(o.options.reference){
    void* raw=nullptr;Check(o.device->QueryInterface(FixtureIid,&raw),"in-process scene effects entry");
    *fx.local.put()=static_cast<ISceneFixture*>(raw);
  }else{
    fx.record=reinterpret_cast<Effects::RecordFn>(GetProcAddress(o.renderer,"WarVKD3D9SetFrameScene"));
    fx.stage=reinterpret_cast<Effects::StageFn>(GetProcAddress(o.renderer,"WarVKD3D9MarkStage"));
    Require(fx.record&&fx.stage,"scene exports");
  }
  Com<IDirect3DSurface9> combinedDepth;
  if(depthStencil){
    o.stage="scene-effects-depth-stencil";
    Check(o.device->CreateDepthStencilSurface(Width,Height,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,FALSE,combinedDepth.put(),nullptr),"D24S8 surface");
    Check(o.device->SetDepthStencilSurface(combinedDepth.get()),"D24S8 bind");
  }
  o.stage="scene-effects-geometry";
  // Warcraft-like framing: Z up, the camera above and behind the origin.
  const Matrix view=LookAt({0,-1150,1050},{0,0,60},{0,0,1}),projection=Perspective(.9f,1,100,5000);
  std::vector<Screen> vertices;
  auto quad=[&](Vec a,Vec b,Vec c,Vec d,DWORD color){
    for(Vec p:{a,b,c,a,c,d})vertices.push_back(Project(view,projection,p,color));};
  quad({-900,-900,0},{900,-900,0},{900,900,0},{-900,900,0},0xff3a6b35);
  auto box=[&](float cx,float cy,float half,float height,DWORD top,DWORD side){
    const float x0=cx-half,x1=cx+half,y0=cy-half,y1=cy+half;
    quad({x0,y0,height},{x1,y0,height},{x1,y1,height},{x0,y1,height},top);
    quad({x0,y0,0},{x1,y0,0},{x1,y0,height},{x0,y0,height},side);
    quad({x1,y0,0},{x1,y1,0},{x1,y1,height},{x1,y0,height},side-0x00101010);
    quad({x1,y1,0},{x0,y1,0},{x0,y1,height},{x1,y1,height},side-0x00202020);
    quad({x0,y1,0},{x0,y0,0},{x0,y0,height},{x0,y1,height},side-0x00181818);
  };
  box(-300,0,110,260,0xffc8b090,0xff9a7a58);box(250,150,110,180,0xff90a8c8,0xff5878a0);box(0,-350,90,320,0xffc89090,0xffa05858);
  worldVertices=UINT(vertices.size());
  const float l=float(UiLeft),t=float(UiTop),r=float(UiLeft+UiSide),b=float(UiTop+UiSide);
  const std::array<std::array<float,2>,6> ui{{{l,t},{r,t},{r,b},{l,t},{r,b},{l,b}}};
  for(const auto& p:ui)vertices.push_back({p[0]-.5f,p[1]-.5f,.5f,1,UiColor});
  const DWORD fvf=D3DFVF_XYZRHW|D3DFVF_DIFFUSE;
  Check(o.device->CreateVertexBuffer(UINT(vertices.size()*sizeof(Screen)),D3DUSAGE_WRITEONLY,fvf,D3DPOOL_DEFAULT,o.vb.put(),nullptr),"scene VB");
  void* data=nullptr;Check(o.vb->Lock(0,0,&data,0),"scene VB lock");
  std::memcpy(data,vertices.data(),vertices.size()*sizeof(Screen));Check(o.vb->Unlock(),"scene VB upload");
  Check(o.device->CreateOffscreenPlainSurface(Width,Height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,o.readback.put(),nullptr),"scene readback");
  Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(Screen)),"scene stream");Check(o.device->SetFVF(fvf),"scene FVF");
  Check(o.device->SetRenderState(D3DRS_LIGHTING,FALSE),"scene unlit");
  Check(o.device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"scene cull");
  Check(o.device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL),"scene depth function");
  Check(o.device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1),"scene color op");
  Check(o.device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE),"scene color");
  Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1),"scene alpha op");
  Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE),"scene alpha");

  scene::Camera camera;camera.view=view;camera.projection=projection;
  camera.x=0;camera.y=0;camera.width=Width;camera.height=Height;camera.minZ=0;camera.maxZ=1;
  const scene::Ssao ssao{6.0f,1.0f,0.02f,1.5f,0.15f,0.75f};
  scene::Snow snow;snow.snowfall=1;snow.gust=.35f;snow.haze=.4f;snow.lowSnow=.5f;
  snow.minX=snow.minY=-1024;snow.maxX=snow.maxY=1024;snow.bottomZ=0;snow.topZ=1400;snow.edgeFeather=.1f;
  snow.timeSeconds=12.25;snow.windDisplacementX=310.5;snow.windDisplacementY=-140.25;snow.seed=0x5eed1234u;snow.id=7;
  // The settings defaults (d3d9_war3_settings.h): SMAA Ultra; FXAA its fallback.
  const scene::Aa smaa{5,.75f,.166f,.0833f,.05f,32,16};
  scene::Aa fxaa=smaa;fxaa.mode=1;
  // Bloom at a threshold this 8-bit scene reaches after the sRGB decode (the
  // default 1.18 blooms near-white pixels only), then the tone map variant.
  const scene::Bloom bloom{.5f,.5f,.4f,1.0f,0,0};
  const scene::Bloom tone{.8f,.5f,.6f,1.2f,1,1};

  // One frame. The UI quad is drawn after BeforeUi with depth off: no effect
  // may touch it. With no records there is no BeforeUi marker to send.
  auto frame=[&](unsigned effects,double time,bool markers){
    if(markers){
      camera.frame=UINT(frames.size());const auto bytes=scene::EncodeCamera(camera);
      Check(fx.Scene(scene::CameraSchema,bytes.data(),UINT(bytes.size())),"camera record");
      if(effects&Ssao){const auto p=scene::EncodeSsao(ssao);Check(fx.Scene(scene::SsaoSchema,p.data(),UINT(p.size())),"ssao record");}
      if(effects&Snow){auto s=snow;s.timeSeconds=time;const auto p=scene::EncodeSnow(s);
        Check(fx.Scene(scene::SnowSchema,p.data(),UINT(p.size())),"snow record");}
      if(effects&(Fxaa|Smaa)){const auto p=scene::EncodeAa(effects&Smaa?smaa:fxaa);
        Check(fx.Scene(scene::AaSchema,p.data(),UINT(p.size())),"aa record");}
      if(effects&(Bloom|Tone)){const auto p=scene::EncodeBloom(effects&Tone?tone:bloom);
        Check(fx.Scene(scene::BloomSchema,p.data(),UINT(p.size())),"bloom record");}
    }
    Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,ClearColor,1,0),"scene clear");
    Check(o.device->BeginScene(),"scene begin");
    if(markers)Check(fx.Mark(scene::Stage::WorldSceneEnter),"world scene enter");
    Check(o.device->SetRenderState(D3DRS_ZENABLE,TRUE),"world depth");Check(o.device->SetRenderState(D3DRS_ZWRITEENABLE,TRUE),"world depth write");
    // Several draws, so the effects sit between two real runs of draws.
    for(UINT first=0;first<worldVertices;first+=30){
      Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,first,std::min<UINT>(10,(worldVertices-first)/3)),"world draw");++draws;}
    if(markers){Check(fx.Mark(scene::Stage::WorldSceneExit),"world scene exit");Check(fx.Mark(scene::Stage::BeforeUi),"before UI");}
    Check(o.device->SetRenderState(D3DRS_ZENABLE,FALSE),"ui depth");Check(o.device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE),"ui depth write");
    Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,worldVertices,2),"ui draw");++draws;
    Check(o.device->EndScene(),"scene end");
    if(markers)Check(fx.Mark(scene::Stage::FrameEnd),"frame end");
  };
  auto read=[&](const char* name){
    Check(o.device->GetRenderTargetData(o.target.get(),o.readback.get()),"scene readback copy");
    D3DLOCKED_RECT lock{};Check(o.readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"scene readback lock");
    std::vector<uint8_t> image(Width*Height*4);
    for(UINT y=0;y<Height;++y)std::memcpy(image.data()+y*Width*4,static_cast<const uint8_t*>(lock.pBits)+y*lock.Pitch,Width*4);
    Check(o.readback->UnlockRect(),"scene readback unlock");
    if(!name)return;
    if(world.empty())world=image;
    Frame result{name,Hash(image.data(),image.size()),0,0};
    for(UINT y=0;y<Height;++y)for(UINT x=0;x<Width;++x){
      const size_t at=(y*Width+x)*4;DWORD pixel=0;std::memcpy(&pixel,image.data()+at,4);
      const bool ui=x>=UiLeft&&x<UiLeft+UiSide&&y>=UiTop&&y<UiTop+UiSide;
      if(ui)result.uiPixels+=pixel==UiColor;
      else result.changed+=std::memcmp(image.data()+at,world.data()+at,4)!=0;
    }
    Require(result.uiPixels==UiSide*UiSide,"an effect touched the UI drawn after BeforeUi");
    char file[64];std::snprintf(file,sizeof(file),"frame-%s.bgra",name);
    FILE* out=std::fopen(file,"wb");Require(out&&std::fwrite(image.data(),1,image.size(),out)==image.size()&&!std::fclose(out),"frame image file");
    frames.push_back(result);
  };
  auto refused=[&](HRESULT hr,const char* what){Require(hr==D3DERR_INVALIDCALL,what);++refusals;};
  // A marker only the host can judge: it knows whether a world scene is open.
  // With WARVK_D3D9_ASYNC_SCENE=1 the proxy publishes markers without waiting,
  // so such a marker is not refused where it is made; the host then stops
  // reading scene records for the session (test_render_host_ring_faults.cpp,
  // scene-refused). Sending one here would end the effects of every later
  // frame, so in that mode these probes are skipped and counted.
  wchar_t asyncSceneValue[8]{};
  const bool asyncScene=!o.options.reference
    &&GetEnvironmentVariableW(L"WARVK_D3D9_ASYNC_SCENE",asyncSceneValue,8)==1&&asyncSceneValue[0]==L'1';
  auto hostRefused=[&](scene::Stage stage,const char* what){
    if(asyncScene){++refusalsSkipped;return;}
    refused(fx.Mark(stage),what);
  };

  o.stage="scene-effects-world";
  frame(0,0,false);read("world");
  Require(frames[0].changed==0,"baseline differs from itself");
  Probe("world");

  o.stage="scene-effects-refusals";
  // Records no executor reads, and a BeforeUi that is not after a world scene.
  {auto p=scene::EncodeSsao(ssao);refused(fx.Scene(scene::SsaoSchema,p.data(),UINT(p.size())-4),"short ssao record accepted");
    const uint32_t nan=0x7fc00000u;std::memcpy(p.data()+4,&nan,4);refused(fx.Scene(scene::SsaoSchema,p.data(),UINT(p.size())),"NaN ssao record accepted");}
  {auto s=snow;s.id=0;const auto p=scene::EncodeSnow(s);refused(fx.Scene(scene::SnowSchema,p.data(),UINT(p.size())),"snow record without an id accepted");
    // 9 to 11 are the shadow records (war3_shadow_scene.h); 99 is nothing.
    refused(fx.Scene(99,p.data(),UINT(p.size())),"unknown scene schema accepted");}
  {auto a=smaa;a.mode=0;auto p=scene::EncodeAa(a);refused(fx.Scene(scene::AaSchema,p.data(),UINT(p.size())),"aa record without a mode accepted");
    a.mode=6;p=scene::EncodeAa(a);refused(fx.Scene(scene::AaSchema,p.data(),UINT(p.size())),"aa record with an unknown mode accepted");}
  {auto b=bloom;b.acesToneMap=2;const auto p=scene::EncodeBloom(b);
    refused(fx.Scene(scene::BloomSchema,p.data(),UINT(p.size())),"bloom record with a non-boolean flag accepted");}
  {const auto p=scene::EncodeSsao(ssao);Check(fx.Scene(scene::SsaoSchema,p.data(),UINT(p.size())),"ssao record before a scene");
    hostRefused(scene::Stage::BeforeUi,"BeforeUi without a world scene accepted");
    Check(fx.Mark(scene::Stage::WorldSceneEnter),"refusal scene enter");
    hostRefused(scene::Stage::BeforeUi,"BeforeUi inside the world scene accepted");
    Check(fx.Mark(scene::Stage::WorldSceneExit),"refusal scene exit");
    // FrameEnd drops the record: this BeforeUi has nothing to insert and is an ordinary marker.
    Check(fx.Mark(scene::Stage::FrameEnd),"refusal frame end");Check(fx.Mark(scene::Stage::BeforeUi),"BeforeUi without records");}

  o.stage="scene-effects-frames";
  frame(Ssao,0,true);read("ssao");
  // A second BeforeUi in the same world scene: the records are gone after
  // FrameEnd, so send one again and expect the once-per-scene refusal.
  {const auto p=scene::EncodeSsao(ssao);Check(fx.Scene(scene::SsaoSchema,p.data(),UINT(p.size())),"ssao record for the repeat");
    hostRefused(scene::Stage::BeforeUi,"second BeforeUi in one world scene accepted");
    Check(fx.Mark(scene::Stage::FrameEnd),"repeat frame end");}
  frame(Snow,12.25,true);read("snow");
  frame(Ssao|Snow,12.25,true);read("both");
  frame(Ssao|Snow,13.5,true);read("both-later");
  // Records absent: the executors do not run and the world is unchanged.
  frame(0,0,true);read("off");
  Require(frames.back().hash==frames[0].hash&&frames.back().changed==0,"a frame without records differs from the world");
  Require(frames[1].changed&&frames[2].changed&&frames[3].changed&&frames[4].changed,"an effect changed nothing");
  Require(frames[3].hash!=frames[1].hash&&frames[3].hash!=frames[2].hash&&frames[4].hash!=frames[3].hash,"effect frames are not distinct");
  // Phase 4b: anti-aliasing and post-processing, alone and over the others.
  const size_t post=frames.size();
  frame(Fxaa,0,true);read("fxaa");
  frame(Smaa,0,true);read("smaa");
  frame(Bloom,0,true);read("bloom");
  frame(Tone,0,true);read("bloom-tone");
  frame(All,12.25,true);read("all");
  for(size_t i=post;i<frames.size();++i){
    Require(frames[i].changed!=0,"a post-processing effect changed nothing");
    for(size_t j=0;j<i;++j)Require(frames[i].hash!=frames[j].hash,"post-processing frames are not distinct");
  }
  Probe("effects");

  o.stage="scene-effects-timing";
  // Wall time per frame including its readback (the only sync both modes share).
  auto timed=[&](bool effects){
    std::vector<double> us;
    for(UINT i=0;i<TimedFrames;++i){
      const auto begin=std::chrono::steady_clock::now();
      frame(effects?All:0u,20.0+i*.05,true);read(nullptr);
      us.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count());
    }
    std::sort(us.begin(),us.end());return us[us.size()/2];
  };
  worldUs=timed(false);effectsUs=timed(true);
  Probe("timed");

  o.stage="scene-effects-release";
  Check(o.device->SetStreamSource(0,nullptr,0,0),"scene unbind stream");
  if(depthStencil){Check(o.device->SetDepthStencilSurface(nullptr),"D24S8 unbind");combinedDepth.reset();}
  fx.local.reset();o.vb.reset();o.readback.reset();o.back.reset();o.target.reset();
  Require(o.device.detach()->Release()==0,"scene final device Release");
  Require(o.factory.detach()->Release()==0,"scene final factory Release");
  Require(FreeLibrary(o.renderer)!=FALSE,"scene renderer FreeLibrary");o.renderer=nullptr;
  o.window.stop();Probe("closed");
  o.normalTeardown=true;o.stage="complete";
}
}
int wmain(int argc,wchar_t** argv){
  Owner* owner=nullptr;
  try{
    // The shared option parser does not know this fixture's own switch.
    std::vector<wchar_t*> args;
    for(int i=0;i<argc;++i){
      if(i&&std::wcscmp(argv[i],L"--depth-stencil")==0){Require(!depthStencil,"duplicate --depth-stencil");depthStencil=true;}
      else args.push_back(argv[i]);
    }
    owner=new Owner;owner->options=ParseOptions(int(args.size()),args.data());
    Require(!depthStencil||owner->options.reference,"--depth-stencil is an in-process reference variant");
    Require(!owner->options.invalid&&!owner->options.present&&!owner->options.wsi,"scene effects workload is offscreen normal only");
    CreateDevice(*owner);Run(*owner);
    std::printf("{\"schema\":\"warvk-d3d9-scene-effects-fixture/v1\",\"ok\":true,\"reference\":%s,\"depth\":\"%s\",\"draws\":%u,"
      "\"worldVertices\":%u,\"refusals\":%u,\"refusalsSkipped\":%u,\"timedFrames\":%u,\"worldFrameUs\":%.1f,\"effectsFrameUs\":%.1f,"
      "\"x86VulkanAbsent\":%s,\"normalTeardown\":true,\"frames\":[",
      JsonBool(owner->options.reference),depthStencil?"d24s8":"d32f",draws,worldVertices,refusals,refusalsSkipped,TimedFrames,worldUs,effectsUs,
      JsonBool(owner->parentVulkanAbsent&&GetModuleHandleW(L"vulkan-1.dll")==nullptr));
    for(size_t i=0;i<frames.size();++i)
      std::printf("%s{\"name\":\"%s\",\"hash\":\"%016llx\",\"changed\":%u,\"uiPixels\":%u}",i?",":"",frames[i].name,
        static_cast<unsigned long long>(frames[i].hash),frames[i].changed,frames[i].uiPixels);
    std::printf("],\"samples\":[");
    for(size_t i=0;i<samples.size();++i){
      std::printf("%s{\"name\":\"%s\",\"memory\":",i?",":"",samples[i].name);lab::PrintProcessMemory(samples[i].memory);std::printf("}");
    }
    std::printf("],\"memoryMeasured\":true,\"gameTouched\":false}\n");std::fflush(stdout);
    delete owner;return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"scene effects fixture failed stage=%s reason=%s\n",owner?owner->stage:"allocate",e.what());}
  ExitProcess(2);
}
