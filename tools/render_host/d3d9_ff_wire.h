#pragma once
#include "d3d9_draw_wire.h"
#include "d3d9_ff_state.h"
namespace warvk::host::d9 {
constexpr uint32_t FixedStateWords=11*16+14+8*11+17+1+8*26;
constexpr uint32_t FixedDrawBytes=DrawBytes+FixedStateWords*4;
using FixedDrawPacket=std::array<uint8_t,FixedDrawBytes>;
// DrawFixedBatch is count(args[0]) independent FixedDrawPackets, each carrying
// its own complete fixed-function state. No delta against an earlier packet.
constexpr uint32_t MaxFixedDrawBatch=dxvkcore::D3D9Backend::MaxFixedDrawBatch;
using FixedDrawBatchPacket=std::array<uint8_t,FixedDrawBytes*MaxFixedDrawBatch>;
static_assert(FixedDrawBytes*MaxFixedDrawBatch<=MaxPayload,"fixed draw batch must fit one bounded packet");
inline FixedDrawPacket EncodeFixedDraw(const DrawCommand& draw,const FixedFunctionState& state) {
  FixedDrawPacket output{};const auto base=EncodeDraw(draw);
  std::copy(base.begin(),base.end(),output.begin());size_t pos=DrawBytes;
  const auto u=[&](uint32_t v){P32(output.data()+pos,v);pos+=4;};
  const auto f=[&](float v){u(FloatBits(v));};
  for(const auto& m:state.transforms)for(const auto& row:m.m)for(float v:row)f(v);
  for(auto v:state.render)u(v);
  for(const auto& stage:state.stages)for(auto v:stage)u(v);
  auto material=state.material;FFMaterialScalars(material,f);u(state.lightCount);
  for(unsigned i=0;i<8;++i) {
    auto light=i<state.lightCount?state.lights[i]:D3DLIGHT9{};
    u(uint32_t(light.Type));FFLightScalars(light,f);
  }
  return output;
}
inline bool DecodeFixedDraw(const uint8_t* bytes,uint32_t size,DrawCommand& draw,FixedFunctionState& state) {
  if(!bytes||size!=FixedDrawBytes)return false;
  DrawCommand candidate;
  if(!DecodeDraw(bytes,DrawBytes,candidate,true)||(candidate.resources[3].id&&candidate.resources[4].id))return false;
  size_t pos=DrawBytes;const auto u=[&](){auto v=U32(bytes+pos);pos+=4;return v;};
  const auto f=[&](float& v){v=Float(u());};
  FixedFunctionState data;
  for(auto& m:data.transforms)for(auto& row:m.m)for(float& v:row)f(v);
  for(auto& v:data.render)v=u();
  for(auto& stage:data.stages)for(auto& v:stage)v=u();
  FFMaterialScalars(data.material,f);data.lightCount=u();if(data.lightCount>8)return false;
  for(unsigned i=0;i<8;++i) {
    auto& light=data.lights[i];const auto start=pos;
    light.Type=D3DLIGHTTYPE(u());FFLightScalars(light,f);
    if(i>=data.lightCount)for(size_t p=start;p<pos;++p)if(bytes[p])return false;
  }
  if(pos!=size||!ValidFixedFunction(data))return false;
  draw=candidate;state=data;return true;
}
}
