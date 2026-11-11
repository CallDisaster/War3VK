#pragma once
#include "buffer_commands.h"
#include "win32_transport.h"
#include <cstring>
#include <stdexcept>

// Externally supervised laboratory framing/replies. The immutable command
// codec itself has no Win32 dependency. None of these blocking functions is a
// product render-thread API. A reply is CPU acceptance plus separately measured
// GPU counters, never implicit permission to retire a resource in another owner.
namespace warvk::host::bufferlab {
inline void Require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}
inline uint64_t Get64(const uint8_t* p) noexcept {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= uint64_t(p[i]) << (8 * i);
  return value;
}
inline void Put64(uint8_t* p, uint64_t value) noexcept {
  for (unsigned i = 0; i < 8; ++i) p[i] = uint8_t(value >> (8 * i));
}
inline constexpr size_t ReplyBytes = 176;
using ReplyPacket = std::array<uint8_t, ReplyBytes>;
struct Reply {
  buffers::Error error = buffers::Error::None;
  uint64_t accepted = 0, submitted = 0, gpuCompleted = 0, retired = 0;
  uint64_t liveBytes = 0, verifiedBytes = 0, hash = 0, backendCalls = 0;
};
inline ReplyPacket EncodeReply(const buffers::Packet& request, const Reply& r) {
  ReplyPacket packet{};
  std::memcpy(packet.data(), request.data(), buffers::HeaderBytes);
  Put64(packet.data() + 96, 0x0000000141435257ull); // WRCA, reply version 1.
  Put64(packet.data() + 104, uint64_t(r.error));
  Put64(packet.data() + 112, r.accepted);
  Put64(packet.data() + 120, r.submitted);
  Put64(packet.data() + 128, r.gpuCompleted);
  Put64(packet.data() + 136, r.retired);
  Put64(packet.data() + 144, r.liveBytes);
  Put64(packet.data() + 152, r.verifiedBytes);
  Put64(packet.data() + 160, r.hash);
  Put64(packet.data() + 168, r.backendCalls);
  return packet;
}
inline Reply DecodeReply(const uint8_t* request, const ReplyPacket& packet) {
  Require(!std::memcmp(request, packet.data(), buffers::HeaderBytes), "reply-request-identity");
  Require(Get64(packet.data() + 96) == 0x0000000141435257ull, "reply-version");
  Require(Get64(packet.data() + 104) <= uint64_t(buffers::Error::Unconfigured), "reply-status-value");
  Reply r;
  r.error = buffers::Error(Get64(packet.data() + 104));
  r.accepted = Get64(packet.data() + 112);
  r.submitted = Get64(packet.data() + 120);
  r.gpuCompleted = Get64(packet.data() + 128);
  r.retired = Get64(packet.data() + 136);
  r.liveBytes = Get64(packet.data() + 144);
  r.verifiedBytes = Get64(packet.data() + 152);
  r.hash = Get64(packet.data() + 160);
  r.backendCalls = Get64(packet.data() + 168);
  Require(r.retired <= r.gpuCompleted && r.gpuCompleted <= r.submitted, "reply-completion-order");
  Require(r.liveBytes <= buffers::ResourceSlots * uint64_t(buffers::MaxBufferBytes), "reply-live-byte-bound");
  return r;
}
inline buffers::Error ReadCommand(HANDLE pipe, HANDLE peer, buffers::Packet& packet,
    size_t& count, lab::IoStats& stats) {
  count = 0;
  const lab::ReadWindow deadline;
  lab::ReadExact(pipe, peer, packet.data(), buffers::HeaderBytes, deadline, lab::Stage::Header, stats);
  buffers::Header header;
  auto error = buffers::DecodeHeader(packet.data(), buffers::HeaderBytes, header);
  if (error != buffers::Error::None) return error;
  lab::ReadExact(pipe, peer, packet.data() + buffers::HeaderBytes, header.payloadBytes,
                 deadline, lab::Stage::Body, stats);
  count = buffers::HeaderBytes + header.payloadBytes;
  return buffers::Error::None;
}
inline uint8_t Pattern(uint64_t index, uint32_t salt) noexcept {
  return uint8_t(index * 29 + (index >> 8) * 7 + uint64_t(salt) * 43);
}
inline uint64_t Golden(uint32_t bytes, uint32_t salt, bool patched) noexcept {
  uint64_t hash = 14695981039346656037ull;
  for (uint32_t i = 0; i < bytes; ++i) {
    const uint8_t value = patched && i >= 3 && i < 100 ? Pattern(i - 3, 9) : Pattern(i, salt);
    hash = (hash ^ value) * 1099511628211ull;
  }
  return hash;
}
} // namespace warvk::host::bufferlab
