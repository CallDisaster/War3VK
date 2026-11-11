#include "../src/d3d9/d3d9_sampler_key.h"
#include <cstdio>
using namespace dxvk;
struct Facts {
  D3DTEXTUREADDRESS addressU,addressV,addressW;D3DCOLOR borderColor;
  D3DTEXTUREFILTERTYPE magFilter,minFilter,mipFilter;float mipLodBias;
  DWORD maxMipLevel,maxAnisotropy;
};
struct Options {bool seamlessCubes;int samplerAnisotropy;float samplerLodBias;bool clampNegativeLodBias;};
// Frozen from the native BindSampler before extraction. Independent control
// flow and original key fields; not another call to the production helper.
static DxvkSamplerKey Reference(const Facts& cState,bool cIsCube,bool cIsMultiMip,bool cIsDepth,const Options& m_d3d9Options) {
  DxvkSamplerKey key = {};
  key.setFilter(DecodeFilter(cState.minFilter), DecodeFilter(cState.magFilter), DecodeMipFilter(cState.mipFilter));
  if(cIsCube) {
    key.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    key.setLegacyCubeFilter(!m_d3d9Options.seamlessCubes);
  }else {
    key.setAddressModes(DecodeAddressMode(cState.addressU),DecodeAddressMode(cState.addressV),DecodeAddressMode(cState.addressW));
  }
  key.setDepthCompare(cIsDepth,VK_COMPARE_OP_LESS_OR_EQUAL);
  if(cState.mipFilter) {
    uint32_t anisotropy=cState.maxAnisotropy;
    if(cState.minFilter!=D3DTEXF_ANISOTROPIC||!cIsMultiMip)anisotropy=0u;
    if(m_d3d9Options.samplerAnisotropy!=-1&&cIsMultiMip&&cState.minFilter>D3DTEXF_POINT)anisotropy=m_d3d9Options.samplerAnisotropy;
    key.setAniso(anisotropy);
    float lodBias=cState.mipLodBias;lodBias+=m_d3d9Options.samplerLodBias;
    if(m_d3d9Options.clampNegativeLodBias)lodBias=std::max(lodBias,0.0f);
    key.setLodRange(float(cState.maxMipLevel),16.0f,lodBias);
  }
  if(key.u.p.hasBorder)DecodeD3DCOLOR(cState.borderColor,key.borderColor.float32);
  return key;
}
int main() {
  uint32_t seed=0x75945a33,checks=0;
  auto next=[&](){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;};
  for(unsigned i=0;i<100000;++i) {
    Facts f{D3DTEXTUREADDRESS(next()%8),D3DTEXTUREADDRESS(next()%8),D3DTEXTUREADDRESS(next()%8),next(),
      D3DTEXTUREFILTERTYPE(next()%4),D3DTEXTUREFILTERTYPE(next()%4),D3DTEXTUREFILTERTYPE(next()%3),
      float(int(next()%8193)-4096)/256.0f,next()%32,next()%33};
    Options o{bool(next()&1),int(next()%34)-1,float(int(next()%4097)-2048)/256.0f,bool(next()&1)};
    const bool cube=next()&1,mips=next()&1,depth=next()&1;
    const auto a=BuildD3D9SamplerKey(f,cube,mips,depth,o),b=Reference(f,cube,mips,depth,o);
    if(std::memcmp(&a,&b,sizeof(a)))return 1;++checks;
    if(!a.eq(b)||a.hash()!=b.hash())return 2;++checks;
  }
  std::printf("sampler key checks=%u failures=0 bits=%zu\n",checks,sizeof(void*)*8);
}
