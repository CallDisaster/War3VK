#include "slot_ledger.h"
#include <cstring>

namespace warvk::host::slots {
namespace {
Error Shape(const Key& key) noexcept {
  if (key.connection.empty() || !key.map || !key.device || !key.frame || !key.generation)
    return Error::Lease;
  if (key.slot >= SlotCount)
    return Error::Slot;
  if (!key.bytes || key.bytes > SlotBytes)
    return Error::Size;
  return Error::None;
}
}

bool Key::operator==(const Key& other) const noexcept {
  return connection == other.connection && map == other.map && device == other.device
    && frame == other.frame && generation == other.generation && slot == other.slot && bytes == other.bytes;
}

Error EncodeKey(const Key& key, uint8_t* output, size_t capacity) noexcept {
  if (!output)
    return Error::NullBuffer;
  if (capacity < DescriptorBytes)
    return Error::DescriptorSize;
  auto error = Shape(key);
  if (error != Error::None)
    return error;
  std::array<uint8_t, DescriptorBytes> bytes{};
  std::memcpy(bytes.data(), "WVL1", 4);
  ipc::WriteLe(bytes.data() + 4, 1, 2);
  ipc::WriteLe(bytes.data() + 6, DescriptorBytes, 2);
  ipc::WriteLe(bytes.data() + 8, key.connection.low, 8);
  ipc::WriteLe(bytes.data() + 16, key.connection.high, 8);
  ipc::WriteLe(bytes.data() + 24, key.map, 8);
  ipc::WriteLe(bytes.data() + 32, key.device, 8);
  ipc::WriteLe(bytes.data() + 40, key.frame, 8);
  ipc::WriteLe(bytes.data() + 48, key.generation, 8);
  ipc::WriteLe(bytes.data() + 56, key.slot, 4);
  ipc::WriteLe(bytes.data() + 60, key.bytes, 4);
  std::memcpy(output, bytes.data(), bytes.size());
  return Error::None;
}

Error DecodeKey(const uint8_t* bytes, size_t count, Key& output) noexcept {
  output = {}; // Failed wire parsing must not leave an old descriptor usable.
  if (!bytes)
    return Error::NullBuffer;
  if (count != DescriptorBytes)
    return Error::DescriptorSize;
  if (std::memcmp(bytes, "WVL1", 4))
    return Error::Magic;
  if (ipc::ReadLe(bytes + 4, 2) != 1 || ipc::ReadLe(bytes + 6, 2) != DescriptorBytes)
    return Error::Version;
  Key key;
  key.connection = { ipc::ReadLe(bytes + 8, 8), ipc::ReadLe(bytes + 16, 8) };
  key.map = ipc::ReadLe(bytes + 24, 8);
  key.device = ipc::ReadLe(bytes + 32, 8);
  key.frame = ipc::ReadLe(bytes + 40, 8);
  key.generation = ipc::ReadLe(bytes + 48, 8);
  key.slot = uint32_t(ipc::ReadLe(bytes + 56, 4));
  key.bytes = uint32_t(ipc::ReadLe(bytes + 60, 4));
  auto error = Shape(key);
  if (error == Error::None)
    output = key;
  return error;
}

ReadPermit::ReadPermit(ReadPermit&& other) noexcept
: m_owner(other.m_owner), m_key(other.m_key), m_issued(other.m_issued) {
  other.m_owner = nullptr;
  other.m_key = {};
}

bool Snapshot::operator==(const Snapshot& other) const noexcept {
  return phase == other.phase && map == other.map && device == other.device && lastFrame == other.lastFrame
    && cursor == other.cursor
    && keys == other.keys && generations == other.generations && states == other.states;
}

SlotLedger::SlotLedger(ipc::Nonce connection, Limits limits) noexcept
: m_connection(connection), m_limits(limits) {
  if (connection.empty() || !limits.generationMax || !limits.frameMax)
    m_phase = Phase::Invalid;
}

bool SlotLedger::allFree() const noexcept {
  for (const auto& record : m_records)
    if (record.state != SlotState::Free)
      return false;
  return true;
}

Error SlotLedger::beginEpoch(uint64_t map, uint64_t device) noexcept {
  if (m_phase != Phase::Dormant && m_phase != Phase::Ready)
    return Error::Phase;
  if (!allFree())
    return Error::SlotState;
  if (!map || !device || map < m_map || device < m_device || (map == m_map && device == m_device))
    return Error::Epoch;
  m_map = map;
  m_device = device;
  m_frame = 0;
  m_phase = Phase::Active;
  return Error::None;
}

Error SlotLedger::endEpoch() noexcept {
  if (m_phase != Phase::Active)
    return Error::Phase;
  if (!allFree())
    return Error::SlotState;
  m_phase = Phase::Ready;
  return Error::None;
}

Error SlotLedger::reserve(uint64_t frame, uint32_t bytes, Key& output) noexcept {
  if (m_phase != Phase::Active)
    return Error::Phase;
  if (!bytes || bytes > SlotBytes)
    return Error::Size;
  if (m_frame >= m_limits.frameMax)
    return Error::FrameExhausted;
  if (!frame || frame != m_frame + 1)
    return Error::Frame;
  bool busy = false;
  for (uint32_t step = 0; step < SlotCount; ++step) {
    const auto slot = (m_cursor + step) % SlotCount;
    auto& record = m_records[slot];
    if (record.state != SlotState::Free) {
      busy = true;
      continue;
    }
    if (record.generation >= m_limits.generationMax)
      continue;
    Key key { m_connection, m_map, m_device, frame, record.generation + 1, slot, bytes };
    record.key = key;
    record.generation = key.generation;
    record.state = SlotState::Writing;
    m_frame = frame;
    m_cursor = (slot + 1) % SlotCount;
    output = key;
    return Error::None;
  }
  return busy ? Error::Capacity : Error::GenerationExhausted;
}

Error SlotLedger::check(const Key& key, SlotState state) const noexcept {
  if (key.slot >= SlotCount)
    return Error::Slot;
  const auto& record = m_records[key.slot];
  if (!(key == record.key))
    return Error::Lease;
  if (record.state != state)
    return Error::SlotState;
  return Error::None;
}

Error SlotLedger::publish(const Key& key) noexcept {
  if (m_phase != Phase::Active)
    return Error::Phase;
  auto error = check(key, SlotState::Writing);
  if (error == Error::None)
    m_records[key.slot].state = SlotState::Published;
  return error;
}

Error SlotLedger::cancelWrite(const Key& key) noexcept {
  if (m_phase != Phase::Active)
    return Error::Phase;
  auto error = check(key, SlotState::Writing);
  if (error == Error::None)
    m_records[key.slot].state = SlotState::Quarantined;
  return error;
}

Error SlotLedger::beginRead(const Key& key, ReadPermit& output) noexcept {
  if (m_phase != Phase::Active)
    return Error::Phase;
  if (output.active())
    return Error::OutputBusy;
  if (output.m_issued)
    return Error::OutputUsed;
  auto error = check(key, SlotState::Published);
  if (error == Error::None) {
    m_records[key.slot].state = SlotState::Reading;
    output.m_owner = this;
    output.m_key = key;
    output.m_issued = true;
  }
  return error;
}

Error SlotLedger::finishRead(ReadPermit& permit) noexcept {
  if (permit.m_owner != this)
    return Error::Permit;
  auto error = check(permit.m_key, SlotState::Reading);
  if (error == Error::None) {
    m_records[permit.m_key.slot].state = m_phase == Phase::Retired ? SlotState::Retired : SlotState::Free;
    permit.m_owner = nullptr;
    permit.m_key = {};
  }
  return error;
}

void SlotLedger::retire() noexcept {
  m_phase = Phase::Retired;
  for (auto& record : m_records)
    if (record.state != SlotState::Reading)
      record.state = SlotState::Retired;
}

bool SlotLedger::localReadersDone() const noexcept {
  for (const auto& record : m_records)
    if (record.state == SlotState::Reading)
      return false;
  return true;
}

Snapshot SlotLedger::snapshot() const noexcept {
  Snapshot result;
  result.phase = m_phase;
  result.map = m_map;
  result.device = m_device;
  result.lastFrame = m_frame;
  result.cursor = m_cursor;
  for (uint32_t i = 0; i < SlotCount; ++i) {
    result.keys[i] = m_records[i].key;
    result.generations[i] = m_records[i].generation;
    result.states[i] = m_records[i].state;
  }
  return result;
}

const char* ErrorName(Error e) noexcept {
  switch (e) {
#define RH_SLOT_ERROR(x) case Error::x: return #x;
    RH_SLOT_ERROR(None) RH_SLOT_ERROR(Phase) RH_SLOT_ERROR(Epoch)
    RH_SLOT_ERROR(Slot) RH_SLOT_ERROR(Size) RH_SLOT_ERROR(Frame) RH_SLOT_ERROR(FrameExhausted)
    RH_SLOT_ERROR(Capacity) RH_SLOT_ERROR(GenerationExhausted) RH_SLOT_ERROR(Lease)
    RH_SLOT_ERROR(SlotState) RH_SLOT_ERROR(Permit) RH_SLOT_ERROR(OutputBusy)
    RH_SLOT_ERROR(OutputUsed)
    RH_SLOT_ERROR(NullBuffer) RH_SLOT_ERROR(DescriptorSize) RH_SLOT_ERROR(Magic) RH_SLOT_ERROR(Version)
#undef RH_SLOT_ERROR
  }
  return "Unknown";
}
} // namespace warvk::host::slots
