#include "war3_native_device_resolver.h"

#include "../../d3d9_war3_debug.h"

#include "../core/war3_memory.h"
#include "../hooks/war3_hook_address_book.h"
#include "war3_runtime_bootstrap.h"

#include <array>
#include <atomic>
#include <windows.h>

namespace dxvk::war3::platform {
namespace {

std::atomic<uintptr_t> g_lastBoundDevice{0u};
std::atomic<uint64_t> g_attemptCount{0u};
std::atomic<uint64_t> g_hitCount{0u};
std::atomic<uint64_t> g_lastQuietAttemptTick{0u};

bool ReadPointer(uintptr_t address, uintptr_t& out) {
  out = 0u;
  if (!dxvk::war3::IsReadableRange(reinterpret_cast<const void*>(address),
                                   sizeof(uintptr_t)))
    return false;
  out = *reinterpret_cast<const uintptr_t*>(address);
  return out != 0u;
}

bool ReadPointerField(uintptr_t base, uintptr_t offset, uintptr_t& out) {
  out = 0u;
  if (base == 0u)
    return false;
  const uintptr_t address = base + offset;
  if (address < base)
    return false;
  return ReadPointer(address, out);
}

bool LooksLikeD3D9Device(uintptr_t device) {
  if (device == 0u)
    return false;

  uintptr_t vtbl = 0u;
  if (!ReadPointer(device, vtbl))
    return false;
  if (!dxvk::war3::IsReadableRange(reinterpret_cast<const void*>(vtbl),
                                   sizeof(uintptr_t) * 120u))
    return false;

  static constexpr std::array<uint32_t, 8> kProbeSlots = {
      0u, 1u, 2u, 3u, 7u, 16u, 17u, 42u,
  };

  for (uint32_t slot : kProbeSlots) {
    uintptr_t fn = 0u;
    if (!ReadPointer(vtbl + sizeof(uintptr_t) * slot, fn))
      return false;
    if (!dxvk::war3::IsExecutableRange(reinterpret_cast<const void*>(fn), 1u))
      return false;
  }

  return true;
}

bool ResolveNativeDevice(uintptr_t& gameBase,
                         uintptr_t& gxDevice,
                         uintptr_t& d3d9Device) {
  gameBase = reinterpret_cast<uintptr_t>(::GetModuleHandleA("Game.dll"));
  gxDevice = 0u;
  d3d9Device = 0u;
  if (gameBase == 0u)
    return false;

  const auto& book = dxvk::war3::hooks::GetWar3HookAddressBook127a();
  if (!ReadPointer(gameBase + book.gxDevice, gxDevice))
    return false;
  if (!ReadPointerField(gxDevice, book.gxDeviceD3dNativeDeviceOffset,
                        d3d9Device))
    return false;
  return LooksLikeD3D9Device(d3d9Device);
}

bool ShouldAttemptQuietResolve() {
  constexpr uint64_t kQuietResolveIntervalMs = 1000u;
  const uint64_t now = ::GetTickCount64();
  uint64_t last = g_lastQuietAttemptTick.load(std::memory_order_relaxed);
  while (now >= last && now - last >= kQuietResolveIntervalMs) {
    if (g_lastQuietAttemptTick.compare_exchange_weak(
            last, now, std::memory_order_relaxed))
      return true;
  }
  return false;
}

} // namespace

bool TryBindNativeDeviceFromWar3Globals(const char* reason, bool forceLog) {
  if (!forceLog && !ShouldAttemptQuietResolve())
    return g_lastBoundDevice.load(std::memory_order_acquire) != 0u;

  g_attemptCount.fetch_add(1u, std::memory_order_relaxed);

  uintptr_t gameBase = 0u;
  uintptr_t gxDevice = 0u;
  uintptr_t d3d9Device = 0u;
  if (!ResolveNativeDevice(gameBase, gxDevice, d3d9Device)) {
    if (forceLog) {
      dxvk::war3dbg::Print(
          "DXVK War3NativeDevice: resolve miss reason=%s gameBase=%p gx=%p "
          "device=%p\n",
          (reason && reason[0]) ? reason : "(unknown)",
          reinterpret_cast<void*>(gameBase), reinterpret_cast<void*>(gxDevice),
          reinterpret_cast<void*>(d3d9Device));
    }
    return false;
  }

  g_hitCount.fetch_add(1u, std::memory_order_relaxed);
  const uintptr_t previous =
      g_lastBoundDevice.exchange(d3d9Device, std::memory_order_acq_rel);
  BindNativeShadowDevice(reinterpret_cast<IDirect3DDevice9*>(d3d9Device));

  if (forceLog || previous != d3d9Device) {
    dxvk::war3dbg::Print(
        "DXVK War3NativeDevice: bound device=%p gx=%p base=%p reason=%s "
        "changed=%d\n",
        reinterpret_cast<void*>(d3d9Device), reinterpret_cast<void*>(gxDevice),
        reinterpret_cast<void*>(gameBase),
        (reason && reason[0]) ? reason : "(unknown)",
        previous != d3d9Device ? 1 : 0);
  }
  return true;
}

War3NativeDeviceResolveSnapshot GetNativeDeviceResolveSnapshot() {
  War3NativeDeviceResolveSnapshot snapshot = {};
  ResolveNativeDevice(snapshot.gameBase, snapshot.gxDevice,
                      snapshot.d3d9Device);
  snapshot.attemptCount = g_attemptCount.load(std::memory_order_relaxed);
  snapshot.hitCount = g_hitCount.load(std::memory_order_relaxed);
  snapshot.bound = g_lastBoundDevice.load(std::memory_order_acquire) != 0u;
  return snapshot;
}

} // namespace dxvk::war3::platform
