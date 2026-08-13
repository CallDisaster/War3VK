#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace dxvk::war3::render {

// Frame-local multi-index with retained storage. Unlike unordered_multimap,
// clear() does not destroy one allocation per published palette. Generation-
// tagged buckets make reset proportional to zero while complete-key callers
// still validate every hash candidate before accepting it.
class War3FrameHashIndex final {
public:
  static constexpr uint32_t InvalidNode =
      (std::numeric_limits<uint32_t>::max)();

  void clear() noexcept {
    m_nodes.clear();
    if (++m_generation == 0u) {
      for (auto& bucket : m_buckets)
        bucket.generation = 0u;
      m_generation = 1u;
    }
  }

  bool empty() const noexcept {
    return m_nodes.empty();
  }

  size_t size() const noexcept {
    return m_nodes.size();
  }

  size_t capacity() const noexcept {
    return m_nodes.capacity();
  }

  void reserve(size_t valueCount) {
    if (valueCount > size_t(InvalidNode))
      valueCount = size_t(InvalidNode);
    m_nodes.reserve(valueCount);

    size_t bucketCount = 8u;
    const size_t wanted = valueCount > (size_t(-1) / 2u)
        ? size_t(-1) : valueCount * 2u;
    const size_t maxPowerOfTwo =
        size_t(1u) << (std::numeric_limits<size_t>::digits - 1u);
    while (bucketCount < wanted) {
      if (bucketCount >= maxPowerOfTwo) {
        bucketCount = maxPowerOfTwo;
        break;
      }
      bucketCount *= 2u;
    }
    if (bucketCount > m_buckets.size())
      rebuildBuckets(bucketCount);
  }

  void insert(uint64_t key, uint32_t value) {
    if (m_nodes.size() >= size_t(InvalidNode))
      return;
    if (m_buckets.empty())
      rebuildBuckets(8u);
    if ((m_nodes.size() + 1u) * 4u > m_buckets.size() * 3u)
      rebuildBuckets(m_buckets.size() * 2u);

    const size_t bucketIndex = hashBucket(key, m_buckets.size());
    auto& bucket = m_buckets[bucketIndex];
    const uint32_t previous = bucket.generation == m_generation
        ? bucket.head : InvalidNode;
    m_nodes.push_back(Node{key, value, previous});
    bucket.generation = m_generation;
    bucket.head = uint32_t(m_nodes.size() - 1u);
  }

  template <typename Callback>
  void forEach(uint64_t key, Callback&& callback) const {
    if (m_buckets.empty())
      return;
    const auto& bucket = m_buckets[hashBucket(key, m_buckets.size())];
    if (bucket.generation != m_generation)
      return;
    uint32_t nodeIndex = bucket.head;
    while (nodeIndex != InvalidNode && nodeIndex < m_nodes.size()) {
      const auto& node = m_nodes[nodeIndex];
      if (node.key == key && !callback(node.value))
        return;
      nodeIndex = node.next;
    }
  }

private:
  struct Bucket {
    uint32_t generation = 0u;
    uint32_t head = InvalidNode;
  };

  struct Node {
    uint64_t key = 0u;
    uint32_t value = 0u;
    uint32_t next = InvalidNode;
  };

  static size_t hashBucket(uint64_t key, size_t bucketCount) noexcept {
    key ^= key >> 33u;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 33u;
    return size_t(key) & (bucketCount - 1u);
  }

  void rebuildBuckets(size_t bucketCount) {
    bucketCount = (std::max)(bucketCount, size_t(8u));
    std::vector<Bucket> buckets(bucketCount);
    for (uint32_t i = 0u; i < m_nodes.size(); ++i) {
      auto& node = m_nodes[i];
      auto& bucket = buckets[hashBucket(node.key, bucketCount)];
      node.next = bucket.generation == m_generation
          ? bucket.head : InvalidNode;
      bucket.generation = m_generation;
      bucket.head = i;
    }
    m_buckets.swap(buckets);
  }

  std::vector<Bucket> m_buckets;
  std::vector<Node> m_nodes;
  uint32_t m_generation = 1u;
};

} // namespace dxvk::war3::render
