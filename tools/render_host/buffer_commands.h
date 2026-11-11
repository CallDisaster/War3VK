#pragma once

#include "render_command_queue.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace warvk::host::buffers {

// WRC1 is a bounded resource-subset lab, NOT the RH0/RH1 protocol, a native
// struct layout, complete D3D9 semantics or a cross-process render owner.
inline constexpr size_t HeaderBytes = 96;
inline constexpr size_t MaxPacketBytes = 65536;
inline constexpr uint32_t MaxUploadBytes = 65440;
inline constexpr uint64_t MaxBufferBytes = 262144;
inline constexpr uint32_t ResourceSlots = 4;
inline constexpr uint16_t Version = 1;
using Packet = std::array<uint8_t, MaxPacketBytes>;

enum class Op : uint16_t { Create = 1, Upload = 2, Verify = 3, Destroy = 4, Finish = 5 };
enum class Error {
  None, NullBuffer, HeaderSize, Magic, Version, Opcode, Reserved,
  PayloadSize, PacketSize, Scope, Sequence, SequenceExhausted,
  ResourceId, Generation, GenerationExhausted, Argument, Range,
  ResourceLive, ResourceInactive, LiveResources, BackendFailure,
  Reentrant, Terminal, Unconfigured,
};

struct Header {
  Op op = Op::Create;
  uint32_t payloadBytes = 0;
  commands::Scope scope;
  uint64_t sequence = 1, resourceId = 0, generation = 0, argument = 0;
};

// Payload is borrowed only during the synchronous Backend::apply callback.
// Neither a decoded View nor a CPU acknowledgement proves GPU completion.
struct View {
  Header header;
  const uint8_t* payload = nullptr;
};

const char* ErrorName(Error error) noexcept;
// DecodeHeader takes EXACTLY 96 bytes and bounds/validates the advertised body,
// allowing a framing owner to read that body safely under its original deadline.
// Failed decodes clear output so an old parsed command cannot be accidentally used.
Error DecodeHeader(const uint8_t* data, size_t count, Header& output) noexcept;
Error Decode(const uint8_t* data, size_t count, View& output) noexcept;
// On rejection, written=0 and output bytes remain unchanged. Source/destination
// payload overlap is supported; native padding/pointers never enter the wire.
Error Encode(const Header& header, const uint8_t* payload,
             uint8_t* output, size_t capacity, size_t& written) noexcept;
Error Encode(const Header& header, const uint8_t* payload,
             Packet& output, size_t& written) noexcept;

enum class State { Active, Finished, Faulted };
struct Slot {
  bool live = false;
  uint64_t lastGeneration = 0, bytes = 0;
  bool operator==(const Slot& other) const noexcept {
    return live == other.live && lastGeneration == other.lastGeneration && bytes == other.bytes;
  }
};
struct Snapshot {
  State state = State::Faulted;
  uint64_t accepted = 0, lastSequence = 0;
  bool sequenceExhausted = false;
  std::array<Slot, ResourceSlots> slots{};
};
struct Limits {
  // Lower immutable limits permit practical exhaustion tests. Neither limit
  // allows a non-one initial sequence/generation or revives an earlier scope.
  uint64_t maxSequence = std::numeric_limits<uint64_t>::max();
  uint64_t maxGeneration = std::numeric_limits<uint64_t>::max();
};

class Backend {
public:
  virtual ~Backend() = default;
  // Called only after complete byte decoding and ledger preflight. true means
  // this CPU operation succeeded, NOT GPU completion. It must synchronously
  // own/copy upload bytes before returning. Verify/Finish GPU proof, outstanding
  // resources and failure quarantine belong to the backend/transport owner.
  // false/throw faults the session without committing its metadata. This class
  // never destroys backend objects merely because CPU execution failed.
  virtual bool apply(const View& command) = 0;
};

// Exactly one serialized owner, not thread-safe or reentrant. All slots, scope
// and sequence are private. There is no public commit or forgeable commit token:
// decode -> read-only preflight -> real backend success -> private commit.
// A failed operation changes only terminal state; no accepted count, sequence,
// resource size/live bit or generation advances. New scope needs a new Session.
class Session final {
public:
  explicit Session(commands::Scope scope, Limits limits = {}) noexcept;
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = delete;
  Session& operator=(Session&&) = delete;

  Error execute(const uint8_t* packet, size_t count, Backend& backend) noexcept;
  // Serialized transport owner only. Does not release/revive unknown GPU work.
  void fault() noexcept { m_state = State::Faulted; }
  Snapshot snapshot() const noexcept;

private:
  Error preflight(const Header& header) const noexcept;
  Error reject(Error error) noexcept { fault(); return error; }
  void commit(const Header& header) noexcept;

  const commands::Scope m_scope;
  const Limits m_limits;
  const bool m_configured;
  State m_state;
  std::array<Slot, ResourceSlots> m_slots{};
  uint64_t m_accepted = 0, m_lastSequence = 0;
  bool m_sequenceExhausted = false, m_inBackend = false;
};

} // namespace warvk::host::buffers
