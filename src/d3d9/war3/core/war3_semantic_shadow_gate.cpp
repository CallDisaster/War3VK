#include "war3_semantic_shadow_gate.h"

#include "war3_internal_test_config.h"

#include "../../util/util_env.h"

#include <string>

namespace dxvk::war3::internal {

namespace {

bool ParseEnabled(const std::string& value) {
  return value == "1" || value == "true" || value == "TRUE" ||
         value == "on" || value == "ON" || value == "yes" ||
         value == "YES";
}

bool EnvFlagOrDefault(const char* name, bool defaultValue) {
  const std::string value = dxvk::env::getEnvVar(name);
  if (value.empty())
    return defaultValue;

  return ParseEnabled(value);
}

bool RuntimeGateValueCacheEnabled() {
  // 环境覆盖在进程启动时确定，AutoTest 的每轮 A/B 也会启动独立进程。
  // 热 draw 路径不应为同一个变量每帧构造上千个 std::string。保留关闭
  // 开关用于同一 DLL 的严格对照与紧急回退。
  static const bool enabled = EnvFlagOrDefault(
      "DXVK_WAR3_SEMANTIC_GATE_VALUE_CACHE", true);
  return enabled;
}

} // namespace

bool IsSemanticShadowPreviewEnabled() {
  static const bool enabled =
      EnvFlagOrDefault("DXVK_WAR3_SEMANTIC_SHADOW_PREVIEW", true);
  return enabled;
}

bool IsSemanticCoreValidationRuntimeEnabled() {
  return kShadowSemanticCoreValidationEnabled &&
         IsSemanticShadowPreviewEnabled();
}

bool IsSemanticSceneSubmissionRuntimeEnabled() {
  const auto resolve = []() {
    return kShadowSemanticCoreSceneSubmissionEnabled &&
           IsSemanticShadowPreviewEnabled() &&
           EnvFlagOrDefault("DXVK_WAR3_SEMANTIC_SHADOW_SCENE_SUBMISSION",
                            true);
  };
  if (!RuntimeGateValueCacheEnabled())
    return resolve();
  static const bool enabled = resolve();
  return enabled;
}

bool IsSemanticSceneBootstrapCatchupRuntimeEnabled() {
  return kShadowSemanticCoreSceneBootstrapCatchupEnabled &&
         IsSemanticShadowPreviewEnabled() &&
         EnvFlagOrDefault("DXVK_WAR3_SEMANTIC_SHADOW_BOOTSTRAP_CATCHUP",
                          true);
}

bool IsSemanticSceneEndFrameBuildRuntimeEnabled() {
  return IsSemanticShadowPreviewEnabled() &&
         EnvFlagOrDefault("DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_BUILD", false);
}

bool IsSemanticSceneEndFrameFlushRuntimeEnabled() {
  return kShadowSemanticCoreSceneEndFrameFlushEnabled &&
         IsSemanticShadowPreviewEnabled() &&
         EnvFlagOrDefault("DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_FLUSH", true);
}

bool IsSemanticSceneTailBoundaryFallbackRuntimeEnabled() {
  return kShadowSemanticCoreSceneTailBoundaryFallbackEnabled &&
         IsSemanticShadowPreviewEnabled() &&
         EnvFlagOrDefault("DXVK_WAR3_SEMANTIC_SHADOW_TAIL_FALLBACK", false);
}

bool IsSemanticSceneBypassLegacyUnitCaptureRuntimeEnabled() {
  return IsSemanticShadowPreviewEnabled() &&
         (kShadowSemanticCoreSceneBypassLegacyUnitCaptureEnabled ||
          ParseEnabled(dxvk::env::getEnvVar(
              "DXVK_WAR3_SEMANTIC_SHADOW_BYPASS_LEGACY_UNIT_CAPTURE")));
}

bool IsSemanticSceneDisableLegacyShadowCaptureRuntimeEnabled() {
  const auto resolve = []() {
    if (!IsSemanticShadowPreviewEnabled())
      return false;

    const std::string overrideValue = dxvk::env::getEnvVar(
        "DXVK_WAR3_SEMANTIC_SHADOW_DISABLE_LEGACY_CAPTURE");
    if (!overrideValue.empty())
      return ParseEnabled(overrideValue);

    return kShadowSemanticCoreSceneDisableLegacyShadowCaptureEnabled;
  };
  if (!RuntimeGateValueCacheEnabled())
    return resolve();
  static const bool enabled = resolve();
  return enabled;
}

bool IsSemanticShadowPreReadyValidationRuntimeEnabled() {
  return IsSemanticShadowPreviewEnabled() &&
         EnvFlagOrDefault("DXVK_WAR3_SEMANTIC_SHADOW_PRE_READY", true);
}

bool IsNativeRendererHostExecuteValidationRuntimeEnabled() {
  return kNativeRendererHostExecuteValidationEnabled &&
         IsSemanticShadowPreviewEnabled() &&
         EnvFlagOrDefault("DXVK_WAR3_NATIVE_SEMANTIC_SHADOW_PREVIEW", false);
}

bool IsNativeSemanticShadowWorldStageValidationRuntimeEnabled() {
  return kNativeSemanticShadowWorldStageValidationEnabled &&
         IsSemanticShadowPreviewEnabled() &&
         EnvFlagOrDefault("DXVK_WAR3_NATIVE_SEMANTIC_SHADOW_PREVIEW", false);
}

} // namespace dxvk::war3::internal
