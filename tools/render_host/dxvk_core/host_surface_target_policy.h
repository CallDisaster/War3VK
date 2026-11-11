#pragma once
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <limits>

namespace warvk::host::surface {
enum class Mode : uint32_t { Peer, OwnedTopLevel, OwnedChild };
inline bool ParseMode(const wchar_t* value, Mode& result) noexcept {
  if (!value || !std::wcscmp(value, L"peer")) { result = Mode::Peer; return true; }
  if (!std::wcscmp(value, L"host-top-level")) { result = Mode::OwnedTopLevel; return true; }
  if (!std::wcscmp(value, L"host-child")) { result = Mode::OwnedChild; return true; }
  return false; // Unknown (including explicit empty) never becomes a fallback.
}
inline const char* ModeName(Mode value) noexcept {
  switch (value) {
    case Mode::Peer: return "peer";
    case Mode::OwnedTopLevel: return "host-top-level";
    case Mode::OwnedChild: return "host-child";
  }
  return "invalid";
}
inline uint64_t CappedDeadline(uint64_t now, uint32_t allowance, uint64_t outer) noexcept {
  const auto limit = std::numeric_limits<uint64_t>::max();
  const auto local = now > limit - allowance ? limit : now + allowance;
  return local < outer ? local : outer;
}
inline uint32_t Remaining(uint64_t now, uint64_t until) noexcept {
  if (now >= until) return 0;
  const auto left = until - now;
  return left < uint64_t(UINT32_MAX) ? uint32_t(left) : UINT32_MAX;
}

// Exact shared owner policy used by the HWND implementation, not a test model.
// One window per object; there is deliberately no reset/revive transition.
class Lifetime final {
  static constexpr uint32_t Ready = 1, Surface = 2, Retired = 4;
  static constexpr uint32_t Abort = 8, Close = 16, Gone = 32, Invalid = 64;
  std::atomic<uint32_t> bits{0};
  template<typename Allowed>
  bool setWhen(uint32_t add, Allowed allowed) noexcept {
    auto before = bits.load(std::memory_order_acquire);
    for (;;) {
      if (!allowed(before)) return false;
      if (bits.compare_exchange_weak(before, before | add,
          std::memory_order_acq_rel, std::memory_order_acquire)) return true;
    }
  }
  static bool destroyAllowed(uint32_t value) noexcept {
    return (value & Close) && (!(value & Surface) || (value & Retired));
  }
public:
  bool publishReady() noexcept {
    return setWhen(Ready, [](uint32_t v) { return !(v & (Ready | Surface | Abort | Close | Gone | Invalid)); });
  }
  bool claimSurface() noexcept {
    return setWhen(Surface, [](uint32_t v) { return (v & Ready) && !(v & (Surface | Abort | Close | Gone | Invalid)); });
  }
  // Called only at the backend's explicit post-Presenter-destruction boundary.
  bool markSurfaceRetired() noexcept {
    return setWhen(Retired, [](uint32_t v) { return (v & Surface) != 0; });
  }
  bool requestCloseAfterRetirement() noexcept {
    return setWhen(Close, [](uint32_t v) { return (v & Surface) && (v & Retired); });
  }
  // Races claimSurface via one CAS word: a startup timeout either aborts BEFORE
  // possible WSI use, or cannot authorize destruction. It never guesses.
  bool abortBeforeSurface() noexcept {
    return setWhen(Abort | Close, [](uint32_t v) { return !(v & Surface); });
  }
  void invalidate() noexcept { bits.fetch_or(Invalid, std::memory_order_acq_rel); }
  bool markWindowGone() noexcept {
    auto before = bits.load(std::memory_order_acquire);
    for (;;) {
      const bool expected = destroyAllowed(before);
      if (bits.compare_exchange_weak(before, before | Gone | (expected ? 0u : Invalid),
          std::memory_order_acq_rel, std::memory_order_acquire)) return expected;
    }
  }
  bool usable() const noexcept {
    const auto v = bits.load(std::memory_order_acquire);
    return (v & Ready) && !(v & (Abort | Close | Gone | Invalid | Retired));
  }
  bool mayDestroy() const noexcept { return destroyAllowed(bits.load(std::memory_order_acquire)); }
  // A fault/WM_QUIT is not a surface retirement. Returning from the HWND owner
  // thread would let Windows destroy the still-live target behind the backend.
  bool mustKeepOwnerAlive() const noexcept {
    const auto v = bits.load(std::memory_order_acquire);
    return (v & Surface) && !(v & (Retired | Gone));
  }
  bool hasSurface() const noexcept { return (bits.load(std::memory_order_acquire) & Surface) != 0; }
  bool invalid() const noexcept { return (bits.load(std::memory_order_acquire) & Invalid) != 0; }
  uint32_t snapshot() const noexcept { return bits.load(std::memory_order_acquire); }
};
} // namespace warvk::host::surface
