// One bounded texture/vertex workload with x86 self-process memory samples,
// through either the original in-process renderer (--reference) or the x64
// proxy. Identical public COM calls; independent pixel and texel oracles in
// every mode. The proxy's residency ledger is its own stderr line.
#define wmain ExistingD3D9FixtureMain
#include "test_render_host_d3d9_parent.cpp"
#undef wmain
#include "../tools/render_host/process_memory_probe.h"
#include <array>
#include <vector>
namespace {
namespace lab=warvk::host::lab;
constexpr UINT Textures=12,Extent=1024,Levels=11,PatchTexels=16;
constexpr DWORD ClearColor=0xff101820u,PatchColor=0xff22ee44u;
// Point-sampled solid levels; u=v=.75 is far from every patch texel.
constexpr float Far=.75f,Patch=8.5f/Extent;
unsigned images=0,pixelChecks=0;uint64_t texelChecks=0,pixelHash=FnvOffset;
struct Screen {float x,y,z,w;DWORD color;float u,v;};
struct Sample {const char* name;lab::ProcessMemory memory;};
std::vector<Sample> samples;
DWORD Color(UINT texture,UINT level){
  return 0xff000000u|((40+texture*17)<<16)|((30+level*21)<<8)|(200-texture*9-level*3);
}
void Probe(const char* name){
  const auto memory=lab::ProbeOwnProcessMemory();Require(memory.valid,"own process memory sample");
  samples.push_back({name,memory});
}
// Left and right half quads, each with its own constant texture coordinate.
void Quad(Screen* v,float x0,float x1,float uv){
  const Screen q[6]={{x0,0,.5f,1,0xffffffff,uv,uv},{x1,0,.5f,1,0xffffffff,uv,uv},{x1,64,.5f,1,0xffffffff,uv,uv},
    {x0,0,.5f,1,0xffffffff,uv,uv},{x1,64,.5f,1,0xffffffff,uv,uv},{x0,64,.5f,1,0xffffffff,uv,uv}};
  std::memcpy(v,q,sizeof(q));
}
void Geometry(Owner& o,float left,float right){
  void* data=nullptr;Check(o.vb->Lock(0,0,&data,0),"geometry lock");
  auto* v=static_cast<Screen*>(data);Quad(v,0,32,left);Quad(v+6,32,64,right);Check(o.vb->Unlock(),"geometry upload");
}
template<class Expected> void Image(Owner& o,Expected expected){
  Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,ClearColor,1,0),"image clear");
  Check(o.device->BeginScene(),"image begin");Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,0,4),"image draw");
  Check(o.device->EndScene(),"image end");
  Check(o.device->GetRenderTargetData(o.target.get(),o.readback.get()),"image readback");
  D3DLOCKED_RECT lock{};Check(o.readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"image readback lock");
  unsigned failures=0;
  for(UINT y=0;y<64;++y)for(UINT x=0;x<64;++x){
    DWORD actual=0;std::memcpy(&actual,static_cast<const uint8_t*>(lock.pBits)+y*lock.Pitch+x*4,4);
    const DWORD want=expected(x,y);++pixelChecks;pixelHash=Hash(&actual,4,pixelHash);
    if(actual!=want){
      if(failures<4)std::fprintf(stderr,"backing image=%u x=%u y=%u actual=%08lx expected=%08lx\n",images,x,y,actual,want);
      ++failures;
    }
  }
  Check(o.readback->UnlockRect(),"image readback unlock");++images;Require(!failures,"backing image mismatch");
}
void Run(Owner& o){
  o.stage="backing-geometry";
  const DWORD fvf=D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1;
  Check(o.device->CreateVertexBuffer(12*sizeof(Screen),D3DUSAGE_WRITEONLY,fvf,D3DPOOL_DEFAULT,o.vb.put(),nullptr),"backing VB");
  Check(o.device->CreateOffscreenPlainSurface(64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,o.readback.put(),nullptr),"backing readback");
  Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(Screen)),"backing stream");Check(o.device->SetFVF(fvf),"backing FVF");
  Check(o.device->SetRenderState(D3DRS_LIGHTING,FALSE),"backing unlit");
  Check(o.device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1),"backing color select");
  Check(o.device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE),"backing color argument");
  Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1),"backing alpha select");
  Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE),"backing alpha argument");
  Check(o.device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE),"backing no UV transform");
  Probe("device");
  o.stage="backing-upload";
  std::array<Com<IDirect3DTexture9>,Textures> textures;
  for(UINT t=0;t<Textures;++t){
    Check(o.device->CreateTexture(Extent,Extent,0,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,textures[t].put(),nullptr),"backing texture");
    Require(textures[t]->GetLevelCount()==Levels,"backing mip chain");
    for(UINT level=0;level<Levels;++level){
      const UINT side=Extent>>level;const DWORD color=Color(t,level);
      D3DLOCKED_RECT lock{};Check(textures[t]->LockRect(level,&lock,nullptr,0),"backing level lock");
      Require(lock.Pitch>=int(side*4),"backing level pitch");
      for(UINT y=0;y<side;++y){auto* row=reinterpret_cast<DWORD*>(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch);
        for(UINT x=0;x<side;++x)row[x]=color;}
      Check(textures[t]->UnlockRect(level),"backing level upload");
    }
  }
  Probe("uploaded");
  o.stage="backing-draw";
  Geometry(o,Far,Far);
  for(UINT t=0;t<Textures;++t){
    Check(o.device->SetTexture(0,textures[t].get()),"backing bind");
    Image(o,[&](UINT,UINT){return Color(t,0);});
  }
  Probe("drawn");
  // Whole-level lock, partial write: every unwritten texel must keep its prior
  // content, whether this x86 copy stayed resident or was read back.
  o.stage="backing-texture-relock";
  D3DLOCKED_RECT lock{};Check(textures[0]->LockRect(0,&lock,nullptr,0),"backing whole-level relock");
  for(UINT y=0;y<PatchTexels;++y)for(UINT x=0;x<PatchTexels;++x)
    std::memcpy(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch+x*4,&PatchColor,4);
  Check(textures[0]->UnlockRect(0),"backing partial-write upload");
  Check(o.device->SetTexture(0,textures[0].get()),"backing patched bind");
  Geometry(o,Far,Patch);Image(o,[&](UINT x,UINT){return x<32?Color(0,0):PatchColor;});
  Check(textures[0]->LockRect(0,&lock,nullptr,D3DLOCK_READONLY),"backing readonly relock");
  for(UINT y=0;y<Extent;++y)for(UINT x=0;x<Extent;++x){DWORD value=0;
    std::memcpy(&value,static_cast<const uint8_t*>(lock.pBits)+y*lock.Pitch+x*4,4);++texelChecks;
    Require(value==((x<PatchTexels&&y<PatchTexels)?PatchColor:Color(0,0)),"backing readonly texel");}
  Check(textures[0]->UnlockRect(0),"backing readonly unlock");
  // Whole-buffer lock, partial write: the left quad is never written here.
  o.stage="backing-vertex-relock";
  Geometry(o,Patch,Far);Image(o,[&](UINT x,UINT){return x<32?PatchColor:Color(0,0);});
  void* data=nullptr;Check(o.vb->Lock(0,0,&data,0),"backing whole-buffer relock");
  Quad(static_cast<Screen*>(data)+6,32,64,Patch);Check(o.vb->Unlock(),"backing partial vertex upload");
  Image(o,[&](UINT,UINT){return PatchColor;});
  // Other textures were not touched since their upload.
  for(UINT t=1;t<Textures;t+=5){
    Check(o.device->SetTexture(0,textures[t].get()),"backing late bind");Geometry(o,Far,Far);
    Image(o,[&](UINT,UINT){return Color(t,0);});
  }
  Probe("relocked");
  o.stage="backing-release";
  Check(o.device->SetTexture(0,nullptr),"backing unbind texture");
  Check(o.device->SetStreamSource(0,nullptr,0,0),"backing unbind stream");
  for(auto& texture:textures)texture.reset();
  o.vb.reset();o.readback.reset();
  Probe("released");
  o.back.reset();o.target.reset();
  Require(o.device.detach()->Release()==0,"backing final device Release");
  Require(o.factory.detach()->Release()==0,"backing final factory Release");
  Require(FreeLibrary(o.renderer)!=FALSE,"backing renderer FreeLibrary");o.renderer=nullptr;
  o.window.stop();Probe("closed");
  o.normalTeardown=true;o.stage="complete";
}
}
int wmain(int argc,wchar_t** argv){
  Owner* owner=nullptr;
  try{
    owner=new Owner;owner->options=ParseOptions(argc,argv);
    Require(!owner->options.invalid&&!owner->options.present&&!owner->options.wsi,"backing workload is offscreen normal only");
    CreateDevice(*owner);Run(*owner);
    std::printf("{\"schema\":\"warvk-d3d9-backing-fixture/v1\",\"ok\":true,\"reference\":%s,\"textures\":%u,"
      "\"textureLevels\":%u,\"textureExtent\":%u,\"images\":%u,\"pixelChecks\":%u,\"texelChecks\":%llu,"
      "\"pixelHash\":\"%016llx\",\"x86VulkanAbsent\":%s,\"normalTeardown\":true,\"samples\":[",
      JsonBool(owner->options.reference),Textures,Levels,Extent,images,pixelChecks,
      static_cast<unsigned long long>(texelChecks),static_cast<unsigned long long>(pixelHash),
      JsonBool(owner->parentVulkanAbsent&&GetModuleHandleW(L"vulkan-1.dll")==nullptr));
    for(size_t i=0;i<samples.size();++i){
      std::printf("%s{\"name\":\"%s\",\"memory\":",i?",":"",samples[i].name);lab::PrintProcessMemory(samples[i].memory);std::printf("}");
    }
    std::printf("],\"memoryMeasured\":true,\"gameTouched\":false}\n");std::fflush(stdout);
    delete owner;return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"backing fixture failed stage=%s reason=%s\n",owner?owner->stage:"allocate",e.what());}
  ExitProcess(2);
}
