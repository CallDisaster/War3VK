// Identical public COM calls and independent pixel membership oracle for the
// original x86 renderer and the x64 transport. No memory sampling in this gate.
namespace { struct Owner; void FactoryPrecreateTest(Owner&); }
#define WARVK_FACTORY_PRECREATE_TEST
#define wmain ExistingD3D9FixtureMain
#include "test_render_host_d3d9_parent.cpp"
#undef wmain
#include <array>
#include <climits>
namespace {
unsigned factoryChecks=0,pixelChecks=0,clearCases=0;
unsigned fixedCases=0,vertexInputChecks=0,fixedStateChecks=0;
unsigned textureCases=0;
uint64_t clearHash=FnvOffset;
void FactoryPrecreateTest(Owner& o) {
  D3DCAPS9 caps{};Check(o.factory->GetDeviceCaps(0,D3DDEVTYPE_HAL,&caps),"factory caps before device");
  Require(caps.VertexShaderVersion>=D3DVS_VERSION(3,0)&&caps.PixelShaderVersion>=D3DPS_VERSION(3,0)
    &&caps.MaxTextureWidth>=64&&caps.MaxTextureHeight>=64,"backend cannot implement current profile");++factoryChecks;
  D3DADAPTER_IDENTIFIER9 id{};Check(o.factory->GetAdapterIdentifier(0,0,&id),"physical adapter identifier");
  Require(id.VendorId&&id.Description[0]&&id.Driver[0],"empty physical identifier");++factoryChecks;
  D3DDISPLAYMODE display{};Check(o.factory->GetAdapterDisplayMode(0,&display),"actual display mode");
  Require(display.Width&&display.Height,"empty actual display mode");++factoryChecks;
  const auto count=o.factory->GetAdapterModeCount(0,D3DFMT_X8R8G8B8);Require(count>0&&count<100000,"bounded modes");++factoryChecks;
  D3DDISPLAYMODE mode{};Check(o.factory->EnumAdapterModes(0,D3DFMT_X8R8G8B8,0,&mode),"enumerated mode");
  Require(mode.Width&&mode.Height&&mode.Format==D3DFMT_X8R8G8B8,"bad enumerated mode");++factoryChecks;
  Require(o.factory->EnumAdapterModes(0,D3DFMT_X8R8G8B8,count,&mode)==D3DERR_INVALIDCALL,"out of range mode accepted");++factoryChecks;
  Check(o.factory->CheckDeviceType(0,D3DDEVTYPE_HAL,D3DFMT_X8R8G8B8,D3DFMT_A8R8G8B8,TRUE),"device format");++factoryChecks;
  Check(o.factory->CheckDeviceFormat(0,D3DDEVTYPE_HAL,D3DFMT_X8R8G8B8,0,D3DRTYPE_TEXTURE,D3DFMT_A8R8G8B8),"texture format");++factoryChecks;
  Check(o.factory->CheckDepthStencilMatch(0,D3DDEVTYPE_HAL,D3DFMT_X8R8G8B8,D3DFMT_A8R8G8B8,D3DFMT_D32F_LOCKABLE),"depth format");++factoryChecks;
  DWORD quality=0;Check(o.factory->CheckDeviceMultiSampleType(0,D3DDEVTYPE_HAL,D3DFMT_A8R8G8B8,TRUE,D3DMULTISAMPLE_NONE,&quality),"sample format");
  Require(quality>0,"missing nonmultisample quality");++factoryChecks;
  Require(o.factory->GetAdapterMonitor(0)!=nullptr,"missing local monitor identity");++factoryChecks;
  Require(o.factory->GetDeviceCaps(99,D3DDEVTYPE_HAL,&caps)==D3DERR_INVALIDCALL,"bad adapter accepted");++factoryChecks;
  if(!o.options.reference) {
    Require(caps.MaxTextureWidth==1024&&caps.MaxStreams==1&&caps.MaxTextureBlendStages==8
      &&caps.MaxSimultaneousTextures==8,"capabilities disagree with implemented profile");++factoryChecks;
    Require(o.factory->CheckDeviceFormat(0,D3DDEVTYPE_HAL,D3DFMT_X8R8G8B8,0,D3DRTYPE_CUBETEXTURE,D3DFMT_A8R8G8B8)==D3DERR_NOTAVAILABLE,"unsupported cube advertised");++factoryChecks;
  }
  std::printf("FACTORY_PRECREATE checks=%u vendor=%lu device=%lu modes=%u x86VulkanAbsent=%s\n",
    factoryChecks,id.VendorId,id.DeviceId,count,JsonBool(GetModuleHandleW(L"vulkan-1.dll")==nullptr));
}
void CheckImage(Owner& o,const std::array<uint32_t,4096>& expected,unsigned rgbTolerance=0) {
  if(!o.readback.get())Check(o.device->CreateOffscreenPlainSurface(64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,o.readback.put(),nullptr),"readback create");
  Check(o.device->GetRenderTargetData(o.target.get(),o.readback.get()),"readback clear result");
  D3DLOCKED_RECT lock{};Check(o.readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"readback lock");
  unsigned failures=0;
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
    const auto* pixel=static_cast<const uint8_t*>(lock.pBits)+y*lock.Pitch+x*4;
    for(unsigned c=0;c<4;++c) {
      ++pixelChecks;const auto want=uint8_t(expected[y*64+x]>>(c*8));
      if(unsigned(std::abs(int(pixel[c])-int(want)))>(c==3?0:rgbTolerance)){if(failures<4)std::fprintf(stderr,"clear mismatch case=%u x=%u y=%u channel=%u actual=%u expected=%u\n",clearCases,x,y,c,pixel[c],want);++failures;}
    }
    clearHash=Hash(pixel,4,clearHash);
  }
  Check(o.readback->UnlockRect(),"readback unlock");++clearCases;
  Require(!failures,"clear/depth membership mismatch");
}
void ExerciseClear(Owner& o) {
  const D3DVIEWPORT9 full{0,0,64,64,0,1},part{8,10,40,36,0,1};
  std::array<uint32_t,4096> expected{};
  auto clear=[&](const D3DVIEWPORT9& vp,DWORD count,const D3DRECT* rects,DWORD color) {
    Check(o.device->SetViewport(&vp),"clear viewport");
    Check(o.device->Clear(count,rects,D3DCLEAR_TARGET,color,1,0),"clear regions call");
    // Per-pixel membership, independent of production rectangle clipping.
    for(int y=0;y<64;++y)for(int x=0;x<64;++x) {
      bool hit=x>=int(vp.X)&&y>=int(vp.Y)&&x<int(vp.X+vp.Width)&&y<int(vp.Y+vp.Height);
      if(!count&&rects)hit=false;
      else if(count&&rects) {
        bool inside=false;for(DWORD i=0;i<count;++i)inside|=x>=rects[i].x1&&x<rects[i].x2&&y>=rects[i].y1&&y<rects[i].y2;
        hit&=inside;
      }
      if(hit)expected[y*64+x]=color;
    }
    CheckImage(o,expected);
  };
  clear(full,0,nullptr,0xff101820);clear(part,0,nullptr,0xffff0000);
  const D3DRECT boxes[]={{INT_MIN,INT_MIN,16,18},{30,26,INT_MAX,INT_MAX},{18,20,18,24},{60,60,64,64}};
  clear(part,4,boxes,0xff0000ff);clear(part,0,boxes,0xffff00ff);
  clear(part,3,nullptr,0xff00ff00);
  std::array<D3DRECT,64> many{};
  for(unsigned i=0;i<64;++i)many[i]={LONG(i),LONG(i),LONG(i+1),LONG(i+2)};
  clear(full,64,many.data(),0xffff8800);
  const D3DRECT disjoint[]={{-100,-100,-1,-1},{80,80,90,90},{32,32,16,16}};
  clear(full,3,disjoint,0xffffffff);
  if(!o.options.reference) {
    std::array<D3DRECT,65> tooMany{};
    Require(o.device->Clear(65,tooMany.data(),D3DCLEAR_TARGET,0xffffffff,1,0)==D3DERR_INVALIDCALL,"oversized rectangle packet accepted");
  }
  CheckImage(o,expected);

  // Observe depth-only Clear through a real draw; clearing color cannot fake it.
  Check(o.device->SetViewport(&full),"depth full viewport");
  Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xff000000,0,0),"blocked depth clear");
  const D3DVIEWPORT9 center{16,16,32,32,0,1};Check(o.device->SetViewport(&center),"depth partial viewport");
  Check(o.device->Clear(0,nullptr,D3DCLEAR_ZBUFFER,0xffffffff,1,0),"partial depth clear");
  constexpr char pixel[]="float4 main():COLOR0{return float4(1,0,0,1);}";
  const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(o.compiler,"D3DCompile"));
  Com<ID3DBlob> code,errors;Check(compile(pixel,sizeof(pixel)-1,"clear-depth-oracle",nullptr,nullptr,"main","ps_3_0",0,0,code.put(),errors.put()),"depth oracle shader");
  Check(o.device->SetPixelShader(nullptr),"unbind pixel shader");o.ps.reset();
  Check(o.device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),o.ps.put()),"depth oracle PS");
  Check(o.device->SetPixelShader(o.ps.get()),"bind depth oracle PS");
  Check(o.device->SetViewport(&full),"draw full viewport");
  Check(o.device->SetRenderState(D3DRS_ZENABLE,TRUE),"depth enable");
  Check(o.device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESS),"depth less");
  Check(o.device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"no cull");
  Check(o.device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE),"no blend");
  Check(o.device->BeginScene(),"depth begin");
  Check(o.device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,12,0,2),"depth observable draw");
  Check(o.device->EndScene(),"depth end");
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)expected[y*64+x]=(x>=16&&x<48&&y>=16&&y<48)?0xffff0000:0xff000000;
  CheckImage(o,expected);
}
void ExerciseFixed(Owner& o) {
  o.stage="fixed-function-input";
  const D3DVIEWPORT9 full{0,0,64,64,0,1};Check(o.device->SetViewport(&full),"fixed viewport");
  Check(o.device->SetRenderState(D3DRS_ZENABLE,FALSE),"fixed no z");
  Check(o.device->SetRenderState(D3DRS_LIGHTING,FALSE),"fixed unlit");
  Check(o.device->SetVertexDeclaration(nullptr),"drop explicit declaration");o.declaration.reset();
  Check(o.device->SetStreamSource(0,nullptr,0,0),"drop original stream");o.vb.reset();
  constexpr DWORD fvf=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX1;
  struct Packed {float x,y,z,nx,ny,nz;DWORD color;float u,v;};
  const Packed vertices[]={{-2,-2,.5f,0,0,-1,0xff00ff00,.25f,.25f},{2,-2,.5f,0,0,-1,0xff00ff00,.25f,.25f},
    {2,2,.5f,0,0,-1,0xff00ff00,.25f,.25f},{-2,-2,.5f,0,0,-1,0xff00ff00,.25f,.25f},
    {2,2,.5f,0,0,-1,0xff00ff00,.25f,.25f},{-2,2,.5f,0,0,-1,0xff00ff00,.25f,.25f}};
  Check(o.device->CreateVertexBuffer(sizeof(vertices),D3DUSAGE_WRITEONLY,fvf,D3DPOOL_DEFAULT,o.vb.put(),nullptr),"FVF vertex buffer");
  D3DVERTEXBUFFER_DESC desc{};Check(o.vb->GetDesc(&desc),"FVF buffer description");Require(desc.FVF==fvf,"buffer FVF lost");++vertexInputChecks;
  void* storage=nullptr;Check(o.vb->Lock(0,0,&storage,0),"FVF lock");std::memcpy(storage,vertices,sizeof(vertices));Check(o.vb->Unlock(),"FVF upload");
  Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(Packed)),"compact stream");
  Check(o.device->SetFVF(fvf),"SetFVF real declaration");
  DWORD actual=0;Check(o.device->GetFVF(&actual),"GetFVF");Require(actual==fvf,"FVF identity");++vertexInputChecks;
  Check(o.device->GetVertexDeclaration(o.declaration.put()),"GetVertexDeclaration FVF");
  std::array<D3DVERTEXELEMENT9,17> elements{};UINT count=UINT(elements.size());
  Check(o.declaration->GetDeclaration(elements.data(),&count),"FVF declaration roundtrip");
  Require(count==5&&elements[0].Type==D3DDECLTYPE_FLOAT3&&elements[1].Offset==12
    &&elements[2].Type==D3DDECLTYPE_D3DCOLOR&&elements[2].Offset==24&&elements[3].Offset==28
    &&elements[4].Stream==0xff,"FVF offsets/types");++vertexInputChecks;
  Check(o.device->SetFVF(0),"FVF zero no-op");Check(o.device->GetFVF(&actual),"FVF after zero");
  Require(actual==fvf,"zero FVF changed binding");++vertexInputChecks;
  auto image=[&](DWORD color,bool center=false) {
    Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff101820,1,0),"fixed clear");
    Check(o.device->BeginScene(),"fixed begin");Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,0,2),"fixed draw");Check(o.device->EndScene(),"fixed end");
    std::array<uint32_t,4096> expected{};
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
      expected[y*64+x]=!center||(x>=16&&x<48&&y>=16&&y<48)?color:0xff101820;
    CheckImage(o,expected);--clearCases;++fixedCases;
  };
  image(0xffff0000); // Original SM3 with compact FVF/normalized D3DCOLOR.
  Check(o.device->SetVertexShader(nullptr),"fixed VS selected");image(0xffff0000); // Mixed FF VS and original PS.
  Check(o.device->SetPixelShader(nullptr),"fixed PS selected");
  Check(o.device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1),"select diffuse");
  Check(o.device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE),"diffuse source");
  Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1),"alpha op");
  Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE),"alpha source");
  image(0xff00ff00);
  Check(o.device->SetTexture(0,nullptr),"untextured fixed");
  image(0xff00ff00);
  D3DMATRIX identity{};for(unsigned i=0;i<4;++i)identity.m[i][i]=1;
  auto scaled=identity;scaled._11=scaled._22=.25f;
  Check(o.device->SetTransform(D3DTS_WORLD,&scaled),"world scale");image(0xff00ff00,true);
  D3DMATRIX got{};Check(o.device->GetTransform(D3DTS_WORLD,&got),"world read");Require(!std::memcmp(&got,&scaled,sizeof(got)),"world roundtrip");++fixedStateChecks;
  auto translated=identity;translated._41=2;Check(o.device->MultiplyTransform(D3DTS_WORLD,&translated),"world multiply");
  Check(o.device->GetTransform(D3DTS_WORLD,&got),"multiplied read");Require(got._41==.5f&&got._11==.25f,"native multiply order");++fixedStateChecks;
  Check(o.device->SetTransform(D3DTS_WORLD,&identity),"world identity");
  // Native WarVK intentionally forces Material.Ambient to white. Use that
  // explicit value on BOTH sides; its game-light override is a separate gate.
  D3DMATERIAL9 material{};material.Diffuse={0,0,1,1};material.Ambient={1,1,1,1};
  Check(o.device->SetMaterial(&material),"material set");D3DMATERIAL9 materialCopy{};
  Check(o.device->GetMaterial(&materialCopy),"material get");Require(!std::memcmp(&material,&materialCopy,sizeof(material)),"material roundtrip");++fixedStateChecks;
  Check(o.device->SetRenderState(D3DRS_LIGHTING,TRUE),"lighting on");
  Check(o.device->SetRenderState(D3DRS_COLORVERTEX,FALSE),"material sources");
  Check(o.device->SetRenderState(D3DRS_AMBIENT,0xffff0000),"ambient red");
  D3DLIGHT9 light{};light.Type=D3DLIGHT_DIRECTIONAL;light.Diffuse={1,1,1,1};light.Direction={0,0,1};
  Check(o.device->SetLight(7,&light),"light 7 set");Check(o.device->LightEnable(7,TRUE),"light 7 enabled");
  D3DLIGHT9 lightCopy{};Check(o.device->GetLight(7,&lightCopy),"light read");Require(!std::memcmp(&light,&lightCopy,sizeof(light)),"light roundtrip");++fixedStateChecks;
  BOOL enabled=FALSE;Check(o.device->GetLightEnable(7,&enabled),"enabled read");Require(enabled!=FALSE,"light enable lost");++fixedStateChecks;
  image(0xffff00ff);
  Check(o.device->LightEnable(7,FALSE),"disable light");image(0xffff0000);
  Check(o.device->SetRenderState(D3DRS_LIGHTING,FALSE),"lighting off");
  Check(o.device->SetTexture(0,o.texture.get()),"texture restore");
  Check(o.device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE),"sample source");image(0xffff0000);
  auto textureMatrix=identity;textureMatrix._22=3;
  Check(o.device->SetTransform(D3DTS_TEXTURE0,&textureMatrix),"texture scale");
  Check(o.device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_COUNT2),"texture transform on");image(0xff0000ff);
  Check(o.device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE),"restore diffuse");
  Check(o.device->SetVertexShader(o.vs.get()),"mixed programmable VS");image(0xff00ff00);
  Check(o.device->SetVertexShader(nullptr),"fixed VS restore");
  // Transformed positions must ignore matrices and lighting, as the native FF
  // path does. A screen-space quad is observed over every output pixel.
  Check(o.device->SetStreamSource(0,nullptr,0,0),"drop packed stream");o.vb.reset();
  struct Screen {float x,y,z,w;DWORD color;};
  const Screen screen[]={{0,0,.5f,1,0xff00ff00},{64,0,.5f,1,0xff00ff00},{64,64,.5f,1,0xff00ff00},
    {0,0,.5f,1,0xff00ff00},{64,64,.5f,1,0xff00ff00},{0,64,.5f,1,0xff00ff00}};
  Check(o.device->CreateVertexBuffer(sizeof(screen),D3DUSAGE_WRITEONLY,D3DFVF_XYZRHW|D3DFVF_DIFFUSE,D3DPOOL_DEFAULT,o.vb.put(),nullptr),"screen VB");
  Check(o.vb->Lock(0,0,&storage,0),"screen lock");std::memcpy(storage,screen,sizeof(screen));Check(o.vb->Unlock(),"screen upload");
  Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(Screen)),"screen stream");
  Check(o.device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE),"screen FVF");
  Check(o.device->SetRenderState(D3DRS_LIGHTING,TRUE),"screen ignores lighting");
  Check(o.device->SetTransform(D3DTS_WORLD,&scaled),"screen ignores world");image(0xff00ff00);
}
// Game.dll's own census used line strips, triangle lists and strips with up to
// 5,896 primitives, and cull/blend/depth values beyond the earlier profile.
// Filled shapes have independent membership oracles; line rasterization has
// none here, so line images only join the shared hash (local == remote) after
// a nonvacuous coverage check. Every state touched is restored afterwards.
unsigned topologyCases=0,lineImages=0;
void ExerciseTopologies(Owner& o) {
  o.stage="primitive-topologies";
  struct Screen {float x,y,z,w;DWORD color;};
  constexpr DWORD fvf=D3DFVF_XYZRHW|D3DFVF_DIFFUSE,clear=0xff101820;
  constexpr UINT strip=5000,stripVertices=strip+2;
  constexpr std::array<D3DRENDERSTATETYPE,10> touched{D3DRS_LIGHTING,D3DRS_CULLMODE,D3DRS_ZENABLE,D3DRS_ZFUNC,D3DRS_ZWRITEENABLE,
    D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_DITHERENABLE};
  std::array<DWORD,touched.size()> saved{};
  for(size_t i=0;i<touched.size();++i)Check(o.device->GetRenderState(touched[i],&saved[i]),"topology save state");
  DWORD savedTransform=0;Check(o.device->GetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,&savedTransform),"topology save transform flags");
  Check(o.device->SetRenderState(D3DRS_LIGHTING,FALSE),"topology unlit");
  Check(o.device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"topology no cull");
  Check(o.device->SetRenderState(D3DRS_ZENABLE,FALSE),"topology no depth");
  Check(o.device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE),"topology no blend");
  Check(o.device->SetTexture(0,nullptr),"topology untextured");
  Check(o.device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE),"topology no texture transform");
  // [0,2s): one full-target strip twice, red then blue; the blue copy is reached
  // only through BaseVertexIndex. Then a fan, the cull pair and the line shapes.
  std::vector<Screen> all;
  for(UINT copy=0;copy<2;++copy)for(UINT i=0;i<stripVertices;++i)
    all.push_back({64.f*float(i/2)/float(strip/2),i%2?64.f:0.f,.5f,1,copy?0xff0000ffu:0xffff0000u});
  const UINT fanStart=UINT(all.size());
  all.insert(all.end(),{{16,16,.5f,1,0xff00ff00},{48,16,.5f,1,0xff00ff00},{48,48,.5f,1,0xff00ff00},{16,48,.5f,1,0xff00ff00}});
  // Left rectangle wound clockwise on screen (y down), right counterclockwise.
  const UINT cullStart=UINT(all.size());
  all.insert(all.end(),{{0,0,.5f,1,0xffffff00},{32,0,.5f,1,0xffffff00},{32,64,.5f,1,0xffffff00},
    {0,0,.5f,1,0xffffff00},{32,64,.5f,1,0xffffff00},{0,64,.5f,1,0xffffff00},
    {32,0,.5f,1,0xffff00ff},{32,64,.5f,1,0xffff00ff},{64,64,.5f,1,0xffff00ff},
    {32,0,.5f,1,0xffff00ff},{64,64,.5f,1,0xffff00ff},{64,0,.5f,1,0xffff00ff}});
  // D3D9 pixel centers are integral; quarter offsets keep lines off ties.
  const UINT lineStart=UINT(all.size());
  all.insert(all.end(),{{.25f,10.25f,.5f,1,0xffffffff},{63.75f,10.25f,.5f,1,0xffffffff},{20.25f,.25f,.5f,1,0xff00ffff},{20.25f,63.75f,.5f,1,0xff00ffff},
    {2.25f,60.25f,.5f,1,0xffff8000},{30.25f,33.75f,.5f,1,0xffff8000},{61.75f,40.25f,.5f,1,0xffff8000},{40.25f,3.75f,.5f,1,0xffff8000}});
  Com<IDirect3DVertexBuffer9> vb;Com<IDirect3DIndexBuffer9> ib;
  auto upload=[&](UINT count){void* data=nullptr;Check(vb->Lock(0,count*sizeof(Screen),&data,0),"topology VB lock");
    std::memcpy(data,all.data(),count*sizeof(Screen));Check(vb->Unlock(),"topology VB upload");};
  Check(o.device->CreateVertexBuffer(UINT(all.size()*sizeof(Screen)),D3DUSAGE_WRITEONLY,fvf,D3DPOOL_DEFAULT,vb.put(),nullptr),"topology VB");
  upload(UINT(all.size()));
  Check(o.device->CreateIndexBuffer(stripVertices*2,D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_DEFAULT,ib.put(),nullptr),"topology IB");
  void* data=nullptr;Check(ib->Lock(0,0,&data,0),"topology IB lock");
  for(UINT i=0;i<stripVertices;++i){const uint16_t index=uint16_t(i);std::memcpy(static_cast<uint8_t*>(data)+i*2,&index,2);}
  Check(ib->Unlock(),"topology IB upload");
  Check(o.device->SetStreamSource(0,vb.get(),0,sizeof(Screen)),"topology stream");Check(o.device->SetFVF(fvf),"topology FVF");
  Check(o.device->SetIndices(ib.get()),"topology indices");
  auto rect=[](unsigned x0,unsigned y0,unsigned x1,unsigned y1,DWORD color){std::array<uint32_t,4096> e{};
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)e[y*64+x]=(x>=x0&&x<x1&&y>=y0&&y<y1)?color:clear;return e;};
  auto image=[&](DWORD background,DWORD flags,const std::array<uint32_t,4096>& expected,unsigned tolerance,auto draw){
    Check(o.device->Clear(0,nullptr,flags,background,.5f,0),"topology clear");
    Check(o.device->BeginScene(),"topology begin");draw();Check(o.device->EndScene(),"topology end");
    CheckImage(o,expected,tolerance);--clearCases;++topologyCases;
  };
  auto red=[&]{Check(o.device->DrawPrimitive(D3DPT_TRIANGLESTRIP,0,strip),"5000-primitive strip");};
  auto blue=[&]{Check(o.device->DrawIndexedPrimitive(D3DPT_TRIANGLESTRIP,INT(stripVertices),0,stripVertices,0,strip),"indexed strip at base vertex");};
  auto fill=[](DWORD color){std::array<uint32_t,4096> e;e.fill(color);return e;};
  image(clear,D3DCLEAR_TARGET,fill(0xffff0000),0,red);
  image(clear,D3DCLEAR_TARGET,fill(0xff0000ff),0,blue);
  image(clear,D3DCLEAR_TARGET,rect(16,16,48,48,0xff00ff00),0,[&]{Check(o.device->DrawPrimitive(D3DPT_TRIANGLEFAN,fanStart,2),"fan");});
  auto both=[&]{Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,cullStart,2),"clockwise rectangle");
    Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,cullStart+6,2),"counterclockwise rectangle");};
  Check(o.device->SetRenderState(D3DRS_CULLMODE,D3DCULL_CW),"cull clockwise");image(clear,D3DCLEAR_TARGET,rect(32,0,64,64,0xffff00ff),0,both);
  Check(o.device->SetRenderState(D3DRS_CULLMODE,D3DCULL_CCW),"cull counterclockwise");image(clear,D3DCLEAR_TARGET,rect(0,0,32,64,0xffffff00),0,both);
  Check(o.device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"strip winding alternates");
  // Blend factors and ops beyond the earlier ZERO/ONE/SRCALPHA/INVSRCALPHA, ADD.
  Check(o.device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE),"blend on");
  Check(o.device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_DESTCOLOR),"modulate source");
  Check(o.device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ZERO),"modulate destination");
  image(0xff808080,D3DCLEAR_TARGET,fill(0xff800000),1,red);  // red * 0x80 grey; alpha ff * ff
  Check(o.device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE),"max source");Check(o.device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ONE),"max destination");
  Check(o.device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_MAX),"blend max");
  image(0xff404040,D3DCLEAR_TARGET,fill(0xffff4040),0,red);
  Check(o.device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD),"blend add");Check(o.device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE),"blend off");
  // GREATER against a .5 clear without depth writes: the far red copy (.75)
  // passes, the near blue one (.25) drawn after it fails. LESS, ALWAYS or an
  // ignored test would all leave blue.
  for(UINT i=0;i<2*stripVertices;++i)all[i].z=i<stripVertices?.75f:.25f;
  upload(2*stripVertices);
  Check(o.device->SetRenderState(D3DRS_ZENABLE,TRUE),"depth on");Check(o.device->SetRenderState(D3DRS_ZFUNC,D3DCMP_GREATER),"depth greater");
  Check(o.device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE),"depth read only");
  image(clear,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,fill(0xffff0000),0,[&]{red();blue();});
  Check(o.device->SetRenderState(D3DRS_ZENABLE,FALSE),"depth off");
  // Dither has no Vulkan effect in DXVK; it must still round-trip.
  DWORD dither=FALSE;Check(o.device->SetRenderState(D3DRS_DITHERENABLE,TRUE),"dither on");
  Check(o.device->GetRenderState(D3DRS_DITHERENABLE,&dither),"dither read");Require(dither==TRUE,"dither lost");++fixedStateChecks;
  // Lines: hash-only images, each with real but partial coverage.
  auto lineImage=[&](D3DPRIMITIVETYPE type,UINT start,UINT count){
    Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET,clear,1,0),"line clear");
    Check(o.device->BeginScene(),"line begin");Check(o.device->DrawPrimitive(type,start,count),"line draw");Check(o.device->EndScene(),"line end");
    if(!o.readback.get())Check(o.device->CreateOffscreenPlainSurface(64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,o.readback.put(),nullptr),"line readback create");
    Check(o.device->GetRenderTargetData(o.target.get(),o.readback.get()),"line readback");
    D3DLOCKED_RECT lock{};Check(o.readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"line lock");
    unsigned covered=0;
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){const auto* pixel=static_cast<const uint8_t*>(lock.pBits)+y*lock.Pitch+x*4;
      DWORD value=0;std::memcpy(&value,pixel,4);covered+=value!=clear;clearHash=Hash(pixel,4,clearHash);}
    Check(o.readback->UnlockRect(),"line unlock");
    std::printf("LINE_IMAGE type=%u covered=%u\n",unsigned(type),covered);
    Require(covered>=60&&covered<=400,"line image coverage outside one-pixel lines");++lineImages;
  };
  lineImage(D3DPT_LINELIST,lineStart,2);
  lineImage(D3DPT_LINESTRIP,lineStart+4,3);
  for(size_t i=0;i<touched.size();++i)Check(o.device->SetRenderState(touched[i],saved[i]),"topology restore state");
  Check(o.device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,savedTransform),"topology restore transform flags");
  Check(o.device->SetTexture(0,o.texture.get()),"topology restore texture");
  Check(o.device->SetIndices(o.ib.get()),"topology restore indices");
  Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(Screen)),"topology restore stream");
  Check(o.device->SetFVF(fvf),"topology restore FVF");
}
void ExerciseTextureLevels(Owner& o) {
  o.stage="texture-subresources";
  Check(o.device->SetTexture(0,nullptr),"drop old texture");o.texture.reset();
  Check(o.device->CreateTexture(257,129,0,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,o.texture.put(),nullptr),"NPOT mip chain");
  const UINT levels=o.texture->GetLevelCount();Require(levels==9,"complete NPOT chain");
  std::array<DWORD,9> colors{};
  for(UINT level=0;level<levels;++level) {
    colors[level]=0xff000000|((20+level*21)<<16)|((40+level*17)<<8)|(60+level*13);
    D3DSURFACE_DESC desc{};Check(o.texture->GetLevelDesc(level,&desc),"mip description");
    Require(desc.Width==std::max(1u,257u>>level)&&desc.Height==std::max(1u,129u>>level),"mip extent");
    D3DLOCKED_RECT lock{};Check(o.texture->LockRect(level,&lock,nullptr,0),"mip lock");
    Require(lock.Pitch>=int(desc.Width*4),"mip pitch");
    for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x)
      std::memcpy(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch+x*4,&colors[level],4);
    Check(o.texture->UnlockRect(level),"mip chunk transaction");
  }
  Check(o.device->SetStreamSource(0,nullptr,0,0),"drop screen stream");o.vb.reset();
  struct Screen {float x,y,z,w;DWORD color;float u,v;};
  Screen vertices[]={{0,0,.5f,1,0xffffffff,.25f,.25f},{64,0,.5f,1,0xffffffff,.25f,.25f},{64,64,.5f,1,0xffffffff,.25f,.25f},
    {0,0,.5f,1,0xffffffff,.25f,.25f},{64,64,.5f,1,0xffffffff,.25f,.25f},{0,64,.5f,1,0xffffffff,.25f,.25f}};
  const DWORD fvf=D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1;
  Check(o.device->CreateVertexBuffer(sizeof(vertices),D3DUSAGE_WRITEONLY,fvf,D3DPOOL_DEFAULT,o.vb.put(),nullptr),"mip screen VB");
  auto upload=[&](){void* data=nullptr;Check(o.vb->Lock(0,0,&data,0),"mip VB lock");std::memcpy(data,vertices,sizeof(vertices));Check(o.vb->Unlock(),"mip VB upload");};
  upload();Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(Screen)),"mip stream");Check(o.device->SetFVF(fvf),"mip FVF");
  Check(o.device->SetRenderState(D3DRS_LIGHTING,FALSE),"mip unlit");Check(o.device->SetTexture(0,o.texture.get()),"mip bind");
  Check(o.device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1),"mip texture select");
  Check(o.device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE),"mip texture argument");
  Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1),"mip alpha select");
  Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE),"mip alpha argument");
  Check(o.device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE),"mip no UV transform");
  auto image=[&](DWORD expected){
    Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff101820,1,0),"mip clear");
    Check(o.device->BeginScene(),"mip begin");Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,0,2),"mip draw");Check(o.device->EndScene(),"mip end");
    std::array<uint32_t,4096> pixels;pixels.fill(expected);CheckImage(o,pixels);--clearCases;++textureCases;
  };
  for(UINT level=0;level<levels;++level) {
    const DWORD prior=o.texture->GetLOD();Require(o.texture->SetLOD(level)==prior&&o.texture->GetLOD()==level,"LOD state roundtrip");image(colors[level]);
  }
  Require(o.texture->SetLOD(999)==8&&o.texture->GetLOD()==8,"LOD clamps to final level");
  o.texture->SetLOD(3);
  const RECT patch{0,0,16,8};const DWORD changed=0xff33ee77;
  D3DLOCKED_RECT lock{};Check(o.texture->LockRect(3,&lock,&patch,0),"partial mip lock");
  for(UINT y=0;y<8;++y)for(UINT x=0;x<16;++x)std::memcpy(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch+x*4,&changed,4);
  Check(o.texture->UnlockRect(3),"partial mip commit");image(changed);
  for(auto& vertex:vertices)vertex.u=vertex.v=.75f;upload();image(colors[3]);
  Check(o.texture->LockRect(3,&lock,nullptr,D3DLOCK_READONLY),"mip readonly lock");
  for(UINT y=0;y<16;++y)for(UINT x=0;x<32;++x){DWORD value=0;std::memcpy(&value,static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch+x*4,4);
    Require(value==((x<16&&y<8)?changed:colors[3]),"partial write changed unrelated managed texels");}
  Check(o.texture->UnlockRect(3),"mip readonly unlock");
}
// Data-layer annotations through the public exports, remote only. Nothing is
// read back, so the hashed image sequence is unchanged; the host's recorder
// digest must equal the one folded here from the same calls. Wire values are
// written out here rather than taken from the production header.
bool annotationsRun=false;
unsigned annotationDraws=0,annotatedDraws=0,annotationScenes=0,annotationStages=0,annotationRefusals=0;
uint64_t annotationDigest=FnvOffset;
void ExerciseAnnotations(Owner& o) {
  if(o.options.reference)return;
  o.stage="data-layer-annotations";
  using RecordFn=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT,const void*,UINT);
  using StageFn=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT);
  const auto annotate=reinterpret_cast<RecordFn>(GetProcAddress(o.renderer,"WarVKD3D9AnnotateNextDraw"));
  const auto scene=reinterpret_cast<RecordFn>(GetProcAddress(o.renderer,"WarVKD3D9SetFrameScene"));
  const auto mark=reinterpret_cast<StageFn>(GetProcAddress(o.renderer,"WarVKD3D9MarkStage"));
  Require(annotate&&scene&&mark,"annotation exports");
  constexpr UINT DrawTag=1,FrameCamera=2,WorldBegin=1,BeforeUi=2,FrameEnd=4;
  auto refused=[&](HRESULT hr,const char* what){Require(hr==D3DERR_INVALIDCALL,what);++annotationRefusals;};
  std::array<uint32_t,40> camera{}; // view, projection, viewport, depth range, frame, game time
  auto bits=[](float v){uint32_t u;std::memcpy(&u,&v,4);return u;};
  for(unsigned i=0;i<4;++i)camera[i*5]=camera[16+i*5]=bits(1);
  camera[34]=camera[35]=bits(64);camera[37]=bits(1);camera[38]=42;camera[39]=bits(3.5f);
  std::array<uint8_t,16> tag{};
  wchar_t mode[4]{};
  annotationsRun=GetEnvironmentVariableW(L"WARVK_TEST_ANNOTATE",mode,4)==1&&mode[0]==L'1';
  if(!annotationsRun) {
    // Not negotiated: every export refuses locally and nothing is sent.
    refused(annotate(o.device.get(),DrawTag,tag.data(),16),"annotation without negotiation");
    refused(scene(o.device.get(),FrameCamera,camera.data(),sizeof(camera)),"scene without negotiation");
    refused(mark(o.device.get(),WorldBegin),"stage without negotiation");
    return;
  }
  auto word=[&](uint32_t v){annotationDigest=Hash(&v,4,annotationDigest);};
  auto stage=[&](UINT value){Check(mark(o.device.get(),value),"stage marker");word(3);word(value);++annotationStages;};
  auto tagFor=[&](unsigned i){const uint64_t object=0x51000+i;const uint32_t kind=i%3,flags=1u<<(i%32);
    std::memcpy(tag.data(),&object,8);std::memcpy(tag.data()+8,&kind,4);std::memcpy(tag.data()+12,&flags,4);};
  // Local refusals publish nothing.
  refused(scene(o.device.get(),DrawTag,camera.data(),sizeof(camera)),"scene schema accepted");
  refused(scene(o.device.get(),FrameCamera,camera.data(),sizeof(camera)-4),"scene size accepted");
  {auto nan=camera;nan[3]=0x7fc00000u;refused(scene(o.device.get(),FrameCamera,nan.data(),sizeof(nan)),"NaN scene accepted");}
  refused(mark(o.device.get(),0),"stage 0 accepted");refused(mark(o.device.get(),FrameEnd+1),"stage past FrameEnd accepted");
  refused(annotate(o.device.get(),FrameCamera,tag.data(),16),"draw record schema accepted");
  refused(annotate(o.device.get(),DrawTag,tag.data(),12),"draw record size accepted");
  refused(annotate(o.device.get(),DrawTag,nullptr,16),"draw record without data accepted");
  Check(scene(o.device.get(),FrameCamera,camera.data(),sizeof(camera)),"frame scene");
  word(2);word(FrameCamera);word(sizeof(camera));annotationDigest=Hash(camera.data(),sizeof(camera),annotationDigest);++annotationScenes;
  struct Screen {float x,y,z,w;DWORD color;float u,v;};
  std::array<Screen,24> vertices{};
  for(unsigned i=0;i<vertices.size();++i)vertices[i]={i%3==1?64.f:0.f,i%3==2?64.f:0.f,.5f,1,0xff2060a0,.25f,.25f};
  Com<IDirect3DVertexBuffer9> vb;
  Check(o.device->CreateVertexBuffer(sizeof(vertices),D3DUSAGE_WRITEONLY,D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1,D3DPOOL_DEFAULT,vb.put(),nullptr),"annotation VB");
  void* data=nullptr;Check(vb->Lock(0,0,&data,0),"annotation VB lock");std::memcpy(data,vertices.data(),sizeof(vertices));Check(vb->Unlock(),"annotation VB upload");
  Check(o.device->SetStreamSource(0,vb.get(),0,sizeof(Screen)),"annotation stream");
  Check(o.device->BeginScene(),"annotation begin");stage(WorldBegin);
  // A refused draw drops its records rather than lending them to the next.
  tagFor(99);Check(annotate(o.device.get(),DrawTag,tag.data(),16),"record for a refused draw");
  Require(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,0,0)==D3DERR_INVALIDCALL,"empty draw accepted");
  // Distinct (start, primitives) per draw make a misplaced record visible.
  // Draw 5 carries none; BeforeUi splits the draws into two batches.
  constexpr std::array<std::array<UINT,2>,8> shapes{{{0,1},{3,2},{9,1},{0,3},{12,4},{6,1},{15,3},{21,1}}};
  for(unsigned i=0;i<shapes.size();++i) {
    if(i==4)stage(BeforeUi);
    const UINT start=shapes[i][0],primitives=shapes[i][1];
    if(i!=5) {
      tagFor(i);Check(annotate(o.device.get(),DrawTag,tag.data(),16),"annotate next draw");
      if(!i)refused(annotate(o.device.get(),DrawTag,tag.data(),16),"repeated record schema accepted");
      word(1);word(DrawTag);word(16);annotationDigest=Hash(tag.data(),16,annotationDigest);
      word(D3DPT_TRIANGLELIST);word(primitives);word(primitives*3);word(start);++annotatedDraws;
    }
    Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,start,primitives),"annotated draw");++annotationDraws;
  }
  Check(o.device->EndScene(),"annotation end");stage(FrameEnd);
  Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(Screen)),"restore texture-level stream");
}
}
#include "test_render_host_texture_formats.h"
#include "test_render_host_sampler_gpu.h"
#include "test_render_host_effects_gpu.h"
#include "test_render_host_texture_stages_gpu.h"
#include "test_render_host_stateblock_gpu.h"
int wmain(int argc,wchar_t** argv) {
  Owner* owner=nullptr;
  try {
    owner=new Owner;owner->options=ParseOptions(argc,argv);
    wchar_t mode[4]{};
    if(GetEnvironmentVariableW(L"WARVK_TEST_FACTORY_ONLY",mode,4)==1&&mode[0]==L'1') {
      owner->renderer=LoadLibraryW(owner->options.dll.c_str());Require(owner->renderer,"factory-only renderer load");
      const auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(owner->renderer,"Direct3DCreate9"));Require(create,"factory entry");
      *owner->factory.put()=create(D3D_SDK_VERSION);Require(owner->factory.get(),"factory created");
      Require(owner->factory->GetAdapterCount()>0,"adapter available");FactoryPrecreateTest(*owner);
      Require(owner->factory.detach()->Release()==0,"factory-only final reference");
      Require(FreeLibrary(owner->renderer)!=FALSE,"factory-only library release");owner->renderer=nullptr;
      std::printf("{\"schema\":\"warvk-d3d9-factory-only/v1\",\"ok\":true,\"factoryChecks\":%u,\"normalTeardown\":true,\"memoryMeasured\":false}\n",factoryChecks);
      delete owner;return 0;
    }
    CreateDevice(*owner);
    CreateGeometry(*owner);CreateShadersAndTexture(*owner);
    ExerciseClear(*owner);ExerciseFixed(*owner);ExerciseTopologies(*owner);ExerciseTextureLevels(*owner);ExerciseAnnotations(*owner);ExerciseTextureFormats(*owner);ExerciseSampler(*owner);ExerciseEffects(*owner);ExerciseTextureStages(*owner);ExerciseStateBlocks(*owner);Teardown(*owner);
    std::printf("{\"schema\":\"warvk-d3d9-factory-clear/v1\",\"ok\":true,\"factoryChecks\":%u,\"clearCases\":%u,\"fixedCases\":%u,\"vertexInputChecks\":%u,\"fixedStateChecks\":%u,\"topologyCases\":%u,\"lineImages\":%u,\"textureCases\":%u,\"pixelChecks\":%u,\"pixelHash\":\"%016llx\",\"normalTeardown\":true,\"memoryMeasured\":false}\n",factoryChecks,clearCases,fixedCases,vertexInputChecks,fixedStateChecks,topologyCases,lineImages,textureCases,pixelChecks,clearHash);
    if(!owner->options.reference)
      std::printf("{\"schema\":\"warvk-d3d9-annotation-fixture/v1\",\"annotate\":%s,\"draws\":%u,\"annotatedDraws\":%u,\"frameScenes\":%u,"
        "\"stageMarkers\":%u,\"localRefusals\":%u,\"digest\":\"%016llx\"}\n",JsonBool(annotationsRun),annotationDraws,annotatedDraws,
        annotationScenes,annotationStages,annotationRefusals,(unsigned long long)annotationDigest);
    std::printf("FORMAT_SURFACE_EVIDENCE formats=%u surfaces=%u\n",formatCases,surfaceChecks);
    Require(formatCases==16&&surfaceChecks==16*19,"format/surface coverage");
    std::printf("SAMPLER_EVIDENCE images=%u\n",samplerCases);Require(samplerCases==18,"sampler coverage");
    std::printf("EFFECTS_EVIDENCE images=%u\n",effectCases);Require(effectCases==44,"effects coverage");
    std::printf("TEXTURE_STAGES_EVIDENCE images=%u\n",textureStageCases);Require(textureStageCases==17,"texture stage coverage");
    std::printf("STATEBLOCK_EVIDENCE images=%u checks=%u\n",stateBlockImages,stateBlockChecks);Require(stateBlockImages==11&&stateBlockChecks>200,"state block coverage");
    delete owner;return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"factory-clear failed stage=%s reason=%s\n",owner?owner->stage:"allocate",e.what());}
  ExitProcess(2);
}
