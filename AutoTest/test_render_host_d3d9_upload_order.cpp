// P1 batch 1 acceptance (docs/research/2026-11-17-x64-p1-staging-slices.md):
// managed-texture updates between the draws of ONE frame are seen in order,
// whether the host uploads with its per-upload settle + submit (default) or
// with staging slices recorded in the stream list (WARVK_D3D9_UPLOAD_SLICES).
// Identical public COM calls through the in-process renderer (--reference) or
// the x64 proxy; an independent per-column pixel oracle in every mode.
//   full     old draw -> full update -> new draw, three times in one frame
//   partial  sub-rectangle updates; the unwritten texels keep their content
//   mip      a level-1 update under SetLOD(1), then level 0 under SetLOD(0)
//   slices   5,000 small updates in one frame (beyond the 4,096-slice credit)
//   bytes    24 updates of a 4 MiB level in one frame (beyond the 16 MiB credit)
//   readonly READONLY locks return the newest bytes
//   reset    Reset to the same mode, then draws and an update after it
// The run ends with every release and the natural host exit.
#define wmain ExistingD3D9FixtureMain
#include "test_render_host_d3d9_parent.cpp"
#undef wmain
#include <array>
#include <vector>
namespace {
constexpr UINT Columns=4,ColumnWidth=Width/Columns;
unsigned images=0,pixelChecks=0,updates=0;uint64_t texelChecks=0,pixelHash=FnvOffset;
struct Screen {float x,y,z,w;DWORD color;float u,v;};
DWORD Color(UINT n){
  return 0xff000000u|(((n*53u+17u)&0xffu)<<16)|(((n*101u+29u)&0xffu)<<8)|((n*197u+71u)&0xffu);
}
// Column c is a full-height quad sampling the constant texel coordinate uv[c].
void Geometry(Owner& o,const std::array<float,Columns>& uv){
  void* data=nullptr;Check(o.vb->Lock(0,0,&data,0),"order geometry lock");
  auto* v=static_cast<Screen*>(data);
  for(UINT c=0;c<Columns;++c){
    const float x0=float(c*ColumnWidth),x1=float((c+1)*ColumnWidth),t=uv[c];
    const Screen q[6]={{x0,0,.5f,1,0xffffffff,t,t},{x1,0,.5f,1,0xffffffff,t,t},{x1,float(Height),.5f,1,0xffffffff,t,t},
      {x0,0,.5f,1,0xffffffff,t,t},{x1,float(Height),.5f,1,0xffffffff,t,t},{x0,float(Height),.5f,1,0xffffffff,t,t}};
    std::memcpy(v+c*6,q,sizeof(q));
  }
  Check(o.vb->Unlock(),"order geometry upload");
}
void Fill(IDirect3DTexture9* texture,UINT level,const RECT* rect,DWORD color){
  D3DSURFACE_DESC d{};Check(texture->GetLevelDesc(level,&d),"order level desc");
  const UINT x0=rect?UINT(rect->left):0,y0=rect?UINT(rect->top):0,x1=rect?UINT(rect->right):d.Width,y1=rect?UINT(rect->bottom):d.Height;
  D3DLOCKED_RECT lock{};Check(texture->LockRect(level,&lock,rect,0),"order update lock");
  for(UINT y=0;y<y1-y0;++y){auto* row=reinterpret_cast<DWORD*>(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch);
    for(UINT x=0;x<x1-x0;++x)row[x]=color;}
  Check(texture->UnlockRect(level),"order update unlock");++updates;
}
void Draw(Owner& o,UINT column){Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,column*6,2),"order column draw");}
void Begin(Owner& o){
  Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xff000000u,1,0),"order clear");
  Check(o.device->BeginScene(),"order begin");
}
void Expect(Owner& o,const char* name,const std::array<DWORD,Columns>& want){
  Check(o.device->EndScene(),"order end");
  Check(o.device->GetRenderTargetData(o.target.get(),o.readback.get()),"order readback");
  D3DLOCKED_RECT lock{};Check(o.readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"order readback lock");
  unsigned failures=0;
  for(UINT y=0;y<Height;++y)for(UINT x=0;x<Width;++x){
    DWORD actual=0;std::memcpy(&actual,static_cast<const uint8_t*>(lock.pBits)+y*lock.Pitch+x*4,4);
    const DWORD expected=want[x/ColumnWidth];++pixelChecks;pixelHash=Hash(&actual,4,pixelHash);
    if(actual!=expected){
      if(failures<4)std::fprintf(stderr,"order image=%s x=%u y=%u actual=%08lx expected=%08lx\n",name,x,y,actual,expected);
      ++failures;
    }
  }
  Check(o.readback->UnlockRect(),"order readback unlock");++images;
  if(failures)throw std::runtime_error(std::string("upload order image mismatch: ")+name);
}
void States(Owner& o){
  const DWORD fvf=D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1;
  Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(Screen)),"order stream");Check(o.device->SetFVF(fvf),"order FVF");
  Check(o.device->SetRenderState(D3DRS_LIGHTING,FALSE),"order unlit");
  Check(o.device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1),"order color select");
  Check(o.device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE),"order color argument");
  Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1),"order alpha select");
  Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE),"order alpha argument");
  Check(o.device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE),"order no UV transform");
}
void Resources(Owner& o){
  const DWORD fvf=D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1;
  Check(o.device->CreateVertexBuffer(Columns*6*sizeof(Screen),D3DUSAGE_WRITEONLY,fvf,D3DPOOL_DEFAULT,o.vb.put(),nullptr),"order VB");
  Check(o.device->CreateOffscreenPlainSurface(Width,Height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,o.readback.put(),nullptr),"order readback");
  States(o);
}
void Texture(Owner& o,Com<IDirect3DTexture9>& t,UINT side,UINT levels){
  Check(o.device->CreateTexture(side,side,levels,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,t.put(),nullptr),"order texture");
  Require(t->GetLevelCount()==levels,"order level count");
}
void Texels(IDirect3DTexture9* t,UINT level,DWORD (*want)(UINT,UINT,void*),void* context,const char* name){
  D3DSURFACE_DESC d{};Check(t->GetLevelDesc(level,&d),"order readonly desc");
  D3DLOCKED_RECT lock{};Check(t->LockRect(level,&lock,nullptr,D3DLOCK_READONLY),"order readonly lock");
  unsigned failures=0;
  for(UINT y=0;y<d.Height;++y)for(UINT x=0;x<d.Width;++x){DWORD value=0;
    std::memcpy(&value,static_cast<const uint8_t*>(lock.pBits)+y*lock.Pitch+x*4,4);++texelChecks;
    if(value!=want(x,y,context)&&failures++<4)std::fprintf(stderr,"order texels=%s x=%u y=%u actual=%08lx\n",name,x,y,value);}
  Check(t->UnlockRect(level),"order readonly unlock");
  if(failures)throw std::runtime_error(std::string("upload order texel mismatch: ")+name);
}
constexpr float Center=.5f,Low=2.5f/16,High=12.5f/16;
void Run(Owner& o){
  o.stage="order-resources";Resources(o);
  // full: old draw -> update -> new draw, three times in one frame.
  o.stage="order-full";
  Com<IDirect3DTexture9> full;Texture(o,full,16,1);Fill(full.get(),0,nullptr,Color(0));
  Geometry(o,{Center,Center,Center,Center});Check(o.device->SetTexture(0,full.get()),"order bind full");
  Begin(o);Draw(o,0);Fill(full.get(),0,nullptr,Color(1));Draw(o,1);Fill(full.get(),0,nullptr,Color(2));Draw(o,2);
  Fill(full.get(),0,nullptr,Color(3));Draw(o,3);Expect(o,"full",{Color(0),Color(1),Color(2),Color(3)});
  // partial: the unwritten texels keep their content between the draws.
  o.stage="order-partial";
  Com<IDirect3DTexture9> partial;Texture(o,partial,16,1);Fill(partial.get(),0,nullptr,Color(10));
  Geometry(o,{Low,Low,High,High});Check(o.device->SetTexture(0,partial.get()),"order bind partial");
  const RECT topLeft{0,0,8,8},bottomRight{8,8,16,16};
  Begin(o);Draw(o,0);Fill(partial.get(),0,&topLeft,Color(11));Draw(o,1);Draw(o,2);
  Fill(partial.get(),0,&bottomRight,Color(12));Draw(o,3);Expect(o,"partial",{Color(10),Color(11),Color(10),Color(12)});
  // mip: level 1 is the base under SetLOD(1); level 0 again under SetLOD(0).
  o.stage="order-mip";
  Com<IDirect3DTexture9> mip;Texture(o,mip,16,2);Fill(mip.get(),0,nullptr,Color(20));Fill(mip.get(),1,nullptr,Color(21));
  Geometry(o,{Center,Center,Center,Center});Check(o.device->SetTexture(0,mip.get()),"order bind mip");
  mip->SetLOD(1);
  Begin(o);Draw(o,0);Fill(mip.get(),1,nullptr,Color(22));Draw(o,1);mip->SetLOD(0);Draw(o,2);
  Fill(mip.get(),0,nullptr,Color(23));Draw(o,3);Expect(o,"mip",{Color(21),Color(22),Color(20),Color(23)});
  // slices: more small updates in one frame than the in-flight slice credit.
  o.stage="order-slices";
  Com<IDirect3DTexture9> small;Texture(o,small,4,1);Fill(small.get(),0,nullptr,Color(100));
  Check(o.device->SetTexture(0,small.get()),"order bind small");
  Begin(o);
  for(UINT i=1;i<=5000;++i){
    Fill(small.get(),0,nullptr,Color(100+i));
    if(i==1200)Draw(o,0);
    if(i==2500)Draw(o,1);
    if(i==3800)Draw(o,2);
    if(i==5000)Draw(o,3);
  }
  Expect(o,"slices",{Color(1300),Color(2600),Color(3900),Color(5100)});
  // bytes: more update bytes in one frame than the in-flight byte credit.
  o.stage="order-bytes";
  Com<IDirect3DTexture9> large;Texture(o,large,1024,1);Fill(large.get(),0,nullptr,Color(200));
  Check(o.device->SetTexture(0,large.get()),"order bind large");
  Begin(o);
  for(UINT i=1;i<=24;++i){Fill(large.get(),0,nullptr,Color(200+i));if(i%6==0)Draw(o,i/6-1);}
  Expect(o,"bytes",{Color(206),Color(212),Color(218),Color(224)});
  // readonly: the newest bytes, also for the partially written levels.
  o.stage="order-readonly";
  Texels(full.get(),0,[](UINT,UINT,void*){return Color(3);},nullptr,"full");
  Texels(partial.get(),0,[](UINT x,UINT y,void*){return x<8&&y<8?Color(11):x>=8&&y>=8?Color(12):Color(10);},nullptr,"partial");
  Texels(mip.get(),1,[](UINT,UINT,void*){return Color(22);},nullptr,"mip1");
  Texels(large.get(),0,[](UINT,UINT,void*){return Color(224);},nullptr,"large");
  // reset: the default pool goes first; managed textures keep their bytes.
  o.stage="order-reset";
  Check(o.device->SetTexture(0,nullptr),"order unbind before Reset");
  Check(o.device->SetStreamSource(0,nullptr,0,0),"order unbind stream before Reset");
  o.vb.reset();o.readback.reset();o.back.reset();o.target.reset();
  D3DPRESENT_PARAMETERS params{};
  params.BackBufferWidth=Width;params.BackBufferHeight=Height;params.BackBufferFormat=D3DFMT_A8R8G8B8;params.BackBufferCount=1;
  params.MultiSampleType=D3DMULTISAMPLE_NONE;params.SwapEffect=D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow=o.window.window.load(std::memory_order_acquire);params.Windowed=TRUE;
  params.EnableAutoDepthStencil=TRUE;params.AutoDepthStencilFormat=D3DFMT_D32F_LOCKABLE;
  params.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
  Check(o.device->Reset(&params),"order device Reset");
  Check(o.device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,o.back.put()),"order back buffer after Reset");
  Check(o.device->GetRenderTarget(0,o.target.put()),"order render target after Reset");
  Resources(o);
  Geometry(o,{Center,Center,Center,Center});Check(o.device->SetTexture(0,full.get()),"order bind after Reset");
  Begin(o);Draw(o,0);Fill(full.get(),0,nullptr,Color(300));Draw(o,1);Check(o.device->SetTexture(0,small.get()),"order bind small after Reset");
  Draw(o,2);Check(o.device->SetTexture(0,large.get()),"order bind large after Reset");Draw(o,3);
  Expect(o,"reset",{Color(3),Color(300),Color(5100),Color(224)});
  // release: everything, then the natural host exit.
  o.stage="order-release";
  Check(o.device->SetTexture(0,nullptr),"order unbind texture");
  Check(o.device->SetStreamSource(0,nullptr,0,0),"order unbind stream");
  full.reset();partial.reset();mip.reset();small.reset();large.reset();
  o.vb.reset();o.readback.reset();o.back.reset();o.target.reset();
  Require(o.device.detach()->Release()==0,"order final device Release");
  Require(o.factory.detach()->Release()==0,"order final factory Release");
  Require(FreeLibrary(o.renderer)!=FALSE,"order renderer FreeLibrary");o.renderer=nullptr;
  o.window.stop();
  o.normalTeardown=true;o.stage="complete";
}
}
int wmain(int argc,wchar_t** argv){
  Owner* owner=nullptr;
  try{
    owner=new Owner;owner->options=ParseOptions(argc,argv);
    Require(!owner->options.invalid&&!owner->options.present&&!owner->options.wsi,"upload order workload is offscreen normal only");
    CreateDevice(*owner);Run(*owner);
    std::printf("{\"schema\":\"warvk-d3d9-upload-order-fixture/v1\",\"ok\":true,\"reference\":%s,\"images\":%u,"
      "\"pixelChecks\":%u,\"texelChecks\":%llu,\"updates\":%u,\"pixelHash\":\"%016llx\",\"x86VulkanAbsent\":%s,"
      "\"normalTeardown\":true,\"gameTouched\":false}\n",JsonBool(owner->options.reference),images,pixelChecks,
      static_cast<unsigned long long>(texelChecks),updates,static_cast<unsigned long long>(pixelHash),
      JsonBool(owner->parentVulkanAbsent&&GetModuleHandleW(L"vulkan-1.dll")==nullptr));
    std::fflush(stdout);
    delete owner;return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"upload order fixture failed stage=%s reason=%s\n",owner?owner->stage:"allocate",e.what());}
  ExitProcess(2);
}
