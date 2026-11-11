#pragma once
#include "protocol.h"

// RH2 is an independent, bounded GPU laboratory protocol. It is not RH0 Echo
// and is not a D3D9 remote command stream. Only the pinned child exports NT
// handles; the parent duplicates them from its retained child process handle.
namespace warvk::host::gpuwire {
constexpr size_t HeaderBytes = 96;
constexpr size_t MaxPayload = 96;
constexpr uint64_t Capability = 8;
using Packet = std::array<uint8_t, HeaderBytes + MaxPayload>;
enum class Op : uint16_t { Hello=1, Begin=2, Render=3, Release=4, End=5, Close=6 };
enum class Error { None, Encoding, Nonce, Sequence, State, Scope, Payload, Frame, Bits, Terminal };
enum class State { AwaitHello, Idle, Active, RenderPending, AwaitConsumer, ReleasePending, Closing, Closed, Fault };
struct Header {
  Op op=Op::Hello;
  uint32_t flags=0, payloadBytes=0;
  ipc::Nonce nonce;
  uint64_t sequence=1, mapEpoch=0, deviceEpoch=0, resourceId=0, generation=0, frame=0;
};
struct View { Header header; const uint8_t* payload=nullptr; };
const char* ErrorName(Error error) noexcept;
Error DecodeHeader(const uint8_t* data, size_t size, Header& result) noexcept;
Error Decode(const uint8_t* data, size_t size, View& result) noexcept;
Error Encode(const Header& h, const uint8_t* payload, uint8_t* out, size_t capacity, size_t& size) noexcept;
// Owner callbacks denote actual backend milestones, never a client-provided
// boolean. The host must wait for the consumed GPU semaphore before finishing
// Release. No subsequent Render/End may enter while that wait is pending.
class Session {
public:
  explicit Session(ipc::Nonce nonce) noexcept : m_nonce(nonce) {}
  Error receive(const uint8_t* data, size_t size, View& result) noexcept;
  Error submitted(uint64_t frame) noexcept;
  Error consumerFinished(uint64_t frame) noexcept;
  Error closeSent(uint64_t sequence) noexcept;
  void fault() noexcept { m_state=State::Fault; }
  State state() const noexcept { return m_state; }
  uint64_t accepted() const noexcept { return m_accepted; }
private:
  Error reject(Error e) noexcept { fault(); return e; }
  ipc::Nonce m_nonce;
  State m_state=State::AwaitHello;
  uint64_t m_next=1,m_map=0,m_device=0,m_generation=0,m_frame=0,m_accepted=0;
};
} // namespace warvk::host::gpuwire
