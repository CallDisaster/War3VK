#include "buffer_commands.h"
#include <cstring>

namespace warvk::host::buffers {
namespace {

uint64_t readLe(const uint8_t* data, unsigned bytes) noexcept {
  uint64_t result = 0;
  for (unsigned i = 0; i < bytes; ++i) result |= uint64_t(data[i]) << (i * 8);
  return result;
}
void writeLe(uint8_t* data, uint64_t value, unsigned bytes) noexcept {
  for (unsigned i = 0; i < bytes; ++i) data[i] = uint8_t(value >> (i * 8));
}
Error validateHeader(const Header& h) noexcept {
  switch (h.op) {
    case Op::Create: case Op::Upload: case Op::Verify: case Op::Destroy: case Op::Finish: break;
    default: return Error::Opcode;
  }
  if (!h.scope.valid()) return Error::Scope;
  if (!h.sequence) return Error::Sequence;
  if (h.payloadBytes > MaxUploadBytes) return Error::PayloadSize;
  if (h.op == Op::Finish) {
    if (h.resourceId) return Error::ResourceId;
    if (h.generation) return Error::Generation;
    if (h.argument) return Error::Argument;
    if (h.payloadBytes) return Error::PayloadSize;
    return Error::None;
  }
  if (!h.resourceId || h.resourceId > ResourceSlots) return Error::ResourceId;
  if (!h.generation) return Error::Generation;
  if (h.op == Op::Upload) {
    if (!h.payloadBytes) return Error::PayloadSize;
    if (h.argument > std::numeric_limits<uint64_t>::max() - h.payloadBytes) return Error::Range;
  } else {
    if (h.payloadBytes) return Error::PayloadSize;
    if (h.op == Op::Create) {
      if (!h.argument || h.argument > MaxBufferBytes) return Error::Argument;
    } else if (h.argument) {
      return Error::Argument;
    }
  }
  return Error::None;
}

} // namespace

const char* ErrorName(Error error) noexcept {
  switch (error) {
#define BUFFER_ERROR_NAME(name) case Error::name: return #name
    BUFFER_ERROR_NAME(None); BUFFER_ERROR_NAME(NullBuffer); BUFFER_ERROR_NAME(HeaderSize);
    BUFFER_ERROR_NAME(Magic); BUFFER_ERROR_NAME(Version); BUFFER_ERROR_NAME(Opcode);
    BUFFER_ERROR_NAME(Reserved); BUFFER_ERROR_NAME(PayloadSize); BUFFER_ERROR_NAME(PacketSize);
    BUFFER_ERROR_NAME(Scope); BUFFER_ERROR_NAME(Sequence); BUFFER_ERROR_NAME(SequenceExhausted);
    BUFFER_ERROR_NAME(ResourceId); BUFFER_ERROR_NAME(Generation); BUFFER_ERROR_NAME(GenerationExhausted);
    BUFFER_ERROR_NAME(Argument); BUFFER_ERROR_NAME(Range); BUFFER_ERROR_NAME(ResourceLive);
    BUFFER_ERROR_NAME(ResourceInactive); BUFFER_ERROR_NAME(LiveResources); BUFFER_ERROR_NAME(BackendFailure);
    BUFFER_ERROR_NAME(Reentrant); BUFFER_ERROR_NAME(Terminal); BUFFER_ERROR_NAME(Unconfigured);
#undef BUFFER_ERROR_NAME
  }
  return "Unknown";
}

Error DecodeHeader(const uint8_t* data, size_t count, Header& output) noexcept {
  output = {};
  if (!data) return Error::NullBuffer;
  if (count != HeaderBytes) return Error::HeaderSize;
  if (data[0] != 'W' || data[1] != 'R' || data[2] != 'C' || data[3] != '1') return Error::Magic;
  if (readLe(data + 4, 2) != Version) return Error::Version;
  for (unsigned i = 12; i < 16; ++i) if (data[i]) return Error::Reserved;
  for (unsigned i = 80; i < HeaderBytes; ++i) if (data[i]) return Error::Reserved;

  Header h;
  h.op = static_cast<Op>(readLe(data + 6, 2));
  h.payloadBytes = static_cast<uint32_t>(readLe(data + 8, 4));
  h.scope.nonceLow = readLe(data + 16, 8);
  h.scope.nonceHigh = readLe(data + 24, 8);
  h.scope.mapEpoch = readLe(data + 32, 8);
  h.scope.deviceEpoch = readLe(data + 40, 8);
  h.sequence = readLe(data + 48, 8);
  h.resourceId = readLe(data + 56, 8);
  h.generation = readLe(data + 64, 8);
  h.argument = readLe(data + 72, 8);
  const auto error = validateHeader(h);
  if (error != Error::None) return error;
  output = h;
  return Error::None;
}

Error Decode(const uint8_t* data, size_t count, View& output) noexcept {
  output = {};
  if (!data) return Error::NullBuffer;
  if (count < HeaderBytes || count > MaxPacketBytes) return Error::PacketSize;
  Header header;
  const auto error = DecodeHeader(data, HeaderBytes, header);
  if (error != Error::None) return error;
  if (count != HeaderBytes + header.payloadBytes) return Error::PacketSize;
  output.header = header;
  output.payload = header.payloadBytes ? data + HeaderBytes : nullptr;
  return Error::None;
}

Error Encode(const Header& input, const uint8_t* payload,
    uint8_t* output, size_t capacity, size_t& written) noexcept {
  written = 0;
  const Header h = input;
  const auto error = validateHeader(h);
  if (error != Error::None) return error;
  if (!output || (h.payloadBytes && !payload)) return Error::NullBuffer;
  const size_t count = HeaderBytes + h.payloadBytes;
  if (capacity < count) return Error::PacketSize;
  if (h.payloadBytes) std::memmove(output + HeaderBytes, payload, h.payloadBytes);
  std::memset(output, 0, HeaderBytes);
  output[0] = 'W'; output[1] = 'R'; output[2] = 'C'; output[3] = '1';
  writeLe(output + 4, Version, 2);
  writeLe(output + 6, static_cast<uint16_t>(h.op), 2);
  writeLe(output + 8, h.payloadBytes, 4);
  writeLe(output + 16, h.scope.nonceLow, 8);
  writeLe(output + 24, h.scope.nonceHigh, 8);
  writeLe(output + 32, h.scope.mapEpoch, 8);
  writeLe(output + 40, h.scope.deviceEpoch, 8);
  writeLe(output + 48, h.sequence, 8);
  writeLe(output + 56, h.resourceId, 8);
  writeLe(output + 64, h.generation, 8);
  writeLe(output + 72, h.argument, 8);
  written = count;
  return Error::None;
}

Error Encode(const Header& h, const uint8_t* payload, Packet& output, size_t& written) noexcept {
  return Encode(h, payload, output.data(), output.size(), written);
}

Session::Session(commands::Scope scope, Limits limits) noexcept
  : m_scope(scope), m_limits(limits),
    m_configured(scope.valid() && limits.maxSequence != 0 && limits.maxGeneration != 0),
    m_state(m_configured ? State::Active : State::Faulted) { }

Error Session::preflight(const Header& h) const noexcept {
  if (!(h.scope == m_scope)) return Error::Scope;
  if (m_sequenceExhausted) return Error::SequenceExhausted;
  if (h.sequence != m_lastSequence + 1) return Error::Sequence;
  if (h.op == Op::Finish) {
    for (const auto& slot : m_slots) if (slot.live) return Error::LiveResources;
    return Error::None;
  }
  const auto& slot = m_slots[static_cast<size_t>(h.resourceId - 1)];
  if (h.op == Op::Create) {
    if (slot.live) return Error::ResourceLive;
    if (slot.lastGeneration == m_limits.maxGeneration) return Error::GenerationExhausted;
    if (h.generation != slot.lastGeneration + 1) return Error::Generation;
  } else {
    if (!slot.live) return Error::ResourceInactive;
    if (h.generation != slot.lastGeneration) return Error::Generation;
    if (h.op == Op::Upload
        && (h.argument > slot.bytes || h.payloadBytes > slot.bytes - h.argument)) return Error::Range;
  }
  return Error::None;
}

void Session::commit(const Header& h) noexcept {
  if (h.op == Op::Finish) {
    m_state = State::Finished;
  } else {
    auto& slot = m_slots[static_cast<size_t>(h.resourceId - 1)];
    if (h.op == Op::Create) {
      slot.live = true;
      slot.lastGeneration = h.generation;
      slot.bytes = h.argument;
    } else if (h.op == Op::Destroy) {
      slot.live = false;
      slot.bytes = 0;
      // lastGeneration is a tombstone; Destroy never resets it for ID reuse.
    }
  }
  ++m_accepted;
  m_lastSequence = h.sequence;
  m_sequenceExhausted = m_lastSequence == m_limits.maxSequence;
}

Error Session::execute(const uint8_t* packet, size_t count, Backend& backend) noexcept {
  if (!m_configured) return reject(Error::Unconfigured);
  if (m_inBackend) return reject(Error::Reentrant);
  if (m_state != State::Active) return reject(Error::Terminal);
  View view;
  auto error = Decode(packet, count, view);
  if (error != Error::None) return reject(error);
  error = preflight(view.header);
  if (error != Error::None) return reject(error);

  m_inBackend = true;
  bool success = false;
  try { success = backend.apply(view); }
  catch (...) { success = false; }
  m_inBackend = false;
  // A backend/transport callback may fault the session, but cannot make its
  // preflight metadata current again or bypass the private commit point.
  if (m_state != State::Active) return reject(Error::Terminal);
  if (!success) return reject(Error::BackendFailure);
  commit(view.header);
  return Error::None;
}

Snapshot Session::snapshot() const noexcept {
  Snapshot result;
  result.state = m_state;
  result.accepted = m_accepted;
  result.lastSequence = m_lastSequence;
  result.sequenceExhausted = m_sequenceExhausted;
  result.slots = m_slots;
  return result;
}

} // namespace warvk::host::buffers
