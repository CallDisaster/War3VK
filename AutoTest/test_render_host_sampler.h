#pragma once
#include "../tools/render_host/d3d9_sampler_wire.h"
namespace warvk_sampler_test {
template<class Check>void Run(Check check) {
  namespace d9=warvk::host::d9;
  d9::SamplerState state,decoded(true);const auto bytes=d9::EncodeSamplerState(state);
  check(d9::ValidSamplerState(state)&&state.values[D3DSAMP_ADDRESSU]==D3DTADDRESS_WRAP);
  check(d9::DecodeSamplerState(bytes.data(),bytes.size(),decoded)&&decoded.values==state.values);
  for(uint32_t size=0;size<bytes.size();++size) {
    const auto saved=decoded.values;check(!d9::DecodeSamplerState(bytes.data(),size,decoded));check(saved==decoded.values);
  }
  check(!d9::DecodeSamplerState(nullptr,bytes.size(),decoded));
  for(uint32_t key=0;key<d9::SamplerStateWords;++key)for(uint32_t value=0;value<256;++value) {
    bool expected=false;
    switch(key) {
      case 1:case 2:case 3:expected=value>=1&&value<=5;break;
      case 4:case 8:case 9:case 10:expected=true;break;
      case 5:case 6:expected=value>=1&&value<=3;break;
      case 7:expected=value<=2;break;
      case 11:expected=value<=1;break;
      case 12:case 13:expected=!value;break;
    }
    check(d9::ValidSamplerValue(key,value)==expected);
    auto changed=bytes;d9::P32(changed.data()+key*4,value);const auto saved=decoded.values;
    const auto accepted=d9::DecodeSamplerState(changed.data(),changed.size(),decoded);
    check(accepted==(key?expected:value==0));
    if(!accepted)check(decoded.values==saved);
  }
  for(uint32_t bits:{0x7f800000u,0xff800000u,0x7fc00000u,0xffffffffu}) {
    check(!d9::ValidSamplerValue(D3DSAMP_MIPMAPLODBIAS,bits));
    auto bad=bytes;d9::P32(bad.data()+8*4,bits);const auto saved=decoded.values;
    check(!d9::DecodeSamplerState(bad.data(),bad.size(),decoded));check(decoded.values==saved);
  }
}
}
