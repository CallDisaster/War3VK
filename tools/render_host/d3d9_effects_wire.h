#pragma once
#include "d3d9_wire.h"
#include <d3d9.h>
#include <cmath>
namespace warvk::host::d9 {
// API values only, ordered with draws. Native shader specialization and push
// constants remain the x64 owner's responsibility; no Vulkan bytes on wire.
// DEPTHBIAS is Game.dll's (S1 G2b); SLOPESCALEDEPTHBIAS is not carried.
constexpr std::array<D3DRENDERSTATETYPE,16> EffectKeys={
  D3DRS_ALPHATESTENABLE,D3DRS_ALPHAFUNC,D3DRS_ALPHAREF,
  D3DRS_FOGENABLE,D3DRS_FOGCOLOR,D3DRS_FOGTABLEMODE,D3DRS_FOGVERTEXMODE,
  D3DRS_FOGSTART,D3DRS_FOGEND,D3DRS_FOGDENSITY,D3DRS_SCISSORTESTENABLE,
  D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_SRCBLENDALPHA,D3DRS_DESTBLENDALPHA,D3DRS_BLENDOPALPHA,
  D3DRS_DEPTHBIAS};
constexpr uint32_t EffectsBytes=(EffectKeys.size()+4)*4;
struct EffectsState {
  std::array<uint32_t,EffectKeys.size()> values{0,D3DCMP_ALWAYS,0,0,0,D3DFOG_NONE,D3DFOG_NONE,
    0,0x3f800000,0x3f800000,0,0,D3DBLEND_ONE,D3DBLEND_ZERO,D3DBLENDOP_ADD,0};
  std::array<int32_t,4> scissor{0,0,Width,Height};
  bool operator==(const EffectsState& other)const{return values==other.values&&scissor==other.scissor;}
  uint32_t get(D3DRENDERSTATETYPE key)const{
    for(size_t i=0;i<EffectKeys.size();++i)if(EffectKeys[i]==key)return values[i];return 0;
  }
};
inline int EffectIndex(D3DRENDERSTATETYPE key){
  for(size_t i=0;i<EffectKeys.size();++i)if(EffectKeys[i]==key)return int(i);return -1;
}
inline bool ValidEffect(D3DRENDERSTATETYPE key,uint32_t value){
  switch(key){
    case D3DRS_ALPHATESTENABLE:case D3DRS_FOGENABLE:case D3DRS_SCISSORTESTENABLE:
    case D3DRS_SEPARATEALPHABLENDENABLE:return value<=1;
    case D3DRS_ALPHAFUNC:return value>=D3DCMP_NEVER&&value<=D3DCMP_ALWAYS;
    case D3DRS_ALPHAREF:case D3DRS_FOGCOLOR:return true;
    case D3DRS_FOGTABLEMODE:case D3DRS_FOGVERTEXMODE:return value<=D3DFOG_LINEAR;
    case D3DRS_FOGSTART:case D3DRS_FOGEND:case D3DRS_FOGDENSITY:case D3DRS_DEPTHBIAS:return std::isfinite(Float(value));
    case D3DRS_SRCBLENDALPHA:case D3DRS_DESTBLENDALPHA:return value>=D3DBLEND_ZERO&&value<=D3DBLEND_SRCALPHASAT;
    case D3DRS_BLENDOPALPHA:return value>=D3DBLENDOP_ADD&&value<=D3DBLENDOP_MAX;
    default:return false;
  }
}
inline bool ValidEffects(const EffectsState& state){
  for(size_t i=0;i<EffectKeys.size();++i)if(!ValidEffect(EffectKeys[i],state.values[i]))return false;
  // Native SetScissorRect stores all signed rectangles, including empty and
  // inverted ones. Clipping at draw time must never underflow a Vulkan extent.
  return true;
}
inline std::array<uint8_t,EffectsBytes> EncodeEffects(const EffectsState& state){
  std::array<uint8_t,EffectsBytes> bytes{};size_t i=0;
  for(auto value:state.values){P32(bytes.data()+i*4,value);++i;}
  for(auto value:state.scissor){P32(bytes.data()+i*4,uint32_t(value));++i;}return bytes;
}
inline bool DecodeEffects(const uint8_t* bytes,uint32_t size,EffectsState& output){
  if(!bytes||size!=EffectsBytes)return false;EffectsState state;size_t i=0;
  for(auto& value:state.values){value=U32(bytes+i*4);++i;}
  for(auto& value:state.scissor){value=int32_t(U32(bytes+i*4));++i;}
  if(!ValidEffects(state))return false;output=state;return true;
}
}
