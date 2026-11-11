#include "dxvk_device_filter.h"
#include "dxvk_startup_report.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace dxvk {
  
  static std::string convertUUID(const uint8_t uuid[VK_UUID_SIZE]) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (size_t i = 0; i < VK_UUID_SIZE; ++i)
      stream << std::setw(2) << static_cast<uint32_t>(uuid[i] & 0xff);
    return stream.str();
  }


  DxvkDeviceFilter::DxvkDeviceFilter(
          DxvkDeviceFilterFlags flags,
    const DxvkOptions&          options)
  : m_flags(flags) {
    m_matchDeviceName = env::getEnvVar("DXVK_FILTER_DEVICE_NAME");
    m_matchDeviceUUID = env::getEnvVar("DXVK_FILTER_DEVICE_UUID");

    if (m_matchDeviceName.empty())
      m_matchDeviceName = options.deviceFilter;

    if (!m_matchDeviceName.empty())
      m_flags.set(DxvkDeviceFilterFlag::MatchDeviceName);

    if (!m_matchDeviceUUID.empty())
      m_flags.set(DxvkDeviceFilterFlag::MatchDeviceUUID);

    if (m_flags.any(DxvkDeviceFilterFlag::MatchDeviceName,
                    DxvkDeviceFilterFlag::MatchDeviceUUID))
      m_flags.clr(DxvkDeviceFilterFlag::SkipCpuDevices);
  }


  DxvkDeviceFilter::~DxvkDeviceFilter() {

  }


  bool DxvkDeviceFilter::testAdapter(DxvkAdapter& adapter) const {
    const auto& properties = adapter.deviceProperties();

    Logger::info(str::format("Found device: ",
      properties.core.properties.deviceName, " (",
      properties.vk12.driverName, " ",
      properties.driverVersion.toString(), ")"));

    // Keep what the player needs to hear about this device: the instance
    // is never published when no adapter passes, so the log alone is lost.
    DxvkStartupAdapterRecord record;
    record.name             = properties.core.properties.deviceName;
    record.vendorId         = properties.core.properties.vendorID;
    record.deviceId         = properties.core.properties.deviceID;
    record.deviceType       = uint32_t(properties.core.properties.deviceType);
    record.apiVersion       = properties.core.properties.apiVersion;
    record.driverVersionRaw = properties.core.properties.driverVersion;
    record.driverId         = uint32_t(properties.vk12.driverID);
    record.driverName       = properties.vk12.driverName;
    record.driverInfo       = properties.vk12.driverInfo;

    const auto& memory = adapter.memoryProperties();

    for (uint32_t i = 0; i < memory.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++) {
      if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        record.deviceLocalBytes = std::max<uint64_t>(record.deviceLocalBytes, memory.memoryHeaps[i].size);
    }

    auto reject = [&record] (std::string reason) {
      Logger::info(str::format("  Skipping: ", reason));
      record.rejection = std::move(reason);
      startup_report::addAdapter(std::move(record));
      return false;
    };

    std::string compatError;

    if (!adapter.isCompatible(compatError))
      return reject(compatError);

    if (m_flags.test(DxvkDeviceFilterFlag::MatchDeviceName)) {
      if (std::string(properties.core.properties.deviceName).find(m_matchDeviceName) == std::string::npos)
        return reject("Device filter");
    }

    if (m_flags.test(DxvkDeviceFilterFlag::MatchDeviceUUID)) {
      std::string uuidStr = convertUUID(properties.vk11.deviceUUID);

      if (uuidStr.find(m_matchDeviceUUID) == std::string::npos)
        return reject("UUID filter");
    }

    if (m_flags.test(DxvkDeviceFilterFlag::SkipCpuDevices)) {
      if (properties.core.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        return reject("Software driver");
    }

    record.accepted = true;
    startup_report::addAdapter(std::move(record));
    return true;
  }

}
