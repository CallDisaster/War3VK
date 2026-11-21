#pragma once
#include "d3d9_ff_wire.h"
#include "d3d9_ff_fast_compare.h"
#include <cstddef>
#include <cstring>
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "FixedDrawDecoder copies the wire's little-endian words as object representations"
#endif
namespace warvk::host::d9 {
// DecodeFixedDraw for a stream of packets, at the cost of what changed since the previous packet.
//
// The draw half goes through DecodeDraw as before. The fixed-function half is kept decoded: on a
// little-endian machine a section of the wire is the object representation of the matching member
// (see d3d9_ff_fast_encode.h), so a section whose bytes equal the kept member's needs neither decoding
// nor checking again, and one that differs is copied and checked with the same functions
// ValidFixedFunction uses. Result and outputs equal DecodeFixedDraw's for every input
// (AutoTest/test_render_host_ff_fast_encode.cpp, including damaged packets).
class FixedDrawDecoder {
public:
  bool decode(const uint8_t* bytes,DrawCommand& draw,FixedFunctionState& state) noexcept {
    if(!bytes)return false;
    DrawCommand candidate;
    if(!DecodeDraw(bytes,DrawBytes,candidate,true)||(candidate.resources[3].id&&candidate.resources[4].id))return false;
    const uint8_t* const in=bytes+DrawBytes;
    const bool fresh=!m_primed;m_primed=true;
    for(unsigned i=0;i<11;++i)if(!Same(&m_state.transforms[i],in+i*64,64)||fresh) {
      bool finite=true;
      for(const auto& row:m_state.transforms[i].m)for(float v:row)finite&=std::isfinite(v);
      m_transformsFinite=uint16_t(finite?m_transformsFinite|(1u<<i):m_transformsFinite&~(1u<<i));
    }
    if(!Same(m_state.render.data(),in+Render,sizeof(m_state.render))||fresh) {
      m_renderValid=true;
      for(size_t i=0;i<FFRenderStates.size();++i)m_renderValid&=ValidFFRender(FFRenderStates[i],m_state.render[i]);
    }
    if(!Same(m_state.stages.data(),in+Stages,sizeof(m_state.stages))||fresh) {
      m_stagesValid=true;
      for(unsigned stage=0;stage<8;++stage)for(size_t i=0;i<FFStageStates.size();++i)
        m_stagesValid&=ValidFFStage(stage,FFStageStates[i],m_state.stages[stage][i]);
    }
    if(!Same(&m_state.material,in+Material,sizeof(m_state.material))||fresh)m_materialValid=FiniteMaterial(m_state.material);
    std::memcpy(&m_state.lightCount,in+LightCount,4);
    for(unsigned i=0;i<8;++i)if(!Same(&m_state.lights[i],in+Lights+i*LightBytes,LightBytes)||fresh) {
      static const uint8_t zero[LightBytes]{};
      const bool valid=ValidLight(m_state.lights[i]),empty=FFSectionEqual(&m_state.lights[i],zero,LightBytes);
      m_lightsValid=uint8_t(valid?m_lightsValid|(1u<<i):m_lightsValid&~(1u<<i));
      m_lightsEmpty=uint8_t(empty?m_lightsEmpty|(1u<<i):m_lightsEmpty&~(1u<<i));
    }
    // A light in use is a valid one; a slot past the count carries nothing.
    if(m_state.lightCount>8)return false;
    const uint32_t used=(1u<<m_state.lightCount)-1,unused=0xffu&~used;
    if((m_lightsValid&used)!=used||(m_lightsEmpty&unused)!=unused)return false;
    if(!m_materialValid||m_transformsFinite!=0x7ffu||!m_renderValid||!m_stagesValid)return false;
    draw=candidate;state=m_state;return true;
  }
private:
  static constexpr size_t LightBytes=sizeof(D3DLIGHT9);
  // Offsets inside the fixed-function half.
  static constexpr size_t Render=11*64,Stages=Render+14*4,Material=Stages+8*11*4,LightCount=Material+17*4,Lights=LightCount+4;
  static_assert(Lights+8*LightBytes==FixedStateWords*4&&sizeof(FixedFunctionState)==FixedStateWords*4);
  static_assert(offsetof(FixedFunctionState,render)==Render&&offsetof(FixedFunctionState,stages)==Stages
    &&offsetof(FixedFunctionState,material)==Material&&offsetof(FixedFunctionState,lightCount)==LightCount
    &&offsetof(FixedFunctionState,lights)==Lights);
  // True when the member already held the section's bytes; brings it up to date otherwise.
  static bool Same(void* member,const uint8_t* section,size_t bytes) noexcept {
    if(FFSectionEqual(member,section,bytes))return true;
    std::memcpy(member,section,bytes);return false;
  }
  FixedFunctionState m_state;
  bool m_primed=false,m_renderValid=false,m_stagesValid=false,m_materialValid=false;
  uint16_t m_transformsFinite=0;
  uint8_t m_lightsValid=0,m_lightsEmpty=0;
};
}
