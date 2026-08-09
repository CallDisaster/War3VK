#include "war3_jass_command_bridge.h"
#include "war3_hook_lifecycle.h"

#include "../../d3d9_war3_debug.h"
#ifndef WAR3_SHADER_API_INTERNAL
#define WAR3_SHADER_API_INTERNAL 1
#endif
#include "../../war3_shader_api.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../japi/war3_japi_v1.h"
#include "../render/war3_lightning_runtime.h"

#include "../../jass/war3_jass_convert.h"
#include "../../jass/war3_jass_types.h"

#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace dxvk::war3::hooks {

namespace {

constexpr size_t kNativeEntryFuncPtrOffset = 0x1C;
constexpr size_t kNativeEntryParamCountOffset = 0x20;
constexpr size_t kNativeEntrySigPtrOffset = 0x24;
constexpr size_t kNativeEntryRetTypeOffset = 0x38;

constexpr const char *kWarVkPrefix = "warvk:";
// The public protocol limit is 512 bytes. Reading one additional byte lets the
// dispatcher report PayloadTooLong without scanning an untrusted JASS string
// arbitrarily far.
constexpr uint32_t kMaxNativeStringBytes = 513;

using NativeVoidStringFn = void(__cdecl *)(uint32_t);
using NativeIntStringFn = int(__cdecl *)(uint32_t);
using NativeStringStringFn = uint32_t(__cdecl *)(uint32_t);
using NativeVoidBooleanFn = void(__cdecl *)(uint32_t);
using NativeConvertCameraFieldFn = uint32_t(__cdecl *)(int32_t);
using NativeSetCameraFieldFn =
    void(__cdecl *)(uint32_t, float *, float *);
using NativeSetCameraPositionFn = void(__cdecl *)(float *, float *);
using NativePanCameraToTimedFn =
    void(__cdecl *)(float *, float *, float *);
using NativeGetCameraFieldFn = uint32_t(__cdecl *)(uint32_t);
using NativeRealVoidFn = uint32_t(__cdecl *)();
using NativeHandleVoidFn = uint32_t(__cdecl *)();
using NativeRealHandleFn = uint32_t(__cdecl *)(uint32_t);
using NativeBooleanVoidFn = uint32_t(__cdecl *)();
using NativePlayerFn = uint32_t(__cdecl *)(int);
using NativeGetLocalPlayerFn = uint32_t(__cdecl *)();
using NativeDisplayTextToPlayerFn =
    void(__cdecl *)(uint32_t, float *, float *, uint32_t);
using NativeDisplayTimedTextToPlayerFn =
    void(__cdecl *)(uint32_t, float *, float *, float *, uint32_t);
using NativeSaveIntegerFn =
    void(__cdecl *)(uint32_t, int32_t, int32_t, int32_t);
using NativeSaveRealFn =
    void(__cdecl *)(uint32_t, int32_t, int32_t, float *);
using NativeLoadIntegerFn =
    int32_t(__cdecl *)(uint32_t, int32_t, int32_t);
using NativeLoadRealFn =
    uint32_t(__cdecl *)(uint32_t, int32_t, int32_t);

float DecodeJassRealReturn(uint32_t bits) {
  float value = 0.0f;
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

uint32_t EncodeJassRealReturn(float value) {
  uint32_t bits = 0u;
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

enum class CarrierKind {
  Command,
  IntQuery,
  StringQuery,
  TypedSaveInteger,
  TypedSaveReal,
  TypedLoadInteger,
  TypedLoadReal,
};

struct CarrierPatch {
  const char *name = nullptr;
  CarrierKind kind = CarrierKind::Command;
  void *bridgeFn = nullptr;
  std::atomic<uintptr_t> originalFn{0};
  std::atomic<uintptr_t> entry{0};
  std::atomic<uint64_t> handled{0};
  std::atomic<uint64_t> passedThrough{0};
  std::atomic<uint64_t> installCount{0};
};

struct BridgeState {
  uint64_t commandCount = 0;
  uint64_t intQueryCount = 0;
  uint64_t stringQueryCount = 0;
  int lastErrorCode = 0;
  std::string lastErrorText = "ok";
  std::string lastResult = "WarVK JASS bridge ready";
};

std::atomic<uintptr_t> s_lookupFn{0};
std::atomic<bool> s_allInstalled{false};
std::atomic<bool> s_typedInstalled{false};
std::atomic<bool> s_typedInstallAttempted{false};
std::atomic<uint64_t> s_installAttempts{0};
std::atomic<uint64_t> s_installSuccesses{0};
std::atomic<uint64_t> s_typedInstallAttempts{0};
std::atomic<uint64_t> s_typedInstallSuccesses{0};
std::mutex s_installMutex;
std::mutex s_internalTestNativeMutex;

struct InternalTestVisibilityLease {
  bool active = false;
  bool fogEnabled = false;
  bool fogMaskEnabled = false;
};

InternalTestVisibilityLease s_visibilityLease;
std::mutex s_stateMutex;
BridgeState s_state;

bool StartsWith(const std::string &value, const char *prefix) {
  const size_t n = std::strlen(prefix);
  return value.size() >= n && std::memcmp(value.data(), prefix, n) == 0;
}

bool IsVersionedPublicCommand(const std::string &value) {
  const size_t prefixLength = std::strlen(kWarVkPrefix);
  if (value.size() <= prefixLength + 1u ||
      value.compare(0u, prefixLength, kWarVkPrefix) != 0 ||
      value[prefixLength] != 'v' ||
      value[prefixLength + 1u] < '0' ||
      value[prefixLength + 1u] > '9')
    return false;
  size_t offset = prefixLength + 2u;
  while (offset < value.size() && value[offset] >= '0' &&
         value[offset] <= '9')
    ++offset;
  return offset == value.size() || value[offset] == ';';
}

std::vector<std::string> SplitPipeArgs(const std::string &value) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= value.size()) {
    const size_t pos = value.find('|', start);
    if (pos == std::string::npos) {
      out.emplace_back(value.substr(start));
      break;
    }
    out.emplace_back(value.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

bool ParseBoolArg(const std::string &value, bool &out) {
  if (value == "1" || value == "true" || value == "TRUE" || value == "True") {
    out = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "FALSE" ||
      value == "False") {
    out = false;
    return true;
  }
  return false;
}

bool ParseIntArg(const std::string &value, int32_t &out) {
  if (value.empty())
    return false;
  char *end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 0);
  if (!end || *end != '\0')
    return false;
  out = static_cast<int32_t>(parsed);
  return true;
}

bool ParseUIntArg(const std::string &value, uint32_t &out) {
  int32_t parsed = 0;
  if (!ParseIntArg(value, parsed) || parsed < 0)
    return false;
  out = static_cast<uint32_t>(parsed);
  return true;
}

bool ParseFloatArg(const std::string &value, float &out) {
  if (value.empty())
    return false;
  char *end = nullptr;
  const float parsed = std::strtof(value.c_str(), &end);
  if (!end || *end != '\0')
    return false;
  out = parsed;
  return true;
}

void SetCommandOk(BridgeState &state, const std::string &result,
                  std::string *stringResult) {
  state.lastErrorCode = 0;
  state.lastErrorText = "ok";
  state.lastResult = result;
  if (stringResult)
    *stringResult = result;
}

int SetCommandFailure(BridgeState &state, int code, const std::string &message,
                      std::string *stringResult) {
  state.lastErrorCode = code;
  state.lastErrorText = message;
  state.lastResult = message;
  if (stringResult)
    *stringResult = message;
  return code;
}

int HandleShaderApiCommand(BridgeState &state, const std::string &command,
                           std::string *stringResult) {
  const std::vector<std::string> args = SplitPipeArgs(command);
  if (args.empty() || args[0].empty())
    return SetCommandFailure(state, -400, "empty shader command",
                             stringResult);

  auto failArgs = [&]() {
    return SetCommandFailure(state, -401, "bad args for command: " + args[0],
                             stringResult);
  };
  auto okBool = [&](bool success) {
    if (!success)
      return SetCommandFailure(state, -500, "shader api failed: " + args[0],
                               stringResult);
    SetCommandOk(state, "ok:" + args[0], stringResult);
    return 1;
  };
  auto parseBoolAt = [&](size_t index, bool &value) {
    return index < args.size() && ParseBoolArg(args[index], value);
  };
  auto parseIntAt = [&](size_t index, int32_t &value) {
    return index < args.size() && ParseIntArg(args[index], value);
  };
  auto parseUIntAt = [&](size_t index, uint32_t &value) {
    return index < args.size() && ParseUIntArg(args[index], value);
  };
  auto parseFloatAt = [&](size_t index, float &value) {
    return index < args.size() && ParseFloatArg(args[index], value);
  };

  if (args[0] == "add-point-light") {
    float x, y, z, range, r, g, b, intensity, shadowIntensity;
    if (args.size() != 10 || !parseFloatAt(1, x) || !parseFloatAt(2, y) ||
        !parseFloatAt(3, z) || !parseFloatAt(4, range) ||
        !parseFloatAt(5, r) || !parseFloatAt(6, g) ||
        !parseFloatAt(7, b) || !parseFloatAt(8, intensity) ||
        !parseFloatAt(9, shadowIntensity))
      return failArgs();
    const int32_t id = war3shader::AddPointLight(
        x, y, z, range, r, g, b, intensity, shadowIntensity);
    SetCommandOk(state, std::to_string(id), stringResult);
    return id;
  }

  if (args[0] == "update-point-light") {
    int32_t id;
    float x, y, z, range, r, g, b, intensity;
    if (args.size() != 10 || !parseIntAt(1, id) || !parseFloatAt(2, x) ||
        !parseFloatAt(3, y) || !parseFloatAt(4, z) ||
        !parseFloatAt(5, range) || !parseFloatAt(6, r) ||
        !parseFloatAt(7, g) || !parseFloatAt(8, b) ||
        !parseFloatAt(9, intensity))
      return failArgs();
    return okBool(war3shader::UpdatePointLight(id, x, y, z, range, r, g, b,
                                               intensity));
  }

  if (args[0] == "update-point-light-ex") {
    int32_t id;
    float x, y, z, range, r, g, b, intensity, shadowIntensity;
    if (args.size() != 11 || !parseIntAt(1, id) || !parseFloatAt(2, x) ||
        !parseFloatAt(3, y) || !parseFloatAt(4, z) ||
        !parseFloatAt(5, range) || !parseFloatAt(6, r) ||
        !parseFloatAt(7, g) || !parseFloatAt(8, b) ||
        !parseFloatAt(9, intensity) || !parseFloatAt(10, shadowIntensity))
      return failArgs();
    return okBool(war3shader::UpdatePointLightEx(
        id, x, y, z, range, r, g, b, intensity, shadowIntensity));
  }

  if (args[0] == "set-point-light-shadow") {
    int32_t id;
    float shadowIntensity;
    if (args.size() != 3 || !parseIntAt(1, id) ||
        !parseFloatAt(2, shadowIntensity))
      return failArgs();
    return okBool(
        war3shader::SetPointLightShadowIntensity(id, shadowIntensity));
  }

  if (args[0] == "remove-point-light") {
    int32_t id;
    if (args.size() != 2 || !parseIntAt(1, id))
      return failArgs();
    return okBool(war3shader::RemovePointLight(id));
  }

  if (args[0] == "clear-point-lights") {
    if (args.size() != 1)
      return failArgs();
    war3shader::ClearPointLights();
    SetCommandOk(state, "ok:" + args[0], stringResult);
    return 1;
  }

  if (args[0] == "get-point-light-count") {
    if (args.size() != 1)
      return failArgs();
    const uint32_t count = war3shader::GetPointLightCount();
    SetCommandOk(state, std::to_string(count), stringResult);
    return static_cast<int>(count);
  }

  if (args[0] == "outline-add-handle") {
    uint32_t handle;
    if (args.size() != 2 || !parseUIntAt(1, handle))
      return failArgs();
    war3shader::AddOutlineHandle(handle);
    SetCommandOk(state, "ok:" + args[0], stringResult);
    return 1;
  }

  if (args[0] == "outline-remove-handle") {
    uint32_t handle;
    if (args.size() != 2 || !parseUIntAt(1, handle))
      return failArgs();
    war3shader::RemoveOutlineHandle(handle);
    SetCommandOk(state, "ok:" + args[0], stringResult);
    return 1;
  }

  if (args[0] == "outline-clear-handles") {
    war3shader::ClearOutlineHandles();
    SetCommandOk(state, "ok:" + args[0], stringResult);
    return 1;
  }

  if (args[0] == "bloom-add-handle") {
    uint32_t handle;
    float boost;
    if (args.size() != 3 || !parseUIntAt(1, handle) || !parseFloatAt(2, boost))
      return failArgs();
    war3shader::AddBloomHandle(handle, boost);
    SetCommandOk(state, "ok:" + args[0], stringResult);
    return 1;
  }

  if (args[0] == "bloom-remove-handle") {
    uint32_t handle;
    if (args.size() != 2 || !parseUIntAt(1, handle))
      return failArgs();
    war3shader::RemoveBloomHandle(handle);
    SetCommandOk(state, "ok:" + args[0], stringResult);
    return 1;
  }

  if (args[0] == "bloom-clear-handles") {
    war3shader::ClearBloomHandles();
    SetCommandOk(state, "ok:" + args[0], stringResult);
    return 1;
  }

  if (args[0] == "set-lighting-enabled") {
    bool enabled;
    if (args.size() != 2 || !parseBoolAt(1, enabled))
      return failArgs();
    return okBool(war3shader::SetLightingEnabled(enabled));
  }

  if (args[0] == "set-sun-direction") {
    float x, y, z;
    if (args.size() != 4 || !parseFloatAt(1, x) || !parseFloatAt(2, y) ||
        !parseFloatAt(3, z))
      return failArgs();
    return okBool(war3shader::SetSunDirection(x, y, z));
  }

  if (args[0] == "set-sun-color") {
    float r, g, b;
    if (args.size() != 4 || !parseFloatAt(1, r) || !parseFloatAt(2, g) ||
        !parseFloatAt(3, b))
      return failArgs();
    return okBool(war3shader::SetSunColor(r, g, b));
  }

  if (args[0] == "set-sun-intensity") {
    float value;
    if (args.size() != 2 || !parseFloatAt(1, value))
      return failArgs();
    return okBool(war3shader::SetSunIntensity(value));
  }

  if (args[0] == "set-shadow-enabled") {
    bool enabled;
    if (args.size() != 2 || !parseBoolAt(1, enabled))
      return failArgs();
    return okBool(war3shader::SetShadowEnabled(enabled));
  }

  if (args[0] == "set-shadow-strength") {
    float value;
    if (args.size() != 2 || !parseFloatAt(1, value))
      return failArgs();
    return okBool(war3shader::SetShadowStrength(value));
  }

  if (args[0] == "set-shadow-bias") {
    float value;
    if (args.size() != 2 || !parseFloatAt(1, value))
      return failArgs();
    return okBool(war3shader::SetShadowBias(value));
  }

  if (args[0] == "set-shadow-pcf-radius") {
    float value;
    if (args.size() != 2 || !parseFloatAt(1, value))
      return failArgs();
    return okBool(war3shader::SetShadowPcfRadius(value));
  }

  if (args[0] == "set-shadow-debug-mode") {
    uint32_t mode;
    if (args.size() != 2 || !parseUIntAt(1, mode))
      return failArgs();
    return okBool(war3shader::SetShadowDebugMode(mode));
  }

  if (args[0] == "set-point-lights-enabled") {
    bool enabled;
    if (args.size() != 2 || !parseBoolAt(1, enabled))
      return failArgs();
    return okBool(war3shader::SetPointLightsEnabled(enabled));
  }

  if (args[0] == "set-point-shadow-enabled") {
    bool enabled;
    if (args.size() != 2 || !parseBoolAt(1, enabled))
      return failArgs();
    return okBool(war3shader::SetPointShadowEnabled(enabled));
  }

  if (args[0] == "set-point-shadow-bias") {
    float value;
    if (args.size() != 2 || !parseFloatAt(1, value))
      return failArgs();
    return okBool(war3shader::SetPointShadowBias(value));
  }

  if (args[0] == "set-volumetric-light-enabled" ||
      args[0] == "set-volumetric-enabled") {
    bool enabled;
    if (args.size() != 2 || !parseBoolAt(1, enabled))
      return failArgs();
    return okBool(war3shader::SetVolumetricLightEnabled(enabled));
  }

  if (args[0] == "set-volumetric-light-params") {
    float intensity, density, weight, decay;
    uint32_t samples;
    if (args.size() != 6 || !parseFloatAt(1, intensity) ||
        !parseFloatAt(2, density) || !parseFloatAt(3, weight) ||
        !parseFloatAt(4, decay) || !parseUIntAt(5, samples))
      return failArgs();
    return okBool(war3shader::SetVolumetricLightParams(
        intensity, density, weight, decay, samples));
  }

  if (args[0] == "set-volumetric-light-fade") {
    float fadeNear, fadeFar, maxRayDistance;
    if (args.size() != 4 || !parseFloatAt(1, fadeNear) ||
        !parseFloatAt(2, fadeFar) || !parseFloatAt(3, maxRayDistance))
      return failArgs();
    return okBool(war3shader::SetVolumetricLightFade(
        fadeNear, fadeFar, maxRayDistance));
  }

  if (args[0] == "set-volumetric-height-fog") {
    float baseHeight, falloff, strength;
    if (args.size() != 4 || !parseFloatAt(1, baseHeight) ||
        !parseFloatAt(2, falloff) || !parseFloatAt(3, strength))
      return failArgs();
    return okBool(
        war3shader::SetVolumetricHeightFog(baseHeight, falloff, strength));
  }

  if (args[0] == "set-volumetric-resolution-divisor") {
    uint32_t divisor;
    if (args.size() != 2 || !parseUIntAt(1, divisor))
      return failArgs();
    return okBool(war3shader::SetVolumetricResolutionDivisor(divisor));
  }

  if (args[0] == "set-outline-enabled") {
    bool enabled;
    if (args.size() != 2 || !parseBoolAt(1, enabled))
      return failArgs();
    return okBool(war3shader::SetOutlineEnabled(enabled));
  }

  if (args[0] == "set-outline-width") {
    float value;
    if (args.size() != 2 || !parseFloatAt(1, value))
      return failArgs();
    return okBool(war3shader::SetOutlineWidth(value));
  }

  if (args[0] == "set-outline-color") {
    float r, g, b, a;
    if (args.size() != 5 || !parseFloatAt(1, r) || !parseFloatAt(2, g) ||
        !parseFloatAt(3, b) || !parseFloatAt(4, a))
      return failArgs();
    return okBool(war3shader::SetOutlineColor(r, g, b, a));
  }

  if (args[0] == "set-outline-mode") {
    uint32_t mode;
    if (args.size() != 2 || !parseUIntAt(1, mode))
      return failArgs();
    return okBool(war3shader::SetOutlineMode(mode));
  }

  if (args[0] == "set-outline-visibility") {
    bool visible, occluded;
    if (args.size() != 3 || !parseBoolAt(1, visible) ||
        !parseBoolAt(2, occluded))
      return failArgs();
    return okBool(war3shader::SetOutlineVisibility(visible, occluded));
  }

  if (args[0] == "set-postfx-enabled") {
    bool enabled;
    if (args.size() != 2 || !parseBoolAt(1, enabled))
      return failArgs();
    return okBool(war3shader::SetPostFxEnabled(enabled));
  }

  if (args[0] == "set-exposure") {
    float value;
    if (args.size() != 2 || !parseFloatAt(1, value))
      return failArgs();
    return okBool(war3shader::SetExposure(value));
  }

  if (args[0] == "set-bloom-enabled") {
    bool enabled;
    if (args.size() != 2 || !parseBoolAt(1, enabled))
      return failArgs();
    return okBool(war3shader::SetBloomEnabled(enabled));
  }

  if (args[0] == "set-bloom-params") {
    float threshold, softKnee, intensity;
    if (args.size() != 4 || !parseFloatAt(1, threshold) ||
        !parseFloatAt(2, softKnee) || !parseFloatAt(3, intensity))
      return failArgs();
    return okBool(war3shader::SetBloomParams(threshold, softKnee, intensity));
  }

  if (args[0] == "set-aces-enabled") {
    bool enabled;
    if (args.size() != 2 || !parseBoolAt(1, enabled))
      return failArgs();
    return okBool(war3shader::SetAcesEnabled(enabled));
  }

  if (args[0] == "set-ssao-enabled") {
    bool enabled;
    if (args.size() != 2 || !parseBoolAt(1, enabled))
      return failArgs();
    return okBool(war3shader::SetSsaoEnabled(enabled));
  }

  if (args[0] == "set-ssao-params") {
    float radius, strength, bias, power;
    if (args.size() != 5 || !parseFloatAt(1, radius) ||
        !parseFloatAt(2, strength) || !parseFloatAt(3, bias) ||
        !parseFloatAt(4, power))
      return failArgs();
    return okBool(war3shader::SetSsaoParams(radius, strength, bias, power));
  }

  if (args[0] == "set-aa-mode") {
    uint32_t mode;
    if (args.size() != 2 || !parseUIntAt(1, mode))
      return failArgs();
    return okBool(war3shader::SetAaMode(mode));
  }

  if (args[0] == "set-fxaa-params") {
    float subpix, threshold, minThreshold;
    if (args.size() != 4 || !parseFloatAt(1, subpix) ||
        !parseFloatAt(2, threshold) || !parseFloatAt(3, minThreshold))
      return failArgs();
    return okBool(war3shader::SetFxaaParams(subpix, threshold, minThreshold));
  }

  if (args[0] == "set-smaa-params") {
    float threshold;
    int32_t search, diagSearch;
    if (args.size() != 4 || !parseFloatAt(1, threshold) ||
        !parseIntAt(2, search) || !parseIntAt(3, diagSearch))
      return failArgs();
    return okBool(war3shader::SetSmaaParams(threshold, search, diagSearch));
  }

  if (args[0] == "set-day-night-enabled") {
    bool enabled;
    if (args.size() != 2 || !parseBoolAt(1, enabled))
      return failArgs();
    return okBool(war3shader::SetDayNightEnabled(enabled));
  }

  if (args[0] == "set-day-night-min-factor") {
    float value;
    if (args.size() != 2 || !parseFloatAt(1, value))
      return failArgs();
    return okBool(war3shader::SetDayNightMinFactor(value));
  }

  if (args[0] == "set-day-night-ambient") {
    float dayR, dayG, dayB, nightR, nightG, nightB;
    if (args.size() != 7 || !parseFloatAt(1, dayR) ||
        !parseFloatAt(2, dayG) || !parseFloatAt(3, dayB) ||
        !parseFloatAt(4, nightR) || !parseFloatAt(5, nightG) ||
        !parseFloatAt(6, nightB))
      return failArgs();
    return okBool(war3shader::SetDayNightAmbient(dayR, dayG, dayB, nightR,
                                                 nightG, nightB));
  }

  if (args[0] == "lightning-create") {
    float x0, y0, z0, x1, y1, z1;
    if (args.size() != 7 || !parseFloatAt(1, x0) || !parseFloatAt(2, y0) ||
        !parseFloatAt(3, z0) || !parseFloatAt(4, x1) ||
        !parseFloatAt(5, y1) || !parseFloatAt(6, z1)) {
      dxvk::war3::render::War3LightningRuntime::instance().noteCommandFailure();
      return failArgs();
    }
    dxvk::war3::render::War3LightningCreateDesc desc = {};
    desc.start = {x0, y0, z0};
    desc.end = {x1, y1, z1};
    const int32_t id =
        dxvk::war3::render::War3LightningRuntime::instance().create(desc);
    SetCommandOk(state, std::to_string(id), stringResult);
    return id;
  }

  if (args[0] == "lightning-move") {
    int32_t id;
    float x0, y0, z0, x1, y1, z1;
    if (args.size() != 8 || !parseIntAt(1, id) || !parseFloatAt(2, x0) ||
        !parseFloatAt(3, y0) || !parseFloatAt(4, z0) ||
        !parseFloatAt(5, x1) || !parseFloatAt(6, y1) ||
        !parseFloatAt(7, z1)) {
      dxvk::war3::render::War3LightningRuntime::instance().noteCommandFailure();
      return failArgs();
    }
    return okBool(dxvk::war3::render::War3LightningRuntime::instance().move(
        id, {x0, y0, z0}, {x1, y1, z1}));
  }

  if (args[0] == "lightning-destroy") {
    int32_t id;
    if (args.size() != 2 || !parseIntAt(1, id)) {
      dxvk::war3::render::War3LightningRuntime::instance().noteCommandFailure();
      return failArgs();
    }
    return okBool(
        dxvk::war3::render::War3LightningRuntime::instance().destroy(id));
  }

  if (args[0] == "lightning-set-color") {
    int32_t id;
    float r0, g0, b0, a0, r1, g1, b1, a1;
    if (args.size() != 10 || !parseIntAt(1, id) || !parseFloatAt(2, r0) ||
        !parseFloatAt(3, g0) || !parseFloatAt(4, b0) ||
        !parseFloatAt(5, a0) || !parseFloatAt(6, r1) ||
        !parseFloatAt(7, g1) || !parseFloatAt(8, b1) ||
        !parseFloatAt(9, a1)) {
      dxvk::war3::render::War3LightningRuntime::instance().noteCommandFailure();
      return failArgs();
    }
    return okBool(dxvk::war3::render::War3LightningRuntime::instance().setColor(
        id, r0, g0, b0, a0, r1, g1, b1, a1));
  }

  if (args[0] == "lightning-set-width") {
    int32_t id;
    float startWidth, endWidth;
    if (args.size() != 4 || !parseIntAt(1, id) ||
        !parseFloatAt(2, startWidth) || !parseFloatAt(3, endWidth)) {
      dxvk::war3::render::War3LightningRuntime::instance().noteCommandFailure();
      return failArgs();
    }
    return okBool(dxvk::war3::render::War3LightningRuntime::instance().setWidth(
        id, startWidth, endWidth));
  }

  if (args[0] == "lightning-set-curve") {
    int32_t id;
    float curve, noise;
    uint32_t segments, branches;
    if (args.size() != 6 || !parseIntAt(1, id) ||
        !parseFloatAt(2, curve) || !parseFloatAt(3, noise) ||
        !parseUIntAt(4, segments) || !parseUIntAt(5, branches)) {
      dxvk::war3::render::War3LightningRuntime::instance().noteCommandFailure();
      return failArgs();
    }
    return okBool(dxvk::war3::render::War3LightningRuntime::instance().setCurve(
        id, curve, noise, segments, branches));
  }

  if (args[0] == "lightning-set-lifetime") {
    int32_t id;
    float lifetime, fadeIn, fadeOut;
    if (args.size() != 5 || !parseIntAt(1, id) ||
        !parseFloatAt(2, lifetime) || !parseFloatAt(3, fadeIn) ||
        !parseFloatAt(4, fadeOut)) {
      dxvk::war3::render::War3LightningRuntime::instance().noteCommandFailure();
      return failArgs();
    }
    return okBool(
        dxvk::war3::render::War3LightningRuntime::instance().setLifetime(
            id, lifetime, fadeIn, fadeOut));
  }

  if (args[0] == "lightning-set-pulse") {
    int32_t id;
    float amplitude, frequencyHz;
    if (args.size() != 4 || !parseIntAt(1, id) ||
        !parseFloatAt(2, amplitude) || !parseFloatAt(3, frequencyHz)) {
      dxvk::war3::render::War3LightningRuntime::instance().noteCommandFailure();
      return failArgs();
    }
    return okBool(dxvk::war3::render::War3LightningRuntime::instance().setPulse(
        id, amplitude, frequencyHz));
  }

  if (args[0] == "lightning-active-count") {
    const auto summary =
        dxvk::war3::render::War3LightningRuntime::instance().snapshot();
    SetCommandOk(state, std::to_string(summary.activeCount), stringResult);
    return static_cast<int>(summary.activeCount);
  }

  if (args[0] == "lightning-stats") {
    const std::string stats =
        dxvk::war3::render::War3LightningRuntime::instance().statsString();
    SetCommandOk(state, stats, stringResult);
    return static_cast<int>(
        dxvk::war3::render::War3LightningRuntime::instance()
            .snapshot()
            .activeCount);
  }

  // Unknown cmd: commands are accepted as a forward-compatible smoke path.
  SetCommandOk(state, "accepted:" + command, stringResult);
  war3dbg::Print("DXVK War3JassBridge: accepted generic command '%s'\n",
                 command.c_str());
  return 1;
}

bool CopyCStringBounded(const char *src, std::string &out) {
  out.clear();
  if (!src)
    return false;

  for (uint32_t i = 0; i < kMaxNativeStringBytes; ++i) {
    if (!dxvk::war3::IsReadableRange(src + i, 1))
      return false;

    const char c = src[i];
    if (c == '\0')
      return true;
    out.push_back(c);
  }

  // A non-terminated 513-byte prefix is enough for public WarVK traffic to
  // fail the strict 512-byte gate. Non-WarVK traffic will still be forwarded
  // with its original native argument.
  return !out.empty();
}

bool ReadCStringPtr(const void *base, size_t offset, std::string &out) {
  const char *str = nullptr;
  if (!dxvk::war3::SafeRead<const char *>(base, offset, str))
    return false;
  return CopyCStringBounded(str, out);
}

bool DecodeNativeStringArg(uint32_t nativeArg, std::string &out) {
  out.clear();
  if (!nativeArg)
    return false;

  const void *arg = reinterpret_cast<const void *>(static_cast<uintptr_t>(nativeArg));

  // Original ExecuteNativeFunction converts JASS string handles to native string
  // memory. In 1.27a this is normally RCString*, whose +0x8 points to CStringRep
  // and CStringRep+0x1C points to the char buffer.
  void *rep = nullptr;
  if (dxvk::war3::SafeReadPtr(arg, 0x8, rep) && rep &&
      ReadCStringPtr(rep, 0x1C, out)) {
    return true;
  }

  // Some string helpers work directly with CStringRep*.
  if (ReadCStringPtr(arg, 0x1C, out))
    return true;

  // Last-resort safety net for functions that may receive const char*.
  return CopyCStringBounded(reinterpret_cast<const char *>(arg), out);
}

bool ReadNativeMeta(void *entry, void *&funcPtr, const char *&sigPtr,
                    uint32_t &paramCount, uint32_t &retType) {
  if (!entry)
    return false;
  if (!dxvk::war3::SafeReadPtr(entry, kNativeEntryFuncPtrOffset, funcPtr))
    return false;
  if (!dxvk::war3::SafeRead<const char *>(entry, kNativeEntrySigPtrOffset,
                                          sigPtr))
    return false;
  if (!dxvk::war3::SafeReadU32(entry, kNativeEntryParamCountOffset, paramCount))
    return false;
  if (!dxvk::war3::SafeReadU32(entry, kNativeEntryRetTypeOffset, retType))
    return false;
  return funcPtr && sigPtr;
}

void *LookupNativeEntry(const char *name) {
  const auto lookupFn = reinterpret_cast<GetTlsJassDataFn>(
      s_lookupFn.load(std::memory_order_relaxed));
  if (!lookupFn || !name)
    return nullptr;
  return lookupFn(const_cast<char *>(name));
}

void *ReadCurrentNativeFunc(void *entry) {
  void *funcPtr = nullptr;
  if (!entry)
    return nullptr;
  if (!dxvk::war3::SafeReadPtr(entry, kNativeEntryFuncPtrOffset, funcPtr))
    return nullptr;
  return funcPtr;
}

bool CopyNativeSignature(const char *signature, std::string &out) {
  out.clear();
  if (!signature)
    return false;

  constexpr size_t kMaxSignatureBytes = 96u;
  for (size_t i = 0u; i < kMaxSignatureBytes; ++i) {
    if (!dxvk::war3::IsReadableRange(signature + i, 1u)) {
      out.clear();
      return false;
    }
    const char value = signature[i];
    if (value == '\0')
      return !out.empty();
    out.push_back(value);
  }
  out.clear();
  return false;
}

bool ResolveNativeStrict(const char *name, const char *expectedSignature,
                         uint32_t expectedParamCount, void *&funcPtr,
                         std::string &actualSignature, std::string &error) {
  funcPtr = nullptr;
  actualSignature.clear();
  void *entry = LookupNativeEntry(name);
  if (!entry) {
    error = std::string(name) + " native entry was not found";
    return false;
  }

  const char *signature = nullptr;
  uint32_t paramCount = 0u;
  uint32_t returnType = 0u;
  if (!ReadNativeMeta(entry, funcPtr, signature, paramCount, returnType) ||
      !CopyNativeSignature(signature, actualSignature)) {
    error = std::string(name) + " native metadata is unreadable";
    funcPtr = nullptr;
    return false;
  }
  if (paramCount != expectedParamCount ||
      actualSignature != expectedSignature) {
    error = std::string(name) + " native signature mismatch: " +
        actualSignature;
    funcPtr = nullptr;
    return false;
  }
  return true;
}

struct ResolvedCameraTestNatives {
  NativeConvertCameraFieldFn convert = nullptr;
  NativeSetCameraFieldFn set = nullptr;
  NativeSetCameraPositionFn position = nullptr;
  NativePanCameraToTimedFn pan = nullptr;
  NativeGetCameraFieldFn getField = nullptr;
  NativeRealVoidFn getTargetX = nullptr;
  NativeRealVoidFn getTargetY = nullptr;
  NativeRealVoidFn getTargetZ = nullptr;
};

bool ResolveCameraApplyNatives(ResolvedCameraTestNatives &out,
                               bool needPosition, bool needPan,
                               std::string &error) {
  void *ptr = nullptr;
  std::string signature;
  if (!ResolveNativeStrict("ConvertCameraField", "(I)Hcamerafield;", 1u,
                           ptr, signature, error))
    return false;
  out.convert = reinterpret_cast<NativeConvertCameraFieldFn>(ptr);
  if (!ResolveNativeStrict("SetCameraField", "(Hcamerafield;RR)V", 3u,
                           ptr, signature, error))
    return false;
  out.set = reinterpret_cast<NativeSetCameraFieldFn>(ptr);
  if (needPosition) {
    if (!ResolveNativeStrict("SetCameraPosition", "(RR)V", 2u, ptr,
                             signature, error))
      return false;
    out.position = reinterpret_cast<NativeSetCameraPositionFn>(ptr);
  }
  if (needPan) {
    if (!ResolveNativeStrict("PanCameraToTimed", "(RRR)V", 3u, ptr,
                             signature, error))
      return false;
    out.pan = reinterpret_cast<NativePanCameraToTimedFn>(ptr);
  }
  return true;
}

bool ResolveCameraSnapshotNatives(ResolvedCameraTestNatives &out,
                                  std::string &error) {
  void *ptr = nullptr;
  std::string signature;
  if (!ResolveNativeStrict("ConvertCameraField", "(I)Hcamerafield;", 1u,
                           ptr, signature, error))
    return false;
  out.convert = reinterpret_cast<NativeConvertCameraFieldFn>(ptr);
  if (!ResolveNativeStrict("GetCameraField", "(Hcamerafield;)R", 1u, ptr,
                           signature, error))
    return false;
  out.getField = reinterpret_cast<NativeGetCameraFieldFn>(ptr);
  if (!ResolveNativeStrict("GetCameraTargetPositionX", "()R", 0u, ptr,
                           signature, error))
    return false;
  out.getTargetX = reinterpret_cast<NativeRealVoidFn>(ptr);
  if (!ResolveNativeStrict("GetCameraTargetPositionY", "()R", 0u, ptr,
                           signature, error))
    return false;
  out.getTargetY = reinterpret_cast<NativeRealVoidFn>(ptr);
  if (!ResolveNativeStrict("GetCameraTargetPositionZ", "()R", 0u, ptr,
                           signature, error))
    return false;
  out.getTargetZ = reinterpret_cast<NativeRealVoidFn>(ptr);
  return true;
}

const char *ExpectedCarrierSignature(CarrierKind kind) {
  switch (kind) {
  case CarrierKind::Command:
    return "(S)V";
  case CarrierKind::IntQuery:
    return "(S)I";
  case CarrierKind::StringQuery:
    return "(S)S";
  case CarrierKind::TypedSaveInteger:
    return "(Hhashtable;III)V";
  case CarrierKind::TypedSaveReal:
    return "(Hhashtable;IIR)V";
  case CarrierKind::TypedLoadInteger:
    return "(Hhashtable;II)I";
  case CarrierKind::TypedLoadReal:
    return "(Hhashtable;II)R";
  }
  return "";
}

uint32_t ExpectedCarrierParameterCount(CarrierKind kind) {
  switch (kind) {
  case CarrierKind::Command:
  case CarrierKind::IntQuery:
  case CarrierKind::StringQuery:
    return 1u;
  case CarrierKind::TypedSaveInteger:
  case CarrierKind::TypedSaveReal:
    return 4u;
  case CarrierKind::TypedLoadInteger:
  case CarrierKind::TypedLoadReal:
    return 3u;
  }
  return 0u;
}

bool SignatureMatchesCarrier(const char *sigPtr, CarrierKind kind) {
  const char *expected = ExpectedCarrierSignature(kind);
  const size_t length = expected ? std::strlen(expected) : 0u;
  if (!sigPtr || !expected || length == 0u ||
      !dxvk::war3::IsReadableRange(sigPtr, length + 1u))
    return false;
  return std::memcmp(sigPtr, expected, length + 1u) == 0;
}

bool WriteNativeFuncPtr(void *entry, void *bridgeFn) {
  if (!entry || !bridgeFn)
    return false;

  void **slot = reinterpret_cast<void **>(
      reinterpret_cast<uint8_t *>(entry) + kNativeEntryFuncPtrOffset);
  if (!dxvk::war3::IsReadableRange(slot, sizeof(void *)))
    return false;

  DWORD oldProtect = 0;
  if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &oldProtect))
    return false;
  *slot = bridgeFn;
  FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
  DWORD ignored = 0;
  VirtualProtect(slot, sizeof(void *), oldProtect, &ignored);
  return true;
}

int HandleWarVkPayload(CarrierKind kind, const std::string &payload,
                       std::string *stringResult) {
  std::lock_guard<std::mutex> lock(s_stateMutex);
  switch (kind) {
  case CarrierKind::Command:
    s_state.commandCount += 1;
    break;
  case CarrierKind::IntQuery:
    s_state.intQueryCount += 1;
    break;
  case CarrierKind::StringQuery:
    s_state.stringQueryCount += 1;
    break;
  case CarrierKind::TypedSaveInteger:
  case CarrierKind::TypedSaveReal:
  case CarrierKind::TypedLoadInteger:
  case CarrierKind::TypedLoadReal:
    return 0;
  }

  if (payload == "ping") {
    s_state.lastErrorCode = 0;
    s_state.lastErrorText = "ok";
    s_state.lastResult = "pong";
    if (stringResult)
      *stringResult = s_state.lastResult;
    return 1;
  }

  if (payload == "version") {
    s_state.lastErrorCode = 0;
    s_state.lastErrorText = "ok";
    s_state.lastResult = "WarVK JASS bridge v1";
    if (stringResult)
      *stringResult = s_state.lastResult;
    return 1;
  }

  if (payload == "plugin-version") {
    s_state.lastErrorCode = 0;
    s_state.lastErrorText = "ok";
    s_state.lastResult = "1";
    if (stringResult)
      *stringResult = s_state.lastResult;
    return 1;
  }

  if (payload == "game-version") {
    s_state.lastErrorCode = 0;
    s_state.lastErrorText = "ok";
    s_state.lastResult = "634";
    if (stringResult)
      *stringResult = s_state.lastResult;
    return 0x27A;
  }

  if (payload == "game-time-ms") {
    const int gameTimeMs =
        static_cast<int>(war3shader::GetGameTime() * 1000.0f);
    s_state.lastErrorCode = 0;
    s_state.lastErrorText = "ok";
    s_state.lastResult = std::to_string(gameTimeMs);
    if (stringResult)
      *stringResult = s_state.lastResult;
    return gameTimeMs;
  }

  if (payload == "last-error") {
    if (stringResult)
      *stringResult = s_state.lastErrorText;
    return s_state.lastErrorCode;
  }

  if (payload == "last-result") {
    if (stringResult)
      *stringResult = s_state.lastResult;
    return s_state.lastErrorCode == 0 ? 1 : s_state.lastErrorCode;
  }

  if (payload == "stats") {
    if (stringResult) {
      char buffer[256] = {};
      std::snprintf(
          buffer, sizeof(buffer),
          "commands=%llu intQueries=%llu stringQueries=%llu lastError=%d",
          static_cast<unsigned long long>(s_state.commandCount),
          static_cast<unsigned long long>(s_state.intQueryCount),
          static_cast<unsigned long long>(s_state.stringQueryCount),
          s_state.lastErrorCode);
      *stringResult = buffer;
    }
    return 1;
  }

  if (StartsWith(payload, "log:")) {
    s_state.lastErrorCode = 0;
    s_state.lastErrorText = "ok";
    s_state.lastResult = payload.substr(4);
    war3dbg::Print("DXVK War3JassBridge: script log: %s\n",
                   s_state.lastResult.c_str());
    if (stringResult)
      *stringResult = s_state.lastResult;
    return 1;
  }

  if (StartsWith(payload, "cmd:")) {
    return HandleShaderApiCommand(s_state, payload.substr(4), stringResult);
  }

  s_state.lastErrorCode = -404;
  s_state.lastErrorText = "unknown command: " + payload;
  s_state.lastResult = s_state.lastErrorText;
  if (stringResult)
    *stringResult = s_state.lastErrorText;
  war3dbg::Print("DXVK War3JassBridge: unknown command '%s'\n",
                 payload.c_str());
  return s_state.lastErrorCode;
}

bool TryHandleCarrier(CarrierPatch &patch, uint32_t nativeArg, int *intResult,
                      uint32_t *stringResult) {
  std::string command;
  if (!DecodeNativeStringArg(nativeArg, command) || !StartsWith(command, kWarVkPrefix)) {
    patch.passedThrough.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  if (IsVersionedPublicCommand(command)) {
    dxvk::war3::japi::Carrier carrier =
        dxvk::war3::japi::Carrier::Preloader;
    if (patch.kind == CarrierKind::IntQuery)
      carrier = dxvk::war3::japi::Carrier::Hotkey;
    else if (patch.kind == CarrierKind::StringQuery)
      carrier = dxvk::war3::japi::Carrier::LocalizedString;

    const dxvk::war3::japi::Reply reply =
        dxvk::war3::japi::Dispatch(carrier, command);
    patch.handled.fetch_add(1, std::memory_order_relaxed);

    if (intResult) {
      *intResult = reply.ok() &&
                           reply.kind ==
                               dxvk::war3::japi::ResultKind::Integer
                       ? reply.integer
                       : 0;
    }
    if (stringResult) {
      *stringResult = 0u;
      if (reply.ok() &&
          reply.kind == dxvk::war3::japi::ResultKind::Text) {
        const jString converted = jass::convert::to_jString(reply.text);
        *stringResult = converted;
        if (!converted)
          dxvk::war3::japi::NoteTransportFailure();
      }
    }
    return true;
  }

  const std::string payload = command.substr(std::strlen(kWarVkPrefix));
  std::string text;
  const int code = HandleWarVkPayload(patch.kind, payload, &text);
  patch.handled.fetch_add(1, std::memory_order_relaxed);

  if (intResult)
    *intResult = code;
  if (stringResult) {
    const jString s = jass::convert::to_jString(text);
    *stringResult = s;
    if (!s) {
      std::lock_guard<std::mutex> lock(s_stateMutex);
      s_state.lastErrorCode = -501;
      s_state.lastErrorText = "to_jString failed";
      s_state.lastResult = s_state.lastErrorText;
    }
  }
  return true;
}

uint32_t MakeSyntheticNativeStringArg(const char *text) {
  struct SyntheticCStringRep {
    void **vfTable = nullptr;
    uint32_t refCount = 1;
    int32_t stringHash = 0;
    uint32_t reserved0C = 0;
    const SyntheticCStringRep *nextString = nullptr;
    uint32_t reserved14 = 0;
    uint32_t reserved18 = 0;
    const char *str = "";
  };

  struct SyntheticRCString {
    void **vfTable = nullptr;
    uint32_t refCount = 1;
    const SyntheticCStringRep *stringRep = nullptr;
  };

  struct SyntheticNativeString {
    SyntheticCStringRep rep;
    SyntheticRCString rc;
  };

  static_assert(offsetof(SyntheticRCString, stringRep) == 0x8,
                "SyntheticRCString must match RCString string_rep offset");
  static_assert(offsetof(SyntheticCStringRep, str) == 0x1C,
                "SyntheticCStringRep must match CStringRep str offset");

  thread_local SyntheticNativeString s = {};
  s.rep.str = text ? text : "";
  s.rc.stringRep = &s.rep;
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s.rc));
}

void __cdecl Bridge_Preloader(uint32_t nativeArg);
int __cdecl Bridge_GetLocalizedHotkey(uint32_t nativeArg);
uint32_t __cdecl Bridge_GetLocalizedString(uint32_t nativeArg);
void __cdecl Bridge_SaveInteger(uint32_t table, int32_t parentKey,
                                int32_t childKey, int32_t value);
void __cdecl Bridge_SaveReal(uint32_t table, int32_t parentKey,
                             int32_t childKey, float *value);
int32_t __cdecl Bridge_LoadInteger(uint32_t table, int32_t parentKey,
                                  int32_t childKey);
uint32_t __cdecl Bridge_LoadReal(uint32_t table, int32_t parentKey,
                                 int32_t childKey);

CarrierPatch s_preloader{
    "Preloader", CarrierKind::Command,
    reinterpret_cast<void *>(&Bridge_Preloader)};
CarrierPatch s_hotkey{
    "GetLocalizedHotkey", CarrierKind::IntQuery,
    reinterpret_cast<void *>(&Bridge_GetLocalizedHotkey)};
CarrierPatch s_string{
    "GetLocalizedString", CarrierKind::StringQuery,
    reinterpret_cast<void *>(&Bridge_GetLocalizedString)};
CarrierPatch s_saveInteger{
    "SaveInteger", CarrierKind::TypedSaveInteger,
    reinterpret_cast<void *>(&Bridge_SaveInteger)};
CarrierPatch s_saveReal{
    "SaveReal", CarrierKind::TypedSaveReal,
    reinterpret_cast<void *>(&Bridge_SaveReal)};
CarrierPatch s_loadInteger{
    "LoadInteger", CarrierKind::TypedLoadInteger,
    reinterpret_cast<void *>(&Bridge_LoadInteger)};
CarrierPatch s_loadReal{
    "LoadReal", CarrierKind::TypedLoadReal,
    reinterpret_cast<void *>(&Bridge_LoadReal)};

CarrierPatch *CoreCarriers[] = {&s_preloader, &s_hotkey, &s_string};
CarrierPatch *TypedCarriers[] = {
    &s_saveInteger, &s_saveReal, &s_loadInteger, &s_loadReal};

void CallOriginalVoid(CarrierPatch &patch, uint32_t nativeArg) {
  const auto fn = reinterpret_cast<NativeVoidStringFn>(
      patch.originalFn.load(std::memory_order_relaxed));
  if (fn)
    fn(nativeArg);
}

int CallOriginalInt(CarrierPatch &patch, uint32_t nativeArg) {
  const auto fn = reinterpret_cast<NativeIntStringFn>(
      patch.originalFn.load(std::memory_order_relaxed));
  return fn ? fn(nativeArg) : 0;
}

uint32_t CallOriginalString(CarrierPatch &patch, uint32_t nativeArg) {
  const auto fn = reinterpret_cast<NativeStringStringFn>(
      patch.originalFn.load(std::memory_order_relaxed));
  return fn ? fn(nativeArg) : 0;
}

void CallOriginalSaveInteger(uint32_t table, int32_t parentKey,
                             int32_t childKey, int32_t value) {
  const auto fn = reinterpret_cast<NativeSaveIntegerFn>(
      s_saveInteger.originalFn.load(std::memory_order_relaxed));
  if (fn)
    fn(table, parentKey, childKey, value);
}

void CallOriginalSaveReal(uint32_t table, int32_t parentKey,
                          int32_t childKey, float *value) {
  const auto fn = reinterpret_cast<NativeSaveRealFn>(
      s_saveReal.originalFn.load(std::memory_order_relaxed));
  if (fn)
    fn(table, parentKey, childKey, value);
}

int32_t CallOriginalLoadInteger(uint32_t table, int32_t parentKey,
                                int32_t childKey) {
  const auto fn = reinterpret_cast<NativeLoadIntegerFn>(
      s_loadInteger.originalFn.load(std::memory_order_relaxed));
  return fn ? fn(table, parentKey, childKey) : 0;
}

uint32_t CallOriginalLoadReal(uint32_t table, int32_t parentKey,
                              int32_t childKey) {
  const auto fn = reinterpret_cast<NativeLoadRealFn>(
      s_loadReal.originalFn.load(std::memory_order_relaxed));
  return fn ? fn(table, parentKey, childKey) : 0u;
}

void __cdecl Bridge_Preloader(uint32_t nativeArg) {
  if (!dxvk::war3::internal::kWar3JassCommandBridgeEnabled) {
    CallOriginalVoid(s_preloader, nativeArg);
    return;
  }
  if (TryHandleCarrier(s_preloader, nativeArg, nullptr, nullptr))
    return;
  CallOriginalVoid(s_preloader, nativeArg);
}

int __cdecl Bridge_GetLocalizedHotkey(uint32_t nativeArg) {
  if (!dxvk::war3::internal::kWar3JassCommandBridgeEnabled)
    return CallOriginalInt(s_hotkey, nativeArg);

  int result = 0;
  if (TryHandleCarrier(s_hotkey, nativeArg, &result, nullptr))
    return result;
  return CallOriginalInt(s_hotkey, nativeArg);
}

uint32_t __cdecl Bridge_GetLocalizedString(uint32_t nativeArg) {
  if (!dxvk::war3::internal::kWar3JassCommandBridgeEnabled)
    return CallOriginalString(s_string, nativeArg);

  uint32_t result = 0;
  if (TryHandleCarrier(s_string, nativeArg, nullptr, &result))
    return result;
  return CallOriginalString(s_string, nativeArg);
}

void __cdecl Bridge_SaveInteger(uint32_t table, int32_t parentKey,
                                int32_t childKey, int32_t value) {
  if (dxvk::war3::internal::kWar3JassCommandBridgeEnabled &&
      dxvk::war3::japi::TryTypedSaveInteger(
          table, parentKey, childKey, value)) {
    s_saveInteger.handled.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  s_saveInteger.passedThrough.fetch_add(1, std::memory_order_relaxed);
  CallOriginalSaveInteger(table, parentKey, childKey, value);
}

void __cdecl Bridge_SaveReal(uint32_t table, int32_t parentKey,
                             int32_t childKey, float *value) {
  if (dxvk::war3::internal::kWar3JassCommandBridgeEnabled && value &&
      dxvk::war3::IsReadableRange(value, sizeof(float)) &&
      dxvk::war3::japi::TryTypedSaveReal(
          table, parentKey, childKey, *value)) {
    s_saveReal.handled.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  s_saveReal.passedThrough.fetch_add(1, std::memory_order_relaxed);
  CallOriginalSaveReal(table, parentKey, childKey, value);
}

int32_t __cdecl Bridge_LoadInteger(uint32_t table, int32_t parentKey,
                                  int32_t childKey) {
  int32_t result = 0;
  if (dxvk::war3::internal::kWar3JassCommandBridgeEnabled &&
      dxvk::war3::japi::TryTypedLoadInteger(
          table, parentKey, childKey, result)) {
    s_loadInteger.handled.fetch_add(1, std::memory_order_relaxed);
    return result;
  }
  s_loadInteger.passedThrough.fetch_add(1, std::memory_order_relaxed);
  return CallOriginalLoadInteger(table, parentKey, childKey);
}

uint32_t __cdecl Bridge_LoadReal(uint32_t table, int32_t parentKey,
                                 int32_t childKey) {
  float result = 0.0f;
  if (dxvk::war3::internal::kWar3JassCommandBridgeEnabled &&
      dxvk::war3::japi::TryTypedLoadReal(
          table, parentKey, childKey, result)) {
    s_loadReal.handled.fetch_add(1, std::memory_order_relaxed);
    return EncodeJassRealReturn(result);
  }
  s_loadReal.passedThrough.fetch_add(1, std::memory_order_relaxed);
  return CallOriginalLoadReal(table, parentKey, childKey);
}

struct PreparedCarrier {
  CarrierPatch *patch = nullptr;
  void *entry = nullptr;
  void *currentFn = nullptr;
  const char *signature = nullptr;
  uintptr_t previousEntry = 0u;
  uintptr_t previousOriginalFn = 0u;
  uint32_t returnType = 0u;
  bool alreadyInstalled = false;
};

bool PrepareCarrier(GetTlsJassDataFn lookupFn, CarrierPatch &patch,
                    PreparedCarrier &prepared) {
  if (!lookupFn || !patch.name || !patch.bridgeFn)
    return false;

  void *entry = lookupFn(const_cast<char *>(patch.name));
  if (!entry)
    return false;

  void *funcPtr = nullptr;
  const char *sigPtr = nullptr;
  uint32_t paramCount = 0;
  uint32_t retType = 0;
  if (!ReadNativeMeta(entry, funcPtr, sigPtr, paramCount, retType))
    return false;
  if (paramCount != ExpectedCarrierParameterCount(patch.kind) ||
      !SignatureMatchesCarrier(sigPtr, patch.kind)) {
    war3dbg::Print(
        "DXVK War3JassBridge: skip %s unexpected signature sig=%s argc=%u ret=%u\n",
        patch.name, sigPtr ? sigPtr : "<null>", paramCount, retType);
    return false;
  }

  prepared.patch = &patch;
  prepared.entry = entry;
  prepared.currentFn = funcPtr;
  prepared.signature = sigPtr;
  prepared.previousEntry =
      patch.entry.load(std::memory_order_acquire);
  prepared.previousOriginalFn =
      patch.originalFn.load(std::memory_order_acquire);
  prepared.returnType = retType;
  prepared.alreadyInstalled = funcPtr == patch.bridgeFn;
  if (prepared.alreadyInstalled && prepared.previousOriginalFn == 0u) {
    war3dbg::Print(
        "DXVK War3JassBridge: skip %s bridge is present but original is unknown\n",
        patch.name);
    return false;
  }
  return true;
}

bool TryInstallTypedCarriers(GetTlsJassDataFn lookupFn, const char *reason) {
  if (s_typedInstalled.load(std::memory_order_acquire))
    return true;
  if (s_typedInstallAttempted.exchange(true, std::memory_order_acq_rel))
    return false;

  const uint64_t attempt =
      s_typedInstallAttempts.fetch_add(1, std::memory_order_relaxed) + 1u;
  std::array<PreparedCarrier, 4u> prepared = {};
  bool allOk = true;
  for (size_t index = 0u; index < prepared.size(); ++index) {
    allOk = PrepareCarrier(
                lookupFn, *TypedCarriers[index], prepared[index]) &&
            allOk;
  }

  std::array<bool, 4u> written = {};
  if (allOk) {
    for (size_t index = 0u; index < prepared.size(); ++index) {
      PreparedCarrier &item = prepared[index];
      item.patch->entry.store(
          reinterpret_cast<uintptr_t>(item.entry),
          std::memory_order_relaxed);
      if (item.alreadyInstalled)
        continue;
      item.patch->originalFn.store(
          reinterpret_cast<uintptr_t>(item.currentFn),
          std::memory_order_release);
      if (!WriteNativeFuncPtr(item.entry, item.patch->bridgeFn)) {
        allOk = false;
        break;
      }
      written[index] = true;
    }
  }

  if (!allOk) {
    for (size_t index = 0u; index < prepared.size(); ++index) {
      PreparedCarrier &item = prepared[index];
      if (!item.patch)
        continue;
      if (written[index])
        static_cast<void>(WriteNativeFuncPtr(item.entry, item.currentFn));
      item.patch->entry.store(item.previousEntry,
                              std::memory_order_release);
      item.patch->originalFn.store(item.previousOriginalFn,
                                   std::memory_order_release);
    }
    s_typedInstalled.store(false, std::memory_order_release);
    if (attempt <= 8u || (attempt % 128u) == 0u) {
      war3dbg::Print(
          "DXVK War3JassBridge: optional typed carriers unavailable reason=%s attempt=%llu; string v1 remains active\n",
          reason ? reason : "<unknown>",
          static_cast<unsigned long long>(attempt));
    }
    return false;
  }

  for (PreparedCarrier &item : prepared) {
    if (!item.alreadyInstalled) {
      item.patch->installCount.fetch_add(1, std::memory_order_relaxed);
      war3dbg::Print(
          "DXVK War3JassBridge: installed typed carrier %s entry=%p original=%p bridge=%p sig=%s ret=%u\n",
          item.patch->name, item.entry, item.currentFn,
          item.patch->bridgeFn, item.signature, item.returnType);
    }
  }
  s_typedInstalled.store(true, std::memory_order_release);
  const uint64_t success =
      s_typedInstallSuccesses.fetch_add(1, std::memory_order_relaxed) + 1u;
  war3dbg::Print(
      "DXVK War3JassBridge: typed data plane installed reason=%s attempt=%llu success=%llu\n",
      reason ? reason : "<unknown>",
      static_cast<unsigned long long>(attempt),
      static_cast<unsigned long long>(success));
  return true;
}

} // namespace

void ConfigureJassCommandBridge(GetTlsJassDataFn lookupFn) {
  s_lookupFn.store(reinterpret_cast<uintptr_t>(lookupFn),
                   std::memory_order_relaxed);
  s_allInstalled.store(false, std::memory_order_relaxed);
  s_typedInstalled.store(false, std::memory_order_relaxed);
  s_typedInstallAttempted.store(false, std::memory_order_relaxed);
  dxvk::war3::japi::ResetTypedTransport();
}

void ResetJassCommandBridgeInstallState() {
  std::lock_guard<std::mutex> lock(s_installMutex);
  s_allInstalled.store(false, std::memory_order_release);
  s_typedInstalled.store(false, std::memory_order_release);
  s_typedInstallAttempted.store(false, std::memory_order_release);
  dxvk::war3::japi::ResetTypedTransport();
  for (CarrierPatch *patch : CoreCarriers) {
    patch->entry.store(0, std::memory_order_relaxed);
    // Preserve the proven stock target. Reset can run while an existing table
    // is still patched; clearing it here would make exact forwarding
    // impossible until the game happened to publish a fresh entry.
  }
  for (CarrierPatch *patch : TypedCarriers) {
    patch->entry.store(0, std::memory_order_relaxed);
    // Preserve the proven stock target. Reset can run while an existing table
    // is still patched; clearing it here would make exact forwarding
    // impossible until the game happened to publish a fresh entry.
  }
}

void TryInstallJassCommandBridge(const char *reason) {
  if constexpr (!dxvk::war3::internal::kWar3JassCommandBridgeEnabled) {
    return;
  }

  if (s_allInstalled.load(std::memory_order_acquire) &&
      s_typedInstalled.load(std::memory_order_acquire))
    return;

  const auto lookupFn = reinterpret_cast<GetTlsJassDataFn>(
      s_lookupFn.load(std::memory_order_relaxed));
  if (!lookupFn)
    return;

  std::lock_guard<std::mutex> lock(s_installMutex);
  if (s_allInstalled.load(std::memory_order_relaxed)) {
    static_cast<void>(TryInstallTypedCarriers(lookupFn, reason));
    return;
  }

  const uint64_t attempt =
      s_installAttempts.fetch_add(1, std::memory_order_relaxed) + 1;

  std::array<PreparedCarrier, 3u> prepared = {};
  bool allOk = true;
  for (size_t index = 0u; index < prepared.size(); ++index) {
    allOk = PrepareCarrier(
                lookupFn, *CoreCarriers[index], prepared[index]) &&
            allOk;
  }

  std::array<bool, 3u> written = {};
  if (allOk) {
    for (size_t index = 0u; index < prepared.size(); ++index) {
      PreparedCarrier &item = prepared[index];
      item.patch->entry.store(
          reinterpret_cast<uintptr_t>(item.entry),
          std::memory_order_relaxed);
      if (item.alreadyInstalled)
        continue;

      item.patch->originalFn.store(
          reinterpret_cast<uintptr_t>(item.currentFn),
          std::memory_order_release);
      if (!WriteNativeFuncPtr(item.entry, item.patch->bridgeFn)) {
        allOk = false;
        break;
      }
      written[index] = true;
    }
  }

  if (!allOk) {
    // Restore both the game slots and the published bridge state. A carrier
    // that was already installed before this transaction remains untouched.
    for (size_t index = 0u; index < prepared.size(); ++index) {
      PreparedCarrier &item = prepared[index];
      if (!item.patch)
        continue;
      if (written[index])
        static_cast<void>(WriteNativeFuncPtr(
            item.entry, item.currentFn));
      item.patch->entry.store(item.previousEntry,
                              std::memory_order_release);
      item.patch->originalFn.store(item.previousOriginalFn,
                                   std::memory_order_release);
    }
  } else {
    for (size_t index = 0u; index < prepared.size(); ++index) {
      PreparedCarrier &item = prepared[index];
      if (!item.alreadyInstalled) {
        item.patch->installCount.fetch_add(1, std::memory_order_relaxed);
        war3dbg::Print(
            "DXVK War3JassBridge: installed carrier %s entry=%p original=%p bridge=%p sig=%s ret=%u\n",
            item.patch->name, item.entry, item.currentFn,
            item.patch->bridgeFn, item.signature, item.returnType);
      }
    }
  }

  if (allOk) {
    s_allInstalled.store(true, std::memory_order_release);
    const uint64_t success =
        s_installSuccesses.fetch_add(1, std::memory_order_relaxed) + 1;
    war3dbg::Print(
        "DXVK War3JassBridge: all carriers installed reason=%s attempt=%llu success=%llu\n",
        reason ? reason : "<unknown>",
        static_cast<unsigned long long>(attempt),
        static_cast<unsigned long long>(success));
    static_cast<void>(TryInstallTypedCarriers(lookupFn, reason));
  } else if (attempt <= 8 || (attempt % 128) == 0) {
    war3dbg::Print(
        "DXVK War3JassBridge: carrier install incomplete reason=%s attempt=%llu preloader=%p hotkey=%p string=%p\n",
        reason ? reason : "<unknown>",
        static_cast<unsigned long long>(attempt),
        reinterpret_cast<void *>(s_preloader.entry.load(std::memory_order_relaxed)),
        reinterpret_cast<void *>(s_hotkey.entry.load(std::memory_order_relaxed)),
        reinterpret_cast<void *>(s_string.entry.load(std::memory_order_relaxed)));
  }
}

bool IsJassCommandBridgeInstalled() {
  return s_allInstalled.load(std::memory_order_acquire);
}

bool IsJassTypedTransportInstalled() {
  return s_typedInstalled.load(std::memory_order_acquire);
}

JassCommandBridgeSelfTestResult RunJassCommandBridgeSelfTest(bool displayText) {
  JassCommandBridgeSelfTestResult result = {};

  TryInstallJassCommandBridge("selftest");
  result.installed = IsJassCommandBridgeInstalled();
  result.typedTransportInstalled = IsJassTypedTransportInstalled();
  if (!result.installed) {
    result.error = "bridge carriers are not installed";
    return result;
  }

  auto *preEntry = reinterpret_cast<void *>(
      s_preloader.entry.load(std::memory_order_relaxed));
  auto *hotkeyEntry =
      reinterpret_cast<void *>(s_hotkey.entry.load(std::memory_order_relaxed));
  auto *stringEntry =
      reinterpret_cast<void *>(s_string.entry.load(std::memory_order_relaxed));

  const auto preloaderFn =
      reinterpret_cast<NativeVoidStringFn>(ReadCurrentNativeFunc(preEntry));
  const auto hotkeyFn =
      reinterpret_cast<NativeIntStringFn>(ReadCurrentNativeFunc(hotkeyEntry));
  const auto stringFn = reinterpret_cast<NativeStringStringFn>(
      ReadCurrentNativeFunc(stringEntry));

  if (!preloaderFn || !hotkeyFn || !stringFn) {
    result.error = "failed to read carrier native function pointers";
    return result;
  }

  preloaderFn(MakeSyntheticNativeStringArg("warvk:log:selftest-from-native"));
  result.preloaderOk = true;

  result.pingCode = hotkeyFn(MakeSyntheticNativeStringArg("warvk:ping"));
  result.intQueryOk = result.pingCode == 1;

  result.versionStringHandle =
      stringFn(MakeSyntheticNativeStringArg("warvk:version"));
  if (result.versionStringHandle != 0) {
    const char *text = jass::convert::to_CString(result.versionStringHandle);
    result.versionText = text ? text : "";
    result.stringQueryOk = result.versionText.find("WarVK JASS bridge") !=
                           std::string::npos;
  }

  // Exercise the exact public v1 path through all three patched stock
  // carriers. This is the runtime witness used before editing a map.
  preloaderFn(MakeSyntheticNativeStringArg("warvk:v1;system.clearError"));
  result.publicProtocolVersion = hotkeyFn(
      MakeSyntheticNativeStringArg("warvk:v1;system.protocolVersion"));
  result.publicVersionStringHandle = stringFn(
      MakeSyntheticNativeStringArg("warvk:v1;system.version"));
  if (result.publicVersionStringHandle != 0u) {
    const char *text =
        jass::convert::to_CString(result.publicVersionStringHandle);
    result.publicVersionText = text ? text : "";
  }
  result.publicV1Ok =
      result.publicProtocolVersion == 1 &&
      result.publicVersionText.find("WarVK JAPI 1.2003") !=
          std::string::npos;

  if (displayText) {
    result.displayTextAttempted = true;
    void *playerEntry = LookupNativeEntry("Player");
    void *localPlayerEntry = LookupNativeEntry("GetLocalPlayer");
    void *displayEntry = LookupNativeEntry("DisplayTextToPlayer");
    void *timedDisplayEntry = LookupNativeEntry("DisplayTimedTextToPlayer");
    const auto playerFn =
        reinterpret_cast<NativePlayerFn>(ReadCurrentNativeFunc(playerEntry));
    const auto localPlayerFn = reinterpret_cast<NativeGetLocalPlayerFn>(
        ReadCurrentNativeFunc(localPlayerEntry));
    const auto displayFn = reinterpret_cast<NativeDisplayTextToPlayerFn>(
        ReadCurrentNativeFunc(displayEntry));
    const auto timedDisplayFn = reinterpret_cast<NativeDisplayTimedTextToPlayerFn>(
        ReadCurrentNativeFunc(timedDisplayEntry));
    if ((!playerFn && !localPlayerFn) || (!displayFn && !timedDisplayFn)) {
      result.error =
          "failed to resolve player getter or DisplayTextToPlayer/DisplayTimedTextToPlayer";
      return result;
    }

    result.playerHandle = localPlayerFn ? localPlayerFn() : 0;
    if (!result.playerHandle && playerFn)
      result.playerHandle = playerFn(0);
    if (!result.playerHandle) {
      result.error = "GetLocalPlayer/Player(0) returned null handle";
      return result;
    }

    float x = 0.02f;
    float y = 0.18f;
    uint32_t messageArg = MakeSyntheticNativeStringArg(
        "WarVK direct native call OK: DisplayTimedTextToPlayer");
    if (timedDisplayFn) {
      float duration = 60.0f;
      timedDisplayFn(result.playerHandle, &x, &y, &duration, messageArg);
    } else {
      displayFn(result.playerHandle, &x, &y, messageArg);
    }
    result.displayTextOk = true;
  }

  if (!result.intQueryOk || !result.stringQueryOk || !result.publicV1Ok) {
    result.error = "carrier selftest did not return expected values";
  }
  return result;
}

JassPauseGameTestResult SetJassGamePausedForTest(bool paused) {
  JassPauseGameTestResult result = {};
  result.paused = paused;

  TryInstallJassCommandBridge("pause-game-test");
  void *entry = LookupNativeEntry("PauseGame");
  if (!entry) {
    result.error = "PauseGame native entry was not found";
    return result;
  }
  result.resolved = true;

  void *funcPtr = nullptr;
  const char *signature = nullptr;
  if (!ReadNativeMeta(entry, funcPtr, signature, result.paramCount,
                      result.returnType)) {
    result.error = "PauseGame native metadata is unreadable";
    return result;
  }

  if (signature && dxvk::war3::IsReadableRange(signature, 4u)) {
    char copy[16] = {};
    size_t length = 0u;
    while (length + 1u < sizeof(copy) &&
           dxvk::war3::IsReadableRange(signature + length, 1u) &&
           signature[length] != '\0') {
      copy[length] = signature[length];
      ++length;
    }
    result.signature.assign(copy, length);
  }

  // Native metadata uses the JASS signature "(B)V" for
  // PauseGame(boolean). Fail closed instead of calling a table entry with an
  // ABI that does not match the one-argument cdecl contract.
  result.signatureValidated =
      result.paramCount == 1u && result.signature.size() >= 4u &&
      result.signature[0] == '(' && result.signature[1] == 'B' &&
      result.signature[2] == ')' && result.signature[3] == 'V';
  if (!result.signatureValidated) {
    result.error = "PauseGame native signature mismatch";
    return result;
  }

  const auto pauseFn = reinterpret_cast<NativeVoidBooleanFn>(funcPtr);
  if (!pauseFn) {
    result.error = "PauseGame native function pointer is null";
    return result;
  }

  const bool previousBypass =
      SetInternalTestGamePauseBypassForCurrentThread(true);
  pauseFn(paused ? 1u : 0u);
  SetInternalTestGamePauseBypassForCurrentThread(previousBypass);
  result.invoked = true;
  return result;
}

JassCameraFieldTestResult SetJassCameraAngleOfAttackForTest(
    float angleDegrees) {
  JassCameraFieldTestResult result = {};
  result.angleDegrees = angleDegrees;

  if (!std::isfinite(angleDegrees)) {
    result.error = "camera angle is not finite";
    return result;
  }

  TryInstallJassCommandBridge("camera-angle-test");
  void *convertEntry = LookupNativeEntry("ConvertCameraField");
  void *setEntry = LookupNativeEntry("SetCameraField");
  result.convertResolved = convertEntry != nullptr;
  result.setResolved = setEntry != nullptr;
  if (!result.convertResolved || !result.setResolved) {
    result.error =
        "ConvertCameraField or SetCameraField native entry was not found";
    return result;
  }

  void *convertPtr = nullptr;
  void *setPtr = nullptr;
  const char *convertSignature = nullptr;
  const char *setSignature = nullptr;
  if (!ReadNativeMeta(convertEntry, convertPtr, convertSignature,
                      result.convertParamCount, result.convertReturnType) ||
      !ReadNativeMeta(setEntry, setPtr, setSignature, result.setParamCount,
                      result.setReturnType)) {
    result.error = "camera native metadata is unreadable";
    return result;
  }

  CopyNativeSignature(convertSignature, result.convertSignature);
  CopyNativeSignature(setSignature, result.setSignature);
  const bool convertSignatureValid =
      result.convertParamCount == 1u &&
      result.convertSignature.rfind("(I)", 0u) == 0u &&
      result.convertSignature.find("camerafield") != std::string::npos;
  const bool setSignatureValid =
      result.setParamCount == 3u &&
      result.setSignature.rfind("(Hcamerafield;RR)", 0u) == 0u &&
      result.setSignature.size() >= 2u &&
      result.setSignature.back() == 'V';
  result.signaturesValidated =
      convertSignatureValid && setSignatureValid;
  if (!result.signaturesValidated) {
    result.error = "camera native signature mismatch";
    return result;
  }

  const auto convertFn =
      reinterpret_cast<NativeConvertCameraFieldFn>(convertPtr);
  const auto setFn = reinterpret_cast<NativeSetCameraFieldFn>(setPtr);
  if (!convertFn || !setFn) {
    result.error = "camera native function pointer is null";
    return result;
  }

  // common.j: CAMERA_FIELD_ANGLE_OF_ATTACK = ConvertCameraField(2).
  result.cameraFieldHandle = convertFn(2);
  if (result.cameraFieldHandle == 0u) {
    result.error = "ConvertCameraField(2) returned a null handle";
    return result;
  }

  float angle = angleDegrees;
  float duration = 0.0f;
  setFn(result.cameraFieldHandle, &angle, &duration);
  result.invoked = true;
  return result;
}

JassFixedCameraTestResult SetJassFixedCameraForTest(
    float targetX, float targetY, float targetDistance, float angleDegrees,
    float rotationDegrees, float fieldOfViewDegrees, float farZ,
    float rollDegrees, float zOffset) {
  JassFixedCameraTestResult result = {};
  result.targetX = targetX;
  result.targetY = targetY;
  result.targetDistance = targetDistance;
  result.angleDegrees = angleDegrees;
  result.rotationDegrees = rotationDegrees;
  result.fieldOfViewDegrees = fieldOfViewDegrees;
  result.farZ = farZ;
  result.rollDegrees = rollDegrees;
  result.zOffset = zOffset;

  const float values[] = {
      targetX, targetY, targetDistance, angleDegrees, rotationDegrees,
      fieldOfViewDegrees, farZ, rollDegrees, zOffset,
  };
  for (const float value : values) {
    if (!std::isfinite(value)) {
      result.error = "fixed camera input is not finite";
      return result;
    }
  }
  if (targetDistance <= 0.0f || fieldOfViewDegrees <= 0.0f ||
      farZ <= 0.0f) {
    result.error = "fixed camera distance/FOV/FarZ must be positive";
    return result;
  }

  TryInstallJassCommandBridge("fixed-camera-test");
  void *convertEntry = LookupNativeEntry("ConvertCameraField");
  void *setEntry = LookupNativeEntry("SetCameraField");
  void *positionEntry = LookupNativeEntry("SetCameraPosition");
  result.convertResolved = convertEntry != nullptr;
  result.setResolved = setEntry != nullptr;
  result.positionResolved = positionEntry != nullptr;
  if (!result.convertResolved || !result.setResolved ||
      !result.positionResolved) {
    result.error = "fixed camera native entry was not found";
    return result;
  }

  void *convertPtr = nullptr;
  void *setPtr = nullptr;
  void *positionPtr = nullptr;
  const char *convertSignature = nullptr;
  const char *setSignature = nullptr;
  const char *positionSignature = nullptr;
  if (!ReadNativeMeta(convertEntry, convertPtr, convertSignature,
                      result.convertParamCount, result.convertReturnType) ||
      !ReadNativeMeta(setEntry, setPtr, setSignature, result.setParamCount,
                      result.setReturnType) ||
      !ReadNativeMeta(positionEntry, positionPtr, positionSignature,
                      result.positionParamCount, result.positionReturnType)) {
    result.error = "fixed camera native metadata is unreadable";
    return result;
  }

  CopyNativeSignature(convertSignature, result.convertSignature);
  CopyNativeSignature(setSignature, result.setSignature);
  CopyNativeSignature(positionSignature, result.positionSignature);
  const bool convertSignatureValid =
      result.convertParamCount == 1u &&
      result.convertSignature.rfind("(I)", 0u) == 0u &&
      result.convertSignature.find("camerafield") != std::string::npos;
  const bool setSignatureValid =
      result.setParamCount == 3u &&
      result.setSignature.rfind("(Hcamerafield;RR)", 0u) == 0u &&
      result.setSignature.size() >= 2u &&
      result.setSignature.back() == 'V';
  const bool positionSignatureValid =
      result.positionParamCount == 2u &&
      result.positionSignature == "(RR)V";
  result.signaturesValidated =
      convertSignatureValid && setSignatureValid && positionSignatureValid;
  if (!result.signaturesValidated) {
    result.error = "fixed camera native signature mismatch";
    return result;
  }

  const auto convertFn =
      reinterpret_cast<NativeConvertCameraFieldFn>(convertPtr);
  const auto setFn = reinterpret_cast<NativeSetCameraFieldFn>(setPtr);
  const auto positionFn =
      reinterpret_cast<NativeSetCameraPositionFn>(positionPtr);
  if (!convertFn || !setFn || !positionFn) {
    result.error = "fixed camera native function pointer is null";
    return result;
  }

  // common.j camera fields 0..6:
  // target distance, far Z, angle of attack, FOV, roll, rotation, Z offset.
  for (int32_t index = 0; index < 7; ++index) {
    result.cameraFieldHandles[index] = convertFn(index);
    // Camera-field handles are enum values, not pointers. Field zero is the
    // valid CAMERA_FIELD_TARGET_DISTANCE value; only a zero result for a
    // non-zero enum input proves that conversion failed.
    if (index != 0 && result.cameraFieldHandles[index] == 0u) {
      result.error = "ConvertCameraField returned a null handle";
      return result;
    }
  }

  float x = targetX;
  float y = targetY;
  positionFn(&x, &y);
  result.positionInvoked = true;

  float duration = 0.0f;
  float fieldValues[] = {
      targetDistance,
      farZ,
      angleDegrees,
      fieldOfViewDegrees,
      rollDegrees,
      rotationDegrees,
      zOffset,
  };
  for (uint32_t index = 0u; index < 7u; ++index) {
    setFn(result.cameraFieldHandles[index], &fieldValues[index], &duration);
    ++result.fieldInvocations;
  }
  result.invoked = result.positionInvoked && result.fieldInvocations == 7u;
  return result;
}

JassCameraSnapshotTestResult SnapshotJassCameraForTest() {
  JassCameraSnapshotTestResult result = {};
  TryInstallJassCommandBridge("camera-snapshot-test");
  std::lock_guard<std::mutex> lock(s_internalTestNativeMutex);

  ResolvedCameraTestNatives natives = {};
  if (!ResolveCameraSnapshotNatives(natives, result.error))
    return result;
  result.resolved = true;
  result.signaturesValidated = true;

  for (int32_t index = 0; index < 7; ++index) {
    result.cameraFieldHandles[index] = natives.convert(index);
    if (index != 0 && result.cameraFieldHandles[index] == 0u) {
      result.error = "ConvertCameraField returned a null handle";
      return result;
    }
  }

  result.targetX = DecodeJassRealReturn(natives.getTargetX());
  result.targetY = DecodeJassRealReturn(natives.getTargetY());
  result.targetZ = DecodeJassRealReturn(natives.getTargetZ());
  float fields[7] = {};
  for (uint32_t index = 0u; index < 7u; ++index) {
    fields[index] = DecodeJassRealReturn(
        natives.getField(result.cameraFieldHandles[index]));
    ++result.fieldInvocations;
  }
  const float values[] = {
      result.targetX, result.targetY, result.targetZ,
      fields[0], fields[1], fields[2], fields[3],
      fields[4], fields[5], fields[6],
  };
  for (const float value : values) {
    if (!std::isfinite(value)) {
      result.error = "camera snapshot returned a non-finite value";
      return result;
    }
  }

  // Warcraft's getter exposes angular camera fields in radians while
  // SetCameraField consumes degrees.  Keep the private AutoTest snapshot/apply
  // contract round-trippable by publishing every angular field in the setter's
  // unit.  Passing the getter bits back unchanged places the camera below the
  // terrain (5.3058 radians was interpreted as 5.3058 degrees instead of the
  // intended 304 degrees).
  constexpr float kRadiansToDegrees =
      57.295779513082320876798154814105f;
  result.targetDistance = fields[0];
  result.farZ = fields[1];
  result.angleOfAttack = fields[2] * kRadiansToDegrees;
  result.fieldOfView = fields[3] * kRadiansToDegrees;
  result.roll = fields[4] * kRadiansToDegrees;
  result.rotation = fields[5] * kRadiansToDegrees;
  result.zOffset = fields[6];
  result.invoked = result.fieldInvocations == 7u;
  return result;
}

JassCameraApplyTestResult ApplyJassCameraForTest(
    float targetX, float targetY, float targetDistance, float angleOfAttack,
    float rotation, float fieldOfView, float farZ, float roll, float zOffset,
    float duration, bool quickPosition) {
  JassCameraApplyTestResult result = {};
  result.targetX = targetX;
  result.targetY = targetY;
  result.duration = duration;
  result.quickPosition = quickPosition;
  const float values[] = {
      targetX, targetY, targetDistance, angleOfAttack, rotation,
      fieldOfView, farZ, roll, zOffset, duration,
  };
  for (const float value : values) {
    if (!std::isfinite(value)) {
      result.error = "camera apply input is not finite";
      return result;
    }
  }
  if (targetDistance <= 0.0f || fieldOfView <= 0.0f || farZ <= 0.0f ||
      duration < 0.0f) {
    result.error = "camera distance/FOV/FarZ/duration is invalid";
    return result;
  }

  TryInstallJassCommandBridge("camera-apply-test");
  std::lock_guard<std::mutex> lock(s_internalTestNativeMutex);
  ResolvedCameraTestNatives natives = {};
  if (!ResolveCameraApplyNatives(natives, quickPosition, !quickPosition,
                                 result.error))
    return result;
  result.resolved = true;
  result.signaturesValidated = true;

  uint32_t fieldHandles[7] = {};
  for (int32_t index = 0; index < 7; ++index) {
    fieldHandles[index] = natives.convert(index);
    if (index != 0 && fieldHandles[index] == 0u) {
      result.error = "ConvertCameraField returned a null handle";
      return result;
    }
  }

  float x = targetX;
  float y = targetY;
  float transition = duration;
  if (quickPosition)
    natives.position(&x, &y);
  else
    natives.pan(&x, &y, &transition);
  result.positionInvoked = true;

  float fieldValues[7] = {
      targetDistance, farZ, angleOfAttack, fieldOfView,
      roll, rotation, zOffset,
  };
  for (uint32_t index = 0u; index < 7u; ++index) {
    float fieldTransition = duration;
    natives.set(fieldHandles[index], &fieldValues[index], &fieldTransition);
    ++result.fieldInvocations;
  }
  result.invoked = result.positionInvoked && result.fieldInvocations == 7u;
  return result;
}

JassCameraPanTestResult PanJassCameraForTest(
    float targetX, float targetY, float duration) {
  JassCameraPanTestResult result = {};
  result.targetX = targetX;
  result.targetY = targetY;
  result.duration = duration;
  if (!std::isfinite(targetX) || !std::isfinite(targetY) ||
      !std::isfinite(duration) || duration < 0.0f) {
    result.error = "camera pan input is invalid";
    return result;
  }

  TryInstallJassCommandBridge("camera-pan-test");
  std::lock_guard<std::mutex> lock(s_internalTestNativeMutex);
  void *funcPtr = nullptr;
  if (!ResolveNativeStrict("PanCameraToTimed", "(RRR)V", 3u, funcPtr,
                           result.signature, result.error))
    return result;
  result.resolved = true;
  result.signatureValidated = true;
  float x = targetX;
  float y = targetY;
  float seconds = duration;
  reinterpret_cast<NativePanCameraToTimedFn>(funcPtr)(&x, &y, &seconds);
  result.invoked = true;
  return result;
}

JassWorldBoundsTestResult QueryJassWorldBoundsForTest() {
  JassWorldBoundsTestResult result = {};
  TryInstallJassCommandBridge("world-bounds-test");
  std::lock_guard<std::mutex> lock(s_internalTestNativeMutex);

  void *worldBoundsPtr = nullptr;
  void *minXPtr = nullptr;
  void *minYPtr = nullptr;
  void *maxXPtr = nullptr;
  void *maxYPtr = nullptr;
  std::string signature;
  if (!ResolveNativeStrict("GetWorldBounds", "()Hrect;", 0u,
                           worldBoundsPtr, signature, result.error) ||
      !ResolveNativeStrict("GetRectMinX", "(Hrect;)R", 1u, minXPtr,
                           signature, result.error) ||
      !ResolveNativeStrict("GetRectMinY", "(Hrect;)R", 1u, minYPtr,
                           signature, result.error) ||
      !ResolveNativeStrict("GetRectMaxX", "(Hrect;)R", 1u, maxXPtr,
                           signature, result.error) ||
      !ResolveNativeStrict("GetRectMaxY", "(Hrect;)R", 1u, maxYPtr,
                           signature, result.error))
    return result;
  result.resolved = true;
  result.signaturesValidated = true;

  result.rectHandle = reinterpret_cast<NativeHandleVoidFn>(worldBoundsPtr)();
  if (result.rectHandle == 0u) {
    result.error = "GetWorldBounds returned a null rect";
    return result;
  }
  result.minX = DecodeJassRealReturn(
      reinterpret_cast<NativeRealHandleFn>(minXPtr)(result.rectHandle));
  result.minY = DecodeJassRealReturn(
      reinterpret_cast<NativeRealHandleFn>(minYPtr)(result.rectHandle));
  result.maxX = DecodeJassRealReturn(
      reinterpret_cast<NativeRealHandleFn>(maxXPtr)(result.rectHandle));
  result.maxY = DecodeJassRealReturn(
      reinterpret_cast<NativeRealHandleFn>(maxYPtr)(result.rectHandle));
  if (!std::isfinite(result.minX) || !std::isfinite(result.minY) ||
      !std::isfinite(result.maxX) || !std::isfinite(result.maxY) ||
      result.minX >= result.maxX || result.minY >= result.maxY ||
      std::abs(result.minX) > 1000000.0f ||
      std::abs(result.minY) > 1000000.0f ||
      std::abs(result.maxX) > 1000000.0f ||
      std::abs(result.maxY) > 1000000.0f) {
    result.error = "GetWorldBounds returned an invalid rect";
    return result;
  }
  result.invoked = true;
  return result;
}

JassVisibilityTestResult SetJassFullMapVisibilityForTest(bool enabled) {
  JassVisibilityTestResult result = {};
  result.enabled = enabled;
  TryInstallJassCommandBridge("full-map-visibility-test");
  std::lock_guard<std::mutex> lock(s_internalTestNativeMutex);

  void *fogEnablePtr = nullptr;
  void *fogMaskEnablePtr = nullptr;
  void *isFogEnabledPtr = nullptr;
  void *isFogMaskEnabledPtr = nullptr;
  std::string signature;
  if (!ResolveNativeStrict("FogEnable", "(B)V", 1u, fogEnablePtr,
                           signature, result.error) ||
      !ResolveNativeStrict("FogMaskEnable", "(B)V", 1u, fogMaskEnablePtr,
                           signature, result.error) ||
      !ResolveNativeStrict("IsFogEnabled", "()B", 0u, isFogEnabledPtr,
                           signature, result.error) ||
      !ResolveNativeStrict("IsFogMaskEnabled", "()B", 0u,
                           isFogMaskEnabledPtr, signature, result.error))
    return result;
  result.resolved = true;
  result.signaturesValidated = true;

  const auto fogEnable = reinterpret_cast<NativeVoidBooleanFn>(fogEnablePtr);
  const auto fogMaskEnable =
      reinterpret_cast<NativeVoidBooleanFn>(fogMaskEnablePtr);
  const auto isFogEnabled =
      reinterpret_cast<NativeBooleanVoidFn>(isFogEnabledPtr);
  const auto isFogMaskEnabled =
      reinterpret_cast<NativeBooleanVoidFn>(isFogMaskEnabledPtr);
  result.fogBefore = isFogEnabled() != 0u;
  result.fogMaskBefore = isFogMaskEnabled() != 0u;

  if (enabled) {
    if (!s_visibilityLease.active) {
      s_visibilityLease.active = true;
      s_visibilityLease.fogEnabled = result.fogBefore;
      s_visibilityLease.fogMaskEnabled = result.fogMaskBefore;
      result.capturedOriginal = true;
    }
    fogEnable(0u);
    fogMaskEnable(0u);
  } else if (s_visibilityLease.active) {
    fogEnable(s_visibilityLease.fogEnabled ? 1u : 0u);
    fogMaskEnable(s_visibilityLease.fogMaskEnabled ? 1u : 0u);
    s_visibilityLease = {};
  }

  result.fogAfter = isFogEnabled() != 0u;
  result.fogMaskAfter = isFogMaskEnabled() != 0u;
  result.leaseActive = s_visibilityLease.active;
  result.invoked = enabled
      ? (!result.fogAfter && !result.fogMaskAfter)
      : !result.leaseActive;
  if (!result.invoked)
    result.error = "fog/fog-mask state did not reach the requested state";
  return result;
}

} // namespace dxvk::war3::hooks
