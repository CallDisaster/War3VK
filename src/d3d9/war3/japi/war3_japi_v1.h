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
  Text,
};

struct Reply {
  bool consumed = false;
  ErrorCode error = ErrorCode::None;
  ResultKind kind = ResultKind::Void;
  int32_t integer = 0;
  std::string text;

  bool ok() const {
    return consumed && error == ErrorCode::None;
  }
};

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
