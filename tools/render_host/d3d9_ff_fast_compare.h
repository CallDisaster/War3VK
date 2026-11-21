#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
namespace warvk::host::d9 {
// Whether two sections of a fixed-function packet hold the same bytes. The sections are 56 to 512
// bytes and compared once or more per draw; the 32-bit C runtime's memcmp took 4% of the game thread
// here, a call and a byte loop per section, where sixteen bytes at a time take a fraction of that.
inline bool FFSectionEqual(const void* a,const void* b,size_t bytes) noexcept {
  const auto* x=static_cast<const uint8_t*>(a);const auto* y=static_cast<const uint8_t*>(b);
  size_t i=0;
#if defined(__SSE2__)
  for(;i+16<=bytes;i+=16)
    if(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x+i)),
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(y+i))))!=0xffff)return false;
#endif
  for(;i+4<=bytes;i+=4) {
    uint32_t p,q;std::memcpy(&p,x+i,4);std::memcpy(&q,y+i,4);
    if(p!=q)return false;
  }
  for(;i<bytes;++i)if(x[i]!=y[i])return false;
  return true;
}
}
