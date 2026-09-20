#pragma once

#include "core/war3_game_structs.h"
#include "core/war3_internal_test_config.h"
#include "core/war3_memory.h"
#include "render/war3_render_objects.h"
#include "shadow/war3_shadow_renderer_core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dxvk::war3 {

struct PathBlockerLocalGeometryBounds {
  float minX = 0.0f;
  float maxX = 0.0f;
  float minY = 0.0f;
  float maxY = 0.0f;
  float minZ = 0.0f;
  float maxZ = 0.0f;
};

inline bool PathBlockerBelowGroundFlatMarkerBoundsFit(
    const PathBlockerLocalGeometryBounds& bounds) {
  const float extentX = bounds.maxX - bounds.minX;
  const float extentY = bounds.maxY - bounds.minY;
  const float extentZ = bounds.maxZ - bounds.minZ;
  return bounds.maxZ <=
             internal::kPathBlockerBelowGroundFlatMarkerMaxLocalZ &&
         extentZ <=
             internal::kPathBlockerBelowGroundFlatMarkerMaxZSpan &&
         extentX > 0.01f && extentY > 0.01f &&
         extentX <=
             internal::kPathBlockerBelowGroundFlatMarkerMaxXYExtent &&
         extentY <=
             internal::kPathBlockerBelowGroundFlatMarkerMaxXYExtent;
}

inline bool PathBlockerHasDynamicUnitEvidence(
    const shadow::ShadowDrawPacket& packet) {
  const auto& renderable = packet.renderable;
  return renderable.queueKind !=
             render::VisibleRenderableQueueKind::Transparent &&
         renderable.groupIdx == 0 &&
         (renderable.unitFlags5C & UnitFlags5C::Building) == 0u &&
         renderable.objectKind == render::ObjectKind::Unit &&
         renderable.unitPtr != nullptr &&
         (renderable.rawcode != 0u || renderable.jHandle != 0u) &&
         packet.path == shadow::ShadowDrawPath::Skinned;
}

inline bool PathBlockerComputePacketLocalBounds(
    const shadow::ShadowDrawPacket& packet,
    uint32_t vertexCount,
    PathBlockerLocalGeometryBounds& outBounds) {
  if (packet.resource.dynamicPositionStream != nullptr)
    return false;

  const auto& positions = packet.resource.positionVec();
  if (vertexCount == 0u || positions.size() < size_t(vertexCount) * 3u)
    return false;

  float x = positions[0u];
  float y = positions[1u];
  float z = positions[2u];
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
    return false;

  outBounds.minX = outBounds.maxX = x;
  outBounds.minY = outBounds.maxY = y;
  outBounds.minZ = outBounds.maxZ = z;

  for (uint32_t i = 1u; i < vertexCount; ++i) {
    const size_t base = size_t(i) * 3u;
    x = positions[base + 0u];
    y = positions[base + 1u];
    z = positions[base + 2u];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
      return false;
    outBounds.minX = (std::min)(outBounds.minX, x);
    outBounds.maxX = (std::max)(outBounds.maxX, x);
    outBounds.minY = (std::min)(outBounds.minY, y);
    outBounds.maxY = (std::max)(outBounds.maxY, y);
    outBounds.minZ = (std::min)(outBounds.minZ, z);
    outBounds.maxZ = (std::max)(outBounds.maxZ, z);
  }

  return true;
}

inline bool PathBlockerTryReadWidgetRawcode(void* widgetPtr,
                                            uint32_t& outRawcode) {
  outRawcode = 0u;
  if (widgetPtr == nullptr)
    return false;

  uint32_t magic = 0u;
  if (!SafeReadU32Fast(widgetPtr, 0x0Cu, magic) ||
      magic != 0x2B5DB42Cu) {
    return false;
  }

  uint32_t rawcode = 0u;
  if (!SafeReadU32Fast(widgetPtr, 0x30u, rawcode) ||
      rawcode == 0u ||
      !internal::IsPathBlockerFourCc(rawcode)) {
    return false;
  }

  outRawcode = rawcode;
  return true;
}

inline bool PathBlockerTryResolveRawcode(
    shadow::ShadowRenderableRecord& renderable) {
  if (internal::IsPathBlockerFourCc(renderable.rawcode))
    return true;

  if (renderable.jHandle != 0u) {
    if (const auto* info =
            render::RenderObjectRegistry::instance().findByHandle(
                renderable.jHandle)) {
      if (internal::IsPathBlockerFourCc(info->rawcode)) {
        renderable.rawcode = info->rawcode;
        return true;
      }
    }
  }

  uint32_t rawcode = 0u;
  if (PathBlockerTryReadWidgetRawcode(renderable.worldObjectEntry,
                                      rawcode) ||
      (renderable.unitPtr != renderable.worldObjectEntry &&
       PathBlockerTryReadWidgetRawcode(renderable.unitPtr, rawcode))) {
    renderable.rawcode = rawcode;
    return true;
  }

  return false;
}

inline bool PathBlockerPacketCanUseBelowGroundFlatMarkerFallback(
    const shadow::ShadowDrawPacket& packet) {
  if (!internal::kPathBlockerHideEnabled ||
      !internal::kPathBlockerBelowGroundFlatMarkerGateEnabled) {
    return false;
  }
  if (packet.path != shadow::ShadowDrawPath::Rigid)
    return false;
  if (PathBlockerHasDynamicUnitEvidence(packet))
    return false;

  const auto& renderable = packet.renderable;
  if (renderable.rawcode != 0u &&
      !internal::IsPathBlockerFourCc(renderable.rawcode)) {
    return false;
  }
  if (renderable.objectKind == render::ObjectKind::Unit &&
      renderable.unitPtr != nullptr) {
    return false;
  }

  return renderable.pathBlocker || renderable.stage == 11 ||
         renderable.groupIdx > 0 ||
         renderable.objectKind == render::ObjectKind::Unknown ||
         renderable.objectKind == render::ObjectKind::Destructible;
}

inline bool PathBlockerPacketIsBelowGroundFlatMarkerGeometry(
    const shadow::ShadowDrawPacket& packet) {
  if (!PathBlockerPacketCanUseBelowGroundFlatMarkerFallback(packet))
    return false;

  const uint32_t vertexCount =
      packet.resource.vertexCount != 0u
          ? packet.resource.vertexCount
          : uint32_t(packet.resource.positionVec().size() / 3u);
  const uint32_t indexCount =
      packet.resource.dynamicIndexCount != 0u
          ? packet.resource.dynamicIndexCount
          : uint32_t(packet.resource.indexVec().size());
  if (vertexCount == 0u ||
      vertexCount >
          internal::kPathBlockerBelowGroundFlatMarkerMaxVertices) {
    return false;
  }
  if (indexCount >
      internal::kPathBlockerBelowGroundFlatMarkerMaxIndices) {
    return false;
  }

  PathBlockerLocalGeometryBounds bounds = {};
  if (packet.resource.localBounds.valid) {
    bounds.minX = packet.resource.localBounds.minX;
    bounds.maxX = packet.resource.localBounds.maxX;
    bounds.minY = packet.resource.localBounds.minY;
    bounds.maxY = packet.resource.localBounds.maxY;
    bounds.minZ = packet.resource.localBounds.minZ;
    bounds.maxZ = packet.resource.localBounds.maxZ;
  } else if (!PathBlockerComputePacketLocalBounds(packet, vertexCount,
                                                  bounds)) {
    return false;
  }

  return PathBlockerBelowGroundFlatMarkerBoundsFit(bounds);
}

inline bool PathBlockerShouldSubmitPacket(
    shadow::ShadowDrawPacket& packet,
    bool& outPathBlocker,
    bool& outGeometryMarker) {
  outPathBlocker = false;
  outGeometryMarker = false;

  auto& renderable = packet.renderable;
  renderable.pathBlocker =
      renderable.pathBlocker || PathBlockerTryResolveRawcode(renderable);

  if (!internal::kPathBlockerHideEnabled)
    return true;

  if (renderable.pathBlocker) {
    outPathBlocker = true;
    return false;
  }

  if (PathBlockerPacketIsBelowGroundFlatMarkerGeometry(packet)) {
    outGeometryMarker = true;
    renderable.pathBlocker = true;
    return false;
  }

  return true;
}

} // namespace dxvk::war3
