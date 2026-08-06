#pragma once

#include "war3_jass_native_plan_cache.h"

#include <cstdint>
#include <string>

namespace dxvk::war3::hooks {

struct JassCommandBridgeSelfTestResult {
  bool installed = false;
  bool typedTransportInstalled = false;
  bool preloaderOk = false;
  bool intQueryOk = false;
  bool stringQueryOk = false;
  bool publicV1Ok = false;
  bool displayTextAttempted = false;
  bool displayTextOk = false;
  int pingCode = 0;
  int publicProtocolVersion = 0;
  uint32_t versionStringHandle = 0;
  uint32_t publicVersionStringHandle = 0;
  uint32_t playerHandle = 0;
  std::string versionText;
  std::string publicVersionText;
  std::string error;
};

struct JassPauseGameTestResult {
  bool resolved = false;
  bool signatureValidated = false;
  bool invoked = false;
  bool paused = false;
  uint32_t paramCount = 0;
  uint32_t returnType = 0;
  std::string signature;
  std::string error;
};

struct JassCameraFieldTestResult {
  bool convertResolved = false;
  bool setResolved = false;
  bool signaturesValidated = false;
  bool invoked = false;
  float angleDegrees = 0.0f;
  uint32_t cameraFieldHandle = 0;
  uint32_t convertParamCount = 0;
  uint32_t convertReturnType = 0;
  uint32_t setParamCount = 0;
  uint32_t setReturnType = 0;
  std::string convertSignature;
  std::string setSignature;
  std::string error;
};

struct JassFixedCameraTestResult {
  bool convertResolved = false;
  bool setResolved = false;
  bool positionResolved = false;
  bool signaturesValidated = false;
  bool positionInvoked = false;
  bool invoked = false;
  float targetX = 0.0f;
  float targetY = 0.0f;
  float targetDistance = 0.0f;
  float angleDegrees = 0.0f;
  float rotationDegrees = 0.0f;
  float fieldOfViewDegrees = 0.0f;
  float farZ = 0.0f;
  float rollDegrees = 0.0f;
  float zOffset = 0.0f;
  uint32_t cameraFieldHandles[7] = {};
  uint32_t fieldInvocations = 0;
  uint32_t convertParamCount = 0;
  uint32_t convertReturnType = 0;
  uint32_t setParamCount = 0;
  uint32_t setReturnType = 0;
  uint32_t positionParamCount = 0;
  uint32_t positionReturnType = 0;
  std::string convertSignature;
  std::string setSignature;
  std::string positionSignature;
  std::string error;
};

struct JassCameraSnapshotTestResult {
  bool resolved = false;
  bool signaturesValidated = false;
  bool invoked = false;
  float targetX = 0.0f;
  float targetY = 0.0f;
  float targetZ = 0.0f;
  float targetDistance = 0.0f;
  float farZ = 0.0f;
  float angleOfAttack = 0.0f;
  float fieldOfView = 0.0f;
  float roll = 0.0f;
  float rotation = 0.0f;
  float zOffset = 0.0f;
  uint32_t cameraFieldHandles[7] = {};
  uint32_t fieldInvocations = 0;
  std::string error;
};

struct JassCameraApplyTestResult {
  bool resolved = false;
  bool signaturesValidated = false;
  bool positionInvoked = false;
  bool invoked = false;
  bool quickPosition = true;
  float targetX = 0.0f;
  float targetY = 0.0f;
  float duration = 0.0f;
  uint32_t fieldInvocations = 0;
  std::string error;
};

struct JassCameraPanTestResult {
  bool resolved = false;
  bool signatureValidated = false;
  bool invoked = false;
  float targetX = 0.0f;
  float targetY = 0.0f;
  float duration = 0.0f;
  std::string signature;
  std::string error;
};

struct JassWorldBoundsTestResult {
  bool resolved = false;
  bool signaturesValidated = false;
  bool invoked = false;
  uint32_t rectHandle = 0;
  float minX = 0.0f;
  float minY = 0.0f;
  float maxX = 0.0f;
  float maxY = 0.0f;
  std::string error;
};

struct JassVisibilityTestResult {
  bool resolved = false;
  bool signaturesValidated = false;
  bool invoked = false;
  bool enabled = false;
  bool leaseActive = false;
  bool capturedOriginal = false;
  bool fogBefore = false;
  bool fogMaskBefore = false;
  bool fogAfter = false;
  bool fogMaskAfter = false;
  std::string error;
};

/**
 * WarVK JASS command bridge.
 *
 * This bridge does not register new natives and does not patch JASS bytecode.
 * Its compatibility/control plane wraps three low-frequency string carriers
 * and only consumes calls whose first argument starts with "warvk:". An
 * optional, independently designed numeric data plane wraps the stock
 * hashtable Save/Load natives. That path requires a private-table capability
 * handshake and otherwise forwards the original native unchanged.
 */
void ConfigureJassCommandBridge(GetTlsJassDataFn lookupFn);

/**
 * Mark carrier entries as needing reinstall.
 * Call after War3 rebuilds the native table, e.g. after InitJassNatives.
 */
void ResetJassCommandBridgeInstallState();

/**
 * Try to patch carrier native table entries.
 * Safe to call multiple times; non-ready native tables simply no-op.
 */
void TryInstallJassCommandBridge(const char *reason);

bool IsJassCommandBridgeInstalled();
bool IsJassTypedTransportInstalled();

/**
 * Directly invoke War3 native table entries to simulate JASS calling the
 * wrapped carriers. Optionally calls DisplayTextToPlayer through the original
 * native ABI as a visible smoke test.
 */
JassCommandBridgeSelfTestResult RunJassCommandBridgeSelfTest(
    bool displayText);

/**
 * Resolve and invoke the stock PauseGame(boolean) native on the render/main
 * thread. This is only exposed through the internal test control plane; it
 * does not install a hook or alter normal gameplay behavior.
 */
JassPauseGameTestResult SetJassGamePausedForTest(bool paused);

/**
 * Resolve ConvertCameraField/SetCameraField and force only the local camera's
 * angle-of-attack field. Map simulation and timer callbacks remain live, so
 * this can distinguish camera-angle-sensitive shadow behavior from a paused
 * scene. The caller may refresh the value while a scripted camera is active.
 */
JassCameraFieldTestResult SetJassCameraAngleOfAttackForTest(
    float angleDegrees);

/**
 * Pin all camera inputs used by the bridge/ramp visual diagnostic while map
 * simulation remains live. This uses only stock JASS natives and validates
 * their metadata before invoking the 32-bit real-pointer ABI.
 */
JassFixedCameraTestResult SetJassFixedCameraForTest(
    float targetX, float targetY, float targetDistance, float angleDegrees,
    float rotationDegrees, float fieldOfViewDegrees, float farZ,
    float rollDegrees, float zOffset);

/** Read the local camera through stock camera getter natives. */
JassCameraSnapshotTestResult SnapshotJassCameraForTest();

/** Apply a complete local camera state with a bounded transition duration. */
JassCameraApplyTestResult ApplyJassCameraForTest(
    float targetX, float targetY, float targetDistance, float angleOfAttack,
    float rotation, float fieldOfView, float farZ, float roll, float zOffset,
    float duration, bool quickPosition);

/** Pan the local camera through the stock PanCameraToTimed native. */
JassCameraPanTestResult PanJassCameraForTest(
    float targetX, float targetY, float duration);

/** Query the game-owned world-bounds rect without destroying it. */
JassWorldBoundsTestResult QueryJassWorldBoundsForTest();

/** Acquire or release the process-local AutoTest fog/fog-mask lease. */
JassVisibilityTestResult SetJassFullMapVisibilityForTest(bool enabled);

} // namespace dxvk::war3::hooks
