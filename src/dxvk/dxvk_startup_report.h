#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dxvk {

  /**
   * \brief One Vulkan physical device as seen while choosing adapters
   *
   * Recorded by the device filter. \c rejection is empty for accepted
   * adapters and holds the English reason for rejected ones.
   */
  struct DxvkStartupAdapterRecord {
    std::string name;
    uint32_t    vendorId          = 0;
    uint32_t    deviceId          = 0;
    uint32_t    deviceType        = 0;
    uint32_t    apiVersion        = 0;
    uint32_t    driverVersionRaw  = 0;
    uint32_t    driverId          = 0;
    std::string driverName;
    std::string driverInfo;
    uint64_t    deviceLocalBytes  = 0;
    bool        accepted          = false;
    std::string rejection;
  };

  enum class DxvkStartupStage : uint32_t {
    NotStarted,
    LoaderMissing,
    InstanceFailed,
    NoAdapter,
    Ready,
  };

  struct DxvkStartupReportSnapshot {
    DxvkStartupStage                      stage = DxvkStartupStage::NotStarted;
    std::vector<DxvkStartupAdapterRecord> adapters;
  };

  /**
   * \brief Process-wide record of the last instance creation attempt
   *
   * A failed DxvkInstance constructor throws and is never published, so
   * the reasons a startup failed are kept here for the error dialog.
   * Written only during instance creation; a few hundred bytes.
   */
  namespace startup_report {

    void beginInstance();

    void setStage(DxvkStartupStage stage);

    void addAdapter(DxvkStartupAdapterRecord record);

    DxvkStartupReportSnapshot snapshot();

  }

}
