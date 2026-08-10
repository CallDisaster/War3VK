#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "dxvk_include.h"

namespace dxvk {

#if defined(WARVK_ENABLE_DEVICE_ADDRESS_BINDING_REPORT_DEV) && \
    WARVK_ENABLE_DEVICE_ADDRESS_BINDING_REPORT_DEV
  inline constexpr bool DxvkDeviceAddressBindingBuildEnabled = true;
#else
  inline constexpr bool DxvkDeviceAddressBindingBuildEnabled = false;
#endif

  struct DxvkDeviceAddressBindingMatch {
    uint32_t faultInfoIndex = 0u;
    uint32_t sequence = 0u;
    VkDeviceAddressBindingTypeEXT bindingType =
      VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT;
    VkDeviceAddressBindingFlagsEXT flags = 0u;
    VkDeviceAddress baseAddress = 0u;
    VkDeviceSize size = 0u;
    VkObjectType objectType = VK_OBJECT_TYPE_UNKNOWN;
    uint64_t objectHandle = 0u;
  };

  struct DxvkDeviceAddressBindingSnapshot {
    static constexpr uint32_t MaxMatches = 32u;

    bool buildEnabled = DxvkDeviceAddressBindingBuildEnabled;
    bool messengerAvailable = false;
    bool deviceFeatureEnabled = false;
    uint32_t observedEventCount = 0u;
    uint32_t droppedEventCount = 0u;
    bool truncated = false;
    uint32_t matchCount = 0u;
    std::array<DxvkDeviceAddressBindingMatch, MaxMatches> matches = { };
  };

  /**
   * \brief Allocation-free GPU address binding event ring
   *
   * Driver callbacks may be concurrent and must not call back into DXVK,
   * allocate, log, or take a lock. The ring deliberately keeps only bounded
   * value data required to correlate a later VK_EXT_device_fault address.
   */
  class DxvkDeviceAddressBindingTracker {
  public:
    // Keep the Release object effectively empty while retaining enough
    // development history to correlate long-lived allocations after a TDR.
    static constexpr uint32_t Capacity =
      DxvkDeviceAddressBindingBuildEnabled ? 16384u : 1u;

    void resetForInstance(bool messengerAvailable) noexcept;

    void setDeviceFeatureEnabled(bool enabled) noexcept;

    void record(
      const VkDeviceAddressBindingCallbackDataEXT& binding,
      const VkDebugUtilsObjectNameInfoEXT*          object) noexcept;

    DxvkDeviceAddressBindingSnapshot metadata() const noexcept;

    DxvkDeviceAddressBindingSnapshot correlate(
      const VkDeviceFaultAddressInfoEXT* addresses,
            uint32_t                     addressCount) const noexcept;

  private:
    struct AtomicU64 {
      std::atomic<uint32_t> low = { 0u };
      std::atomic<uint32_t> high = { 0u };

      void store(uint64_t value) noexcept {
        low.store(static_cast<uint32_t>(value), std::memory_order_relaxed);
        high.store(static_cast<uint32_t>(value >> 32), std::memory_order_relaxed);
      }

      uint64_t load() const noexcept {
        const uint64_t lowPart = low.load(std::memory_order_relaxed);
        const uint64_t highPart = high.load(std::memory_order_relaxed);
        return lowPart | (highPart << 32);
      }
    };

    struct Slot {
      std::atomic<uint32_t> guard = { 0u };
      std::atomic<uint32_t> bindingType = { 0u };
      std::atomic<uint32_t> flags = { 0u };
      AtomicU64 baseAddress = { };
      AtomicU64 size = { };
      std::atomic<uint32_t> objectType = { 0u };
      AtomicU64 objectHandle = { };
    };

    bool readSlot(uint32_t index,
      DxvkDeviceAddressBindingMatch& event) const noexcept;

    std::atomic<bool> m_messengerAvailable = { false };
    std::atomic<bool> m_deviceFeatureEnabled = { false };
    std::atomic<uint32_t> m_nextSequence = { 0u };
    std::atomic<uint32_t> m_observedEventCount = { 0u };
    std::atomic<uint32_t> m_droppedEventCount = { 0u };
    std::array<Slot, Capacity> m_slots = { };
  };

  DxvkDeviceAddressBindingTracker& GetDxvkDeviceAddressBindingTracker() noexcept;

}
