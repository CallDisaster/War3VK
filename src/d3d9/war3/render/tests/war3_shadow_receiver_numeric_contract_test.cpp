#include "../war3_shadow_receiver_numeric_contract.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>

namespace {

using dxvk::war3::render::shadowmath::Vec2;
namespace math = dxvk::war3::render::shadowmath;

bool near(double actual, double expected, double epsilon = 1.0e-10) {
  return std::abs(actual - expected) <= epsilon;
}

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_shadow_receiver_numeric_contract_test: " << message
              << '\n';
  return condition;
}

bool testCompareFirstSubTexelContinuity() {
  // A vertical 0.2/1.0 depth edge compared at Dref=0.5 has visibility equal
  // to the horizontal sub-texel phase. Filtering raw depth first would turn
  // much of this interval into one hard lit result instead.
  constexpr std::array<double, 4> depth = {0.2, 1.0, 0.2, 1.0};
  constexpr int kSteps = 2048;
  double previous = -1.0;
  for (int i = 0; i <= kSteps; i++) {
    const double phase = static_cast<double>(i) / kSteps;
    const double visibility =
      math::manualCompareLinear2x2(depth, 0.5, phase, 0.5);
    if (!require(near(visibility, phase, 1.0e-12),
                 "compare-first fallback lost sub-texel linearity"))
      return false;
    if (i > 0 && !require(visibility - previous <= 1.0 / kSteps + 1.0e-12,
                          "compare-first fallback made a whole-texel jump"))
      return false;
    previous = visibility;
  }
  return true;
}

bool testReceiverPlaneSignedSlopeAndAtomicFallback() {
  const Vec2 uvDx = {0.40, 0.10};
  const Vec2 uvDy = {-0.20, 0.50};
  const Vec2 offset = {0.003, -0.002};
  for (const Vec2 expected : {Vec2{0.12, -0.07}, Vec2{-0.12, 0.07}}) {
    const double depthDx = expected.x * uvDx.x + expected.y * uvDx.y;
    const double depthDy = expected.x * uvDy.x + expected.y * uvDy.y;
    Vec2 recovered = {};
    if (!require(math::receiverPlaneDepthGradient(
                     uvDx, uvDy, depthDx, depthDy, recovered),
                 "valid receiver plane rejected"))
      return false;
    if (!require(near(recovered.x, expected.x) && near(recovered.y, expected.y),
                 "receiver-plane signed gradient has the wrong UV sign"))
      return false;
    const double expectedReference =
      0.5 + expected.x * offset.x + expected.y * offset.y;
    if (!require(near(math::receiverPlaneTapReference(
                         0.5, offset, recovered, true),
                      expectedReference),
                 "per-tap receiver-plane reference is not affine"))
      return false;
  }

  Vec2 gradient = {};
  if (!require(!math::receiverPlaneDepthGradient(
                   {0.5, 0.25}, {1.0, 0.5}, 0.1, 0.2, gradient),
               "degenerate Jacobian was accepted"))
    return false;
  if (!require(!math::receiverPlaneDepthGradient(
                   {std::numeric_limits<double>::quiet_NaN(), 0.0},
                   {0.0, 1.0}, 0.0, 0.0, gradient),
               "non-finite Jacobian was accepted"))
    return false;
  if (!require(!math::receiverPlaneKernelValid({0.25, -0.25}, 0.01),
               "unsafe whole kernel was accepted"))
    return false;
  if (!require(near(math::receiverPlaneTapReference(
                         0.42, {0.25, -0.25}, {0.25, -0.25}, false),
                      0.42),
               "fallback changed only some taps instead of the whole kernel"))
    return false;
  return true;
}

bool testCascadeBoundaryContinuityAndInvalidNextFallback() {
  constexpr double primary = 0.20;
  constexpr double next = 0.80;
  constexpr double farSplit = 100.0;
  constexpr double blendRange = 20.0;
  if (!require(near(math::blendCascadeVisibility(
                         primary, next, 80.0, farSplit, blendRange, true),
                    primary),
               "cascade blend start is discontinuous"))
    return false;
  if (!require(near(math::blendCascadeVisibility(
                         primary, next, 90.0, farSplit, blendRange, true),
                    0.50),
               "cascade blend midpoint is not smoothstep symmetric"))
    return false;
  if (!require(near(math::blendCascadeVisibility(
                         primary, next, 100.0, farSplit, blendRange, true),
                    next),
               "cascade blend end is discontinuous"))
    return false;
  constexpr double kEpsilon = 1.0e-5;
  const double left = math::blendCascadeVisibility(
      primary, next, 90.0 - kEpsilon, farSplit, blendRange, true);
  const double right = math::blendCascadeVisibility(
      primary, next, 90.0 + kEpsilon, farSplit, blendRange, true);
  if (!require(std::abs(left - right) < 1.0e-6,
               "cascade blend has a sub-texel boundary jump"))
    return false;
  return require(near(math::blendCascadeVisibility(
                          primary, next, 95.0, farSplit, blendRange, false),
                      primary),
                 "invalid next cascade blended a synthetic lit sample");
}

}  // namespace

int main() {
  if (!testCompareFirstSubTexelContinuity() ||
      !testReceiverPlaneSignedSlopeAndAtomicFallback() ||
      !testCascadeBoundaryContinuityAndInvalidNextFallback())
    return 1;
  std::cout << "war3_shadow_receiver_numeric_contract_test: PASS\n";
  return 0;
}
