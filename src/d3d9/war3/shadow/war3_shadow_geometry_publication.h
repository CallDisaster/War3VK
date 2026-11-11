#pragma once

#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace dxvk::war3::shadow {

// Single publication algorithm shared by the device and allocation-failure
// tests. GPU preparation, domain admission and GC remain with the device owner.
// This boundary starts AFTER GC; it must never restore a pre-GC registry slot.
// Restricted to the audited std::unordered_map and priority_queue<vector>
// adapters: single insert / vector growth must have the strong guarantee and
// erase(iterator) / allocator cleanup must not throw. The assertions below do
// not prove those guarantees for arbitrary custom containers. Candidate entry,
// slot and token must not alias an existing map entry that rollback can erase.
template<typename GeometryMap, typename RegistryMap, typename ExpiryQueue>
typename GeometryMap::iterator PublishPersistentGeometry(
    GeometryMap& geometries, RegistryMap& registry, ExpiryQueue& expiry,
    uint64_t& bytesUsed, uint64_t bytesNeeded,
    const typename GeometryMap::key_type& geometryId,
    const typename RegistryMap::key_type& key,
    typename GeometryMap::mapped_type&& entry,
    const typename RegistryMap::mapped_type& publishedSlot,
    const typename ExpiryQueue::value_type& token) {
  using Slot = typename RegistryMap::mapped_type;
  using Token = typename ExpiryQueue::value_type;
  using Compare = typename ExpiryQueue::value_compare;
  static_assert(std::is_nothrow_copy_assignable_v<Slot>);
  static_assert(std::is_nothrow_copy_constructible_v<Token>
             && std::is_nothrow_move_constructible_v<Token>
             && std::is_nothrow_move_assignable_v<Token>
             && std::is_nothrow_destructible_v<Token>);
  static_assert(std::is_nothrow_copy_constructible_v<Compare>
             && std::is_nothrow_move_constructible_v<Compare>
             && std::is_nothrow_invocable_r_v<bool, Compare&,
                  const Token&, const Token&>);
  static_assert(std::is_nothrow_destructible_v<Slot>
             && std::is_nothrow_destructible_v<typename GeometryMap::mapped_type>);

  // Exception-atomic under the existing device owner, not a concurrency lock.
  // The production maps use pure integer hashes/equality and no-throw cleanup.
  // Do not add noexcept to their hash declarations: that can alter STL node
  // layout/hash caching. Rollback may retain rehashed bucket capacity.
  auto geometry = geometries.end();
  auto slot = registry.end();
  bool insertedSlot = false;
  const auto rollback = [&]() noexcept {
    if (insertedSlot)
      registry.erase(slot);
    if (geometry != geometries.end())
      geometries.erase(geometry);
  };
  try {
    auto inserted = geometries.emplace(geometryId, std::move(entry));
    if (!inserted.second)
      return geometries.end();
    geometry = inserted.first;

    // Leave an existing slot untouched until every allocation has succeeded.
    auto registryInsert = registry.try_emplace(key);
    slot = registryInsert.first;
    insertedSlot = registryInsert.second;

    // With a no-throw token and comparator, only vector growth can throw.
    // A failed growth preserves the heap; no heap rollback is needed.
    expiry.push(token);
    slot->second = publishedSlot;
    bytesUsed += bytesNeeded; // The device already proved cap - bytesNeeded.
    return geometry;
  } catch (const std::bad_alloc&) {
    rollback();
    return geometries.end();
  } catch (...) {
    rollback();
    throw;
  }
}

} // namespace dxvk::war3::shadow
