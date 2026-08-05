#pragma once

#include <cstdint>
#include <limits>

namespace dxvk::war3::model {

// Process-lifetime monotonic issuer owned by ShadowModelResourceCache.  The
// cache calls issue() only while holding its unique writer lock.  The issuer
// deliberately refuses the terminal value so exhaustion can never wrap back
// to a previously published generation.
class ImmutableModelGenerationIssuer final {
public:
  constexpr ImmutableModelGenerationIssuer() noexcept = default;

  // The explicit seed exists only for isolated value-contract tests.  The
  // production cache always uses the process-lifetime default seed of one.
  explicit constexpr ImmutableModelGenerationIssuer(uint64_t next) noexcept
  : m_next(next) { }

  ImmutableModelGenerationIssuer(
      const ImmutableModelGenerationIssuer&) = delete;
  ImmutableModelGenerationIssuer& operator=(
      const ImmutableModelGenerationIssuer&) = delete;

  uint64_t issue() noexcept {
    if (m_next == 0u ||
        m_next == std::numeric_limits<uint64_t>::max()) {
      m_exhausted = true;
      return 0u;
    }

    return m_next++;
  }

  bool exhausted() const noexcept { return m_exhausted; }
  uint64_t nextForDiagnostics() const noexcept { return m_next; }

private:
  uint64_t m_next = 1u;
  bool m_exhausted = false;
};

}  // namespace dxvk::war3::model
