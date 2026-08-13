#include "war3_japi_v1.h"

#if !defined(WARVK_JAPI_PROTOCOL_TEST)
#include "../../d3d9_war3_light.h"
#include "../../d3d9_war3_settings.h"
#ifndef WAR3_SHADER_API_INTERNAL
#define WAR3_SHADER_API_INTERNAL 1
#endif
#include "../../war3_shader_api.h"
#include "../math/war3_curve_runtime.h"
#include "../render/war3_lightning_runtime.h"
#include "../state/war3_render_state.h"
#include "../war3.h"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dxvk::war3::japi {
namespace {

constexpr std::string_view kProtocolPrefix = "warvk:";
constexpr std::string_view kCanonicalVersion = "v1";
constexpr std::string_view kApiVersion = "WarVK JAPI 1.2003";
constexpr size_t kMaximumMessageBytes = 512u;
constexpr size_t kMaximumArgumentCount = 16u;

constexpr uint32_t kFeatureSun = 0x00000001u;
constexpr uint32_t kFeatureCsm = 0x00000002u;
constexpr uint32_t kFeaturePointLight = 0x00000004u;
constexpr uint32_t kFeatureVolumetric = 0x00000008u;
constexpr uint32_t kFeatureDayNight = 0x00000100u;
constexpr uint32_t kFeatureLightning = 0x00000200u;
constexpr uint32_t kFeatureManagedObject = 0x00000400u;
constexpr uint32_t kFeatureTime = 0x00000800u;
constexpr uint32_t kFeatureStats = 0x00001000u;
constexpr uint32_t kFeatureMathCurve = 0x00002000u;
constexpr uint32_t kFeaturePolylineCurve = 0x00004000u;
constexpr uint32_t kFeatureLocalFog = 0x00008000u;
constexpr uint32_t kImplementedFeatureMask =
    kFeatureSun | kFeatureCsm | kFeaturePointLight | kFeatureVolumetric |
    kFeatureDayNight | kFeatureLightning | kFeatureManagedObject |
    kFeatureTime | kFeatureStats | kFeatureMathCurve |
    kFeaturePolylineCurve | kFeatureLocalFog;

enum class WireType : uint8_t {
  Bool,
  I32,
  Id,
  Real,
  String,
};

enum class CommandId : uint16_t {
  SystemVersion = 0,
  SystemProtocolVersion,
  SystemLastErrorCode,
  SystemLastError,
  SystemClearError,
  SystemFeatureFlags,
  SystemRuntimeReady,
  SunSetEnabled,
  SunSetDirection,
  SunSetColorIntensity,
  CsmSetEnabled,
  CsmSetLayout,
  CsmSetTuning,
  PointLightCreate,
  PointLightDestroy,
  PointLightSetEnabled,
  PointLightSetPosition,
  PointLightSetColorIntensity,
  PointLightSetRadius,
  PointLightSetShadowEnabled,
  PointLightSetShadowConfig,
  PointLightIsAlive,
  VolumetricSetEnabled,
  VolumetricSetGlobalMediumEnabled,
  VolumetricSetDensity,
  VolumetricSetScattering,
  VolumetricSetQuality,
  VolumetricSetBackend,
  VolumetricFogSetEnabled,
  VolumetricFogSetSettings,
  LocalFogCreateSphere,
  LocalFogCreateBox,
  LocalFogCreateCylinder,
  LocalFogDestroy,
  LocalFogSetEnabled,
  LocalFogSetPosition,
  LocalFogSetRotation,
  LocalFogSetDensity,
  LocalFogSetEdgeFeather,
  LocalFogSetSphereRadius,
  LocalFogSetBoxSize,
  LocalFogSetCylinderSize,
  LocalFogIsAlive,
  OutlineSetEnabled,
  OutlineSetColor,
  OutlineSetParameters,
  BloomSetEnabled,
  BloomSetParameters,
  BloomSetRadius,
  PostfxSetEnabled,
  PostfxSetExposureGamma,
  PostfxSetColorGrade,
  AaSetMode,
  AaSetSharpness,
  DayNightSetEnabled,
  DayNightSetTime,
  DayNightSetSpeed,
  LightingClockSetMode,
  LightingClockHoldTime,
  LightingClockSetDayDuration,
  LightingCycleSetCelestialMotionEnabled,
  LightingCycleSetTimeColorGradingEnabled,
  LightingCycleSetColorTemperatureProfile,
  LightingCycleResetColorTemperatureProfile,
  MathProgramCompile,
  MathProgramDestroy,
  MathProgramIsAlive,
  MathProgramLastError,
  MathEvaluateReal,
  MathEvaluateInteger,
  CurveCreate,
  CurveDestroy,
  CurveSetReal,
  CurveSetCoordinateMode,
  CurveSetEndpointLocks,
  CurveEvaluateComponent,
  CurveDerivativeComponent,
  CurveArcLength,
  CurvePointCreate,
  CurvePointAppend4,
  CurvePointFinalize,
  LightningCreate,
  LightningDestroy,
  LightningSetEnabled,
  LightningSetEndpoints,
  LightningSetColor,
  LightningSetWidth,
  LightningIsAlive,
  LightningTemplateCreate,
  LightningTemplateSetBasic,
  LightningTemplateSetAdvanced,
  LightningTemplateSetOptional,
  LightningTemplateFinalize,
  LightningCreateFromTemplate,
  LightningCreatePolylineFromTemplate,
  LightningTemplateSetFormulaCurve,
  LightningSetFormulaCurve,
  LightningSetPolylineCurve,
  ManagedObjectCount,
  ManagedObjectIsAlive,
  ManagedObjectType,
  TimeVisualSeconds,
  TimeFrameIndex,
  StatsFramesPerSecond,
  StatsFrameTimeMilliseconds,
  StatsDrawCallCount,
};

struct CommandSpec {
  CommandId id;
  const char* name;
  Carrier carrier;
  const char* argumentTypes;
  uint32_t featureMask;
  bool backendRequired;
};

constexpr std::array<CommandSpec, 106> kCommands = {{
    {CommandId::SystemVersion, "system.version", Carrier::LocalizedString, "", 0u, false},
    {CommandId::SystemProtocolVersion, "system.protocolVersion", Carrier::Hotkey, "", 0u, false},
    {CommandId::SystemLastErrorCode, "system.lastErrorCode", Carrier::Hotkey, "", 0u, false},
    {CommandId::SystemLastError, "system.lastError", Carrier::LocalizedString, "", 0u, false},
    {CommandId::SystemClearError, "system.clearError", Carrier::Preloader, "", 0u, false},
    {CommandId::SystemFeatureFlags, "system.featureFlags", Carrier::Hotkey, "", 0u, false},
    {CommandId::SystemRuntimeReady, "system.runtimeReady", Carrier::Hotkey, "", 0u, false},
    {CommandId::SunSetEnabled, "sun.setEnabled", Carrier::Preloader, "b", kFeatureSun, true},
    {CommandId::SunSetDirection, "sun.setDirection", Carrier::Preloader, "rrr", kFeatureSun, true},
    {CommandId::SunSetColorIntensity, "sun.setColorIntensity", Carrier::Preloader, "rrrr", kFeatureSun, true},
    {CommandId::CsmSetEnabled, "csm.setEnabled", Carrier::Preloader, "b", kFeatureCsm, true},
    {CommandId::CsmSetLayout, "csm.setLayout", Carrier::Preloader, "ir", kFeatureCsm, true},
    {CommandId::CsmSetTuning, "csm.setTuning", Carrier::Preloader, "rr", kFeatureCsm, true},
    {CommandId::PointLightCreate, "pointLight.create", Carrier::Hotkey, "rrrrrrrr", kFeaturePointLight, true},
    {CommandId::PointLightDestroy, "pointLight.destroy", Carrier::Preloader, "d", kFeaturePointLight, true},
    {CommandId::PointLightSetEnabled, "pointLight.setEnabled", Carrier::Preloader, "db", kFeaturePointLight, true},
    {CommandId::PointLightSetPosition, "pointLight.setPosition", Carrier::Preloader, "drrr", kFeaturePointLight, true},
    {CommandId::PointLightSetColorIntensity, "pointLight.setColorIntensity", Carrier::Preloader, "drrrr", kFeaturePointLight, true},
    {CommandId::PointLightSetRadius, "pointLight.setRadius", Carrier::Preloader, "dr", kFeaturePointLight, true},
    {CommandId::PointLightSetShadowEnabled, "pointLight.setShadowEnabled", Carrier::Preloader, "db", kFeaturePointLight, true},
    {CommandId::PointLightSetShadowConfig, "pointLight.setShadowConfig", Carrier::Preloader, "dir", kFeaturePointLight, true},
    {CommandId::PointLightIsAlive, "pointLight.isAlive", Carrier::Hotkey, "d", kFeaturePointLight, true},
    {CommandId::VolumetricSetEnabled, "volumetric.setEnabled", Carrier::Preloader, "b", kFeatureVolumetric, true},
    {CommandId::VolumetricSetGlobalMediumEnabled, "volumetric.setGlobalMediumEnabled", Carrier::Preloader, "b", kFeatureVolumetric, true},
    {CommandId::VolumetricSetDensity, "volumetric.setDensity", Carrier::Preloader, "r", kFeatureVolumetric, true},
    {CommandId::VolumetricSetScattering, "volumetric.setScattering", Carrier::Preloader, "rr", kFeatureVolumetric, true},
    {CommandId::VolumetricSetQuality, "volumetric.setQuality", Carrier::Preloader, "ir", kFeatureVolumetric, true},
    {CommandId::VolumetricSetBackend, "volumetric.setBackend", Carrier::Preloader, "i", kFeatureVolumetric, true},
    {CommandId::VolumetricFogSetEnabled, "volumetricFog.setEnabled", Carrier::Preloader, "b", kFeatureVolumetric, true},
    {CommandId::VolumetricFogSetSettings, "volumetricFog.setSettings", Carrier::Preloader, "rrr", kFeatureVolumetric, true},
    {CommandId::LocalFogCreateSphere, "localFog.createSphere", Carrier::Hotkey, "rrrrrr", kFeatureLocalFog, true},
    {CommandId::LocalFogCreateBox, "localFog.createBox", Carrier::Hotkey, "rrrrrrrr", kFeatureLocalFog, true},
    {CommandId::LocalFogCreateCylinder, "localFog.createCylinder", Carrier::Hotkey, "rrrrrrr", kFeatureLocalFog, true},
    {CommandId::LocalFogDestroy, "localFog.destroy", Carrier::Preloader, "d", kFeatureLocalFog, true},
    {CommandId::LocalFogSetEnabled, "localFog.setEnabled", Carrier::Preloader, "db", kFeatureLocalFog, true},
    {CommandId::LocalFogSetPosition, "localFog.setPosition", Carrier::Preloader, "drrr", kFeatureLocalFog, true},
    {CommandId::LocalFogSetRotation, "localFog.setRotation", Carrier::Preloader, "drrr", kFeatureLocalFog, true},
    {CommandId::LocalFogSetDensity, "localFog.setDensity", Carrier::Preloader, "dr", kFeatureLocalFog, true},
    {CommandId::LocalFogSetEdgeFeather, "localFog.setEdgeFeather", Carrier::Preloader, "dr", kFeatureLocalFog, true},
    {CommandId::LocalFogSetSphereRadius, "localFog.setSphereRadius", Carrier::Preloader, "dr", kFeatureLocalFog, true},
    {CommandId::LocalFogSetBoxSize, "localFog.setBoxSize", Carrier::Preloader, "drrr", kFeatureLocalFog, true},
    {CommandId::LocalFogSetCylinderSize, "localFog.setCylinderSize", Carrier::Preloader, "drr", kFeatureLocalFog, true},
    {CommandId::LocalFogIsAlive, "localFog.isAlive", Carrier::Hotkey, "d", kFeatureLocalFog, true},
    {CommandId::OutlineSetEnabled, "outline.setEnabled", Carrier::Preloader, "b", 0x00000010u, true},
    {CommandId::OutlineSetColor, "outline.setColor", Carrier::Preloader, "rrrr", 0x00000010u, true},
    {CommandId::OutlineSetParameters, "outline.setParameters", Carrier::Preloader, "rr", 0x00000010u, true},
    {CommandId::BloomSetEnabled, "bloom.setEnabled", Carrier::Preloader, "b", 0x00000020u, true},
    {CommandId::BloomSetParameters, "bloom.setParameters", Carrier::Preloader, "rrr", 0x00000020u, true},
    {CommandId::BloomSetRadius, "bloom.setRadius", Carrier::Preloader, "r", 0x00000020u, true},
    {CommandId::PostfxSetEnabled, "postfx.setEnabled", Carrier::Preloader, "b", 0x00000040u, true},
    {CommandId::PostfxSetExposureGamma, "postfx.setExposureGamma", Carrier::Preloader, "rr", 0x00000040u, true},
    {CommandId::PostfxSetColorGrade, "postfx.setColorGrade", Carrier::Preloader, "rr", 0x00000040u, true},
    {CommandId::AaSetMode, "aa.setMode", Carrier::Preloader, "ii", 0x00000080u, true},
    {CommandId::AaSetSharpness, "aa.setSharpness", Carrier::Preloader, "r", 0x00000080u, true},
    {CommandId::DayNightSetEnabled, "dayNight.setEnabled", Carrier::Preloader, "b", kFeatureDayNight, true},
    {CommandId::DayNightSetTime, "dayNight.setTime", Carrier::Preloader, "r", kFeatureDayNight, true},
    {CommandId::DayNightSetSpeed, "dayNight.setSpeed", Carrier::Preloader, "r", kFeatureDayNight, true},
    {CommandId::LightingClockSetMode, "lightingClock.setMode", Carrier::Preloader, "i", kFeatureDayNight, true},
    {CommandId::LightingClockHoldTime, "lightingClock.holdTime", Carrier::Preloader, "r", kFeatureDayNight, true},
    {CommandId::LightingClockSetDayDuration, "lightingClock.setDayDuration", Carrier::Preloader, "r", kFeatureDayNight, true},
    {CommandId::LightingCycleSetCelestialMotionEnabled, "lightingCycle.setCelestialMotionEnabled", Carrier::Preloader, "b", kFeatureDayNight, true},
    {CommandId::LightingCycleSetTimeColorGradingEnabled, "lightingCycle.setTimeColorGradingEnabled", Carrier::Preloader, "b", kFeatureDayNight, true},
    {CommandId::LightingCycleSetColorTemperatureProfile, "lightingCycle.setColorTemperatureProfile", Carrier::Preloader, "rrrr", kFeatureDayNight, true},
    {CommandId::LightingCycleResetColorTemperatureProfile, "lightingCycle.resetColorTemperatureProfile", Carrier::Preloader, "", kFeatureDayNight, true},
    {CommandId::MathProgramCompile, "math.program.compile", Carrier::Hotkey, "s", kFeatureMathCurve, false},
    {CommandId::MathProgramDestroy, "math.program.destroy", Carrier::Preloader, "d", kFeatureMathCurve, false},
    {CommandId::MathProgramIsAlive, "math.program.isAlive", Carrier::Hotkey, "d", kFeatureMathCurve, false},
    {CommandId::MathProgramLastError, "math.program.lastError", Carrier::LocalizedString, "", kFeatureMathCurve, false},
    {CommandId::MathEvaluateReal, "math.evaluateReal", Carrier::LocalizedString, "drri", kFeatureMathCurve, false},
    {CommandId::MathEvaluateInteger, "math.evaluateInteger", Carrier::Hotkey, "drrii", kFeatureMathCurve, false},
    {CommandId::CurveCreate, "curve.create", Carrier::Hotkey, "d", kFeatureMathCurve, false},
    {CommandId::CurveDestroy, "curve.destroy", Carrier::Preloader, "d", kFeatureMathCurve, false},
    {CommandId::CurveSetReal, "curve.setReal", Carrier::Preloader, "dsr", kFeatureMathCurve, false},
    {CommandId::CurveSetCoordinateMode, "curve.setCoordinateMode", Carrier::Preloader, "di", kFeatureMathCurve, false},
    {CommandId::CurveSetEndpointLocks, "curve.setEndpointLocks", Carrier::Preloader, "dbb", kFeatureMathCurve, false},
    {CommandId::CurveEvaluateComponent, "curve.evaluateComponent", Carrier::LocalizedString, "dirrrrrrrri", kFeatureMathCurve, false},
    {CommandId::CurveDerivativeComponent, "curve.derivativeComponent", Carrier::LocalizedString, "dirrrrrrrri", kFeatureMathCurve, false},
    {CommandId::CurveArcLength, "curve.arcLength", Carrier::LocalizedString, "drrrrrrrii", kFeatureMathCurve, false},
    {CommandId::CurvePointCreate, "curve.points.create", Carrier::Hotkey, "i", kFeatureMathCurve | kFeaturePolylineCurve, false},
    {CommandId::CurvePointAppend4, "curve.points.append4", Carrier::Preloader, "dirrrrrrrrrrrr", kFeatureMathCurve | kFeaturePolylineCurve, false},
    {CommandId::CurvePointFinalize, "curve.points.finalize", Carrier::Preloader, "d", kFeatureMathCurve | kFeaturePolylineCurve, false},
    {CommandId::LightningCreate, "lightning.create", Carrier::Hotkey, "rrrrrrrrrrr", kFeatureLightning, false},
    {CommandId::LightningDestroy, "lightning.destroy", Carrier::Preloader, "d", kFeatureLightning, false},
    {CommandId::LightningSetEnabled, "lightning.setEnabled", Carrier::Preloader, "db", kFeatureLightning, false},
    {CommandId::LightningSetEndpoints, "lightning.setEndpoints", Carrier::Preloader, "drrrrrr", kFeatureLightning, false},
    {CommandId::LightningSetColor, "lightning.setColor", Carrier::Preloader, "drrrr", kFeatureLightning, false},
    {CommandId::LightningSetWidth, "lightning.setWidth", Carrier::Preloader, "dr", kFeatureLightning, false},
    {CommandId::LightningIsAlive, "lightning.isAlive", Carrier::Hotkey, "d", kFeatureLightning, false},
    {CommandId::LightningTemplateCreate, "lightning.template.create", Carrier::Hotkey, "s", kFeatureLightning, false},
    {CommandId::LightningTemplateSetBasic, "lightning.template.setBasic", Carrier::Preloader, "dsrrrrrrrrrrrri", kFeatureLightning, false},
    {CommandId::LightningTemplateSetAdvanced, "lightning.template.setAdvanced", Carrier::Preloader, "driirrrriirr", kFeatureLightning, false},
    {CommandId::LightningTemplateSetOptional, "lightning.template.setOptional", Carrier::Preloader, "drrrrrrrrrr", kFeatureLightning, false},
    {CommandId::LightningTemplateFinalize, "lightning.template.finalize", Carrier::Preloader, "d", kFeatureLightning, false},
    {CommandId::LightningCreateFromTemplate, "lightning.createFromTemplate", Carrier::Hotkey, "drrrrrri", kFeatureLightning, false},
    {CommandId::LightningCreatePolylineFromTemplate, "lightning.createPolylineFromTemplate", Carrier::Hotkey, "ddi", kFeatureLightning | kFeaturePolylineCurve, false},
    {CommandId::LightningTemplateSetFormulaCurve, "lightning.template.setFormulaCurve", Carrier::Preloader, "dd", kFeatureLightning | kFeatureMathCurve, false},
    {CommandId::LightningSetFormulaCurve, "lightning.setFormulaCurve", Carrier::Preloader, "dd", kFeatureLightning | kFeatureMathCurve, false},
    {CommandId::LightningSetPolylineCurve, "lightning.setPolylineCurve", Carrier::Preloader, "dd", kFeatureLightning | kFeaturePolylineCurve, false},
    {CommandId::ManagedObjectCount, "managedObject.count", Carrier::Hotkey, "", kFeatureManagedObject, true},
    {CommandId::ManagedObjectIsAlive, "managedObject.isAlive", Carrier::Hotkey, "d", kFeatureManagedObject, true},
    {CommandId::ManagedObjectType, "managedObject.type", Carrier::Hotkey, "d", kFeatureManagedObject, true},
    {CommandId::TimeVisualSeconds, "time.visualSeconds", Carrier::LocalizedString, "", kFeatureTime, true},
    {CommandId::TimeFrameIndex, "time.frameIndex", Carrier::Hotkey, "", kFeatureTime, true},
    {CommandId::StatsFramesPerSecond, "stats.framesPerSecond", Carrier::LocalizedString, "", kFeatureStats, true},
    {CommandId::StatsFrameTimeMilliseconds, "stats.frameTimeMilliseconds", Carrier::LocalizedString, "", kFeatureStats, true},
    {CommandId::StatsDrawCallCount, "stats.drawCallCount", Carrier::Hotkey, "", kFeatureStats, true},
}};

struct Argument {
  WireType type = WireType::I32;
  bool boolean = false;
  int32_t integer = 0;
  float real = 0.0f;
  std::string text;
};

struct ParsedRequest {
  const CommandSpec* spec = nullptr;
  std::array<Argument, kMaximumArgumentCount> arguments = {};
  size_t argumentCount = 0u;
};

thread_local ErrorCode g_lastError = ErrorCode::None;

struct TypedCommandBinding {
  TypedOpcode opcode;
  CommandId command;
  ResultKind result;
};

constexpr std::array<TypedCommandBinding, 15u> kTypedCommands = {{
    {TypedOpcode::PointLightSetPosition,
     CommandId::PointLightSetPosition, ResultKind::Void},
    {TypedOpcode::PointLightSetColorIntensity,
     CommandId::PointLightSetColorIntensity, ResultKind::Void},
    {TypedOpcode::PointLightSetRadius,
     CommandId::PointLightSetRadius, ResultKind::Void},
    {TypedOpcode::MathEvaluateReal,
     CommandId::MathEvaluateReal, ResultKind::Real},
    {TypedOpcode::MathEvaluateInteger,
     CommandId::MathEvaluateInteger, ResultKind::Integer},
    {TypedOpcode::CurveEvaluateComponent,
     CommandId::CurveEvaluateComponent, ResultKind::Real},
    {TypedOpcode::CurveDerivativeComponent,
     CommandId::CurveDerivativeComponent, ResultKind::Real},
    {TypedOpcode::CurveArcLength,
     CommandId::CurveArcLength, ResultKind::Real},
    {TypedOpcode::CurvePointAppend4,
     CommandId::CurvePointAppend4, ResultKind::Void},
    {TypedOpcode::LightningSetEndpoints,
     CommandId::LightningSetEndpoints, ResultKind::Void},
    {TypedOpcode::LightningSetColor,
     CommandId::LightningSetColor, ResultKind::Void},
    {TypedOpcode::LightningSetWidth,
     CommandId::LightningSetWidth, ResultKind::Void},
    {TypedOpcode::TimeVisualSeconds,
     CommandId::TimeVisualSeconds, ResultKind::Real},
    {TypedOpcode::StatsFramesPerSecond,
     CommandId::StatsFramesPerSecond, ResultKind::Real},
    {TypedOpcode::StatsFrameTimeMilliseconds,
     CommandId::StatsFrameTimeMilliseconds, ResultKind::Real},
}};

struct TypedTransaction {
  int32_t opcode = 0;
  ParsedRequest request = {};
  uint32_t writtenMask = 0u;
  bool invalid = false;
};

std::mutex g_typedTransportMutex;
uint32_t g_typedPendingTable = 0u;
uint32_t g_typedActiveTable = 0u;
bool g_typedRegisterASeen = false;
std::unordered_map<int32_t, TypedTransaction> g_typedTransactions;
constexpr size_t kMaximumTypedTransactions = 4u;

#if !defined(WARVK_JAPI_PROTOCOL_TEST)
enum class ManagedType : int32_t {
  PointLight = 1,
  Lightning = 2,
  LocalFog = 3,
};

struct ManagedObject {
  ManagedType type = ManagedType::PointLight;
  int32_t internalId = 0;
};

std::mutex g_objectMutex;
std::unordered_map<int32_t, ManagedObject> g_objects;
int32_t g_nextPublicId = 1;
#endif

const CommandSpec* FindCommand(std::string_view name) {
  for (const CommandSpec& spec : kCommands) {
    if (name == spec.name)
      return &spec;
  }
  return nullptr;
}

const CommandSpec* FindCommand(CommandId id) {
  for (const CommandSpec& spec : kCommands) {
    if (spec.id == id)
      return &spec;
  }
  return nullptr;
}

const TypedCommandBinding* FindTypedCommand(int32_t opcode) {
  for (const TypedCommandBinding& binding : kTypedCommands) {
    if (static_cast<int32_t>(binding.opcode) == opcode)
      return &binding;
  }
  return nullptr;
}

Reply Failure(ErrorCode code) {
  g_lastError = code;
  Reply reply;
  reply.consumed = true;
  reply.error = code;
  return reply;
}

Reply SuccessVoid(bool clearError = true) {
  if (clearError)
    g_lastError = ErrorCode::None;
  Reply reply;
  reply.consumed = true;
  reply.kind = ResultKind::Void;
  return reply;
}

Reply SuccessInteger(int32_t value, bool clearError = true) {
  if (clearError)
    g_lastError = ErrorCode::None;
  Reply reply;
  reply.consumed = true;
  reply.kind = ResultKind::Integer;
  reply.integer = value;
  return reply;
}

[[maybe_unused]] Reply SuccessReal(float value, bool clearError = true) {
  if (clearError)
    g_lastError = ErrorCode::None;
  Reply reply;
  reply.consumed = true;
  reply.kind = ResultKind::Real;
  reply.real = value;
  return reply;
}

Reply SuccessText(std::string value, bool clearError = true) {
  if (clearError)
    g_lastError = ErrorCode::None;
  Reply reply;
  reply.consumed = true;
  reply.kind = ResultKind::Text;
  reply.text = std::move(value);
  return reply;
}

bool IsAsciiLetter(char value) {
  return (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z');
}

bool IsAsciiDigit(char value) {
  return value >= '0' && value <= '9';
}

bool IsCommandName(std::string_view value) {
  if (value.empty() || !IsAsciiLetter(value.front()))
    return false;
  for (size_t index = 1; index < value.size(); ++index) {
    const char character = value[index];
    if (!IsAsciiLetter(character) && !IsAsciiDigit(character) &&
        character != '.' && character != '_' && character != '-')
      return false;
  }
  return true;
}

enum class NumberStatus {
  Ok,
  Invalid,
  OutOfRange,
};

NumberStatus ParseInt32(std::string_view text, int32_t& output) {
  if (text.empty())
    return NumberStatus::Invalid;

  size_t offset = 0u;
  bool negative = false;
  if (text.front() == '-') {
    negative = true;
    offset = 1u;
    if (offset == text.size())
      return NumberStatus::Invalid;
  }

  if (text[offset] == '0') {
    if (negative || text.size() != 1u)
      return NumberStatus::Invalid;
    output = 0;
    return NumberStatus::Ok;
  }
  if (text[offset] < '1' || text[offset] > '9')
    return NumberStatus::Invalid;

  const uint64_t limit = negative ? 2147483648ull : 2147483647ull;
  uint64_t value = 0u;
  for (; offset < text.size(); ++offset) {
    const char c = text[offset];
    if (!IsAsciiDigit(c))
      return NumberStatus::Invalid;
    value = value * 10u + uint64_t(c - '0');
    if (value > limit)
      return NumberStatus::OutOfRange;
  }

  if (negative) {
    if (value == 2147483648ull)
      output = std::numeric_limits<int32_t>::min();
    else
      output = -static_cast<int32_t>(value);
  } else {
    output = static_cast<int32_t>(value);
  }
  return NumberStatus::Ok;
}

bool IsStrictRealSyntax(std::string_view value) {
  if (value.empty())
    return false;
  size_t offset = 0u;
  if (value[offset] == '-') {
    ++offset;
    if (offset == value.size())
      return false;
  }
  if (!IsAsciiDigit(value[offset]))
    return false;
  if (value[offset] == '0') {
    ++offset;
    if (offset < value.size() && IsAsciiDigit(value[offset]))
      return false;
  } else {
    while (offset < value.size() && IsAsciiDigit(value[offset]))
      ++offset;
  }
  if (offset < value.size() && value[offset] == '.') {
    ++offset;
    const size_t start = offset;
    while (offset < value.size() && IsAsciiDigit(value[offset]))
      ++offset;
    if (offset == start)
      return false;
  }
  if (offset < value.size() &&
      (value[offset] == 'e' || value[offset] == 'E')) {
    ++offset;
    if (offset < value.size() &&
        (value[offset] == '+' || value[offset] == '-'))
      ++offset;
    const size_t start = offset;
    while (offset < value.size() && IsAsciiDigit(value[offset]))
      ++offset;
    if (offset == start)
      return false;
  }
  return offset == value.size();
}

NumberStatus ParseReal(std::string_view text, float& output) {
  if (!IsStrictRealSyntax(text))
    return NumberStatus::Invalid;
  const char* const first = text.data();
  const char* const last = first + text.size();
  const auto parsed =
      std::from_chars(first, last, output, std::chars_format::general);
  if (parsed.ec == std::errc::result_out_of_range ||
      (parsed.ec == std::errc{} && !std::isfinite(output)))
    return NumberStatus::OutOfRange;
  return parsed.ec == std::errc{} && parsed.ptr == last
      ? NumberStatus::Ok : NumberStatus::Invalid;
}

ErrorCode ParseArgument(std::string_view token, Argument& output) {
  if (token.size() < 2u || token[1] != ':')
    return ErrorCode::InvalidArgumentType;
  const std::string_view value = token.substr(2u);
  switch (token[0]) {
    case 'b':
      output.type = WireType::Bool;
      if (value == "0") {
        output.boolean = false;
        return ErrorCode::None;
      }
      if (value == "1") {
        output.boolean = true;
        return ErrorCode::None;
      }
      return ErrorCode::InvalidBoolean;
    case 'i': {
      output.type = WireType::I32;
      const NumberStatus status = ParseInt32(value, output.integer);
      if (status == NumberStatus::OutOfRange)
        return ErrorCode::IntegerOverflow;
      return status == NumberStatus::Ok
          ? ErrorCode::None : ErrorCode::InvalidInteger;
    }
    case 'd': {
      output.type = WireType::Id;
      const NumberStatus status = ParseInt32(value, output.integer);
      return status == NumberStatus::Ok && output.integer > 0
          ? ErrorCode::None : ErrorCode::InvalidId;
    }
    case 'r': {
      output.type = WireType::Real;
      const NumberStatus status = ParseReal(value, output.real);
      if (status == NumberStatus::OutOfRange)
        return ErrorCode::RealOutOfRange;
      return status == NumberStatus::Ok
          ? ErrorCode::None : ErrorCode::InvalidReal;
    }
    case 's':
      output.type = WireType::String;
      output.text.assign(value.data(), value.size());
      return ErrorCode::None;
    default:
      return ErrorCode::InvalidArgumentType;
  }
}

char WireTypeCode(WireType type) {
  switch (type) {
    case WireType::Bool: return 'b';
    case WireType::I32: return 'i';
    case WireType::Id: return 'd';
    case WireType::Real: return 'r';
    case WireType::String: return 's';
  }
  return '\0';
}

ErrorCode TypeMismatchError(char expected) {
  switch (expected) {
    case 'b': return ErrorCode::InvalidBoolean;
    case 'i': return ErrorCode::InvalidInteger;
    case 'd': return ErrorCode::InvalidId;
    case 'r': return ErrorCode::InvalidReal;
    case 's': return ErrorCode::InvalidArgumentType;
    default: return ErrorCode::InternalError;
  }
}

ErrorCode ParseRequest(std::string_view payload, Carrier carrier,
                       ParsedRequest& output) {
  if (payload.size() > kMaximumMessageBytes)
    return ErrorCode::PayloadTooLong;

  // The public wire format is deliberately ASCII-only.  One compatibility
  // exception is needed for maps already saved through the YDWE GUI before
  // the JASS wrapper started hashing template display names: those maps send
  // a local-codepage name in lightning.template.create's sole s: field.
  // Keep the exception narrow; all command names, paths, numbers, and every
  // other request continue to reject non-ASCII bytes fail-closed.
  bool hasNonAscii = false;
  for (const unsigned char character : payload) {
    if (character > 0x7fu) {
      hasNonAscii = true;
      continue;
    }
    if (character < 0x20u || character == 0x7fu)
      return ErrorCode::ControlCharacter;
  }
  if (payload.size() < kProtocolPrefix.size() ||
      payload.substr(0u, kProtocolPrefix.size()) != kProtocolPrefix)
    return ErrorCode::InternalError;

  const std::string_view body = payload.substr(kProtocolPrefix.size());
  if (body.empty())
    return ErrorCode::MissingCommand;

  std::array<std::string_view, kMaximumArgumentCount + 2u> tokens = {};
  size_t tokenCount = 0u;
  size_t offset = 0u;
  while (true) {
    if (tokenCount >= tokens.size())
      return ErrorCode::TooManyArguments;
    const size_t delimiter = body.find(';', offset);
    if (delimiter == std::string_view::npos) {
      tokens[tokenCount++] = body.substr(offset);
      break;
    }
    tokens[tokenCount++] = body.substr(offset, delimiter - offset);
    offset = delimiter + 1u;
  }
  for (size_t index = 0u; index < tokenCount; ++index) {
    if (tokens[index].empty())
      return ErrorCode::EmptyToken;
  }

  if (hasNonAscii) {
    const bool isLegacyLocalizedTemplateName =
        tokenCount == 3u && tokens[0] == kCanonicalVersion &&
        tokens[1] == "lightning.template.create" &&
        tokens[2].size() >= 3u && tokens[2][0] == 's' &&
        tokens[2][1] == ':';
    if (!isLegacyLocalizedTemplateName)
      return ErrorCode::NonAscii;
  }

  if (tokens[0] != kCanonicalVersion)
    return ErrorCode::UnsupportedVersion;
  if (tokenCount < 2u || tokens[1].empty() || !IsCommandName(tokens[1]))
    return ErrorCode::MissingCommand;

  output.spec = FindCommand(tokens[1]);
  if (!output.spec)
    return ErrorCode::UnknownCommand;

  if (output.spec->carrier != carrier)
    return ErrorCode::CarrierMismatch;

  output.argumentCount = tokenCount - 2u;
  const size_t expectedCount = std::strlen(output.spec->argumentTypes);
  if (output.argumentCount != expectedCount)
    return ErrorCode::ArgumentCountMismatch;

  for (size_t index = 0u; index < output.argumentCount; ++index) {
    const ErrorCode error = ParseArgument(tokens[index + 2u],
                                          output.arguments[index]);
    if (error != ErrorCode::None)
      return error;
    if (WireTypeCode(output.arguments[index].type) !=
        output.spec->argumentTypes[index])
      return TypeMismatchError(output.spec->argumentTypes[index]);
  }
  return ErrorCode::None;
}

#if !defined(WARVK_JAPI_PROTOCOL_TEST)
bool IsNonNegative(float value) {
  return std::isfinite(value) && value >= 0.0f;
}

bool IsValidColor(float r, float g, float b, float a = 0.0f,
                  bool validateAlpha = false) {
  if (!IsNonNegative(r) || !IsNonNegative(g) || !IsNonNegative(b))
    return false;
  return !validateAlpha || (std::isfinite(a) && a >= 0.0f && a <= 1.0f);
}

bool IsValidPointShadowResolution(int32_t resolution) {
  if (resolution < 128 || resolution > 2048)
    return false;
  const uint32_t value = static_cast<uint32_t>(resolution);
  return (value & (value - 1u)) == 0u;
}

bool IsValidFogPosition(float x, float y, float z) {
  constexpr float kMaximumCoordinate = 1'000'000.0f;
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
      std::abs(x) <= kMaximumCoordinate &&
      std::abs(y) <= kMaximumCoordinate &&
      std::abs(z) <= kMaximumCoordinate;
}

bool IsValidFogFullSize(float value) {
  return std::isfinite(value) && value >= 1.0f && value <= 200'000.0f;
}

bool IsValidFogRadius(float value) {
  return std::isfinite(value) && value >= 0.5f && value <= 100'000.0f;
}

bool IsValidFogDensity(float value) {
  return std::isfinite(value) && value >= 0.0f && value <= 2.0f;
}

bool IsValidFogFeather(float value) {
  return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

bool IsValidFogRotation(float value) {
  return std::isfinite(value) && std::abs(value) <= 360'000.0f;
}

bool IsLightningTemplateName(std::string_view value) {
  if (value.empty() || value.size() > 64u)
    return false;

  bool hasNonAscii = false;
  for (const char character : value) {
    if (static_cast<unsigned char>(character) > 0x7fu) {
      hasNonAscii = true;
      continue;
    }
    if (!IsAsciiLetter(character) && !IsAsciiDigit(character) &&
        character != '_' && character != '-' && character != '.')
      return false;
  }
  // Existing ASCII-only names retain the original first-letter rule.  A
  // localized name is a display label only and has already passed the narrow
  // parser exception above, so do not require its first codepage byte to be a
  // Latin letter.
  return hasNonAscii || IsAsciiLetter(value.front());
}

char ToAsciiLower(char value) {
  return value >= 'A' && value <= 'Z'
      ? char(value - 'A' + 'a') : value;
}

bool HasAllowedLightningTextureExtension(std::string_view value) {
  const size_t dot = value.find_last_of('.');
  if (dot == std::string_view::npos)
    return false;
  std::string extension;
  extension.reserve(value.size() - dot);
  for (size_t index = dot; index < value.size(); ++index)
    extension.push_back(ToAsciiLower(value[index]));
  return extension == ".blp" || extension == ".tga" ||
         extension == ".png" || extension == ".jpg" ||
         extension == ".jpeg" || extension == ".bmp";
}

bool IsLightningTexturePath(std::string_view value) {
  if (value.empty() || value.size() > 192u || value.front() == '/' ||
      value.front() == '\\' || !HasAllowedLightningTextureExtension(value))
    return false;

  size_t segmentStart = 0u;
  for (size_t index = 0u; index <= value.size(); ++index) {
    const bool atEnd = index == value.size();
    if (!atEnd && value[index] != '/' && value[index] != '\\') {
      const char character = value[index];
      if (!IsAsciiLetter(character) && !IsAsciiDigit(character) &&
          character != '_' && character != '-' && character != '.' &&
          character != ' ')
        return false;
      continue;
    }

    const std::string_view segment = value.substr(segmentStart, index - segmentStart);
    if (segment.empty() || segment == "." || segment == "..")
      return false;
    segmentStart = index + 1u;
  }
  return true;
}

std::string NormalizeLightningTexturePath(std::string_view value) {
  std::string normalized(value);
  for (char& character : normalized) {
    if (character == '/')
      character = '\\';
  }
  return normalized;
}

std::string FormatReal(float value) {
  char buffer[64] = {};
  const auto converted = std::to_chars(
      buffer, buffer + sizeof(buffer) - 1u, value,
      std::chars_format::general, std::numeric_limits<float>::max_digits10);
  if (converted.ec != std::errc{})
    return "0";
  return std::string(buffer, static_cast<size_t>(converted.ptr - buffer));
}

int32_t SaturateToInt32(uint64_t value) {
  return value > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())
      ? std::numeric_limits<int32_t>::max()
      : static_cast<int32_t>(value);
}

bool IsBackendObjectAlive(const ManagedObject& object) {
  switch (object.type) {
    case ManagedType::PointLight:
      return War3LightManager::Instance().IsPointLightAlive(object.internalId);
    case ManagedType::Lightning:
      return render::War3LightningRuntime::instance().isAlive(object.internalId);
    case ManagedType::LocalFog:
      return war3shader::IsFogVolumeAlive(object.internalId);
  }
  return false;
}

bool ResolveObject(int32_t publicId, ManagedType expected,
                   int32_t& internalId) {
  ManagedObject object;
  {
    std::lock_guard<std::mutex> lock(g_objectMutex);
    const auto entry = g_objects.find(publicId);
    if (entry == g_objects.end() || entry->second.type != expected)
      return false;
    object = entry->second;
  }
  if (!IsBackendObjectAlive(object)) {
    std::lock_guard<std::mutex> lock(g_objectMutex);
    const auto entry = g_objects.find(publicId);
    if (entry != g_objects.end() &&
        entry->second.type == object.type &&
        entry->second.internalId == object.internalId)
      g_objects.erase(entry);
    return false;
  }
  internalId = object.internalId;
  return true;
}

int32_t RegisterObject(ManagedType type, int32_t internalId) {
  if (internalId <= 0)
    return 0;
  std::lock_guard<std::mutex> lock(g_objectMutex);
  if (g_objects.size() >=
      static_cast<size_t>(std::numeric_limits<int32_t>::max() - 1))
    return 0;
  const size_t attempts = g_objects.size() + 1u;
  for (size_t index = 0u; index < attempts; ++index) {
    int32_t candidate = g_nextPublicId++;
    if (g_nextPublicId <= 0)
      g_nextPublicId = 1;
    if (candidate <= 0)
      continue;
    if (g_objects.emplace(candidate, ManagedObject{type, internalId}).second)
      return candidate;
  }
  return 0;
}

void RemoveRegistryEntry(int32_t publicId, ManagedType expected,
                         int32_t internalId) {
  std::lock_guard<std::mutex> lock(g_objectMutex);
  const auto entry = g_objects.find(publicId);
  if (entry != g_objects.end() && entry->second.type == expected &&
      entry->second.internalId == internalId)
    g_objects.erase(entry);
}

bool ManagedObjectAlive(int32_t publicId, ManagedType* typeOut = nullptr) {
  ManagedObject object;
  {
    std::lock_guard<std::mutex> lock(g_objectMutex);
    const auto entry = g_objects.find(publicId);
    if (entry == g_objects.end())
      return false;
    object = entry->second;
  }
  if (!IsBackendObjectAlive(object)) {
    RemoveRegistryEntry(publicId, object.type, object.internalId);
    return false;
  }
  if (typeOut)
    *typeOut = object.type;
  return true;
}

int32_t ManagedObjectCount() {
  std::vector<std::pair<int32_t, ManagedObject>> snapshot;
  {
    std::lock_guard<std::mutex> lock(g_objectMutex);
    snapshot.reserve(g_objects.size());
    for (const auto& entry : g_objects)
      snapshot.push_back(entry);
  }
  int32_t count = 0;
  for (const auto& entry : snapshot) {
    if (IsBackendObjectAlive(entry.second))
      ++count;
    else
      RemoveRegistryEntry(entry.first, entry.second.type,
                          entry.second.internalId);
  }
  return count;
}

Reply BackendRejected() {
  return Failure(ErrorCode::BackendRejected);
}

void AdvanceLightingClockRevision(War3DayNightSettings& settings) {
  if (++settings.clockRevision == 0u)
    settings.clockRevision = 1u;
}

bool HoldLightingTime(War3DayNightSettings& settings, float hours) {
  if (!std::isfinite(hours) || hours < 0.0f || hours > 24.0f)
    return false;
  settings.renderTimeHours = hours >= 24.0f ? 0.0f : hours;
  settings.clockMode = War3LightingClockMode::Held;
  AdvanceLightingClockRevision(settings);
  return true;
}

bool EvaluateAuthorScalar(int32_t curveId, float t, float time, int32_t seed,
                          float& output) {
  if (t < 0.0f || t > 1.0f)
    return false;
  math::CurveSnapshot curve;
  if (!math::CurveRuntime::instance().snapshotCurve(curveId, curve) ||
      !curve.isFormula() || curve.program->resultType() != math::ValueType::Scalar)
    return false;
  math::CurveContext context;
  context.t = t;
  context.time = time;
  context.seed = static_cast<uint32_t>(seed);
  context.index = static_cast<uint32_t>(std::lround(t));
  context.segments = 1u;
  math::Value value;
  if (!math::EvaluateCurveRaw(curve, context, value) ||
      value.type != math::ValueType::Scalar || !std::isfinite(value.x))
    return false;
  output = value.x;
  return true;
}

bool ConvertAuthorScalarToInteger(float value, int32_t roundingMode,
                                  int32_t& output) {
  double rounded = 0.0;
  switch (roundingMode) {
    case 0:
      rounded = value >= 0.0f
          ? std::floor(double(value) + 0.5)
          : std::ceil(double(value) - 0.5);
      break;
    case 1: rounded = std::floor(double(value)); break;
    case 2: rounded = std::ceil(double(value)); break;
    case 3: rounded = std::trunc(double(value)); break;
    default: return false;
  }
  if (rounded < double(std::numeric_limits<int32_t>::min()) ||
      rounded > double(std::numeric_limits<int32_t>::max()))
    return false;
  output = static_cast<int32_t>(rounded);
  return true;
}

bool CommandUsesDirectRenderSettings(CommandId command) noexcept {
  switch (command) {
    case CommandId::SunSetEnabled:
    case CommandId::SunSetDirection:
    case CommandId::SunSetColorIntensity:
    case CommandId::CsmSetEnabled:
    case CommandId::CsmSetLayout:
    case CommandId::CsmSetTuning:
    case CommandId::PointLightSetShadowConfig:
    case CommandId::VolumetricSetDensity:
    case CommandId::VolumetricSetScattering:
    case CommandId::VolumetricSetQuality:
    case CommandId::DayNightSetEnabled:
    case CommandId::DayNightSetTime:
    case CommandId::DayNightSetSpeed:
    case CommandId::LightingClockSetMode:
    case CommandId::LightingClockHoldTime:
    case CommandId::LightingClockSetDayDuration:
    case CommandId::LightingCycleSetCelestialMotionEnabled:
    case CommandId::LightingCycleSetTimeColorGradingEnabled:
    case CommandId::LightingCycleSetColorTemperatureProfile:
    case CommandId::LightingCycleResetColorTemperatureProfile:
      return true;
    default:
      return false;
  }
}

Reply DispatchBackend(const ParsedRequest& request) {
  const auto& a = request.arguments;

  // Acquire the settings mailbox only for cases that mutate it directly.
  // Point-light and volumetric delegate APIs acquire the mailbox themselves;
  // holding it here would self-deadlock and would also invert the
  // LightManager -> settings lock order used by AddPointLight.
  dxvk::war3::War3SettingsWrite settings;
  if (CommandUsesDirectRenderSettings(request.spec->id)) {
    settings = dxvk::war3::GetMutableSettings();
    if (!settings)
      return Failure(ErrorCode::BackendUnavailable);
  }

  switch (request.spec->id) {
    case CommandId::SunSetEnabled:
      settings->sun.enabled = a[0].boolean;
      return SuccessVoid();
    case CommandId::SunSetDirection: {
      const float lengthSq =
          a[0].real * a[0].real + a[1].real * a[1].real +
          a[2].real * a[2].real;
      if (!std::isfinite(lengthSq) || lengthSq <= 1.0e-8f)
        return BackendRejected();
      settings->sun.direction =
          Vector4(a[0].real, a[1].real, a[2].real, 0.0f);
      return SuccessVoid();
    }
    case CommandId::SunSetColorIntensity:
      if (!IsValidColor(a[0].real, a[1].real, a[2].real) ||
          !IsNonNegative(a[3].real))
        return BackendRejected();
      settings->sun.color =
          Vector4(a[0].real, a[1].real, a[2].real, 0.0f);
      settings->sun.intensity = a[3].real;
      return SuccessVoid();
    case CommandId::CsmSetEnabled:
      settings->shadows.enabled = a[0].boolean;
      return SuccessVoid();
    case CommandId::CsmSetLayout:
      if (a[0].integer < 1 || a[0].integer > 4 ||
          a[1].real < 256.0f || a[1].real > 50000.0f)
        return BackendRejected();
      settings->shadows.csm.cascadeCount =
          static_cast<uint32_t>(a[0].integer);
      settings->shadows.csm.maxDistance = a[1].real;
      return SuccessVoid();
    case CommandId::CsmSetTuning:
      if (a[0].real < 0.0f || a[0].real > 1.0f ||
          a[1].real < 0.0f || a[1].real > 10000.0f)
        return BackendRejected();
      settings->shadows.receiverBias = a[0].real;
      settings->shadows.cascadeBlendRange = a[1].real;
      return SuccessVoid();
    case CommandId::PointLightCreate: {
      if (a[3].real <= 0.0f ||
          !IsValidColor(a[4].real, a[5].real, a[6].real) ||
          !IsNonNegative(a[7].real))
        return BackendRejected();
      const int32_t internalId = war3shader::AddPointLight(
          a[0].real, a[1].real, a[2].real, a[3].real,
          a[4].real, a[5].real, a[6].real, a[7].real, 0.0f);
      if (internalId <= 0)
        return BackendRejected();
      const int32_t publicId =
          RegisterObject(ManagedType::PointLight, internalId);
      if (publicId <= 0) {
        static_cast<void>(war3shader::RemovePointLight(internalId));
        return Failure(ErrorCode::InternalError);
      }
      return SuccessInteger(publicId);
    }
    case CommandId::PointLightDestroy: {
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::PointLight, internalId))
        return BackendRejected();
      if (!war3shader::RemovePointLight(internalId))
        return BackendRejected();
      RemoveRegistryEntry(a[0].integer, ManagedType::PointLight, internalId);
      return SuccessVoid();
    }
    case CommandId::PointLightSetEnabled: {
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::PointLight, internalId) ||
          !War3LightManager::Instance().SetPointLightActive(
              internalId, a[1].boolean))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::PointLightSetPosition: {
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::PointLight, internalId) ||
          !War3LightManager::Instance().SetPointLightPosition(
              internalId, a[1].real, a[2].real, a[3].real))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::PointLightSetColorIntensity: {
      if (!IsValidColor(a[1].real, a[2].real, a[3].real) ||
          !IsNonNegative(a[4].real))
        return BackendRejected();
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::PointLight, internalId) ||
          !War3LightManager::Instance().SetPointLightColorIntensity(
              internalId, a[1].real, a[2].real, a[3].real, a[4].real))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::PointLightSetRadius: {
      if (a[1].real <= 0.0f)
        return BackendRejected();
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::PointLight, internalId) ||
          !War3LightManager::Instance().SetPointLightRadius(
              internalId, a[1].real))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::PointLightSetShadowEnabled: {
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::PointLight, internalId) ||
          !war3shader::SetPointLightShadowIntensity(
              internalId, a[1].boolean ? 1.0f : 0.0f))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::PointLightSetShadowConfig: {
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::PointLight, internalId) ||
          !IsValidPointShadowResolution(a[1].integer) ||
          a[2].real < 0.0f || a[2].real > 10.0f)
        return BackendRejected();
      // Current renderer uses a single cube-map allocation policy. The public
      // light id is still validated so a future per-light implementation can
      // preserve the v1 wire contract.
      settings->shadows.pointShadowResolution =
          static_cast<uint32_t>(a[1].integer);
      settings->shadows.pointShadowBias = a[2].real;
      settings->shadows.pointLightsEnabled = true;
      settings->shadows.pointShadowEnabled = true;
      return SuccessVoid();
    }
    case CommandId::PointLightIsAlive: {
      int32_t internalId = 0;
      return SuccessInteger(
          ResolveObject(a[0].integer, ManagedType::PointLight, internalId)
              ? 1 : 0);
    }
    case CommandId::VolumetricSetEnabled:
      return war3shader::SetVolumetricLightEnabled(a[0].boolean)
          ? SuccessVoid() : BackendRejected();
    case CommandId::VolumetricSetGlobalMediumEnabled:
      return war3shader::SetVolumetricGlobalMediumEnabled(a[0].boolean)
          ? SuccessVoid() : BackendRejected();
    case CommandId::VolumetricSetDensity:
      if (a[0].real < 0.0f || a[0].real > 2.0f)
        return BackendRejected();
      settings->postFx.volumetricLight.density = a[0].real;
      return SuccessVoid();
    case CommandId::VolumetricSetScattering:
      if (a[0].real < 0.0f || a[0].real > 3.0f ||
          a[1].real < 0.70f || a[1].real > 0.999f)
        return BackendRejected();
      settings->postFx.volumetricLight.weight = a[0].real;
      settings->postFx.volumetricLight.decay = a[1].real;
      return SuccessVoid();
    case CommandId::VolumetricSetQuality:
      if (a[0].integer < 4 || a[0].integer > 16 ||
          a[1].real < 1.0f || a[1].real > 50000.0f)
        return BackendRejected();
      settings->postFx.volumetricLight.sampleCount = a[0].integer;
      settings->postFx.volumetricLight.sunDistance = a[1].real;
      // The public max-distance argument predates the Froxel backend. Preserve
      // its legacy ray-march meaning. Froxel uses the independently bounded
      // camera-stable domain; old maps commonly pass 1200-1800 and must not
      // silently reintroduce a pitch-dependent short grid.
      settings->postFx.volumetricLight.froxelFar = 10000.0f;
      settings->postFx.volumetricLight.maxRayDistance = 1.0f;
      return SuccessVoid();
    case CommandId::VolumetricSetBackend:
      if (a[0].integer < 0 || a[0].integer > 2)
        return BackendRejected();
      return war3shader::SetVolumetricBackend(
          static_cast<uint32_t>(a[0].integer))
          ? SuccessVoid() : BackendRejected();
    case CommandId::VolumetricFogSetEnabled:
      return war3shader::SetVolumetricHeightFogEnabled(a[0].boolean)
          ? SuccessVoid() : BackendRejected();
    case CommandId::VolumetricFogSetSettings:
      if (a[1].real < 0.0f || a[1].real > 0.05f ||
          a[2].real < 0.0f || a[2].real > 2.0f)
        return BackendRejected();
      return war3shader::SetVolumetricHeightFog(
          a[0].real, a[1].real, a[2].real)
          ? SuccessVoid() : BackendRejected();
    case CommandId::LocalFogCreateSphere: {
      if (!IsValidFogPosition(a[0].real, a[1].real, a[2].real) ||
          !IsValidFogRadius(a[3].real) ||
          !IsValidFogDensity(a[4].real) ||
          !IsValidFogFeather(a[5].real))
        return BackendRejected();
      const int32_t internalId = war3shader::AddSphereFogVolume(
          a[0].real, a[1].real, a[2].real, a[3].real,
          a[4].real, a[5].real);
      if (internalId <= 0)
        return BackendRejected();
      const int32_t publicId =
          RegisterObject(ManagedType::LocalFog, internalId);
      if (publicId <= 0) {
        static_cast<void>(war3shader::RemoveFogVolume(internalId));
        return Failure(ErrorCode::InternalError);
      }
      return SuccessInteger(publicId);
    }
    case CommandId::LocalFogCreateBox: {
      if (!IsValidFogPosition(a[0].real, a[1].real, a[2].real) ||
          !IsValidFogFullSize(a[3].real) ||
          !IsValidFogFullSize(a[4].real) ||
          !IsValidFogFullSize(a[5].real) ||
          !IsValidFogDensity(a[6].real) ||
          !IsValidFogFeather(a[7].real))
        return BackendRejected();
      const int32_t internalId = war3shader::AddBoxFogVolume(
          a[0].real, a[1].real, a[2].real,
          a[3].real, a[4].real, a[5].real,
          a[6].real, a[7].real);
      if (internalId <= 0)
        return BackendRejected();
      const int32_t publicId =
          RegisterObject(ManagedType::LocalFog, internalId);
      if (publicId <= 0) {
        static_cast<void>(war3shader::RemoveFogVolume(internalId));
        return Failure(ErrorCode::InternalError);
      }
      return SuccessInteger(publicId);
    }
    case CommandId::LocalFogCreateCylinder: {
      if (!IsValidFogPosition(a[0].real, a[1].real, a[2].real) ||
          !IsValidFogRadius(a[3].real) ||
          !IsValidFogFullSize(a[4].real) ||
          !IsValidFogDensity(a[5].real) ||
          !IsValidFogFeather(a[6].real))
        return BackendRejected();
      const int32_t internalId = war3shader::AddCylinderFogVolume(
          a[0].real, a[1].real, a[2].real,
          a[3].real, a[4].real, a[5].real, a[6].real);
      if (internalId <= 0)
        return BackendRejected();
      const int32_t publicId =
          RegisterObject(ManagedType::LocalFog, internalId);
      if (publicId <= 0) {
        static_cast<void>(war3shader::RemoveFogVolume(internalId));
        return Failure(ErrorCode::InternalError);
      }
      return SuccessInteger(publicId);
    }
    case CommandId::LocalFogDestroy: {
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::LocalFog, internalId) ||
          !war3shader::RemoveFogVolume(internalId))
        return BackendRejected();
      RemoveRegistryEntry(a[0].integer, ManagedType::LocalFog, internalId);
      return SuccessVoid();
    }
    case CommandId::LocalFogSetEnabled: {
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::LocalFog, internalId) ||
          !war3shader::SetFogVolumeEnabled(internalId, a[1].boolean))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LocalFogSetPosition: {
      if (!IsValidFogPosition(a[1].real, a[2].real, a[3].real))
        return BackendRejected();
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::LocalFog, internalId) ||
          !war3shader::SetFogVolumePosition(
              internalId, a[1].real, a[2].real, a[3].real))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LocalFogSetRotation: {
      if (!IsValidFogRotation(a[1].real) ||
          !IsValidFogRotation(a[2].real) ||
          !IsValidFogRotation(a[3].real))
        return BackendRejected();
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::LocalFog, internalId) ||
          !war3shader::SetFogVolumeRotation(
              internalId, a[1].real, a[2].real, a[3].real))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LocalFogSetDensity: {
      if (!IsValidFogDensity(a[1].real))
        return BackendRejected();
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::LocalFog, internalId) ||
          !war3shader::SetFogVolumeDensity(internalId, a[1].real))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LocalFogSetEdgeFeather: {
      if (!IsValidFogFeather(a[1].real))
        return BackendRejected();
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::LocalFog, internalId) ||
          !war3shader::SetFogVolumeEdgeFeather(internalId, a[1].real))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LocalFogSetSphereRadius: {
      if (!IsValidFogRadius(a[1].real))
        return BackendRejected();
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::LocalFog, internalId) ||
          !war3shader::SetSphereFogVolumeRadius(internalId, a[1].real))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LocalFogSetBoxSize: {
      if (!IsValidFogFullSize(a[1].real) ||
          !IsValidFogFullSize(a[2].real) ||
          !IsValidFogFullSize(a[3].real))
        return BackendRejected();
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::LocalFog, internalId) ||
          !war3shader::SetBoxFogVolumeSize(
              internalId, a[1].real, a[2].real, a[3].real))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LocalFogSetCylinderSize: {
      if (!IsValidFogRadius(a[1].real) || !IsValidFogFullSize(a[2].real))
        return BackendRejected();
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::LocalFog, internalId) ||
          !war3shader::SetCylinderFogVolumeSize(
              internalId, a[1].real, a[2].real))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LocalFogIsAlive: {
      int32_t internalId = 0;
      return SuccessInteger(
          ResolveObject(a[0].integer, ManagedType::LocalFog, internalId)
              ? 1 : 0);
    }
    case CommandId::DayNightSetEnabled:
      settings->dayNight.enabled = a[0].boolean;
      settings->dayNight.celestialMotionEnabled = a[0].boolean;
      settings->dayNight.timeColorGradingEnabled = a[0].boolean;
      AdvanceLightingClockRevision(settings->dayNight);
      return SuccessVoid();
    case CommandId::DayNightSetTime:
      return HoldLightingTime(settings->dayNight, a[0].real)
          ? SuccessVoid() : BackendRejected();
    case CommandId::DayNightSetSpeed:
      if (a[0].real < 0.0f || a[0].real > 480.0f)
        return BackendRejected();
      if (a[0].real == 0.0f) {
        settings->dayNight.clockMode = War3LightingClockMode::Held;
      } else {
        settings->dayNight.clockMode = War3LightingClockMode::Independent;
        settings->dayNight.independentDayLengthSeconds =
            std::clamp(480.0f / a[0].real, 1.0f, 86400.0f);
      }
      AdvanceLightingClockRevision(settings->dayNight);
      return SuccessVoid();
    case CommandId::LightingClockSetMode:
      if (a[0].integer < 0 || a[0].integer > 2)
        return BackendRejected();
      settings->dayNight.clockMode =
          static_cast<War3LightingClockMode>(a[0].integer);
      AdvanceLightingClockRevision(settings->dayNight);
      return SuccessVoid();
    case CommandId::LightingClockHoldTime:
      return HoldLightingTime(settings->dayNight, a[0].real)
          ? SuccessVoid() : BackendRejected();
    case CommandId::LightingClockSetDayDuration:
      if (a[0].real < 1.0f || a[0].real > 86400.0f)
        return BackendRejected();
      settings->dayNight.independentDayLengthSeconds = a[0].real;
      AdvanceLightingClockRevision(settings->dayNight);
      return SuccessVoid();
    case CommandId::LightingCycleSetCelestialMotionEnabled:
      settings->dayNight.celestialMotionEnabled = a[0].boolean;
      return SuccessVoid();
    case CommandId::LightingCycleSetTimeColorGradingEnabled:
      settings->dayNight.timeColorGradingEnabled = a[0].boolean;
      settings->dayNight.enabled = a[0].boolean;
      return SuccessVoid();
    case CommandId::LightingCycleSetColorTemperatureProfile:
      for (size_t index = 0u; index < 4u; ++index) {
        if (a[index].real < 1000.0f || a[index].real > 20000.0f)
          return BackendRejected();
      }
      settings->dayNight.midnightKelvin = a[0].real;
      settings->dayNight.dawnKelvin = a[1].real;
      settings->dayNight.noonKelvin = a[2].real;
      settings->dayNight.duskKelvin = a[3].real;
      settings->dayNight.customColorTemperatureProfile = true;
      return SuccessVoid();
    case CommandId::LightingCycleResetColorTemperatureProfile:
      settings->dayNight.customColorTemperatureProfile = false;
      settings->dayNight.midnightKelvin = 9000.0f;
      settings->dayNight.dawnKelvin = 2500.0f;
      settings->dayNight.noonKelvin = 6500.0f;
      settings->dayNight.duskKelvin = 2500.0f;
      return SuccessVoid();
    case CommandId::MathProgramCompile: {
      const int32_t programId =
          math::CurveRuntime::instance().compileProgram(a[0].text);
      return programId > 0 ? SuccessInteger(programId) : BackendRejected();
    }
    case CommandId::MathProgramDestroy:
      return math::CurveRuntime::instance().destroyProgram(a[0].integer)
          ? SuccessVoid() : BackendRejected();
    case CommandId::MathProgramIsAlive:
      return SuccessInteger(
          math::CurveRuntime::instance().isProgramAlive(a[0].integer)
              ? 1 : 0);
    case CommandId::MathProgramLastError:
      return SuccessText(math::CurveRuntime::instance().lastCompileError());
    case CommandId::MathEvaluateReal: {
      float value = 0.0f;
      return EvaluateAuthorScalar(
          a[0].integer, a[1].real, a[2].real, a[3].integer, value)
          ? SuccessReal(value) : BackendRejected();
    }
    case CommandId::MathEvaluateInteger: {
      float value = 0.0f;
      int32_t integer = 0;
      return EvaluateAuthorScalar(
                 a[0].integer, a[1].real, a[2].real, a[3].integer, value) &&
             ConvertAuthorScalarToInteger(value, a[4].integer, integer)
          ? SuccessInteger(integer) : BackendRejected();
    }
    case CommandId::CurveCreate: {
      const int32_t curveId =
          math::CurveRuntime::instance().createCurve(a[0].integer);
      return curveId > 0 ? SuccessInteger(curveId) : BackendRejected();
    }
    case CommandId::CurveDestroy:
      return math::CurveRuntime::instance().destroyCurve(a[0].integer)
          ? SuccessVoid() : BackendRejected();
    case CommandId::CurveSetReal:
      return math::CurveRuntime::instance().setCurveReal(
          a[0].integer, a[1].text, a[2].real)
          ? SuccessVoid() : BackendRejected();
    case CommandId::CurveSetCoordinateMode:
      return math::CurveRuntime::instance().setCurveCoordinateMode(
          a[0].integer, a[1].integer)
          ? SuccessVoid() : BackendRejected();
    case CommandId::CurveSetEndpointLocks:
      return math::CurveRuntime::instance().setCurveEndpointLocks(
          a[0].integer, a[1].boolean, a[2].boolean)
          ? SuccessVoid() : BackendRejected();
    case CommandId::CurveEvaluateComponent:
    case CommandId::CurveDerivativeComponent: {
      if (a[2].real < 0.0f || a[2].real > 1.0f)
        return BackendRejected();
      math::CurveContext context;
      context.t = a[2].real;
      context.time = a[3].real;
      context.start = {a[4].real, a[5].real, a[6].real};
      context.end = {a[7].real, a[8].real, a[9].real};
      context.seed = static_cast<uint32_t>(a[10].integer);
      context.segments = 64u;
      context.index = static_cast<uint32_t>(std::lround(context.t * 64.0f));
      float value = 0.0f;
      const bool evaluated = request.spec->id ==
              CommandId::CurveEvaluateComponent
          ? math::CurveRuntime::instance().evaluateComponent(
                a[0].integer, context, a[1].integer, value)
          : math::CurveRuntime::instance().evaluateDerivativeComponent(
                a[0].integer, context, a[1].integer, value);
      return evaluated ? SuccessReal(value) : BackendRejected();
    }
    case CommandId::CurveArcLength: {
      if (a[9].integer < 2 || a[9].integer > 256)
        return BackendRejected();
      math::CurveContext context;
      context.time = a[1].real;
      context.start = {a[2].real, a[3].real, a[4].real};
      context.end = {a[5].real, a[6].real, a[7].real};
      context.seed = static_cast<uint32_t>(a[8].integer);
      context.segments = static_cast<uint32_t>(a[9].integer);
      float value = 0.0f;
      return math::CurveRuntime::instance().evaluateArcLength(
          a[0].integer, context, context.segments, value)
          ? SuccessReal(value) : BackendRejected();
    }
    case CommandId::CurvePointCreate: {
      if (a[0].integer < int32_t(math::kMinimumPointCurvePoints) ||
          a[0].integer > int32_t(math::kMaximumPointCurvePoints))
        return BackendRejected();
      const int32_t curveId = math::CurveRuntime::instance().createPointCurve(
          static_cast<uint32_t>(a[0].integer));
      return curveId > 0 ? SuccessInteger(curveId) : BackendRejected();
    }
    case CommandId::CurvePointAppend4: {
      if (a[1].integer < 1 || a[1].integer > 4)
        return BackendRejected();
      std::array<math::Vec3, 4u> points = {};
      for (size_t index = 0u; index < points.size(); ++index) {
        const size_t base = 2u + index * 3u;
        points[index] = {a[base].real, a[base + 1u].real,
                         a[base + 2u].real};
      }
      return math::CurveRuntime::instance().appendPointCurve(
          a[0].integer, points.data(), static_cast<uint32_t>(a[1].integer))
          ? SuccessVoid() : BackendRejected();
    }
    case CommandId::CurvePointFinalize:
      return math::CurveRuntime::instance().finalizePointCurve(a[0].integer)
          ? SuccessVoid() : BackendRejected();
    case CommandId::LightningCreate: {
      if (!IsValidColor(
              a[6].real, a[7].real, a[8].real, a[9].real, true) ||
          a[10].real <= 0.0f)
        return BackendRejected();
      render::War3LightningCreateDesc desc;
      desc.start = {a[0].real, a[1].real, a[2].real};
      desc.end = {a[3].real, a[4].real, a[5].real};
      auto& runtime = render::War3LightningRuntime::instance();
      const int32_t internalId = runtime.create(desc);
      if (internalId <= 0 ||
          !runtime.setColor(
              internalId,
              a[6].real, a[7].real, a[8].real, a[9].real,
              a[6].real, a[7].real, a[8].real, a[9].real) ||
          !runtime.setWidth(internalId, a[10].real, a[10].real)) {
        if (internalId > 0)
          static_cast<void>(runtime.destroy(internalId));
        return BackendRejected();
      }
      const int32_t publicId =
          RegisterObject(ManagedType::Lightning, internalId);
      if (publicId <= 0) {
        static_cast<void>(runtime.destroy(internalId));
        return Failure(ErrorCode::InternalError);
      }
      return SuccessInteger(publicId);
    }
    case CommandId::LightningDestroy: {
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::Lightning, internalId) ||
          !render::War3LightningRuntime::instance().destroy(internalId))
        return BackendRejected();
      RemoveRegistryEntry(a[0].integer, ManagedType::Lightning, internalId);
      return SuccessVoid();
    }
    case CommandId::LightningSetEnabled: {
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::Lightning, internalId) ||
          !render::War3LightningRuntime::instance().setEnabled(
              internalId, a[1].boolean))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LightningSetEndpoints: {
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::Lightning, internalId))
        return BackendRejected();
      const render::War3LightningPoint start{
          a[1].real, a[2].real, a[3].real};
      const render::War3LightningPoint end{
          a[4].real, a[5].real, a[6].real};
      if (!render::War3LightningRuntime::instance().move(
              internalId, start, end))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LightningSetColor: {
      if (!IsValidColor(
              a[1].real, a[2].real, a[3].real, a[4].real, true))
        return BackendRejected();
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::Lightning, internalId) ||
          !render::War3LightningRuntime::instance().setColor(
              internalId,
              a[1].real, a[2].real, a[3].real, a[4].real,
              a[1].real, a[2].real, a[3].real, a[4].real))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LightningSetWidth: {
      if (a[1].real <= 0.0f)
        return BackendRejected();
      int32_t internalId = 0;
      if (!ResolveObject(a[0].integer, ManagedType::Lightning, internalId) ||
          !render::War3LightningRuntime::instance().setWidth(
              internalId, a[1].real, a[1].real))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LightningIsAlive: {
      int32_t internalId = 0;
      return SuccessInteger(
          ResolveObject(a[0].integer, ManagedType::Lightning, internalId)
              ? 1 : 0);
    }
    case CommandId::LightningTemplateCreate: {
      if (!IsLightningTemplateName(a[0].text))
        return BackendRejected();
      const int32_t templateId =
          render::War3LightningRuntime::instance().createTemplate(a[0].text);
      return templateId > 0 ? SuccessInteger(templateId) : BackendRejected();
    }
    case CommandId::LightningTemplateSetBasic: {
      if (!IsLightningTexturePath(a[1].text) ||
          !IsValidColor(a[2].real, a[3].real, a[4].real, a[5].real, true) ||
          !IsValidColor(a[6].real, a[7].real, a[8].real, a[9].real, true))
        return BackendRejected();
      render::War3LightningTemplateBasicDesc desc = {};
      desc.texturePath = NormalizeLightningTexturePath(a[1].text);
      desc.startColor = {a[2].real, a[3].real, a[4].real, a[5].real};
      desc.endColor = {a[6].real, a[7].real, a[8].real, a[9].real};
      desc.startWidth = a[10].real;
      desc.endWidth = a[11].real;
      desc.uvTiling = a[12].real;
      desc.uvScrollSpeed = a[13].real;
      desc.renderMode = a[14].integer;
      return render::War3LightningRuntime::instance().setTemplateBasic(
          a[0].integer, desc) ? SuccessVoid() : BackendRejected();
    }
    case CommandId::LightningTemplateSetAdvanced: {
      render::War3LightningTemplateAdvancedDesc desc = {};
      desc.averageSegmentLength = a[1].real;
      desc.minimumSegments = static_cast<uint32_t>(a[2].integer);
      desc.maximumSegments = static_cast<uint32_t>(a[3].integer);
      desc.curveAmplitude = a[4].real;
      desc.noiseAmplitude = a[5].real;
      desc.noiseFrequency = a[6].real;
      desc.noiseScrollSpeed = a[7].real;
      desc.noiseOctaves = static_cast<uint32_t>(a[8].integer);
      desc.branchCount = static_cast<uint32_t>(a[9].integer);
      desc.branchLengthScale = a[10].real;
      desc.branchWidthScale = a[11].real;
      return render::War3LightningRuntime::instance().setTemplateAdvanced(
          a[0].integer, desc) ? SuccessVoid() : BackendRejected();
    }
    case CommandId::LightningTemplateSetOptional: {
      render::War3LightningTemplateOptionalDesc desc = {};
      desc.lifetimeSec = a[1].real;
      desc.fadeInSec = a[2].real;
      desc.fadeOutSec = a[3].real;
      desc.pulseAmplitude = a[4].real;
      desc.pulseFrequency = a[5].real;
      desc.pulseTravelSpeed = a[6].real;
      desc.flickerAmplitude = a[7].real;
      desc.flickerFrequencyHz = a[8].real;
      desc.glowWidthScale = a[9].real;
      desc.glowOpacity = a[10].real;
      return render::War3LightningRuntime::instance().setTemplateOptional(
          a[0].integer, desc) ? SuccessVoid() : BackendRejected();
    }
    case CommandId::LightningTemplateFinalize:
      return render::War3LightningRuntime::instance().finalizeTemplate(
          a[0].integer) ? SuccessVoid() : BackendRejected();
    case CommandId::LightningCreateFromTemplate: {
      render::War3LightningCreateDesc desc = {};
      desc.start = {a[1].real, a[2].real, a[3].real};
      desc.end = {a[4].real, a[5].real, a[6].real};
      const int32_t internalId =
          render::War3LightningRuntime::instance().createFromTemplate(
              a[0].integer, desc, static_cast<uint32_t>(a[7].integer));
      if (internalId <= 0)
        return BackendRejected();
      const int32_t publicId =
          RegisterObject(ManagedType::Lightning, internalId);
      if (publicId <= 0) {
        static_cast<void>(
            render::War3LightningRuntime::instance().destroy(internalId));
        return Failure(ErrorCode::InternalError);
      }
      return SuccessInteger(publicId);
    }
    case CommandId::LightningCreatePolylineFromTemplate: {
      math::CurveSnapshot curve;
      if (!math::CurveRuntime::instance().snapshotCurve(
              a[1].integer, curve) || !curve.isPointCurve())
        return BackendRejected();
      const int32_t internalId = render::War3LightningRuntime::instance()
          .createPolylineFromTemplate(
              a[0].integer, curve.pointCurve,
              static_cast<uint32_t>(a[2].integer));
      if (internalId <= 0)
        return BackendRejected();
      const int32_t publicId =
          RegisterObject(ManagedType::Lightning, internalId);
      if (publicId <= 0) {
        static_cast<void>(
            render::War3LightningRuntime::instance().destroy(internalId));
        return Failure(ErrorCode::InternalError);
      }
      return SuccessInteger(publicId);
    }
    case CommandId::LightningTemplateSetFormulaCurve: {
      math::CurveSnapshot curve;
      if (!math::CurveRuntime::instance().snapshotCurve(
              a[1].integer, curve) ||
          !render::War3LightningRuntime::instance().setTemplateFormulaCurve(
              a[0].integer, curve))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LightningSetFormulaCurve: {
      int32_t internalId = 0;
      math::CurveSnapshot curve;
      if (!ResolveObject(a[0].integer, ManagedType::Lightning, internalId) ||
          !math::CurveRuntime::instance().snapshotCurve(
              a[1].integer, curve) ||
          !render::War3LightningRuntime::instance().setFormulaCurve(
              internalId, curve))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::LightningSetPolylineCurve: {
      int32_t internalId = 0;
      math::CurveSnapshot curve;
      if (!ResolveObject(a[0].integer, ManagedType::Lightning, internalId) ||
          !math::CurveRuntime::instance().snapshotCurve(
              a[1].integer, curve) || !curve.isPointCurve() ||
          !render::War3LightningRuntime::instance().setPolylineCurve(
              internalId, curve.pointCurve))
        return BackendRejected();
      return SuccessVoid();
    }
    case CommandId::ManagedObjectCount:
      return SuccessInteger(ManagedObjectCount());
    case CommandId::ManagedObjectIsAlive:
      return SuccessInteger(ManagedObjectAlive(a[0].integer) ? 1 : 0);
    case CommandId::ManagedObjectType: {
      ManagedType type = ManagedType::PointLight;
      if (!ManagedObjectAlive(a[0].integer, &type))
        return SuccessInteger(0);
      return SuccessInteger(static_cast<int32_t>(type));
    }
    case CommandId::TimeVisualSeconds:
      return SuccessReal(war3shader::GetGameTime());
    case CommandId::TimeFrameIndex:
      return SuccessInteger(SaturateToInt32(
          dxvk::war3::state::RenderState::instance().getFrameIndex()));
    case CommandId::StatsFramesPerSecond: {
      const float frameTime = war3shader::GetFrameTime();
      const float fps = std::isfinite(frameTime) && frameTime > 1.0e-6f
          ? 1.0f / frameTime : 0.0f;
      return SuccessReal(fps);
    }
    case CommandId::StatsFrameTimeMilliseconds: {
      const float frameTime = war3shader::GetFrameTime();
      return SuccessReal(
          std::isfinite(frameTime) && frameTime >= 0.0f
              ? frameTime * 1000.0f : 0.0f);
    }
    case CommandId::StatsDrawCallCount: {
      uint32_t count = 0u;
      static_cast<void>(war3shader::GetDrawCalls(&count));
      return SuccessInteger(SaturateToInt32(count));
    }
    default:
      return Failure(ErrorCode::UnsupportedFeature);
  }
}
#else
Reply DispatchBackend(const ParsedRequest&) {
  return Failure(ErrorCode::BackendUnavailable);
}
#endif

uint32_t RequiredArgumentMask(const ParsedRequest& request) {
  if (!request.spec || request.argumentCount == 0u)
    return 0u;
  return (uint32_t(1u) << request.argumentCount) - 1u;
}

bool AssignTypedInteger(TypedTransaction& transaction, size_t slot,
                        int32_t value) {
  if (!transaction.request.spec ||
      slot >= transaction.request.argumentCount || slot >= 32u)
    return false;
  const uint32_t bit = uint32_t(1u) << slot;
  if ((transaction.writtenMask & bit) != 0u) {
    transaction.invalid = true;
    return true;
  }

  Argument& argument = transaction.request.arguments[slot];
  switch (transaction.request.spec->argumentTypes[slot]) {
    case 'b':
      argument.type = WireType::Bool;
      if (value != 0 && value != 1)
        transaction.invalid = true;
      argument.boolean = value != 0;
      break;
    case 'i':
      argument.type = WireType::I32;
      argument.integer = value;
      break;
    case 'd':
      argument.type = WireType::Id;
      if (value <= 0)
        transaction.invalid = true;
      argument.integer = value;
      break;
    default:
      transaction.invalid = true;
      break;
  }
  transaction.writtenMask |= bit;
  return true;
}

bool AssignTypedReal(TypedTransaction& transaction, size_t slot,
                     float value) {
  if (!transaction.request.spec ||
      slot >= transaction.request.argumentCount || slot >= 32u)
    return false;
  const uint32_t bit = uint32_t(1u) << slot;
  if ((transaction.writtenMask & bit) != 0u) {
    transaction.invalid = true;
    return true;
  }

  Argument& argument = transaction.request.arguments[slot];
  if (transaction.request.spec->argumentTypes[slot] != 'r' ||
      !std::isfinite(value)) {
    transaction.invalid = true;
  } else {
    argument.type = WireType::Real;
    argument.real = value;
  }
  transaction.writtenMask |= bit;
  return true;
}

Reply DispatchTypedRequest(const ParsedRequest& request) {
  if (!request.spec)
    return Failure(ErrorCode::InternalError);
  if (request.spec->backendRequired && !IsRuntimeReady())
    return Failure(ErrorCode::BackendUnavailable);
  if (request.spec->featureMask == 0u ||
      (kImplementedFeatureMask & request.spec->featureMask) !=
          request.spec->featureMask)
    return Failure(ErrorCode::UnsupportedFeature);
  return DispatchBackend(request);
}

bool IsTypedTransactionComplete(const TypedTransaction& transaction) {
  return !transaction.invalid && transaction.request.spec &&
         transaction.writtenMask == RequiredArgumentMask(transaction.request);
}

} // namespace

const char* GetErrorText(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::None: return "no error";
    case ErrorCode::PayloadTooLong: return "payload exceeds 512 bytes";
    case ErrorCode::NonAscii: return "payload contains a non-ASCII byte";
    case ErrorCode::ControlCharacter: return "payload contains a control character";
    case ErrorCode::EmptyToken: return "payload contains an empty token";
    case ErrorCode::TooManyArguments: return "payload exceeds 16 arguments";
    case ErrorCode::UnsupportedVersion: return "protocol version is unsupported";
    case ErrorCode::MissingCommand: return "command token is missing";
    case ErrorCode::UnknownCommand: return "command is not declared";
    case ErrorCode::CarrierMismatch: return "command used the wrong stock native carrier";
    case ErrorCode::ArgumentCountMismatch: return "argument count does not match the command";
    case ErrorCode::InvalidInteger: return "integer token is malformed";
    case ErrorCode::IntegerOverflow: return "integer token is outside int32 range";
    case ErrorCode::InvalidBoolean: return "boolean token must be b:0 or b:1";
    case ErrorCode::InvalidId: return "id token must be a positive int32";
    case ErrorCode::InvalidReal: return "real token is malformed or non-finite";
    case ErrorCode::RealOutOfRange: return "real token is outside the representable range";
    case ErrorCode::BackendUnavailable: return "visual backend is unavailable";
    case ErrorCode::UnsupportedFeature: return "visual feature is not supported by the backend";
    case ErrorCode::BackendRejected: return "visual backend rejected the command";
    case ErrorCode::InternalError: return "internal protocol error";
    case ErrorCode::BackendContractViolation: return "backend result violates the declared result type";
    case ErrorCode::InvalidArgumentType: return "typed argument tag is unknown or does not match the declared type";
  }
  return "internal protocol error";
}

uint32_t GetFeatureFlags() noexcept {
#if defined(WARVK_JAPI_PROTOCOL_TEST)
  return 0u;
#else
  return IsRuntimeReady() ? kImplementedFeatureMask : 0u;
#endif
}

bool IsRuntimeReady() noexcept {
#if defined(WARVK_JAPI_PROTOCOL_TEST)
  return false;
#else
  return dxvk::war3::HasActivePipeline();
#endif
}

Reply Dispatch(Carrier carrier, std::string_view payload) noexcept {
  try {
    if (payload.size() < kProtocolPrefix.size() ||
        payload.substr(0u, kProtocolPrefix.size()) != kProtocolPrefix)
      return {};

    ParsedRequest request;
    const ErrorCode parseError = ParseRequest(payload, carrier, request);
    if (parseError != ErrorCode::None)
      return Failure(parseError);

    switch (request.spec->id) {
      case CommandId::SystemVersion:
        return SuccessText(std::string(kApiVersion));
      case CommandId::SystemProtocolVersion:
        return SuccessInteger(1);
      case CommandId::SystemLastErrorCode:
        return SuccessInteger(static_cast<int32_t>(g_lastError), false);
      case CommandId::SystemLastError:
        return SuccessText(std::string(GetErrorText(g_lastError)), false);
      case CommandId::SystemClearError:
        return SuccessVoid();
      case CommandId::SystemFeatureFlags:
        return SuccessInteger(static_cast<int32_t>(GetFeatureFlags()));
      case CommandId::SystemRuntimeReady:
        return SuccessInteger(IsRuntimeReady() ? 1 : 0);
      default:
        break;
    }

    if (request.spec->backendRequired && !IsRuntimeReady())
      return Failure(ErrorCode::BackendUnavailable);
    if (request.spec->featureMask == 0u ||
        (kImplementedFeatureMask & request.spec->featureMask) !=
            request.spec->featureMask)
      return Failure(ErrorCode::UnsupportedFeature);
    Reply reply = DispatchBackend(request);
#if !defined(WARVK_JAPI_PROTOCOL_TEST)
    // The legacy string control plane keeps its v1 wire contract. The typed
    // data plane calls DispatchBackend directly and therefore retains the
    // native float without a format/parse round trip.
    if (reply.ok() && reply.kind == ResultKind::Real &&
        request.spec->carrier == Carrier::LocalizedString) {
      reply.kind = ResultKind::Text;
      reply.text = FormatReal(reply.real);
    }
#endif
    return reply;
  } catch (...) {
    return Failure(ErrorCode::InternalError);
  }
}

bool TryTypedSaveInteger(uint32_t table, int32_t parentKey,
                         int32_t childKey, int32_t value) noexcept {
  try {
    ParsedRequest request;
    bool dispatch = false;
    {
      std::unique_lock<std::mutex> lock(g_typedTransportMutex);

      // Registration writes are deliberately forwarded to Warcraft. This
      // makes the handshake observational if the optional data plane is not
      // installed and preserves the native hashtable contract.
      if (parentKey == kTypedRegisterParent &&
          childKey == kTypedRegisterChildA &&
          value == kTypedRegisterCookieA) {
        g_typedPendingTable = table;
        g_typedRegisterASeen = table != 0u;
        return false;
      }
      if (parentKey == kTypedRegisterParent &&
          childKey == kTypedRegisterChildB &&
          value == kTypedRegisterCookieB) {
        if (table != 0u && g_typedRegisterASeen &&
            table == g_typedPendingTable) {
          g_typedActiveTable = table;
          g_typedTransactions.clear();
        }
        g_typedPendingTable = 0u;
        g_typedRegisterASeen = false;
        return false;
      }

      if (table == 0u || table != g_typedActiveTable)
        return false;

      if (childKey == kTypedBeginChild) {
        const TypedCommandBinding* const binding = FindTypedCommand(value);
        if (!binding) {
          static_cast<void>(Failure(ErrorCode::UnknownCommand));
          return true;
        }
        const CommandSpec* const command = FindCommand(binding->command);
        if (!command || std::strchr(command->argumentTypes, 's')) {
          static_cast<void>(Failure(ErrorCode::InternalError));
          return true;
        }
        if (g_typedTransactions.find(parentKey) ==
                g_typedTransactions.end() &&
            g_typedTransactions.size() >= kMaximumTypedTransactions) {
          static_cast<void>(Failure(ErrorCode::BackendRejected));
          return true;
        }

        TypedTransaction transaction;
        transaction.opcode = value;
        transaction.request.spec = command;
        transaction.request.argumentCount =
            std::strlen(command->argumentTypes);
        g_typedTransactions[parentKey] = std::move(transaction);
        return true;
      }

      const auto found = g_typedTransactions.find(parentKey);
      if (found == g_typedTransactions.end())
        return false;

      TypedTransaction& transaction = found->second;
      if (childKey == kTypedCommitChild) {
        const TypedCommandBinding* const binding =
            FindTypedCommand(transaction.opcode);
        const bool valid = binding && value == transaction.opcode &&
                           binding->result == ResultKind::Void &&
                           IsTypedTransactionComplete(transaction);
        if (valid) {
          request = transaction.request;
          dispatch = true;
        }
        g_typedTransactions.erase(found);
        lock.unlock();
        if (!valid) {
          static_cast<void>(Failure(ErrorCode::InvalidArgumentType));
          return true;
        }
      } else if (childKey >= 0 &&
                 childKey < static_cast<int32_t>(kMaximumArgumentCount)) {
        static_cast<void>(AssignTypedInteger(
            transaction, static_cast<size_t>(childKey), value));
        return true;
      } else {
        return false;
      }
    }

    if (dispatch)
      static_cast<void>(DispatchTypedRequest(request));
    return true;
  } catch (...) {
    static_cast<void>(Failure(ErrorCode::InternalError));
    return true;
  }
}

bool TryTypedSaveReal(uint32_t table, int32_t parentKey,
                      int32_t childKey, float value) noexcept {
  try {
    std::lock_guard<std::mutex> lock(g_typedTransportMutex);
    if (table == 0u || table != g_typedActiveTable)
      return false;
    const auto found = g_typedTransactions.find(parentKey);
    if (found == g_typedTransactions.end())
      return false;
    if (childKey < 0 ||
        childKey >= static_cast<int32_t>(kMaximumArgumentCount))
      return false;
    static_cast<void>(AssignTypedReal(
        found->second, static_cast<size_t>(childKey), value));
    return true;
  } catch (...) {
    static_cast<void>(Failure(ErrorCode::InternalError));
    return true;
  }
}

bool TryTypedLoadInteger(uint32_t table, int32_t parentKey,
                         int32_t childKey, int32_t& value) noexcept {
  value = 0;
  try {
    ParsedRequest request;
    {
      std::unique_lock<std::mutex> lock(g_typedTransportMutex);
      if (table == 0u || table != g_typedActiveTable)
        return false;
      if (parentKey == kTypedRegisterParent &&
          childKey == kTypedProbeChild) {
        value = kTypedProbeAck;
        return true;
      }
      if (childKey != kTypedQueryIntegerChild)
        return false;

      const auto found = g_typedTransactions.find(parentKey);
      if (found == g_typedTransactions.end())
        return false;
      const TypedCommandBinding* const binding =
          FindTypedCommand(found->second.opcode);
      const bool valid = binding && binding->result == ResultKind::Integer &&
                         IsTypedTransactionComplete(found->second);
      if (valid)
        request = found->second.request;
      g_typedTransactions.erase(found);
      lock.unlock();
      if (!valid) {
        static_cast<void>(Failure(ErrorCode::InvalidArgumentType));
        return true;
      }
    }

    const Reply reply = DispatchTypedRequest(request);
    if (reply.ok() && reply.kind == ResultKind::Integer)
      value = reply.integer;
    else if (reply.ok())
      static_cast<void>(Failure(ErrorCode::BackendContractViolation));
    return true;
  } catch (...) {
    static_cast<void>(Failure(ErrorCode::InternalError));
    return true;
  }
}

bool TryTypedLoadReal(uint32_t table, int32_t parentKey,
                      int32_t childKey, float& value) noexcept {
  value = 0.0f;
  try {
    ParsedRequest request;
    {
      std::unique_lock<std::mutex> lock(g_typedTransportMutex);
      if (table == 0u || table != g_typedActiveTable ||
          childKey != kTypedQueryRealChild)
        return false;

      const auto found = g_typedTransactions.find(parentKey);
      if (found == g_typedTransactions.end())
        return false;
      const TypedCommandBinding* const binding =
          FindTypedCommand(found->second.opcode);
      const bool valid = binding && binding->result == ResultKind::Real &&
                         IsTypedTransactionComplete(found->second);
      if (valid)
        request = found->second.request;
      g_typedTransactions.erase(found);
      lock.unlock();
      if (!valid) {
        static_cast<void>(Failure(ErrorCode::InvalidArgumentType));
        return true;
      }
    }

    const Reply reply = DispatchTypedRequest(request);
    if (reply.ok() && reply.kind == ResultKind::Real) {
      value = reply.real;
    } else if (reply.ok()) {
      static_cast<void>(Failure(ErrorCode::BackendContractViolation));
    }
    return true;
  } catch (...) {
    static_cast<void>(Failure(ErrorCode::InternalError));
    return true;
  }
}

void ResetTypedTransport() noexcept {
  try {
    std::lock_guard<std::mutex> lock(g_typedTransportMutex);
    g_typedPendingTable = 0u;
    g_typedActiveTable = 0u;
    g_typedRegisterASeen = false;
    g_typedTransactions.clear();
  } catch (...) {
  }
}

void ResetAuthorState() noexcept {
  ResetTypedTransport();
#if !defined(WARVK_JAPI_PROTOCOL_TEST)
  std::vector<ManagedObject> objects;
  {
    std::lock_guard<std::mutex> lock(g_objectMutex);
    objects.reserve(g_objects.size());
    for (const auto& entry : g_objects)
      objects.push_back(entry.second);
    g_objects.clear();
    g_nextPublicId = 1;
  }
  for (const ManagedObject& object : objects) {
    if (object.type == ManagedType::PointLight)
      static_cast<void>(war3shader::RemovePointLight(object.internalId));
    else if (object.type == ManagedType::LocalFog)
      static_cast<void>(war3shader::RemoveFogVolume(object.internalId));
  }
  // Records and templates are map-scoped author data. Texture cache ownership
  // remains with the renderer and is retired only by the Present transaction.
  render::War3LightningRuntime::instance().resetAuthorState();
  // Programs and mutable curve handles are map-scoped as well. Lightning
  // records already own immutable snapshots, so reset order is intentional.
  math::CurveRuntime::instance().reset();
#endif
  g_lastError = ErrorCode::None;
}

void Reset() noexcept {
  ResetAuthorState();
}

void NoteTransportFailure() noexcept {
  g_lastError = ErrorCode::InternalError;
}

} // namespace dxvk::war3::japi
