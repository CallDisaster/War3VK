#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

namespace dxvk::war3::runtime {

enum class War3RuntimeProfile : uint32_t {
  FullDefault = 0,
  DxvkOnly,
  HooksMinimal,
  HooksDefault,
  RenderBase,
  ShadowCaptureOnly,
  ShadowFull,
  FullAnalysis,
  FullPerfExperimental,
};

enum class War3RuntimeModule : uint32_t {
  HookLifecycle = 0,
  HookUi,
  HookJass,
  HookRender,
  Diag,
  RenderQueue,
  ShadowCapture,
  ShadowMap,
  ShadowReceiver,
  ShadowTaa,
  PostFx,
  Ssao,
  Aa,
  SemanticData,
  Count,
};

struct War3RuntimeConfig {
  War3RuntimeProfile profile = War3RuntimeProfile::FullDefault;
  uint32_t defaultDisabledMask = 0u;
  uint32_t explicitDisabledMask = 0u;
  uint32_t effectiveDisabledMask = 0u;
  bool legacyFpsUnlockOnly = false;
};

const War3RuntimeConfig& GetWar3RuntimeConfig();
War3RuntimeProfile GetWar3RuntimeProfile();
const char* GetWar3RuntimeProfileName(War3RuntimeProfile profile);
const char* GetWar3RuntimeProfileName();

const char* GetWar3RuntimeModuleName(War3RuntimeModule module);
bool IsWar3RuntimeModuleEnabled(War3RuntimeModule module);
bool IsWar3RuntimeModuleDisabled(War3RuntimeModule module);
bool IsAnyWar3RuntimeModuleEnabled(
    std::initializer_list<War3RuntimeModule> modules);

bool IsWar3RuntimeProfile(War3RuntimeProfile profile);
bool UseDxvkOnlyCompatibilityMode();
bool IsStandaloneShadowCaptureProfile();

std::string GetWar3RuntimeDisabledModulesCsv();
std::string GetWar3RuntimeEnabledModulesCsv();

} // namespace dxvk::war3::runtime
