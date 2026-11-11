#pragma once
namespace {
unsigned effectCases=0;
void ExerciseEffects(Owner& o){
  o.stage="alpha-fog-scissor-separate-blend";
  Check(o.device->SetTexture(0,nullptr),"effects unbind texture");o.texture.reset();
  Check(o.device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1),"effects color op");
  Check(o.device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE),"effects color arg");
  Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1),"effects alpha op");
  Check(o.device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE),"effects alpha arg");
  Check(o.device->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE),"effects FVF");
  struct Vertex{float x,y,z;DWORD color;};
  const Vertex vertices[]={{-1,1,.5f,0x80ff0000},{1,1,.5f,0x80ff0000},{1,-1,.5f,0x80ff0000},
    {-1,1,.5f,0x80ff0000},{1,-1,.5f,0x80ff0000},{-1,-1,.5f,0x80ff0000}};
  void* data=nullptr;Check(o.vb->Lock(0,0,&data,0),"effects vertices lock");
  std::memcpy(data,vertices,sizeof(vertices));Check(o.vb->Unlock(),"effects vertices upload");
  Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(Vertex)),"effects stream");
  D3DMATRIX identity{};for(unsigned i=0;i<4;++i)identity.m[i][i]=1;
  for(auto transform:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION})Check(o.device->SetTransform(transform,&identity),"effects identity");
  auto set=[&](D3DRENDERSTATETYPE key,DWORD value){
    Check(o.device->SetRenderState(key,value),"effects set");DWORD actual=~value;
    Check(o.device->GetRenderState(key,&actual),"effects get");Require(actual==value,"effects state roundtrip");
  };
  auto scalar=[&](D3DRENDERSTATETYPE key,float value){DWORD word;std::memcpy(&word,&value,4);set(key,word);};
  constexpr DWORD background=0x40102030,red=0x80ff0000;
  auto image=[&](DWORD color,const RECT* rect=nullptr,unsigned tolerance=0){
    std::fprintf(stderr,"effects image=%u expected=%08lx\n",effectCases,color);
    // Native Clear also obeys the scissor. Initialize a full background before
    // testing draw clipping, otherwise old red pixels outside it remain valid.
    if(rect)set(D3DRS_SCISSORTESTENABLE,FALSE);
    Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET,background,1,0),"effects clear");
    if(rect)set(D3DRS_SCISSORTESTENABLE,TRUE);
    Check(o.device->BeginScene(),"effects begin");Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,0,2),"effects draw");Check(o.device->EndScene(),"effects end");
    std::array<uint32_t,4096> pixels{};
    for(int y=0;y<64;++y)for(int x=0;x<64;++x)
      pixels[y*64+x]=!rect||(x>=rect->left&&x<rect->right&&y>=rect->top&&y<rect->bottom)?color:background;
    CheckImage(o,pixels,tolerance);--clearCases;++effectCases;
  };
  set(D3DRS_ALPHATESTENABLE,TRUE);
  for(DWORD ref:{127u,128u,129u})for(DWORD func=D3DCMP_NEVER;func<=D3DCMP_ALWAYS;++func){
    set(D3DRS_ALPHAFUNC,func);set(D3DRS_ALPHAREF,ref);
    const bool pass=func==D3DCMP_ALWAYS||(func==D3DCMP_LESS&&128<ref)||(func==D3DCMP_EQUAL&&128==ref)
      ||(func==D3DCMP_LESSEQUAL&&128<=ref)||(func==D3DCMP_GREATER&&128>ref)
      ||(func==D3DCMP_NOTEQUAL&&128!=ref)||(func==D3DCMP_GREATEREQUAL&&128>=ref);
    image(pass?red:background);
  }
  set(D3DRS_ALPHAFUNC,D3DCMP_NEVER);set(D3DRS_ALPHATESTENABLE,FALSE);image(red);
  set(D3DRS_SCISSORTESTENABLE,TRUE);
  const RECT rectangles[]={{16,16,48,48},{-10,-10,32,32},{0,0,64,64},{32,32,32,32},{40,40,20,20},{48,-8,72,40}};
  for(const auto& rect:rectangles){Check(o.device->SetScissorRect(&rect),"effects scissor");RECT actual{};
    Check(o.device->GetScissorRect(&actual),"effects scissor get");Require(!std::memcmp(&rect,&actual,sizeof(rect)),"scissor roundtrip");image(red,&rect);}
  set(D3DRS_SCISSORTESTENABLE,FALSE);
  for(unsigned test=0;test<2;++test){
    const D3DVIEWPORT9 full{0,0,64,64,0,1},part{8,8,40,40,0,1};
    Check(o.device->SetViewport(&full),"scissor clear full viewport");
    Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET,background,1,0),"scissor clear background");
    const RECT clip{16,16,60,60};Check(o.device->SetScissorRect(&clip),"scissor clear clip");
    set(D3DRS_SCISSORTESTENABLE,TRUE);
    if(test)Check(o.device->SetViewport(&part),"scissor clear part viewport");
    const D3DRECT rect{24,4,36,32};
    Check(o.device->Clear(test?1:0,test?&rect:nullptr,D3DCLEAR_TARGET,red,1,0),"scissor clear intersection");
    std::array<uint32_t,4096> pixels{};
    for(int y=0;y<64;++y)for(int x=0;x<64;++x){
      const bool hit=x>=16&&y>=16&&x<60&&y<60&&(!test||(x>=24&&x<36&&y<32));
      pixels[y*64+x]=hit?red:background;
    }
    CheckImage(o,pixels);--clearCases;++effectCases;
    set(D3DRS_SCISSORTESTENABLE,FALSE);Check(o.device->SetViewport(&full),"scissor clear restore viewport");
  }
  set(D3DRS_ALPHABLENDENABLE,TRUE);set(D3DRS_SEPARATEALPHABLENDENABLE,TRUE);
  set(D3DRS_SRCBLEND,D3DBLEND_ONE);set(D3DRS_DESTBLEND,D3DBLEND_ZERO);
  set(D3DRS_SRCBLENDALPHA,D3DBLEND_ZERO);set(D3DRS_DESTBLENDALPHA,D3DBLEND_ONE);image(0x40ff0000);
  set(D3DRS_SRCBLENDALPHA,D3DBLEND_ONE);set(D3DRS_DESTBLENDALPHA,D3DBLEND_ZERO);image(red);
  set(D3DRS_DESTBLENDALPHA,D3DBLEND_ONE);set(D3DRS_BLENDOPALPHA,D3DBLENDOP_MAX);image(red);
  set(D3DRS_ALPHABLENDENABLE,FALSE);set(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
  set(D3DRS_FOGENABLE,TRUE);set(D3DRS_FOGCOLOR,0xff0000ff);
  scalar(D3DRS_FOGSTART,0);scalar(D3DRS_FOGEND,1);scalar(D3DRS_FOGDENSITY,2);
  set(D3DRS_FOGTABLEMODE,D3DFOG_LINEAR);image(0x80800080,nullptr,1);
  set(D3DRS_FOGTABLEMODE,D3DFOG_EXP);image(0x805e00a1,nullptr,1);
  set(D3DRS_FOGTABLEMODE,D3DFOG_EXP2);image(0x805e00a1,nullptr,1);
  set(D3DRS_FOGTABLEMODE,D3DFOG_NONE);set(D3DRS_FOGVERTEXMODE,D3DFOG_LINEAR);scalar(D3DRS_FOGEND,2);
  image(0x80bf0040,nullptr,1);set(D3DRS_RANGEFOGENABLE,TRUE);image(0x804000bf,nullptr,1);
  set(D3DRS_RANGEFOGENABLE,FALSE);set(D3DRS_FOGVERTEXMODE,D3DFOG_EXP);set(D3DRS_FOGTABLEMODE,D3DFOG_LINEAR);
  image(0x80bf0040,nullptr,1);set(D3DRS_FOGENABLE,FALSE);image(red);
  // State changes while earlier SM3 batches are still in flight. No per-tile
  // readback or Query wait may hide a shared specialization-UBO overwrite.
  constexpr char shader[]="struct V{float4 p:POSITION;float4 c:COLOR0;};V vs(float3 p:POSITION,float4 c:COLOR0){V o;o.p=float4(p,1);o.c=c;return o;}float4 ps(float4 c:COLOR0):COLOR0{return c;}";
  const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(o.compiler,"D3DCompile"));
  Com<ID3DBlob> code,errors;
  Check(compile(shader,sizeof(shader)-1,"effects-flight-vs",nullptr,nullptr,"vs","vs_3_0",0,0,code.put(),errors.put()),"effects VS compile");
  Check(o.device->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()),o.vs.put()),"effects VS create");
  Check(o.device->SetVertexShader(o.vs.get()),"effects VS bind");code.reset();errors.reset();
  Check(compile(shader,sizeof(shader)-1,"effects-flight-ps",nullptr,nullptr,"ps","ps_3_0",0,0,code.put(),errors.put()),"effects PS compile");
  Check(o.device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),o.ps.put()),"effects PS create");
  Check(o.device->SetPixelShader(o.ps.get()),"effects PS bind");
  Check(o.device->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,o.texture.put(),nullptr),"effects flight texture");
  D3DLOCKED_RECT lock{};Check(o.texture->LockRect(0,&lock,nullptr,0),"effects flight texture lock");
  const DWORD white=0xffffffff;std::memcpy(lock.pBits,&white,4);Check(o.texture->UnlockRect(0),"effects flight upload");
  Check(o.device->SetTexture(0,o.texture.get()),"effects flight texture bind");
  Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET,background,1,0),"effects flight clear");
  set(D3DRS_ALPHATESTENABLE,TRUE);
  Check(o.device->BeginScene(),"effects flight begin");
  for(unsigned i=0;i<16;++i){const D3DVIEWPORT9 viewport{(i%4)*16,(i/4)*16,16,16,0,1};
    Check(o.device->SetViewport(&viewport),"effects flight viewport");
    set(D3DRS_ALPHAFUNC,i&1?D3DCMP_ALWAYS:D3DCMP_NEVER);
    Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,0,2),"effects flight draw");
  }
  Check(o.device->EndScene(),"effects flight end");
  std::array<uint32_t,4096> flightPixels{};
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)flightPixels[y*64+x]=((x/16)&1)?red:background;
  CheckImage(o,flightPixels);--clearCases;++effectCases;
}
}
