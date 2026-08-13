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


    bool SameObjectRange(const DxvkDeviceAddressBindingMatch& lhs,
                         const DxvkDeviceAddressBindingMatch& rhs) noexcept {
      return lhs.faultInfoIndex == rhs.faultInfoIndex &&
        lhs.objectType == rhs.objectType &&
        lhs.objectHandle == rhs.objectHandle &&
        lhs.baseAddress == rhs.baseAddress &&
        lhs.size == rhs.size;
    }


    bool HasObjectName(const DxvkDeviceAddressBindingMatch& event) noexcept {
      return event.objectName[0] != '\0';
    }

  }


  void DxvkDeviceAddressBindingTracker::resetForInstance(
          bool messengerAvailable) noexcept {
    m_messengerAvailable.store(false, std::memory_order_release);
    m_deviceFeatureEnabled.store(false, std::memory_order_release);
    m_nextSequence.store(0u, std::memory_order_relaxed);
    m_observedEventCount.store(0u, std::memory_order_relaxed);
    m_droppedEventCount.store(0u, std::memory_order_relaxed);
    m_driverLossSequence.store(std::numeric_limits<uint32_t>::max(),
      std::memory_order_relaxed);
    for (auto& slot : m_slots)
      slot.guard.store(0u, std::memory_order_relaxed);
    m_messengerAvailable.store(
      DxvkDeviceAddressBindingBuildEnabled && messengerAvailable,
      std::memory_order_release);
  }


  uint32_t DxvkDeviceAddressBindingTracker::sequenceBeforeDriverCall()
      const noexcept {
    return std::min(m_nextSequence.load(std::memory_order_acquire),
      std::numeric_limits<uint32_t>::max() - 1u);
  }


  void DxvkDeviceAddressBindingTracker::markDriverLossObserved() noexcept {
    markDriverLossObserved(sequenceBeforeDriverCall());
  }


  void DxvkDeviceAddressBindingTracker::markDriverLossObserved(
          uint32_t sequenceBeforeCall) noexcept {
    if (!DxvkDeviceAddressBindingBuildEnabled ||
        !m_messengerAvailable.load(std::memory_order_acquire) ||
        !m_deviceFeatureEnabled.load(std::memory_order_acquire))
      return;

    const uint32_t cutoff = std::min(sequenceBeforeCall,
      std::numeric_limits<uint32_t>::max() - 1u);
    uint32_t expected = std::numeric_limits<uint32_t>::max();
    while (cutoff < expected &&
           !m_driverLossSequence.compare_exchange_weak(expected, cutoff,
             std::memory_order_release, std::memory_order_relaxed)) { }
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
    std::array<char, DxvkDeviceAddressBindingMatch::ObjectNameCapacity>
      objectName = { };
    if (object && object->pObjectName) {
      for (uint32_t index = 0u; index + 1u < objectName.size() &&
           object->pObjectName[index] != '\0'; ++index)
        objectName[index] = object->pObjectName[index];
    }
    for (uint32_t wordIndex = 0u;
         wordIndex < Slot::ObjectNameWordCount; ++wordIndex) {
      uint32_t word = 0u;
      for (uint32_t byteIndex = 0u; byteIndex < sizeof(uint32_t); ++byteIndex) {
        const uint32_t nameIndex = wordIndex * sizeof(uint32_t) + byteIndex;
        word |= static_cast<uint32_t>(
          static_cast<uint8_t>(objectName[nameIndex])) << (byteIndex * 8u);
      }
      slot.objectName[wordIndex].store(word, std::memory_order_relaxed);
    }
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
    for (uint32_t wordIndex = 0u;
         wordIndex < Slot::ObjectNameWordCount; ++wordIndex) {
      const uint32_t word = slot.objectName[wordIndex].load(
        std::memory_order_relaxed);
      for (uint32_t byteIndex = 0u; byteIndex < sizeof(uint32_t); ++byteIndex) {
        const uint32_t nameIndex = wordIndex * sizeof(uint32_t) + byteIndex;
        event.objectName[nameIndex] = static_cast<char>(
          (word >> (byteIndex * 8u)) & 0xffu);
      }
    }
    event.objectName.back() = '\0';

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
    const uint32_t driverLossSequence = m_driverLossSequence.load(
      std::memory_order_acquire);
    result.driverLossObserved = driverLossSequence !=
      std::numeric_limits<uint32_t>::max();
    result.driverLossSequence = result.driverLossObserved
      ? driverLossSequence
      : 0u;
    const uint32_t nextSequence = m_nextSequence.load(
      std::memory_order_acquire);
    result.postDriverLossEventCount = result.driverLossObserved &&
      nextSequence > driverLossSequence
        ? nextSequence - driverLossSequence
        : 0u;
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
      if (result.driverLossObserved &&
          event.sequence > result.driverLossSequence)
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

        uint32_t existing = result.matchCount;
        for (uint32_t index = 0u; index < result.matchCount; ++index) {
          if (SameObjectRange(result.matches[index], event)) {
            existing = index;
            break;
          }
        }

        if (existing < result.matchCount) {
          if (event.sequence > result.matches[existing].sequence)
            result.matches[existing] = event;
          break;
        }

        if (result.matchCount < DxvkDeviceAddressBindingSnapshot::MaxMatches) {
          result.matches[result.matchCount++] = event;
          break;
        }

        uint32_t oldest = 0u;
        for (uint32_t index = 1u; index < result.matchCount; ++index) {
          if (result.matches[index].sequence < result.matches[oldest].sequence)
            oldest = index;
        }
        if (event.sequence > result.matches[oldest].sequence)
          result.matches[oldest] = event;
        break;
      }
    }

    std::sort(result.matches.begin(),
      result.matches.begin() + result.matchCount,
      [] (const auto& lhs, const auto& rhs) {
        return lhs.sequence > rhs.sequence;
      });

    // The first pass retains only the newest overlapping event for each exact
    // object/range key. A bounded second pass attaches prior lifecycle context
    // without allocating or treating unrelated aliased objects as one owner.
    for (uint32_t matchIndex = 0u;
         matchIndex < result.matchCount; ++matchIndex) {
      auto& match = result.matches[matchIndex];
      match.latestForObjectRange = true;
      for (uint32_t slotIndex = 0u; slotIndex < Capacity; ++slotIndex) {
        DxvkDeviceAddressBindingMatch previous = { };
        if (!readSlot(slotIndex, previous))
          continue;
        previous.faultInfoIndex = match.faultInfoIndex;
        if (!SameObjectRange(match, previous))
          continue;

        // A debug name may only become visible after object creation, and a
        // terminal driver result can race CPU-side retirement. It is safe to
        // borrow a bounded name from a later event with the same exact Vulkan
        // object/range identity, but never use that event as lifecycle state.
        if (!HasObjectName(match) && HasObjectName(previous) &&
            previous.sequence > match.nameSourceSequence) {
          match.objectName = previous.objectName;
          match.nameSourceSequence = previous.sequence;
          match.nameObservedAfterDriverLoss = result.driverLossObserved &&
            previous.sequence > result.driverLossSequence;
        }
        if (previous.sequence >= match.sequence)
          continue;

        if (!match.hasPreviousEvent ||
            previous.sequence > match.previousSequence) {
          match.hasPreviousEvent = true;
          match.previousSequence = previous.sequence;
          match.previousBindingType = previous.bindingType;
        }
        if (previous.bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT &&
            (!match.hasPriorBind ||
             previous.sequence > match.priorBindSequence)) {
          match.hasPriorBind = true;
          match.priorBindSequence = previous.sequence;
        }
      }
      if (HasObjectName(match) && !match.nameSourceSequence)
        match.nameSourceSequence = match.sequence;
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
