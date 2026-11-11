#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace dxvk::war3::memory {

// Intended for a thread_local INSTANCE, not a shared cross-thread container.
// This changes backing allocation timing only; N and std::array semantics stay
// fixed. Cold TLS contains just the owning pointer, never sizeof(T) * N bytes.
//
// get(), [], begin/end and fill (including const reads) may allocate on first
// access and may throw. Maintenance/reset/invalidation paths must use peek()
// when they should leave an untouched thread cold. size()/capacity()/peek()
// never allocate. An existing array is neither shrunk nor replaced by fill.
// try_get() is the non-throwing storage-preflight boundary: allocation OR
// element-construction failure returns null, never a partially built array.
// It catches only work done by ensure(), not the caller's business operation.
template<typename T, std::size_t N>
class LazyThreadArray final {
public:
  using array_type = std::array<T, N>;
  using value_type = T;
  using size_type = std::size_t;
  using iterator = typename array_type::iterator;
  using const_iterator = typename array_type::const_iterator;

  LazyThreadArray() noexcept = default;
  ~LazyThreadArray() = default;
  LazyThreadArray(const LazyThreadArray&) = delete;
  LazyThreadArray& operator=(const LazyThreadArray&) = delete;
  LazyThreadArray(LazyThreadArray&&) = delete;
  LazyThreadArray& operator=(LazyThreadArray&&) = delete;

  array_type& get() { return ensure(); }
  const array_type& get() const { return ensure(); }

  array_type* try_get() noexcept { return tryEnsure(); }
  const array_type* try_get() const noexcept { return tryEnsure(); }

  array_type* peek() noexcept { return storage_.get(); }
  const array_type* peek() const noexcept { return storage_.get(); }

  T& operator[](size_type index) { return ensure()[index]; }
  const T& operator[](size_type index) const { return ensure()[index]; }

  iterator begin() { return ensure().begin(); }
  iterator end() { return ensure().end(); }
  const_iterator begin() const { return ensure().begin(); }
  const_iterator end() const { return ensure().end(); }
  const_iterator cbegin() const { return ensure().cbegin(); }
  const_iterator cend() const { return ensure().cend(); }

  void fill(const T& value) { ensure().fill(value); }
  static constexpr size_type size() noexcept { return N; }
  static constexpr size_type capacity() noexcept { return N; }

private:
  array_type* tryEnsure() const noexcept {
    if (storage_)
      return storage_.get();
    try {
      return &ensure();
    } catch (const std::bad_alloc&) {
      return nullptr;
    } catch (...) {
      // Optional callers also receive a refusal for a throwing T constructor.
      // get() deliberately retains its original propagating exception contract.
      return nullptr;
    }
  }

  array_type& ensure() const {
    if (!storage_) {
      // Value-initialize all elements before publishing the owner. Allocation
      // or element-construction exceptions leave storage_ null; new-expression
      // cleanup destroys any partially constructed elements automatically.
      auto fresh = std::make_unique<array_type>();
      storage_ = std::move(fresh);
    }
    return *storage_;
  }

  mutable std::unique_ptr<array_type> storage_;
};

// Prepare backing only, in argument order and with short-circuit failure. No
// caller data is published here. Earlier successful allocations stay owned and
// value-initialized if a later one fails; retry neither reallocates nor clears
// them. The caller must not mutate its records/occupancy before success.
template<typename... Arrays>
bool TryPrepareThreadArrays(Arrays&... arrays) noexcept {
  return ((arrays.try_get() != nullptr) && ...);
}

} // namespace dxvk::war3::memory
