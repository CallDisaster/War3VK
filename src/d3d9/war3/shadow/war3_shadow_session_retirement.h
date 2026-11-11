#pragma once

#include <cassert>
#include <cstdint>
#include <limits>
#include <list>
#include <new>
#include <type_traits>
#include <utility>

namespace dxvk::war3::shadow {

template<typename T>
void TransferShadowSessionOwner(T& destination, T& source) noexcept {
  static_assert(std::is_nothrow_move_assignable_v<T>,
                "Owner transfer must not allocate or throw after preparation");
  destination = std::move(source);
}

// Logical persistent allocation charges, NOT physical residency or every Rc
// retained by the GPU. Retained-source aliases carry zero additional charge.
// The existing live GC lifetime is unchanged; whole-session retirement keeps
// its charge until the existing completion serial authorizes collection.
struct PersistentGeometryCharges {
  uint64_t live = 0;
  uint64_t retired = 0;

  uint64_t total() const noexcept {
    const auto max = std::numeric_limits<uint64_t>::max();
    return live > max - retired ? max : live + retired;
  }
  bool canAdmit(uint64_t bytes, uint64_t cap) const noexcept {
    return bytes <= cap && retired <= cap - bytes
        && live <= cap - bytes - retired;
  }
  void releaseLive(uint64_t bytes) noexcept {
    // Preserve the old saturating GC policy without consuming retired charges.
    live = bytes <= live ? live - bytes : 0;
  }
};

// A stable node avoids copying/moving all old sessions on vector growth.
// Allocation occurs before quarantine/owner mutation. Preparation failure or
// an exception before commit leaves both live owners and charges untouched.
// Session is the production payload; tests use the same transition algorithm.
template<typename Session, typename Allocator = std::allocator<Session>>
class PreparedShadowSessionRetirement {
public:
  using Queue = std::list<Session, Allocator>;

  explicit PreparedShadowSessionRetirement(Queue& queue) : m_queue(queue) {
    try {
      m_slot = m_queue.emplace(m_queue.end());
      m_prepared = true;
    } catch (const std::bad_alloc&) {
      // No owner has moved; caller must not acknowledge/reopen the transition.
    }
  }
  PreparedShadowSessionRetirement(const PreparedShadowSessionRetirement&) = delete;
  PreparedShadowSessionRetirement& operator=(const PreparedShadowSessionRetirement&) = delete;
  ~PreparedShadowSessionRetirement() {
    if (m_prepared && !m_committed)
      m_queue.erase(m_slot);
  }
  explicit operator bool() const noexcept { return m_prepared; }

  template<typename Transfer>
  Session& commit(PersistentGeometryCharges& charges, uint64_t mapEpoch,
                  uint64_t retireSerial, Transfer&& transfer) noexcept {
    static_assert(std::is_nothrow_invocable_v<Transfer, Session&>);
    assert(m_prepared && !m_committed);
    assert(charges.live <= std::numeric_limits<uint64_t>::max() - charges.retired);
    auto& session = *m_slot;
    session.mapEpoch = mapEpoch;
    session.retireSerial = retireSerial;
    session.persistentChargeBytes = charges.live;
    // The device adapter transfers ONLY owners here; no locks, allocation,
    // alias cleanup, census formatting or GPU calls inside this transaction.
    transfer(session);
    charges.retired += std::exchange(charges.live, 0);
    m_committed = true;
    return session;
  }

private:
  Queue& m_queue;
  typename Queue::iterator m_slot;
  bool m_prepared = false;
  bool m_committed = false;
};

template<typename Session, typename Allocator>
size_t CollectRetiredShadowSessions(std::list<Session, Allocator>& queue,
                                   PersistentGeometryCharges& charges,
                                   uint64_t completedSerial) noexcept {
  size_t collected = 0;
  for (auto it = queue.begin(); it != queue.end();) {
    if (it->retireSerial == 0 || it->retireSerial > completedSerial
        || it->persistentChargeBytes > charges.retired) {
      ++it; // Unknown completion or inconsistent charge cannot release owners.
      continue;
    }
    const uint64_t bytes = it->persistentChargeBytes;
    it = queue.erase(it);
    charges.retired -= bytes;
    ++collected;
  }
  return collected;
}

} // namespace dxvk::war3::shadow
