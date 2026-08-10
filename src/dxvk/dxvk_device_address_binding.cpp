#include "dxvk_device_address_binding.h"

#include <algorithm>
#include <limits>

namespace dxvk {

  namespace {

    uint32_t SaturatingIncrement(std::atomic<uint32_t>& value) noexcept {
      uint32_t current = value.load(std::memory_order_relaxed);
      while (current != std::numeric_limits<uint32_t>::max() &&
             !value.compare_exchange_weak(current, current + 1u,
               std::memory_order_relaxed, std::memory_order_relaxed)) { }
      return current;
    }


    uint64_t SaturatingRangeEnd(uint64_t base, uint64_t size) noexcept {
      if (size <= 1u)
        return base;
      const uint64_t extent = size - 1u;
      return extent > std::numeric_limits<uint64_t>::max() - base
        ? std::numeric_limits<uint64_t>::max()
        : base + extent;
    }


    void FaultAddressRange(const VkDeviceFaultAddressInfoEXT& address,
      uint64_t& lower, uint64_t& upper) noexcept {
      uint64_t precision = address.addressPrecision;
      if (!precision || (precision & (precision - 1u)))
        precision = 1u;
      const uint64_t mask = precision - 1u;
      lower = address.reportedAddress & ~mask;
      upper = address.reportedAddress | mask;
    }

  }


  void DxvkDeviceAddressBindingTracker::resetForInstance(
          bool messengerAvailable) noexcept {
    m_messengerAvailable.store(false, std::memory_order_release);
    m_deviceFeatureEnabled.store(false, std::memory_order_release);
    m_nextSequence.store(0u, std::memory_order_relaxed);
    m_observedEventCount.store(0u, std::memory_order_relaxed);
    m_droppedEventCount.store(0u, std::memory_order_relaxed);
    for (auto& slot : m_slots)
      slot.guard.store(0u, std::memory_order_relaxed);
    m_messengerAvailable.store(
      DxvkDeviceAddressBindingBuildEnabled && messengerAvailable,
      std::memory_order_release);
  }


  void DxvkDeviceAddressBindingTracker::setDeviceFeatureEnabled(
          bool enabled) noexcept {
    m_deviceFeatureEnabled.store(
      DxvkDeviceAddressBindingBuildEnabled && enabled,
      std::memory_order_release);
  }


  void DxvkDeviceAddressBindingTracker::record(
    const VkDeviceAddressBindingCallbackDataEXT& binding,
    const VkDebugUtilsObjectNameInfoEXT*          object) noexcept {
    if (!DxvkDeviceAddressBindingBuildEnabled ||
        !m_messengerAvailable.load(std::memory_order_acquire) ||
        !m_deviceFeatureEnabled.load(std::memory_order_acquire))
      return;

    uint32_t sequence = m_nextSequence.fetch_add(
      1u, std::memory_order_relaxed) + 1u;
    if (!sequence)
      sequence = m_nextSequence.fetch_add(1u, std::memory_order_relaxed) + 1u;

    Slot& slot = m_slots[(sequence - 1u) % Capacity];
    uint32_t expected = slot.guard.load(std::memory_order_acquire);
    const uint32_t busy = (sequence << 1u) | 1u;
    if ((expected & 1u) || !slot.guard.compare_exchange_strong(
          expected, busy, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
      SaturatingIncrement(m_droppedEventCount);
      return;
    }

    slot.bindingType.store(static_cast<uint32_t>(binding.bindingType),
      std::memory_order_relaxed);
    slot.flags.store(static_cast<uint32_t>(binding.flags),
      std::memory_order_relaxed);
    slot.baseAddress.store(binding.baseAddress);
    slot.size.store(binding.size);
    slot.objectType.store(object
      ? static_cast<uint32_t>(object->objectType)
      : static_cast<uint32_t>(VK_OBJECT_TYPE_UNKNOWN),
      std::memory_order_relaxed);
    slot.objectHandle.store(object ? object->objectHandle : 0u);
    slot.guard.store(sequence << 1u, std::memory_order_release);
    SaturatingIncrement(m_observedEventCount);
  }


  bool DxvkDeviceAddressBindingTracker::readSlot(uint32_t index,
      DxvkDeviceAddressBindingMatch& event) const noexcept {
    const Slot& slot = m_slots[index];
    const uint32_t before = slot.guard.load(std::memory_order_acquire);
    if (!before || (before & 1u))
      return false;

    event.sequence = before >> 1u;
    event.bindingType = static_cast<VkDeviceAddressBindingTypeEXT>(
      slot.bindingType.load(std::memory_order_relaxed));
    event.flags = static_cast<VkDeviceAddressBindingFlagsEXT>(
      slot.flags.load(std::memory_order_relaxed));
    event.baseAddress = slot.baseAddress.load();
    event.size = slot.size.load();
    event.objectType = static_cast<VkObjectType>(
      slot.objectType.load(std::memory_order_relaxed));
    event.objectHandle = slot.objectHandle.load();

    const uint32_t after = slot.guard.load(std::memory_order_acquire);
    return before == after && !(after & 1u);
  }


  DxvkDeviceAddressBindingSnapshot
  DxvkDeviceAddressBindingTracker::metadata() const noexcept {
    DxvkDeviceAddressBindingSnapshot result = { };
    result.messengerAvailable = m_messengerAvailable.load(
      std::memory_order_acquire);
    result.deviceFeatureEnabled = m_deviceFeatureEnabled.load(
      std::memory_order_acquire);
    result.observedEventCount = m_observedEventCount.load(
      std::memory_order_acquire);
    result.droppedEventCount = m_droppedEventCount.load(
      std::memory_order_acquire);
    result.truncated = result.observedEventCount > Capacity ||
      result.droppedEventCount != 0u;
    return result;
  }


  DxvkDeviceAddressBindingSnapshot
  DxvkDeviceAddressBindingTracker::correlate(
    const VkDeviceFaultAddressInfoEXT* addresses,
          uint32_t                     addressCount) const noexcept {
    DxvkDeviceAddressBindingSnapshot result = metadata();
    if (!addresses || !addressCount || !result.messengerAvailable ||
        !result.deviceFeatureEnabled)
      return result;

    uint32_t matchingEventCount = 0u;
    for (uint32_t slotIndex = 0u; slotIndex < Capacity; ++slotIndex) {
      DxvkDeviceAddressBindingMatch event = { };
      if (!readSlot(slotIndex, event))
        continue;

      const uint64_t eventLower = event.baseAddress;
      const uint64_t eventUpper = SaturatingRangeEnd(
        event.baseAddress, event.size);
      for (uint32_t faultIndex = 0u; faultIndex < addressCount; ++faultIndex) {
        uint64_t faultLower = 0u;
        uint64_t faultUpper = 0u;
        FaultAddressRange(addresses[faultIndex], faultLower, faultUpper);
        if (eventUpper < faultLower || faultUpper < eventLower)
          continue;

        event.faultInfoIndex = faultIndex;
        matchingEventCount += 1u;

        uint32_t insert = std::min(result.matchCount,
          DxvkDeviceAddressBindingSnapshot::MaxMatches - 1u);
        if (result.matchCount < DxvkDeviceAddressBindingSnapshot::MaxMatches)
          result.matchCount += 1u;
        else if (event.sequence <= result.matches[insert].sequence)
          break;

        while (insert &&
               result.matches[insert - 1u].sequence < event.sequence) {
          result.matches[insert] = result.matches[insert - 1u];
          insert -= 1u;
        }
        result.matches[insert] = event;
        break;
      }
    }

    result.truncated = result.truncated ||
      matchingEventCount > DxvkDeviceAddressBindingSnapshot::MaxMatches;
    return result;
  }


  DxvkDeviceAddressBindingTracker&
  GetDxvkDeviceAddressBindingTracker() noexcept {
    static DxvkDeviceAddressBindingTracker tracker;
    return tracker;
  }

}
