#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "dxvk_include.h"

namespace dxvk {

  enum class DxvkDeviceFaultCaptureState : uint32_t {
    Disabled,
    Armed,
    Capturing,
    Complete,
  };

  /**
   * \brief Bounded, owning device-fault record
   *
   * This contains no driver-owned pointer. A diagnostic consumer may retain a
   * copy after the DxvkDevice and its Vulkan loader have been destroyed.
   */
  struct DxvkDeviceFaultSnapshot {
    static constexpr uint32_t MaxAddressInfos = 64u;
    static constexpr uint32_t MaxVendorInfos  = 32u;

    bool supported = false;
    DxvkDeviceFaultCaptureState captureState = DxvkDeviceFaultCaptureState::Disabled;
    bool complete = false;
    VkResult queryResult = VK_ERROR_EXTENSION_NOT_PRESENT;
    bool truncated = false;
    std::array<char, VK_MAX_DESCRIPTION_SIZE> description = { };
    uint32_t addressInfoCount = 0u;
    std::array<VkDeviceFaultAddressInfoEXT, MaxAddressInfos> addressInfos = { };
    uint32_t vendorInfoCount = 0u;
    std::array<VkDeviceFaultVendorInfoEXT, MaxVendorInfos> vendorInfos = { };
    bool vendorBinaryEnabled = false;
  };

  /**
   * \brief One-shot, allocation-free VK_EXT_device_fault text capture
   *
   * The lost-device path may transition Armed to Capturing exactly once. A
   * snapshot observed while Capturing intentionally contains no partially
   * written driver output; Complete publishes the bounded arrays with release
   * semantics.
   */
  class DxvkDeviceFaultCapture {
  public:
    DxvkDeviceFaultCapture() = default;

    DxvkDeviceFaultCapture(
      bool                          featureEnabled,
      VkDevice                      device,
      PFN_vkGetDeviceFaultInfoEXT   getDeviceFaultInfo) noexcept;

    void captureOnce(VkResult trigger) noexcept;

    DxvkDeviceFaultSnapshot snapshot() const noexcept;

  private:
    std::atomic<DxvkDeviceFaultCaptureState> m_state = {
      DxvkDeviceFaultCaptureState::Disabled };
    VkDevice m_device = VK_NULL_HANDLE;
    PFN_vkGetDeviceFaultInfoEXT m_getDeviceFaultInfo = nullptr;
    bool m_supported = false;
    VkResult m_queryResult = VK_ERROR_EXTENSION_NOT_PRESENT;
    bool m_truncated = false;
    std::array<char, VK_MAX_DESCRIPTION_SIZE> m_description = { };
    uint32_t m_addressInfoCount = 0u;
    std::array<VkDeviceFaultAddressInfoEXT,
      DxvkDeviceFaultSnapshot::MaxAddressInfos> m_addressInfos = { };
    uint32_t m_vendorInfoCount = 0u;
    std::array<VkDeviceFaultVendorInfoEXT,
      DxvkDeviceFaultSnapshot::MaxVendorInfos> m_vendorInfos = { };
  };

}
