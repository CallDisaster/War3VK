#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace dxvk::war3::model {

// Derived only from a complete immutable position stream.  The model cache
// stores this value beside the generation it minted; callers may carry the
// pair but must never treat bounds computed from an unversioned pointer as
// culling authority.
struct ShadowGeosetLocalBounds {
  float minX = 0.0f;
  float minY = 0.0f;
  float minZ = 0.0f;
  float maxX = 0.0f;
  float maxY = 0.0f;
  float maxZ = 0.0f;
  float centerX = 0.0f;
  float centerY = 0.0f;
  float centerZ = 0.0f;
  float radius = 0.0f;
  bool valid = false;
};

inline ShadowGeosetLocalBounds ComputeShadowGeosetLocalBounds(
    const std::vector<float>& positions, uint32_t vertexCount) noexcept {
  ShadowGeosetLocalBounds out = {};
  if (vertexCount == 0u ||
      uint64_t(vertexCount) * 3u != uint64_t(positions.size())) {
    return out;
  }

  float minX = positions[0];
  float minY = positions[1];
  float minZ = positions[2];
  float maxX = minX;
  float maxY = minY;
  float maxZ = minZ;
  if (!std::isfinite(minX) || !std::isfinite(minY) ||
      !std::isfinite(minZ)) {
    return out;
  }

  for (uint32_t vertex = 1u; vertex < vertexCount; ++vertex) {
    const size_t base = size_t(vertex) * 3u;
    const float x = positions[base + 0u];
    const float y = positions[base + 1u];
    const float z = positions[base + 2u];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
      return {};
    minX = std::min(minX, x);
    minY = std::min(minY, y);
    minZ = std::min(minZ, z);
    maxX = std::max(maxX, x);
    maxY = std::max(maxY, y);
    maxZ = std::max(maxZ, z);
  }

  const float centerX = minX + (maxX - minX) * 0.5f;
  const float centerY = minY + (maxY - minY) * 0.5f;
  const float centerZ = minZ + (maxZ - minZ) * 0.5f;
  if (!std::isfinite(centerX) || !std::isfinite(centerY) ||
      !std::isfinite(centerZ)) {
    return {};
  }

  double radiusSq = 0.0;
  for (uint32_t vertex = 0u; vertex < vertexCount; ++vertex) {
    const size_t base = size_t(vertex) * 3u;
    const double dx = double(positions[base + 0u]) - double(centerX);
    const double dy = double(positions[base + 1u]) - double(centerY);
    const double dz = double(positions[base + 2u]) - double(centerZ);
    const double distanceSq = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(distanceSq))
      return {};
    radiusSq = std::max(radiusSq, distanceSq);
  }
  const double radius = std::sqrt(radiusSq);
  if (!std::isfinite(radius) ||
      radius > double(std::numeric_limits<float>::max())) {
    return {};
  }

  out.minX = minX;
  out.minY = minY;
  out.minZ = minZ;
  out.maxX = maxX;
  out.maxY = maxY;
  out.maxZ = maxZ;
  out.centerX = centerX;
  out.centerY = centerY;
  out.centerZ = centerZ;
  out.radius = float(radius);
  out.valid = true;
  return out;
}

} // namespace dxvk::war3::model
