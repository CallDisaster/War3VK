#pragma once

#include "../render/war3_render_exec_batch.h"
#include "war3_stage_tag_map.h"

#include <cstdint>

namespace dxvk::war3::render {
class RenderQueueTracker;
}

namespace dxvk::war3::hooks {

/**
 * @brief Dispatch 局部合并状态。
 *
 * 用于在同一 renderablePart 连续 Dispatch 时复用 ExecBatch 上下文，
 * 减少 Begin/End 往返开销。
 */
struct DispatchLocalMergeState {
  bool active = false;
  bool isType3 = false;
  void *renderablePart = nullptr;
  War3BatchTag tag = War3BatchTag::Unknown;
  int stage = -1;
  dxvk::war3::render::ExecBatchContext ctx = {};
};

/**
 * @brief Dispatch 查询请求契约。
 */
struct War3DispatchQueryRequest {
  void *renderablePart = nullptr;
  int stageHint = -1;
  std::uint32_t modeFlags = 0;
};

/**
 * @brief Dispatch 查询结果来源。
 */
enum class War3DispatchCacheSource : std::uint8_t {
  None = 0,
  HotRenderable,
  RenderableSlot,
  RenderableSlotScan,
  HotScene,
  SceneSlot,
  SceneSlotScan,
  Tracker,
};

/**
 * @brief Dispatch 查询结果契约。
 */
struct War3DispatchQueryResult {
  bool resolved = false;
  War3BatchTag tag = War3BatchTag::Unknown;
  int stage = -1;
  War3DispatchCacheSource cacheSource = War3DispatchCacheSource::None;
};

constexpr std::uint32_t kDispatchTagStageCacheSlotCount = 32;

/**
 * @brief Dispatch tag/stage 缓存条目。
 */
struct DispatchTagStageCacheEntry {
  bool valid = false;
  bool resolved = false;
  void *renderablePart = nullptr;
  void *sceneNode = nullptr;
  War3BatchTag tag = War3BatchTag::Unknown;
  int stage = -1;
  std::uint32_t stamp = 0;
};

/**
 * @brief Dispatch tag/stage 缓存状态。
 */
struct DispatchTagStageCacheState {
  DispatchTagStageCacheEntry slots[kDispatchTagStageCacheSlotCount];
  std::uint32_t clock = 0;
  bool hotRenderableValid = false;
  bool hotRenderableResolved = false;
  void *hotRenderablePart = nullptr;
  War3BatchTag hotRenderableTag = War3BatchTag::Unknown;
  int hotRenderableStage = -1;
  bool hotSceneValid = false;
  bool hotSceneResolved = false;
  void *hotSceneNode = nullptr;
  War3BatchTag hotSceneTag = War3BatchTag::Unknown;
  int hotSceneStage = -1;
};

DispatchLocalMergeState &GetDispatchLocalMergeState();
void ResetDispatchLocalMergeState(DispatchLocalMergeState &state);
void ResetDispatchLocalMergeState();

DispatchTagStageCacheState &GetDispatchTagStageCacheState();
void ResetDispatchTagStageCacheState(DispatchTagStageCacheState &state);
void ResetDispatchTagStageCacheState();

War3DispatchQueryResult QueryTagStageCached(
    dxvk::war3::render::RenderQueueTracker &tracker,
    const War3DispatchQueryRequest &request);

bool QueryTagStageCached(dxvk::war3::render::RenderQueueTracker &tracker,
                         void *renderablePart, War3BatchTag &outTag,
                         int &outStage);

} // namespace dxvk::war3::hooks

