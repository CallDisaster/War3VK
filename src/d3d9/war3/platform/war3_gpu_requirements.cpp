#include "war3_gpu_requirements.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dxvk::war3::gpureq {

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kDiscreteThreshold = 512ull * kMiB;

// 下限依据（2026-11 核对）：
// - NVIDIA：551.23 只报 Vulkan 1.3.271，没有 VK_KHR_maintenance6；
//   551.61（2024-02）起报 1.3.277 并带 maintenance6。Kepler 停在 474.xx。
// - AMD：Adrenalin 24.2.1（2024-02）加入 VK_KHR_maintenance6；
//   Polaris/Vega 的 Windows 驱动没有 maintenance5（DXVK 2.7 发布说明）。
// - Intel：32.0.101.6557 起列出 VK_KHR_maintenance6（Arc/Xe 驱动）；
//   6～10 代核显的旧版驱动分支未核实，按不支持提示。
const VendorFloor kFloors[] = {
  { Vendor::Nvidia, "551.61", "https://www.nvidia.cn/drivers/",
    "GeForce GTX 750/900 系列（Maxwell）及更新的显卡：GTX 10/16 系列、RTX 全系列，"
    "以及 MX110/130/150/250/350/450 等笔记本显卡",
    "GT 710/730、GTX 600 系列和 GTX 700 系列的大部分型号（Kepler 架构），"
    "它们的驱动最高只到 474.xx" },
  { Vendor::Amd, "24.2.1", "https://www.amd.com/zh-cn/support/download/drivers.html",
    "Radeon RX 5000/6000/7000/9000 系列，以及 Ryzen 6000 及更新处理器里的 "
    "Radeon 610M/660M/680M/740M/760M/780M/880M/890M 核显",
    "RX 400/500 系列（Polaris）、Vega 系列、Radeon VII、带 Vega 核显的 Ryzen 处理器"
    "（如 5600G/5700G）以及更早的 HD 7000/R7/R9 系列：AMD 不再为它们的 Windows 驱动"
    "加入所需的 Vulkan 功能" },
  { Vendor::Intel, "32.0.101.6557", "https://www.intel.cn/content/www/cn/zh/download-center/home.html",
    "Arc 独立显卡、Core Ultra 处理器核显，以及 11 代及更新酷睿的 Iris Xe 核显",
    "6～10 代酷睿的 HD/UHD 6xx 核显和 Iris Plus 核显：它们的驱动已经不再更新到所需版本" },
};

DriverVersion make(std::initializer_list<uint32_t> parts, const char* format) {
  DriverVersion v;
  for (uint32_t p : parts) {
    if (v.count < 4)
      v.parts[v.count++] = p;
  }
  char buffer[64] = {};
  switch (v.count) {
    case 2: std::snprintf(buffer, sizeof(buffer), format, v.parts[0], v.parts[1]); break;
    case 3: std::snprintf(buffer, sizeof(buffer), format, v.parts[0], v.parts[1], v.parts[2]); break;
    case 4: std::snprintf(buffer, sizeof(buffer), format, v.parts[0], v.parts[1], v.parts[2], v.parts[3]); break;
    default: break;
  }
  v.text = buffer;
  return v;
}

void umdParts(uint64_t umd, uint32_t out[4]) {
  out[0] = uint32_t((umd >> 48) & 0xffff);
  out[1] = uint32_t((umd >> 32) & 0xffff);
  out[2] = uint32_t((umd >> 16) & 0xffff);
  out[3] = uint32_t(umd & 0xffff);
}

std::string formatMemory(const AdapterFacts& a) {
  char buffer[64] = {};
  if (a.dedicatedVideoMemory >= kDiscreteThreshold) {
    const uint64_t mib = a.dedicatedVideoMemory / kMiB;
    if (mib >= 1024)
      std::snprintf(buffer, sizeof(buffer), "%.1f GB 显存", double(mib) / 1024.0);
    else
      std::snprintf(buffer, sizeof(buffer), "%u MB 显存", unsigned(mib));
  } else {
    std::snprintf(buffer, sizeof(buffer), "核显，共享内存");
  }
  return buffer;
}

bool startsWith(const std::string& s, const char* prefix) {
  return s.compare(0, std::strlen(prefix), prefix) == 0;
}

bool isFilterRejection(const std::string& compatError) {
  return compatError == "Device filter" || compatError == "UUID filter";
}

std::string between(const std::string& s, const std::string& open, const std::string& close) {
  const size_t a = s.find(open);
  if (a == std::string::npos)
    return {};
  const size_t b = s.find(close, a + open.size());
  if (b == std::string::npos)
    return {};
  return s.substr(a + open.size(), b - a - open.size());
}

} // namespace

Vendor VendorFromPciId(uint32_t vendorId) {
  switch (vendorId) {
    case 0x10de: return Vendor::Nvidia;
    case 0x1002:
    case 0x1022: return Vendor::Amd;
    case 0x8086: return Vendor::Intel;
    case 0x1414: return Vendor::Microsoft;
    default: return Vendor::Unknown;
  }
}

const char* VendorName(Vendor vendor) {
  switch (vendor) {
    case Vendor::Nvidia: return "NVIDIA";
    case Vendor::Amd: return "AMD";
    case Vendor::Intel: return "Intel";
    case Vendor::Microsoft: return "Microsoft";
    default: return "显卡厂商";
  }
}

DriverVersion NvidiaFromDxgiUmd(uint64_t umd) {
  uint32_t p[4];
  umdParts(umd, p);
  // 31.0.15.5161 -> (5 * 10000 + 5161) = 55161 -> 551.61
  const uint32_t packed = (p[2] % 10) * 10000 + p[3];
  return make({ packed / 100, packed % 100 }, "%u.%02u");
}

DriverVersion IntelFromDxgiUmd(uint64_t umd) {
  return DottedFromDxgiUmd(umd);
}

DriverVersion DottedFromDxgiUmd(uint64_t umd) {
  uint32_t p[4];
  umdParts(umd, p);
  return make({ p[0], p[1], p[2], p[3] }, "%u.%u.%u.%u");
}

DriverVersion NvidiaFromVulkan(uint32_t version) {
  return make({ (version >> 22) & 0x3ff, (version >> 14) & 0xff }, "%u.%02u");
}

DriverVersion IntelFromVulkanWindows(uint32_t version) {
  return make({ version >> 14, version & 0x3fff }, "%u.%u");
}

DriverVersion ParseDotted(const std::string& text) {
  DriverVersion v;
  size_t i = 0;
  while (i < text.size() && !(text[i] >= '0' && text[i] <= '9'))
    ++i;
  const size_t begin = i;
  while (i < text.size() && v.count < 4) {
    if (!(text[i] >= '0' && text[i] <= '9'))
      break;
    uint32_t value = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      value = value * 10 + uint32_t(text[i] - '0');
      ++i;
    }
    v.parts[v.count++] = value;
    if (i < text.size() && text[i] == '.')
      ++i;
    else
      break;
  }
  size_t end = i;
  if (end > begin && text[end - 1] == '.')
    --end;
  v.text = text.substr(begin, end - begin);
  return v;
}

int Compare(const DriverVersion& a, const DriverVersion& b) {
  const uint32_t n = std::max(a.count, b.count);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t x = i < a.count ? a.parts[i] : 0;
    const uint32_t y = i < b.count ? b.parts[i] : 0;
    if (x != y)
      return x < y ? -1 : 1;
  }
  return 0;
}

const VendorFloor* FloorFor(Vendor vendor) {
  for (const auto& f : kFloors) {
    if (f.vendor == vendor)
      return &f;
  }
  return nullptr;
}

std::string ExplainCompatError(const std::string& compatError, uint32_t vulkanApiVersion) {
  if (compatError.empty())
    return {};
  if (startsWith(compatError, "Device does not support Vulkan")) {
    char buffer[160] = {};
    if (vulkanApiVersion) {
      std::snprintf(buffer, sizeof(buffer), "驱动只支持到 Vulkan %u.%u，WarVK 需要 Vulkan 1.3",
        unsigned(vulkanApiVersion >> 22) & 0x7f, unsigned(vulkanApiVersion >> 12) & 0x3ff);
    } else {
      std::snprintf(buffer, sizeof(buffer), "驱动不支持 Vulkan 1.3，WarVK 需要 Vulkan 1.3");
    }
    return buffer;
  }
  if (startsWith(compatError, "Device does not support required feature")) {
    const std::string feature = between(compatError, "'", "'");
    const std::string extension = between(compatError, "(extension: ", ")");
    if (!extension.empty())
      return "驱动缺少 WarVK 必需的 Vulkan 扩展 " + extension;
    return "驱动缺少 WarVK 必需的 Vulkan 功能 " + feature;
  }
  if (isFilterRejection(compatError))
    return "这块显卡被设备过滤设置排除了（dxvk.conf 里的 dxgi.deviceFilter，"
           "或环境变量 DXVK_FILTER_DEVICE_NAME / DXVK_FILTER_DEVICE_UUID）";
  if (compatError == "Software driver")
    return "这是软件渲染设备，不能运行 WarVK";
  if (startsWith(compatError, "Device does not have a graphics queue"))
    return "这个 Vulkan 设备不能用来画图（没有图形队列）";
  if (compatError.find("push data") != std::string::npos)
    return "驱动提供的推送常量空间不够";
  return compatError;
}

const AdapterFacts* PrimaryAdapter(const StartupDiagnosis& diagnosis) {
  const AdapterFacts* best = nullptr;
  int bestScore = -1;
  for (const auto& a : diagnosis.adapters) {
    int score = 0;
    if (a.software || a.vendor == Vendor::Microsoft)
      score = 0;
    else if (a.dedicatedVideoMemory >= kDiscreteThreshold
          && (a.vendor == Vendor::Nvidia || a.vendor == Vendor::Amd || a.vendor == Vendor::Intel))
      score = 3;
    else if (a.vendor != Vendor::Unknown)
      score = 2;
    else
      score = 1;
    if (score > bestScore) {
      best = &a;
      bestScore = score;
    }
  }
  return best;
}

Advice ComposeAdvice(const StartupDiagnosis& diagnosis, const std::string& reportPath) {
  Advice advice;
  advice.title = "WarVK 无法启动";

  const AdapterFacts* primary = PrimaryAdapter(diagnosis);
  std::string reason;
  switch (diagnosis.kind) {
    case FailureKind::NoVulkanRuntime:
      reason = "没有找到 Vulkan 运行库（vulkan-1.dll）。通常是显卡驱动没有安装，"
               "或者装的是 Windows 自带的基础显示驱动。";
      break;
    case FailureKind::VulkanInstanceFailed:
      reason = "Vulkan 初始化失败。通常是显卡驱动损坏或版本太旧。";
      break;
    case FailureKind::NoCompatibleDevice: {
      std::string why = primary ? ExplainCompatError(primary->compatError, primary->vulkanApiVersion) : std::string();
      if (primary && !primary->vulkanSeen && !primary->software)
        why = "这块显卡没有出现在 Vulkan 里，显卡驱动可能没装好或版本太旧";
      reason = why.empty() ? "没有一块显卡满足 WarVK 的要求。" : why + "。";
      break;
    }
    case FailureKind::DeviceCreateFailed:
      reason = "创建 Vulkan 设备失败。";
      break;
    default:
      reason = "启动时发生错误。";
      break;
  }
  if (!diagnosis.detail.empty() && diagnosis.kind != FailureKind::NoCompatibleDevice)
    reason += "\n错误信息：" + diagnosis.detail;

  std::string adapters;
  for (const auto& a : diagnosis.adapters) {
    if (a.software && diagnosis.adapters.size() > 1)
      continue;
    adapters += "  · " + a.name + "（" + formatMemory(a) + "）  驱动版本 "
              + (a.driver.valid() ? a.driver.text : std::string("未知")) + "\n";
  }
  if (adapters.empty())
    adapters = "  · 没有检测到显卡\n";

  std::string steps;
  const VendorFloor* floor = primary ? FloorFor(primary->vendor) : nullptr;
  const bool filtered = primary && isFilterRejection(primary->compatError);
  if (filtered) {
    steps += "1. 打开游戏目录里的 dxvk.conf，删除 dxgi.deviceFilter 这一行；"
             "或者清除环境变量 DXVK_FILTER_DEVICE_NAME / DXVK_FILTER_DEVICE_UUID。\n";
  } else if (floor) {
    const bool driverKnown = primary->driver.valid();
    const bool driverOld = driverKnown && Compare(primary->driver, ParseDotted(floor->minimumDriver)) < 0;
    steps += std::string("1. 把 ") + VendorName(primary->vendor) + " 显卡驱动更新到 "
           + floor->minimumDriver + " 或更新的版本（建议直接装最新版）：\n   "
           + floor->downloadUrl + "\n";
    if (driverKnown && !driverOld && diagnosis.kind == FailureKind::NoCompatibleDevice)
      steps += "   你的驱动已经是 " + primary->driver.text + "，不低于 " + floor->minimumDriver
             + "。如果仍然报这个错，说明这块显卡本身不在支持范围内。\n";
    steps += std::string("2. 支持的显卡：") + floor->supportedFamilies + "。\n";
    steps += std::string("   不支持的显卡：") + floor->unsupportedFamilies + "。\n";
  } else {
    steps += "1. 把显卡驱动更新到最新版本。\n";
  }

  bool hybrid = false;
  bool hasIntel = false;
  for (const auto& a : diagnosis.adapters) {
    hasIntel |= a.vendor == Vendor::Intel;
    hybrid |= (a.vendor == Vendor::Nvidia || a.vendor == Vendor::Amd) && a.dedicatedVideoMemory >= kDiscreteThreshold;
  }
  if (hybrid && hasIntel)
    steps += "3. 双显卡笔记本：在 Windows 设置 → 系统 → 显示 → 图形 里，把 war3.exe 设为“高性能”。\n";

  advice.body = "WarVK 画面增强没能在这台电脑上启动。关闭这个窗口后，魔兽争霸还会弹出它自己的"
                "错误提示，然后退出。\n\n"
                "原因：" + reason + "\n\n"
                "检测到的显卡：\n" + adapters + "\n"
                "怎么解决：\n" + steps + "\n"
                "如果按上面做了还是不行，可以先删除游戏目录里的 d3d9.dll"
                "（用过锁定脚本的先运行 WarVK_UnlockD3D9.cmd），游戏会恢复原版画面。\n";
  if (!reportPath.empty())
    advice.body += "详细信息已保存到：" + reportPath + "\n";
  advice.body += "（按 Ctrl+C 可以复制这个窗口里的全部文字）";
  return advice;
}

VramTier ClassifyVram(uint64_t deviceLocalBytes, bool integrated) {
  if (integrated)
    return VramTier::Low;
  const uint64_t mib = deviceLocalBytes / kMiB;
  if (mib <= 2560)
    return VramTier::Low;
  if (mib <= 5632)
    return VramTier::Medium;
  return VramTier::High;
}

bool ParseVramTier(const std::string& text, VramTier& tier) {
  std::string value;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      continue;
    value += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
  }
  if (value == "low" || value == "\xE4\xBD\x8E") {
    tier = VramTier::Low;
    return true;
  }
  if (value == "medium" || value == "mid" || value == "\xE4\xB8\xAD") {
    tier = VramTier::Medium;
    return true;
  }
  if (value == "high" || value == "\xE9\xAB\x98") {
    tier = VramTier::High;
    return true;
  }
  return false;
}

uint32_t CsmResolutionCapFor(VramTier tier) {
  return tier == VramTier::Low ? 2048u : 4096u;
}

const char* VramTierName(VramTier tier) {
  switch (tier) {
    case VramTier::Low: return "low";
    case VramTier::Medium: return "medium";
    case VramTier::High: return "high";
  }
  return "unknown";
}

} // namespace dxvk::war3::gpureq
