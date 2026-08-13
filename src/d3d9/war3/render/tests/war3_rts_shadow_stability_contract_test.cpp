#include "../war3_rts_shadow_stability_contract.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

namespace rts = dxvk::war3::render;

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_rts_shadow_stability_contract_test: " << message
              << '\n';
  return condition;
}

rts::War3RtsShadowReceiverBandQuery makeQuery(double xOffset = 0.0) {
  rts::War3RtsShadowReceiverBandQuery query = {};
  query.frustumCorners = {{
      {xOffset - 100.0, -80.0, -100.0},
      {xOffset + 100.0, -80.0, -100.0},
      {xOffset + 100.0, 80.0, -100.0},
      {xOffset - 100.0, 80.0, -100.0},
      {xOffset - 400.0, -300.0, 100.0},
      {xOffset + 400.0, -300.0, 100.0},
      {xOffset + 400.0, 300.0, 100.0},
      {xOffset - 400.0, 300.0, 100.0},
  }};
  query.receiverBandHalfHeight = 16.0;
  query.receiverPadding = 32.0;
  query.worldTexelSize = 0.25;
  query.shadowResolution = 4096u;
  return query;
}

bool testFixedDensityAndCoverage() {
  const auto fit = rts::War3FitRtsShadowReceiverBand(makeQuery());
  return require(fit.valid, "finite receiver band was rejected") &&
      require(fit.densitySatisfied, "fixed density failed coverage") &&
      require(fit.footprintPointCount != 0u,
              "receiver band produced no footprint") &&
      require(std::abs(fit.worldTexelSize - 0.25) < 1.0e-12,
              "world texel size changed") &&
      require(std::abs(fit.halfExtent - 512.0) < 1.0e-12,
              "fixed clip radius changed");
}

bool testSubTexelPanAndSnap() {
  const auto base = rts::War3FitRtsShadowReceiverBand(makeQuery(0.0));
  const auto subTexel = rts::War3FitRtsShadowReceiverBand(makeQuery(0.10));
  const auto nextTexel = rts::War3FitRtsShadowReceiverBand(makeQuery(0.26));
  return require(base.valid && subTexel.valid && nextTexel.valid,
                 "pan queries were rejected") &&
      require(base.centerLightX == subTexel.centerLightX,
              "sub-texel pan moved the clip origin") &&
      require(std::abs(nextTexel.centerLightX - base.centerLightX - 0.25) <
                  1.0e-12,
              "clip origin did not move by one world texel");
}

bool testZoomAndDensityFailureFallback() {
  auto query = makeQuery();
  for (auto& corner : query.frustumCorners) {
    corner.x *= 4.0;
    corner.y *= 4.0;
  }
  const auto rejected = rts::War3FitRtsShadowReceiverBand(query);
  query.worldTexelSize = 1.0;
  const auto accepted = rts::War3FitRtsShadowReceiverBand(query);
  return require(!rejected.valid && !rejected.densitySatisfied,
                 "undersized fixed clip silently clipped receivers") &&
      require(accepted.valid && accepted.densitySatisfied,
              "coarser explicit clip level did not cover receivers");
}

bool testPlaneOrientationAndInvalidInput() {
  auto yUp = makeQuery();
  for (auto& corner : yUp.frustumCorners)
    std::swap(corner.y, corner.z);
  yUp.planeNormal = {0.0, 1.0, 0.0};
  yUp.lightUp = {0.0, 0.0, 1.0};
  const auto positiveDirection = rts::War3FitRtsShadowReceiverBand(yUp);
  yUp.lightRight = {-1.0, 0.0, 0.0};
  const auto negativeDirection = rts::War3FitRtsShadowReceiverBand(yUp);
  yUp.worldTexelSize = std::numeric_limits<double>::quiet_NaN();
  const auto invalid = rts::War3FitRtsShadowReceiverBand(yUp);
  return require(positiveDirection.valid && negativeDirection.valid,
                 "Y-up or signed light basis rejected") &&
      require(std::abs(positiveDirection.halfExtent -
                       negativeDirection.halfExtent) < 1.0e-12,
              "light direction sign changed clip density") &&
      require(!invalid.valid, "non-finite density was accepted");
}

}  // namespace

int main() {
  using Mode = rts::War3RtsShadowCandidateMode;
  const bool policy =
      rts::ParseRtsShadowCandidateMode(0u) == Mode::Off &&
      rts::ParseRtsShadowCandidateMode(1u) == Mode::ReceiverBand &&
      rts::ParseRtsShadowCandidateMode(2u) == Mode::Off;
  return require(policy, "candidate mode parser accepted a release-unsafe mode") &&
      testFixedDensityAndCoverage() && testSubTexelPanAndSnap() &&
      testZoomAndDensityFailureFallback() &&
      testPlaneOrientationAndInvalidInput()
      ? 0
      : 1;
}
