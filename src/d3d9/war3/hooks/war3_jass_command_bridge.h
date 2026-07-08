#pragma once

#include "war3_jass_native_plan_cache.h"

#include <cstdint>
#include <string>

namespace dxvk::war3::hooks {

struct JassCommandBridgeSelfTestResult {
  bool installed = false;
  bool preloaderOk = false;
  bool intQueryOk = false;
  bool stringQueryOk = false;
  bool displayTextAttempted = false;
  bool displayTextOk = false;
  int pingCode = 0;
  uint32_t versionStringHandle = 0;
  uint32_t playerHandle = 0;
  std::string versionText;
  std::string error;
};

/**
 * WarVK JASS command bridge.
 *
 * This bridge does not register new natives and does not patch JASS bytecode.
 * It wraps a few existing, low-frequency native carriers and only consumes
 * calls whose first string argument starts with "warvk:".
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

/**
 * Directly invoke War3 native table entries to simulate JASS calling the
 * wrapped carriers. Optionally calls DisplayTextToPlayer through the original
 * native ABI as a visible smoke test.
 */
JassCommandBridgeSelfTestResult RunJassCommandBridgeSelfTest(
    bool displayText);

} // namespace dxvk::war3::hooks
