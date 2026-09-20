#pragma once
#include <cstdint>
#include <string>
namespace dxvk::war3::timeline {
bool Enabled() noexcept;
uint64_t Enter(const char* name) noexcept;
void Leave(uint64_t token) noexcept;
void Present(const void* swapchain, bool recording) noexcept;
void PresentResult(int32_t result) noexcept;
void LegacyWindow(bool inside) noexcept;
std::string CaptureJson();
// Diagnostic only: preserve the actual LockRect result and all resource work.
void BackbufferCreated(const void* surface, uint32_t flags, uint32_t width,
                      uint32_t height, uint32_t format) noexcept;
uint64_t BackbufferLockBegin() noexcept;
void BackbufferLockEnd(const void* surface, uintptr_t caller, uint32_t flags,
                       bool subrect, int32_t result, uint64_t begin) noexcept;
void NativeFrameSyncInstalled(bool installed, bool candidate) noexcept;
bool NativeFrameSyncRecordingOwner() noexcept;
void NativeFrameSyncRequest(int32_t request, uint32_t capture,
                           bool readable, bool nested, bool elided) noexcept;
struct Scope {
  explicit Scope(const char* name) noexcept : token(Enter(name)) {}
  ~Scope() { Leave(token); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  uint64_t token;
};
}
