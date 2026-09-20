#pragma once
#include <array>
#include <cstdint>

namespace dxvk::war3::timeline {
constexpr uint32_t BucketCount = 40;
static_assert(BucketCount <= 64);
struct Slice {
  uint64_t start = 0, end = 0;
  std::array<uint64_t, BucketCount> self{}, inclusive{}, legacySelf{};
  std::array<uint32_t, BucketCount> calls{};
  uint64_t legacyTicks = 0, events = 0;
  uint32_t faults = 0;
};
// One owner thread. Every elapsed tick belongs to one exclusive leaf, including
// bucket 0 (outside instrumented scopes). Inclusive values are NOT additive.
class Ledger {
public:
  uint64_t enter(uint32_t id, uint64_t now) noexcept {
    advance(now);
    if (id == 0 || id >= BucketCount || depth == stack.size() || serial == UINT64_MAX) {
      ++row.faults; faulted = true; return 0;
    }
    const uint64_t token = ++serial;
    stack[depth++] = {id, token};
    ++row.calls[id]; ++row.events;
    return token;
  }
  void leave(uint64_t token, uint64_t now) noexcept {
    if (!token) return;
    advance(now);
    if (!depth || stack[depth-1].token != token) {
      ++row.faults; faulted = true; return;
    }
    --depth; ++row.events;
  }
  void legacy(bool value, uint64_t now) noexcept { advance(now); insideLegacy = value; }
  Slice cut(uint64_t now) noexcept {
    advance(now); row.end = now;
    Slice result = row; row = {}; row.start = now;
    row.faults = faulted ? 1 : 0;
    return result;
  }
  void advance(uint64_t now) noexcept {
    if (!last) { last = now; row.start = now; return; }
    if (now < last) { ++row.faults; faulted = true; return; }
    const uint64_t dt = now-last; last = now;
    const uint32_t leaf = (!faulted && depth) ? stack[depth-1].id : 0;
    row.self[leaf] += dt;
    if (insideLegacy) { row.legacySelf[leaf] += dt; row.legacyTicks += dt; }
    if (!faulted) {
      uint64_t seen = 0;
      for (uint32_t i=0; i<depth; ++i) {
        const uint64_t bit = uint64_t(1) << stack[i].id;
        if (!(seen & bit)) row.inclusive[stack[i].id] += dt;
        seen |= bit;
      }
    }
  }
private:
  struct Entry { uint32_t id; uint64_t token; };
  std::array<Entry,64> stack{};
  uint32_t depth = 0;
  uint64_t serial = 0, last = 0;
  bool insideLegacy = false, faulted = false;
  Slice row{};
};
}
