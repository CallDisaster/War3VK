#pragma once

#include "../../d3d9_war3_debug.h"
#include "../render/war3_render_state.h"
#include "war3_render_types.h"
#include <algorithm>
#include <cstdint>
#include <cstring>


namespace dxvk {
namespace war3 {
namespace reimpl {

class War3BatchMerger {
public:
  void Reset() {
    m_active = false;
    m_savedDrawCalls = 0;
    m_totalBatches = 0;
    m_lastBatch = nullptr;
    m_lastTag = War3BatchTag::Unknown;
  }

  void Analyze(const RenderBatchElement *currentBatch,
               War3BatchTag currentTag) {
    m_totalBatches++;
    bool isSpecial = (currentBatch->flags & 3) == 3;

    // Only optimized standard batches (not Special/Type3)
    // And only WorldObjects support instancing for now
    bool supportInstancing =
        !isSpecial && (currentTag == War3BatchTag::WorldObjects);

    if (!m_active) {
      if (supportInstancing) {
        m_active = true; // Start a new sequence
        m_lastBatch = currentBatch;
        m_lastTag = currentTag;
      }
      return;
    }

    // Attempt merge
    if (supportInstancing && CanMerge(m_lastBatch, currentBatch)) {
      m_savedDrawCalls++;
    } else {
      // Break sequence
      if (supportInstancing) {
        // Start new sequence
        m_lastBatch = currentBatch;
        m_lastTag = currentTag;
      } else {
        m_active = false;
        m_lastBatch = nullptr;
      }
    }
  }

  void LogStats(uint32_t frameId) {
    if (frameId % 300 == 0 && m_totalBatches > 0) {
      WAR3_RENDER_LOG(
          "[BatchMerger] Potential Saved DrawCalls: %u / %u (%.1f%%)\n",
          m_savedDrawCalls, m_totalBatches,
          100.0f * m_savedDrawCalls / m_totalBatches);
    }
  }

private:
  bool CanMerge(const RenderBatchElement *a, const RenderBatchElement *b) {
    // Check MeshData (Geometry)
    // Unsafe read relying on caller validation
    // RenderablePart + 0x0C is MeshData*
    // RenderablePart is accessible as void* in RenderBatchElement.

    // We assume pointers are valid here as they are validated in
    // FlushSortedItems loop
    void *meshDataA =
        *reinterpret_cast<void **>((uint8_t *)a->renderablePart + 0x0C);
    void *meshDataB =
        *reinterpret_cast<void **>((uint8_t *)b->renderablePart + 0x0C);

    if (meshDataA != meshDataB)
      return false;

    // Check SubIndex / LayerIndex
    if (a->subIndex != b->subIndex)
      return false;
    if (a->layerIndex != b->layerIndex)
      return false;

    // Check Material/State (First 20 bytes of LayerStateBlock)
    if (std::memcmp(a->layerStatePtr, b->layerStatePtr, 20) != 0)
      return false;

    return true;
  }

  bool m_active = false;
  const RenderBatchElement *m_lastBatch = nullptr;
  War3BatchTag m_lastTag = War3BatchTag::Unknown;

  uint32_t m_savedDrawCalls = 0;
  uint32_t m_totalBatches = 0;
};

} // namespace reimpl
} // namespace war3
} // namespace dxvk
