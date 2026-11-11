#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>
#include <array>
#include <cmath>
#include <cstdint>

namespace warvk::host::d9 {
// Fixed-function API facts only. Matrices retain the original D3D row-vector
// layout until the existing DXVK Matrix4/key compiler consumes them in x64.
constexpr std::array<D3DRENDERSTATETYPE,14> FFRenderStates={
  D3DRS_LIGHTING,D3DRS_COLORVERTEX,D3DRS_NORMALIZENORMALS,D3DRS_LOCALVIEWER,
  D3DRS_SPECULARENABLE,D3DRS_DIFFUSEMATERIALSOURCE,D3DRS_AMBIENTMATERIALSOURCE,
  D3DRS_SPECULARMATERIALSOURCE,D3DRS_EMISSIVEMATERIALSOURCE,D3DRS_AMBIENT,
  D3DRS_TEXTUREFACTOR,D3DRS_RANGEFOGENABLE,D3DRS_VERTEXBLEND,D3DRS_CLIPPLANEENABLE};
constexpr std::array<D3DTEXTURESTAGESTATETYPE,11> FFStageStates={
  D3DTSS_COLOROP,D3DTSS_COLORARG0,D3DTSS_COLORARG1,D3DTSS_COLORARG2,
  D3DTSS_ALPHAOP,D3DTSS_ALPHAARG0,D3DTSS_ALPHAARG1,D3DTSS_ALPHAARG2,
  D3DTSS_RESULTARG,D3DTSS_TEXCOORDINDEX,D3DTSS_TEXTURETRANSFORMFLAGS};
template<class T,size_t N> inline int FFIndex(const std::array<T,N>& keys,T key) {
  for(size_t i=0;i<N;++i)if(keys[i]==key)return int(i);return -1;
}
inline int FFTransformIndex(D3DTRANSFORMSTATETYPE type) {
  if(type==D3DTS_WORLD)return 0;if(type==D3DTS_VIEW)return 1;if(type==D3DTS_PROJECTION)return 2;
  return type>=D3DTS_TEXTURE0&&type<=D3DTS_TEXTURE7?3+int(type-D3DTS_TEXTURE0):-1;
}
struct FixedFunctionState {
  std::array<D3DMATRIX,11> transforms{};
  std::array<uint32_t,14> render{TRUE,TRUE,FALSE,TRUE,FALSE,
    D3DMCS_COLOR1,D3DMCS_MATERIAL,D3DMCS_COLOR2,D3DMCS_MATERIAL,0,0xffffffff,0,0,0};
  std::array<std::array<uint32_t,11>,8> stages{};
  D3DMATERIAL9 material{};
  uint32_t lightCount=0;
  std::array<D3DLIGHT9,8> lights{};
  FixedFunctionState() {
    for(auto& m:transforms)for(unsigned i=0;i<4;++i)m.m[i][i]=1;
    material.Diffuse=material.Ambient={1,1,1,1};
    for(unsigned i=0;i<8;++i)stages[i]={
      i?D3DTOP_DISABLE:D3DTOP_MODULATE,D3DTA_CURRENT,D3DTA_TEXTURE,D3DTA_CURRENT,
      i?D3DTOP_DISABLE:D3DTOP_SELECTARG1,D3DTA_CURRENT,D3DTA_TEXTURE,D3DTA_CURRENT,
      D3DTA_CURRENT,i,D3DTTFF_DISABLE};
  }
};
inline bool ValidFFRender(D3DRENDERSTATETYPE key,uint32_t value) {
  switch(key) {
    case D3DRS_LIGHTING:case D3DRS_COLORVERTEX:case D3DRS_NORMALIZENORMALS:
    case D3DRS_LOCALVIEWER:case D3DRS_SPECULARENABLE:case D3DRS_RANGEFOGENABLE:return value<=1;
    case D3DRS_DIFFUSEMATERIALSOURCE:case D3DRS_AMBIENTMATERIALSOURCE:
    case D3DRS_SPECULARMATERIALSOURCE:case D3DRS_EMISSIVEMATERIALSOURCE:return value<=D3DMCS_COLOR2;
    case D3DRS_AMBIENT:case D3DRS_TEXTUREFACTOR:return true;
    // Clip planes and vertex blending must not silently become no-ops.
    case D3DRS_VERTEXBLEND:case D3DRS_CLIPPLANEENABLE:return value==0;
    default:return false;
  }
}
inline bool ValidFFStage(unsigned stage,D3DTEXTURESTAGESTATETYPE key,uint32_t value) {
  if(stage>=8)return false;
  switch(key) {
    case D3DTSS_COLOROP:case D3DTSS_ALPHAOP:
      return value>=D3DTOP_DISABLE && value<=D3DTOP_LERP
        && value!=D3DTOP_BUMPENVMAP && value!=D3DTOP_BUMPENVMAPLUMINANCE;
    case D3DTSS_COLORARG0:case D3DTSS_COLORARG1:case D3DTSS_COLORARG2:
    case D3DTSS_ALPHAARG0:case D3DTSS_ALPHAARG1:case D3DTSS_ALPHAARG2:
      return !(value&~uint32_t(D3DTA_SELECTMASK|D3DTA_COMPLEMENT|D3DTA_ALPHAREPLICATE))
        && (value&D3DTA_SELECTMASK)<=D3DTA_TEMP;
    case D3DTSS_RESULTARG:return value==D3DTA_CURRENT||value==D3DTA_TEMP;
    case D3DTSS_TEXCOORDINDEX:return (value&0xffff)<8&&(value>>16)<=3;
    case D3DTSS_TEXTURETRANSFORMFLAGS:return value<=D3DTTFF_COUNT4; // Projected divide pending.
    default:return false;
  }
}
template<class F> inline void FFMaterialScalars(D3DMATERIAL9& m,F&& f) {
  for(auto* c:{&m.Diffuse,&m.Ambient,&m.Specular,&m.Emissive}){f(c->r);f(c->g);f(c->b);f(c->a);}
  f(m.Power);
}
template<class F> inline void FFLightScalars(D3DLIGHT9& light,F&& f) {
  for(auto* c:{&light.Diffuse,&light.Specular,&light.Ambient}){f(c->r);f(c->g);f(c->b);f(c->a);}
  for(auto* v:{&light.Position,&light.Direction}){f(v->x);f(v->y);f(v->z);}
  f(light.Range);f(light.Falloff);f(light.Attenuation0);f(light.Attenuation1);f(light.Attenuation2);f(light.Theta);f(light.Phi);
}
inline bool FiniteMaterial(D3DMATERIAL9 value) {
  bool ok=value.Power>=0;FFMaterialScalars(value,[&](float f){ok&=std::isfinite(f);});return ok;
}
inline bool ValidLight(D3DLIGHT9 value) {
  bool ok=value.Type>=D3DLIGHT_POINT&&value.Type<=D3DLIGHT_DIRECTIONAL;
  FFLightScalars(value,[&](float f){ok&=std::isfinite(f);});
  return ok&&value.Range>=0&&value.Attenuation0>=0&&value.Attenuation1>=0&&value.Attenuation2>=0;
}
inline bool ValidFixedFunction(const FixedFunctionState& state) {
  if(state.lightCount>8||!FiniteMaterial(state.material))return false;
  for(const auto& m:state.transforms)for(const auto& row:m.m)for(float v:row)if(!std::isfinite(v))return false;
  for(size_t i=0;i<FFRenderStates.size();++i)if(!ValidFFRender(FFRenderStates[i],state.render[i]))return false;
  for(unsigned s=0;s<8;++s)for(size_t i=0;i<FFStageStates.size();++i)
    if(!ValidFFStage(s,FFStageStates[i],state.stages[s][i]))return false;
  for(unsigned i=0;i<state.lightCount;++i)if(!ValidLight(state.lights[i]))return false;
  return true;
}
}
