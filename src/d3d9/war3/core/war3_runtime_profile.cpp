#include "war3_runtime_profile.h"

#include "war3_internal_test_config.h"

#include "../../util/util_env.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <string_view>

namespace dxvk::war3::runtime {

namespace {

constexpr uint32_t ModuleBit(War3RuntimeModule module) {
  return 1u << static_cast<uint32_t>(module);
}

constexpr std::array<const char*, static_cast<size_t>(War3RuntimeModule::Count)>
    kModuleNames = {
        "hook.lifecycle", "hook.ui",      "hook.jass",  "hook.render",
        "diag",           "render.queue", "shadow.capture",
        "shadow.map",     "shadow.receiver", "shadow.taa",
        "postfx",         "ssao",         "aa",
        "semantic.data",
    };

std::string ToLowerAscii(std::string text) {
  for (char& ch : text)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

std::string TrimAscii(std::string text) {
  auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!text.empty() && isSpace(static_cast<unsigned char>(text.front())))
    text.erase(text.begin());
  while (!text.empty() && isSpace(static_cast<unsigned char>(text.back())))
    text.pop_back();
  return text;
}

War3RuntimeProfile ParseProfile(std::string text) {
  text = ToLowerAscii(TrimAscii(std::move(text)));
  if (text == "dxvk_only")
    return War3RuntimeProfile::DxvkOnly;
  if (text == "hooks_minimal")
    return War3RuntimeProfile::HooksMinimal;
  if (text == "hooks_default")
    return War3RuntimeProfile::HooksDefault;
  if (text == "render_base")
    return War3RuntimeProfile::RenderBase;
  if (text == "shadow_capture_only")
    return War3RuntimeProfile::ShadowCaptureOnly;
  if (text == "shadow_full")
    return War3RuntimeProfile::ShadowFull;
  if (text == "full_analysis")
    return War3RuntimeProfile::FullAnalysis;
  if (text == "full_perf_experimental")
    return War3RuntimeProfile::FullPerfExperimental;
  return War3RuntimeProfile::FullDefault;
}

uint32_t DefaultDisabledMaskForProfile(War3RuntimeProfile profile) {
  switch (profile) {
  case War3RuntimeProfile::DxvkOnly:
    return 0xFFFFFFFFu;
  case War3RuntimeProfile::HooksMinimal:
    return ModuleBit(War3RuntimeModule::HookUi) |
           ModuleBit(War3RuntimeModule::HookJass) |
           ModuleBit(War3RuntimeModule::HookRender) |
           ModuleBit(War3RuntimeModule::Diag) |
           ModuleBit(War3RuntimeModule::RenderQueue) |
           ModuleBit(War3RuntimeModule::ShadowCapture) |
           ModuleBit(War3RuntimeModule::ShadowMap) |
           ModuleBit(War3RuntimeModule::ShadowReceiver) |
           ModuleBit(War3RuntimeModule::ShadowTaa) |
           ModuleBit(War3RuntimeModule::PostFx) |
           ModuleBit(War3RuntimeModule::Ssao) |
           ModuleBit(War3RuntimeModule::Aa) |
           ModuleBit(War3RuntimeModule::SemanticData);
  case War3RuntimeProfile::HooksDefault:
    return ModuleBit(War3RuntimeModule::RenderQueue) |
           ModuleBit(War3RuntimeModule::ShadowCapture) |
           ModuleBit(War3RuntimeModule::ShadowMap) |
           ModuleBit(War3RuntimeModule::ShadowReceiver) |
           ModuleBit(War3RuntimeModule::ShadowTaa) |
           ModuleBit(War3RuntimeModule::PostFx) |
           ModuleBit(War3RuntimeModule::Ssao) |
           ModuleBit(War3RuntimeModule::Aa) |
           ModuleBit(War3RuntimeModule::SemanticData);
  case War3RuntimeProfile::RenderBase:
    return ModuleBit(War3RuntimeModule::ShadowCapture) |
           ModuleBit(War3RuntimeModule::ShadowMap) |
           ModuleBit(War3RuntimeModule::ShadowReceiver) |
           ModuleBit(War3RuntimeModule::ShadowTaa) |
           ModuleBit(War3RuntimeModule::PostFx) |
           ModuleBit(War3RuntimeModule::Ssao) |
           ModuleBit(War3RuntimeModule::Aa) |
           ModuleBit(War3RuntimeModule::SemanticData);
  case War3RuntimeProfile::ShadowCaptureOnly:
    return ModuleBit(War3RuntimeModule::ShadowMap) |
           ModuleBit(War3RuntimeModule::ShadowReceiver) |
           ModuleBit(War3RuntimeModule::ShadowTaa) |
           ModuleBit(War3RuntimeModule::PostFx) |
           ModuleBit(War3RuntimeModule::Ssao) |
           ModuleBit(War3RuntimeModule::Aa) |
           ModuleBit(War3RuntimeModule::SemanticData);
  case War3RuntimeProfile::ShadowFull:
    return ModuleBit(War3RuntimeModule::PostFx) |
           ModuleBit(War3RuntimeModule::Ssao) |
           ModuleBit(War3RuntimeModule::Aa) |
           ModuleBit(War3RuntimeModule::SemanticData);
  case War3RuntimeProfile::FullAnalysis:
  case War3RuntimeProfile::FullPerfExperimental:
  case War3RuntimeProfile::FullDefault:
  default:
    return 0u;
  }
}

constexpr uint32_t RenderInterferenceMask() {
  return ModuleBit(War3RuntimeModule::HookRender) |
         ModuleBit(War3RuntimeModule::RenderQueue) |
         ModuleBit(War3RuntimeModule::ShadowCapture) |
         ModuleBit(War3RuntimeModule::ShadowMap) |
         ModuleBit(War3RuntimeModule::ShadowReceiver) |
         ModuleBit(War3RuntimeModule::ShadowTaa) |
         ModuleBit(War3RuntimeModule::PostFx) |
         ModuleBit(War3RuntimeModule::Ssao) |
         ModuleBit(War3RuntimeModule::Aa);
}

constexpr uint32_t ShadowStackMask() {
  return ModuleBit(War3RuntimeModule::ShadowCapture) |
         ModuleBit(War3RuntimeModule::ShadowMap) |
         ModuleBit(War3RuntimeModule::ShadowReceiver) |
         ModuleBit(War3RuntimeModule::ShadowTaa);
}

constexpr uint32_t PostFxStackMask() {
  return ModuleBit(War3RuntimeModule::PostFx) |
         ModuleBit(War3RuntimeModule::Ssao) |
         ModuleBit(War3RuntimeModule::Aa);
}

constexpr uint32_t CompileTimeDisabledMask() {
  uint32_t mask = 0u;

  if constexpr (internal::kWar3RuntimeConfigDxvkOnlyBaseline)
    mask |= 0xFFFFFFFFu;
  if constexpr (internal::kWar3RuntimeConfigDisableRenderInterference)
    mask |= RenderInterferenceMask();
  if constexpr (internal::kWar3RuntimeConfigDisableShadowStack)
    mask |= ShadowStackMask();
  if constexpr (internal::kWar3RuntimeConfigDisablePostFxStack)
    mask |= PostFxStackMask();
  if constexpr (internal::kWar3RuntimeConfigDisableSemanticData)
    mask |= ModuleBit(War3RuntimeModule::SemanticData);

  if constexpr (!internal::kWar3UiHookEnabled)
    mask |= ModuleBit(War3RuntimeModule::HookUi);
  if constexpr (!internal::kWar3RenderHookEnabled)
    mask |= ModuleBit(War3RuntimeModule::HookRender);
  if constexpr (!internal::kWar3ModelHookEnabled)
    mask |= ModuleBit(War3RuntimeModule::SemanticData);

  return mask;
}

uint32_t ParseDisableMask(std::string csv) {
  csv = ToLowerAscii(csv);
  if (csv.empty())
    return 0u;

  uint32_t mask = 0u;
  std::stringstream ss(csv);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = TrimAscii(std::move(item));
    if (item.empty())
      continue;
    if (item == "all") {
      mask = 0xFFFFFFFFu;
      continue;
    }
    if (item == "shadow") {
      mask |= ModuleBit(War3RuntimeModule::ShadowCapture) |
              ModuleBit(War3RuntimeModule::ShadowMap) |
              ModuleBit(War3RuntimeModule::ShadowReceiver) |
              ModuleBit(War3RuntimeModule::ShadowTaa);
      continue;
    }
    if (item == "render") {
      mask |= RenderInterferenceMask();
      continue;
    }
    if (item == "semantic" || item == "semantic-data" ||
        item == "upper" || item == "upper.data") {
      mask |= ModuleBit(War3RuntimeModule::SemanticData);
      continue;
    }

    for (size_t i = 0; i < kModuleNames.size(); ++i) {
      if (item == kModuleNames[i]) {
        mask |= (1u << static_cast<uint32_t>(i));
        break;
      }
    }
  }
  return mask;
}

const War3RuntimeConfig& BuildConfig() {
  static const War3RuntimeConfig kConfig = []() {
    War3RuntimeConfig cfg = {};
    const std::string legacyFpsUnlockOnly =
        dxvk::env::getEnvVar("DXVK_WAR3_FPS_UNLOCK_ONLY");
    const std::string profileText = dxvk::env::getEnvVar("DXVK_WAR3_PROFILE");
    if (legacyFpsUnlockOnly == "1") {
      cfg.profile = War3RuntimeProfile::DxvkOnly;
      cfg.legacyFpsUnlockOnly = true;
    } else {
      cfg.profile = ParseProfile(profileText);
    }

    cfg.defaultDisabledMask =
        DefaultDisabledMaskForProfile(cfg.profile) | CompileTimeDisabledMask();
    cfg.explicitDisabledMask =
        ParseDisableMask(dxvk::env::getEnvVar("DXVK_WAR3_DISABLE"));
    cfg.effectiveDisabledMask =
        cfg.defaultDisabledMask | cfg.explicitDisabledMask;
    return cfg;
  }();
  return kConfig;
}

} // namespace

const War3RuntimeConfig& GetWar3RuntimeConfig() { return BuildConfig(); }

War3RuntimeProfile GetWar3RuntimeProfile() {
  return GetWar3RuntimeConfig().profile;
}

const char* GetWar3RuntimeProfileName(War3RuntimeProfile profile) {
  switch (profile) {
  case War3RuntimeProfile::DxvkOnly:
    return "dxvk_only";
  case War3RuntimeProfile::HooksMinimal:
    return "hooks_minimal";
  case War3RuntimeProfile::HooksDefault:
    return "hooks_default";
  case War3RuntimeProfile::RenderBase:
    return "render_base";
  case War3RuntimeProfile::ShadowCaptureOnly:
    return "shadow_capture_only";
  case War3RuntimeProfile::ShadowFull:
    return "shadow_full";
  case War3RuntimeProfile::FullAnalysis:
    return "full_analysis";
  case War3RuntimeProfile::FullPerfExperimental:
    return "full_perf_experimental";
  case War3RuntimeProfile::FullDefault:
  default:
    return "full_default";
  }
}

const char* GetWar3RuntimeProfileName() {
  return GetWar3RuntimeProfileName(GetWar3RuntimeProfile());
}

const char* GetWar3RuntimeModuleName(War3RuntimeModule module) {
  const uint32_t index = static_cast<uint32_t>(module);
  if (index >= kModuleNames.size())
    return "unknown";
  return kModuleNames[index];
}

bool IsWar3RuntimeModuleEnabled(War3RuntimeModule module) {
  return !IsWar3RuntimeModuleDisabled(module);
}

bool IsWar3RuntimeModuleDisabled(War3RuntimeModule module) {
  return (GetWar3RuntimeConfig().effectiveDisabledMask & ModuleBit(module)) !=
         0u;
}

bool IsAnyWar3RuntimeModuleEnabled(
    std::initializer_list<War3RuntimeModule> modules) {
  return std::any_of(modules.begin(), modules.end(),
                     [](War3RuntimeModule module) {
                       return IsWar3RuntimeModuleEnabled(module);
                     });
}

bool IsWar3RuntimeProfile(War3RuntimeProfile profile) {
  return GetWar3RuntimeProfile() == profile;
}

bool UseDxvkOnlyCompatibilityMode() {
  return IsWar3RuntimeProfile(War3RuntimeProfile::DxvkOnly);
}

bool IsStandaloneShadowCaptureProfile() {
  return IsWar3RuntimeProfile(War3RuntimeProfile::ShadowCaptureOnly);
}

std::string GetWar3RuntimeDisabledModulesCsv() {
  std::ostringstream oss;
  bool first = true;
  for (size_t i = 0; i < kModuleNames.size(); ++i) {
    const auto module = static_cast<War3RuntimeModule>(i);
    if (!IsWar3RuntimeModuleDisabled(module))
      continue;
    if (!first)
      oss << ",";
    oss << kModuleNames[i];
    first = false;
  }
  return oss.str();
}

std::string GetWar3RuntimeEnabledModulesCsv() {
  std::ostringstream oss;
  bool first = true;
  for (size_t i = 0; i < kModuleNames.size(); ++i) {
    const auto module = static_cast<War3RuntimeModule>(i);
    if (!IsWar3RuntimeModuleEnabled(module))
      continue;
    if (!first)
      oss << ",";
    oss << kModuleNames[i];
    first = false;
  }
  return oss.str();
}

} // namespace dxvk::war3::runtime
