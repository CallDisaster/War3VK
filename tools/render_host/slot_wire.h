#pragma once
#include "protocol.h"

// RH1 is a distinct protocol, not RH0 Echo with a hidden resource payload.
namespace warvk::host::slotwire {
inline constexpr size_t HeaderBytes = 48;
inline constexpr uint32_t MaxPayloadBytes = 128;
inline constexpr size_t MaxMessageBytes = HeaderBytes + MaxPayloadBytes;
inline constexpr uint64_t SharedCpuSlots = 2;
inline constexpr uint64_t EnvelopeSamples = 4;
inline constexpr uint64_t RecorderEvents = 8; // WVE1 CPU history, never EnvelopeSamples.
enum class Op : uint16_t { Hello=1, Begin=2, Reserve=3, Publish=4, Cancel=5, End=6, Close=7, Retire=8 };
enum class Error { None, NullBuffer, HeaderSize, Magic, Version, Opcode, Flags,
                   Reserved, PayloadLimit, PacketSize, EmptyNonce };
struct Header {
  Op op = Op::Hello;
  uint32_t flags = 0, payloadBytes = 0;
  ipc::Nonce nonce;
  uint64_t sequence = 1;
};
struct View { Header header; const uint8_t* payload = nullptr; };
using Packet = std::array<uint8_t, MaxMessageBytes>;
Error DecodeHeader(const uint8_t* data, size_t count, Header& out) noexcept;
Error Decode(const uint8_t* data, size_t count, View& out) noexcept;
Error Encode(const Header& h, const uint8_t* payload, size_t count,
             uint8_t* output, size_t capacity, size_t& written) noexcept;
const char* ErrorName(Error error) noexcept;
std::array<uint8_t, 24> Hello(uint32_t sender, uint32_t peer, uint64_t capabilities = SharedCpuSlots) noexcept;
} // namespace warvk::host::slotwire
