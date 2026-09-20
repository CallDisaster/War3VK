#pragma once
#include <cstdint>
namespace dxvk::war3::hooks {
void InstallNativeCapture(uintptr_t gameBase);
void BeginNativeFrameSyncPresent(const void* swapchain);
void CompleteNativeFrameSyncPresent(const void* swapchain);
void RevokeNativeFrameSyncPresent(bool permanent);
bool InvokeNativeScreenshotEventForTest();
struct NativeFrameSyncStatus {
  bool persistent = false, installed = false, owner = false;
  uint32_t calls = 0, elided = 0, retained = 0, presents = 0;
  uint64_t queryTicks = 0, frequency = 0;
};
NativeFrameSyncStatus QueryNativeFrameSyncStatus();
}
