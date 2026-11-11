#include "war3_unit_animation_api.h"
#include "war3_unit_walk_policy.h"

#include "../core/war3_game_structs.h"
#include "../core/war3_memory.h"
#include "../handle/war3_handle_resolver.h"
#include "../hooks/war3_hook_install_util.h"
#include "war3_unit.h"

#include <atomic>
#include <cstring>

#include <windows.h>

namespace dxvk::war3::game {
namespace {

// One Game.dll 1.27a identity, independently checked against IDA and both
// local game copies (MD5 267861A0DFD416DBAD13E7EE3EC7794A). The byte
// guards below are mandatory: version 1.27.x alone is not an address proof.
constexpr uintptr_t kCUnitSetAnimationPropertyRva = 0x67BB90u;
constexpr uintptr_t kWalkCallARva = 0x64C752u;
constexpr uintptr_t kWalkCallBRva = 0x64C891u;
// IDA Game.dll+0x67BB90 writes this+0x2E0 when it selects an animation
// property. It is only read for a bounded internal-test observation.
constexpr size_t kCurrentAnimationPropertyOffset = 0x2E0u;

// The CUnit method returns a floating-point value in ST(0); its automatic
// Walk callers both execute `fstp st` immediately after the call. A skipped
// request must therefore return a real FPU value, not a C++ void.
using SetAnimationPropertyFn = double (__thiscall *)(void*, int, int, int);
SetAnimationPropertyFn g_originalSetAnimationProperty = nullptr;
std::atomic<bool> g_installed{false};
uintptr_t g_gameBase = 0u;
UnitWalkPolicy g_walkPolicy;
std::atomic<bool> g_traceEnabled{false};

// Install: 0=not attempted, 1=bad base, 2=instruction mismatch,
// 3=resolver unavailable, 4=MinHook failed, 5=installed.
std::atomic<uint32_t> g_installStatus{0u};
std::atomic<uint32_t> g_setterCalls{0u};
std::atomic<uint32_t> g_setterAccepted{0u};
std::atomic<uint32_t> g_setterRejected{0u};
std::atomic<uint32_t> g_lastSetterHandle{0u};
// Setter failure: 0=none, 1=hook unavailable, 2=bad handle,
// 3=table miss, 4=agent type, 5=unit pointer/readability,
// 6=dead unit, 7=policy table full.
std::atomic<uint32_t> g_lastSetterFailure{0u};
std::atomic<uint32_t> g_lastSetterObservedType{0u};
std::atomic<uint32_t> g_lastSetterDirectRawcode{0u};
std::atomic<uint32_t> g_lastSetterDirectMethodRva{0u};
std::atomic<uint32_t> g_lastSetterCurrentProperty{0u};
std::atomic<bool> g_lastSetterCurrentPropertyKnown{false};
std::atomic<uint32_t> g_hookCalls{0u};
std::atomic<uint32_t> g_autoCallerSeen{0u};
std::atomic<uint32_t> g_autoRequestMatched{0u};
std::atomic<uint32_t> g_autoRequestSuppressed{0u};
std::atomic<uint32_t> g_lastSuppressedCurrentProperty{0u};
std::atomic<bool> g_lastSuppressedCurrentPropertyKnown{false};
std::atomic<uint32_t> g_autoWithoutDisabledEntry{0u};
std::atomic<uint32_t> g_autoIdentityRejected{0u};
std::atomic<uint32_t> g_otherWalkRequests{0u};
std::atomic<uint32_t> g_lastOtherWalkCallerRva{0u};

bool ExactCode(uintptr_t address, const uint8_t* bytes, size_t count) noexcept {
  return IsReadableRange(reinterpret_cast<const void*>(address), count) &&
         IsExecutableRange(reinterpret_cast<const void*>(address), count) &&
         std::memcmp(reinterpret_cast<const void*>(address), bytes, count) == 0;
}

bool ResolveExactUnit(uint32_t jHandle, void*& unit,
                      void*& agent, uint32_t* failure = nullptr) noexcept {
  unit = nullptr;
  agent = nullptr;
  if (failure)
    *failure = 0u;
  if (!IsValidHandle(jHandle)) {
    if (failure)
      *failure = 2u;
    return false;
  }
  if (!HandleResolver::instance().resolveHandle(GetHandleId(jHandle), 0u,
                                                &agent) || !agent) {
    if (failure)
      *failure = 3u;
    return false;
  }

  uint32_t kind = 0u;
  if (!SafeReadU32(agent, CAgentOffsets::TypeFourCC, kind) ||
      kind != AgentTypeFourCC::Unit_LE) {
    uint32_t rawcode = 0u;
    void* vtable = nullptr;
    void* method = nullptr;
    const bool rawcodeKnown =
        SafeReadU32(agent, CUnitOffsets::Rawcode, rawcode);
    if (SafeReadPtr(agent, 0u, vtable) && vtable)
      static_cast<void>(SafeReadPtr(vtable, 0x8Cu, method));
    const uintptr_t methodAddress = reinterpret_cast<uintptr_t>(method);
    const uint32_t methodRva = methodAddress >= g_gameBase && g_gameBase != 0u
        ? static_cast<uint32_t>(methodAddress - g_gameBase) : 0u;
    if (rawcodeKnown &&
        IsDirectCUnitHandleProof(rawcode, methodAddress, g_gameBase) &&
        UnitWrapper(agent).IsValid()) {
      unit = agent;
      return true;
    }
    if (failure) {
      *failure = 4u;
      g_lastSetterObservedType.store(kind, std::memory_order_relaxed);
      g_lastSetterDirectRawcode.store(rawcode, std::memory_order_relaxed);
      g_lastSetterDirectMethodRva.store(methodRva, std::memory_order_relaxed);
    }
    return false;
  }
  if (!SafeReadPtr(agent, CAgentOffsets::UnitPtr, unit) || !unit) {
    if (failure)
      *failure = 5u;
    return false;
  }
  const UnitWrapper wrapper(unit);
  if (!wrapper.IsValid() || wrapper.GetRawcode() == 0u) {
    if (failure)
      *failure = 5u;
    return false;
  }
  return true;
}

double __fastcall Hook_CUnitSetAnimationProperty(void* unit, void*,
                                                   int property, int flags,
                                                   int reset) {
  uintptr_t caller = 0u;
#if defined(_MSC_VER)
  caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
  caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
  const bool trace = g_traceEnabled.load(std::memory_order_relaxed);
  if (trace) {
    g_hookCalls.fetch_add(1u, std::memory_order_relaxed);
    if (caller >= g_gameBase) {
      const uintptr_t rva = caller - g_gameBase;
      if (rva == kWalkCallbackReturnA || rva == kWalkCallbackReturnB)
        g_autoCallerSeen.fetch_add(1u, std::memory_order_relaxed);
      else if (property == 5) {
        g_otherWalkRequests.fetch_add(1u, std::memory_order_relaxed);
        g_lastOtherWalkCallerRva.store(static_cast<uint32_t>(rva),
                                      std::memory_order_relaxed);
      }
    }
  }
  if (g_installed.load(std::memory_order_acquire) && caller >= g_gameBase &&
      IsAutomaticWalkRequest(caller - g_gameBase, property, flags, reset)) {
    if (trace)
      g_autoRequestMatched.fetch_add(1u, std::memory_order_relaxed);
    const UnitWalkPolicy::Identity identity =
        g_walkPolicy.disabledIdentity(reinterpret_cast<uintptr_t>(unit));
    if (identity.jHandle != 0u) {
      void* liveUnit = nullptr;
      void* liveAgent = nullptr;
      if (ResolveExactUnit(identity.jHandle, liveUnit, liveAgent) &&
          liveUnit == unit &&
          reinterpret_cast<uintptr_t>(liveAgent) == identity.agent &&
          !UnitWrapper(unit).IsDead()) {
        if (trace) {
          uint32_t currentProperty = 0u;
          const bool known = SafeReadU32(
              unit, kCurrentAnimationPropertyOffset, currentProperty);
          g_lastSuppressedCurrentProperty.store(currentProperty,
                                                std::memory_order_relaxed);
          g_lastSuppressedCurrentPropertyKnown.store(known,
                                                     std::memory_order_relaxed);
          g_autoRequestSuppressed.fetch_add(1u, std::memory_order_relaxed);
        }
        return 0.0; // Valid ST(0), with no change to movement/order state.
      }
      if (trace)
        g_autoIdentityRejected.fetch_add(1u, std::memory_order_relaxed);
      g_walkPolicy.erase(reinterpret_cast<uintptr_t>(unit));
    } else if (trace) {
      g_autoWithoutDisabledEntry.fetch_add(1u, std::memory_order_relaxed);
    }
  }
  return g_originalSetAnimationProperty
      ? g_originalSetAnimationProperty(unit, property, flags, reset) : 0.0;
}

} // namespace

bool InstallUnitWalkHook(uintptr_t gameBase) noexcept {
  if (g_installed.load(std::memory_order_acquire))
    return true;
  char traceEnabled[4] = {};
  g_traceEnabled.store(
      GetEnvironmentVariableA("DXVK_WAR3_INTERNAL_TEST_API", traceEnabled,
                              static_cast<DWORD>(sizeof(traceEnabled))) == 1u &&
          traceEnabled[0] == '1',
      std::memory_order_relaxed);
  if (gameBase == 0u) {
    g_installStatus.store(1u, std::memory_order_release);
    return false;
  }

  // Target prologue and both movement call sites. The latter prove the
  // exact `call eax; fstp st` ABI and are also used as the runtime caller
  // allowlist. An unknown Game.dll must retain its original behavior.
  static constexpr uint8_t kTarget[] = {
      0x55, 0x8B, 0xEC, 0x83, 0x7D, 0x10, 0x00, 0x56,
      0x8B, 0xF1, 0x74, 0x08};
  static constexpr uint8_t kCaller[] = {0xFF, 0xD0, 0xDD, 0xD8};
  if (!ExactCode(gameBase + kCUnitSetAnimationPropertyRva,
                 kTarget, sizeof(kTarget)) ||
      !ExactCode(gameBase + kWalkCallARva, kCaller, sizeof(kCaller)) ||
      !ExactCode(gameBase + kWalkCallBRva, kCaller, sizeof(kCaller))) {
    g_installStatus.store(2u, std::memory_order_release);
    return false;
  }
  if (!HandleResolver::instance().initialize(gameBase)) {
    g_installStatus.store(3u, std::memory_order_release);
    return false;
  }

  g_gameBase = gameBase;
  if (!hooks::InstallMinHook(
          reinterpret_cast<LPVOID>(gameBase + kCUnitSetAnimationPropertyRva),
          reinterpret_cast<LPVOID>(&Hook_CUnitSetAnimationProperty),
          reinterpret_cast<LPVOID*>(&g_originalSetAnimationProperty),
          "UnitAnimation", "CUnitSetAnimationProperty", true, true)) {
    g_installStatus.store(4u, std::memory_order_release);
    return false;
  }
  g_installed.store(true, std::memory_order_release);
  g_installStatus.store(5u, std::memory_order_release);
  return true;
}

bool IsUnitWalkHookAvailable() noexcept {
  return g_installed.load(std::memory_order_acquire);
}

bool SetUnitWalkLoopEnabled(uint32_t jHandle, bool enabled) noexcept {
  const bool trace = g_traceEnabled.load(std::memory_order_relaxed);
  if (trace) {
    g_setterCalls.fetch_add(1u, std::memory_order_relaxed);
    g_lastSetterHandle.store(jHandle, std::memory_order_relaxed);
  }
  auto reject = [&](uint32_t failure) noexcept {
    if (trace) {
      g_setterRejected.fetch_add(1u, std::memory_order_relaxed);
      g_lastSetterFailure.store(failure, std::memory_order_relaxed);
    }
    return false;
  };
  if (!IsUnitWalkHookAvailable())
    return reject(1u);
  void* unit = nullptr;
  void* agent = nullptr;
  uint32_t failure = 0u;
  if (!ResolveExactUnit(jHandle, unit, agent, &failure))
    return reject(failure);
  if (UnitWrapper(unit).IsDead())
    return reject(6u);
  if (!g_walkPolicy.set(reinterpret_cast<uintptr_t>(unit),
                        {reinterpret_cast<uintptr_t>(agent), jHandle},
                        enabled))
    return reject(7u);
  if (trace) {
    uint32_t currentProperty = 0u;
    const bool known = SafeReadU32(
        unit, kCurrentAnimationPropertyOffset, currentProperty);
    g_lastSetterCurrentProperty.store(currentProperty,
                                      std::memory_order_relaxed);
    g_lastSetterCurrentPropertyKnown.store(known,
                                           std::memory_order_relaxed);
    g_setterAccepted.fetch_add(1u, std::memory_order_relaxed);
    g_lastSetterFailure.store(0u, std::memory_order_relaxed);
  }
  return true;
}

UnitWalkDiagnostics QueryUnitWalkDiagnostics() noexcept {
  UnitWalkDiagnostics result;
  result.installStatus = g_installStatus.load(std::memory_order_acquire);
  result.setterCalls = g_setterCalls.load(std::memory_order_relaxed);
  result.setterAccepted = g_setterAccepted.load(std::memory_order_relaxed);
  result.setterRejected = g_setterRejected.load(std::memory_order_relaxed);
  result.lastSetterHandle = g_lastSetterHandle.load(std::memory_order_relaxed);
  result.lastSetterFailure = g_lastSetterFailure.load(std::memory_order_relaxed);
  result.lastSetterObservedType =
      g_lastSetterObservedType.load(std::memory_order_relaxed);
  result.lastSetterDirectRawcode =
      g_lastSetterDirectRawcode.load(std::memory_order_relaxed);
  result.lastSetterDirectMethodRva =
      g_lastSetterDirectMethodRva.load(std::memory_order_relaxed);
  result.lastSetterCurrentProperty =
      g_lastSetterCurrentProperty.load(std::memory_order_relaxed);
  result.lastSetterCurrentPropertyKnown =
      g_lastSetterCurrentPropertyKnown.load(std::memory_order_relaxed);
  result.hookCalls = g_hookCalls.load(std::memory_order_relaxed);
  result.autoCallerSeen = g_autoCallerSeen.load(std::memory_order_relaxed);
  result.autoRequestMatched = g_autoRequestMatched.load(std::memory_order_relaxed);
  result.autoRequestSuppressed = g_autoRequestSuppressed.load(std::memory_order_relaxed);
  result.lastSuppressedCurrentProperty =
      g_lastSuppressedCurrentProperty.load(std::memory_order_relaxed);
  result.lastSuppressedCurrentPropertyKnown =
      g_lastSuppressedCurrentPropertyKnown.load(std::memory_order_relaxed);
  result.autoWithoutDisabledEntry = g_autoWithoutDisabledEntry.load(std::memory_order_relaxed);
  result.autoIdentityRejected = g_autoIdentityRejected.load(std::memory_order_relaxed);
  result.otherWalkRequests = g_otherWalkRequests.load(std::memory_order_relaxed);
  result.lastOtherWalkCallerRva = g_lastOtherWalkCallerRva.load(std::memory_order_relaxed);
  result.disabledUnitCount = g_walkPolicy.count();
  return result;
}

void ResetUnitWalkAuthorState() noexcept {
  g_walkPolicy.reset();
}

} // namespace dxvk::war3::game
