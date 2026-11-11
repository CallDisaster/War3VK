#pragma once
// Included after the proxy resource types. The capture categories and special
// cases follow src/d3d9/d3d9_stateblock.{h,cpp}, restricted to implemented APIs.
CapturedState::~CapturedState() {
  Bind(vb,static_cast<VertexBuffer*>(nullptr));Bind(ib,static_cast<IndexBuffer*>(nullptr));
  Bind(vs,static_cast<VertexShader*>(nullptr));Bind(ps,static_cast<PixelShader*>(nullptr));
  Bind(decl,static_cast<Declaration*>(nullptr));
  for(auto& texture:textures)Bind(texture,static_cast<Texture*>(nullptr));
}

class StateBlock final : public Child<IDirect3DStateBlock9> {
public:
  // Native counts state blocks as losable: Reset waits for their release.
  explicit StateBlock(Device* device):Child(device,{},d9::Type::None){++device->losable;}
  ~StateBlock(){--parent->losable;}
  std::unique_ptr<CapturedState> state;
  HRESULT STDMETHODCALLTYPE Capture() override {
    parent->followWindow();
    if(!parent->allowed()||parent->recording)return D3DERR_INVALIDCALL;
    parent->capture(*state,true);return parent->allowed()?D3D_OK:D3DERR_DEVICELOST;
  }
  HRESULT STDMETHODCALLTYPE Apply() override {
    parent->followWindow();
    if(!parent->allowed()||parent->recording)return D3DERR_INVALIDCALL;
    return parent->apply(*state);
  }
};

// 1 = PIXEL, 2 = VERTEX. Fog distances belong to both native categories.
static unsigned StateBlockRenderCategory(D3DRENDERSTATETYPE key) {
  if(key==D3DRS_CULLMODE)return 2;
  dxvkcore::D3D9Backend::RenderStates dummy{};
  if(RenderField(dummy,key))return 1;
  if(d9::FFIndex(d9::FFRenderStates,key)>=0)return key==D3DRS_TEXTUREFACTOR?1:2;
  switch(key) {
    case D3DRS_FOGSTART:case D3DRS_FOGEND:case D3DRS_FOGDENSITY:return 3;
    case D3DRS_DITHERENABLE:return 1;
    case D3DRS_FOGENABLE:case D3DRS_FOGCOLOR:case D3DRS_FOGTABLEMODE:case D3DRS_FOGVERTEXMODE:return 2;
    default:return d9::EffectIndex(key)>=0?1:0;
  }
}

void Device::capture(CapturedState& s,bool preserveOffset) {
  for(unsigned i=0;i<s.renderMask.size();++i)if(s.renderMask[i])GetRenderState(D3DRENDERSTATETYPE(i),&s.render[i]);
  for(unsigned i=0;i<8;++i) {
    for(unsigned k=1;k<d9::SamplerStateWords;++k)if(s.samplerMask[i][k])
      s.samplers[i].values[k]=(i?textureStages.samplers[i-1]:samplerState).values[k];
    for(unsigned k=0;k<d9::FFStageStates.size();++k)if(s.stageMask[i][k])s.fixed.stages[i][k]=fixed.stages[i][k];
    if(s.textureMask[i])Bind(s.textures[i],i?additionalTextures[i-1]:tex);
  }
  for(unsigned i=0;i<11;++i)if(s.transformMask[i])s.fixed.transforms[i]=fixed.transforms[i];
  for(unsigned shader=0;shader<2;++shader)for(unsigned i=0;i<32;++i)if(s.constantMask[shader][i])
    std::copy_n((shader?draw.pixelFloatConstants:draw.vertexFloatConstants).data()+i*4,4,s.constants[shader].data()+i*4);
  if(s.materialMask)s.fixed.material=fixed.material;
  if(s.viewportMask)s.viewport=draw.viewport;
  if(s.scissorMask)s.scissor={effects.scissor[0],effects.scissor[1],effects.scissor[2],effects.scissor[3]};
  if(s.vbMask){Bind(s.vb,vb);s.stride=draw.stride;if(!preserveOffset)s.offset=draw.streamOffset;}
  if(s.ibMask)Bind(s.ib,ib);
  if(s.vsMask)Bind(s.vs,vs);
  if(s.psMask)Bind(s.ps,ps);
  if(s.declMask)Bind(s.decl,decl);
  // Native Lights capture copies all currently defined lights, retaining
  // previously captured definitions, but only the selected enable bits.
  if(s.lightsMask)for(unsigned i=0;i<lights.size();++i) {
    if(lights[i].defined){s.lights[i].value=lights[i].value;s.lights[i].defined=true;}
    if(s.enabledMask[i]) {
      if(!s.lights[i].defined){s.lights[i].value={};s.lights[i].value.Type=D3DLIGHT_DIRECTIONAL;
        s.lights[i].value.Diffuse={1,1,1,0};s.lights[i].value.Direction={0,0,1};s.lights[i].defined=true;}
      s.lights[i].enabled=lights[i].enabled;
    }
  }
}

HRESULT Device::apply(const CapturedState& s) {
  // Capture preserves the old stream offset even when replacing the buffer.
  // Reject an out-of-profile resulting span before any state/resource changes.
  if(s.vbMask&&s.vb&&(!s.stride||s.stride>256||s.offset>s.vb->extent))return D3DERR_INVALIDCALL;
  // The finite profile supports eight active lights. Preflight the final set,
  // so an unsupported combination cannot partially publish a block.
  auto nextLights=lights;
  if(s.lightsMask)for(unsigned i=0;i<lights.size();++i) {
    if(s.lights[i].defined){nextLights[i].value=s.lights[i].value;nextLights[i].defined=true;}
    if(s.enabledMask[i])nextLights[i].enabled=s.lights[i].enabled;
  }
  unsigned active=0;for(const auto& light:nextLights)active+=light.enabled;
  if(active>8)return D3DERR_INVALIDCALL;
  // These values were validated before recording; resources have private
  // holds on this same Device. Restore through the normal local setters.
  const auto accept=[](HRESULT hr){return hr==D3D_OK;};
  for(unsigned i=0;i<s.renderMask.size();++i)if(s.renderMask[i]){
    const auto hr=SetRenderState(D3DRENDERSTATETYPE(i),s.render[i]);if(!accept(hr))return hr;
  }
  for(unsigned i=0;i<8;++i) {
    for(unsigned k=1;k<d9::SamplerStateWords;++k)if(s.samplerMask[i][k]){
      const auto hr=SetSamplerState(i,D3DSAMPLERSTATETYPE(k),s.samplers[i].values[k]);if(!accept(hr))return hr;
    }
    for(unsigned k=0;k<d9::FFStageStates.size();++k)if(s.stageMask[i][k]){
      const auto hr=SetTextureStageState(i,d9::FFStageStates[k],s.fixed.stages[i][k]);if(!accept(hr))return hr;
    }
    if(s.textureMask[i]){const auto hr=SetTexture(i,s.textures[i]);if(!accept(hr))return hr;}
  }
  for(unsigned i=0;i<11;++i)if(s.transformMask[i])fixed.transforms[i]=s.fixed.transforms[i];
  for(unsigned shader=0;shader<2;++shader)for(unsigned i=0;i<32;++i)if(s.constantMask[shader][i])
    std::copy_n(s.constants[shader].data()+i*4,4,(shader?draw.pixelFloatConstants:draw.vertexFloatConstants).data()+i*4);
  if(s.materialMask)fixed.material=s.fixed.material;
  if(s.viewportMask)draw.viewport=s.viewport;
  if(s.scissorMask)effects.scissor={s.scissor.left,s.scissor.top,s.scissor.right,s.scissor.bottom};
  if(s.vbMask){const auto hr=SetStreamSource(0,s.vb,s.offset,s.stride);if(!accept(hr))return hr;}
  if(s.ibMask){const auto hr=SetIndices(s.ib);if(!accept(hr))return hr;}
  if(s.vsMask){const auto hr=SetVertexShader(s.vs);if(!accept(hr))return hr;}
  if(s.psMask){const auto hr=SetPixelShader(s.ps);if(!accept(hr))return hr;}
  // Original Apply explicitly leaves the live declaration unchanged for null.
  if(s.declMask&&s.decl){const auto hr=SetVertexDeclaration(s.decl);if(!accept(hr))return hr;}
  lights=nextLights;
  return allowed()?D3D_OK:D3DERR_DEVICELOST;
}

HRESULT Device::BeginStateBlock() {
  if(!allowed()||recording)return D3DERR_INVALIDCALL;
  try{recording=std::make_unique<CapturedState>();return D3D_OK;}
  catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
}
HRESULT Device::EndStateBlock(IDirect3DStateBlock9** out) {
  if(out)*out=nullptr;
  if(!allowed()||!out||!recording)return D3DERR_INVALIDCALL;
  try{auto block=std::make_unique<StateBlock>(this);block->state=std::move(recording);
    block->AddRef();*out=block.release();return D3D_OK;}
  catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
}
HRESULT Device::CreateStateBlock(D3DSTATEBLOCKTYPE type,IDirect3DStateBlock9** out) {
  if(out)*out=nullptr;followWindow();
  if(!allowed()||!out||recording||(type!=D3DSBT_ALL&&type!=D3DSBT_PIXELSTATE&&type!=D3DSBT_VERTEXSTATE))return D3DERR_INVALIDCALL;
  try {
    auto block=std::make_unique<StateBlock>(this);block->state=std::make_unique<CapturedState>();auto& s=*block->state;
    const unsigned category=type==D3DSBT_ALL?3:type==D3DSBT_PIXELSTATE?1:2;
    for(unsigned i=0;i<s.renderMask.size();++i)s.renderMask[i]=(StateBlockRenderCategory(D3DRENDERSTATETYPE(i))&category)!=0;
    if(category&1) {
      s.psMask=true;s.constantMask[1].fill(true);
      for(auto& mask:s.stageMask)mask.fill(true);
      // Pixel state captures ELEMENTINDEX but not DMAPOFFSET (vertex sampler).
      for(auto& mask:s.samplerMask)for(unsigned k=1;k<=D3DSAMP_ELEMENTINDEX;++k)mask[k]=true;
    }
    if(category&2) {
      s.vsMask=s.declMask=s.freqMask=s.lightsMask=true;s.constantMask[0].fill(true);
      unsigned extent=0;for(unsigned i=0;i<lights.size();++i)if(lights[i].defined)extent=i+1;
      for(unsigned i=0;i<extent;++i)s.enabledMask[i]=true;
    }
    if(type==D3DSBT_ALL) {
      s.textureMask.fill(true);s.transformMask.fill(true);
      s.vbMask=s.ibMask=s.materialMask=s.viewportMask=s.scissorMask=true;
    }
    capture(s,false);if(!allowed())return D3DERR_DEVICELOST;
    block->AddRef();*out=block.release();return D3D_OK;
  }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
}
