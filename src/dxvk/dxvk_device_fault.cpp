#include "dxvk_device_fault.h"

#include <algorithm>
#include <cstring>

namespace dxvk {

  DxvkDeviceFaultCapture::DxvkDeviceFaultCapture(
    bool                          featureEnabled,
    VkDevice                      device,
    PFN_vkGetDeviceFaultInfoEXT   getDeviceFaultInfo) noexcept
  : m_device(device),
    m_getDeviceFaultInfo(getDeviceFaultInfo),
    m_supported(featureEnabled && getDeviceFaultInfo != nullptr) {
    if (m_supported && m_device != VK_NULL_HANDLE)
      m_state.store(DxvkDeviceFaultCaptureState::Armed,
        std::memory_order_relaxed);
  }


  void DxvkDeviceFaultCapture::captureOnce(VkResult trigger) noexcept {
    if (trigger != VK_ERROR_DEVICE_LOST || !m_supported ||
        m_device == VK_NULL_HANDLE || m_getDeviceFaultInfo == nullptr)
      return;

    DxvkDeviceFaultCaptureState expected = DxvkDeviceFaultCaptureState::Armed;
    if (!m_state.compare_exchange_strong(expected,
          DxvkDeviceFaultCaptureState::Capturing,
          std::memory_order_acq_rel, std::memory_order_acquire))
      return;

    VkDeviceFaultCountsEXT counts = { VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
    counts.addressInfoCount = DxvkDeviceFaultSnapshot::MaxAddressInfos;
    counts.vendorInfoCount = DxvkDeviceFaultSnapshot::MaxVendorInfos;
    counts.vendorBinarySize = 0u;

    VkDeviceFaultInfoEXT info = { VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
    info.pAddressInfos = m_addressInfos.data();
    info.pVendorInfos = m_vendorInfos.data();
    info.pVendorBinaryData = nullptr;

    const VkResult result = m_getDeviceFaultInfo(m_device, &counts, &info);
    m_queryResult = result;
    m_truncated = result == VK_INCOMPLETE ||
      counts.addressInfoCount > DxvkDeviceFaultSnapshot::MaxAddressInfos ||
      counts.vendorInfoCount > DxvkDeviceFaultSnapshot::MaxVendorInfos ||
      counts.vendorBinarySize != 0u;

    m_addressInfoCount = std::min(counts.addressInfoCount,
      DxvkDeviceFaultSnapshot::MaxAddressInfos);
    m_vendorInfoCount = std::min(counts.vendorInfoCount,
      DxvkDeviceFaultSnapshot::MaxVendorInfos);
    std::memcpy(m_description.data(), info.description, m_description.size());

    // Publish query errors as Complete as well. Retrying a terminal driver
    // query can itself add unbounded work to the lost-device path.
    m_state.store(DxvkDeviceFaultCaptureState::Complete,
      std::memory_order_release);
  }


  DxvkDeviceFaultSnapshot DxvkDeviceFaultCapture::snapshot() const noexcept {
    DxvkDeviceFaultSnapshot result = { };
    const DxvkDeviceFaultCaptureState state = m_state.load(
      std::memory_order_acquire);

    result.supported = m_supported;
    result.captureState = state;
    result.complete = state == DxvkDeviceFaultCaptureState::Complete;
    result.vendorBinaryEnabled = false;

    // Do not race the capture writer. Capturing is observable but returns no
    // partial text or arrays; Complete's acquire load makes the copies below
    // safe and self-contained.
    if (!result.complete)
      return result;

    result.queryResult = m_queryResult;
    result.truncated = m_truncated;
    result.description = m_description;
    result.addressInfoCount = m_addressInfoCount;
    result.addressInfos = m_addressInfos;
    result.vendorInfoCount = m_vendorInfoCount;
    result.vendorInfos = m_vendorInfos;
    return result;
  }

}
