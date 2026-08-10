#include "../war3_shadow_receiver_numeric_contract.h"
#include "../war3_shadow_alpha_cascade_contract.h"
#include "../war3_shadow_hashed_alpha_contract.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>

namespace {

using dxvk::war3::render::shadowmath::Vec2;
namespace math = dxvk::war3::render::shadowmath;
namespace alpha = dxvk::war3::render;
namespace hashed = dxvk::war3::render::shadowalpha;

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
  constexpr int kExtent = 2048;
  constexpr int kSteps = 2048;
  double previous = -1.0;
  for (int i = 0; i < kSteps; i++) {
    const double phase = static_cast<double>(i) / kSteps;
    math::ManualCompareFootprint footprint = {};
    if (!require(math::manualCompareFootprint(
                     {(0.5 + phase) / kExtent, 1.0 / kExtent},
                     kExtent, kExtent, footprint),
                 "2048 manual-compare footprint rejected a finite UV"))
      return false;
    if (!require(footprint.p00x == 0 && footprint.p10x == 1 &&
                     near(footprint.phaseX, phase, 1.0e-12),
                 "uv * extent - 0.5 no longer produces the shader phase"))
      return false;
    const double visibility =
      math::manualCompareLinear2x2(
          depth, 0.5, footprint.phaseX, footprint.phaseY);
    if (!require(near(visibility, phase, 1.0e-12),
                 "compare-first fallback lost sub-texel linearity"))
      return false;
    if (i > 0 && !require(visibility - previous <= 1.0 / kSteps + 1.0e-12,
                          "compare-first fallback made a whole-texel jump"))
      return false;
    previous = visibility;
  }

  math::ManualCompareFootprint left = {};
  math::ManualCompareFootprint right = {};
  if (!require(math::manualCompareFootprint(
                   {0.0, 0.0}, kExtent, kExtent, left) &&
                   math::manualCompareFootprint(
                   {1.0, 1.0}, kExtent, kExtent, right),
               "manual-compare edge footprints were rejected"))
    return false;
  if (!require(left.p00x == 0 && left.p10x == 0 && left.p00y == 0 &&
                   left.p01y == 0 && right.p00x == kExtent - 1 &&
                   right.p10x == kExtent - 1 &&
                   right.p00y == kExtent - 1 && right.p01y == kExtent - 1,
               "manual-compare edge clamp does not match shader texel clamp"))
    return false;
  return true;
}

bool testRowMajorReceiverPlaneYFlip() {
  math::RowMajorMat4 lightViewProj = {};
  lightViewProj.rows[0] = {2.0, 0.0, 0.0, 0.0};
  lightViewProj.rows[1] = {0.0, 3.0, 0.0, 0.0};
  lightViewProj.rows[2] = {0.0, 0.0, 4.0, 0.0};
  lightViewProj.rows[3] = {0.5, -0.25, 0.75, 1.0};
  math::ReceiverPlaneDifferentials differentials = {};
  if (!require(math::receiverPlaneDifferentialsRowMajor(
                   lightViewProj, {1.0, 2.0, 3.0}, {0.25, -0.5, 0.75},
                   {-0.5, 0.25, -0.5}, differentials),
               "finite row-major light differential was rejected"))
    return false;
  // Hand-derived from row-vector multiplication: ndcDx=(0.5,-1.5,3),
  // ndcDy=(-1,0.75,-2). The negative half Y is intentional and must not
  // silently turn into +0.5 * ndcY.
  if (!require(near(differentials.uvDx.x, 0.25) &&
                   near(differentials.uvDx.y, 0.75) &&
                   near(differentials.uvDy.x, -0.5) &&
                   near(differentials.uvDy.y, -0.375) &&
                   near(differentials.depthDx, 3.0) &&
                   near(differentials.depthDy, -2.0),
               "row-major NDC-to-UV differential lost the fixed Y flip"))
    return false;
  Vec2 gradient = {};
  if (!require(math::receiverPlaneDepthGradient(
                   differentials.uvDx, differentials.uvDy,
                   differentials.depthDx, differentials.depthDy, gradient),
               "row-major receiver-plane gradient was rejected"))
    return false;
  return require(near(gradient.x, 4.0 / 3.0) &&
                     near(gradient.y, 32.0 / 9.0),
                 "row-major receiver-plane gradient disagrees with oracle");
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
  const std::array<Vec2, 4> kernelOffsets = {
    Vec2{-0.01, -0.01}, Vec2{0.01, -0.01},
    Vec2{-0.01, 0.01}, Vec2{0.01, 0.01},
  };
  const bool kernelValid = math::receiverPlaneKernelValid(
      {0.25, -0.25}, 0.01);
  if (!require(!kernelValid, "unsafe whole kernel was accepted"))
    return false;
  for (const Vec2 offset : kernelOffsets) {
    if (!require(near(math::receiverPlaneTapReference(
                           0.42, offset, {0.25, -0.25}, kernelValid),
                       0.42),
                 "fallback changed only some taps instead of the whole kernel"))
      return false;
  }
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

bool testCascadeProjectionDecisionChain() {
  const math::Vec4 valid = {0.0, 0.0, 0.5, 1.0};
  const math::Vec4 invalidW = {0.0, 0.0, 0.5, 0.0};
  const math::Vec4 invalidNdc = {0.0, 0.0, 1.5, 1.0};
  const math::Vec4 invalidUv = {1.5, 0.0, 0.5, 1.0};
  const math::Vec4 invalidFinite = {
    std::numeric_limits<double>::quiet_NaN(), 0.0, 0.5, 1.0,
  };
  if (!require(math::cascadeProjectionValid(valid),
               "valid cascade projection was rejected"))
    return false;
  for (const math::Vec4 invalid : {
           invalidW, invalidNdc, invalidUv, invalidFinite,
       }) {
    if (!require(!math::cascadeProjectionValid(invalid),
                 "clip/NDC/UV invalid projection was accepted"))
      return false;
    if (!require(near(math::blendCascadeVisibility(
                           0.31, 0.79, 99.0, 100.0, 20.0,
                           math::cascadeProjectionValid(invalid)),
                       0.31),
                 "invalid next cascade did not retain primary visibility"))
      return false;
  }
  return true;
}

bool testAlphaCascadeParityContract() {
  const std::array<float, 4> rejected = {
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
      -0.25f,
  };
  for (float bias : rejected) {
    if (!require(alpha::SanitizeShadowAlphaFarRefBias(bias) == 0.0f,
                 "non-finite or negative alpha cascade bias was accepted"))
      return false;
  }

  if (!require(near(alpha::SanitizeShadowAlphaFarRefBias(0.35f), 0.35f) &&
                   near(alpha::SanitizeShadowAlphaFarRefBias(2.0f), 1.0f),
               "finite alpha cascade bias sanitize range changed"))
    return false;

  for (uint32_t cascadeCount : {1u, 4u}) {
    for (uint32_t cascadeIndex = 0u; cascadeIndex < 6u; cascadeIndex++) {
      if (!require(alpha::ShadowAlphaRefBiasForCascade(
                       0.0f, cascadeIndex, cascadeCount) == 0.0f,
                   "default alpha cascade bias changed a cutout threshold"))
        return false;
    }
  }

  constexpr std::array<double, 4> expected = {0.0, 0.1, 0.2, 0.3};
  for (uint32_t cascadeIndex = 0u; cascadeIndex < expected.size();
       cascadeIndex++) {
    if (!require(near(alpha::ShadowAlphaRefBiasForCascade(
                         0.3f, cascadeIndex, 4u), expected[cascadeIndex],
                      1.0e-6),
                 "opt-in alpha cascade bias interpolation changed"))
      return false;
  }

  if (!require(alpha::ShadowAlphaRefBiasForCascade(0.3f, 9u, 4u) == 0.3f &&
                   alpha::ShadowAlphaRefBiasForCascade(0.3f, 9u, 1u) == 0.0f,
               "alpha cascade count/index guards changed"))
    return false;
  return true;
}

bool testStableHashedAlphaContract() {
  double threshold = 0.0;
  const hashed::Vec3 surface = {0.25, 1.5, -0.75};
  const hashed::Vec3 dx = {0.125, 0.0, 0.0};
  const hashed::Vec3 dy = {0.0, 0.125, 0.0};
  if (!require(hashed::stableHashedThreshold(
                   surface, dx, dy, 1.0, threshold),
               "finite stable hashed-alpha input was rejected") ||
      !require(threshold >= 1.0e-6 && threshold <= 1.0,
               "stable hashed-alpha threshold left its bounded range"))
    return false;

  const double originalThreshold = threshold;
  if (!require(hashed::stableHashedThreshold(
                   {surface.x + 0.01, surface.y, surface.z},
                   dx, dy, 1.0, threshold) &&
                   near(threshold, originalThreshold, 1.0e-12),
               "sub-cell translation changed the surface-anchored hash"))
    return false;

  for (const hashed::Vec3 invalid : {
           hashed::Vec3{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0},
           hashed::Vec3{std::numeric_limits<double>::infinity(), 0.0, 0.0},
       }) {
    if (!require(!hashed::stableHashedThreshold(
                     invalid, dx, dy, 1.0, threshold),
                 "non-finite hashed-alpha coordinate was accepted"))
      return false;
  }
  if (!require(!hashed::stableHashedThreshold(
                   surface, {}, {}, 1.0, threshold),
               "degenerate hashed-alpha derivatives were accepted"))
    return false;

  if (!require(hashed::coverageBlendFromTextureFootprint(
                   {1.0 / 1024.0, 0.0}, {0.0, 1.0 / 1024.0},
                   1024.0, 1024.0) == 0.0,
               "magnified/one-texel alpha unexpectedly enabled hashing"))
    return false;
  const double minifiedBlend = hashed::coverageBlendFromTextureFootprint(
      {8.0 / 1024.0, 0.0}, {0.0, 8.0 / 1024.0}, 1024.0, 1024.0);
  if (!require(near(minifiedBlend, 0.25, 1.0e-12),
               "six-level quadratic hashed-alpha fade changed"))
    return false;

  if (!require(near(hashed::stableCoverageThreshold(0.7, 0.2, 0.0), 0.7) &&
                   near(hashed::stableCoverageThreshold(0.7, 0.2, 1.0), 0.2),
               "hashed-alpha blend no longer preserves near authored cutoff"))
    return false;

  for (double interpolation : {0.0, 0.125, 0.5, 0.875, 1.0}) {
    for (double mixed = 0.0; mixed <= 1.0; mixed += 1.0 / 128.0) {
      const double value = hashed::uniformizeInterpolatedHashes(
          mixed, interpolation);
      if (!require(std::isfinite(value) && value >= 1.0e-6 && value <= 1.0,
                   "CDF-uniformized threshold was non-finite or out of range"))
        return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  if (!testCompareFirstSubTexelContinuity() ||
      !testRowMajorReceiverPlaneYFlip() ||
      !testReceiverPlaneSignedSlopeAndAtomicFallback() ||
      !testCascadeBoundaryContinuityAndInvalidNextFallback() ||
      !testCascadeProjectionDecisionChain() ||
      !testAlphaCascadeParityContract() ||
      !testStableHashedAlphaContract())
    return 1;
  std::cout << "war3_shadow_receiver_numeric_contract_test: PASS\n";
  return 0;
}
