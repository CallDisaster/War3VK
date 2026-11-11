#pragma once
#include "gpu_wire.h"
#include "win32_transport.h"

namespace warvk::host::gpuwire {
inline Error ReadPacket(HANDLE pipe,HANDLE peer,Packet& packet,size_t& count,lab::IoStats& stats) {
  count=0;const lab::ReadWindow window;
  lab::ReadExact(pipe,peer,packet.data(),HeaderBytes,window,lab::Stage::Header,stats);
  Header header;auto e=DecodeHeader(packet.data(),HeaderBytes,header);
  if(e!=Error::None)return e;
  lab::ReadExact(pipe,peer,packet.data()+HeaderBytes,header.payloadBytes,window,lab::Stage::Body,stats);
  count=HeaderBytes+header.payloadBytes;return Error::None;
}
} // namespace warvk::host::gpuwire
