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
constexpr std::string_view kApiVersion = "WarVK JAPI 1.3.0-polyline-curves";
constexpr size_t kMaximumMessageBytes = 512u;
constexpr size_t kMaximumArgumentCount = 16u;

constexpr uint32_t kFeatureSun = 0x00000001u;
constexpr uint32_t kFeatureCsm = 0x00000002u;
constexpr uint32_t kFeaturePointLight = 0x00000004u;
constexpr uint32_t kFeatureLightning = 0x00000200u;
constexpr uint32_t kFeatureManagedObject = 0x00000400u;
constexpr uint32_t kFeatureTime = 0x00000800u;
constexpr uint32_t kFeatureStats = 0x00001000u;
constexpr uint32_t kFeatureMathCurve = 0x00002000u;
constexpr uint32_t kFeaturePolylineCurve = 0x00004000u;
constexpr uint32_t kImplementedFeatureMask =
    kFeatureSun | kFeatureCsm | kFeaturePointLight | kFeatureLightning |
    kFeatureManagedObject | kFeatureTime | kFeatureStats |
    kFeatureMathCurve | kFeaturePolylineCurve;

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
  VolumetricSetDensity,
  VolumetricSetScattering,
  VolumetricSetQuality,
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
  MathProgramCompile,
  MathProgramDestroy,
  MathProgramIsAlive,
  MathProgramLastError,
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

constexpr std::array<CommandSpec, 80> kCommands = {{
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
    {CommandId::VolumetricSetEnabled, "volumetric.setEnabled", Carrier::Preloader, "b", 0x00000008u, true},
    {CommandId::VolumetricSetDensity, "volumetric.setDensity", Carrier::Preloader, "r", 0x00000008u, true},
    {CommandId::VolumetricSetScattering, "volumetric.setScattering", Carrier::Preloader, "rr", 0x00000008u, true},
    {CommandId::VolumetricSetQuality, "volumetric.setQuality", Carrier::Preloader, "ir", 0x00000008u, true},
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
    {CommandId::DayNightSetEnabled, "dayNight.setEnabled", Carrier::Preloader, "b", 0x00000100u, true},
    {CommandId::DayNightSetTime, "dayNight.setTime", Carrier::Preloader, "r", 0x00000100u, true},
    {CommandId::DayNightSetSpeed, "dayNight.setSpeed", Carrier::Preloader, "r", 0x00000100u, true},
    {CommandId::MathProgramCompile, "math.program.compile", Carrier::Hotkey, "s", kFeatureMathCurve, false},
    {CommandId::MathProgramDestroy, "math.program.destroy", Carrier::Preloader, "d", kFeatureMathCurve, false},
    {CommandId::MathProgramIsAlive, "math.program.isAlive", Carrier::Hotkey, "d", kFeatureMathCurve, false},
    {CommandId::MathProgramLastError, "math.program.lastError", Carrier::LocalizedString, "", kFeatureMathCurve, false},
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

#if !defined(WARVK_JAPI_PROTOCOL_TEST)
enum class ManagedType : int32_t {
  PointLight = 1,
  Lightning = 2,
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

Reply DispatchBackend(const ParsedRequest& request) {
  const auto& a = request.arguments;
  auto* const settings = dxvk::war3::GetMutableSettings();
  const bool mathCurveCpuCommand =
      request.spec->id >= CommandId::MathProgramCompile &&
      request.spec->id <= CommandId::CurvePointFinalize;
  const bool lightningCpuCommand =
      request.spec->id >= CommandId::LightningCreate &&
      request.spec->id <= CommandId::LightningSetPolylineCurve;
  if (!settings && !mathCurveCpuCommand && !lightningCpuCommand)
    return Failure(ErrorCode::BackendUnavailable);

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
      settings->shadows.pointLightsEnabled = true;
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
      if (a[1].boolean) {
        settings->shadows.pointLightsEnabled = true;
        settings->shadows.pointShadowEnabled = true;
      }
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
      return evaluated ? SuccessText(FormatReal(value)) : BackendRejected();
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
          ? SuccessText(FormatReal(value)) : BackendRejected();
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
      return SuccessText(FormatReal(war3shader::GetGameTime()));
    case CommandId::TimeFrameIndex:
      return SuccessInteger(SaturateToInt32(
          dxvk::war3::state::RenderState::instance().getFrameIndex()));
    case CommandId::StatsFramesPerSecond: {
      const float frameTime = war3shader::GetFrameTime();
      const float fps = std::isfinite(frameTime) && frameTime > 1.0e-6f
          ? 1.0f / frameTime : 0.0f;
      return SuccessText(FormatReal(fps));
    }
    case CommandId::StatsFrameTimeMilliseconds: {
      const float frameTime = war3shader::GetFrameTime();
      return SuccessText(FormatReal(
          std::isfinite(frameTime) && frameTime >= 0.0f
              ? frameTime * 1000.0f : 0.0f));
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
  return dxvk::war3::GetActivePipeline() != nullptr &&
         dxvk::war3::GetMutableSettings() != nullptr;
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
    return DispatchBackend(request);
  } catch (...) {
    return Failure(ErrorCode::InternalError);
  }
}

void Reset() noexcept {
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
    else if (object.type == ManagedType::Lightning)
      static_cast<void>(
          render::War3LightningRuntime::instance().destroy(object.internalId));
  }
  // Templates are map-scoped CPU descriptors. They are not managed objects,
  // so a JASS VM rebuild must explicitly drop them and their texture cache.
  render::War3LightningRuntime::instance().reset();
  // Programs and mutable curve handles are map-scoped as well. Lightning
  // records already own immutable snapshots, so reset order is intentional.
  math::CurveRuntime::instance().reset();
#endif
  g_lastError = ErrorCode::None;
}

void NoteTransportFailure() noexcept {
  g_lastError = ErrorCode::InternalError;
}

} // namespace dxvk::war3::japi
