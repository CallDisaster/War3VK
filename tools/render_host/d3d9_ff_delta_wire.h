#pragma once
#include "d3d9_ff_wire.h"
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace warvk::host::d9 {
constexpr uint32_t FixedDrawWords=FixedDrawBytes/4,FixedDeltaBlocks=(FixedDrawWords+31)/32;
static_assert(FixedDrawBytes==3180&&FixedDeltaBlocks==25&&FixedDrawWords%32==27);
inline constexpr uint32_t FixedDeltaRecordBound(bool verify) {
  return FixedDrawBytes+4*(1+FixedDeltaBlocks)+(verify?8:0);
}
// Reserve one worst-case record before admitting its annotations. Dense deltas
// can be larger than the original records; the transport limit still applies.
inline bool FixedDeltaHasRoom(uint32_t used,bool verify) {
  return used<=MaxPayload-FixedDeltaRecordBound(verify);
}
// Bit w is set when 32-bit word w of a and b differ (words<=32). No branch
// depends on the data: on the x86 game thread a branch per word cost about
// 0.9 us per draw, more than the full record's two copies.
inline uint32_t FixedDeltaDirtyWords(const uint8_t* a,const uint8_t* b,uint32_t words) {
  uint32_t mask=0,w=0;
#if defined(__SSE2__)
  for(;w+4<=words;w+=4) {
    const auto same=_mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+w*4)),
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(b+w*4)));
    mask|=(uint32_t(_mm_movemask_ps(_mm_castsi128_ps(same)))^15u)<<w;
  }
#endif
  for(;w<words;++w)mask|=uint32_t(U32(a+w*4)!=U32(b+w*4))<<w;
  return mask;
}
// The caller zeros previous at the start of each message. Only dirty words
// are written back to it, so an identical draw writes just its block mask.
inline uint32_t EncodeFixedDeltaRecord(const FixedDrawPacket& full,FixedDrawPacket& previous,
    uint8_t* out,uint32_t capacity,bool verify) {
  if(!out||capacity<FixedDeltaRecordBound(verify))return 0;
  uint32_t blocks=0,at=4;
  for(uint32_t b=0;b<FixedDeltaBlocks;++b) {
    const auto words=std::min(32u,FixedDrawWords-b*32);
    const auto* src=full.data()+b*128;auto* base=previous.data()+b*128;
    auto mask=FixedDeltaDirtyWords(src,base,words);
    if(!mask)continue;
    blocks|=1u<<b;P32(out+at,mask);at+=4;
    for(;mask;mask&=mask-1) {  // dirty words only, ascending
      const auto offset=uint32_t(__builtin_ctz(mask))*4;
      std::memcpy(out+at,src+offset,4);at+=4;std::memcpy(base+offset,src+offset,4);
    }
  }
  P32(out,blocks);
  if(verify){P64(out+at,FoldFnv1a64(full.data(),full.size()));at+=8;}
  return at;
}

enum class FixedDeltaResult { Valid,Invalid,HashMismatch };
// stage may validate into private batch scratch only. Its contents may be
// consumed only after Valid, including the final exact-length check.
template<class Stage>
FixedDeltaResult StageFixedDeltaBatch(const Header& h,const uint8_t* p,Stage&& stage) {
  if(h.op!=Op::DrawFixedDeltaBatch||h.id||h.generation||!p||h.payloadBytes>MaxPayload
    ||!h.args[0]||h.args[0]>MaxFixedDrawBatch||h.args[1]>1)return FixedDeltaResult::Invalid;
  for(size_t i=2;i<h.args.size();++i)if(h.args[i])return FixedDeltaResult::Invalid;
  FixedDrawPacket full{};uint32_t at=0;
  for(uint32_t i=0;i<h.args[0];++i) {
    if(h.payloadBytes-at<4)return FixedDeltaResult::Invalid;
    const auto blocks=U32(p+at);at+=4;
    if(blocks>>FixedDeltaBlocks)return FixedDeltaResult::Invalid;
    for(uint32_t b=0;b<FixedDeltaBlocks;++b) {
      if(!(blocks&(1u<<b)))continue;
      if(h.payloadBytes-at<4)return FixedDeltaResult::Invalid;
      auto words=U32(p+at);at+=4;
      if(!words||(b+1==FixedDeltaBlocks&&words>>(FixedDrawWords%32)))return FixedDeltaResult::Invalid;
      for(uint32_t w=0;words;++w,words>>=1) {
        if(!(words&1))continue;
        if(h.payloadBytes-at<4)return FixedDeltaResult::Invalid;
        P32(full.data()+(b*32+w)*4,U32(p+at));at+=4;
      }
    }
    if(h.args[1]) {
      if(h.payloadBytes-at<8)return FixedDeltaResult::Invalid;
      if(U64(p+at)!=FoldFnv1a64(full.data(),full.size()))return FixedDeltaResult::HashMismatch;
      at+=8;
    }
    if(!stage(i,full.data()))return FixedDeltaResult::Invalid;
  }
  return at==h.payloadBytes?FixedDeltaResult::Valid:FixedDeltaResult::Invalid;
}
}
