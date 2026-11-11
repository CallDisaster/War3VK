#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace dxvk::war3::japi {

// Detach the complete old author registry before invoking any backend. The
// registry is a CPU handle directory, not an owner of GPU resources. The
// calling lifecycle thread remains the cleanup owner; this does not schedule
// work, wait for GPU completion or change the renderer's Present retirement.
//
// Restricted to the production unordered_map/default allocator (and its test
// equivalent): an empty container does not allocate on our supported STL,
// swapping equal allocators transfers nodes without allocation, and node
// destruction is no-throw. The traits below constrain exception safety; the
// fail-allocation production test also checks the no-allocation requirement.
// Mutex acquisition retains its existing error semantics; lock failure is not
// swallowed as though an old map had been successfully retired.
template<typename Registry, typename Mutex, typename Retire>
size_t DetachAndRetireManagedObjects(
    Registry& live, Mutex& mutex, Retire&& retire) {
  static_assert(std::is_nothrow_default_constructible_v<Registry>);
  static_assert(std::is_nothrow_destructible_v<Registry>);
  static_assert(std::is_nothrow_destructible_v<typename Registry::value_type>);
  static_assert(std::allocator_traits<
      typename Registry::allocator_type>::is_always_equal::value);
  static_assert(noexcept(std::declval<Registry&>().swap(
      std::declval<Registry&>())));
  static_assert(std::is_nothrow_invocable_v<
      Retire&, const typename Registry::mapped_type&>);

  Registry detached;
  {
    std::lock_guard<Mutex> lock(mutex);
    live.swap(detached);
  }

  const size_t count = detached.size();
  for (const auto& entry : detached)
    retire(entry.second);
  // Destruction and callbacks both happen after unlocking. A callback that
  // re-enters the directory observes no old ids; newly inserted nodes belong
  // to live and cannot be erased by destruction of this detached generation.
  return count;
}

} // namespace dxvk::war3::japi
