#pragma once
#include "recorder_event_wire.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

// 64-bit host-side long-term CPU event history (recorder-offload E1). The
// caller is single-threaded; no Windows/Vulkan/IPC/disk dependency and no
// pointer-bitness assumption. See docs/plan/2026-09-16-recorder-cpu-offload-contract.md.
namespace warvk::host::recorder {

enum class StoreState : uint32_t { Empty = 0, Recording, Frozen, Fault };

enum class StoreError : uint32_t {
  None = 0,
  WireDecode,        // packet failed the real WVE1 codec; summary().wireError has detail
  BadState,          // op not allowed now: early/late Data, duplicate Begin/Seal
  SessionMismatch,   // header or record session differs from the armed session
  OrdinalMismatch,   // packet ordinal is not the expected next one (skip/replay)
  BadBeginFields,    // Begin shape: ordinal 1, count 0, no trigger/totals, capacity 4..262144
  BadDataFields,     // Data shape: count 1..160, no totals, capacity/session unchanged
  BadSealFields,     // Seal shape: count 0, reason 1..4, capacity/session unchanged
  SequenceGap,       // records are not one continuous 1-based sequence
  TriggerConflict,   // a fixed cut was later changed
  TriggerRegressed,  // first cut < lastSequence (would reclassify stored records)
  TriggerNotCovered, // Seal cut beyond lastSequence
  PostOverflow,      // post region index beyond capacity; post is never overwritten
  TotalsMismatch,    // Seal statistics do not close exactly against stored facts
  AllocationFailed,
  SequenceOverflow,  // advancing lastSequence would wrap uint64; wrap never becomes legal
  OrdinalOverflow,   // nextOrdinal == UINT64_MAX; never wraps to a legal 0
};

struct Summary {
  StoreState state = StoreState::Empty;
  uint64_t session = 0;
  uint64_t nextOrdinal = 1;
  uint64_t lastSequence = 0;
  uint64_t trigger = NoTrigger;
  uint32_t capacity = 0;
  uint64_t accepted = 0, attempted = 0, lost = 0, evicted = 0, retained = 0;
  uint32_t reason = 0; // evidence::Reason numbering carried from Seal
  uint64_t storageBytes = 0;
  WireError wireError = WireError::None;
  StoreError storeError = StoreError::None;
};

class HistoryStore {
public:
  HistoryStore() = default;
  HistoryStore(const HistoryStore&) = delete;
  HistoryStore& operator=(const HistoryStore&) = delete;

  // Decodes the real packet itself; a caller-supplied View is never trusted.
  // Any invalid packet or allocation failure is a permanent Fault; a faulted
  // or frozen object never reopens, only a new object starts a new session.
  StoreError accept(const uint8_t* packet, size_t size);
  Summary summary() const noexcept;
  // Frozen only, ascending sequence, bounded pointer sort (never a second
  // Event copy). A visitor failure or a non-frozen state returns false and
  // never claims an export completed.
  bool visitFrozen(const std::function<bool(const Event&)>& visitor) const;

private:
  StoreError fail(StoreError e) noexcept;
  StoreError failWire(WireError w) noexcept;
  StoreError acceptBegin(const Header& h);
  StoreError acceptData(const Header& h, const uint8_t* events);
  StoreError acceptSeal(const Header& h);

  StoreState m_state = StoreState::Empty;
  uint64_t m_session = 0, m_nextOrdinal = 1, m_lastSequence = 0;
  uint64_t m_trigger = NoTrigger;
  uint32_t m_capacity = 0, m_pre = 0; // post = capacity - pre
  uint64_t m_accepted = 0, m_evicted = 0;
  uint64_t m_attempted = 0, m_lost = 0; // totals arrive with Seal only
  uint32_t m_reason = 0;
  WireError m_wireError = WireError::None;
  StoreError m_storeError = StoreError::None;
  std::unique_ptr<Event[]> m_storage;   // one allocation at Begin
};
} // namespace warvk::host::recorder