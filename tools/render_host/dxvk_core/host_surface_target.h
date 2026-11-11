#pragma once
#include "host_surface_target_policy.h"
#include "../win32_transport.h"
#include <memory>

namespace warvk::host::surface {
// Read once in host startup, never in a draw or WndProc. Absent => peer. Unknown
// values fail. This is a dev-host setting; it does not change D9C2 or Game.dll.
Mode ReadStartupMode();

// HOST DEV ONLY. Current profile: same non-input Desktop, fixed 64x64, no input
// forwarding, no foreign subclassing, no implicit fallback. All public methods
// except destruction belong to the constructing backend thread.
class HostSurfaceTarget final {
public:
  HostSurfaceTarget();
  ~HostSurfaceTarget() noexcept;
  HostSurfaceTarget(const HostSurfaceTarget&) = delete;
  HostSurfaceTarget& operator=(const HostSurfaceTarget&) = delete;
  void start(Mode mode, HWND authenticatedParent, DWORD peerPid, DWORD peerTid,
    HANDLE authenticatedPeerProcess, const wchar_t* peerMarker, uint64_t operationDeadline);
  void validate() const;
  HWND validatedWindow() const;
  // Must precede the first operation which can create a VkSurface. A failed or
  // throwing surface creation remains potentially live, never automatically free.
  void claimSurface();
  void noteSurfaceResult(HWND actual, int32_t result);
  // Only AFTER production Presenter/surface teardown. No parent validation here:
  // a previously invalid parent may still permit proven quiescent GPU cleanup.
  void closeAfterSurfaceRetired(uint64_t operationDeadline, bool allowWindowFault = false);
  // Before surface use: request owner-thread cleanup. Once possibly live: keep
  // the HWND owner/storage alive with the backend's existing quarantine policy.
  void onOwnerQuarantined() noexcept;
  void report(const char* event) const noexcept;
private:
  struct State;
  std::shared_ptr<State> state;
  HANDLE thread = nullptr; // Backend-owned HANDLE, not the thread's storage.
  DWORD callerThread = 0;
  uint64_t threadCreation = 0;
  uint32_t surfaceCalls = 0;
  void requireCaller() const;
};
} // namespace warvk::host::surface
