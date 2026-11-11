#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <cstdint>
#include <utility>

namespace dxvk::war3::memory {

// Scoped (non-TLS) owner. Do NOT declare this type thread_local: under GCC's
// emulated TLS its destructor would run after the TLS block was freed. Every
// thread_local site uses ThreadLocalLazyArray below.
//
// Intended for a per-thread INSTANCE, not a shared cross-thread container.
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

namespace detail {

// Per-thread owner list for ThreadLocalLazyArray backings. It is a single
// process-heap block reachable only through one fiber-local-storage slot, so
// its release does not depend on the C++ thread_local teardown order, and it
// is not a C++ allocation (operator new accounting sees only the backings).
struct ThreadLocalArrayRegistry {
  // Product threads own at most four thread_local tables; leave headroom.
  static constexpr uint32_t kCapacity = 16u;
  struct Entry {
    void* backing;
    void (*destroy)(void*) noexcept;
  };
  uint32_t count;
  Entry entries[kCapacity];
};

// Under GCC/mingw emulated TLS, a thread's emutls blocks are freed by a
// winpthreads key destructor (at pthread exit, or at DLL_THREAD_DETACH for
// threads it did not create) BEFORE __cxa_thread_atexit destructors run. A
// thread_local owner with a non-trivial destructor would therefore read freed
// memory; this was observed with the release GCC (the owner's pointer field
// had already changed). The owner is trivially destructible instead, and this
// FLS callback, which runs from the OS thread-exit path, returns the backing.
// It must not touch any thread_local object: emutls may already be gone here.
inline void WINAPI ReleaseThreadLocalArrays(void* value) noexcept {
  auto* registry = static_cast<ThreadLocalArrayRegistry*>(value);
  if (!registry)
    return;
  for (uint32_t i = registry->count; i-- > 0u; )
    registry->entries[i].destroy(registry->entries[i].backing);
  ::HeapFree(::GetProcessHeap(), 0, registry);
}

inline DWORD ThreadLocalArraySlot() noexcept {
  // Allocated on the first backing request only. d3d9.dll is never unloaded
  // after rendering started (the stock retry unload happens only when
  // Direct3DCreate9 failed, before any table exists), so the callback address
  // remains valid for every later thread exit.
  static const DWORD slot = ::FlsAlloc(&ReleaseThreadLocalArrays);
  return slot;
}

// Ensures this thread's registry exists and has room for one more entry.
// Returns null when FLS, heap memory or registry capacity is unavailable.
inline ThreadLocalArrayRegistry* PrepareThreadLocalArrayRegistry() noexcept {
  const DWORD slot = ThreadLocalArraySlot();
  if (slot == FLS_OUT_OF_INDEXES)
    return nullptr;
  auto* registry = static_cast<ThreadLocalArrayRegistry*>(::FlsGetValue(slot));
  if (!registry) {
    registry = static_cast<ThreadLocalArrayRegistry*>(::HeapAlloc(
        ::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ThreadLocalArrayRegistry)));
    if (!registry)
      return nullptr;
    if (!::FlsSetValue(slot, registry)) {
      ::HeapFree(::GetProcessHeap(), 0, registry);
      return nullptr;
    }
  }
  return registry->count < ThreadLocalArrayRegistry::kCapacity ? registry : nullptr;
}

} // namespace detail

// The thread_local form of LazyThreadArray. Same access contract (get/try_get/
// peek/[]/iteration/fill), but the object itself is trivially destructible:
// only a raw pointer lives in TLS, and the backing is released by the thread's
// FLS callback at thread exit. Use LazyThreadArray for scoped (non-TLS)
// instances; use this type for every `thread_local` declaration. Do not touch
// one from another thread_local object's destructor: its backing may already
// have been returned by then.
template<typename T, std::size_t N>
class ThreadLocalLazyArray final {
public:
  using array_type = std::array<T, N>;
  using value_type = T;
  using size_type = std::size_t;
  using iterator = typename array_type::iterator;
  using const_iterator = typename array_type::const_iterator;

  constexpr ThreadLocalLazyArray() noexcept = default;
  ThreadLocalLazyArray(const ThreadLocalLazyArray&) = delete;
  ThreadLocalLazyArray& operator=(const ThreadLocalLazyArray&) = delete;
  ThreadLocalLazyArray(ThreadLocalLazyArray&&) = delete;
  ThreadLocalLazyArray& operator=(ThreadLocalLazyArray&&) = delete;

  array_type& get() { return ensure(); }
  const array_type& get() const { return ensure(); }

  array_type* try_get() noexcept { return tryEnsure(); }
  const array_type* try_get() const noexcept { return tryEnsure(); }

  array_type* peek() noexcept { return array(); }
  const array_type* peek() const noexcept { return array(); }

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
  static void destroy(void* backing) noexcept {
    delete static_cast<array_type*>(backing);
  }

  array_type* array() const noexcept { return storage_; }

  array_type* tryEnsure() const noexcept {
    if (storage_)
      return array();
    try {
      return &ensure();
    } catch (...) {
      // bad_alloc, registry refusal or a throwing T constructor.
      return nullptr;
    }
  }

  array_type& ensure() const {
    if (!storage_) {
      // Check registry room first, then value-initialize every element before
      // publishing. Any failure leaves storage_ null and retryable.
      auto* registry = detail::PrepareThreadLocalArrayRegistry();
      if (!registry)
        throw std::bad_alloc();
      auto fresh = std::make_unique<array_type>();
      registry->entries[registry->count++] = {fresh.get(), &destroy};
      storage_ = fresh.release();
    }
    return *array();
  }

  // Not cleared at thread exit: the exiting thread no longer uses it, and the
  // release callback must not write into possibly freed emulated TLS.
  mutable array_type* storage_ = nullptr;
};

static_assert(std::is_trivially_destructible_v<ThreadLocalLazyArray<int, 1>>,
              "thread_local lazy arrays must not rely on TLS destructor order");

// Prepare backing only, in argument order and with short-circuit failure. No
// caller data is published here. Earlier successful allocations stay owned and
// value-initialized if a later one fails; retry neither reallocates nor clears
// them. The caller must not mutate its records/occupancy before success.
template<typename... Arrays>
bool TryPrepareThreadArrays(Arrays&... arrays) noexcept {
  return ((arrays.try_get() != nullptr) && ...);
}

} // namespace dxvk::war3::memory
