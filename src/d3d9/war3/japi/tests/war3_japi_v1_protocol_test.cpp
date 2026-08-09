#include "../war3_japi_v1.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

using dxvk::war3::japi::Carrier;
using dxvk::war3::japi::Dispatch;
using dxvk::war3::japi::ErrorCode;
using dxvk::war3::japi::TypedOpcode;

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
  Check(version.text == "WarVK JAPI 1.2003",
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

  const auto validLightingClock = Dispatch(
      Carrier::Preloader, "warvk:v1;lightingClock.holdTime;r:16.5");
  Check(validLightingClock.error == ErrorCode::BackendUnavailable,
        "lighting clock time must pass strict protocol parsing");
  const auto validTemperatureProfile = Dispatch(
      Carrier::Preloader,
      "warvk:v1;lightingCycle.setColorTemperatureProfile;"
      "r:9000;r:2500;r:6500;r:2800");
  Check(validTemperatureProfile.error == ErrorCode::BackendUnavailable,
        "temperature profile must pass strict protocol parsing");
  CheckError(Carrier::Preloader,
             "warvk:v1;lightingClock.setMode;r:1",
             ErrorCode::InvalidInteger,
             "lighting clock mode must require an integer token");
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
             ErrorCode::InvalidBoolean,
             "string type must not satisfy a boolean argument");

  const auto validTemplate = Dispatch(
      Carrier::Hotkey, "warvk:v1;lightning.template.create;s:SmokeBlue");
  Check(validTemplate.error == ErrorCode::BackendUnavailable,
        "template create must accept the strict string wire form");

  std::string localizedTemplate =
      "warvk:v1;lightning.template.create;s:";
  localizedTemplate.push_back(static_cast<char>(0xd6));
  localizedTemplate.push_back(static_cast<char>(0xd0));
  localizedTemplate.push_back(static_cast<char>(0xce));
  localizedTemplate.push_back(static_cast<char>(0xc4));
  const auto localizedTemplateReply =
      Dispatch(Carrier::Hotkey, localizedTemplate);
  Check(localizedTemplateReply.error == ErrorCode::BackendUnavailable,
        "localized legacy template names must pass the narrow compatibility gate");
  CheckError(Carrier::Hotkey,
             "warvk:v1;lightning.template.create;s:Smoke;Blue",
             ErrorCode::ArgumentCountMismatch,
             "template strings must not escape the semicolon token boundary");

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

void TestMathCurveWireContract() {
  const auto compile = Dispatch(
      Carrier::Hotkey,
      "warvk:v1;math.program.compile;s:vec2(cos(t*tau)*radius,"
      "sin(t*tau)*radius)");
  Check(compile.error == ErrorCode::BackendUnavailable,
        "formula compile must pass strict wire parsing");

  const auto setReal = Dispatch(
      Carrier::Preloader,
      "warvk:v1;curve.setReal;d:1;s:radius;r:120");
  Check(setReal.error == ErrorCode::BackendUnavailable,
        "named curve parameter must pass strict wire parsing");

  const auto evaluate = Dispatch(
      Carrier::LocalizedString,
      "warvk:v1;curve.evaluateComponent;d:1;i:0;r:0.5;r:0;"
      "r:0;r:0;r:0;r:100;r:0;r:0;i:7");
  Check(evaluate.error == ErrorCode::BackendUnavailable,
        "curve component query must match its declared signature");

  const auto evaluateScalar = Dispatch(
      Carrier::LocalizedString,
      "warvk:v1;math.evaluateReal;d:1;r:0.5;r:2;i:7");
  Check(evaluateScalar.error == ErrorCode::BackendUnavailable,
        "scalar real query must match its declared signature");

  const auto evaluateInteger = Dispatch(
      Carrier::Hotkey,
      "warvk:v1;math.evaluateInteger;d:1;r:0.5;r:2;i:7;i:0");
  Check(evaluateInteger.error == ErrorCode::BackendUnavailable,
        "scalar integer query must match its declared signature");

  const auto arcLength = Dispatch(
      Carrier::LocalizedString,
      "warvk:v1;curve.arcLength;d:1;r:0;r:0;r:0;r:0;"
      "r:100;r:0;r:0;i:7;i:32");
  Check(arcLength.error == ErrorCode::BackendUnavailable,
        "curve arc-length query must match its declared signature");

  const auto bindTemplate = Dispatch(
      Carrier::Preloader,
      "warvk:v1;lightning.template.setFormulaCurve;d:1;d:2");
  Check(bindTemplate.error == ErrorCode::BackendUnavailable,
        "template curve binding must pass strict wire parsing");

  const auto createPoints = Dispatch(
      Carrier::Hotkey, "warvk:v1;curve.points.create;i:640");
  Check(createPoints.error == ErrorCode::BackendUnavailable,
        "point-curve creation must pass strict wire parsing");

  const auto appendPoints = Dispatch(
      Carrier::Preloader,
      "warvk:v1;curve.points.append4;d:1;i:4;"
      "r:0;r:1;r:2;r:3;r:4;r:5;r:6;r:7;r:8;r:9;r:10;r:11");
  Check(appendPoints.error == ErrorCode::BackendUnavailable,
        "four-point chunk must fit the strict 16-token transport cap");

  const auto createPolyline = Dispatch(
      Carrier::Hotkey,
      "warvk:v1;lightning.createPolylineFromTemplate;d:1;d:2;i:7");
  Check(createPolyline.error == ErrorCode::BackendUnavailable,
        "polyline lightning creation must pass strict wire parsing");

  CheckError(Carrier::Hotkey,
             "warvk:v1;math.program.compile;s:vec2(t,0);s:extra",
             ErrorCode::ArgumentCountMismatch,
             "formula strings must not escape the semicolon boundary");
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

void RegisterTypedTable(uint32_t table) {
  using namespace dxvk::war3::japi;
  Check(!TryTypedSaveInteger(
            table, kTypedRegisterParent, kTypedRegisterChildA,
            kTypedRegisterCookieA),
        "typed registration A must preserve stock SaveInteger forwarding");
  Check(!TryTypedSaveInteger(
            table, kTypedRegisterParent, kTypedRegisterChildB,
            kTypedRegisterCookieB),
        "typed registration B must preserve stock SaveInteger forwarding");
}

void TestTypedHashtableTransport() {
  using namespace dxvk::war3::japi;
  constexpr uint32_t table = 0x12345678u;
  constexpr uint32_t otherTable = 0x87654321u;
  int32_t integer = -1;
  float real = -1.0f;

  ResetTypedTransport();
  Check(!TryTypedLoadInteger(
            table, kTypedRegisterParent, kTypedProbeChild, integer),
        "unregistered table must forward LoadInteger");
  RegisterTypedTable(table);
  Check(TryTypedLoadInteger(
            table, kTypedRegisterParent, kTypedProbeChild, integer) &&
            integer == kTypedProbeAck,
        "registered table must receive the typed capability ack");
  Check(!TryTypedLoadInteger(
            otherTable, kTypedRegisterParent, kTypedProbeChild, integer),
        "capability must be bound to the exact hashtable handle");

  constexpr int32_t positionTx = 11;
  const int32_t positionOpcode =
      static_cast<int32_t>(TypedOpcode::PointLightSetPosition);
  Check(TryTypedSaveInteger(
            table, positionTx, kTypedBeginChild, positionOpcode),
        "typed point position begin must be consumed");
  Check(TryTypedSaveInteger(table, positionTx, 0, 7),
        "typed id slot must be consumed");
  Check(TryTypedSaveReal(table, positionTx, 1, 100.0f) &&
            TryTypedSaveReal(table, positionTx, 2, 200.0f) &&
            TryTypedSaveReal(table, positionTx, 3, 300.0f),
        "typed position real slots must be consumed");
  Check(TryTypedSaveInteger(
            table, positionTx, kTypedCommitChild, positionOpcode),
        "complete typed setter commit must be consumed atomically");
  const auto setterError =
      Dispatch(Carrier::Hotkey, "warvk:v1;system.lastErrorCode");
  Check(setterError.integer ==
            static_cast<int32_t>(ErrorCode::BackendUnavailable),
        "typed setter must reach the protocol-test backend stub");

  constexpr int32_t realTx = 12;
  const int32_t realOpcode =
      static_cast<int32_t>(TypedOpcode::MathEvaluateReal);
  Check(TryTypedSaveInteger(table, realTx, kTypedBeginChild, realOpcode) &&
            TryTypedSaveInteger(table, realTx, 0, 1) &&
            TryTypedSaveReal(table, realTx, 1, 0.5f) &&
            TryTypedSaveReal(table, realTx, 2, 2.0f) &&
            TryTypedSaveInteger(table, realTx, 3, 7),
        "typed real query arguments must be accepted by declared type");
  Check(TryTypedLoadReal(
            table, realTx, kTypedQueryRealChild, real) && real == 0.0f,
        "typed real query must be consumed without creating a JASS string");

  constexpr int32_t integerTx = 14;
  const int32_t integerOpcode =
      static_cast<int32_t>(TypedOpcode::MathEvaluateInteger);
  Check(TryTypedSaveInteger(
            table, integerTx, kTypedBeginChild, integerOpcode) &&
            TryTypedSaveInteger(table, integerTx, 0, 1) &&
            TryTypedSaveReal(table, integerTx, 1, 0.5f) &&
            TryTypedSaveReal(table, integerTx, 2, 2.0f) &&
            TryTypedSaveInteger(table, integerTx, 3, 7) &&
            TryTypedSaveInteger(table, integerTx, 4, 0) &&
            TryTypedLoadInteger(
                table, integerTx, kTypedQueryIntegerChild, integer) &&
            integer == 0,
        "typed integer query must be consumed through LoadInteger");

  constexpr int32_t zeroArgTx = 15;
  const int32_t zeroArgOpcode =
      static_cast<int32_t>(TypedOpcode::TimeVisualSeconds);
  Check(TryTypedSaveInteger(
            table, zeroArgTx, kTypedBeginChild, zeroArgOpcode) &&
            TryTypedLoadReal(
                table, zeroArgTx, kTypedQueryRealChild, real),
        "zero-argument typed real query must close atomically");

  constexpr int32_t invalidTx = 13;
  Check(TryTypedSaveInteger(
            table, invalidTx, kTypedBeginChild, positionOpcode) &&
            TryTypedSaveInteger(table, invalidTx, 0, 7) &&
            TryTypedSaveReal(
                table, invalidTx, 1,
                std::numeric_limits<float>::quiet_NaN()) &&
            TryTypedSaveReal(table, invalidTx, 2, 2.0f) &&
            TryTypedSaveReal(table, invalidTx, 3, 3.0f) &&
            TryTypedSaveInteger(
                table, invalidTx, kTypedCommitChild, positionOpcode),
        "malformed typed transaction must remain consumed fail-closed");
  const auto invalidError =
      Dispatch(Carrier::Hotkey, "warvk:v1;system.lastErrorCode");
  Check(invalidError.integer ==
            static_cast<int32_t>(ErrorCode::InvalidArgumentType),
        "non-finite typed real must invalidate the whole transaction");

  ResetAuthorState();
  Check(!TryTypedLoadReal(
            table, realTx, kTypedQueryRealChild, real),
        "CPU-only author reset must revoke the previous hashtable capability");
}

} // namespace

int main() {
  TestSystemAndForwarding();
  TestStrictScalars();
  TestMessageShapeAndLimits();
  TestMathCurveWireContract();
  TestStableLastError();
  TestTypedHashtableTransport();
  if (g_failures != 0) {
    std::cerr << g_failures << " WarVK JAPI protocol test(s) failed\n";
    return 1;
  }
  std::cout << "WarVK JAPI v1 protocol tests passed\n";
  return 0;
}
