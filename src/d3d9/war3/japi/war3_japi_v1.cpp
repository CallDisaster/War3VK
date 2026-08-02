#include "war3_japi_v1.h"

#if !defined(WARVK_JAPI_PROTOCOL_TEST)
#include "../../d3d9_war3_light.h"
#include "../../d3d9_war3_settings.h"
#ifndef WAR3_SHADER_API_INTERNAL
#define WAR3_SHADER_API_INTERNAL 1
#endif
#include "../../war3_shader_api.h"
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
constexpr std::string_view kApiVersion = "WarVK JAPI 1.0.0-p0";
constexpr size_t kMaximumMessageBytes = 512u;
constexpr size_t kMaximumArgumentCount = 16u;

constexpr uint32_t kFeatureSun = 0x00000001u;
constexpr uint32_t kFeatureCsm = 0x00000002u;
constexpr uint32_t kFeaturePointLight = 0x00000004u;
constexpr uint32_t kFeatureLightning = 0x00000200u;
constexpr uint32_t kFeatureManagedObject = 0x00000400u;
constexpr uint32_t kFeatureTime = 0x00000800u;
constexpr uint32_t kFeatureStats = 0x00001000u;
constexpr uint32_t kImplementedFeatureMask =
    kFeatureSun | kFeatureCsm | kFeaturePointLight | kFeatureLightning |
    kFeatureManagedObject | kFeatureTime | kFeatureStats;

enum class WireType : uint8_t {
  Bool,
  I32,
  Id,
  Real,
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
  LightningCreate,
  LightningDestroy,
  LightningSetEnabled,
  LightningSetEndpoints,
  LightningSetColor,
  LightningSetWidth,
  LightningIsAlive,
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

constexpr std::array<CommandSpec, 55> kCommands = {{
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
    {CommandId::LightningCreate, "lightning.create", Carrier::Hotkey, "rrrrrrrrrrr", kFeatureLightning, true},
    {CommandId::LightningDestroy, "lightning.destroy", Carrier::Preloader, "d", kFeatureLightning, true},
    {CommandId::LightningSetEnabled, "lightning.setEnabled", Carrier::Preloader, "db", kFeatureLightning, true},
    {CommandId::LightningSetEndpoints, "lightning.setEndpoints", Carrier::Preloader, "drrrrrr", kFeatureLightning, true},
    {CommandId::LightningSetColor, "lightning.setColor", Carrier::Preloader, "drrrr", kFeatureLightning, true},
    {CommandId::LightningSetWidth, "lightning.setWidth", Carrier::Preloader, "dr", kFeatureLightning, true},
    {CommandId::LightningIsAlive, "lightning.isAlive", Carrier::Hotkey, "d", kFeatureLightning, true},
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
  if (token.size() < 3u || token[1] != ':')
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
  }
  return '\0';
}

ErrorCode TypeMismatchError(char expected) {
  switch (expected) {
    case 'b': return ErrorCode::InvalidBoolean;
    case 'i': return ErrorCode::InvalidInteger;
    case 'd': return ErrorCode::InvalidId;
    case 'r': return ErrorCode::InvalidReal;
    default: return ErrorCode::InternalError;
  }
}

ErrorCode ParseRequest(std::string_view payload, Carrier carrier,
                       ParsedRequest& output) {
  if (payload.size() > kMaximumMessageBytes)
    return ErrorCode::PayloadTooLong;
  for (const unsigned char character : payload) {
    if (character > 0x7fu)
      return ErrorCode::NonAscii;
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
  if (!settings)
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
#endif
  g_lastError = ErrorCode::None;
}

void NoteTransportFailure() noexcept {
  g_lastError = ErrorCode::InternalError;
}

} // namespace dxvk::war3::japi
