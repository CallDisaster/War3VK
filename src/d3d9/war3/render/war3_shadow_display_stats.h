#pragma once
#include <cstdint>

namespace dxvk::war3::render {
// Copies only existing published fields under the scene-stat lock. It never
// asks the semantic builder to advance or holds geometry/resource references.
struct ShadowDisplayStats {
  uint64_t producerFrame = 0, mapRenderSerial = 0;
  uint32_t hasCompleteMap = 0, mapExecuted = 0, receiverExecuted = 0;
  uint32_t earlyReturnReason = 0, prepared = 0, terrainDoodadPrepared = 0;
  uint32_t cascadeDrawn[4] = {};
};
ShadowDisplayStats QueryShadowDisplayStats();
}
