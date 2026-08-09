// war3_render_state.cpp - Render state tracking implementation

#include "war3_render_state.h"
#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_settings.h"
#include "../../util/util_env.h"
#include "../core/war3_internal_test_config.h"
#include "../war3.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace dxvk {

namespace {
constexpr uint32_t kOutlineAllHandle = 0x0010000Du;
std::atomic<bool> g_outlineAllObjects{false};
std::atomic<bool> g_outlineAllChecked{false};
std::atomic<bool> g_outlineForceChecked{false};
std::atomic<bool> g_outlineForceEnabled{false};

std::atomic<int> g_stateStage{-1};
std::atomic<bool> g_stateValid{false};
std::atomic<bool> g_stageTouchedThisFrame{false};
std::atomic<int> g_maxStageThisFrame{-1};
std::atomic<int> g_maxStageCompletedThisFrame{-1};
std::atomic<int> g_maxMainWorldStageCompletedThisFrame{-1};
std::atomic<bool> g_mainWorldStageActive{false};
std::atomic<bool> g_currentViewportValid{false};
std::atomic<uint32_t> g_currentViewportX{0u};
std::atomic<uint32_t> g_currentViewportY{0u};
std::atomic<uint32_t> g_currentViewportWidth{0u};
std::atomic<uint32_t> g_currentViewportHeight{0u};
std::atomic<uint64_t> g_currentViewportSerial{0u};
std::atomic<bool> g_worldFramePrepareTouchedThisFrame{false};
std::atomic<bool> g_worldFramePrepareCompletedThisFrame{false};
std::atomic<bool> g_worldRenderSceneTouchedThisFrame{false};
std::atomic<bool> g_worldRenderSceneCompletedThisFrame{false};
std::atomic<bool> g_worldRenderSceneActive{false};
std::atomic<bool> g_uiDispatchTouchedThisFrame{false};
std::atomic<int> g_dispatcherStage{-1};
std::atomic<bool> g_dispatcherTouchedThisFrame{false};
std::atomic<bool> g_uiBatchTouchedThisFrame{false};
std::atomic<War3RenderLayer> g_stateLayer{War3RenderLayer::Unknown};
std::atomic<bool> g_stateSkipUi{false};
std::atomic<War3BatchTag> g_stateBatchTag{War3BatchTag::Unknown};
thread_local War3BatchTag g_tlsBatchTag = War3BatchTag::Unknown;
thread_local int g_tlsDispatcherStage = -1;
thread_local uint32_t g_tlsBatchHandle = 0;
thread_local uint32_t g_tlsDispatchHandle = 0;
std::atomic<int> g_terrainDepth{0};
std::atomic<uint32_t> g_lastRenderHandle{0};

std::unordered_set<uint32_t> g_outlineHandles;
std::shared_mutex g_outlineHandleMutex;
std::atomic<uint32_t> g_outlineHandleCount{0};
std::unordered_map<uint32_t, float> g_bloomHandles;
std::shared_mutex g_bloomHandleMutex;
std::atomic<uint32_t> g_bloomHandleCount{0};
std::atomic<uint32_t> g_outlineDebugLogCount{0};
std::atomic<bool> g_forceObjectTrackingChecked{false};
std::atomic<bool> g_forceObjectTracking{false};
std::atomic<bool> g_needsObjectTrackingCached{false};
std::atomic<bool> g_needsShadowSemanticTrackingCached{false};
std::atomic<bool> g_needsShadowObjectIdentityCached{false};
std::atomic<bool> g_needsShadowDrawFallbackBridgeCached{false};
std::atomic<bool> g_batchTagTrackingEnabled{true};
std::atomic<uint32_t> g_nativeShadowMode{
    ::dxvk::war3::internal::kNativeShadowDefaultMode};

std::atomic<War3RenderState::DebugRenderMode> g_debugMode{
    War3RenderState::DebugRenderMode::Normal};

std::atomic<float> g_gameTime = -1.0f;
thread_local War3TlsShadowSemanticState g_tlsShadowSemanticState = {};

uint32_t NormalizeHandle(uint32_t handle) {
  const uint32_t handleId = handle & 0x0FFFFFu;
  if (handleId == 0u)
    return 0u;
  return handleId | 0x100000u;
}

bool IsOutlineAllObjectsEnabled() {
  if (!g_outlineAllChecked.exchange(true, std::memory_order_relaxed)) {
    // 内部测试版本：不依赖环境变量，直接使用编译期配置。
    g_outlineAllObjects.store(dxvk::war3::internal::kOutlineAllObjectsEnabled,
                              std::memory_order_relaxed);
  }
  return g_outlineAllObjects.load(std::memory_order_relaxed);
}

bool IsOutlineForceEnabled() {
  if (!g_outlineForceChecked.exchange(true, std::memory_order_relaxed)) {
    // 内部测试版本：不依赖环境变量，直接使用编译期配置。
    g_outlineForceEnabled.store(dxvk::war3::internal::kOutlineForceEnabled,
                                std::memory_order_relaxed);
  }
  return g_outlineForceEnabled.load(std::memory_order_relaxed);
}

bool ComputeNeedsObjectTracking() {
  if (!g_forceObjectTrackingChecked.exchange(true, std::memory_order_relaxed)) {
    // 内部测试版本：不依赖环境变量，直接使用编译期配置。
    g_forceObjectTracking.store(
        dxvk::war3::internal::kForceObjectTrackingEnabled,
        std::memory_order_relaxed);
  }
  if (g_forceObjectTracking.load(std::memory_order_relaxed))
    return true;

  if (dxvk::war3::internal::kWar3RenderModuleTakeoverEnabled &&
      dxvk::war3::internal::kPathBlockerHideEnabled &&
      dxvk::war3::internal::kPathBlockerForceBridgeTrackingEnabled) {
    return true;
  }

  if (IsOutlineAllObjectsEnabled())
    return true;

  {
    std::shared_lock<std::shared_mutex> lock(g_outlineHandleMutex);
    if (!g_outlineHandles.empty())
      return true;
  }

  {
    std::shared_lock<std::shared_mutex> lock(g_bloomHandleMutex);
    if (!g_bloomHandles.empty())
      return true;
  }

  return false;
}
} // namespace

void War3RenderState::SetStage(int stage) {
  g_stateStage.store(stage, std::memory_order_relaxed);
  g_stateValid.store(true, std::memory_order_relaxed);
  g_stageTouchedThisFrame.store(true, std::memory_order_relaxed);

  int prevMax = g_maxStageThisFrame.load(std::memory_order_relaxed);
  while (stage > prevMax && !g_maxStageThisFrame.compare_exchange_weak(
                                prevMax, stage, std::memory_order_relaxed,
                                std::memory_order_relaxed)) {
    // prevMax updated by compare_exchange_weak
  }
}

int War3RenderState::GetStage() {
  return g_stateStage.load(std::memory_order_relaxed);
}

void War3RenderState::SetDispatcherStage(int stage) {
  g_tlsDispatcherStage = stage;
  g_dispatcherStage.store(stage, std::memory_order_relaxed);
  g_dispatcherTouchedThisFrame.store(true, std::memory_order_relaxed);

  if (stage == 67) {
    g_uiBatchTouchedThisFrame.store(true, std::memory_order_relaxed);
  }
}

int War3RenderState::GetDispatcherStage() { return g_tlsDispatcherStage; }

bool War3RenderState::HasUiBatchStageThisFrame() {
  return g_uiBatchTouchedThisFrame.load(std::memory_order_relaxed);
}

bool War3RenderState::IsUiBatchStage() { return GetDispatcherStage() == 67; }

void War3RenderState::PushUiLayer() {
  g_stateLayer.store(War3RenderLayer::UI, std::memory_order_relaxed);
}

War3RenderLayer War3RenderState::PopLayer(War3RenderLayer prev) {
  War3RenderLayer old = g_stateLayer.load(std::memory_order_relaxed);
  g_stateLayer.store(prev, std::memory_order_relaxed);
  return old;
}

War3RenderLayer War3RenderState::CurrentLayer() {
  return g_stateLayer.load(std::memory_order_relaxed);
}

War3RenderState::StageCategory War3RenderState::GetStageCategory() {
  const int stage = g_stateStage.load(std::memory_order_relaxed);
  const bool valid = g_stateValid.load(std::memory_order_relaxed);

  if (g_stateLayer.load(std::memory_order_relaxed) == War3RenderLayer::UI)
    return StageCategory::UI;

  if (IsUiBatchStage())
    return StageCategory::UI;

  const War3BatchTag batchTag = GetCurrentBatchTag();
  if (batchTag == War3BatchTag::UI)
    return StageCategory::UI;
  if (batchTag == War3BatchTag::SelectionOverlay)
    return StageCategory::Effect;
  if (batchTag == War3BatchTag::Lightning)
    return StageCategory::Effect;

  if (g_terrainDepth.load(std::memory_order_relaxed) > 0)
    return StageCategory::Terrain;

  if (!g_stageTouchedThisFrame.load(std::memory_order_relaxed))
    return StageCategory::Unknown;

  if (!valid)
    return StageCategory::Unknown;

  switch (stage) {
  case 0:
    return StageCategory::Skybox;
  case 1:
  case 2:
  case 3:
  case 4:
  case 5:
  case 6:
  case 7:
  case 8:
  case 9:
  case 10:
  case 14:
  case 17:
  case 19:
    return StageCategory::Terrain;
  case 11:
  case 12:
  case 13:
    return StageCategory::WorldObject;
  case 15:
  case 16:
  case 18:
  case 20:
    return StageCategory::Effect;
  case 21:
    return StageCategory::PostProcess;
  default:
    return StageCategory::Unknown;
  }
}

bool War3RenderState::IsTerrainPhase() {
  return GetStageCategory() == StageCategory::Terrain || IsTerrainRendering();
}

bool War3RenderState::IsWorldObjectPhase() {
  return GetStageCategory() == StageCategory::WorldObject;
}

bool War3RenderState::IsEffectPhase() {
  return GetStageCategory() == StageCategory::Effect;
}

bool War3RenderState::IsSkyboxPhase() {
  return GetStageCategory() == StageCategory::Skybox;
}

bool War3RenderState::IsPostProcessPhase() {
  return GetStageCategory() == StageCategory::PostProcess;
}

bool War3RenderState::IsUiPhase() {
  return CurrentLayer() == War3RenderLayer::UI ||
         GetStageCategory() == StageCategory::UI || IsUiBatchStage();
}

void War3RenderState::OnTerrainEnter() {
  g_terrainDepth.fetch_add(1, std::memory_order_relaxed);
}

void War3RenderState::OnTerrainExit() {
  g_terrainDepth.fetch_sub(1, std::memory_order_relaxed);
}

bool War3RenderState::IsTerrainRendering() {
  return g_terrainDepth.load(std::memory_order_relaxed) > 0;
}

void War3RenderState::SetBatchTag(War3BatchTag tag) {
  g_tlsBatchTag = tag;
  g_stateBatchTag.store(tag, std::memory_order_relaxed);
}

War3BatchTag War3RenderState::GetCurrentBatchTag() {
  War3BatchTag tag = g_tlsBatchTag;
  if (tag == War3BatchTag::Unknown) {
    tag = g_stateBatchTag.load(std::memory_order_relaxed);
  }
  return tag;
}

War3BatchTag War3RenderState::GetTlsBatchTag() { return g_tlsBatchTag; }

void War3RenderState::SetTlsBatchHandle(uint32_t handle) {
  g_tlsBatchHandle = handle;
}

uint32_t War3RenderState::GetTlsBatchHandle() { return g_tlsBatchHandle; }

void War3RenderState::SetLastRenderHandle(uint32_t handle) {
  const uint32_t normalized = NormalizeHandle(handle);
  if (!normalized)
    return;
  g_lastRenderHandle.store(normalized, std::memory_order_relaxed);
}

uint32_t War3RenderState::GetLastRenderHandle() {
  return g_lastRenderHandle.load(std::memory_order_relaxed);
}

void War3RenderState::SetTlsDispatchHandle(uint32_t handle) {
  g_tlsDispatchHandle = handle;
}
uint32_t War3RenderState::GetTlsDispatchHandle() { return g_tlsDispatchHandle; }

void War3RenderState::AddOutlineHandle(uint32_t handle) {
  const uint32_t normalized = NormalizeHandle(handle);
  if (!normalized) {
    const uint32_t dbg =
        g_outlineDebugLogCount.fetch_add(1, std::memory_order_relaxed);
    if (dbg < 8) {
      war3dbg::Print("DXVK_Outline: ignore invalid handle=%u\n", handle);
    }
    return;
  }
  std::unique_lock<std::shared_mutex> lock(g_outlineHandleMutex);
  const auto [it, inserted] = g_outlineHandles.insert(normalized);
  const size_t count = g_outlineHandles.size();
  g_outlineHandleCount.store(static_cast<uint32_t>(count),
                             std::memory_order_relaxed);
  g_needsObjectTrackingCached.store(true, std::memory_order_relaxed);

  const uint32_t dbg =
      g_outlineDebugLogCount.fetch_add(1, std::memory_order_relaxed);
  if (dbg < 8) {
    war3dbg::Print(
        "DXVK_Outline: add handle=%u normalized=%u inserted=%d count=%zu\n",
        handle, normalized, inserted ? 1 : 0, count);
  }
}

void War3RenderState::RemoveOutlineHandle(uint32_t handle) {
  const uint32_t normalized = NormalizeHandle(handle);
  if (!normalized)
    return;
  std::unique_lock<std::shared_mutex> lock(g_outlineHandleMutex);
  g_outlineHandles.erase(normalized);
  g_outlineHandleCount.store(static_cast<uint32_t>(g_outlineHandles.size()),
                             std::memory_order_relaxed);
}

void War3RenderState::ClearOutlineHandles() {
  std::unique_lock<std::shared_mutex> lock(g_outlineHandleMutex);
  g_outlineHandles.clear();
  g_outlineHandleCount.store(0, std::memory_order_relaxed);
}

bool War3RenderState::HasOutlineHandles() {
  if (IsOutlineAllObjectsEnabled())
    return true;
  return g_outlineHandleCount.load(std::memory_order_relaxed) > 0;
}

bool War3RenderState::IsOutlineHandle(uint32_t handle) {
  const uint32_t normalized = NormalizeHandle(handle);
  if (!normalized)
    return false;
  if (IsOutlineAllObjectsEnabled())
    return true;
  if (g_outlineHandleCount.load(std::memory_order_relaxed) == 0)
    return false;
  std::shared_lock<std::shared_mutex> lock(g_outlineHandleMutex);
  return g_outlineHandles.find(normalized) != g_outlineHandles.end();
}

uint32_t War3RenderState::GetOutlineHandleCount() {
  return g_outlineHandleCount.load(std::memory_order_relaxed);
}

void War3RenderState::AddBloomHandle(uint32_t handle, float boost) {
  const uint32_t normalized = NormalizeHandle(handle);
  if (!normalized)
    return;
  std::unique_lock<std::shared_mutex> lock(g_bloomHandleMutex);
  g_bloomHandles[normalized] = std::max(0.0f, boost);
  g_bloomHandleCount.store(static_cast<uint32_t>(g_bloomHandles.size()),
                           std::memory_order_relaxed);
  g_needsObjectTrackingCached.store(true, std::memory_order_relaxed);
}

void War3RenderState::RemoveBloomHandle(uint32_t handle) {
  const uint32_t normalized = NormalizeHandle(handle);
  if (!normalized)
    return;
  std::unique_lock<std::shared_mutex> lock(g_bloomHandleMutex);
  g_bloomHandles.erase(normalized);
  g_bloomHandleCount.store(static_cast<uint32_t>(g_bloomHandles.size()),
                           std::memory_order_relaxed);
}

void War3RenderState::ClearBloomHandles() {
  std::unique_lock<std::shared_mutex> lock(g_bloomHandleMutex);
  g_bloomHandles.clear();
  g_bloomHandleCount.store(0, std::memory_order_relaxed);
}

float War3RenderState::GetBloomBoost(uint32_t handle) {
  const uint32_t normalized = NormalizeHandle(handle);
  if (!normalized)
    return 0.0f;
  std::shared_lock<std::shared_mutex> lock(g_bloomHandleMutex);
  auto it = g_bloomHandles.find(normalized);
  if (it == g_bloomHandles.end())
    return 0.0f;
  return it->second;
}

bool War3RenderState::IsBloomHandle(uint32_t handle) {
  const uint32_t normalized = NormalizeHandle(handle);
  if (!normalized)
    return false;
  if (g_bloomHandleCount.load(std::memory_order_relaxed) == 0)
    return false;
  std::shared_lock<std::shared_mutex> lock(g_bloomHandleMutex);
  return g_bloomHandles.find(normalized) != g_bloomHandles.end();
}

bool War3RenderState::HasBloomHandles() {
  return g_bloomHandleCount.load(std::memory_order_relaxed) > 0;
}

uint32_t War3RenderState::GetBloomHandleCount() {
  return g_bloomHandleCount.load(std::memory_order_relaxed);
}

bool War3RenderState::IsTrackedHandle(uint32_t handle) {
  if (IsOutlineHandle(handle))
    return true;
  return IsBloomHandle(handle);
}

bool War3RenderState::NeedsObjectTracking() {
  if (!g_forceObjectTrackingChecked.exchange(true, std::memory_order_relaxed)) {
    // 内部测试版本：不依赖环境变量，直接使用编译期配置。
    g_forceObjectTracking.store(
        dxvk::war3::internal::kForceObjectTrackingEnabled,
        std::memory_order_relaxed);
  }

  return g_needsObjectTrackingCached.load(std::memory_order_relaxed);
}

void War3RenderState::SetShadowSemanticTrackingEnabled(bool enabled) {
  g_needsShadowSemanticTrackingCached.store(enabled, std::memory_order_relaxed);
}

bool War3RenderState::NeedsShadowSemanticTracking() {
  return g_needsShadowSemanticTrackingCached.load(std::memory_order_relaxed);
}

void War3RenderState::SetShadowObjectIdentityTrackingEnabled(bool enabled) {
  g_needsShadowObjectIdentityCached.store(enabled, std::memory_order_relaxed);
  g_needsShadowSemanticTrackingCached.store(
      enabled ||
          g_needsShadowDrawFallbackBridgeCached.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
}

bool War3RenderState::NeedsShadowObjectIdentity() {
  return g_needsShadowObjectIdentityCached.load(std::memory_order_relaxed);
}

void War3RenderState::SetShadowDrawFallbackBridgeEnabled(bool enabled) {
  g_needsShadowDrawFallbackBridgeCached.store(enabled,
                                              std::memory_order_relaxed);
  g_needsShadowSemanticTrackingCached.store(
      enabled || g_needsShadowObjectIdentityCached.load(
                     std::memory_order_relaxed),
      std::memory_order_relaxed);
}

bool War3RenderState::NeedsShadowDrawFallbackBridge() {
  return g_needsShadowDrawFallbackBridgeCached.load(std::memory_order_relaxed);
}

void War3RenderState::SetTlsShadowSemanticState(
    const War3TlsShadowSemanticState &state) {
  g_tlsShadowSemanticState = state;
}

const War3TlsShadowSemanticState &War3RenderState::GetTlsShadowSemanticState() {
  return g_tlsShadowSemanticState;
}

void War3RenderState::ClearTlsShadowSemanticState() {
  g_tlsShadowSemanticState = {};
}

void War3RenderState::SetBatchTagTrackingEnabled(bool enabled) {
  g_batchTagTrackingEnabled.store(enabled, std::memory_order_relaxed);
}

bool War3RenderState::IsBatchTagTrackingEnabled() {
  return g_batchTagTrackingEnabled.load(std::memory_order_relaxed);
}

bool War3RenderState::IsForceObjectTrackingEnabled() {
  if (!g_forceObjectTrackingChecked.exchange(true, std::memory_order_relaxed)) {
    // 内部测试版本：不依赖环境变量，直接使用编译期配置。
    g_forceObjectTracking.store(
        dxvk::war3::internal::kForceObjectTrackingEnabled,
        std::memory_order_relaxed);
  }
  return g_forceObjectTracking.load(std::memory_order_relaxed);
}

void War3RenderState::SnapshotTrackedHandles(
    std::vector<uint32_t> &outHandles) {
  outHandles.clear();

  // 预估容量，减少 push_back 时的扩容
  const uint32_t reserve = GetOutlineHandleCount() + GetBloomHandleCount();
  if (reserve != 0 && outHandles.capacity() < reserve) {
    outHandles.reserve(reserve);
  }

  {
    std::shared_lock<std::shared_mutex> lock(g_outlineHandleMutex);
    outHandles.insert(outHandles.end(), g_outlineHandles.begin(),
                      g_outlineHandles.end());
  }

  {
    std::shared_lock<std::shared_mutex> lock(g_bloomHandleMutex);
    outHandles.reserve(outHandles.size() + g_bloomHandles.size());
    for (const auto &pair : g_bloomHandles) {
      outHandles.push_back(pair.first);
    }
  }

  // 去重（outline/bloom 可能重复）
  std::sort(outHandles.begin(), outHandles.end());
  outHandles.erase(std::unique(outHandles.begin(), outHandles.end()),
                   outHandles.end());
}

bool War3RenderState::IsCurrentBatchWorldObject() {
  const auto tag = GetCurrentBatchTag();
  return tag == War3BatchTag::WorldObjects ||
         tag == War3BatchTag::SelectionOverlay ||
         tag == War3BatchTag::Decorations;
}

bool War3RenderState::IsCurrentBatchUnit() {
  return GetCurrentBatchTag() == War3BatchTag::WorldObjects;
}

void War3RenderState::SetDebugRenderMode(DebugRenderMode mode) {
  g_debugMode.store(mode, std::memory_order_relaxed);
}

War3RenderState::DebugRenderMode War3RenderState::GetDebugRenderMode() {
  return g_debugMode.load(std::memory_order_relaxed);
}

bool War3RenderState::ShouldRenderCurrent(DebugRenderMode mode) {
  if (mode == DebugRenderMode::Normal) {
    return true;
  }

  auto layer = CurrentLayer();
  auto cat = GetStageCategory();
  auto tag = GetCurrentBatchTag();
  const bool stateKnown =
      (cat != StageCategory::Unknown) || (tag != War3BatchTag::Unknown) ||
      IsTerrainRendering() || (layer == War3RenderLayer::UI);

  if (layer == War3RenderLayer::UI) {
    cat = StageCategory::UI;
  }

  switch (mode) {
  case DebugRenderMode::TerrainOnly:
    if (!stateKnown)
      return true;
    return cat == StageCategory::Terrain || IsTerrainRendering();
  case DebugRenderMode::ObjectsOnly:
    if (!stateKnown)
      return true;
    if (layer == War3RenderLayer::UI)
      return false;
    if (tag == War3BatchTag::WorldObjects ||
        tag == War3BatchTag::SelectionOverlay ||
        tag == War3BatchTag::Decorations) {
      return true;
    }
    return cat == StageCategory::WorldObject || cat == StageCategory::Effect;
  case DebugRenderMode::SkyboxOnly:
    if (!stateKnown)
      return true;
    return cat == StageCategory::Skybox;
  case DebugRenderMode::UIOnly:
    if (!stateKnown)
      return true;
    return cat == StageCategory::UI || cat == StageCategory::PostProcess ||
           layer == War3RenderLayer::UI;
  case DebugRenderMode::Normal:
  default:
    return true;
  }
}

bool War3RenderState::ShouldSkipUi() {
  return g_stateSkipUi.load(std::memory_order_relaxed);
}

void War3RenderState::SetSkipUi(bool skip) {
  g_stateSkipUi.store(skip, std::memory_order_relaxed);
}

void War3RenderState::OnFrameStart() {
  g_stageTouchedThisFrame.store(false, std::memory_order_relaxed);
  g_maxStageThisFrame.store(-1, std::memory_order_relaxed);
  g_maxStageCompletedThisFrame.store(-1, std::memory_order_relaxed);
  g_maxMainWorldStageCompletedThisFrame.store(-1, std::memory_order_relaxed);
  g_mainWorldStageActive.store(false, std::memory_order_relaxed);
  g_worldFramePrepareTouchedThisFrame.store(false, std::memory_order_relaxed);
  g_worldFramePrepareCompletedThisFrame.store(false, std::memory_order_relaxed);
  g_worldRenderSceneTouchedThisFrame.store(false, std::memory_order_relaxed);
  g_worldRenderSceneCompletedThisFrame.store(false, std::memory_order_relaxed);
  g_worldRenderSceneActive.store(false, std::memory_order_relaxed);
  g_stateLayer.store(War3RenderLayer::Unknown, std::memory_order_relaxed);
  g_uiDispatchTouchedThisFrame.store(false, std::memory_order_relaxed);
  g_dispatcherTouchedThisFrame.store(false, std::memory_order_relaxed);
  g_dispatcherStage.store(-1, std::memory_order_relaxed);
  g_uiBatchTouchedThisFrame.store(false, std::memory_order_relaxed);
  g_tlsDispatcherStage = -1;
  g_tlsShadowSemanticState = {};
  g_needsObjectTrackingCached.store(ComputeNeedsObjectTracking(),
                                    std::memory_order_relaxed);
  g_needsShadowSemanticTrackingCached.store(false, std::memory_order_relaxed);
  g_needsShadowObjectIdentityCached.store(false, std::memory_order_relaxed);
  g_needsShadowDrawFallbackBridgeCached.store(false,
                                              std::memory_order_relaxed);

  // [调试] 可通过环境变量强制开启描边参数（默认不覆盖脚本配置）
  if (IsOutlineForceEnabled()) {
    auto settings = dxvk::war3::GetMutableSettings();
    if (settings) {
      settings->occludedOutline.enabled = true;
      settings->occludedOutline.showVisible = true;
      settings->occludedOutline.showOccluded = true;
      settings->occludedOutline.widthPx = 2.0f;
    }
  }
}

bool War3RenderState::HasWorldStageThisFrame() {
  return g_stageTouchedThisFrame.load(std::memory_order_relaxed);
}

bool War3RenderState::HasWorldFramePrepareThisFrame() {
  return g_worldFramePrepareTouchedThisFrame.load(std::memory_order_relaxed);
}

bool War3RenderState::HasCompletedWorldFramePrepareThisFrame() {
  return g_worldFramePrepareCompletedThisFrame.load(std::memory_order_relaxed);
}

bool War3RenderState::HasWorldRenderSceneThisFrame() {
  return g_worldRenderSceneTouchedThisFrame.load(std::memory_order_relaxed);
}

bool War3RenderState::HasCompletedWorldRenderSceneThisFrame() {
  return g_worldRenderSceneCompletedThisFrame.load(std::memory_order_relaxed);
}

bool War3RenderState::IsWorldRenderSceneActive() {
  return g_worldRenderSceneActive.load(std::memory_order_relaxed);
}

bool War3RenderState::IsMainWorldStageActive() {
  return g_mainWorldStageActive.load(std::memory_order_relaxed);
}

bool War3RenderState::HasReachedStageThisFrame(int stage) {
  return g_maxStageThisFrame.load(std::memory_order_relaxed) >= stage;
}

bool War3RenderState::HasCompletedStageThisFrame(int stage) {
  return g_maxStageCompletedThisFrame.load(std::memory_order_relaxed) >= stage;
}

bool War3RenderState::HasMainWorldCompletedStageThisFrame(int stage) {
  return g_maxMainWorldStageCompletedThisFrame.load(
             std::memory_order_relaxed) >= stage;
}

void War3RenderState::OnWorldFramePrepareEnter() {
  g_worldFramePrepareTouchedThisFrame.store(true, std::memory_order_relaxed);
  g_worldFramePrepareCompletedThisFrame.store(false, std::memory_order_relaxed);
}

void War3RenderState::OnWorldFramePrepareExit() {
  g_worldFramePrepareTouchedThisFrame.store(true, std::memory_order_relaxed);
  g_worldFramePrepareCompletedThisFrame.store(true, std::memory_order_relaxed);
}

void War3RenderState::OnWorldRenderSceneEnter() {
  g_worldRenderSceneTouchedThisFrame.store(true, std::memory_order_relaxed);
  g_worldRenderSceneCompletedThisFrame.store(false, std::memory_order_relaxed);
  g_worldRenderSceneActive.store(true, std::memory_order_relaxed);
}

void War3RenderState::OnWorldRenderSceneExit() {
  g_worldRenderSceneTouchedThisFrame.store(true, std::memory_order_relaxed);
  g_worldRenderSceneCompletedThisFrame.store(true, std::memory_order_relaxed);
  g_worldRenderSceneActive.store(false, std::memory_order_relaxed);
}

void War3RenderState::OnStageExit(int stage) {
  int prevMax = g_maxStageCompletedThisFrame.load(std::memory_order_relaxed);
  while (stage > prevMax && !g_maxStageCompletedThisFrame.compare_exchange_weak(
                                prevMax, stage, std::memory_order_relaxed,
                                std::memory_order_relaxed)) {
    // prevMax updated by compare_exchange_weak
  }

  if (stage == 21) {
    // [DISABLED] ENTER LOG
  }
}

void War3RenderState::OnMainWorldStageEnter(int stage) {
  if (stage >= 0)
    g_mainWorldStageActive.store(true, std::memory_order_relaxed);
}

void War3RenderState::OnMainWorldStageExit(int stage) {
  g_mainWorldStageActive.store(false, std::memory_order_relaxed);
  int prevMax =
      g_maxMainWorldStageCompletedThisFrame.load(std::memory_order_relaxed);
  while (stage > prevMax &&
         !g_maxMainWorldStageCompletedThisFrame.compare_exchange_weak(
             prevMax, stage, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
    // prevMax updated by compare_exchange_weak
  }
}

void War3RenderState::SetCurrentViewport(uint32_t x,
                                         uint32_t y,
                                         uint32_t width,
                                         uint32_t height) {
  g_currentViewportX.store(x, std::memory_order_relaxed);
  g_currentViewportY.store(y, std::memory_order_relaxed);
  g_currentViewportWidth.store(width, std::memory_order_relaxed);
  g_currentViewportHeight.store(height, std::memory_order_relaxed);
  g_currentViewportValid.store(width != 0u && height != 0u,
                               std::memory_order_relaxed);
  g_currentViewportSerial.fetch_add(1u, std::memory_order_relaxed);
}

War3ViewportSnapshot War3RenderState::GetCurrentViewportSnapshot() {
  War3ViewportSnapshot snapshot = {};
  snapshot.serial = g_currentViewportSerial.load(std::memory_order_relaxed);
  snapshot.valid = g_currentViewportValid.load(std::memory_order_relaxed);
  snapshot.x = g_currentViewportX.load(std::memory_order_relaxed);
  snapshot.y = g_currentViewportY.load(std::memory_order_relaxed);
  snapshot.width = g_currentViewportWidth.load(std::memory_order_relaxed);
  snapshot.height = g_currentViewportHeight.load(std::memory_order_relaxed);
  return snapshot;
}

void War3RenderState::SetGameTime(float time) {
  g_gameTime.store(time, std::memory_order_relaxed);
}

float War3RenderState::GetGameTime() {
  return g_gameTime.load(std::memory_order_relaxed);
}

void War3RenderState::OnUiDispatch() {
  const bool wasSet =
      g_uiDispatchTouchedThisFrame.exchange(true, std::memory_order_relaxed);
  if (!wasSet) {
    // [DISABLED] ENTER LOG
  }
}

bool War3RenderState::HasUiDispatchThisFrame() {
  return g_uiDispatchTouchedThisFrame.load(std::memory_order_relaxed);
}

bool War3RenderState::IsOutlineDebugAllObjectsEnabled() {
  return IsOutlineAllObjectsEnabled();
}

uint32_t War3RenderState::GetOutlineDebugHandle() { return kOutlineAllHandle; }

void War3RenderState::SetOutlineDebugAllObjectsEnabled(bool enabled) {
  g_outlineAllObjects.store(enabled, std::memory_order_relaxed);
  g_outlineAllChecked.store(true, std::memory_order_relaxed);
  if (enabled) {
    g_needsObjectTrackingCached.store(true, std::memory_order_relaxed);
  } else {
    g_needsObjectTrackingCached.store(ComputeNeedsObjectTracking(),
                                      std::memory_order_relaxed);
  }
}

void War3RenderState::SetOutlineForceEnabled(bool enabled) {
  g_outlineForceEnabled.store(enabled, std::memory_order_relaxed);
  g_outlineForceChecked.store(true, std::memory_order_relaxed);
}

bool War3RenderState::IsOutlineForceEnabledForTest() {
  return IsOutlineForceEnabled();
}

void War3RenderState::SetNativeShadowMode(uint32_t mode) {
  if (mode > 2u)
    mode = 2u;
  const uint32_t prev =
      g_nativeShadowMode.exchange(mode, std::memory_order_relaxed);
  if (prev != mode) {
    static std::atomic<uint32_t> s_nativeShadowLogCount{0};
    if (s_nativeShadowLogCount.fetch_add(1, std::memory_order_relaxed) < 16) {
      WAR3_RENDER_LOG("DXVK War3Hook: NativeShadowMode %u -> %u\n", prev, mode);
    }
  }
}

uint32_t War3RenderState::GetNativeShadowMode() {
  return g_nativeShadowMode.load(std::memory_order_relaxed);
}

void War3RenderState::ResetRuntimeState() {
  g_stateStage.store(-1, std::memory_order_relaxed);
  g_stateValid.store(false, std::memory_order_relaxed);
  g_stageTouchedThisFrame.store(false, std::memory_order_relaxed);
  g_maxStageThisFrame.store(-1, std::memory_order_relaxed);
  g_maxStageCompletedThisFrame.store(-1, std::memory_order_relaxed);
  g_maxMainWorldStageCompletedThisFrame.store(-1, std::memory_order_relaxed);
  g_worldFramePrepareTouchedThisFrame.store(false, std::memory_order_relaxed);
  g_worldFramePrepareCompletedThisFrame.store(false, std::memory_order_relaxed);
  g_worldRenderSceneTouchedThisFrame.store(false, std::memory_order_relaxed);
  g_worldRenderSceneCompletedThisFrame.store(false, std::memory_order_relaxed);
  g_worldRenderSceneActive.store(false, std::memory_order_relaxed);
  g_uiDispatchTouchedThisFrame.store(false, std::memory_order_relaxed);
  g_dispatcherStage.store(-1, std::memory_order_relaxed);
  g_dispatcherTouchedThisFrame.store(false, std::memory_order_relaxed);
  g_uiBatchTouchedThisFrame.store(false, std::memory_order_relaxed);
  g_stateLayer.store(War3RenderLayer::Unknown, std::memory_order_relaxed);
  g_stateSkipUi.store(false, std::memory_order_relaxed);
  g_stateBatchTag.store(War3BatchTag::Unknown, std::memory_order_relaxed);
  g_tlsBatchTag = War3BatchTag::Unknown;
  g_tlsBatchHandle = 0;
  g_tlsDispatcherStage = -1;
  g_tlsShadowSemanticState = {};
  g_lastRenderHandle.store(0, std::memory_order_relaxed);
  g_terrainDepth.store(0, std::memory_order_relaxed);
  g_debugMode.store(DebugRenderMode::Normal, std::memory_order_relaxed);
  g_needsObjectTrackingCached.store(false, std::memory_order_relaxed);
  g_needsShadowSemanticTrackingCached.store(false, std::memory_order_relaxed);
  g_needsShadowObjectIdentityCached.store(false, std::memory_order_relaxed);
  g_needsShadowDrawFallbackBridgeCached.store(false,
                                              std::memory_order_relaxed);
  g_batchTagTrackingEnabled.store(true, std::memory_order_relaxed);
  g_nativeShadowMode.store(::dxvk::war3::internal::kNativeShadowDefaultMode,
                           std::memory_order_relaxed);

  ClearOutlineHandles();
  ClearBloomHandles();
}

} // namespace dxvk
