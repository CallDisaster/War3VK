#pragma once

#include "war3_cpu_readable_buffer_span.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace dxvk::war3::memory {

// Value-only identity for an exact index-domain observation. Addresses are
// identity tokens only: this cache never owns or dereferences a resource.
// Every lifetime-relevant generation and both render epochs participate in
// equality, so a map/device transition or source mutation is always a miss.
struct War3ExactIndexDomainObserverKey {
  uint64_t mapEpoch = 0u;
  uint64_t deviceEpoch = 0u;
  uintptr_t ownerIdentity = 0u;
  uintptr_t spanDataIdentity = 0u;
  uint64_t identityGeneration = 0u;
  uint64_t allocationGeneration = 0u;
  uint64_t contentGeneration = 0u;
  uint64_t spanLength = 0u;
  uint32_t indexElementBytes = 0u;
  uint32_t indexCount = 0u;
  int32_t baseVertex = 0;
  uint32_t vertexCapacity = 0u;

  constexpr bool valid() const noexcept {
    return mapEpoch != 0u && deviceEpoch != 0u && ownerIdentity != 0u &&
        spanDataIdentity != 0u && identityGeneration != 0u &&
        allocationGeneration != 0u && contentGeneration != 0u &&
        spanLength != 0u &&
        (indexElementBytes == 2u || indexElementBytes == 4u) &&
        indexCount != 0u && vertexCapacity != 0u;
  }
};

constexpr bool operator==(
    const War3ExactIndexDomainObserverKey& a,
    const War3ExactIndexDomainObserverKey& b) noexcept {
  return a.mapEpoch == b.mapEpoch && a.deviceEpoch == b.deviceEpoch &&
      a.ownerIdentity == b.ownerIdentity &&
      a.spanDataIdentity == b.spanDataIdentity &&
      a.identityGeneration == b.identityGeneration &&
      a.allocationGeneration == b.allocationGeneration &&
      a.contentGeneration == b.contentGeneration &&
      a.spanLength == b.spanLength &&
      a.indexElementBytes == b.indexElementBytes &&
      a.indexCount == b.indexCount && a.baseVertex == b.baseVertex &&
      a.vertexCapacity == b.vertexCapacity;
}

constexpr bool operator!=(
    const War3ExactIndexDomainObserverKey& a,
    const War3ExactIndexDomainObserverKey& b) noexcept {
  return !(a == b);
}

constexpr uint64_t War3ExactIndexDomainObserverKeyHash(
    const War3ExactIndexDomainObserverKey& key) noexcept {
  auto fold = [](uint64_t hash, uint64_t value) constexpr noexcept {
    return hash ^
        (value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u));
  };
  uint64_t hash = 0xcbf29ce484222325ull;
  hash = fold(hash, key.mapEpoch);
  hash = fold(hash, key.deviceEpoch);
  hash = fold(hash, uint64_t(key.ownerIdentity));
  hash = fold(hash, uint64_t(key.spanDataIdentity));
  hash = fold(hash, key.identityGeneration);
  hash = fold(hash, key.allocationGeneration);
  hash = fold(hash, key.contentGeneration);
  hash = fold(hash, key.spanLength);
  hash = fold(hash, uint64_t(key.indexElementBytes) |
                        (uint64_t(key.indexCount) << 32u));
  hash = fold(hash, uint64_t(uint32_t(key.baseVertex)) |
                        (uint64_t(key.vertexCapacity) << 32u));
  return hash;
}

enum class War3ExactIndexDomainObserverLookup : uint8_t {
  InvalidKey = 0u,
  Hit,
  MissEmpty,
  MissCollision,
};

enum class War3ExactIndexDomainObserverStore : uint8_t {
  InvalidKey = 0u,
  Updated,
  Inserted,
  Replaced,
};

// Small fixed, set-associative cache for development exact-domain consumers.
// It stores only a derived POD domain and never resource ownership, mapped
// bytes or a Vulkan binding. Every lookup therefore still starts from a
// current readable span and its complete generation key. The replacement
// order is deterministic: first invalid way, otherwise least recently used
// with the lowest way index as the tie-break.
template <size_t SetCount = 256u, size_t WayCount = 4u>
class War3ExactIndexDomainObserverCache {
  static_assert(SetCount != 0u && (SetCount & (SetCount - 1u)) == 0u);
  static_assert(WayCount != 0u);

  struct Entry {
    War3ExactIndexDomainObserverKey key = {};
    War3ExactIndexVertexDomain domain = {};
    uint64_t lastUseSerial = 0u;
    bool valid = false;
  };

public:
  War3ExactIndexDomainObserverLookup lookup(
      const War3ExactIndexDomainObserverKey& key,
      War3ExactIndexVertexDomain& domain) noexcept {
    if (!key.valid())
      return War3ExactIndexDomainObserverLookup::InvalidKey;

    auto& set = m_sets[setIndex(key)];
    bool occupied = false;
    for (auto& entry : set) {
      occupied |= entry.valid;
      if (entry.valid && entry.key == key) {
        entry.lastUseSerial = nextSerial();
        domain = entry.domain;
        return War3ExactIndexDomainObserverLookup::Hit;
      }
    }
    return occupied
        ? War3ExactIndexDomainObserverLookup::MissCollision
        : War3ExactIndexDomainObserverLookup::MissEmpty;
  }

  War3ExactIndexDomainObserverStore store(
      const War3ExactIndexDomainObserverKey& key,
      const War3ExactIndexVertexDomain& domain) noexcept {
    if (!key.valid())
      return War3ExactIndexDomainObserverStore::InvalidKey;

    auto& set = m_sets[setIndex(key)];
    for (auto& entry : set) {
      if (entry.valid && entry.key == key) {
        entry.domain = domain;
        entry.lastUseSerial = nextSerial();
        return War3ExactIndexDomainObserverStore::Updated;
      }
    }

    size_t victim = 0u;
    bool replacing = true;
    for (size_t way = 0u; way < WayCount; ++way) {
      if (!set[way].valid) {
        victim = way;
        replacing = false;
        break;
      }
      if (set[way].lastUseSerial < set[victim].lastUseSerial)
        victim = way;
    }

    set[victim].key = key;
    set[victim].domain = domain;
    set[victim].lastUseSerial = nextSerial();
    set[victim].valid = true;
    return replacing ? War3ExactIndexDomainObserverStore::Replaced
                     : War3ExactIndexDomainObserverStore::Inserted;
  }

private:
  size_t setIndex(const War3ExactIndexDomainObserverKey& key) const noexcept {
    return size_t(War3ExactIndexDomainObserverKeyHash(key)) &
        (SetCount - 1u);
  }

  uint64_t nextSerial() noexcept {
    if (m_accessSerial != std::numeric_limits<uint64_t>::max())
      ++m_accessSerial;
    return m_accessSerial;
  }

  std::array<std::array<Entry, WayCount>, SetCount> m_sets = {};
  uint64_t m_accessSerial = 0u;
};

static_assert(std::is_standard_layout_v<War3ExactIndexDomainObserverKey>);
static_assert(std::is_trivially_copyable_v<War3ExactIndexDomainObserverKey>);

}  // namespace dxvk::war3::memory
