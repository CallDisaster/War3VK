#include "../../../../dxvk/dxvk_device_fault.h"
#include "../../../../vulkan/vulkan_loader.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>

namespace {

std::atomic<size_t> gNewCalls = 0u;

struct FakeFaultState {
  VkResult result = VK_SUCCESS;
  uint32_t addressInfoCount = 0u;
  uint32_t vendorInfoCount = 0u;
  uint32_t calls = 0u;
  bool sawFixedCapacity = false;
  bool sawNoVendorBinary = false;
};

FakeFaultState gFake = {};

void resetFake(VkResult result, uint32_t addressInfoCount,
               uint32_t vendorInfoCount) {
  gFake = {};
  gFake.result = result;
  gFake.addressInfoCount = addressInfoCount;
  gFake.vendorInfoCount = vendorInfoCount;
}

VKAPI_ATTR VkResult VKAPI_CALL fakeGetDeviceFaultInfo(
    VkDevice, VkDeviceFaultCountsEXT* counts, VkDeviceFaultInfoEXT* info) {
  ++gFake.calls;
  gFake.sawFixedCapacity =
      counts->addressInfoCount == dxvk::DxvkDeviceFaultSnapshot::MaxAddressInfos &&
      counts->vendorInfoCount == dxvk::DxvkDeviceFaultSnapshot::MaxVendorInfos;
  gFake.sawNoVendorBinary =
      counts->vendorBinarySize == 0u && info->pVendorBinaryData == nullptr;

  if (gFake.result != VK_SUCCESS && gFake.result != VK_INCOMPLETE)
    return gFake.result;

  std::memset(info->description, 0, VK_MAX_DESCRIPTION_SIZE);
  std::memcpy(info->description, "fake device fault", 17u);

  const uint32_t addressWriteCount = std::min(
      gFake.addressInfoCount,
      dxvk::DxvkDeviceFaultSnapshot::MaxAddressInfos);
  for (uint32_t index = 0u; index < addressWriteCount; ++index) {
    info->pAddressInfos[index].addressType =
        VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT;
    info->pAddressInfos[index].reportedAddress = 0x1000u + index;
    info->pAddressInfos[index].addressPrecision = 16u;
  }

  const uint32_t vendorWriteCount = std::min(
      gFake.vendorInfoCount,
      dxvk::DxvkDeviceFaultSnapshot::MaxVendorInfos);
  for (uint32_t index = 0u; index < vendorWriteCount; ++index) {
    std::memset(info->pVendorInfos[index].description, 0,
      VK_MAX_DESCRIPTION_SIZE);
    std::memcpy(info->pVendorInfos[index].description, "fake vendor", 12u);
    info->pVendorInfos[index].vendorFaultCode = 0x2000u + index;
    info->pVendorInfos[index].vendorFaultData = 0x3000u + index;
  }

  counts->addressInfoCount = gFake.addressInfoCount;
  counts->vendorInfoCount = gFake.vendorInfoCount;
  counts->vendorBinarySize = 0u;
  return gFake.result;
}

bool check(bool condition, const char* message) {
  if (!condition)
    std::cerr << "dxvk_device_fault_capture_test: " << message << '\n';
  return condition;
}

VkDevice fakeDevice() {
  return reinterpret_cast<VkDevice>(uintptr_t(1u));
}

bool testDisabledAndNonLossDoNotQuery() {
  resetFake(VK_SUCCESS, 1u, 1u);
  dxvk::DxvkDeviceFaultCapture disabled(
      false, fakeDevice(), fakeGetDeviceFaultInfo);
  disabled.captureOnce(VK_ERROR_DEVICE_LOST);
  if (!check(gFake.calls == 0u, "disabled capture queried the driver"))
    return false;

  dxvk::DxvkDeviceFaultCapture armed(
      true, fakeDevice(), fakeGetDeviceFaultInfo);
  armed.captureOnce(VK_ERROR_UNKNOWN);
  const auto snapshot = armed.snapshot();
  return check(gFake.calls == 0u, "non-loss result queried the driver") &&
    check(snapshot.captureState == dxvk::DxvkDeviceFaultCaptureState::Armed,
      "non-loss result consumed the one-shot capture");
}

bool testSuccessfulCaptureIsSingleAndOwned() {
  resetFake(VK_SUCCESS, 1u, 1u);
  dxvk::DxvkDeviceFaultSnapshot snapshot = {};
  {
    dxvk::DxvkDeviceFaultCapture capture(
        true, fakeDevice(), fakeGetDeviceFaultInfo);
    capture.captureOnce(VK_ERROR_DEVICE_LOST);
    capture.captureOnce(VK_ERROR_DEVICE_LOST);
    snapshot = capture.snapshot();
  }

  return check(gFake.calls == 1u, "lost capture queried more than once") &&
    check(gFake.sawFixedCapacity, "capture did not request fixed capacities") &&
    check(gFake.sawNoVendorBinary, "capture requested a vendor binary") &&
    check(snapshot.complete, "successful capture was not complete") &&
    check(snapshot.queryResult == VK_SUCCESS, "successful query result lost") &&
    check(snapshot.addressInfoCount == 1u && snapshot.vendorInfoCount == 1u,
      "successful counts are wrong") &&
    check(snapshot.addressInfos[0].reportedAddress == 0x1000u,
      "address record was not copied by value") &&
    check(snapshot.vendorInfos[0].vendorFaultCode == 0x2000u,
      "vendor record was not copied by value") &&
    check(std::strncmp(snapshot.description.data(), "fake device fault", 17u) == 0,
      "description was not copied by value") &&
    check(!snapshot.vendorBinaryEnabled, "snapshot claims vendor binary support");
}

bool testIncompleteClampsAndMarksTruncated() {
  resetFake(VK_INCOMPLETE,
      dxvk::DxvkDeviceFaultSnapshot::MaxAddressInfos + 1u,
      dxvk::DxvkDeviceFaultSnapshot::MaxVendorInfos + 1u);
  dxvk::DxvkDeviceFaultCapture capture(
      true, fakeDevice(), fakeGetDeviceFaultInfo);
  capture.captureOnce(VK_ERROR_DEVICE_LOST);
  const auto snapshot = capture.snapshot();

  return check(snapshot.complete, "incomplete query did not complete capture") &&
    check(snapshot.queryResult == VK_INCOMPLETE,
      "incomplete query result was lost") &&
    check(snapshot.truncated, "incomplete query did not mark truncation") &&
    check(snapshot.addressInfoCount ==
        dxvk::DxvkDeviceFaultSnapshot::MaxAddressInfos,
      "address records were not clamped") &&
    check(snapshot.vendorInfoCount ==
        dxvk::DxvkDeviceFaultSnapshot::MaxVendorInfos,
      "vendor records were not clamped");
}

bool testQueryFailureCompletesWithoutRetry() {
  resetFake(VK_ERROR_UNKNOWN,
      dxvk::DxvkDeviceFaultSnapshot::MaxAddressInfos,
      dxvk::DxvkDeviceFaultSnapshot::MaxVendorInfos);
  dxvk::DxvkDeviceFaultCapture capture(
      true, fakeDevice(), fakeGetDeviceFaultInfo);
  capture.captureOnce(VK_ERROR_DEVICE_LOST);
  capture.captureOnce(VK_ERROR_DEVICE_LOST);
  const auto snapshot = capture.snapshot();

  return check(gFake.calls == 1u, "failed query retried") &&
    check(snapshot.complete, "failed query did not publish Complete") &&
    check(snapshot.queryResult == VK_ERROR_UNKNOWN,
      "failed query result was hidden") &&
    check(snapshot.addressInfoCount == 0u && snapshot.vendorInfoCount == 0u,
      "failed query consumed undefined output counts") &&
    check(!snapshot.truncated, "failed query marked undefined output truncated") &&
    check(snapshot.description[0] == '\0',
      "failed query consumed undefined output description");
}

bool testLostPathDoesNotAllocate() {
  resetFake(VK_SUCCESS, 0u, 0u);
  dxvk::DxvkDeviceFaultCapture capture(
      true, fakeDevice(), fakeGetDeviceFaultInfo);
  const size_t before = gNewCalls.load(std::memory_order_relaxed);
  capture.captureOnce(VK_ERROR_DEVICE_LOST);
  const size_t after = gNewCalls.load(std::memory_order_relaxed);
  return check(before == after, "lost capture allocated memory");
}

bool testDeviceLossStateIsMonotonicAndAllocationFree() {
  dxvk::vk::DeviceLossState state = {};
  const size_t before = gNewCalls.load(std::memory_order_relaxed);

  state.notifyDeviceErrorFromDriverResult(VK_SUCCESS);
  state.notifyDeviceErrorFromDriverResult(VK_ERROR_UNKNOWN);
  const bool ignoredNonLoss = !state.driverDeviceLossObserved();

  state.notifyDeviceErrorFromDriverResult(VK_ERROR_DEVICE_LOST);
  const bool observedLoss = state.driverDeviceLossObserved();
  state.notifyDeviceErrorFromDriverResult(VK_SUCCESS);
  state.notifyDeviceErrorFromDriverResult(VK_ERROR_UNKNOWN);
  const size_t after = gNewCalls.load(std::memory_order_relaxed);

  return check(ignoredNonLoss, "non-loss result set direct driver-loss state") &&
    check(observedLoss, "device-loss result did not set direct state") &&
    check(state.driverDeviceLossObserved(), "direct driver-loss state regressed") &&
    check(before == after, "direct driver-loss state allocated memory");
}

}

void* operator new(std::size_t size) {
  gNewCalls.fetch_add(1u, std::memory_order_relaxed);
  if (void* result = std::malloc(size))
    return result;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  gNewCalls.fetch_add(1u, std::memory_order_relaxed);
  if (void* result = std::malloc(size))
    return result;
  throw std::bad_alloc();
}

void operator delete(void* pointer) noexcept {
  std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
  std::free(pointer);
}

int main() {
  bool passed = true;
  passed = testDisabledAndNonLossDoNotQuery() && passed;
  passed = testSuccessfulCaptureIsSingleAndOwned() && passed;
  passed = testIncompleteClampsAndMarksTruncated() && passed;
  passed = testQueryFailureCompletesWithoutRetry() && passed;
  passed = testLostPathDoesNotAllocate() && passed;
  passed = testDeviceLossStateIsMonotonicAndAllocationFree() && passed;
  return passed ? 0 : 1;
}
