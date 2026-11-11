#pragma once
#include "../tools/render_host/d3d9_effects_wire.h"
#include "../tools/render_host/d3d9_clear_wire.h"
namespace warvk_effects_test {
template<class Check>void Run(Check check){
  namespace d9=warvk::host::d9;
  d9::EffectsState state,decoded;state.scissor={INT32_MIN,INT32_MAX,12,-3};
  const auto bytes=d9::EncodeEffects(state);
  check(d9::DecodeEffects(bytes.data(),bytes.size(),decoded)&&decoded==state);
  for(uint32_t n=0;n<bytes.size();++n){const auto before=decoded;
    check(!d9::DecodeEffects(bytes.data(),n,decoded));check(decoded==before);}
  check(!d9::DecodeEffects(nullptr,bytes.size(),decoded));
  for(unsigned index=0;index<d9::EffectKeys.size();++index)for(uint32_t value=0;value<256;++value){
    bool valid=false;
    switch(index){
      case 0:case 3:case 10:case 11:valid=value<2;break;
      case 1:valid=value>=1&&value<=8;break;
      case 2:case 4:case 7:case 8:case 9:case 15:valid=true;break;
      case 5:case 6:valid=value<4;break;
      case 12:case 13:valid=value>=1&&value<=11;break;
      case 14:valid=value>=1&&value<=5;break;
    }
    auto input=bytes;d9::P32(input.data()+index*4,value);const auto before=decoded;
    check(d9::ValidEffect(d9::EffectKeys[index],value)==valid);
    check(d9::DecodeEffects(input.data(),input.size(),decoded)==valid);
    if(!valid)check(decoded==before);
  }
  for(unsigned index:{7,8,9,15})for(uint32_t value:{0x7f800000u,0xff800000u,0x7fc00000u}){
    auto input=bytes;d9::P32(input.data()+index*4,value);const auto before=decoded;
    check(!d9::DecodeEffects(input.data(),input.size(),decoded));check(decoded==before);
  }
  const D3DVIEWPORT9 viewport{8,10,40,36,0,1};
  const std::array<D3DRECT,8> rectangles={{{INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX},{12,14,30,38},
    {0,0,8,10},{50,50,64,64},{20,20,20,30},{32,40,16,15},{-5,18,64,24},{0,0,64,64}}};
  for(const auto& clear:rectangles)for(const auto& scissor:rectangles){
    const auto first=d9::IntersectClear(viewport,clear);
    const D3DVIEWPORT9 region{first.x,first.y,first.width,first.height,0,1};
    const auto clipped=d9::IntersectClear(region,scissor);
    for(int y=0;y<64;++y)for(int x=0;x<64;++x){
      const bool expected=x>=8&&x<48&&y>=10&&y<46&&x>=clear.x1&&x<clear.x2&&y>=clear.y1&&y<clear.y2
        &&x>=scissor.x1&&x<scissor.x2&&y>=scissor.y1&&y<scissor.y2;
      const bool actual=uint32_t(x)>=clipped.x&&uint32_t(x)<clipped.x+clipped.width
        &&uint32_t(y)>=clipped.y&&uint32_t(y)<clipped.y+clipped.height;
      check(actual==expected);
    }
  }
}
}
