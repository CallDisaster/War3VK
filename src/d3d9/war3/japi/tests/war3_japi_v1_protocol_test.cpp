#include "../war3_japi_v1.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

using dxvk::war3::japi::Carrier;
using dxvk::war3::japi::Dispatch;
using dxvk::war3::japi::ErrorCode;

int g_failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++g_failures;
  }
}

void CheckError(Carrier carrier, const std::string& payload,
                ErrorCode expected, const char* message) {
  const auto reply = Dispatch(carrier, payload);
  Check(reply.consumed, "WarVK namespace request must be consumed");
  Check(reply.error == expected, message);
}

void TestSystemAndForwarding() {
  const auto forwarded = Dispatch(Carrier::Hotkey, "not-warvk");
  Check(!forwarded.consumed, "non-WarVK text must retain forwarding marker");

  const auto version =
      Dispatch(Carrier::LocalizedString, "warvk:v1;system.version");
  Check(version.ok(), "system.version must succeed without a backend");
  Check(version.text == "WarVK JAPI 1.0.0-p0",
        "system.version must identify the integrated runtime");

  const auto protocol =
      Dispatch(Carrier::Hotkey, "warvk:v1;system.protocolVersion");
  Check(protocol.ok() && protocol.integer == 1,
        "system.protocolVersion must be one");
  CheckError(Carrier::Preloader, "warvk:v1;system.protocolVersion",
             ErrorCode::CarrierMismatch,
             "system.protocolVersion must reject the wrong carrier");
  CheckError(Carrier::Hotkey, "warvk:system.protocolVersion",
             ErrorCode::UnsupportedVersion,
             "unversioned public requests must remain parse-only");
}

void TestStrictScalars() {
  CheckError(Carrier::Preloader, "warvk:v1;csm.setLayout;i:+1;r:4000",
             ErrorCode::InvalidInteger, "leading plus must be rejected");
  CheckError(Carrier::Preloader, "warvk:v1;csm.setLayout;i:01;r:4000",
             ErrorCode::InvalidInteger, "integer leading zero must be rejected");
  CheckError(Carrier::Preloader, "warvk:v1;csm.setLayout;i:2147483648;r:4000",
             ErrorCode::IntegerOverflow, "int32 overflow must be distinct");
  CheckError(Carrier::Preloader, "warvk:v1;csm.setLayout;i:4;r:nan",
             ErrorCode::InvalidReal, "NaN must be rejected");
  CheckError(Carrier::Preloader, "warvk:v1;csm.setLayout;i:4;r:3.5e38",
             ErrorCode::RealOutOfRange, "float overflow must be distinct");
  CheckError(Carrier::Preloader, "warvk:v1;sun.setEnabled;b:true",
             ErrorCode::InvalidBoolean, "textual bool must be rejected");
  CheckError(Carrier::Hotkey, "warvk:v1;pointLight.isAlive;d:0",
             ErrorCode::InvalidId, "zero managed id must be rejected");
  CheckError(Carrier::Hotkey, "warvk:v1;pointLight.isAlive;i:1",
             ErrorCode::InvalidId, "typed-id mismatch must be rejected");

  const auto validBackendRequest = Dispatch(
      Carrier::Hotkey,
      "warvk:v1;pointLight.create;r:0;r:0;r:100;r:900;r:1;r:1;r:1;r:4");
  Check(validBackendRequest.error == ErrorCode::BackendUnavailable,
        "valid backend request must pass parsing before the test stub");
}

void TestMessageShapeAndLimits() {
  CheckError(Carrier::Hotkey, "warvk:v1;missing.command",
             ErrorCode::UnknownCommand, "unknown command must fail closed");
  CheckError(Carrier::Hotkey, "warvk:v1;system.protocolVersion;",
             ErrorCode::EmptyToken, "trailing empty token must fail");
  CheckError(Carrier::Hotkey, "warvk:v2;system.protocolVersion",
             ErrorCode::UnsupportedVersion, "unknown version must fail");
  CheckError(Carrier::Preloader, "warvk:v1;sun.setDirection;r:1;r:2",
             ErrorCode::ArgumentCountMismatch,
             "argument count mismatch must fail");
  CheckError(Carrier::Preloader, "warvk:v1;sun.setEnabled;s:1",
             ErrorCode::InvalidArgumentType,
             "unknown typed argument tag must fail");

  std::string tooMany = "warvk:v1;missing";
  for (int i = 0; i < 17; ++i)
    tooMany += ";i:0";
  CheckError(Carrier::Hotkey, tooMany, ErrorCode::TooManyArguments,
             "seventeen arguments must fail before command lookup");

  std::string maximum = "warvk:v1;";
  maximum.append(512u - maximum.size(), 'A');
  Check(maximum.size() == 512u, "maximum payload fixture must be 512 bytes");
  CheckError(Carrier::Hotkey, maximum, ErrorCode::UnknownCommand,
             "512-byte payload must be accepted by the size gate");
  maximum.push_back('A');
  CheckError(Carrier::Hotkey, maximum, ErrorCode::PayloadTooLong,
             "513-byte payload must fail");

  std::string nonAscii = "warvk:v1;system.protocolVersion";
  nonAscii.push_back(static_cast<char>(0x80));
  CheckError(Carrier::Hotkey, nonAscii, ErrorCode::NonAscii,
             "non-ASCII byte must fail");
  CheckError(Carrier::Hotkey, "warvk:v1;system.protocolVersion\n",
             ErrorCode::ControlCharacter, "control byte must fail");
}

void TestStableLastError() {
  CheckError(Carrier::Preloader, "warvk:v1;sun.setEnabled;b:2",
             ErrorCode::InvalidBoolean, "fixture must set last error");
  const auto code =
      Dispatch(Carrier::Hotkey, "warvk:v1;system.lastErrorCode");
  Check(code.ok() &&
            code.integer == static_cast<int32_t>(ErrorCode::InvalidBoolean),
        "lastErrorCode query must preserve and return the error");
  const auto text =
      Dispatch(Carrier::LocalizedString, "warvk:v1;system.lastError");
  Check(text.ok() && text.text == "boolean token must be b:0 or b:1",
        "lastError must use manifest-stable text");
  Check(Dispatch(Carrier::Preloader, "warvk:v1;system.clearError").ok(),
        "clearError must succeed");
  const auto cleared =
      Dispatch(Carrier::Hotkey, "warvk:v1;system.lastErrorCode");
  Check(cleared.ok() && cleared.integer == 0,
        "clearError must reset the thread-local code");
}

} // namespace

int main() {
  TestSystemAndForwarding();
  TestStrictScalars();
  TestMessageShapeAndLimits();
  TestStableLastError();
  if (g_failures != 0) {
    std::cerr << g_failures << " WarVK JAPI protocol test(s) failed\n";
    return 1;
  }
  std::cout << "WarVK JAPI v1 protocol tests passed\n";
  return 0;
}
