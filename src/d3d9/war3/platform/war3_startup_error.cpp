#include "war3_startup_error.h"
#include "war3_gpu_requirements.h"

#include "../../../dxvk/dxvk_startup_report.h"
#include "../../../util/log/log.h"
#include "../../../util/util_string.h"

#include <windows.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <vector>

namespace dxvk::war3::startup {

namespace {

using namespace dxvk::war3::gpureq;

// 本文件只在启动失败时运行一次，不在帧路径上。

// IID 直接写在这里，避免依赖 dxguid/uuid 导入库。
const GUID kIidDxgiFactory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
const GUID kIidDxgiDevice   = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };

std::atomic<bool> g_reported{false};

// VkPhysicalDeviceType 的取值；这里不引入 Vulkan 头，避免和 dxgi.h 混用。
constexpr uint32_t kVkDeviceTypeDiscrete = 2;
constexpr uint32_t kVkDeviceTypeCpu = 4;

struct RawAdapter {
  AdapterFacts facts;
  uint64_t umd = 0;            ///< DXGI 用户态驱动版本，0 表示未知
  uint64_t registryMemory = 0; ///< 注册表 HardwareInformation.qwMemorySize
  std::string registryDriver;  ///< 注册表 DriverVersion（a.b.c.d）
  std::string radeonVersion;   ///< 注册表 RadeonSoftwareVersion
  bool fromDxgi = false;
  bool vulkanAccepted = false;
  std::string vulkanDriverName;
  std::string vulkanDriverInfo;
  uint32_t vulkanDriverRaw = 0;
  uint64_t vulkanDeviceLocal = 0;
};

std::string WideToUtf8(const wchar_t* text) {
  if (!text || !*text)
    return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1)
    return {};
  std::string out(size_t(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
  return out;
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty())
    return {};
  const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
  if (len <= 0)
    return {};
  std::wstring out(size_t(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), len);
  return out;
}

std::string ToCrlf(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 64);
  for (char c : text) {
    if (c == '\n')
      out += '\r';
    out += c;
  }
  return out;
}

std::string Hex4(uint32_t v) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "%04X", v & 0xffffu);
  return buffer;
}

void EnumerateDxgi(std::vector<RawAdapter>& out) {
  std::array<wchar_t, MAX_PATH> path = {};
  const UINT len = GetSystemDirectoryW(path.data(), UINT(path.size()));
  if (len == 0 || len >= path.size() - 16)
    return;
  std::wstring dll(path.data(), len);
  dll += L"\\dxgi.dll";

  // 只从系统目录加载，避开游戏目录里可能存在的第三方 dxgi.dll。
  HMODULE module = LoadLibraryW(dll.c_str());
  if (!module)
    return;

  using CreateFactory1 = HRESULT (WINAPI*)(REFIID, void**);
  auto create = reinterpret_cast<CreateFactory1>(
    reinterpret_cast<void*>(GetProcAddress(module, "CreateDXGIFactory1")));
  IDXGIFactory1* factory = nullptr;
  if (create && SUCCEEDED(create(kIidDxgiFactory1, reinterpret_cast<void**>(&factory))) && factory) {
    for (UINT i = 0; i < 16; ++i) {
      IDXGIAdapter1* adapter = nullptr;
      if (factory->EnumAdapters1(i, &adapter) != S_OK || !adapter)
        break;
      DXGI_ADAPTER_DESC1 desc = {};
      if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        RawAdapter raw;
        raw.fromDxgi = true;
        raw.facts.name = WideToUtf8(desc.Description);
        raw.facts.vendorId = desc.VendorId;
        raw.facts.deviceId = desc.DeviceId;
        raw.facts.vendor = VendorFromPciId(desc.VendorId);
        raw.facts.dedicatedVideoMemory = uint64_t(desc.DedicatedVideoMemory);
        raw.facts.sharedSystemMemory = uint64_t(desc.SharedSystemMemory);
        raw.facts.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        LARGE_INTEGER umd = {};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(kIidDxgiDevice, &umd)))
          raw.umd = uint64_t(umd.QuadPart);
        // 同一块卡在多显示器时可能出现多次，按 ID 去重。
        const bool duplicate = std::any_of(out.begin(), out.end(), [&](const RawAdapter& r) {
          return r.facts.vendorId == raw.facts.vendorId && r.facts.deviceId == raw.facts.deviceId
              && r.facts.name == raw.facts.name;
        });
        if (!duplicate)
          out.push_back(std::move(raw));
      }
      adapter->Release();
    }
    factory->Release();
  }
  FreeLibrary(module);
}

bool ParsePciIds(const std::wstring& matching, uint32_t& vendor, uint32_t& device) {
  std::wstring lower = matching;
  for (auto& c : lower)
    c = wchar_t(towlower(c));
  const size_t v = lower.find(L"ven_");
  const size_t d = lower.find(L"dev_");
  if (v == std::wstring::npos || d == std::wstring::npos)
    return false;
  vendor = uint32_t(std::wcstoul(lower.c_str() + v + 4, nullptr, 16));
  device = uint32_t(std::wcstoul(lower.c_str() + d + 4, nullptr, 16));
  return vendor != 0;
}

std::wstring RegString(HKEY key, const wchar_t* name) {
  std::array<wchar_t, 512> buffer = {};
  DWORD type = 0;
  DWORD bytes = DWORD(sizeof(wchar_t) * (buffer.size() - 1));
  if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes) != ERROR_SUCCESS)
    return {};
  if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ)
    return {};
  return std::wstring(buffer.data());
}

uint64_t RegMemorySize(HKEY key) {
  uint64_t value = 0;
  DWORD type = 0;
  DWORD bytes = sizeof(value);
  if (RegQueryValueExW(key, L"HardwareInformation.qwMemorySize", nullptr, &type,
                       reinterpret_cast<BYTE*>(&value), &bytes) == ERROR_SUCCESS
      && (type == REG_QWORD || type == REG_BINARY) && bytes == sizeof(value))
    return value;
  uint32_t small = 0;
  bytes = sizeof(small);
  if (RegQueryValueExW(key, L"HardwareInformation.MemorySize", nullptr, &type,
                       reinterpret_cast<BYTE*>(&small), &bytes) == ERROR_SUCCESS
      && (type == REG_DWORD || type == REG_BINARY) && bytes == sizeof(small))
    return small;
  return 0;
}

/**
 * 显示适配器类键：AMD 的 Adrenalin 版本号只在这里有；32 位进程里
 * DXGI 的显存字段是 SIZE_T，大显存会被截断，这里有完整的 64 位值。
 */
void ReadDisplayClassRegistry(std::vector<RawAdapter>& adapters) {
  HKEY root = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
        0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS)
    return;
  for (DWORD i = 0; i < 64; ++i) {
    std::array<wchar_t, 64> sub = {};
    DWORD subLen = DWORD(sub.size());
    const LONG r = RegEnumKeyExW(root, i, sub.data(), &subLen, nullptr, nullptr, nullptr, nullptr);
    if (r == ERROR_NO_MORE_ITEMS)
      break;
    if (r != ERROR_SUCCESS)
      continue;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub.data(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
      continue;
    uint32_t vendor = 0, device = 0;
    if (ParsePciIds(RegString(key, L"MatchingDeviceId"), vendor, device)) {
      for (auto& a : adapters) {
        if (a.facts.vendorId != vendor || a.facts.deviceId != device)
          continue;
        a.registryMemory = std::max(a.registryMemory, RegMemorySize(key));
        if (a.registryDriver.empty())
          a.registryDriver = WideToUtf8(RegString(key, L"DriverVersion").c_str());
        if (a.radeonVersion.empty())
          a.radeonVersion = WideToUtf8(RegString(key, L"RadeonSoftwareVersion").c_str());
      }
    }
    RegCloseKey(key);
  }
  RegCloseKey(root);
}

uint64_t UmdFromDotted(const std::string& text) {
  const DriverVersion v = ParseDotted(text);
  if (v.count != 4)
    return 0;
  return (uint64_t(v.parts[0] & 0xffff) << 48) | (uint64_t(v.parts[1] & 0xffff) << 32)
       | (uint64_t(v.parts[2] & 0xffff) << 16) | uint64_t(v.parts[3] & 0xffff);
}

void MergeVulkan(std::vector<RawAdapter>& adapters, const DxvkStartupReportSnapshot& report) {
  for (const auto& vk : report.adapters) {
    if (vk.deviceType == kVkDeviceTypeCpu)
      continue;
    RawAdapter* match = nullptr;
    for (auto& a : adapters) {
      if (a.facts.vendorId == vk.vendorId && a.facts.deviceId == vk.deviceId && !a.facts.vulkanSeen) {
        match = &a;
        break;
      }
    }
    if (!match) {
      RawAdapter raw;
      raw.facts.name = vk.name;
      raw.facts.vendorId = vk.vendorId;
      raw.facts.deviceId = vk.deviceId;
      raw.facts.vendor = VendorFromPciId(vk.vendorId);
      if (vk.deviceType == kVkDeviceTypeDiscrete)
        raw.facts.dedicatedVideoMemory = vk.deviceLocalBytes;
      adapters.push_back(std::move(raw));
      match = &adapters.back();
    }
    match->facts.vulkanSeen = true;
    match->facts.vulkanApiVersion = vk.apiVersion;
    match->vulkanAccepted = vk.accepted;
    match->facts.compatError = vk.accepted ? std::string() : vk.rejection;
    match->vulkanDriverName = vk.driverName;
    match->vulkanDriverInfo = vk.driverInfo;
    match->vulkanDriverRaw = vk.driverVersionRaw;
    match->vulkanDeviceLocal = vk.deviceLocalBytes;
  }
}

void ResolveDriverAndMemory(RawAdapter& a) {
  a.facts.dedicatedVideoMemory = std::max(a.facts.dedicatedVideoMemory, a.registryMemory);
  uint64_t umd = a.umd ? a.umd : UmdFromDotted(a.registryDriver);
  switch (a.facts.vendor) {
    case Vendor::Nvidia:
      if (umd)
        a.facts.driver = NvidiaFromDxgiUmd(umd);
      else if (a.vulkanDriverRaw)
        a.facts.driver = NvidiaFromVulkan(a.vulkanDriverRaw);
      break;
    case Vendor::Amd:
      // DXGI/注册表里的 a.b.c.d 不是 Adrenalin 版本号，不能拿来和 24.2.1 比。
      if (!a.radeonVersion.empty())
        a.facts.driver = ParseDotted(a.radeonVersion);
      else if (!a.vulkanDriverInfo.empty())
        a.facts.driver = ParseDotted(a.vulkanDriverInfo);
      break;
    case Vendor::Intel:
      if (umd)
        a.facts.driver = DottedFromDxgiUmd(umd);
      else if (a.vulkanDriverRaw)
        a.facts.driver = IntelFromVulkanWindows(a.vulkanDriverRaw);
      break;
    default:
      if (umd)
        a.facts.driver = DottedFromDxgiUmd(umd);
      break;
  }
}

FailureKind KindFor(FailureSite site, DxvkStartupStage stage) {
  if (site == FailureSite::Device)
    return FailureKind::DeviceCreateFailed;
  switch (stage) {
    case DxvkStartupStage::LoaderMissing: return FailureKind::NoVulkanRuntime;
    case DxvkStartupStage::InstanceFailed: return FailureKind::VulkanInstanceFailed;
    case DxvkStartupStage::NoAdapter: return FailureKind::NoCompatibleDevice;
    default: return FailureKind::Other;
  }
}

const char* KindName(FailureKind kind) {
  switch (kind) {
    case FailureKind::NoVulkanRuntime: return "NoVulkanRuntime";
    case FailureKind::VulkanInstanceFailed: return "VulkanInstanceFailed";
    case FailureKind::NoCompatibleDevice: return "NoCompatibleDevice";
    case FailureKind::DeviceCreateFailed: return "DeviceCreateFailed";
    case FailureKind::Other: return "Other";
    default: return "None";
  }
}

std::wstring GameDirectory() {
  std::array<wchar_t, MAX_PATH * 2> path = {};
  const DWORD len = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  if (len == 0 || len >= path.size())
    return L".";
  std::wstring result(path.data(), len);
  const size_t slash = result.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring(L".") : result.substr(0, slash);
}

std::wstring EnsureLogDirectory() {
  std::wstring dir = GameDirectory() + L"\\WarVK";
  CreateDirectoryW(dir.c_str(), nullptr);
  dir += L"\\Log";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

bool WriteUtf8File(const std::wstring& path, const std::string& text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  static const char bom[] = "\xEF\xBB\xBF";
  DWORD written = 0;
  bool ok = WriteFile(file, bom, 3, &written, nullptr) && written == 3;
  const std::string body = ToCrlf(text);
  ok = ok && WriteFile(file, body.data(), DWORD(body.size()), &written, nullptr) && written == body.size();
  CloseHandle(file);
  return ok;
}

enum class DialogMode { Auto, Never, Force };

DialogMode ReadDialogMode() {
  const char* value = std::getenv("DXVK_WAR3_STARTUP_DIALOG");
  if (!value || !*value)
    return DialogMode::Auto;
  if (std::strcmp(value, "0") == 0 || _stricmp(value, "off") == 0 || _stricmp(value, "false") == 0)
    return DialogMode::Never;
  if (_stricmp(value, "force") == 0 || std::strcmp(value, "1") == 0 || _stricmp(value, "on") == 0)
    return DialogMode::Force;
  return DialogMode::Auto;
}

std::string ThreadDesktopName() {
  HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
  std::array<wchar_t, 256> name = {};
  DWORD needed = 0;
  if (!desktop || !GetUserObjectInformationW(desktop, UOI_NAME, name.data(), DWORD(sizeof(wchar_t) * name.size()), &needed))
    return {};
  return WideToUtf8(name.data());
}

/**
 * 魔兽在 Direct3DCreate9 失败后会卸载 d3d9.dll 再重试一次（2026-11-11 AutoTest：
 * 两次调用都在主线程、调用计数都从 1 开始）。重新加载会清空 DLL 里的静态变量，
 * 所以“本进程已经报告过”记在进程环境变量里，值是进程 ID：从世界编辑器启动的
 * war3.exe 会继承环境变量，但进程 ID 不同，照样会报告。
 */
constexpr wchar_t kReportedEnv[] = L"WARVK_STARTUP_FAILURE_REPORTED_PID";

bool AlreadyReportedInProcess() {
  std::array<wchar_t, 32> value = {};
  const DWORD len = GetEnvironmentVariableW(kReportedEnv, value.data(), DWORD(value.size()));
  if (len == 0 || len >= value.size())
    return false;
  return std::wcstoul(value.data(), nullptr, 10) == GetCurrentProcessId();
}

void MarkReportedInProcess() {
  const std::wstring pid = std::to_wstring(GetCurrentProcessId());
  SetEnvironmentVariableW(kReportedEnv, pid.c_str());
}

std::string TechnicalSection(FailureSite site, FailureKind kind, const std::string& detail,
                             const DxvkStartupReportSnapshot& report,
                             const std::vector<RawAdapter>& adapters) {
  std::string s = "---- 技术信息（反馈问题时请附上整个文件） ----\n";
  s += std::string("site=") + (site == FailureSite::Device ? "CreateDevice" : "Direct3DCreate9")
     + " kind=" + KindName(kind) + " stage=" + std::to_string(uint32_t(report.stage)) + "\n";
  if (!detail.empty())
    s += "error=" + detail + "\n";
  for (const auto& a : adapters) {
    char line[512] = {};
    std::snprintf(line, sizeof(line),
      "adapter name=\"%s\" pci=%s:%s dxgi=%d software=%d dedicatedMiB=%llu sharedMiB=%llu "
      "registryMiB=%llu umd=%u.%u.%u.%u\n",
      a.facts.name.c_str(), Hex4(a.facts.vendorId).c_str(), Hex4(a.facts.deviceId).c_str(),
      a.fromDxgi ? 1 : 0, a.facts.software ? 1 : 0,
      static_cast<unsigned long long>(a.facts.dedicatedVideoMemory >> 20),
      static_cast<unsigned long long>(a.facts.sharedSystemMemory >> 20),
      static_cast<unsigned long long>(a.registryMemory >> 20),
      unsigned(a.umd >> 48) & 0xffff, unsigned(a.umd >> 32) & 0xffff,
      unsigned(a.umd >> 16) & 0xffff, unsigned(a.umd) & 0xffff);
    s += line;
    s += "  driver=" + (a.facts.driver.valid() ? a.facts.driver.text : std::string("unknown"))
       + " registryDriver=" + a.registryDriver + " radeon=" + a.radeonVersion + "\n";
    if (a.facts.vulkanSeen) {
      std::snprintf(line, sizeof(line),
        "  vulkan api=%u.%u.%u driverRaw=0x%08X deviceLocalMiB=%llu accepted=%d\n",
        unsigned(a.facts.vulkanApiVersion >> 22) & 0x7f, unsigned(a.facts.vulkanApiVersion >> 12) & 0x3ff,
        unsigned(a.facts.vulkanApiVersion) & 0xfff, a.vulkanDriverRaw,
        static_cast<unsigned long long>(a.vulkanDeviceLocal >> 20), a.vulkanAccepted ? 1 : 0);
      s += line;
      s += "  vulkanDriver=\"" + a.vulkanDriverName + "\" info=\"" + a.vulkanDriverInfo + "\"\n";
      if (!a.facts.compatError.empty())
        s += "  rejected: " + a.facts.compatError + "\n";
    } else {
      s += "  vulkan: not enumerated\n";
    }
  }
  return s;
}

} // namespace

void ReportStartupFailure(FailureSite site, const std::string& detail) {
  bool expected = false;
  if (!g_reported.compare_exchange_strong(expected, true) || AlreadyReportedInProcess()) {
    Logger::warn(str::format("WarVK startup failure repeated (already reported): ", detail));
    return;
  }
  MarkReportedInProcess();

  try {
    const DxvkStartupReportSnapshot report = startup_report::snapshot();

    std::vector<RawAdapter> raw;
    EnumerateDxgi(raw);
    ReadDisplayClassRegistry(raw);
    MergeVulkan(raw, report);
    for (auto& a : raw)
      ResolveDriverAndMemory(a);

    StartupDiagnosis diagnosis;
    diagnosis.kind = KindFor(site, report.stage);
    diagnosis.detail = detail;
    for (const auto& a : raw)
      diagnosis.adapters.push_back(a.facts);

    const std::wstring dir = EnsureLogDirectory();
    const std::wstring reportPath = dir + L"\\startup-error.txt";
    const std::string reportPathUtf8 = WideToUtf8(reportPath.c_str());

    const Advice advice = ComposeAdvice(diagnosis, reportPathUtf8);
    const std::string technical = TechnicalSection(site, diagnosis.kind, detail, report, raw);

    const DialogMode mode = ReadDialogMode();
    const std::string desktop = ThreadDesktopName();
    const bool interactive = desktop.empty() || _stricmp(desktop.c_str(), "Default") == 0;
    const bool showDialog = mode == DialogMode::Force || (mode == DialogMode::Auto && interactive);

    std::string file = advice.title + "\n\n" + advice.body + "\n\n" + technical;
    file += std::string("dialog=") + (showDialog ? "shown" : "suppressed") + " desktop=" + desktop + "\n";
    const bool written = WriteUtf8File(reportPath, file);

    Logger::err(str::format("WarVK startup failure: kind=", KindName(diagnosis.kind),
      " adapters=", diagnosis.adapters.size(), " report=", written ? reportPathUtf8 : std::string("<write failed>"),
      " dialog=", showDialog ? "shown" : "suppressed"));
    Logger::err(technical);

    if (showDialog) {
      const std::wstring title = Utf8ToWide(advice.title);
      const std::wstring body = Utf8ToWide(advice.body);
      Logger::info("WarVK startup dialog: open");
      const int result = MessageBoxW(nullptr, body.c_str(), title.c_str(),
                                     MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND | MB_TASKMODAL);
      Logger::info(str::format("WarVK startup dialog: closed result=", result));
    }
  } catch (...) {
    // 报错路径自身失败时不能再让游戏崩溃；日志里已经有原始错误。
  }
}

} // namespace dxvk::war3::startup
