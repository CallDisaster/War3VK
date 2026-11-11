#pragma once
#include "d3d9_sampler_wire.h"
namespace warvk::host::d9 {
constexpr uint32_t PixelTextureStages=8,AdditionalTextureStages=PixelTextureStages-1;
// Stage zero retains its existing draw reference and sampler message. This
// ordered extension is a complete transaction, never seven partial setters.
struct TextureStages {
  std::array<ResourceRef,AdditionalTextureStages> textures{};
  std::array<SamplerState,AdditionalTextureStages> samplers{};
};
constexpr uint32_t TextureStageBytes=8+SamplerStateBytes;
constexpr uint32_t TextureStagesBytes=AdditionalTextureStages*TextureStageBytes;
using TextureStagesPacket=std::array<uint8_t,TextureStagesBytes>;
inline TextureStagesPacket EncodeTextureStages(const TextureStages& state) {
  TextureStagesPacket bytes{};
  for(uint32_t i=0;i<AdditionalTextureStages;++i) {
    auto* p=bytes.data()+i*TextureStageBytes;
    P32(p,state.textures[i].id);P32(p+4,state.textures[i].generation);
    const auto sampler=EncodeSamplerState(state.samplers[i]);
    std::copy(sampler.begin(),sampler.end(),p+8);
  }
  return bytes;
}
inline bool DecodeTextureStages(const uint8_t* bytes,uint32_t size,TextureStages& output) {
  if(!bytes||size!=TextureStagesBytes)return false;
  TextureStages state;
  for(uint32_t i=0;i<AdditionalTextureStages;++i) {
    const auto* p=bytes+i*TextureStageBytes;auto& ref=state.textures[i];
    ref={U32(p),U32(p+4)};
    if(!ValidRef(ref)&&(ref.id||ref.generation))return false;
    if(!DecodeSamplerState(p+8,SamplerStateBytes,state.samplers[i]))return false;
  }
  output=state;return true;
}
}
