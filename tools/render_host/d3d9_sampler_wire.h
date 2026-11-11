#pragma once
#include "d3d9_wire.h"
#include <d3d9.h>
#include <cmath>
namespace warvk::host::d9 {
constexpr uint32_t SamplerStateWords=D3DSAMP_DMAPOFFSET+1,SamplerStateBytes=SamplerStateWords*4;
struct SamplerState {
  std::array<DWORD,SamplerStateWords> values{};
  SamplerState():SamplerState(false) { }
  explicit SamplerState(bool legacyClamp) {
    values[D3DSAMP_ADDRESSU]=values[D3DSAMP_ADDRESSV]=values[D3DSAMP_ADDRESSW]=legacyClamp?D3DTADDRESS_CLAMP:D3DTADDRESS_WRAP;
    values[D3DSAMP_MINFILTER]=values[D3DSAMP_MAGFILTER]=D3DTEXF_POINT;
    values[D3DSAMP_MAXANISOTROPY]=1;
  }
};
inline bool ValidSamplerValue(uint32_t key,uint32_t value) {
  switch(key) {
    case D3DSAMP_ADDRESSU:case D3DSAMP_ADDRESSV:case D3DSAMP_ADDRESSW:return value>=D3DTADDRESS_WRAP&&value<=D3DTADDRESS_MIRRORONCE;
    case D3DSAMP_MAGFILTER:case D3DSAMP_MINFILTER:return value>=D3DTEXF_POINT&&value<=D3DTEXF_ANISOTROPIC;
    case D3DSAMP_MIPFILTER:return value<=D3DTEXF_LINEAR;
    case D3DSAMP_MIPMAPLODBIAS:{float f;std::memcpy(&f,&value,4);return std::isfinite(f);}
    case D3DSAMP_BORDERCOLOR:case D3DSAMP_MAXMIPLEVEL:case D3DSAMP_MAXANISOTROPY:return true;
    case D3DSAMP_SRGBTEXTURE:return value<=1;
    case D3DSAMP_ELEMENTINDEX:case D3DSAMP_DMAPOFFSET:return value==0;
    default:return false;
  }
}
inline bool ValidSamplerState(const SamplerState& state) {
  if(state.values[0])return false;
  for(uint32_t i=1;i<SamplerStateWords;++i)if(!ValidSamplerValue(i,state.values[i]))return false;
  return true;
}
inline std::array<uint8_t,SamplerStateBytes> EncodeSamplerState(const SamplerState& state) {
  std::array<uint8_t,SamplerStateBytes> bytes{};
  for(uint32_t i=0;i<SamplerStateWords;++i)P32(bytes.data()+i*4,state.values[i]);return bytes;
}
inline bool DecodeSamplerState(const uint8_t* bytes,uint32_t size,SamplerState& output) {
  if(!bytes||size!=SamplerStateBytes)return false;
  SamplerState state;
  for(uint32_t i=0;i<SamplerStateWords;++i)state.values[i]=U32(bytes+i*4);
  if(!ValidSamplerState(state))return false;output=state;return true;
}
}
