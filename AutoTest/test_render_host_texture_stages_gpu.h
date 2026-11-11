#pragma once
namespace {
unsigned textureStageCases=0;
void ExerciseTextureStages(Owner& o) {
  o.stage="multiple-texture-stages";
  Check(o.device->SetVertexShader(nullptr),"multi fixed VS");Check(o.device->SetPixelShader(nullptr),"multi fixed PS");
  Check(o.device->SetTexture(0,nullptr),"multi remove old texture");o.texture.reset();
  const D3DVIEWPORT9 viewport{0,0,64,64,0,1};Check(o.device->SetViewport(&viewport),"multi viewport");
  for(auto state:{D3DRS_ALPHATESTENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_FOGENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_LIGHTING,D3DRS_ZENABLE})
    Check(o.device->SetRenderState(state,FALSE),"multi reset effects");
  struct Vertex{float x,y,z,u,v;};
  Vertex vertices[]={{-1,1,.5f,.25f,.25f},{1,1,.5f,.25f,.25f},{1,-1,.5f,.25f,.25f},
    {-1,1,.5f,.25f,.25f},{1,-1,.5f,.25f,.25f},{-1,-1,.5f,.25f,.25f}};
  Check(o.device->SetFVF(D3DFVF_XYZ|D3DFVF_TEX1),"multi FVF");
  Check(o.device->SetStreamSource(0,o.vb.get(),0,sizeof(Vertex)),"multi stream");
  auto upload=[&](){void* data=nullptr;Check(o.vb->Lock(0,sizeof(vertices),&data,0),"multi lock");
    std::memcpy(data,vertices,sizeof(vertices));Check(o.vb->Unlock(),"multi upload");};upload();
  std::array<Com<IDirect3DTexture9>,8> textures;
  std::array<DWORD,8> colors{};
  auto set=[&](unsigned stage,D3DTEXTURESTAGESTATETYPE key,DWORD value){Check(o.device->SetTextureStageState(stage,key,value),"multi stage set");
    DWORD actual=0;Check(o.device->GetTextureStageState(stage,key,&actual),"multi stage get");Require(actual==value,"multi stage roundtrip");};
  for(unsigned i=0;i<8;++i){
    colors[i]=0xff000000|((i+1)<<16)|((2*i+1)<<8)|(3*i+1);
    Check(o.device->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,textures[i].put(),nullptr),"multi texture create");
    D3DLOCKED_RECT lock{};Check(textures[i]->LockRect(0,&lock,nullptr,0),"multi texture lock");
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)std::memcpy(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch+x*4,&colors[i],4);
    Check(textures[i]->UnlockRect(0),"multi texture upload");Check(o.device->SetTexture(i,textures[i].get()),"multi texture bind");
    Com<IDirect3DBaseTexture9> read;Check(o.device->GetTexture(i,read.put()),"multi texture get");Require(read.get()==textures[i].get(),"multi texture identity");
    set(i,D3DTSS_TEXCOORDINDEX,0);set(i,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);
    set(i,D3DTSS_COLORARG1,D3DTA_TEXTURE);set(i,D3DTSS_COLORARG2,D3DTA_CURRENT);
    set(i,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);set(i,D3DTSS_ALPHAARG2,D3DTA_CURRENT);
    set(i,D3DTSS_COLOROP,D3DTOP_DISABLE);set(i,D3DTSS_ALPHAOP,i?D3DTOP_SELECTARG2:D3DTOP_SELECTARG1);
    for(auto key:{D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV})Check(o.device->SetSamplerState(i,key,D3DTADDRESS_CLAMP),"multi sampler address");
    Check(o.device->SetSamplerState(i,D3DSAMP_SRGBTEXTURE,FALSE),"multi sampler linear");
    Check(o.device->SetSamplerState(i,D3DSAMP_MIPFILTER,D3DTEXF_NONE),"multi sampler mip");
    Check(o.device->SetSamplerState(i,D3DSAMP_MINFILTER,D3DTEXF_POINT),"multi sampler min");
    Check(o.device->SetSamplerState(i,D3DSAMP_MAGFILTER,D3DTEXF_POINT),"multi sampler mag");
  }
  auto image=[&](DWORD expected){
    Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff887766,1,0),"multi clear");
    Check(o.device->BeginScene(),"multi begin");Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,0,2),"multi draw");Check(o.device->EndScene(),"multi end");
    std::array<uint32_t,4096> pixels;pixels.fill(expected);CheckImage(o,pixels);--clearCases;++textureStageCases;
  };
  DWORD sum=0xff000000;
  for(unsigned i=0;i<8;++i){set(i,D3DTSS_COLOROP,i?D3DTOP_ADD:D3DTOP_SELECTARG1);sum+=colors[i]&0xffffff;image(sum);}
  // All eight actual samplers in the original SM3 compiler, not an FF-only shortcut.
  constexpr char shader[]="struct V{float4 p:POSITION;float2 t:TEXCOORD0;};V vs(float3 p:POSITION,float2 t:TEXCOORD0){V o;o.p=float4(p,1);o.t=t;return o;}"
    "sampler2D s0:register(s0);sampler2D s1:register(s1);sampler2D s2:register(s2);sampler2D s3:register(s3);"
    "sampler2D s4:register(s4);sampler2D s5:register(s5);sampler2D s6:register(s6);sampler2D s7:register(s7);"
    "float4 ps(float2 t:TEXCOORD0):COLOR0{return tex2D(s0,t)+tex2D(s1,t)+tex2D(s2,t)+tex2D(s3,t)+tex2D(s4,t)+tex2D(s5,t)+tex2D(s6,t)+tex2D(s7,t);}";
  const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(o.compiler,"D3DCompile"));Com<ID3DBlob> code,errors;
  Check(compile(shader,sizeof(shader)-1,"multi-vs",nullptr,nullptr,"vs","vs_3_0",0,0,code.put(),errors.put()),"multi compile VS");
  Check(o.device->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()),o.vs.put()),"multi create VS");
  Check(o.device->SetVertexShader(o.vs.get()),"multi bind VS");code.reset();errors.reset();
  Check(compile(shader,sizeof(shader)-1,"multi-ps",nullptr,nullptr,"ps","ps_3_0",0,0,code.put(),errors.put()),"multi compile PS");
  Check(o.device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),o.ps.put()),"multi create PS");
  Check(o.device->SetPixelShader(o.ps.get()),"multi bind PS");image(sum);
  Check(o.device->SetVertexShader(nullptr),"multi restore fixed VS");Check(o.device->SetPixelShader(nullptr),"multi restore fixed PS");
  // Drop every public texture reference while eight device-private bindings remain.
  for(auto& texture:textures)texture.reset();image(sum);
  Com<IDirect3DBaseTexture9> recovered;Check(o.device->GetTexture(7,recovered.put()),"multi public resurrection");
  Check(recovered->QueryInterface(__uuidof(IDirect3DTexture9),reinterpret_cast<void**>(textures[7].put())),"multi recovered texture");recovered.reset();
  for(unsigned i=0;i<7;++i){set(i,D3DTSS_COLOROP,D3DTOP_SELECTARG1);set(i,D3DTSS_COLORARG1,D3DTA_DIFFUSE);}
  set(7,D3DTSS_COLOROP,D3DTOP_SELECTARG1);set(7,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
  D3DLOCKED_RECT lock{};Check(textures[7]->LockRect(0,&lock,nullptr,0),"multi high stage edit");
  const DWORD pattern[]={0xff401020,0xff104020,0xff111188,0xff888811};
  for(unsigned y=0;y<2;++y)std::memcpy(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch,pattern+y*2,8);
  Check(textures[7]->UnlockRect(0),"multi high stage upload");
  for(auto& vertex:vertices)vertex.u=1.25f;upload();
  Check(o.device->SetSamplerState(7,D3DSAMP_BORDERCOLOR,0xff223344),"multi border");
  for(DWORD address=D3DTADDRESS_WRAP;address<=D3DTADDRESS_MIRRORONCE;++address){
    Check(o.device->SetSamplerState(7,D3DSAMP_ADDRESSU,address),"multi high stage address");DWORD actual=0;
    Check(o.device->GetSamplerState(7,D3DSAMP_ADDRESSU,&actual),"multi high stage get sampler");Require(actual==address,"multi sampler identity");
    image(address==D3DTADDRESS_BORDER?0xff223344:address==D3DTADDRESS_WRAP?pattern[0]:pattern[1]);
  }
  Check(textures[7]->LockRect(0,&lock,nullptr,0),"multi srgb edit");const DWORD gray=0xff808080;
  for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)std::memcpy(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch+x*4,&gray,4);
  Check(textures[7]->UnlockRect(0),"multi srgb upload");
  Check(o.device->SetSamplerState(7,D3DSAMP_SRGBTEXTURE,TRUE),"multi srgb enable");image(0xff373737);
  Check(o.device->SetSamplerState(7,D3DSAMP_SRGBTEXTURE,FALSE),"multi srgb disable");image(gray);
  for(unsigned i=0;i<7;++i)Check(o.device->SetTexture(i,nullptr),"multi private unbind");
  for(unsigned i=1;i<8;++i)set(i,D3DTSS_COLOROP,D3DTOP_DISABLE);
  // The shared teardown deliberately drops the caller's device reference
  // while one public texture still holds it. Preserve that contract, now
  // exercising an additional-stage private binding rather than only stage 0.
  *o.texture.put()=textures[7].detach();
}
}
