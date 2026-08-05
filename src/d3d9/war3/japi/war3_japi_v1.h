#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dxvk::war3::japi {

enum class Carrier : uint8_t {
  Preloader,
  Hotkey,
  LocalizedString,
};

enum class ErrorCode : int32_t {
  None = 0,
  PayloadTooLong = 1,
  NonAscii = 2,
  ControlCharacter = 3,
  EmptyToken = 4,
  TooManyArguments = 5,
  UnsupportedVersion = 6,
  MissingCommand = 7,
  UnknownCommand = 8,
  CarrierMismatch = 9,
  ArgumentCountMismatch = 10,
  InvalidInteger = 11,
  IntegerOverflow = 12,
  InvalidBoolean = 13,
  InvalidId = 14,
  InvalidReal = 15,
  RealOutOfRange = 16,
  BackendUnavailable = 17,
  UnsupportedFeature = 18,
  BackendRejected = 19,
  InternalError = 20,
  BackendContractViolation = 21,
  InvalidArgumentType = 22,
};

enum class ResultKind : uint8_t {
  Void,
  Integer,
  Real,
  Text,
};

struct Reply {
  bool consumed = false;
  ErrorCode error = ErrorCode::None;
  ResultKind kind = ResultKind::Void;
  int32_t integer = 0;
  float real = 0.0f;
  std::string text;

  bool ok() const {
    return consumed && error == ErrorCode::None;
  }
};

// Optional numeric data plane built on Warcraft's stock hashtable natives.
// The string v1 protocol remains the compatibility/control plane. These
// constants form a private capability handshake and a bounded transaction
// envelope; they do not register a custom native or expose a raw pointer.
inline constexpr int32_t kTypedRegisterParent = 0x57564b54;
inline constexpr int32_t kTypedRegisterChildA = 0x52454731;
inline constexpr int32_t kTypedRegisterChildB = 0x52454732;
inline constexpr int32_t kTypedRegisterCookieA = 0x13572468;
inline constexpr int32_t kTypedRegisterCookieB = 0x24681357;
inline constexpr int32_t kTypedProbeChild = 0x50524f42;
inline constexpr int32_t kTypedProbeAck = 0x574b5632;
inline constexpr int32_t kTypedBeginChild = -2147418111;
inline constexpr int32_t kTypedCommitChild = -2147418110;
inline constexpr int32_t kTypedQueryIntegerChild = -2147418109;
inline constexpr int32_t kTypedQueryRealChild = -2147418108;

enum class TypedOpcode : int32_t {
  PointLightSetPosition = 101,
  PointLightSetColorIntensity = 102,
  PointLightSetRadius = 103,
  MathEvaluateReal = 201,
  MathEvaluateInteger = 202,
  CurveEvaluateComponent = 203,
  CurveDerivativeComponent = 204,
  CurveArcLength = 205,
  CurvePointAppend4 = 206,
  LightningSetEndpoints = 301,
  LightningSetColor = 302,
  LightningSetWidth = 303,
  TimeVisualSeconds = 401,
  StatsFramesPerSecond = 402,
  StatsFrameTimeMilliseconds = 403,
};

// Return true only when the bridge consumed the stock native call. A false
// result requires the caller to forward the call to Warcraft unchanged.
bool TryTypedSaveInteger(uint32_t table, int32_t parentKey,
                         int32_t childKey, int32_t value) noexcept;
bool TryTypedSaveReal(uint32_t table, int32_t parentKey,
                      int32_t childKey, float value) noexcept;
bool TryTypedLoadInteger(uint32_t table, int32_t parentKey,
                         int32_t childKey, int32_t& value) noexcept;
bool TryTypedLoadReal(uint32_t table, int32_t parentKey,
                      int32_t childKey, float& value) noexcept;
void ResetTypedTransport() noexcept;

// Strict public protocol entry point. Non-WarVK strings are never consumed.
Reply Dispatch(Carrier carrier, std::string_view payload) noexcept;

// Destroys only objects created through this JAPI generation and resets its
// thread-local error state. Call after War3 rebuilds the JASS VM/native table.
void Reset() noexcept;

// Carrier conversion failures are transport errors, not backend success.
void NoteTransportFailure() noexcept;

uint32_t GetFeatureFlags() noexcept;
bool IsRuntimeReady() noexcept;
const char* GetErrorText(ErrorCode code) noexcept;

} // namespace dxvk::war3::japi
