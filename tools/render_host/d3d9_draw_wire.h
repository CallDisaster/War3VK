#pragma once
#include "d3d9_wire.h"
#include "dxvk_core/d3d9_backend.h"

namespace warvk::host::d9 {
// Order: VB, IB, texture, VS, PS, declaration. The backend receives only typed
// slots after the host validates each complete reference. A canonical empty
// IB selects nonindexed drawing; an empty texture uses native null sampling.
// Vertex/pixel shaders are optional only in the fixed-function envelope.
// Backend numeric slots are intentionally not serialized from state.
struct DrawCommand {
  std::array<ResourceRef,6> resources{};
  dxvkcore::D3D9Backend::DrawState state{};
};
constexpr uint32_t DrawWords=14+6+9+6+256, DrawBytes=DrawWords*4;
using DrawPacket=std::array<uint8_t,DrawBytes>;
constexpr uint32_t MaxDrawBatch = dxvkcore::D3D9Backend::MaxDrawBatch;
using DrawBatchPacket = std::array<uint8_t, DrawBytes * MaxDrawBatch>;
static_assert(DrawBytes * MaxDrawBatch <= MaxPayload, "draw batch must fit one bounded packet");
inline DrawPacket EncodeDraw(const DrawCommand& command) {
  const auto& s=command.state;
  DrawPacket p{};size_t i=0;
  auto u=[&](uint32_t n){P32(p.data()+4*i++,n);};
  for(const auto ref:command.resources){u(ref.id);u(ref.generation);}
  u(s.streamOffset);u(s.stride);
  u(s.viewport.X);u(s.viewport.Y);u(s.viewport.Width);u(s.viewport.Height);u(FloatBits(s.viewport.MinZ));u(FloatBits(s.viewport.MaxZ));
  u(s.render.zEnable);u(s.render.zWriteEnable);u(s.render.zFunc);u(s.render.cullMode);
  u(s.render.alphaBlendEnable);u(s.render.srcBlend);u(s.render.destBlend);u(s.render.blendOp);u(s.render.colorWriteMask);
  u(uint32_t(s.baseVertexIndex));u(s.minVertexIndex);u(s.numVertices);u(s.startIndex);u(s.primitiveCount);u(uint32_t(s.primitiveType));
  for(float f:s.vertexFloatConstants)u(FloatBits(f));
  for(float f:s.pixelFloatConstants)u(FloatBits(f));
  return p;
}
inline bool DecodeDraw(const uint8_t* p,uint32_t bytes,DrawCommand& command,bool fixedFunction=false) {
  if(!p||bytes!=DrawBytes)return false;
  size_t i=0;auto u=[&](){return U32(p+4*i++);};
  DrawCommand decoded;auto& s=decoded.state;
  for(size_t slot=0;slot<decoded.resources.size();++slot){auto& ref=decoded.resources[slot];ref={u(),u()};
    // Empty means exactly {0,0}; half-null and stale keys never become wildcards.
    if(!ValidRef(ref)&&!((slot==1||slot==2||(fixedFunction&&slot>=3&&slot<=4))&&!ref.id&&!ref.generation))return false;}
  s.streamOffset=u();s.stride=u();
  s.viewport.X=u();s.viewport.Y=u();s.viewport.Width=u();s.viewport.Height=u();s.viewport.MinZ=Float(u());s.viewport.MaxZ=Float(u());
  s.render.zEnable=u();s.render.zWriteEnable=u();s.render.zFunc=u();s.render.cullMode=u();
  s.render.alphaBlendEnable=u();s.render.srcBlend=u();s.render.destBlend=u();s.render.blendOp=u();s.render.colorWriteMask=u();
  s.baseVertexIndex=int32_t(u());s.minVertexIndex=u();s.numVertices=u();s.startIndex=u();s.primitiveCount=u();s.primitiveType=D3DPRIMITIVETYPE(u());
  for(float& f:s.vertexFloatConstants)f=Float(u());
  for(float& f:s.pixelFloatConstants)f=Float(u());
  if(i!=DrawWords)return false;command=decoded;return true;
}
} // namespace warvk::host::d9
