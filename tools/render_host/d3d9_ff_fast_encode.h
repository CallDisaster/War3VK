#pragma once
#include "d3d9_ff_wire.h"
#include "d3d9_ff_fast_compare.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "FixedDrawEncoder copies object representations as the wire's little-endian words"
#endif
namespace warvk::host::d9 {
// EncodeFixedDraw's packet and ValidFixedFunction's verdict for a stream of draws, at the cost of what
// changed since the previous draw rather than of the whole 3180-byte packet and its 795 checks.
//
// The packet is kept between draws. On a little-endian machine the wire form of every section is the
// object representation of the matching member, in the same order (asserted below, and compared byte
// for byte against EncodeFixedDraw in AutoTest/test_render_host_ff_fast_encode.cpp), so a section is
// current exactly when its bytes equal the member's. The one exception is the lights from lightCount
// on, which the wire carries as zero whatever the array holds.
class FixedDrawEncoder {
public:
  const FixedDrawPacket& encode(const DrawCommand& draw,const FixedFunctionState& state) noexcept {
    uint8_t* const out=m_packet.data();
    const bool fresh=!m_primed;m_primed=true;
    // The draw's own words always differ from the last draw's; its constants rarely do.
    size_t word=0;
    const auto u=[&](uint32_t n){std::memcpy(out+4*word++,&n,4);};
    for(const auto ref:draw.resources){u(ref.id);u(ref.generation);}
    const auto& s=draw.state;
    u(s.streamOffset);u(s.stride);
    u(s.viewport.X);u(s.viewport.Y);u(s.viewport.Width);u(s.viewport.Height);
    u(FloatBits(s.viewport.MinZ));u(FloatBits(s.viewport.MaxZ));
    u(s.render.zEnable);u(s.render.zWriteEnable);u(s.render.zFunc);u(s.render.cullMode);
    u(s.render.alphaBlendEnable);u(s.render.srcBlend);u(s.render.destBlend);u(s.render.blendOp);u(s.render.colorWriteMask);
    u(uint32_t(s.baseVertexIndex));u(s.minVertexIndex);u(s.numVertices);u(s.startIndex);u(s.primitiveCount);
    u(uint32_t(s.primitiveType));
    static_assert(HeadWords*4+sizeof(s.vertexFloatConstants)+sizeof(s.pixelFloatConstants)==DrawBytes);
    Same(out+HeadWords*4,s.vertexFloatConstants.data(),sizeof(s.vertexFloatConstants));
    Same(out+HeadWords*4+sizeof(s.vertexFloatConstants),s.pixelFloatConstants.data(),sizeof(s.pixelFloatConstants));

    for(unsigned i=0;i<11;++i)if(!Same(out+Transforms+i*64,&state.transforms[i],64)||fresh) {
      bool finite=true;
      for(const auto& row:state.transforms[i].m)for(float v:row)finite&=std::isfinite(v);
      m_transformsFinite=uint16_t(finite?m_transformsFinite|(1u<<i):m_transformsFinite&~(1u<<i));
    }
    if(!Same(out+Render,state.render.data(),sizeof(state.render))||fresh) {
      m_renderValid=true;
      for(size_t i=0;i<FFRenderStates.size();++i)m_renderValid&=ValidFFRender(FFRenderStates[i],state.render[i]);
    }
    if(!Same(out+Stages,state.stages.data(),sizeof(state.stages))||fresh) {
      m_stagesValid=true;
      for(unsigned stage=0;stage<8;++stage)for(size_t i=0;i<FFStageStates.size();++i)
        m_stagesValid&=ValidFFStage(stage,FFStageStates[i],state.stages[stage][i]);
    }
    if(!Same(out+Material,&state.material,sizeof(state.material))||fresh)m_materialValid=FiniteMaterial(state.material);
    std::memcpy(out+LightCount,&state.lightCount,4);
    const uint32_t lights=std::min<uint32_t>(state.lightCount,8);
    for(uint32_t i=0;i<lights;++i)if(!Same(out+Lights+i*LightBytes,&state.lights[i],LightBytes)||fresh||i>=m_lights)
      m_lightsValid=uint8_t(ValidLight(state.lights[i])?m_lightsValid|(1u<<i):m_lightsValid&~(1u<<i));
    if(lights<m_lights)std::memset(out+Lights+lights*LightBytes,0,(m_lights-lights)*LightBytes);
    m_lights=lights;m_lightCount=state.lightCount;
    return m_packet;
  }
  // ValidFixedFunction of the state last encoded.
  bool valid() const noexcept {
    const uint32_t used=(1u<<m_lights)-1;
    return m_primed&&m_lightCount<=8&&m_materialValid&&m_transformsFinite==0x7ffu&&m_renderValid&&m_stagesValid
      &&(m_lightsValid&used)==used;
  }
private:
  static constexpr size_t HeadWords=12+2+6+9+6,LightBytes=sizeof(D3DLIGHT9);
  static constexpr size_t Transforms=DrawBytes,Render=Transforms+11*64,Stages=Render+14*4,Material=Stages+8*11*4,
    LightCount=Material+17*4,Lights=LightCount+4;
  static_assert(Lights+8*LightBytes==FixedDrawBytes);
  static_assert(sizeof(ResourceRef)==8&&sizeof(D3DMATRIX)==64&&sizeof(D3DMATERIAL9)==17*4&&sizeof(D3DLIGHT9)==26*4);
  static_assert(sizeof(FixedFunctionState)==FixedStateWords*4);
  static_assert(offsetof(D3DMATERIAL9,Diffuse)==0&&offsetof(D3DMATERIAL9,Ambient)==16&&offsetof(D3DMATERIAL9,Specular)==32
    &&offsetof(D3DMATERIAL9,Emissive)==48&&offsetof(D3DMATERIAL9,Power)==64);
  static_assert(offsetof(D3DLIGHT9,Type)==0&&offsetof(D3DLIGHT9,Diffuse)==4&&offsetof(D3DLIGHT9,Specular)==20
    &&offsetof(D3DLIGHT9,Ambient)==36&&offsetof(D3DLIGHT9,Position)==52&&offsetof(D3DLIGHT9,Direction)==64
    &&offsetof(D3DLIGHT9,Range)==76&&offsetof(D3DLIGHT9,Falloff)==80&&offsetof(D3DLIGHT9,Attenuation0)==84
    &&offsetof(D3DLIGHT9,Attenuation1)==88&&offsetof(D3DLIGHT9,Attenuation2)==92&&offsetof(D3DLIGHT9,Theta)==96
    &&offsetof(D3DLIGHT9,Phi)==100);
  // True when the section already held the member's bytes; brings it up to date otherwise.
  static bool Same(uint8_t* section,const void* member,size_t bytes) noexcept {
    if(FFSectionEqual(section,member,bytes))return true;
    std::memcpy(section,member,bytes);return false;
  }
  FixedDrawPacket m_packet{};
  bool m_primed=false,m_renderValid=false,m_stagesValid=false,m_materialValid=false;
  uint16_t m_transformsFinite=0;
  // Bit i: the light in slot i is a valid one. Meaningful below m_lights, the slots that hold a light.
  uint8_t m_lightsValid=0;
  uint32_t m_lights=0,m_lightCount=0;
};
}
