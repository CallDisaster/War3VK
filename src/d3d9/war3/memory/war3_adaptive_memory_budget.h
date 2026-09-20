#pragma once
#include <algorithm>
#include <cstdint>

namespace dxvk::war3::memory {
constexpr uint64_t MemoryMiB = 1024ull * 1024ull;
// Owner-only negative cache: throttles repeated driver queries under pressure,
// never grants allocation. Existing page tails/holes remain independently usable.
struct BudgetGrowthRetryGate {
  bool valid = false;
  uint64_t frame = 0, minimumBytes = 0;
  bool refused(uint64_t f, uint64_t bytes) const noexcept {
    return valid && frame == f && bytes >= minimumBytes;
  }
  void refuse(uint64_t f, uint64_t bytes) noexcept {
    minimumBytes = valid && frame == f ? std::min(minimumBytes, bytes) : bytes;
    valid = true; frame = f;
  }
  void reset() noexcept { valid = false; frame = minimumBytes = 0; }
};
struct AdaptiveBudgetInput {
  bool supported = false, heapValid = false, vaValid = false;
  uint64_t heapSize = 0, budget = 0, committed = 0, availableVA = 0;
  uint64_t resident = 0, hardCap = 0, fallbackCap = 0, pageBytes = 0;
  uint32_t quotaEighths = 1; // snapshot=1, Arena=3, remainder for other users
};
struct AdaptiveBudgetDecision {
  bool trusted = false, vaPressure = true;
  uint64_t target = 0, headroom = 0, reserve = 0;
  bool canGrow(uint64_t resident, uint64_t bytes) const noexcept {
    return bytes && !vaPressure && bytes <= headroom &&
        bytes <= target && resident <= target - bytes;
  }
};
inline AdaptiveBudgetDecision DecideAdaptiveMemoryBudget(
    const AdaptiveBudgetInput& in) noexcept {
  AdaptiveBudgetDecision out;
  if (!in.pageBytes || !in.quotaEighths || in.quotaEighths > 8) return out;
  out.vaPressure = !in.vaValid || in.availableVA < 256 * MemoryMiB;
  out.trusted = in.supported && in.heapValid && in.heapSize && in.budget &&
      in.budget <= in.heapSize;
  out.target = std::min(in.fallbackCap, in.hardCap);
  if (out.trusted) {
    out.reserve = std::max(512 * MemoryMiB, in.budget / 8);
    const uint64_t free = in.budget > in.committed ? in.budget - in.committed : 0;
    out.headroom = free > out.reserve ? free - out.reserve : 0;
    out.target = std::min(in.hardCap, (in.budget / 8) * in.quotaEighths);
    // A target below live residency is a request to retire idle storage, NOT
    // authority to revoke it. Under hard pressure growth is zero immediately.
    if (!out.headroom) out.target = 0;
  } else {
    out.headroom = out.target > in.resident ? out.target - in.resident : 0;
  }
  out.target -= out.target % in.pageBytes;
  if (out.vaPressure) { out.target = 0; out.headroom = 0; }
  return out;
}

// Demand hysteresis for a retired Arena generation. Work and GPU retirement
// are supplied by the existing owner; this helper does not establish either.
struct ArenaTrimHistory {
  uint64_t windowStart = 0, peak = 0;
  uint64_t target(uint64_t frame, uint64_t used, uint64_t capacity,
                  uint64_t pageBytes, bool pressure) noexcept {
    peak = std::max(peak, used);
    if (!windowStart || frame < windowStart) windowStart = frame;
    if (!pressure && frame - windowStart < 120) return capacity;
    const uint64_t demand = pressure ? used : peak;
    const uint64_t pages = demand / pageBytes + (demand % pageBytes != 0);
    const uint64_t keep = std::min(capacity, (pages + 1) * pageBytes);
    if (frame - windowStart >= 120) { windowStart = frame; peak = 0; }
    // At most one page per decision avoids dropping a warm pool in one cut.
    return keep < capacity ? std::max(keep, capacity - pageBytes) : capacity;
  }
};
}
