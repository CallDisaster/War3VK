#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// RH0 is a CPU protocol laboratory, not a D3D9/GPU command protocol.
// No Windows handles, native pointers or native struct layout cross the wire.
namespace warvk::host::ipc {
inline constexpr size_t HeaderBytes = 96;
inline constexpr uint32_t MaxPayloadBytes = 65536;
inline constexpr size_t MaxMessageBytes = HeaderBytes + MaxPayloadBytes;
inline constexpr uint16_t Major = 0, Minor = 1;
inline constexpr uint64_t CpuEcho = 1;

enum class Op : uint16_t { Hello=1, BeginEpoch=2, SampleEcho=3, EndEpoch=4, Close=5 };
enum class Error {
  None, NullBuffer, ShortHeader, Magic, Version, HeaderSize, Opcode, Flags,
  Reserved, PayloadLimit, PacketSize, EmptyNonce, Session, Sequence,
  SequenceExhausted, ResourceUnsupported, State, Scope, Payload,
  Bits, Capabilities, Frame, Terminal
};
enum class State { AwaitHello, Ready, Active, Closing, Closed, Fault };
struct Nonce {
  uint64_t low=0, high=0;
  bool empty() const noexcept { return !(low|high); }
  bool operator==(Nonce n) const noexcept { return low==n.low && high==n.high; }
};
struct Header {
  Op op=Op::Hello;
  uint32_t flags=0, payloadBytes=0;
  Nonce nonce;
  uint64_t sequence=1, mapEpoch=0, deviceEpoch=0;
  uint64_t resourceId=0, resourceGeneration=0, frameId=0;
};
// Borrowed only until the caller mutates/releases the byte buffer. It is never
// a resource lease. ServerSession always decodes bytes itself, not a forged view.
struct MessageView { Header header; const uint8_t* payload=nullptr; };
uint64_t ReadLe(const uint8_t* data, unsigned bytes) noexcept;
void WriteLe(uint8_t* data, uint64_t value, unsigned bytes) noexcept;
Error DecodeHeader(const uint8_t* data, size_t bytes, Header& result) noexcept;
Error Decode(const uint8_t* data, size_t bytes, MessageView& result) noexcept;
Error Encode(const Header& h, const uint8_t* payload, size_t payloadBytes,
             uint8_t* output, size_t capacity, size_t& written) noexcept;
const char* ErrorName(Error error) noexcept;

struct Hello {
  uint32_t senderBits=0, expectedPeerBits=0;
  uint64_t requiredCaps=CpuEcho, supportedCaps=CpuEcho;
};
std::array<uint8_t,24> EncodeHello(Hello hello) noexcept;

struct Reply {
  Header header;
  std::array<uint8_t,24> hello{};
  const uint8_t* echo=nullptr;
  // Valid only after receive==None; consumed synchronously before input reuse.
  const uint8_t* payload() const noexcept {
    return header.op==Op::Hello ? hello.data() : echo;
  }
};

class ServerSession {
public:
  ServerSession(Nonce nonce, uint32_t localBits, uint32_t peerBits) noexcept;
  Error receive(const uint8_t* data, size_t bytes, Reply& reply) noexcept;
  // Transport owner calls this only after the exact Close reply finished I/O.
  // Not a wire command, not a GPU fence, and not permission to release GPU data.
  Error closeReplySent(uint64_t sequence) noexcept;
  void transportFailed() noexcept { if(m_state!=State::Closed)m_state=State::Fault; }
  State state() const noexcept { return m_state; }
  uint64_t accepted() const noexcept { return m_accepted; }
  uint64_t mapEpoch() const noexcept { return m_map; }
  uint64_t deviceEpoch() const noexcept { return m_device; }
  uint64_t frame() const noexcept { return m_frame; }
private:
  Error reject(Error error) noexcept { m_state=State::Fault; return error; }
  Nonce m_nonce;
  uint32_t m_localBits=0, m_peerBits=0;
  State m_state=State::AwaitHello;
  uint64_t m_next=1, m_accepted=0, m_map=0, m_device=0, m_frame=0;
};
} // namespace warvk::host::ipc
