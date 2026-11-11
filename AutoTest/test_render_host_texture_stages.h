#pragma once
#include "../tools/render_host/d3d9_texture_stages_wire.h"
namespace warvk_texture_stages_test {
template<class Check> void Run(Check check) {
  using namespace warvk::host::d9;
  TextureStages state,output;
  for(unsigned i=0;i<AdditionalTextureStages;++i) {
    state.textures[i]={i+1,32+i};state.samplers[i].values[D3DSAMP_BORDERCOLOR]=0xff112200+i;
  }
  const auto valid=EncodeTextureStages(state);
  check(DecodeTextureStages(valid.data(),valid.size(),output));check(EncodeTextureStages(output)==valid);
  for(unsigned n=0;n<valid.size();++n) {
    check(!DecodeTextureStages(valid.data(),n,output));check(EncodeTextureStages(output)==valid);
  }
  check(!DecodeTextureStages(nullptr,valid.size(),output));
  for(unsigned i=0;i<AdditionalTextureStages;++i)for(unsigned kind=0;kind<5;++kind) {
    auto bad=valid;auto* p=bad.data()+i*TextureStageBytes;
    if(kind==0)P32(p,0);if(kind==1)P32(p+4,0);if(kind==2)P32(p,MaxResources+1);
    if(kind==3)P32(p+8+D3DSAMP_ADDRESSU*4,0);
    if(kind==4)P32(p+8+D3DSAMP_MIPMAPLODBIAS*4,0x7f800000);
    check(!DecodeTextureStages(bad.data(),bad.size(),output));check(EncodeTextureStages(output)==valid);
  }
  for(unsigned i=0;i<AdditionalTextureStages;++i) {
    auto next=state;next.textures[i]={};const auto bytes=EncodeTextureStages(next);
    check(DecodeTextureStages(bytes.data(),bytes.size(),output));check(EncodeTextureStages(output)==bytes);
  }
}
}
