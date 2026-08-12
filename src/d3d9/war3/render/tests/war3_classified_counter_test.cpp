#include "../war3_classified_counter.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_classified_counter_test: " << message << '\n';
  return condition;
}

bool testDerivedSuccessCount() {
  using dxvk::war3::render::DeriveClassifiedSuccessCount;
  if (!require(DeriveClassifiedSuccessCount(
                   10u, std::array<uint64_t, 4>{1u, 2u, 1u, 0u}) == 6u,
               "classified successes differ from attempts minus failures"))
    return false;
  if (!require(DeriveClassifiedSuccessCount(
                   3u, std::array<uint64_t, 2>{2u, 2u}) == 0u,
               "inconsistent snapshots must fail soft instead of underflow"))
    return false;
  return require(
      DeriveClassifiedSuccessCount(
          (std::numeric_limits<uint64_t>::max)(),
          std::array<uint64_t, 2>{
              (std::numeric_limits<uint64_t>::max)(), 1u}) == 0u,
      "overflowing failure totals must fail soft");
}

} // namespace

int main() {
  return testDerivedSuccessCount() ? 0 : 1;
}
