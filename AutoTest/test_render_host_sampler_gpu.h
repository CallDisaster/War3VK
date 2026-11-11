#pragma once
namespace {
unsigned samplerCases=0;
void ExerciseSampler(Owner& o) {
  o.stage="sampler-address-filter-lod-srgb";
  Check(o.device->SetTexture(0,nullptr),"sampler unbind");o.texture.reset();
  Check(o.device->CreateTexture(8,8,0,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,o.texture.put(),nullptr),"sampler texture");
  constexpr DWORD mipColors[]={0xffffffff,0xffff0000,0xff00ff00,0xff0000ff};
  auto fill=[&](unsigned level,bool quadrants,DWORD value){
    D3DSURFACE_DESC desc{};Check(o.texture->GetLevelDesc(level,&desc),"sampler mip desc");
    D3DLOCKED_RECT lock{};Check(o.texture->LockRect(level,&lock,nullptr,0),"sampler mip lock");
    for(unsigned y=0;y<desc.Height;++y) {
      for(unsigned x=0;x<desc.Width;++x) {
        const DWORD color=quadrants?(y<4?(x<4?0xff0000ff:0xffff0000):(x<4?0xff00ff00:0xffffffff)):value;
        std::memcpy(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch+x*4,&color,4);
      }
    }
    Check(o.texture->UnlockRect(level),"sampler mip upload");
  };
  for(unsigned i=0;i<4;++i)fill(i,!i,mipColors[i]);
  Check(o.device->SetTexture(0,o.texture.get()),"sampler bind");
  struct Screen {float x,y,z,w;DWORD color;float u,v;};
  Screen vertices[]={{0,0,.5f,1,0xffffffff,0,0},{64,0,.5f,1,0xffffffff,0,0},{64,64,.5f,1,0xffffffff,0,0},
    {0,0,.5f,1,0xffffffff,0,0},{64,64,.5f,1,0xffffffff,0,0},{0,64,.5f,1,0xffffffff,0,0}};
  auto uv=[&](float u,float v,bool varying=false){
    for(auto& vertex:vertices){vertex.u=varying?vertex.x*u/64:u;vertex.v=varying?vertex.y*v/64:v;}
    void* data=nullptr;Check(o.vb->Lock(0,0,&data,0),"sampler VB lock");std::memcpy(data,vertices,sizeof(vertices));Check(o.vb->Unlock(),"sampler VB upload");
  };
  auto set=[&](D3DSAMPLERSTATETYPE type,DWORD value){
    Check(o.device->SetSamplerState(0,type,value),"sampler set");DWORD read=~value;
    Check(o.device->GetSamplerState(0,type,&read),"sampler get");Require(read==value,"sampler roundtrip mismatch");
  };
  auto image=[&](DWORD expected,unsigned rgbTolerance=0){
    Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff101820,1,0),"sampler clear");
    Check(o.device->BeginScene(),"sampler begin");Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,0,2),"sampler draw");Check(o.device->EndScene(),"sampler end");
    std::array<uint32_t,4096> pixels;pixels.fill(expected);CheckImage(o,pixels,rgbTolerance);--clearCases;++samplerCases;
  };
  set(D3DSAMP_BORDERCOLOR,0xff123456);set(D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
  const struct {DWORD mode;float u;DWORD color;} address[]={
    {D3DTADDRESS_WRAP,1.25f,0xff0000ff},{D3DTADDRESS_MIRROR,1.25f,0xffff0000},
    {D3DTADDRESS_CLAMP,1.25f,0xffff0000},{D3DTADDRESS_BORDER,1.25f,0xff123456},
    {D3DTADDRESS_MIRRORONCE,-1.25f,0xffff0000},{D3DTADDRESS_WRAP,-.25f,0xffff0000},
    {D3DTADDRESS_MIRROR,-.25f,0xff0000ff}};
  for(auto example:address){set(D3DSAMP_ADDRESSU,example.mode);uv(example.u,.25f);image(example.color);}
  set(D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);set(D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP);set(D3DSAMP_ADDRESSW,D3DTADDRESS_MIRROR);
  uv(.25f,1.75f);image(0xff00ff00);
  // The analytic midpoint is 127.5 UNORM8. Allow its two quantizations only;
  // the complete observed image hash must still match native and remote exactly.
  set(D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);uv(.5f,.5f);image(0xff808080,1);
  // Uniform levels make the independent LOD oracle insensitive to pixel-center
  // conventions. UV derivatives are 4 texels/pixel => explicit lambda 2.
  fill(0,false,mipColors[0]);set(D3DSAMP_MAGFILTER,D3DTEXF_POINT);set(D3DSAMP_MINFILTER,D3DTEXF_POINT);
  set(D3DSAMP_MIPFILTER,D3DTEXF_POINT);uv(32,32,true);image(mipColors[2]);
  set(D3DSAMP_MAXMIPLEVEL,3);image(mipColors[3]);set(D3DSAMP_MAXMIPLEVEL,0);
  auto bias=[&](float value){DWORD bits;std::memcpy(&bits,&value,4);set(D3DSAMP_MIPMAPLODBIAS,bits);};
  bias(-2);image(mipColors[0]);bias(.5f);set(D3DSAMP_MIPFILTER,D3DTEXF_LINEAR);image(0xff008080,1);
  bias(0);set(D3DSAMP_MIPFILTER,D3DTEXF_POINT);set(D3DSAMP_MINFILTER,D3DTEXF_ANISOTROPIC);set(D3DSAMP_MAXANISOTROPY,4);image(mipColors[2]);
  set(D3DSAMP_MINFILTER,D3DTEXF_POINT);o.texture->SetLOD(1);image(mipColors[2]);o.texture->SetLOD(0);
  fill(0,false,0xff808080);set(D3DSAMP_MIPFILTER,D3DTEXF_NONE);uv(.25f,.25f);image(0xff808080);
  Check(o.factory->CheckDeviceFormat(0,D3DDEVTYPE_HAL,D3DFMT_X8R8G8B8,D3DUSAGE_QUERY_SRGBREAD,D3DRTYPE_TEXTURE,D3DFMT_A8R8G8B8),"sRGB physical support");
  set(D3DSAMP_SRGBTEXTURE,TRUE);image(0xff373737);set(D3DSAMP_SRGBTEXTURE,FALSE);image(0xff808080);
}
}
