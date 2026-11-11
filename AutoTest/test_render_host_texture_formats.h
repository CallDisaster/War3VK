#pragma once
// Public D3D9 calls and independently constructed format blocks. Deliberately
// does not include the production layout/format helper for its pixel oracle.
namespace {
unsigned formatCases=0,surfaceChecks=0;
struct FormatExample {
  D3DFORMAT format;unsigned blockWidth,blockBytes;DWORD color,changed;
  uint32_t value,changedValue;
};
constexpr FormatExample FormatExamples[]={
  {D3DFMT_A8R8G8B8,1,4,0xff00ff00,0xffff0000,0xff00ff00,0xffff0000},
  {D3DFMT_X8R8G8B8,1,4,0xff00ff00,0xffff0000,0x0000ff00,0x00ff0000},
  {D3DFMT_A8B8G8R8,1,4,0xffff0000,0xff00ff00,0xff0000ff,0xff00ff00},
  {D3DFMT_X8B8G8R8,1,4,0xffff0000,0xff00ff00,0x000000ff,0x0000ff00},
  {D3DFMT_R5G6B5,1,2,0xff00ff00,0xffff0000,0x07e0,0xf800},
  {D3DFMT_A1R5G5B5,1,2,0xffff0000,0xff00ff00,0xfc00,0x83e0},
  {D3DFMT_X1R5G5B5,1,2,0xffff0000,0xff00ff00,0x7c00,0x03e0},
  {D3DFMT_A4R4G4B4,1,2,0xff0000ff,0xffff0000,0xf00f,0xff00},
  {D3DFMT_A8L8,1,2,0x80ffffff,0xff000000,0x80ff,0xff00},
  {D3DFMT_A8,1,1,0x80000000,0xff000000,0x80,0xff},
  {D3DFMT_L8,1,1,0xff7f7f7f,0xffffffff,0x7f,0xff},
  {D3DFMT_DXT1,4,8,0xffff0000,0xff00ff00,0xf800,0x07e0},
  {D3DFMT_DXT2,4,16,0x88ff0000,0x8800ff00,0xf800,0x07e0},
  {D3DFMT_DXT3,4,16,0x88ff0000,0x8800ff00,0xf800,0x07e0},
  {D3DFMT_DXT4,4,16,0x80ff0000,0x8000ff00,0xf800,0x07e0},
  {D3DFMT_DXT5,4,16,0x80ff0000,0x8000ff00,0xf800,0x07e0},
};
void FillFormat(const FormatExample& example,D3DLOCKED_RECT lock,unsigned w,unsigned h,bool changed) {
  std::array<uint8_t,16> block{};
  const auto value=changed?example.changedValue:example.value;
  if(example.blockWidth==1)std::memcpy(block.data(),&value,example.blockBytes);
  else {
    unsigned colorOffset=0;
    if(example.format==D3DFMT_DXT2||example.format==D3DFMT_DXT3) {
      for(unsigned i=0;i<8;++i)block[i]=0x88;colorOffset=8;
    }else if(example.format==D3DFMT_DXT4||example.format==D3DFMT_DXT5) {
      block[0]=0x80;block[1]=0;colorOffset=8;
    }
    // All color indices select endpoint 0, independently of interpolation.
    block[colorOffset]=uint8_t(value);block[colorOffset+1]=uint8_t(value>>8);
  }
  const unsigned cols=(w+example.blockWidth-1)/example.blockWidth,rows=(h+example.blockWidth-1)/example.blockWidth;
  Require(lock.Pitch>=int(cols*example.blockBytes),"format pitch shorter than row");
  for(unsigned y=0;y<rows;++y)for(unsigned x=0;x<cols;++x)
    std::memcpy(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch+x*example.blockBytes,block.data(),example.blockBytes);
}
void ExerciseTextureFormats(Owner& o) {
  o.stage="texture-formats-and-surfaces";
  struct Screen {float x,y,z,w;DWORD color;float u,v;};
  Screen vertices[]={{0,0,.5f,1,0xffffffff,.25f,.25f},{64,0,.5f,1,0xffffffff,.25f,.25f},{64,64,.5f,1,0xffffffff,.25f,.25f},
    {0,0,.5f,1,0xffffffff,.25f,.25f},{64,64,.5f,1,0xffffffff,.25f,.25f},{0,64,.5f,1,0xffffffff,.25f,.25f}};
  auto uv=[&](float u,float v){
    for(auto& vertex:vertices){vertex.u=u;vertex.v=v;}
    void* data=nullptr;Check(o.vb->Lock(0,0,&data,0),"format VB lock");
    std::memcpy(data,vertices,sizeof(vertices));Check(o.vb->Unlock(),"format VB upload");
  };
  auto image=[&](DWORD expected){
    Check(o.device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff101820,1,0),"format clear");
    Check(o.device->BeginScene(),"format begin");Check(o.device->DrawPrimitive(D3DPT_TRIANGLELIST,0,2),"format draw");Check(o.device->EndScene(),"format end");
    std::array<uint32_t,4096> pixels;pixels.fill(expected);CheckImage(o,pixels);--clearCases;++textureCases;
  };
  for(const auto& example:FormatExamples) {
    Check(o.device->SetTexture(0,nullptr),"unbind previous format");o.texture.reset();
    Check(o.factory->CheckDeviceFormat(0,D3DDEVTYPE_HAL,D3DFMT_X8R8G8B8,0,D3DRTYPE_TEXTURE,example.format),"physical format support");
    Check(o.device->CreateTexture(128,64,0,0,example.format,D3DPOOL_MANAGED,o.texture.put(),nullptr),"format mip texture");
    Require(o.texture->GetLevelCount()==8,"format mip chain");
    Com<IDirect3DSurface9> top;
    for(UINT level=0;level<8;++level) {
      Com<IDirect3DSurface9> surface,again;Check(o.texture->GetSurfaceLevel(level,surface.put()),"surface level");
      Check(o.texture->GetSurfaceLevel(level,again.put()),"stable surface identity");Require(surface.get()==again.get(),"surface identity changed");++surfaceChecks;
      D3DSURFACE_DESC desc{};Check(surface->GetDesc(&desc),"surface description");
      Require(desc.Format==example.format&&desc.Width==std::max(1u,128u>>level)&&desc.Height==std::max(1u,64u>>level),"surface format/extent");++surfaceChecks;
      D3DLOCKED_RECT lock{};Check(surface->LockRect(&lock,nullptr,0),"surface format lock");
      FillFormat(example,lock,desc.Width,desc.Height,false);Check(surface->UnlockRect(),"surface format upload");
      if(!level)*top.put()=surface.detach();
    }
    // Locks belong to the shared subresource, not to the particular interface.
    D3DLOCKED_RECT lock{},other{};
    Check(top->LockRect(&lock,nullptr,D3DLOCK_READONLY),"surface readonly lock");
    Require(o.texture->LockRect(0,&other,nullptr,D3DLOCK_READONLY)==D3DERR_INVALIDCALL,"double lock through container");++surfaceChecks;
    Check(o.texture->LockRect(1,&other,nullptr,D3DLOCK_READONLY),"independent mip lock");
    Check(o.texture->UnlockRect(1),"independent mip unlock");Check(o.texture->UnlockRect(0),"container unlocks surface lock");
    Check(o.device->SetTexture(0,o.texture.get()),"bind format");uv(.25f,.25f);
    for(UINT level=0;level<8;++level){o.texture->SetLOD(level);image(example.color);}
    o.texture->SetLOD(0);const RECT patch=example.blockWidth==4?RECT{5,5,11,11}:RECT{4,4,12,12};
    Check(top->LockRect(&lock,&patch,0),"format patch lock");FillFormat(example,lock,8,8,true);Check(top->UnlockRect(),"format patch upload");
    uv(.0625f,.125f);image(example.changed);uv(.75f,.75f);image(example.color);
    // The surface alone must keep the container + device alive, while its own
    // IUnknown remains distinct. GetContainer reopens the same texture object.
    auto* original=o.texture.get();Check(o.device->SetTexture(0,nullptr),"unbind surface-owned texture");o.texture.reset();
    Check(top->GetContainer(__uuidof(IDirect3DTexture9),reinterpret_cast<void**>(o.texture.put())),"recover container from sole surface");
    Require(o.texture.get()==original,"container identity changed");++surfaceChecks;
    Com<IUnknown> unknown;Check(top->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(unknown.put())),"surface identity QI");
    Require(unknown.get()==static_cast<IUnknown*>(top.get())&&unknown.get()!=static_cast<IUnknown*>(original),"surface/container IUnknown alias");++surfaceChecks;
    Check(o.device->SetTexture(0,o.texture.get()),"rebind recovered container");image(example.color);++formatCases;
  }
}
}
