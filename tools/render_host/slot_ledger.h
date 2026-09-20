#pragma once
#include "protocol.h"
#include <array>
#include <cstdint>

// RH1 CPU ownership core. Metadata is private to one serialized owner, not a
// shared-memory layout. This class neither maps memory nor proves GPU completion.
namespace warvk::host::slots {
inline constexpr uint32_t SlotCount = 4;
inline constexpr uint32_t SlotBytes = 65536;
inline constexpr size_t DescriptorBytes = 64;
inline constexpr uint32_t PoolBytes = SlotCount * SlotBytes;

enum class Error {
  None, Phase, Epoch, Slot, Size, Frame, FrameExhausted, Capacity,
  GenerationExhausted, Lease, SlotState, Permit, OutputBusy, OutputUsed,
  NullBuffer, DescriptorSize, Magic, Version
};
enum class Phase { Dormant, Active, Ready, Retired, Invalid };
enum class SlotState { Free, Writing, Published, Reading, Quarantined, Retired };

struct Key {
  ipc::Nonce connection;
  uint64_t map = 0, device = 0, frame = 0, generation = 0;
  uint32_t slot = 0, bytes = 0;
  bool operator==(const Key& other) const noexcept;
};
// Decode only validates descriptor shape. Ledger state is the only authority
// to admit a key. Failure clears the decoded key, never leaving an old parsed
// descriptor usable. Native struct padding/size never crosses the wire.
Error EncodeKey(const Key& key, uint8_t* output, size_t capacity) noexcept;
Error DecodeKey(const uint8_t* bytes, size_t count, Key& output) noexcept;
const char* ErrorName(Error error) noexcept;

class SlotLedger;
class ReadPermit {
public:
  ReadPermit() = default;
  ReadPermit(const ReadPermit&) = delete;
  ReadPermit& operator=(const ReadPermit&) = delete;
  ReadPermit(ReadPermit&& other) noexcept;
  ReadPermit& operator=(ReadPermit&&) = delete;
  // No automatic release/callback: dropping a permit is not consumption proof.
  ~ReadPermit() = default;
  bool active() const noexcept { return m_owner != nullptr; }
  // Observers get a copy, never an alias into the authoritative permit.
  Key key() const noexcept { return m_key; }
private:
  friend class SlotLedger;
  const SlotLedger* m_owner = nullptr; // Local identity only; never dereferenced.
  Key m_key;
  bool m_issued = false; // One-shot object; never rebind a late holder to a new lease.
};

struct Limits {
  uint64_t generationMax = UINT64_MAX;
  uint64_t frameMax = UINT64_MAX;
};
struct Snapshot {
  Phase phase = Phase::Invalid;
  uint64_t map = 0, device = 0, lastFrame = 0;
  uint32_t cursor = 0;
  std::array<Key, SlotCount> keys{};
  std::array<uint64_t, SlotCount> generations{};
  std::array<SlotState, SlotCount> states{};
  bool operator==(const Snapshot& other) const noexcept;
};

class SlotLedger {
public:
  explicit SlotLedger(ipc::Nonce connection, Limits limits = {}) noexcept;
  SlotLedger(const SlotLedger&) = delete;
  SlotLedger& operator=(const SlotLedger&) = delete;
  SlotLedger(SlotLedger&&) = delete;
  SlotLedger& operator=(SlotLedger&&) = delete;

  Error beginEpoch(uint64_t map, uint64_t device) noexcept;
  Error endEpoch() noexcept;
  Error reserve(uint64_t frame, uint32_t bytes, Key& output) noexcept;
  Error publish(const Key& key) noexcept;
  // Cancel requests never prove the writer stopped touching shared storage.
  // Quarantine until the whole connection retires, not immediate slot reuse.
  Error cancelWrite(const Key& key) noexcept;
  Error beginRead(const Key& key, ReadPermit& output) noexcept;
  Error finishRead(ReadPermit& permit) noexcept;
  void retire() noexcept;
  bool localReadersDone() const noexcept;
  // Observation is not a lease or a destruction permission.
  Snapshot snapshot() const noexcept;
private:
  struct Record {
    Key key;
    uint64_t generation = 0;
    SlotState state = SlotState::Free;
  };
  Error check(const Key& key, SlotState state) const noexcept;
  bool allFree() const noexcept;
  ipc::Nonce m_connection;
  Limits m_limits;
  Phase m_phase = Phase::Dormant;
  uint64_t m_map = 0, m_device = 0, m_frame = 0;
  uint32_t m_cursor = 0;
  std::array<Record, SlotCount> m_records{};
};
} // namespace warvk::host::slots
