// Host-only proof for the player-facing GPU floor, driver decoding and advice text.
// No Vulkan, DXGI or window is involved.
#include "../src/d3d9/war3/platform/war3_gpu_requirements.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dxvk::war3::gpureq;

static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static uint64_t umd(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
  return (uint64_t(a) << 48) | (uint64_t(b) << 32) | (uint64_t(c) << 16) | uint64_t(d);
}

static bool contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

static AdapterFacts adapter(const char* name, uint32_t vendorId, uint64_t dedicatedMiB, DriverVersion driver) {
  AdapterFacts a;
  a.name = name;
  a.vendorId = vendorId;
  a.vendor = VendorFromPciId(vendorId);
  a.dedicatedVideoMemory = dedicatedMiB * 1024ull * 1024ull;
  a.driver = driver;
  return a;
}

int main() {
  // Vendor IDs.
  CHECK(VendorFromPciId(0x10de) == Vendor::Nvidia);
  CHECK(VendorFromPciId(0x1002) == Vendor::Amd);
  CHECK(VendorFromPciId(0x8086) == Vendor::Intel);
  CHECK(VendorFromPciId(0x1414) == Vendor::Microsoft);
  CHECK(VendorFromPciId(0x5143) == Vendor::Unknown);

  // NVIDIA DXGI UMD: 31.0.15.5161 -> 551.61, 31.0.15.3742 -> 537.42, 27.21.14.7489 -> 474.89.
  CHECK(NvidiaFromDxgiUmd(umd(31, 0, 15, 5161)).text == "551.61");
  CHECK(NvidiaFromDxgiUmd(umd(31, 0, 15, 3742)).text == "537.42");
  CHECK(NvidiaFromDxgiUmd(umd(27, 21, 14, 7489)).text == "474.89");
  CHECK(NvidiaFromDxgiUmd(umd(32, 0, 15, 6094)).text == "560.94");
  CHECK(NvidiaFromDxgiUmd(umd(31, 0, 15, 5105)).text == "551.05");

  // Vulkan driverVersion encodings.
  const uint32_t nv55161 = (551u << 22) | (61u << 14);
  CHECK(NvidiaFromVulkan(nv55161).text == "551.61");
  CHECK(IntelFromVulkanWindows((101u << 14) | 6557u).text == "101.6557");
  CHECK(DottedFromDxgiUmd(umd(32, 0, 101, 6557)).text == "32.0.101.6557");
  CHECK(IntelFromDxgiUmd(umd(31, 0, 101, 2130)).text == "31.0.101.2130");

  // Parsing and comparing.
  CHECK(ParseDotted("24.4.1 (LLPC)").text == "24.4.1");
  CHECK(ParseDotted("24.4.1 (LLPC)").count == 3);
  CHECK(ParseDotted("Adrenalin 23.11.1").text == "23.11.1");
  CHECK(ParseDotted("").count == 0);
  CHECK(ParseDotted("551.").text == "551");
  CHECK(Compare(ParseDotted("551.61"), ParseDotted("551.61")) == 0);
  CHECK(Compare(ParseDotted("551.23"), ParseDotted("551.61")) < 0);
  CHECK(Compare(ParseDotted("560.94"), ParseDotted("551.61")) > 0);
  CHECK(Compare(ParseDotted("24.1.1"), ParseDotted("24.2.1")) < 0);
  CHECK(Compare(ParseDotted("24.10.1"), ParseDotted("24.2.1")) > 0);
  CHECK(Compare(ParseDotted("23.12.1"), ParseDotted("24.2.1")) < 0);
  CHECK(Compare(ParseDotted("24.2"), ParseDotted("24.2.0")) == 0);
  CHECK(Compare(ParseDotted("31.0.101.2130"), ParseDotted("32.0.101.6557")) < 0);
  CHECK(Compare(ParseDotted("32.0.101.6790"), ParseDotted("32.0.101.6557")) > 0);

  // Floors.
  CHECK(std::string(FloorFor(Vendor::Nvidia)->minimumDriver) == "551.61");
  CHECK(std::string(FloorFor(Vendor::Amd)->minimumDriver) == "24.2.1");
  CHECK(std::string(FloorFor(Vendor::Intel)->minimumDriver) == "32.0.101.6557");
  CHECK(FloorFor(Vendor::Microsoft) == nullptr);
  CHECK(FloorFor(Vendor::Unknown) == nullptr);

  // DXVK compatibility strings.
  CHECK(contains(ExplainCompatError("Device does not support Vulkan 1.3", (1u << 22) | (2u << 12) | 175u), "Vulkan 1.2"));
  CHECK(contains(ExplainCompatError("Device does not support required feature 'maintenance6' (extension: VK_KHR_maintenance6)", 0),
                 "VK_KHR_maintenance6"));
  CHECK(contains(ExplainCompatError("Device does not support required feature 'nullDescriptor'", 0), "nullDescriptor"));
  CHECK(ExplainCompatError("", 0).empty());

  // Primary adapter: discrete over iGPU over software.
  {
    StartupDiagnosis d;
    d.adapters.push_back(adapter("Intel(R) UHD Graphics 610", 0x8086, 128, ParseDotted("31.0.101.2130")));
    d.adapters.push_back(adapter("NVIDIA GeForce MX150", 0x10de, 2048, ParseDotted("537.42")));
    AdapterFacts basic = adapter("Microsoft Basic Render Driver", 0x1414, 0, {});
    basic.software = true;
    d.adapters.push_back(basic);
    CHECK(PrimaryAdapter(d) == &d.adapters[1]);
  }
  {
    StartupDiagnosis d;
    CHECK(PrimaryAdapter(d) == nullptr);
  }

  // Old NVIDIA driver: the advice names the floor, the player's version and the hybrid hint.
  {
    StartupDiagnosis d;
    d.kind = FailureKind::NoCompatibleDevice;
    d.adapters.push_back(adapter("Intel(R) UHD Graphics 610", 0x8086, 128, ParseDotted("31.0.101.2130")));
    AdapterFacts nv = adapter("NVIDIA GeForce MX150", 0x10de, 2048, ParseDotted("537.42"));
    nv.vulkanSeen = true;
    nv.vulkanApiVersion = (1u << 22) | (3u << 12) | 260u;
    nv.compatError = "Device does not support required feature 'maintenance6' (extension: VK_KHR_maintenance6)";
    d.adapters.push_back(nv);
    const Advice a = ComposeAdvice(d, "E:\\Game\\WarVK\\Logs\\startup-error.txt");
    CHECK(contains(a.title, "WarVK"));
    CHECK(contains(a.body, "551.61"));
    CHECK(contains(a.body, "537.42"));
    CHECK(contains(a.body, "NVIDIA GeForce MX150"));
    CHECK(contains(a.body, "VK_KHR_maintenance6"));
    CHECK(contains(a.body, "nvidia"));
    CHECK(contains(a.body, "startup-error.txt"));
    CHECK(contains(a.body, "高性能"));
    CHECK(!contains(a.body, "已经是"));
  }

  // New-enough driver that still fails: say the card itself is unsupported.
  {
    StartupDiagnosis d;
    d.kind = FailureKind::NoCompatibleDevice;
    AdapterFacts amd = adapter("Radeon RX 580 Series", 0x1002, 8192, ParseDotted("24.9.1"));
    amd.vulkanSeen = true;
    amd.vulkanApiVersion = (1u << 22) | (3u << 12) | 260u;
    amd.compatError = "Device does not support required feature 'maintenance5' (extension: VK_KHR_maintenance5)";
    d.adapters.push_back(amd);
    const Advice a = ComposeAdvice(d, "");
    CHECK(contains(a.body, "24.2.1"));
    CHECK(contains(a.body, "24.9.1"));
    CHECK(contains(a.body, "已经是"));
    CHECK(contains(a.body, "Polaris"));
    CHECK(!contains(a.body, "高性能"));
    CHECK(!contains(a.body, "详细信息已保存到"));
  }

  // No Vulkan runtime and no vendor driver: generic driver advice, unknown version.
  {
    StartupDiagnosis d;
    d.kind = FailureKind::NoVulkanRuntime;
    AdapterFacts basic = adapter("Microsoft Basic Display Adapter", 0x1414, 0, {});
    basic.software = true;
    d.adapters.push_back(basic);
    const Advice a = ComposeAdvice(d, "");
    CHECK(contains(a.body, "vulkan-1.dll"));
    CHECK(contains(a.body, "未知"));
    CHECK(contains(a.body, "最新版本"));
  }

  // Card absent from Vulkan entirely.
  {
    StartupDiagnosis d;
    d.kind = FailureKind::NoCompatibleDevice;
    d.adapters.push_back(adapter("NVIDIA GeForce GT 730", 0x10de, 2048, ParseDotted("474.89")));
    const Advice a = ComposeAdvice(d, "");
    CHECK(contains(a.body, "没有出现在 Vulkan 里"));
    CHECK(contains(a.body, "474.89"));
    CHECK(contains(a.body, "Kepler"));
  }

  // Rejected by a device filter: point at the filter, not at the driver.
  {
    StartupDiagnosis d;
    d.kind = FailureKind::NoCompatibleDevice;
    AdapterFacts nv = adapter("NVIDIA GeForce RTX 4070", 0x10de, 12282, ParseDotted("576.02"));
    nv.vulkanSeen = true;
    nv.compatError = "Device filter";
    d.adapters.push_back(nv);
    const Advice a = ComposeAdvice(d, "");
    CHECK(contains(a.body, "dxgi.deviceFilter"));
    CHECK(contains(a.body, "576.02"));
    CHECK(!contains(a.body, "551.61"));
    CHECK(!contains(a.body, "已经是"));
  }

  // VRAM tiers.
  const uint64_t gib = 1024ull * 1024ull * 1024ull;
  CHECK(ClassifyVram(2 * gib, false) == VramTier::Low);
  CHECK(ClassifyVram(3 * gib, false) == VramTier::Medium);
  CHECK(ClassifyVram(4 * gib, false) == VramTier::Medium);
  CHECK(ClassifyVram(6 * gib, false) == VramTier::High);
  CHECK(ClassifyVram(24 * gib, false) == VramTier::High);
  CHECK(ClassifyVram(16 * gib, true) == VramTier::Low);
  // Drivers report a little less than the marketed size.
  CHECK(ClassifyVram(6 * gib - 200ull * 1024 * 1024, false) == VramTier::High);
  CHECK(ClassifyVram(3 * gib - 200ull * 1024 * 1024, false) == VramTier::Medium);
  CHECK(std::string(VramTierName(VramTier::Medium)) == "medium");

  // Player overrides.
  VramTier parsed = VramTier::High;
  CHECK(ParseVramTier("low", parsed) && parsed == VramTier::Low);
  CHECK(ParseVramTier(" Medium\r\n", parsed) && parsed == VramTier::Medium);
  CHECK(ParseVramTier("HIGH", parsed) && parsed == VramTier::High);
  CHECK(ParseVramTier("\xE4\xBD\x8E", parsed) && parsed == VramTier::Low);
  parsed = VramTier::Medium;
  CHECK(!ParseVramTier("auto", parsed) && parsed == VramTier::Medium);
  CHECK(!ParseVramTier("", parsed));
  CHECK(!ParseVramTier("ultra", parsed));
  CHECK(CsmResolutionCapFor(VramTier::Low) == 2048u);
  CHECK(CsmResolutionCapFor(VramTier::Medium) == 4096u);
  CHECK(CsmResolutionCapFor(VramTier::High) == 4096u);

  std::printf("gpu_requirements: %u checks passed\n", checks);
  return 0;
}
