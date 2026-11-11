#pragma once

#include <cstdint>

namespace dxvk::war3::game {

// Installed on supported 1.27a Game.dll identities. Installation checks the
// exact instruction contract for both automatic movement callback call sites.
bool InstallUnitWalkHook(uintptr_t gameBase) noexcept;
bool IsUnitWalkHookAvailable() noexcept;

// The argument is the complete JASS handle returned by GetHandleId(unit),
// not a model ID, CUnit pointer, or a raw handle-table index.
bool SetUnitWalkLoopEnabled(uint32_t jHandle, bool enabled) noexcept;
void ResetUnitWalkAuthorState() noexcept;

// Internal-test-only observation. Counters are populated only when the
// existing DXVK_WAR3_INTERNAL_TEST_API diagnostic mode is active; this is not
// another activation requirement for the public animation API.
struct UnitWalkDiagnostics {
  uint32_t installStatus = 0u;
  uint32_t setterCalls = 0u;
  uint32_t setterAccepted = 0u;
  uint32_t setterRejected = 0u;
  uint32_t lastSetterHandle = 0u;
  uint32_t lastSetterFailure = 0u;
  uint32_t lastSetterObservedType = 0u;
  uint32_t lastSetterDirectRawcode = 0u;
  uint32_t lastSetterDirectMethodRva = 0u;
  uint32_t lastSetterCurrentProperty = 0u;
  bool lastSetterCurrentPropertyKnown = false;
  uint32_t hookCalls = 0u;
  uint32_t autoCallerSeen = 0u;
  uint32_t autoRequestMatched = 0u;
  uint32_t autoRequestSuppressed = 0u;
  uint32_t lastSuppressedCurrentProperty = 0u;
  bool lastSuppressedCurrentPropertyKnown = false;
  uint32_t autoWithoutDisabledEntry = 0u;
  uint32_t autoIdentityRejected = 0u;
  uint32_t otherWalkRequests = 0u;
  uint32_t lastOtherWalkCallerRva = 0u;
  uint32_t disabledUnitCount = 0u;
};
UnitWalkDiagnostics QueryUnitWalkDiagnostics() noexcept;

} // namespace dxvk::war3::game
