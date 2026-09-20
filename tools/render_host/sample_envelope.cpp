#include "sample_envelope.h"
#include <cstring>

namespace warvk::host::envelope {
namespace {
Error Check(Header h) noexcept {
  if(!h.scope.valid())return Error::Scope;
  if(!h.sourceFrame)return Error::SourceFrame;
  if(!h.attempt||h.attempt>UINT32_MAX)return Error::Attempt;
  if(!h.transfer)return Error::Transfer;
  return Error::None;
}
bool Overlap(const void* a,size_t an,const void* b,size_t bn) noexcept {
  const auto x=reinterpret_cast<uintptr_t>(a),y=reinterpret_cast<uintptr_t>(b);
  if(an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y)return true;
  return x<y+bn&&y<x+an;
}
}
Error Encode(Header h,const uint8_t* payload,uint32_t bytes,uint8_t* out,size_t capacity,size_t& written) noexcept {
  written=0;
  if(!payload||!out)return Error::NullBuffer;
  if(!bytes||bytes>MaxPayloadBytes)return Error::Size;
  if(capacity<HeaderBytes+bytes)return Error::Capacity;
  const auto error=Check(h);if(error!=Error::None)return error;
  if(Overlap(payload,bytes,out,HeaderBytes+bytes))return Error::Overlap;
  std::memcpy(out,"WVP1",4);
  ipc::WriteLe(out+4,1,2);ipc::WriteLe(out+6,0,2);
  ipc::WriteLe(out+8,HeaderBytes,4);ipc::WriteLe(out+12,0,4);
  ipc::WriteLe(out+16,HeaderBytes+bytes,4);ipc::WriteLe(out+20,bytes,4);
  ipc::WriteLe(out+24,h.scope.connection.low,8);ipc::WriteLe(out+32,h.scope.connection.high,8);
  ipc::WriteLe(out+40,h.scope.map,8);ipc::WriteLe(out+48,h.scope.device,8);
  ipc::WriteLe(out+56,h.sourceFrame,8);ipc::WriteLe(out+64,h.attempt,8);ipc::WriteLe(out+72,h.transfer,8);
  std::memcpy(out+HeaderBytes,payload,bytes);written=HeaderBytes+bytes;return Error::None;
}
Error Decode(const uint8_t* in,size_t count,View& out) noexcept {
  out={};
  if(!in)return Error::NullBuffer;
  if(count<HeaderBytes||count>slots::SlotBytes)return Error::Size;
  if(std::memcmp(in,"WVP1",4))return Error::Magic;
  if(ipc::ReadLe(in+4,2)!=1||ipc::ReadLe(in+6,2)!=0)return Error::Version;
  if(ipc::ReadLe(in+8,4)!=HeaderBytes)return Error::Header;
  if(ipc::ReadLe(in+12,4)!=0)return Error::Flags;
  const auto bytes=ipc::ReadLe(in+20,4);
  if(!bytes||bytes>MaxPayloadBytes||ipc::ReadLe(in+16,4)!=count||HeaderBytes+bytes!=count)return Error::Size;
  Header h;
  h.scope={{ipc::ReadLe(in+24,8),ipc::ReadLe(in+32,8)},ipc::ReadLe(in+40,8),ipc::ReadLe(in+48,8)};
  h.sourceFrame=ipc::ReadLe(in+56,8);h.attempt=ipc::ReadLe(in+64,8);h.transfer=ipc::ReadLe(in+72,8);
  const auto error=Check(h);if(error!=Error::None)return error;
  out={h,in+HeaderBytes,uint32_t(bytes)};return Error::None;
}
Validator::Validator(inbox::Scope scope,Limits limits) noexcept:m_scope(scope),m_limits(limits) {
  if(!scope.valid())m_progress.fault=Error::Scope;
  else if(!limits.transfer||!limits.attempt)m_progress.fault=Error::Exhausted;
}
Error Validator::accept(const slots::Key& key,const uint8_t* bytes,size_t count,View& out) noexcept {
  out={};if(m_progress.fault!=Error::None)return Error::Terminal;
  View v;const auto decoded=Decode(bytes,count,v);if(decoded!=Error::None)return fail(decoded);
  if(!(v.header.scope==m_scope)||!(key.connection==m_scope.connection)||key.map!=m_scope.map||key.device!=m_scope.device)
    return fail(Error::Scope);
  if(!key.generation||key.slot>=slots::SlotCount||key.bytes!=count||key.frame!=v.header.transfer)
    return fail(Error::Lease);
  if(m_progress.transfer>=m_limits.transfer||v.header.attempt>m_limits.attempt)return fail(Error::Exhausted);
  if(v.header.transfer!=m_progress.transfer+1||v.header.attempt<=m_progress.attempt||v.header.sourceFrame<m_progress.sourceFrame)
    return fail(Error::Order);
  m_progress={v.header.sourceFrame,v.header.attempt,v.header.transfer,Error::None};out=v;return Error::None;
}
const char* ErrorName(Error e) noexcept {
  switch(e){
#define WV_NAME(x) case Error::x:return #x
    WV_NAME(None);WV_NAME(NullBuffer);WV_NAME(Size);WV_NAME(Capacity);WV_NAME(Overlap);WV_NAME(Magic);
    WV_NAME(Version);WV_NAME(Header);WV_NAME(Flags);WV_NAME(Scope);WV_NAME(SourceFrame);WV_NAME(Attempt);
    WV_NAME(Transfer);WV_NAME(Lease);WV_NAME(Order);WV_NAME(Exhausted);WV_NAME(Terminal);
#undef WV_NAME
  }return "Unknown";
}
}
