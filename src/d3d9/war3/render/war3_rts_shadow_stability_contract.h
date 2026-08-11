#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dxvk::war3::render {

enum class War3RtsShadowCandidateMode : uint8_t {
  Off = 0u,
  ReceiverBand = 1u,
};

#if defined(WARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV) && \
    WARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV
inline constexpr bool kDevelopmentRtsShadowCandidateEnabled = true;
#else
inline constexpr bool kDevelopmentRtsShadowCandidateEnabled = false;
#endif

constexpr War3RtsShadowCandidateMode
ParseRtsShadowCandidateMode(uint32_t configuredMode) noexcept {
  return configuredMode == 1u
      ? War3RtsShadowCandidateMode::ReceiverBand
      : War3RtsShadowCandidateMode::Off;
}

struct War3RtsShadowVec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct War3RtsShadowReceiverBandQuery {
  std::array<War3RtsShadowVec3, 8> frustumCorners = {};
  War3RtsShadowVec3 planeNormal = {0.0, 0.0, 1.0};
  War3RtsShadowVec3 lightRight = {1.0, 0.0, 0.0};
  War3RtsShadowVec3 lightUp = {0.0, 1.0, 0.0};
  double receiverPlaneHeight = 0.0;
  double receiverBandHalfHeight = 512.0;
  double receiverPadding = 64.0;
  double worldTexelSize = 1.0;
  uint32_t shadowResolution = 4096u;
  bool stableSnap = true;
};

struct War3RtsShadowReceiverBandFit {
  bool valid = false;
  bool densitySatisfied = false;
  uint32_t footprintPointCount = 0u;
  double centerLightX = 0.0;
  double centerLightY = 0.0;
  double halfExtent = 0.0;
  double requiredHalfExtent = 0.0;
  double worldTexelSize = 0.0;
};

inline bool War3RtsShadowFinite(double value) noexcept {
  return std::isfinite(value);
}

inline double War3RtsShadowDot(War3RtsShadowVec3 a,
                               War3RtsShadowVec3 b) noexcept {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline bool War3RtsShadowNormalize(War3RtsShadowVec3 value,
                                   War3RtsShadowVec3& out) noexcept {
  const double lengthSquared = War3RtsShadowDot(value, value);
  if (!War3RtsShadowFinite(lengthSquared) || lengthSquared <= 1.0e-18)
    return false;
  const double inverseLength = 1.0 / std::sqrt(lengthSquared);
  out = {value.x * inverseLength, value.y * inverseLength,
         value.z * inverseLength};
  return War3RtsShadowFinite(out.x) && War3RtsShadowFinite(out.y) &&
      War3RtsShadowFinite(out.z);
}

inline War3RtsShadowReceiverBandFit War3FitRtsShadowReceiverBand(
    const War3RtsShadowReceiverBandQuery& query) noexcept {
  War3RtsShadowReceiverBandFit result = {};
  if (query.shadowResolution == 0u ||
      !War3RtsShadowFinite(query.receiverPlaneHeight) ||
      !War3RtsShadowFinite(query.receiverBandHalfHeight) ||
      query.receiverBandHalfHeight < 0.0 ||
      !War3RtsShadowFinite(query.receiverPadding) ||
      query.receiverPadding < 0.0 ||
      !War3RtsShadowFinite(query.worldTexelSize) ||
      query.worldTexelSize <= 0.0)
    return result;

  War3RtsShadowVec3 planeNormal = {};
  War3RtsShadowVec3 lightRight = {};
  War3RtsShadowVec3 lightUp = {};
  if (!War3RtsShadowNormalize(query.planeNormal, planeNormal) ||
      !War3RtsShadowNormalize(query.lightRight, lightRight) ||
      !War3RtsShadowNormalize(query.lightUp, lightUp))
    return result;

  for (const auto& corner : query.frustumCorners) {
    if (!War3RtsShadowFinite(corner.x) || !War3RtsShadowFinite(corner.y) ||
        !War3RtsShadowFinite(corner.z))
      return result;
  }

  constexpr std::array<std::array<uint32_t, 2>, 12> kFrustumEdges = {{
      {{0u, 1u}}, {{1u, 2u}}, {{2u, 3u}}, {{3u, 0u}},
      {{4u, 5u}}, {{5u, 6u}}, {{6u, 7u}}, {{7u, 4u}},
      {{0u, 4u}}, {{1u, 5u}}, {{2u, 6u}}, {{3u, 7u}},
  }};
  std::array<War3RtsShadowVec3, 32> footprint = {};
  uint32_t footprintCount = 0u;
  const double lower = query.receiverPlaneHeight -
      query.receiverBandHalfHeight;
  const double upper = query.receiverPlaneHeight +
      query.receiverBandHalfHeight;
  const auto signedHeight = [&](War3RtsShadowVec3 point) noexcept {
    return War3RtsShadowDot(planeNormal, point);
  };
  const auto append = [&](War3RtsShadowVec3 point) noexcept {
    if (footprintCount < footprint.size())
      footprint[footprintCount++] = point;
  };

  for (const auto& corner : query.frustumCorners) {
    const double height = signedHeight(corner);
    if (height >= lower && height <= upper)
      append(corner);
  }
  for (const auto& edge : kFrustumEdges) {
    const War3RtsShadowVec3 a = query.frustumCorners[edge[0]];
    const War3RtsShadowVec3 b = query.frustumCorners[edge[1]];
    const double ha = signedHeight(a);
    const double hb = signedHeight(b);
    const double denominator = hb - ha;
    if (!War3RtsShadowFinite(denominator) ||
        std::abs(denominator) <= 1.0e-18)
      continue;
    for (const double boundary : {lower, upper}) {
      const double t = (boundary - ha) / denominator;
      if (t < 0.0 || t > 1.0 || !War3RtsShadowFinite(t))
        continue;
      append({a.x + (b.x - a.x) * t,
              a.y + (b.y - a.y) * t,
              a.z + (b.z - a.z) * t});
    }
  }
  if (footprintCount == 0u)
    return result;

  double minX = (std::numeric_limits<double>::max)();
  double minY = (std::numeric_limits<double>::max)();
  double maxX = (std::numeric_limits<double>::lowest)();
  double maxY = (std::numeric_limits<double>::lowest)();
  for (uint32_t i = 0u; i < footprintCount; i++) {
    const double x = War3RtsShadowDot(footprint[i], lightRight);
    const double y = War3RtsShadowDot(footprint[i], lightUp);
    minX = (std::min)(minX, x);
    minY = (std::min)(minY, y);
    maxX = (std::max)(maxX, x);
    maxY = (std::max)(maxY, y);
  }
  minX -= query.receiverPadding;
  minY -= query.receiverPadding;
  maxX += query.receiverPadding;
  maxY += query.receiverPadding;

  double centerX = 0.5 * (minX + maxX);
  double centerY = 0.5 * (minY + maxY);
  const double halfExtent = 0.5 * query.worldTexelSize *
      double(query.shadowResolution);
  const double requiredHalfExtent = (std::max)(
      0.5 * (maxX - minX), 0.5 * (maxY - minY));
  if (query.stableSnap) {
    centerX = std::round(centerX / query.worldTexelSize) *
        query.worldTexelSize;
    centerY = std::round(centerY / query.worldTexelSize) *
        query.worldTexelSize;
  }

  result.footprintPointCount = footprintCount;
  result.centerLightX = centerX;
  result.centerLightY = centerY;
  result.halfExtent = halfExtent;
  result.requiredHalfExtent = requiredHalfExtent;
  result.worldTexelSize = query.worldTexelSize;
  result.densitySatisfied =
      minX >= centerX - halfExtent && maxX <= centerX + halfExtent &&
      minY >= centerY - halfExtent && maxY <= centerY + halfExtent;
  result.valid = result.densitySatisfied &&
      War3RtsShadowFinite(centerX) && War3RtsShadowFinite(centerY) &&
      War3RtsShadowFinite(halfExtent) && halfExtent > 0.0;
  return result;
}

}  // namespace dxvk::war3::render
