#pragma once

#include <algorithm>
#include <cmath>

namespace dxvk::war3::render::shadowalpha {

struct Vec2 {
  double x = 0.0;
  double y = 0.0;
};

struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

inline bool finite(Vec2 value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

inline bool finite(Vec3 value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

inline double length(Vec2 value) noexcept {
  return std::sqrt(value.x * value.x + value.y * value.y);
}

inline double length(Vec3 value) noexcept {
  return std::sqrt(value.x * value.x + value.y * value.y +
                   value.z * value.z);
}

inline double fract(double value) noexcept {
  return value - std::floor(value);
}

// Wyman and McGuire, "Hashed Alpha Testing", I3D 2017, Listing 1.
// This CPU form is a deterministic numerical oracle for the directional
// caster shader. It owns no render state and never authorizes a GPU route.
inline double hash2(double x, double y) noexcept {
  return fract(1.0e4 * std::sin(17.0 * x + 0.1 * y) *
               (0.1 + std::abs(std::sin(13.0 * y + x))));
}

inline double hash3(Vec3 value) noexcept {
  return hash2(hash2(value.x, value.y), value.z);
}

inline double uniformizeInterpolatedHashes(double mixed,
                                           double interpolation) noexcept {
  if (!std::isfinite(mixed) || !std::isfinite(interpolation))
    return 0.5;

  const double x = std::clamp(mixed, 0.0, 1.0);
  const double t = std::clamp(interpolation, 0.0, 1.0);
  const double a = std::min(t, 1.0 - t);
  if (a <= 1.0e-8)
    return std::clamp(x, 1.0e-6, 1.0);

  const double denominator = 2.0 * a * (1.0 - a);
  double threshold = 0.0;
  if (x < a)
    threshold = x * x / denominator;
  else if (x < 1.0 - a)
    threshold = (x - 0.5 * a) / (1.0 - a);
  else
    threshold = 1.0 - (1.0 - x) * (1.0 - x) / denominator;
  return std::clamp(threshold, 1.0e-6, 1.0);
}

inline bool stableHashedThreshold(Vec3 surfaceCoordinate,
                                  Vec3 surfaceDx,
                                  Vec3 surfaceDy,
                                  double hashScale,
                                  double& threshold) noexcept {
  threshold = 0.5;
  if (!finite(surfaceCoordinate) || !finite(surfaceDx) ||
      !finite(surfaceDy) || !std::isfinite(hashScale) ||
      hashScale <= 0.0)
    return false;

  const double maxDerivative = std::max(length(surfaceDx), length(surfaceDy));
  if (!std::isfinite(maxDerivative) || maxDerivative <= 1.0e-8)
    return false;

  const double pixelScale = 1.0 / (hashScale * maxDerivative);
  if (!std::isfinite(pixelScale) || pixelScale <= 0.0)
    return false;

  // The finite clamp prevents extreme coordinates from entering undefined
  // transcendental domains while preserving all practical Warcraft scales.
  const double logScale = std::clamp(std::log2(pixelScale), -24.0, 24.0);
  const double scale0 = std::exp2(std::floor(logScale));
  const double scale1 = std::exp2(std::ceil(logScale));
  const auto cell = [&](double scale) noexcept {
    return Vec3{std::floor(surfaceCoordinate.x * scale),
                std::floor(surfaceCoordinate.y * scale),
                std::floor(surfaceCoordinate.z * scale)};
  };
  const Vec3 cell0 = cell(scale0);
  const Vec3 cell1 = cell(scale1);
  if (!finite(cell0) || !finite(cell1))
    return false;
  const double h0 = hash3(cell0);
  const double h1 = hash3(cell1);
  const double interpolation = fract(logScale);
  threshold = uniformizeInterpolatedHashes(
      h0 + (h1 - h0) * interpolation, interpolation);
  return std::isfinite(threshold) && threshold >= 1.0e-6 &&
         threshold <= 1.0;
}

inline double coverageBlendFromTextureFootprint(Vec2 uvDx,
                                                Vec2 uvDy,
                                                double textureWidth,
                                                double textureHeight,
                                                double fullHashLod = 6.0) noexcept {
  if (!finite(uvDx) || !finite(uvDy) || !std::isfinite(textureWidth) ||
      !std::isfinite(textureHeight) || !std::isfinite(fullHashLod) ||
      textureWidth <= 0.0 || textureHeight <= 0.0 || fullHashLod <= 0.0)
    return 0.0;

  const double footprintX = length(
      Vec2{uvDx.x * textureWidth, uvDx.y * textureHeight});
  const double footprintY = length(
      Vec2{uvDy.x * textureWidth, uvDy.y * textureHeight});
  const double maxFootprint = std::max(footprintX, footprintY);
  if (!std::isfinite(maxFootprint) || maxFootprint <= 1.0)
    return 0.0;

  const double minFootprint = std::min(footprintX, footprintY);
  const double anisotropy = minFootprint > 1.0e-8
      ? std::max(footprintX / minFootprint,
                 footprintY / minFootprint)
      : 1.0;
  const double lod = std::max(std::log2(maxFootprint), 0.0);
  const double normalized = std::clamp(
      anisotropy * lod / fullHashLod, 0.0, 1.0);
  return normalized * normalized;
}

inline double stableCoverageThreshold(double authoredAlphaReference,
                                      double hashedThreshold,
                                      double coverageBlend) noexcept {
  if (!std::isfinite(authoredAlphaReference) ||
      !std::isfinite(hashedThreshold) || !std::isfinite(coverageBlend))
    return std::clamp(
        std::isfinite(authoredAlphaReference) ? authoredAlphaReference : 0.5,
        0.0, 1.0);
  const double reference = std::clamp(authoredAlphaReference, 0.0, 1.0);
  const double hashed = std::clamp(hashedThreshold, 1.0e-6, 1.0);
  const double blend = std::clamp(coverageBlend, 0.0, 1.0);
  return reference + (hashed - reference) * blend;
}

}  // namespace dxvk::war3::render::shadowalpha
