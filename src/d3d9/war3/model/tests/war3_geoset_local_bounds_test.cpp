#include "../war3_geoset_local_bounds.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

int g_failures = 0;

#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,        \
                   __LINE__, #expr);                                         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

void TestFiniteBounds() {
  const std::vector<float> positions = {
      -2.0f, -4.0f, -6.0f,
       2.0f,  4.0f,  6.0f,
       0.0f,  1.0f, -1.0f,
  };
  const auto bounds =
      dxvk::war3::model::ComputeShadowGeosetLocalBounds(positions, 3u);
  CHECK(bounds.valid);
  CHECK(bounds.minX == -2.0f && bounds.maxX == 2.0f);
  CHECK(bounds.minY == -4.0f && bounds.maxY == 4.0f);
  CHECK(bounds.minZ == -6.0f && bounds.maxZ == 6.0f);
  CHECK(bounds.centerX == 0.0f && bounds.centerY == 0.0f &&
        bounds.centerZ == 0.0f);
  CHECK(std::abs(bounds.radius - std::sqrt(56.0f)) < 0.0001f);
}

void TestPayloadShapeMustBeExact() {
  const std::vector<float> shortPayload = {0.0f, 1.0f, 2.0f};
  CHECK(!dxvk::war3::model::ComputeShadowGeosetLocalBounds(
             shortPayload, 2u).valid);
  const std::vector<float> trailingPayload = {
      0.0f, 1.0f, 2.0f, 3.0f};
  CHECK(!dxvk::war3::model::ComputeShadowGeosetLocalBounds(
             trailingPayload, 1u).valid);
}

void TestNonFinitePayloadFailsClosed() {
  const std::vector<float> nanPayload = {
      0.0f, 0.0f, 0.0f,
      std::numeric_limits<float>::quiet_NaN(), 1.0f, 2.0f};
  CHECK(!dxvk::war3::model::ComputeShadowGeosetLocalBounds(
             nanPayload, 2u).valid);
  const std::vector<float> infPayload = {
      0.0f, 0.0f, 0.0f,
      std::numeric_limits<float>::infinity(), 1.0f, 2.0f};
  CHECK(!dxvk::war3::model::ComputeShadowGeosetLocalBounds(
             infPayload, 2u).valid);
}

} // namespace

int main() {
  TestFiniteBounds();
  TestPayloadShapeMustBeExact();
  TestNonFinitePayloadFailsClosed();
  return g_failures == 0 ? 0 : 1;
}
