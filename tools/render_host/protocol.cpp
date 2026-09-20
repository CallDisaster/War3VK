#include "protocol.h"
#include <algorithm>
#include <cstring>

namespace warvk::host::ipc {
uint64_t ReadLe(const uint8_t* data, unsigned bytes) noexcept {
  uint64_t value=0;
  for(unsigned i=0;i<bytes;++i)value|=uint64_t(data[i])<<(8*i);
  return value;
}
void WriteLe(uint8_t* data, uint64_t value, unsigned bytes) noexcept {
  for(unsigned i=0;i<bytes;++i)data[i]=uint8_t(value>>(8*i));
}
Error DecodeHeader(const uint8_t* data, size_t bytes, Header& out) noexcept {
  out={};
  if(!data)return Error::NullBuffer;
  if(bytes<HeaderBytes)return Error::ShortHeader;
  if(std::memcmp(data,"WVK0",4))return Error::Magic;
  if(ReadLe(data+4,2)!=Major||ReadLe(data+6,2)!=Minor)return Error::Version;
  if(ReadLe(data+8,2)!=HeaderBytes)return Error::HeaderSize;
  auto op=ReadLe(data+10,2);
  if(op<uint16_t(Op::Hello)||op>uint16_t(Op::Close))return Error::Opcode;
  auto flags=ReadLe(data+12,4);
  if(flags>1)return Error::Flags;
  if(ReadLe(data+20,4)||ReadLe(data+88,4)||ReadLe(data+92,4))return Error::Reserved;
  auto payload=ReadLe(data+16,4);
  if(payload>MaxPayloadBytes)return Error::PayloadLimit;
  Header h;
  h.op=Op(op);h.flags=uint32_t(flags);h.payloadBytes=uint32_t(payload);
  h.nonce={ReadLe(data+24,8),ReadLe(data+32,8)};
  if(h.nonce.empty())return Error::EmptyNonce;
  h.sequence=ReadLe(data+40,8);h.mapEpoch=ReadLe(data+48,8);h.deviceEpoch=ReadLe(data+56,8);
  h.resourceId=ReadLe(data+64,8);h.resourceGeneration=ReadLe(data+72,8);h.frameId=ReadLe(data+80,8);
  out=h;return Error::None;
}
Error Decode(const uint8_t* data, size_t bytes, MessageView& out) noexcept {
  out={};Header h;
  const auto error=DecodeHeader(data,bytes,h);
  if(error!=Error::None)return error;
  if(bytes-HeaderBytes!=h.payloadBytes)return Error::PacketSize;
  out={h,data+HeaderBytes};return Error::None;
}
Error Encode(const Header& h, const uint8_t* payload, size_t payloadBytes,
             uint8_t* output, size_t capacity, size_t& written) noexcept {
  written=0;
  if(!output||(payloadBytes&&!payload))return Error::NullBuffer;
  if(payloadBytes>MaxPayloadBytes)return Error::PayloadLimit;
  if(h.payloadBytes!=payloadBytes||capacity<HeaderBytes||capacity-HeaderBytes<payloadBytes)
    return Error::PacketSize;
  // Construct privately, then validate before changing caller output.
  std::array<uint8_t,HeaderBytes> bytes{};
  std::memcpy(bytes.data(),"WVK0",4);
  WriteLe(bytes.data()+4,Major,2);WriteLe(bytes.data()+6,Minor,2);
  WriteLe(bytes.data()+8,HeaderBytes,2);WriteLe(bytes.data()+10,uint16_t(h.op),2);
  WriteLe(bytes.data()+12,h.flags,4);WriteLe(bytes.data()+16,payloadBytes,4);
  WriteLe(bytes.data()+24,h.nonce.low,8);WriteLe(bytes.data()+32,h.nonce.high,8);
  WriteLe(bytes.data()+40,h.sequence,8);WriteLe(bytes.data()+48,h.mapEpoch,8);
  WriteLe(bytes.data()+56,h.deviceEpoch,8);WriteLe(bytes.data()+64,h.resourceId,8);
  WriteLe(bytes.data()+72,h.resourceGeneration,8);WriteLe(bytes.data()+80,h.frameId,8);
  Header checked;auto error=DecodeHeader(bytes.data(),bytes.size(),checked);
  if(error!=Error::None)return error;
  // memmove allows echo payload to share its original receive buffer.
  if(payloadBytes)std::memmove(output+HeaderBytes,payload,payloadBytes);
  std::memcpy(output,bytes.data(),HeaderBytes);
  written=HeaderBytes+payloadBytes;return Error::None;
}
std::array<uint8_t,24> EncodeHello(Hello h) noexcept {
  std::array<uint8_t,24> result{};
  WriteLe(result.data(),h.senderBits,4);WriteLe(result.data()+4,h.expectedPeerBits,4);
  WriteLe(result.data()+8,h.requiredCaps,8);WriteLe(result.data()+16,h.supportedCaps,8);
  return result;
}
ServerSession::ServerSession(Nonce nonce, uint32_t local, uint32_t peer) noexcept
:m_nonce(nonce),m_localBits(local),m_peerBits(peer) {
  if(nonce.empty()||(local!=32&&local!=64)||(peer!=32&&peer!=64))m_state=State::Fault;
}
Error ServerSession::receive(const uint8_t* data, size_t bytes, Reply& reply) noexcept {
  reply={};
  if(m_state==State::Closed||m_state==State::Fault)return Error::Terminal;
  if(m_state==State::Closing)return reject(Error::State);
  MessageView v;auto error=Decode(data,bytes,v);
  if(error!=Error::None)return reject(error);
  const auto& h=v.header;
  if(h.flags)return reject(Error::Flags);
  if(!(h.nonce==m_nonce))return reject(Error::Session);
  if(h.sequence==UINT64_MAX)return reject(Error::SequenceExhausted);
  if(h.sequence!=m_next)return reject(Error::Sequence);
  if(h.resourceId||h.resourceGeneration)return reject(Error::ResourceUnsupported);
  auto nextState=m_state;auto nextMap=m_map;auto nextDevice=m_device;auto nextFrame=m_frame;
  switch(h.op){
    case Op::Hello:
      if(m_state!=State::AwaitHello)return reject(Error::State);
      if(h.mapEpoch||h.deviceEpoch||h.frameId)return reject(Error::Scope);
      if(h.payloadBytes!=24)return reject(Error::Payload);
      if(ReadLe(v.payload,4)!=m_peerBits||ReadLe(v.payload+4,4)!=m_localBits)return reject(Error::Bits);
      if(ReadLe(v.payload+8,8)!=CpuEcho||ReadLe(v.payload+16,8)!=CpuEcho)return reject(Error::Capabilities);
      reply.hello=EncodeHello({m_localBits,m_peerBits,CpuEcho,CpuEcho});nextState=State::Ready;break;
    case Op::BeginEpoch:
      if(m_state!=State::Ready)return reject(Error::State);
      if(h.payloadBytes)return reject(Error::Payload);
      if(!h.mapEpoch||!h.deviceEpoch||h.frameId||h.mapEpoch<m_map||h.deviceEpoch<m_device||
        (h.mapEpoch==m_map&&h.deviceEpoch==m_device))return reject(Error::Scope);
      nextMap=h.mapEpoch;nextDevice=h.deviceEpoch;nextFrame=0;nextState=State::Active;break;
    case Op::SampleEcho:
    case Op::EndEpoch:
      if(m_state!=State::Active)return reject(Error::State);
      if(h.mapEpoch!=m_map||h.deviceEpoch!=m_device)return reject(Error::Scope);
      if(h.op==Op::SampleEcho){
        if(m_frame==UINT64_MAX||h.frameId!=m_frame+1)return reject(Error::Frame);
        nextFrame=h.frameId;reply.echo=v.payload;
      }else{
        if(h.frameId)return reject(Error::Frame);
        if(h.payloadBytes)return reject(Error::Payload);
        nextState=State::Ready;
      }
      break;
    case Op::Close:
      if(m_state!=State::Ready)return reject(Error::State);
      if(h.payloadBytes)return reject(Error::Payload);
      if(h.mapEpoch||h.deviceEpoch||h.frameId)return reject(Error::Scope);
      nextState=State::Closing;break;
  }
  // Only complete validation commits counters/scope. No callbacks or I/O here.
  m_state=nextState;m_map=nextMap;m_device=nextDevice;m_frame=nextFrame;++m_next;++m_accepted;
  reply.header=h;reply.header.flags=1;
  return Error::None;
}
Error ServerSession::closeReplySent(uint64_t sequence) noexcept {
  if(m_state==State::Closed||m_state==State::Fault)return Error::Terminal;
  if(m_state!=State::Closing)return reject(Error::State);
  if(sequence!=m_next-1)return reject(Error::Sequence);
  m_state=State::Closed;return Error::None;
}
const char* ErrorName(Error e) noexcept {
  switch(e){
#define RH_ERROR(x) case Error::x:return #x;
    RH_ERROR(None) RH_ERROR(NullBuffer) RH_ERROR(ShortHeader) RH_ERROR(Magic)
    RH_ERROR(Version) RH_ERROR(HeaderSize) RH_ERROR(Opcode) RH_ERROR(Flags)
    RH_ERROR(Reserved) RH_ERROR(PayloadLimit) RH_ERROR(PacketSize) RH_ERROR(EmptyNonce)
    RH_ERROR(Session) RH_ERROR(Sequence) RH_ERROR(SequenceExhausted)
    RH_ERROR(ResourceUnsupported) RH_ERROR(State) RH_ERROR(Scope) RH_ERROR(Payload)
    RH_ERROR(Bits) RH_ERROR(Capabilities) RH_ERROR(Frame) RH_ERROR(Terminal)
#undef RH_ERROR
  }
  return "Unknown";
}
} // namespace warvk::host::ipc
