#pragma once
#include "d3d9_util.h"
#include "../dxvk/dxvk_sampler.h"

namespace dxvk {
// Original BindSampler construction, shared with the x64 consumer. API facts
// and options are inputs; the Vulkan key remains local to the GPU owner.
template<class State,class Options>
inline DxvkSamplerKey BuildD3D9SamplerKey(const State& cState,bool cIsCube,
    bool cIsMultiMip,bool cIsDepth,const Options& options) {
  DxvkSamplerKey key = {};
  key.setFilter(DecodeFilter(cState.minFilter),DecodeFilter(cState.magFilter),DecodeMipFilter(cState.mipFilter));
  if(cIsCube) {
    key.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    key.setLegacyCubeFilter(!options.seamlessCubes);
  }else key.setAddressModes(DecodeAddressMode(cState.addressU),DecodeAddressMode(cState.addressV),DecodeAddressMode(cState.addressW));
  key.setDepthCompare(cIsDepth,VK_COMPARE_OP_LESS_OR_EQUAL);
  if(cState.mipFilter) {
    uint32_t anisotropy=cState.maxAnisotropy;
    if(cState.minFilter!=D3DTEXF_ANISOTROPIC||!cIsMultiMip)anisotropy=0u;
    if(options.samplerAnisotropy!=-1&&cIsMultiMip&&cState.minFilter>D3DTEXF_POINT)anisotropy=options.samplerAnisotropy;
    key.setAniso(anisotropy);
    float lodBias=cState.mipLodBias;lodBias+=options.samplerLodBias;
    if(options.clampNegativeLodBias)lodBias=std::max(lodBias,0.0f);
    key.setLodRange(float(cState.maxMipLevel),16.0f,lodBias);
  }
  if(key.u.p.hasBorder)DecodeD3DCOLOR(cState.borderColor,key.borderColor.float32);
  return key;
}
}
