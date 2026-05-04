// war3_render_state.h - Render state tracking shared across hooks and render
// code

#pragma once

#include <cstdint>
#include <vector>

namespace dxvk {

namespace war3::render {
enum class ObjectKind : uint8_t;
struct RenderObjectInfo;
}

// Render layer marker
enum class War3RenderLayer : uint8_t {
  Unknown = 0,
  World = 1,
  UI = 2,
};

// Batch tag classification
// 根据用户实测结果建立的 Stage -> Tag 映射:
// S0=天空盒, S1=地形, S2=阴影/迷雾, S5=地面效果(瘟疫), S6=天气(下雪),
// S7=选择圈, S9=地面效果(血迹/脚印), S10=装饰物, S11=单位渲染,
// S12=范围指示器目标(绿色), S14=水, S19=建筑地板贴画, S21=范围指示器
enum class War3BatchTag : int32_t {
  Unknown = -1,
  Terrain = 0,          // S1: 地形
  WorldObjects = 1,     // S11: 单位渲染
  SelectionOverlay = 2, // S7: 单位选择圈
  Decorations = 3,      // S10: 装饰物渲染
  UI = 4,
  Skybox = 5,               // S0: 天空盒
  GroundEffect = 6,         // S5/S9: 地面效果(瘟疫/血迹/脚印)
  Weather = 7,              // S6: 天气效果(下雪)
  RangeIndicatorTarget = 8, // S12: 被范围指示器框选的对象(绿色)
  Water = 9,                // S14: 水面渲染
  BuildingFloorDecal = 10,  // S19: 建筑地板贴画
  RangeIndicator = 11,      // S21: 对象范围指示器渲染
};

// Shadow 语义桥接：只用于在 draw 热路径中保留稳定对象上下文，
// 不直接改变当前阴影重放/预算逻辑。
struct War3TlsShadowSemanticState {
  void *renderablePart = nullptr;
  void *sceneNode = nullptr;
  void *worldObjectEntry = nullptr;
  const war3::render::RenderObjectInfo *object = nullptr;
  uint32_t jHandle = 0;
  uint32_t rawcode = 0;
  war3::render::ObjectKind objectKind =
      static_cast<war3::render::ObjectKind>(0);
  War3BatchTag tag = War3BatchTag::Unknown;
  int stage = -1;

  bool HasAnyContext() const {
    return renderablePart != nullptr || sceneNode != nullptr ||
           worldObjectEntry != nullptr || object != nullptr || jHandle != 0u ||
           rawcode != 0u || static_cast<uint32_t>(objectKind) != 0u ||
           tag != War3BatchTag::Unknown || stage >= 0;
  }
};

// Lightweight render state storage
class War3RenderState {
public:
  enum class StageCategory : uint8_t {
    Unknown = 0,
    Skybox,
    Terrain,
    WorldObject,
    Effect,
    PostProcess,
    UI,
  };

  enum class DebugRenderMode : uint8_t {
    Normal = 0,
    TerrainOnly,
    ObjectsOnly,
    SkyboxOnly,
    UIOnly,
  };

  static void SetStage(int stage);
  static int GetStage();

  static void SetDispatcherStage(int stage);
  static int GetDispatcherStage();
  static bool HasUiBatchStageThisFrame();
  static bool IsUiBatchStage();

  static void PushUiLayer();
  static War3RenderLayer PopLayer(War3RenderLayer prev);
  static War3RenderLayer CurrentLayer();

  static StageCategory GetStageCategory();
  static bool IsTerrainPhase();
  static bool IsWorldObjectPhase();
  static bool IsEffectPhase();
  static bool IsSkyboxPhase();
  static bool IsPostProcessPhase();
  static bool IsUiPhase();

  static void SetDebugRenderMode(DebugRenderMode mode);
  static DebugRenderMode GetDebugRenderMode();
  static bool ShouldRenderCurrent(DebugRenderMode mode);

  static void OnTerrainEnter();
  static void OnTerrainExit();
  static bool IsTerrainRendering();

  static void SetBatchTag(War3BatchTag tag);
  static War3BatchTag GetCurrentBatchTag();
  static War3BatchTag GetTlsBatchTag();
  static uint32_t GetTlsBatchHandle();
  static void SetTlsBatchHandle(uint32_t handle);
  // 最近一次渲染捕获的对象句柄（完整 jHandle）
  static void SetLastRenderHandle(uint32_t handle);
  static uint32_t GetLastRenderHandle();

  // [DISPATCH BRIDGE]
  static void SetTlsDispatchHandle(uint32_t handle);
  static uint32_t GetTlsDispatchHandle();

  static bool IsCurrentBatchWorldObject();
  static bool IsCurrentBatchUnit();

  static void AddOutlineHandle(uint32_t handle);
  static void RemoveOutlineHandle(uint32_t handle);
  static void ClearOutlineHandles();
  static bool HasOutlineHandles();
  static bool IsOutlineHandle(uint32_t handle);
  static uint32_t GetOutlineHandleCount();

  static void AddBloomHandle(uint32_t handle, float boost);
  static void RemoveBloomHandle(uint32_t handle);
  static void ClearBloomHandles();
  static float GetBloomBoost(uint32_t handle);
  static bool IsBloomHandle(uint32_t handle);
  static bool HasBloomHandles();
  static uint32_t GetBloomHandleCount();
  static bool IsTrackedHandle(uint32_t handle);
  static bool NeedsObjectTracking();
  static void SetShadowSemanticTrackingEnabled(bool enabled);
  static bool NeedsShadowSemanticTracking();
  static void SetShadowObjectIdentityTrackingEnabled(bool enabled);
  static bool NeedsShadowObjectIdentity();
  static void SetShadowDrawFallbackBridgeEnabled(bool enabled);
  static bool NeedsShadowDrawFallbackBridge();
  static void SetTlsShadowSemanticState(
      const War3TlsShadowSemanticState &state);
  static const War3TlsShadowSemanticState &GetTlsShadowSemanticState();
  static void ClearTlsShadowSemanticState();
  // 是否需要追踪 RenderQueue 标签/阶段（用于 BeforeUi/Shadow/调试/Probe）
  static void SetBatchTagTrackingEnabled(bool enabled);
  static bool IsBatchTagTrackingEnabled();
  // 是否通过环境变量强制开启对象追踪（DXVK_WAR3_FORCE_OBJECT_TRACKING=1）
  static bool IsForceObjectTrackingEnabled();
  // 获取"当前需要追踪"的句柄快照（outline/bloom），用于在热路径避免频繁加锁查表。
  // 注意：返回的 handle 统一为完整 jHandle（包含 0x100000）。
  static void SnapshotTrackedHandles(std::vector<uint32_t> &outHandles);

  static bool ShouldSkipUi();
  static void SetSkipUi(bool skip);

  static bool HasWorldStageThisFrame();
  static bool HasWorldFramePrepareThisFrame();
  static bool HasCompletedWorldFramePrepareThisFrame();
  static bool HasWorldRenderSceneThisFrame();
  static bool HasCompletedWorldRenderSceneThisFrame();
  static bool IsWorldRenderSceneActive();
  static bool HasReachedStageThisFrame(int stage);
  static bool HasCompletedStageThisFrame(int stage);
  static bool HasMainWorldCompletedStageThisFrame(int stage);
  static void OnFrameStart();
  static void OnWorldFramePrepareEnter();
  static void OnWorldFramePrepareExit();
  static void OnWorldRenderSceneEnter();
  static void OnWorldRenderSceneExit();

  static void OnUiDispatch();
  static bool HasUiDispatchThisFrame();

  static void OnStageExit(int stage);
  static void OnMainWorldStageExit(int stage);

  static void SetGameTime(float time);
  static float GetGameTime();

  // Debug switches for outline flow (temporary, for stability testing).
  static bool IsOutlineDebugAllObjectsEnabled();
  static uint32_t GetOutlineDebugHandle();
  // 调试：强制开启/关闭"描边全部对象"
  static void SetOutlineDebugAllObjectsEnabled(bool enabled);
  // 调试：强制启用描边设置（与脚本设置解耦）
  static void SetOutlineForceEnabled(bool enabled);

  // 原生阴影模式：0=默认(完整) 1=仅保留雾/边界 2=完全禁用
  static void SetNativeShadowMode(uint32_t mode);
  static uint32_t GetNativeShadowMode();

  // Reset internal state for map exit.
  static void ResetRuntimeState();
};

} // namespace dxvk
