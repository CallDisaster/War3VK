#pragma once
#include "slot_wire.h"
#include "win32_transport.h"

namespace warvk::host::slotwire {
// Shared overlapped lifetime, independent RH1 parser and strict small cap.
inline Error ReadPacket(HANDLE pipe, HANDLE peer, Packet& packet, size_t& count, lab::IoStats& stats) {
  count = 0;
  const lab::ReadWindow window;
  lab::ReadExact(pipe, peer, packet.data(), HeaderBytes, window, lab::Stage::Header, stats);
  Header header;
  auto error = DecodeHeader(packet.data(), HeaderBytes, header);
  if (error != Error::None) return error;
  lab::ReadExact(pipe, peer, packet.data() + HeaderBytes, header.payloadBytes, window, lab::Stage::Body, stats);
  count = HeaderBytes + header.payloadBytes;
  return Error::None;
}
} // namespace warvk::host::slotwire
