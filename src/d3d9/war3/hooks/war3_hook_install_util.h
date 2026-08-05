#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace dxvk::war3::hooks {

// Stable, explicitly assigned identifiers for the descriptor catalog. This is
// intentionally a partial pilot rather than an ordinal of every installed
// hook: adding another domain must allocate a new fixed range and must not
// renumber existing entries.
enum class War3HookId : uint32_t {
  Invalid = 0u,

  RenderPerfWorldPrepareCameraBuildFrustum = 0x52500001u,
  RenderPerfWorldPrepareTerrainShadowFlush = 0x52500002u,
  RenderPerfWorldPrepareTerrainExtraPass = 0x52500003u,
  RenderPerfWorldPrepareShadowProjectorFlush = 0x52500004u,
  RenderPerfWorldPrepareTargetIndicatorRingAdvance = 0x52500005u,
  RenderPerfWorldPrepareCinematicFilterTimeAdvance = 0x52500006u,
  RenderPerfWorldPrepareRuntimeFlagClockAdvance3B8760 = 0x52500007u,
  RenderPerfWorldPrepareFlushDeferredSelectionObjects = 0x52500008u,
  RenderPerfWorldPrepareGlobalRenderCallbackPass = 0x52500009u,
  RenderPerfWorldPrepareRenderWaypointIndicators = 0x5250000au,
  RenderPerfWorldPrepareUnsafeImplicitEdi368E90 = 0x5250000bu,
  RenderPerfWorldPrepareFrameUpdateGate = 0x5250000cu,
  RenderPerfWorldPrepareGameUiFrameSync = 0x5250000du,
  RenderPerfWorldPrepareUpdateIndicatorAnchor = 0x5250000eu,
  RenderPerfWorldPrepareCameraAdvance = 0x5250000fu,
  RenderPerfWorldPrepareCameraPrepareConstants = 0x52500010u,
  RenderPerfWorldPrepareViewProjPrepare = 0x52500011u,
  RenderPerfWorldPrepareSceneQueryFlushSync = 0x52500012u,
  RenderPerfWorldPrepareFixedPointRemap = 0x52500013u,
  RenderPerfWorldPreparePostVisibilityGlobalAdvanceA = 0x52500014u,
  RenderPerfWorldPreparePostVisibilityFrameAnchorUpdate = 0x52500015u,
  RenderPerfWorldPreparePostVisibilityFrameAnchorVisibilityQuery =
      0x52500016u,
  RenderPerfWorldPreparePostVisibilityGlobalAdvanceB = 0x52500017u,
  RenderPerfWorldPrepareVisibilityTailAdvanceA = 0x52500018u,
  RenderPerfWorldPrepareVisibilityTailAdvanceB = 0x52500019u,
  RenderPerfRenderQueueStageUpdate = 0x5250001au,
  RenderPerfRenderQueueFlushTransparent = 0x5250001bu,
  RenderPerfTransparentDispatchType0 = 0x5250001cu,
  RenderPerfTransparentDispatchType1 = 0x5250001du,
  RenderPerfTransparentDispatchType2 = 0x5250001eu,
  RenderPerfTransparentDispatchType3 = 0x5250001fu,
  RenderPerfTransparentDispatchType4 = 0x52500020u,
};

enum class War3HookKind : uint8_t {
  MinHookDetour = 0,
  QueuedGroup,
  D3D9Frontend,
  WarVKCallback,
  PatchOnly,
};

enum class War3HookCatalogStatus : uint8_t {
  NotEvaluated = 0,
  Installed,
  DisabledByCompileConfig,
  DisabledByEnvironment,
  SkippedUnsafeABI,
  AddressUnavailable,
  InstallFailed,
};

struct War3HookActivationGate {
  const char* compileConfig = "";
  const char* environment = "";
  uint32_t minPerfLevel = 0u;
  const char* runtimeModules = "";
  const char* owner = "";
  const char* displayExpression = "";
};

struct War3HookDescriptor {
  War3HookId id = War3HookId::Invalid;
  const char* domain = "Unknown";
  const char* hookName = "UnnamedHook";
  War3HookKind kind = War3HookKind::MinHookDetour;
  uintptr_t targetRva = 0u;
  const char* timingRoot = "";
  const char* nativeMode = "unresolved";
  const char* customPhase = "Unresolved";
  const char* safetyClass = "Unclassified";
  War3HookActivationGate activationGate = {};
};

struct War3HookRuntimeState {
  War3HookId id = War3HookId::Invalid;
  War3HookCatalogStatus status = War3HookCatalogStatus::NotEvaluated;
  War3HookCatalogStatus lastAttemptState =
      War3HookCatalogStatus::NotEvaluated;
  uintptr_t target = 0u;
  uintptr_t detour = 0u;
  uintptr_t trampoline = 0u;
  int32_t minHookStatus = 0;
  uint32_t attemptCount = 0u;
  bool installed = false;
  std::string reason;
};

// Owning, export-safe merge of a static descriptor and its mutable runtime
// state. Catalog registration and state writes happen only at installation
// time; snapshotting happens only on the report export worker.
struct War3HookCatalogRecord {
  uint32_t id = 0u;
  std::string domain;
  std::string hookName;
  std::string kind;
  uintptr_t targetRva = 0u;
  std::string timingRoot;
  std::string nativeMode;
  std::string customPhase;
  std::string safetyClass;
  std::string compileConfig;
  std::string environment;
  uint32_t minPerfLevel = 0u;
  std::string runtimeModules;
  std::string owner;
  std::string displayExpression;
  std::string status;
  std::string lastAttemptState;
  uintptr_t target = 0u;
  uintptr_t detour = 0u;
  uintptr_t trampoline = 0u;
  int32_t minHookStatus = 0;
  uint32_t attemptCount = 0u;
  bool installed = false;
  std::string reason;
};

struct War3HookInstallRecord {
  std::string domain;
  std::string hookName;
  std::string state;
  uintptr_t target = 0u;
  uintptr_t detour = 0u;
  uintptr_t trampoline = 0u;
  bool installed = false;
};

// 所有 MinHook 安装路径共用的只读诊断注册表。写入只发生在安装/回滚期，
// 性能报告导出时复制快照；绝不进入 per-draw 热路径。
void RecordHookInstallState(const char* domain, const char* hookName,
                            LPVOID target, LPVOID detour,
                            LPVOID trampoline, const char* state,
                            bool installed);
std::vector<War3HookInstallRecord> SnapshotHookInstallRecords();

// Explicit domain-span registration avoids static constructors, linker
// sections, and PE-specific discovery. Re-registering the same stable ID
// updates descriptor metadata without discarding its runtime state.
void RegisterHookDescriptors(const War3HookDescriptor* descriptors,
                             size_t descriptorCount);
void RecordHookCatalogState(
    War3HookId id, War3HookCatalogStatus state, LPVOID target = nullptr,
    LPVOID detour = nullptr, LPVOID trampoline = nullptr,
    int32_t minHookStatus = 0, const char* reason = nullptr,
    bool installAttempt = false);
std::vector<War3HookCatalogRecord> SnapshotHookCatalogRecords();

const char* War3HookCatalogStatusName(War3HookCatalogStatus state);
const char* War3HookKindName(War3HookKind kind);

/**
 * @brief 统一安装并启用单个 MinHook 钩子。
 *
 * 行为约定：
 * 1. 校验 `target/detour/original` 非空；
 * 2. 可选调用 `MH_Initialize`（用于独立路径兜底）；
 * 3. 执行 `MH_CreateHook + MH_EnableHook`；
 * 4. 统一输出失败日志，避免各域日志格式漂移。
 *
 * @param target 目标函数地址。
 * @param detour Hook 函数地址。
 * @param original 原函数/trampoline 存储地址。
 * @param domain 域名（如 Render/UI/Shadow）。
 * @param hookName Hook 点名（用于日志）。
 * @param ensureInitialized 是否确保 MinHook 已初始化。
 * @param logSuccess 是否输出成功日志。
 * @return true 安装成功（含 already-created/enabled）；false 安装失败。
 */
bool InstallMinHook(LPVOID target, LPVOID detour, LPVOID* original,
                    const char* domain, const char* hookName,
                    bool ensureInitialized = false, bool logSuccess = false);

// Descriptor-aware overload used by catalog-migrated domains. The legacy
// signature above remains source- and behavior-compatible.
bool InstallMinHook(War3HookId hookId, LPVOID target, LPVOID detour,
                    LPVOID* original, const char* domain,
                    const char* hookName, bool ensureInitialized = false,
                    bool logSuccess = false);

}  // namespace dxvk::war3::hooks

