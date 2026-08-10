#include "../../../../dxvk/dxvk_device_address_binding.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {

std::atomic<size_t> gNewCalls = { 0u };

bool check(bool value, const char* message) {
  if (!value)
    std::cerr << message << std::endl;
  return value;
}

VkDebugUtilsObjectNameInfoEXT objectInfo(
    VkObjectType type, uint64_t handle) {
  VkDebugUtilsObjectNameInfoEXT result = {
    VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
  result.objectType = type;
  result.objectHandle = handle;
  return result;
}

VkDeviceAddressBindingCallbackDataEXT bindingInfo(
    uint64_t base, uint64_t size, VkDeviceAddressBindingTypeEXT type) {
  VkDeviceAddressBindingCallbackDataEXT result = {
    VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT };
  result.baseAddress = base;
  result.size = size;
  result.bindingType = type;
  return result;
}

VkDeviceFaultAddressInfoEXT faultInfo(
    uint64_t address, uint64_t precision) {
  VkDeviceFaultAddressInfoEXT result = { };
  result.addressType = VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT;
  result.reportedAddress = address;
  result.addressPrecision = precision;
  return result;
}

dxvk::DxvkDeviceAddressBindingTracker& tracker() {
  static dxvk::DxvkDeviceAddressBindingTracker instance;
  return instance;
}

bool testInactiveTrackerDoesNotRecord() {
  auto& bindingTracker = tracker();
  bindingTracker.resetForInstance(false);
  bindingTracker.setDeviceFeatureEnabled(true);
  const auto binding = bindingInfo(0x1000u, 0x100u,
    VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT);
  bindingTracker.record(binding, nullptr);
  const auto snapshot = bindingTracker.metadata();
  return check(snapshot.buildEnabled, "dev policy was not compiled") &&
    check(!snapshot.messengerAvailable, "inactive messenger reported active") &&
    check(snapshot.observedEventCount == 0u,
      "inactive tracker recorded an event");
}

bool testFaultPrecisionAndLatestEventCorrelation() {
  auto& bindingTracker = tracker();
  bindingTracker.resetForInstance(true);
  bindingTracker.setDeviceFeatureEnabled(true);

  const auto buffer = objectInfo(VK_OBJECT_TYPE_BUFFER, 0x1111u);
  const auto memory = objectInfo(VK_OBJECT_TYPE_DEVICE_MEMORY, 0x2222u);
  const auto bind = bindingInfo(0x1080u, 0x20u,
    VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT);
  const auto unbind = bindingInfo(0x1080u, 0x20u,
    VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT);
  bindingTracker.record(bind, &buffer);
  bindingTracker.record(unbind, &memory);

  // Khronos defines precision as an aligned possible range. 0x1087/0x100
  // therefore covers [0x1000, 0x10ff] and overlaps both events.
  const auto fault = faultInfo(0x1087u, 0x100u);
  const auto snapshot = bindingTracker.correlate(&fault, 1u);
  return check(snapshot.observedEventCount == 2u,
      "binding events were not counted") &&
    check(snapshot.matchCount == 2u, "fault range did not match both events") &&
    check(snapshot.matches[0].bindingType ==
        VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT,
      "matches were not ordered newest first") &&
    check(snapshot.matches[0].objectType == VK_OBJECT_TYPE_DEVICE_MEMORY &&
        snapshot.matches[0].objectHandle == 0x2222u,
      "object identity was not copied by value") &&
    check(snapshot.matches[1].bindingType ==
        VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT,
      "older bind event was lost");
}

bool testRingIsBoundedAndAllocationFree() {
  auto& bindingTracker = tracker();
  bindingTracker.resetForInstance(true);
  bindingTracker.setDeviceFeatureEnabled(true);
  const auto object = objectInfo(VK_OBJECT_TYPE_IMAGE, 0x3333u);
  const auto binding = bindingInfo(0x4000u, 0x1000u,
    VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT);
  const auto fault = faultInfo(0x4444u, 0x100u);

  const size_t before = gNewCalls.load(std::memory_order_relaxed);
  for (uint32_t index = 0u;
       index < dxvk::DxvkDeviceAddressBindingTracker::Capacity + 8u;
       ++index)
    bindingTracker.record(binding, &object);
  const auto snapshot = bindingTracker.correlate(&fault, 1u);
  const size_t after = gNewCalls.load(std::memory_order_relaxed);

  return check(before == after, "binding callback/correlation allocated") &&
    check(snapshot.observedEventCount ==
        dxvk::DxvkDeviceAddressBindingTracker::Capacity + 8u,
      "bounded ring lost its monotonic event count") &&
    check(snapshot.matchCount ==
        dxvk::DxvkDeviceAddressBindingSnapshot::MaxMatches,
      "bounded correlation did not clamp matches") &&
    check(snapshot.truncated, "wrapped/match-clamped ring was not marked truncated");
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

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main() {
  bool passed = true;
  passed = testInactiveTrackerDoesNotRecord() && passed;
  passed = testFaultPrecisionAndLatestEventCorrelation() && passed;
  passed = testRingIsBoundedAndAllocationFree() && passed;
  return passed ? 0 : 1;
}
