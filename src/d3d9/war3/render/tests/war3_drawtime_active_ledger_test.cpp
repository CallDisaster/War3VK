#include "../war3_drawtime_active_ledger.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace {

struct Key {
  uint64_t object = 0u;
  uint32_t part = 0u;

  bool operator==(const Key& other) const noexcept {
    return object == other.object && part == other.part;
  }
};

struct KeyHash {
  size_t operator()(const Key& key) const noexcept {
    return size_t(key.object ^ (uint64_t(key.part) << 32u));
  }
};

struct Entry {
  dxvk::war3::render::War3DrawTimeActiveLedger<Key>::EntryStamp stamp = {};
  bool complete = true;
};

using Ledger = dxvk::war3::render::War3DrawTimeActiveLedger<Key>;
using Map = std::unordered_map<Key, Entry, KeyHash>;

std::vector<Key> collectActive(const Ledger& ledger, const Map& entries,
                               uint64_t frameSerial) {
  std::vector<Key> result;
  if (!ledger.usableForFrame(frameSerial)) {
    for (const auto& pair : entries) {
      if (pair.second.complete)
        result.push_back(pair.first);
    }
    return result;
  }

  for (const auto& record : ledger.records()) {
    const auto found = entries.find(record.key);
    if (found != entries.end() && found->second.complete &&
        Ledger::matches(record, found->second.stamp)) {
      result.push_back(found->first);
    }
  }
  return result;
}

}  // namespace

int main() {
  Ledger ledger;
  Map entries;
  const Key keyA{1u, 10u};
  const Key keyB{2u, 20u};

  ledger.beginFrame(7u);
  auto& a = entries[keyA];
  assert(ledger.activate(7u, keyA, a.stamp));
  assert(ledger.activate(7u, keyA, a.stamp));
  assert(ledger.records().size() == 1u);

  // Rehash cannot invalidate a value-key ledger record.
  entries.reserve(1024u);
  auto active = collectActive(ledger, entries, 7u);
  assert(active.size() == 1u && active[0] == keyA);

  // Erasing and rebuilding the same key receives a new ordinal. The stale
  // record fails stamp validation while the replacement remains visible once.
  const uint64_t oldOrdinal = a.stamp.activationOrdinal;
  entries.erase(keyA);
  auto& replacement = entries[keyA];
  assert(ledger.activate(7u, keyA, replacement.stamp));
  assert(replacement.stamp.activationOrdinal != oldOrdinal);
  active = collectActive(ledger, entries, 7u);
  assert(active.size() == 1u && active[0] == keyA);

  // Tombstoned keys simply miss their canonical map lookup.
  entries.erase(keyA);
  assert(collectActive(ledger, entries, 7u).empty());

  // A new frame clears logical records while retaining the ledger storage and
  // monotonic ordinal domain. Incomplete entries are not resurrected.
  ledger.beginFrame(8u);
  auto& b = entries[keyB];
  b.complete = false;
  assert(ledger.activate(8u, keyB, b.stamp));
  assert(collectActive(ledger, entries, 8u).empty());
  b.complete = true;
  active = collectActive(ledger, entries, 8u);
  assert(active.size() == 1u && active[0] == keyB);
  assert(!ledger.usableForFrame(7u));

  // A session reset never exposes old keys. Until the next frame is begun,
  // the caller safely falls back to its canonical full scan.
  ledger.resetSession();
  assert(!ledger.usableForFrame(8u));
  active = collectActive(ledger, entries, 8u);
  assert(active.size() == 1u && active[0] == keyB);

  // Ordinal exhaustion disables the optimization. It does not return an
  // incomplete active set; callers fall back to the full map scan.
  Ledger exhausted(std::numeric_limits<uint64_t>::max());
  exhausted.beginFrame(9u);
  Entry exhaustedEntry = {};
  assert(!exhausted.activate(9u, keyA, exhaustedEntry.stamp));
  assert(!exhausted.usableForFrame(9u));

  return 0;
}
