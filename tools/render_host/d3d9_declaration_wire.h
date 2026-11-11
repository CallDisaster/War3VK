#pragma once
#include "d3d9_wire.h"
#include "../../src/d3d9/d3d9_fvf.h"

namespace warvk::host::d9 {
constexpr uint32_t MaxDeclarationElements = 16;
constexpr uint32_t DeclarationElementBytes = 8;
// Single-stream input contract. Packed UDEC3/DEC3N still need native unpacking;
// they are refused explicitly rather than interpreted as a different format.
inline bool ValidDeclaration(const D3DVERTEXELEMENT9* es, uint32_t count, uint32_t& extent) {
  if(!es || !count || count>MaxDeclarationElements)return false;
  uint32_t end=0;
  for(uint32_t i=0;i<count;++i) {
    const auto& e=es[i];
    const auto size=dxvk::DecltypeSize(D3DDECLTYPE(e.Type));
    if(e.Stream || e.Method!=D3DDECLMETHOD_DEFAULT || !size
      || e.Type==D3DDECLTYPE_UDEC3 || e.Type==D3DDECLTYPE_DEC3N
      || uint32_t(e.Offset)+size>256 || e.Offset%4
      || e.Usage>D3DDECLUSAGE_SAMPLE || e.UsageIndex>15)return false;
    for(uint32_t j=0;j<i;++j)
      if(e.Usage==es[j].Usage && e.UsageIndex==es[j].UsageIndex)return false;
    end=std::max(end,uint32_t(e.Offset)+size);
  }
  extent=end;return true;
}
inline void EncodeDeclarationElement(uint8_t* p,const D3DVERTEXELEMENT9& e) {
  ipc::WriteLe(p,e.Stream,2);ipc::WriteLe(p+2,e.Offset,2);
  p[4]=e.Type;p[5]=e.Method;p[6]=e.Usage;p[7]=e.UsageIndex;
}
struct DeclarationCommand {
  std::array<D3DVERTEXELEMENT9,MaxDeclarationElements> elements{};
  uint32_t count=0,extent=0;
};
inline bool DecodeDeclaration(const uint8_t* p,uint32_t bytes,DeclarationCommand& output) {
  if(!p || !bytes || bytes%DeclarationElementBytes || bytes>MaxDeclarationElements*DeclarationElementBytes)return false;
  DeclarationCommand candidate;candidate.count=bytes/DeclarationElementBytes;
  for(uint32_t i=0;i<candidate.count;++i) {
    const auto* q=p+i*DeclarationElementBytes;auto& e=candidate.elements[i];
    e={WORD(ipc::ReadLe(q,2)),WORD(ipc::ReadLe(q+2,2)),q[4],q[5],q[6],q[7]};
  }
  if(!ValidDeclaration(candidate.elements.data(),candidate.count,candidate.extent))return false;
  output=candidate;return true;
}
}
