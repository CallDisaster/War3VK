#include "../war3_frame_hash_index.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using dxvk::war3::render::War3FrameHashIndex;

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_frame_hash_index_test: " << message << '\n';
  return condition;
}

bool testDuplicateKeysAndEarlyExit() {
  War3FrameHashIndex index;
  index.reserve(16u);
  index.insert(7u, 11u);
  index.insert(9u, 13u);
  index.insert(7u, 17u);

  std::vector<uint32_t> values;
  index.forEach(7u, [&](uint32_t value) {
    values.push_back(value);
    return true;
  });
  if (!require(values.size() == 2u, "duplicate key lost a value") ||
      !require(values[0] == 17u && values[1] == 11u,
               "duplicate traversal changed insertion-chain order"))
    return false;

  uint32_t calls = 0u;
  index.forEach(7u, [&](uint32_t) {
    ++calls;
    return false;
  });
  return require(calls == 1u, "early exit visited more than one value");
}

bool testClearRetainsStorageAndRejectsOldGeneration() {
  War3FrameHashIndex index;
  index.reserve(32u);
  index.insert(5u, 3u);
  index.insert(5u, 4u);
  const size_t retainedCapacity = index.capacity();
  index.clear();

  uint32_t staleVisits = 0u;
  index.forEach(5u, [&](uint32_t) {
    ++staleVisits;
    return true;
  });
  if (!require(staleVisits == 0u, "clear exposed stale generation") ||
      !require(index.empty(), "clear did not empty active nodes") ||
      !require(index.capacity() == retainedCapacity,
               "clear released retained node storage"))
    return false;

  index.insert(5u, 9u);
  uint32_t value = 0u;
  index.forEach(5u, [&](uint32_t candidate) {
    value = candidate;
    return true;
  });
  return require(value == 9u, "new generation did not replace old chain");
}

bool testGrowthPreservesCandidates() {
  War3FrameHashIndex index;
  for (uint32_t i = 0u; i < 200u; ++i)
    index.insert(uint64_t(i % 23u), i);

  uint32_t count = 0u;
  index.forEach(3u, [&](uint32_t value) {
    if ((value % 23u) == 3u)
      ++count;
    return true;
  });
  return require(count == 9u, "rehash lost matching candidates");
}

} // namespace

int main() {
  return testDuplicateKeysAndEarlyExit() &&
          testClearRetainsStorageAndRejectsOldGeneration() &&
          testGrowthPreservesCandidates()
      ? 0 : 1;
}
