#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dxvk::war3::gpureq {

/**
 * @brief 显卡厂商（按 PCI vendor ID 归类）。
 */
enum class Vendor : uint32_t {
  Unknown,
  Nvidia,
  Amd,
  Intel,
  Microsoft,
};

Vendor VendorFromPciId(uint32_t vendorId);
const char* VendorName(Vendor vendor);

/**
 * @brief 面向玩家的驱动版本号（厂商自己的写法）。
 *
 * NVIDIA 写作 "551.61"，AMD 写作 "24.2.1"，Intel 写作 "32.0.101.6557"。
 * 比较只看数字段，不看文本。
 */
struct DriverVersion {
  uint32_t parts[4] = {};
  uint32_t count = 0;
  std::string text;

  bool valid() const { return count != 0; }
};

/** DXGI 用户态驱动版本（CheckInterfaceSupport 返回的 a.b.c.d，各 16 位）。 */
DriverVersion NvidiaFromDxgiUmd(uint64_t umd);
DriverVersion IntelFromDxgiUmd(uint64_t umd);
DriverVersion DottedFromDxgiUmd(uint64_t umd);

/** Vulkan VkPhysicalDeviceProperties::driverVersion 的厂商编码。 */
DriverVersion NvidiaFromVulkan(uint32_t version);
DriverVersion IntelFromVulkanWindows(uint32_t version);

/** 解析 "24.4.1 (LLPC)"、"551.61" 之类的文本，取开头的数字段。 */
DriverVersion ParseDotted(const std::string& text);

/** 逐段比较；缺的段按 0 处理。返回 -1/0/1。 */
int Compare(const DriverVersion& a, const DriverVersion& b);

/**
 * @brief 各厂商在 Windows 上满足 WarVK 硬性要求的最低驱动。
 *
 * 硬性要求来自 src/dxvk/dxvk_device_info.cpp：Vulkan 1.3 加
 * VK_KHR_maintenance5/6、VK_EXT_robustness2 等扩展。下限就是第一个
 * 同时提供这些扩展的公开驱动。
 */
struct VendorFloor {
  Vendor vendor;
  const char* minimumDriver;     ///< 玩家要装到的最低版本（厂商写法）
  const char* downloadUrl;       ///< 官方驱动下载页
  const char* supportedFamilies; ///< 支持的显卡系列（中文）
  const char* unsupportedFamilies; ///< 已知不支持的系列（中文）
};

const VendorFloor* FloorFor(Vendor vendor);

/**
 * @brief 启动失败的类别，按从早到晚的顺序。
 */
enum class FailureKind : uint32_t {
  None,
  NoVulkanRuntime,     ///< 找不到 vulkan-1.dll：驱动没装或太旧
  VulkanInstanceFailed,///< 有运行库但创建实例失败
  NoCompatibleDevice,  ///< 有 Vulkan 设备，但没有一个满足要求
  DeviceCreateFailed,  ///< 选中了设备，但创建设备失败
  Other,
};

/**
 * @brief 一块显卡的事实（DXGI 与 Vulkan 两边合并）。
 */
struct AdapterFacts {
  std::string name;
  Vendor vendor = Vendor::Unknown;
  uint32_t vendorId = 0;
  uint32_t deviceId = 0;
  uint64_t dedicatedVideoMemory = 0; ///< 字节；集显通常很小或为 0
  uint64_t sharedSystemMemory = 0;   ///< 字节
  bool software = false;             ///< Microsoft Basic Render 等软件设备
  DriverVersion driver;

  bool vulkanSeen = false;           ///< Vulkan 枚举到了这块卡
  uint32_t vulkanApiVersion = 0;     ///< VK_MAKE_API_VERSION 编码
  std::string compatError;           ///< DXVK 给出的不兼容原因（英文原文）
};

struct StartupDiagnosis {
  FailureKind kind = FailureKind::None;
  std::string detail;                ///< 失败时的原始错误文本（英文）
  std::vector<AdapterFacts> adapters;
};

/**
 * @brief 给玩家看的报错文字（UTF-8，中文）。
 */
struct Advice {
  std::string title;
  std::string body;
};

/** 把 DXVK 的英文不兼容原因翻成玩家能看懂的一句话。 */
std::string ExplainCompatError(const std::string& compatError, uint32_t vulkanApiVersion);

/** 选出最值得给建议的那块卡：独显优先，软件设备最后。 */
const AdapterFacts* PrimaryAdapter(const StartupDiagnosis& diagnosis);

Advice ComposeAdvice(const StartupDiagnosis& diagnosis, const std::string& reportPath);

/** 显存分级，用于开局自动选择画质预设。 */
enum class VramTier : uint32_t {
  Low,     ///< 2 GB 及以下，或共享显存的集显
  Medium,  ///< 3～5 GB
  High,    ///< 6 GB 及以上
};

VramTier ClassifyVram(uint64_t deviceLocalBytes, bool integrated);
const char* VramTierName(VramTier tier);

/** 解析玩家写的档位：low/medium/high（也接受 低/中/高）。auto 或无法识别返回 false。 */
bool ParseVramTier(const std::string& text, VramTier& tier);

/** 档位对应的阴影贴图分辨率上限：低档 2048，其余 4096。 */
uint32_t CsmResolutionCapFor(VramTier tier);

} // namespace dxvk::war3::gpureq
