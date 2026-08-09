#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace dxvk::war3::render::shadowmath {

struct Vec2 {
  double x = 0.0;
  double y = 0.0;
};

inline bool finite(double value) {
  return std::isfinite(value) && std::abs(value) < 1.0e20;
}

inline bool receiverPlaneDepthGradient(Vec2 uvDx, Vec2 uvDy,
                                       double depthDx, double depthDy,
                                       Vec2& gradient) {
  gradient = {};
  if (!finite(uvDx.x) || !finite(uvDx.y) || !finite(uvDy.x) ||
      !finite(uvDy.y) || !finite(depthDx) || !finite(depthDy))
    return false;

  const double determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
  if (!finite(determinant) || std::abs(determinant) < 1.0e-10)
    return false;

  gradient = {
    (depthDx * uvDy.y - uvDx.y * depthDy) / determinant,
    (uvDx.x * depthDy - depthDx * uvDy.x) / determinant,
  };
  return finite(gradient.x) && finite(gradient.y);
}

inline bool receiverPlaneKernelValid(Vec2 gradient, double maxAbsOffsetUv) {
  if (!finite(gradient.x) || !finite(gradient.y) ||
      !finite(maxAbsOffsetUv) || maxAbsOffsetUv < 0.0)
    return false;
  constexpr double kMaxReceiverPlaneDepthDelta = 0.0025;
  const double worstDepthDelta =
    (std::abs(gradient.x) + std::abs(gradient.y)) * maxAbsOffsetUv;
  return finite(worstDepthDelta) &&
    worstDepthDelta <= kMaxReceiverPlaneDepthDelta;
}

inline double receiverPlaneTapReference(double centerReference,
                                        Vec2 tapOffsetUv, Vec2 gradient,
                                        bool kernelValid) {
  const double reference = kernelValid
    ? centerReference + gradient.x * tapOffsetUv.x + gradient.y * tapOffsetUv.y
    : centerReference;
  return std::clamp(reference, 0.0, 1.0);
}

// Exact scalar equivalent of the shader's compare-first manual 2x2 fallback.
// depths are ordered p00, p10, p01, p11. The phase arguments are the frac()
// components of `uv * extent - 0.5` after integer texel selection.
inline double manualCompareLinear2x2(const std::array<double, 4>& depths,
                                     double reference, double phaseX,
                                     double phaseY) {
  const auto compare = [reference](double depth) {
    return reference <= depth ? 1.0 : 0.0;
  };
  const double v00 = compare(depths[0]);
  const double v10 = compare(depths[1]);
  const double v01 = compare(depths[2]);
  const double v11 = compare(depths[3]);
  const double top = v00 + (v10 - v00) * phaseX;
  const double bottom = v01 + (v11 - v01) * phaseX;
  return top + (bottom - top) * phaseY;
}

inline double smoothstep01(double value) {
  const double t = std::clamp(value, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

// Both DirectInline and Prepass keep primary visibility if the next cascade
// has no valid projection. A next-cascade failure must not blend in "lit".
inline double blendCascadeVisibility(double primary, double next,
                                    double viewDepth, double farSplit,
                                    double blendRange,
                                    bool nextCascadeValid) {
  if (!nextCascadeValid || !finite(blendRange) || blendRange <= 0.0)
    return primary;
  const double t = (viewDepth - (farSplit - blendRange)) / blendRange;
  const double weight = smoothstep01(t);
  return primary + (next - primary) * weight;
}

}  // namespace dxvk::war3::render::shadowmath
