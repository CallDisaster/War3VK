#include "recorder_history_store.h"
#include <algorithm>
#include <new>
#include <vector>

namespace warvk::host::recorder {

StoreError HistoryStore::fail(StoreError e) noexcept {
  m_storeError = e;
  m_state = StoreState::Fault;
  return e;
}
StoreError HistoryStore::failWire(WireError w) noexcept {
  m_wireError = w;
  return fail(StoreError::WireDecode);
}

StoreError HistoryStore::accept(const uint8_t* packet, size_t size) {
  if (m_state == StoreState::Fault) return m_storeError; // permanent
  View view;
  const WireError wire = Decode(packet, size, view); // self-decode; no caller View trusted
  if (wire != WireError::None) return failWire(wire);
  try {
    switch (view.header.op) {
      case Op::Begin: return acceptBegin(view.header);
      case Op::Data:  return acceptData(view.header, view.events);
      case Op::Seal:  return acceptSeal(view.header);
    }
  } catch (const std::bad_alloc&) {
    return fail(StoreError::AllocationFailed);
  }
  return fail(StoreError::BadState); // unreachable: Decode rejects unknown ops
}

StoreError HistoryStore::acceptBegin(const Header& h) {
  if (m_state != StoreState::Empty) return fail(StoreError::BadState); // duplicate/late Begin
  if (h.ordinal != 1 || h.count != 0 || h.trigger != NoTrigger ||
      h.attempted || h.lost || h.accepted || h.reason) return fail(StoreError::BadBeginFields);
  if (h.capacity < 4 || h.capacity > MaxHistoryEvents) return fail(StoreError::BadBeginFields);
  // One allocation for the whole history; failure is a permanent Fault.
  m_storage = std::make_unique<Event[]>(h.capacity);
  m_capacity = h.capacity;
  m_pre = h.capacity - h.capacity / 4;
  m_session = h.session;
  m_nextOrdinal = 2;
  m_state = StoreState::Recording;
  return StoreError::None;
}

StoreError HistoryStore::acceptData(const Header& h, const uint8_t* events) {
  if (m_state != StoreState::Recording) return fail(StoreError::BadState); // early/late Data
  if (h.session != m_session) return fail(StoreError::SessionMismatch);
  if (h.capacity != m_capacity) return fail(StoreError::BadDataFields);
  if (h.ordinal != m_nextOrdinal) return fail(StoreError::OrdinalMismatch);
  if (h.count == 0 || h.count > MaxEvents) return fail(StoreError::BadDataFields);
  if (h.attempted || h.lost || h.accepted || h.reason) return fail(StoreError::BadDataFields);
  if (m_nextOrdinal == UINT64_MAX) return fail(StoreError::OrdinalOverflow); // never wraps to a legal 0
  if (h.count > UINT64_MAX - 1 - m_lastSequence) return fail(StoreError::SequenceOverflow); // no wrap
  // Once fixed, every later packet must repeat the cut exactly (NoTrigger is a
  // change); a first cut must not reclassify already stored records.
  uint64_t cut = m_trigger;
  if (m_trigger != NoTrigger) {
    if (h.trigger != m_trigger) return fail(StoreError::TriggerConflict);
  } else if (h.trigger != NoTrigger) {
    if (h.trigger < m_lastSequence) return fail(StoreError::TriggerRegressed);
    cut = h.trigger;
  }
  // Full validation of every record before any write: no partial commit.
  for (uint32_t i = 0; i < h.count; ++i) {
    Event e;
    const WireError w = DecodeEvent(events + size_t(i) * EventBytes, EventBytes, e);
    if (w != WireError::None) return failWire(w);
    if (e.session != m_session) return fail(StoreError::SessionMismatch);
    if (e.sequence != m_lastSequence + 1 + i) return fail(StoreError::SequenceGap);
    if (cut != NoTrigger && e.sequence > cut) {
      // Compare against the post size without adding m_pre: no uint64 wrap.
      if (e.sequence - cut - 1 >= m_capacity - m_pre) return fail(StoreError::PostOverflow);
    }
  }
  // Commit: pre-ring (sequence-1)%pre overwrites count as evicted; post slots
  // pre+(sequence-cut-1) are strictly unique and never overwritten.
  for (uint32_t i = 0; i < h.count; ++i) {
    Event e;
    DecodeEvent(events + size_t(i) * EventBytes, EventBytes, e); // already proven valid above
    uint64_t index;
    if (cut == NoTrigger || e.sequence <= cut) {
      index = (e.sequence - 1) % m_pre;
      if (m_storage[size_t(index)].sequence) ++m_evicted;
    } else {
      index = m_pre + (e.sequence - cut - 1);
    }
    m_storage[size_t(index)] = e;
  }
  if (cut != NoTrigger && m_trigger == NoTrigger) m_trigger = cut;
  m_lastSequence += h.count;
  m_accepted += h.count; // processed events, including ones later evicted
  ++m_nextOrdinal;
  return StoreError::None;
}

StoreError HistoryStore::acceptSeal(const Header& h) {
  if (m_state != StoreState::Recording) return fail(StoreError::BadState); // duplicate/early Seal
  if (h.session != m_session) return fail(StoreError::SessionMismatch);
  if (h.capacity != m_capacity) return fail(StoreError::BadSealFields);
  if (h.ordinal != m_nextOrdinal) return fail(StoreError::OrdinalMismatch);
  if (h.count != 0) return fail(StoreError::BadSealFields);
  if (h.reason < 1 || h.reason > 4) return fail(StoreError::BadSealFields);
  if (m_nextOrdinal == UINT64_MAX) return fail(StoreError::OrdinalOverflow); // never wraps to a legal 0
  // Once fixed, Seal must repeat the cut exactly (NoTrigger is a change).
  uint64_t cut = m_trigger;
  if (m_trigger != NoTrigger) {
    if (h.trigger != m_trigger) return fail(StoreError::TriggerConflict);
  } else if (h.trigger != NoTrigger) {
    if (h.trigger < m_lastSequence) return fail(StoreError::TriggerRegressed);
    cut = h.trigger;
  }
  if (cut != NoTrigger && cut > m_lastSequence) return fail(StoreError::TriggerNotCovered);
  // Statistics must close exactly against the stored facts.
  if (h.accepted != m_accepted || h.attempted < h.accepted ||
      h.lost != h.attempted - h.accepted) return fail(StoreError::TotalsMismatch);
  if (m_lastSequence != m_accepted) return fail(StoreError::TotalsMismatch); // invariant
  if (cut != NoTrigger && m_trigger == NoTrigger) m_trigger = cut;
  m_attempted = h.attempted;
  m_lost = h.lost;
  m_reason = h.reason;
  m_state = StoreState::Frozen;
  ++m_nextOrdinal;
  return StoreError::None;
}

Summary HistoryStore::summary() const noexcept {
  Summary s;
  s.state = m_state;
  s.session = m_session;
  s.nextOrdinal = m_nextOrdinal;
  s.lastSequence = m_lastSequence;
  s.trigger = m_trigger;
  s.capacity = m_capacity;
  s.accepted = m_accepted;
  s.attempted = m_attempted;
  s.lost = m_lost;
  s.evicted = m_evicted;
  s.retained = m_accepted - m_evicted;
  s.reason = m_reason;
  s.storageBytes = uint64_t(m_capacity) * sizeof(Event);
  s.wireError = m_wireError;
  s.storeError = m_storeError;
  return s;
}

bool HistoryStore::visitFrozen(const std::function<bool(const Event&)>& visitor) const {
  if (m_state != StoreState::Frozen || !visitor) return false;
  try {
    // Bounded pointer/index sort only; never a second Event-sized copy.
    std::vector<const Event*> order;
    order.reserve(m_capacity);
    for (uint32_t i = 0; i < m_capacity; ++i)
      if (m_storage[i].sequence) order.push_back(&m_storage[i]);
    std::sort(order.begin(), order.end(),
              [](const Event* a, const Event* b) { return a->sequence < b->sequence; });
    for (const Event* e : order) {
      if (!visitor(*e)) return false; // caller failure is not an export completion
      // A visitor must not reenter accept(): any accept while frozen faults the
      // store, and this visit then fails instead of reporting success.
      if (m_state != StoreState::Frozen) return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}
} // namespace warvk::host::recorder
