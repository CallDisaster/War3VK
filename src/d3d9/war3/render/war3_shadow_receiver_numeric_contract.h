#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace dxvk::war3::render::shadowmath {

struct Vec2 {
  double x = 0.0;
  double y = 0.0;
};

struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Vec4 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double w = 0.0;
};

struct RowMajorMat4 {
  std::array<std::array<double, 4>, 4> rows = {};
};

struct ManualCompareFootprint {
  int p00x = 0;
  int p00y = 0;
  int p10x = 0;
  int p10y = 0;
  int p01x = 0;
  int p01y = 0;
  int p11x = 0;
  int p11y = 0;
  double phaseX = 0.0;
  double phaseY = 0.0;
};

struct ReceiverPlaneDifferentials {
  Vec2 uvDx = {};
  Vec2 uvDy = {};
  double depthDx = 0.0;
  double depthDy = 0.0;
};

inline bool finite(double value) {
  return std::isfinite(value) && std::abs(value) < 1.0e20;
}

inline bool finite(Vec2 value) {
  return finite(value.x) && finite(value.y);
}

inline bool finite(Vec3 value) {
  return finite(value.x) && finite(value.y) && finite(value.z);
}

inline bool finite(Vec4 value) {
  return finite(value.x) && finite(value.y) && finite(value.z) &&
    finite(value.w);
}

inline Vec4 rowVectorMultiply(Vec4 value, const RowMajorMat4& matrix) {
  Vec4 result = {};
  const std::array<double, 4> input = {
    value.x, value.y, value.z, value.w,
  };
  std::array<double, 4> output = {};
  for (size_t column = 0; column < 4; column++) {
    for (size_t row = 0; row < 4; row++)
      output[column] += input[row] * matrix.rows[row][column];
  }
  result = {output[0], output[1], output[2], output[3]};
  return result;
}

inline bool manualCompareFootprint(Vec2 uv, int width, int height,
                                   ManualCompareFootprint& footprint) {
  footprint = {};
  if (!finite(uv) || width <= 0 || height <= 0)
    return false;

  const double texelX = uv.x * static_cast<double>(width) - 0.5;
  const double texelY = uv.y * static_cast<double>(height) - 0.5;
  if (!finite(texelX) || !finite(texelY))
    return false;
  const int baseX = static_cast<int>(std::floor(texelX));
  const int baseY = static_cast<int>(std::floor(texelY));
  const auto clampX = [width](int value) {
    return std::clamp(value, 0, width - 1);
  };
  const auto clampY = [height](int value) {
    return std::clamp(value, 0, height - 1);
  };
  footprint = {
    clampX(baseX), clampY(baseY),
    clampX(baseX + 1), clampY(baseY),
    clampX(baseX), clampY(baseY + 1),
    clampX(baseX + 1), clampY(baseY + 1),
    texelX - std::floor(texelX), texelY - std::floor(texelY),
  };
  return true;
}

inline bool receiverPlaneDifferentialsRowMajor(
    const RowMajorMat4& lightViewProj, Vec3 worldPos, Vec3 worldDx,
    Vec3 worldDy, ReceiverPlaneDifferentials& differentials) {
  differentials = {};
  if (!finite(worldPos) || !finite(worldDx) || !finite(worldDy))
    return false;
  const Vec4 lightClip = rowVectorMultiply(
      {worldPos.x, worldPos.y, worldPos.z, 1.0}, lightViewProj);
  const Vec4 lightDx = rowVectorMultiply(
      {worldDx.x, worldDx.y, worldDx.z, 0.0}, lightViewProj);
  const Vec4 lightDy = rowVectorMultiply(
      {worldDy.x, worldDy.y, worldDy.z, 0.0}, lightViewProj);
  if (!finite(lightClip) || !finite(lightDx) || !finite(lightDy) ||
      std::abs(lightClip.w) < 1.0e-6)
    return false;
  const double invW = 1.0 / lightClip.w;
  const Vec3 ndc = {
    lightClip.x * invW, lightClip.y * invW, lightClip.z * invW,
  };
  const Vec3 ndcDx = {
    (lightDx.x - ndc.x * lightDx.w) * invW,
    (lightDx.y - ndc.y * lightDx.w) * invW,
    (lightDx.z - ndc.z * lightDx.w) * invW,
  };
  const Vec3 ndcDy = {
    (lightDy.x - ndc.x * lightDy.w) * invW,
    (lightDy.y - ndc.y * lightDy.w) * invW,
    (lightDy.z - ndc.z * lightDy.w) * invW,
  };
  if (!finite(ndc) || !finite(ndcDx) || !finite(ndcDy))
    return false;
  // This is the shader's fixed NDC-to-UV convention: X keeps its sign and
  // Y is flipped because the shadow texture has top-left UV origin.
  differentials = {
    {0.5 * ndcDx.x, -0.5 * ndcDx.y},
    {0.5 * ndcDy.x, -0.5 * ndcDy.y},
    ndcDx.z, ndcDy.z,
  };
  return finite(differentials.uvDx) && finite(differentials.uvDy) &&
    finite(differentials.depthDx) && finite(differentials.depthDy);
}

inline bool cascadeProjectionValid(Vec4 lightClip) {
  if (!finite(lightClip) || lightClip.w <= 0.0)
    return false;
  const Vec3 ndc = {
    lightClip.x / lightClip.w,
    lightClip.y / lightClip.w,
    lightClip.z / lightClip.w,
  };
  if (!finite(ndc) || ndc.z < 0.0 || ndc.z > 1.0)
    return false;
  const Vec2 uv = {0.5 * ndc.x + 0.5, 1.0 - (0.5 * ndc.y + 0.5)};
  return finite(uv) && uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 &&
    uv.y <= 1.0;
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
