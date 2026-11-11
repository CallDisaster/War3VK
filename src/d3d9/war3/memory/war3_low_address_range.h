#pragma once

#include <cstdint>

namespace dxvk::war3::memory::lowaddress {
constexpr uint64_t kLimit = 0x80000000ull;
constexpr uint64_t kGranularity = 0x10000ull;
constexpr uint64_t kPage = 0x1000ull;
constexpr uint32_t kMaxQueries = 32768;

inline bool Fits(uint64_t base, uint64_t bytes) noexcept {
  return base >= kGranularity && bytes && base < kLimit && bytes <= kLimit-base;
}

struct Region { uint64_t base = 0, bytes = 0; bool free = false; };
struct Result {
  uintptr_t address = 0;
  uint32_t queries = 0, attempts = 0, rejectedHigh = 0;
  bool invalid = false, releaseFailed = false;
};

// The actual production selection algorithm. Ops owns the OS calls; tests use
// adversarial regions and races without reserving the author's address space.
// NULL+MEM_COMMIT also reserves on Windows. Explicit candidates must therefore
// add MEM_RESERVE, but keep all other flags/protection unchanged.
template<class Ops>
Result Allocate(uint64_t bytes, uint32_t flags, uint32_t protection, Ops& ops) noexcept {
  Result r;
  const uint64_t rounded = (bytes + kPage-1) & ~(kPage-1);
  if (!bytes || bytes > kLimit-kGranularity || !Fits(kGranularity, rounded)) {
    r.invalid = true;
    return r;
  }
  ++r.attempts;
  uintptr_t base = ops.allocate(0, bytes, flags, protection);
  if (base && Fits(base, rounded)) { r.address = base; return r; }
  if (base) {
    ++r.rejectedHigh;
    if (!ops.release(base)) { r.releaseFailed = true; return r; }
  }
  uint64_t cursor = kGranularity;
  while (cursor < kLimit && r.queries < kMaxQueries) {
    Region region;
    ++r.queries;
    if (!ops.query(uintptr_t(cursor), region) || !region.bytes ||
        region.base > cursor || region.bytes > UINT64_MAX-region.base)
      break;
    const uint64_t end = region.base + region.bytes;
    if (end <= cursor) break;
    if (region.free) {
      const uint64_t candidate = (cursor+kGranularity-1) & ~(kGranularity-1);
      if (candidate < end && Fits(candidate, rounded) && rounded <= end-candidate) {
        ++r.attempts;
        base = ops.allocate(uintptr_t(candidate), bytes, flags|0x2000u, protection);
        if (base && base == candidate && Fits(base, rounded)) {
          r.address = base; return r;
        }
        if (base) {
          ++r.rejectedHigh;
          if (!ops.release(base)) { r.releaseFailed = true; return r; }
        }
        // Another thread may have won this candidate. Progress is bounded;
        // never retry the same address indefinitely or fall back to high VA.
        cursor = candidate+kGranularity;
        continue;
      }
    }
    cursor = (end+kGranularity-1) & ~(kGranularity-1);
  }
  return r;
}
} // namespace dxvk::war3::memory::lowaddress
