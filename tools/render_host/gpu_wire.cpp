#include "gpu_wire.h"
#include <cstring>

namespace warvk::host::gpuwire {
const char* ErrorName(Error e) noexcept {
  switch(e) {
    case Error::None:return "None";case Error::Encoding:return "Encoding";
    case Error::Nonce:return "Nonce";case Error::Sequence:return "Sequence";
    case Error::State:return "State";case Error::Scope:return "Scope";
    case Error::Payload:return "Payload";case Error::Frame:return "Frame";
    case Error::Bits:return "Bits";case Error::Terminal:return "Terminal";
  }
  return "Unknown";
}
Error DecodeHeader(const uint8_t* p,size_t n,Header& result) noexcept {
  result={};
  if(!p||n<HeaderBytes||std::memcmp(p,"WVG2",4)||ipc::ReadLe(p+4,2)!=1||
      ipc::ReadLe(p+6,2)!=0||ipc::ReadLe(p+8,2)!=HeaderBytes||
      ipc::ReadLe(p+10,2)<1||ipc::ReadLe(p+10,2)>6||ipc::ReadLe(p+12,4)>1||
      ipc::ReadLe(p+16,4)>MaxPayload||ipc::ReadLe(p+20,4)||ipc::ReadLe(p+88,8))
    return Error::Encoding;
  Header h;h.op=Op(ipc::ReadLe(p+10,2));h.flags=uint32_t(ipc::ReadLe(p+12,4));
  h.payloadBytes=uint32_t(ipc::ReadLe(p+16,4));
  h.nonce={ipc::ReadLe(p+24,8),ipc::ReadLe(p+32,8)};
  h.sequence=ipc::ReadLe(p+40,8);h.mapEpoch=ipc::ReadLe(p+48,8);
  h.deviceEpoch=ipc::ReadLe(p+56,8);h.resourceId=ipc::ReadLe(p+64,8);
  h.generation=ipc::ReadLe(p+72,8);h.frame=ipc::ReadLe(p+80,8);
  if(h.nonce.empty()||!h.sequence)return Error::Encoding;
  result=h;return Error::None;
}
Error Decode(const uint8_t* p,size_t n,View& out) noexcept {
  out={};Header h;auto e=DecodeHeader(p,n,h);
  if(e!=Error::None)return e;
  if(n!=HeaderBytes+h.payloadBytes)return Error::Encoding;
  out={h,p+HeaderBytes};return Error::None;
}
Error Encode(const Header& h,const uint8_t* payload,uint8_t* out,size_t capacity,size_t& n) noexcept {
  n=0;
  if(!out||h.payloadBytes>MaxPayload||capacity<HeaderBytes+h.payloadBytes||
      (h.payloadBytes&&!payload))return Error::Encoding;
  std::memset(out,0,HeaderBytes);std::memcpy(out,"WVG2",4);
  ipc::WriteLe(out+4,1,2);ipc::WriteLe(out+8,HeaderBytes,2);
  ipc::WriteLe(out+10,uint16_t(h.op),2);ipc::WriteLe(out+12,h.flags,4);
  ipc::WriteLe(out+16,h.payloadBytes,4);ipc::WriteLe(out+24,h.nonce.low,8);
  ipc::WriteLe(out+32,h.nonce.high,8);ipc::WriteLe(out+40,h.sequence,8);
  ipc::WriteLe(out+48,h.mapEpoch,8);ipc::WriteLe(out+56,h.deviceEpoch,8);
  ipc::WriteLe(out+64,h.resourceId,8);ipc::WriteLe(out+72,h.generation,8);ipc::WriteLe(out+80,h.frame,8);
  Header checked;if(DecodeHeader(out,HeaderBytes,checked)!=Error::None)return Error::Encoding;
  if(h.payloadBytes)std::memcpy(out+HeaderBytes,payload,h.payloadBytes);
  n=HeaderBytes+h.payloadBytes;return Error::None;
}
Error Session::receive(const uint8_t* data,size_t bytes,View& out) noexcept {
  out={};
  if(m_state==State::Closed||m_state==State::Fault)return Error::Terminal;
  View v;auto e=Decode(data,bytes,v);if(e!=Error::None)return reject(e);
  const auto& h=v.header;
  if(!(h.nonce==m_nonce))return reject(Error::Nonce);
  if(h.flags||h.sequence!=m_next||m_next==UINT64_MAX)return reject(Error::Sequence);
  const bool zero=!h.mapEpoch&&!h.deviceEpoch&&!h.resourceId&&!h.generation&&!h.frame;
  const bool current=h.mapEpoch==m_map&&h.deviceEpoch==m_device&&h.resourceId==1&&h.generation==m_generation;
  switch(h.op) {
    case Op::Hello:
      if(m_state!=State::AwaitHello)return reject(Error::State);
      if(!zero)return reject(Error::Scope);
      if(h.payloadBytes!=24)return reject(Error::Payload);
      if(ipc::ReadLe(v.payload,4)!=32||ipc::ReadLe(v.payload+4,4)!=64)return reject(Error::Bits);
      if(ipc::ReadLe(v.payload+8,8)!=Capability||ipc::ReadLe(v.payload+16,8)!=Capability)return reject(Error::Payload);
      m_state=State::Idle;break;
    case Op::Begin:
      if(m_state!=State::Idle)return reject(Error::State);
      if(!h.mapEpoch||!h.deviceEpoch||h.mapEpoch<m_map||h.deviceEpoch<m_device||
          (h.mapEpoch==m_map&&h.deviceEpoch==m_device)||h.resourceId!=1||
          m_generation==UINT64_MAX||h.generation!=m_generation+1||h.frame)return reject(Error::Scope);
      if(h.payloadBytes!=8||ipc::ReadLe(v.payload,4)<16||ipc::ReadLe(v.payload,4)>256||
          ipc::ReadLe(v.payload+4,4)<16||ipc::ReadLe(v.payload+4,4)>256)return reject(Error::Payload);
      m_map=h.mapEpoch;m_device=h.deviceEpoch;m_generation=h.generation;m_frame=0;m_state=State::Active;break;
    case Op::Render:
      if(m_state!=State::Active)return reject(Error::State);
      if(!current)return reject(Error::Scope);
      if(m_frame==UINT64_MAX||h.frame!=m_frame+1)return reject(Error::Frame);
      if(h.payloadBytes!=4||ipc::ReadLe(v.payload,4)>3)return reject(Error::Payload);
      m_frame=h.frame;m_state=State::RenderPending;break;
    case Op::Release:
      if(m_state!=State::AwaitConsumer)return reject(Error::State);
      if(!current)return reject(Error::Scope);
      if(h.frame!=m_frame)return reject(Error::Frame);
      if(h.payloadBytes)return reject(Error::Payload);
      m_state=State::ReleasePending;break;
    case Op::End:
      if(m_state!=State::Active)return reject(Error::State);
      if(!current||h.frame)return reject(Error::Scope);
      if(h.payloadBytes)return reject(Error::Payload);
      m_state=State::Idle;break;
    case Op::Close:
      if(m_state!=State::Idle)return reject(Error::State);
      if(!zero)return reject(Error::Scope);
      if(h.payloadBytes)return reject(Error::Payload);
      m_state=State::Closing;break;
  }
  ++m_next;++m_accepted;out=v;return Error::None;
}
Error Session::submitted(uint64_t frame) noexcept {
  if(m_state!=State::RenderPending||frame!=m_frame)return reject(Error::State);
  m_state=State::AwaitConsumer;return Error::None;
}
Error Session::consumerFinished(uint64_t frame) noexcept {
  if(m_state!=State::ReleasePending||frame!=m_frame)return reject(Error::State);
  m_state=State::Active;return Error::None;
}
Error Session::closeSent(uint64_t sequence) noexcept {
  if(m_state!=State::Closing||sequence!=m_next-1)return reject(Error::State);
  m_state=State::Closed;return Error::None;
}
} // namespace warvk::host::gpuwire
