#pragma once
#include "d3d9_wire.h"
#include <d3d9.h>
#include <cmath>

namespace warvk::host::d9 {
// Resolved viewport intersections, owned by the call. No application pointer,
// signed coordinate or partially decoded prefix crosses the wire.
constexpr uint32_t MaxClearRegions=64, ClearRegionBytes=16;
struct ClearRegion { uint32_t x=0,y=0,width=0,height=0; };
struct ClearRegions {
  uint32_t color=0,flags=0,count=0;
  float depth=1;
  std::array<ClearRegion,MaxClearRegions> regions{};
};
inline bool ValidClearRegion(const ClearRegion& r,uint32_t width,uint32_t height) {
  return r.x<width&&r.y<height&&r.width&&r.height&&r.width<=width-r.x&&r.height<=height-r.y;
}
inline ClearRegion IntersectClear(const D3DVIEWPORT9& v,const D3DRECT& r) {
  const auto x=std::max<int64_t>(v.X,r.x1),y=std::max<int64_t>(v.Y,r.y1);
  const auto right=std::min<int64_t>(uint64_t(v.X)+v.Width,r.x2);
  const auto bottom=std::min<int64_t>(uint64_t(v.Y)+v.Height,r.y2);
  if(right<=x||bottom<=y)return {};
  return {uint32_t(x),uint32_t(y),uint32_t(right-x),uint32_t(bottom-y)};
}
inline void EncodeClearRegion(uint8_t* p,const ClearRegion& r) {
  P32(p,r.x);P32(p+4,r.y);P32(p+8,r.width);P32(p+12,r.height);
}
// Regions are validated against the negotiated target extent.
inline bool DecodeClearRegions(const Header& h,const uint8_t* p,ClearRegions& result,uint32_t width,uint32_t height) {
  if(h.op!=Op::ClearRegions||h.id||h.generation||!h.args[3]||h.args[3]>MaxClearRegions
    ||!p||h.payloadBytes!=h.args[3]*ClearRegionBytes||!h.args[2]
    ||(h.args[2]&~uint32_t(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER)))return false;
  for(size_t i=4;i<h.args.size();++i)if(h.args[i])return false;
  ClearRegions candidate;candidate.color=h.args[0];candidate.depth=Float(h.args[1]);
  candidate.flags=h.args[2];candidate.count=h.args[3];
  if(!std::isfinite(candidate.depth)||candidate.depth<0||candidate.depth>1)return false;
  for(uint32_t i=0;i<candidate.count;++i) {
    const auto* at=p+i*ClearRegionBytes;
    auto& r=candidate.regions[i];r={U32(at),U32(at+4),U32(at+8),U32(at+12)};
    if(!ValidClearRegion(r,width,height))return false;
  }
  result=candidate;return true;
}
} // namespace warvk::host::d9
