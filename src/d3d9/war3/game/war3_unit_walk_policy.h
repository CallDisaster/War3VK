#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

namespace dxvk::war3::game {

// Game.dll 1.27a: these are the return PCs immediately after the two
// movement-state callbacks' virtual CUnit animation-property calls. The
// caller is part of the proof: property 5 alone also occurs in manual/native
// requests and must never be intercepted globally.
inline constexpr uintptr_t kWalkCallbackReturnA = 0x64C754u;
inline constexpr uintptr_t kWalkCallbackReturnB = 0x64C893u;

inline bool IsAutomaticWalkRequest(uintptr_t callerRva, int property,
                                   int flags, int reset) noexcept {
  return (callerRva == kWalkCallbackReturnA ||
          callerRva == kWalkCallbackReturnB) &&
         property == 5 && flags == 0x40 && reset == 1;
}

// Some 1.27a JASS handle-table entries own a CUnit directly rather than a
// CAgent. Admit that form only when its rawcode is a plausible four-character
// object ID and its virtual animation setter is the exact reviewed Game.dll
// function. A merely readable pointer or a nonzero rawcode is not proof.
inline bool IsDirectCUnitHandleProof(uint32_t rawcode,
                                     uintptr_t animationSetter,
                                     uintptr_t gameBase) noexcept {
  if (gameBase == 0u || animationSetter < gameBase ||
      animationSetter - gameBase != 0x67BB90u)
    return false;
  for (uint32_t i = 0u; i < 4u; ++i) {
    const uint32_t ch = (rawcode >> (8u * i)) & 0xFFu;
    if (ch < 0x20u || ch > 0x7Eu)
      return false;
  }
  return true;
}

// A disabled entry is keyed by the exact CUnit address. The JASS handle is
// retained as a second identity witness and must be re-resolved against the
// live handle table before a hook is allowed to suppress an engine request.
// The table never allocates in the game's animation path. Callers that cannot
// establish an exact unit/handle pair must use the original game behavior.
class UnitWalkPolicy final {
public:
  static constexpr uint32_t kCapacity = 512u;

  struct Identity {
    uintptr_t agent = 0u;
    uint32_t jHandle = 0u;
  };

  bool set(uintptr_t unit, Identity identity, bool enabled) noexcept {
    if (unit == 0u || identity.agent == 0u || identity.jHandle == 0u)
      return false;
    std::unique_lock lock(m_mutex);
    uint32_t empty = kCapacity;
    for (uint32_t i = 0; i < kCapacity; ++i) {
      const uint32_t slot = (hash(unit) + i) & (kCapacity - 1u);
      Entry& entry = m_entries[slot];
      if (entry.state == State::Active && entry.unit == unit) {
        if (enabled) {
          entry = {0u, {}, State::Tombstone};
          m_count.fetch_sub(1u, std::memory_order_release);
        } else {
          entry.identity = identity;
        }
        return true;
      }
      if (empty == kCapacity && entry.state != State::Active)
        empty = slot;
      if (entry.state == State::Empty)
        break;
    }
    if (enabled)
      return true;
    if (empty == kCapacity)
      return false;
    m_entries[empty] = {unit, identity, State::Active};
    m_count.fetch_add(1u, std::memory_order_release);
    return true;
  }

  Identity disabledIdentity(uintptr_t unit) const noexcept {
    if (unit == 0u || m_count.load(std::memory_order_acquire) == 0u)
      return {};
    std::shared_lock lock(m_mutex);
    for (uint32_t i = 0; i < kCapacity; ++i) {
      const Entry& entry = m_entries[(hash(unit) + i) & (kCapacity - 1u)];
      if (entry.state == State::Empty)
        break;
      if (entry.state == State::Active && entry.unit == unit)
        return entry.identity;
    }
    return {};
  }

  void erase(uintptr_t unit) noexcept {
    if (unit == 0u || m_count.load(std::memory_order_acquire) == 0u)
      return;
    std::unique_lock lock(m_mutex);
    for (uint32_t i = 0; i < kCapacity; ++i) {
      Entry& entry = m_entries[(hash(unit) + i) & (kCapacity - 1u)];
      if (entry.state == State::Empty)
        break;
      if (entry.state == State::Active && entry.unit == unit) {
          entry = {0u, {}, State::Tombstone};
        m_count.fetch_sub(1u, std::memory_order_release);
        return;
      }
    }
  }

  void reset() noexcept {
    std::unique_lock lock(m_mutex);
    m_entries = {};
    m_count.store(0u, std::memory_order_release);
  }

  uint32_t count() const noexcept {
    return m_count.load(std::memory_order_acquire);
  }

private:
  enum class State : uint8_t { Empty, Active, Tombstone };

  struct Entry {
    uintptr_t unit = 0u;
    Identity identity;
    State state = State::Empty;
  };

  static uint32_t hash(uintptr_t unit) noexcept {
    uint32_t x = static_cast<uint32_t>(unit >> 4u);
    x ^= x >> 16u;
    x *= 0x7FEB352Du;
    x ^= x >> 15u;
    return x & (kCapacity - 1u);
  }

  mutable std::shared_mutex m_mutex;
  std::array<Entry, kCapacity> m_entries{};
  std::atomic<uint32_t> m_count{0u};
};

} // namespace dxvk::war3::game
