#include "war3_vram_preset.h"

#include "../../../dxvk/dxvk_adapter.h"
#include "../../../util/log/log.h"
#include "../../../util/util_env.h"
#include "../../../util/util_string.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>

namespace dxvk::war3::platform {

namespace {

dxvk::mutex g_presetMutex;
bool g_presetDone = false;
VramPreset g_preset;
std::atomic<const VramPreset*> g_published{nullptr};

std::wstring GameDirectory() {
  std::array<wchar_t, MAX_PATH * 2> path = {};
  const DWORD len = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  if (len == 0 || len >= path.size())
    return L".";
  std::wstring result(path.data(), len);
  const size_t slash = result.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring(L".") : result.substr(0, slash);
}

std::string ReadIniPreset() {
  const std::wstring ini = GameDirectory() + L"\\WarVK\\warvk.ini";
  if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
    return {};
  std::array<wchar_t, 32> value = {};
  GetPrivateProfileStringW(L"Graphics", L"VramPreset", L"", value.data(), DWORD(value.size()), ini.c_str());
  // 转成 UTF-8，这样 ini 里写“低/中/高”也能识别（ANSI 或 UTF-16 保存都行）。
  char utf8[96] = {};
  if (!WideCharToMultiByte(CP_UTF8, 0, value.data(), -1, utf8, int(sizeof(utf8)), nullptr, nullptr))
    return {};
  return utf8;
}

void WriteGpuSummary(const std::string& text) {
  std::wstring dir = GameDirectory() + L"\\WarVK";
  CreateDirectoryW(dir.c_str(), nullptr);
  dir += L"\\Log";
  CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring path = dir + L"\\gpu.txt";
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  DWORD written = 0;
  WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr);
  CloseHandle(file);
}

void Compute(const DxvkAdapter& adapter) {
  const auto& props = adapter.deviceProperties().core.properties;
  g_preset.adapterName = props.deviceName;
  g_preset.integrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

  const DxvkAdapterMemoryInfo memory = adapter.getMemoryHeapInfo();
  for (uint32_t i = 0; i < memory.heapCount; ++i) {
    const auto& heap = memory.heaps[i];
    if (!(heap.heapFlags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
      continue;
    if (heap.heapSize >= g_preset.deviceLocalBytes) {
      g_preset.deviceLocalBytes = heap.heapSize;
      g_preset.budgetBytes = heap.memoryBudget;
    }
  }

  gpureq::VramTier tier = gpureq::ClassifyVram(g_preset.deviceLocalBytes, g_preset.integrated);
  g_preset.source = "auto";

  const std::string envValue = env::getEnvVar("DXVK_WAR3_VRAM_PRESET");
  const std::string iniValue = envValue.empty() ? ReadIniPreset() : std::string();
  gpureq::VramTier overrideTier = tier;
  if (!envValue.empty() && gpureq::ParseVramTier(envValue, overrideTier)) {
    tier = overrideTier;
    g_preset.source = "env";
  } else if (!iniValue.empty() && gpureq::ParseVramTier(iniValue, overrideTier)) {
    tier = overrideTier;
    g_preset.source = "ini";
  }

  g_preset.tier = tier;
  g_preset.csmResolutionCap = gpureq::CsmResolutionCapFor(tier);

  char line[512] = {};
  std::snprintf(line, sizeof(line),
    "WarVK GPU: %s vendor=0x%04X device=0x%04X integrated=%d deviceLocalMiB=%llu budgetMiB=%llu "
    "tier=%s source=%s csmResolutionCap=%u\r\n",
    g_preset.adapterName.c_str(), unsigned(props.vendorID), unsigned(props.deviceID),
    g_preset.integrated ? 1 : 0,
    static_cast<unsigned long long>(g_preset.deviceLocalBytes >> 20),
    static_cast<unsigned long long>(g_preset.budgetBytes >> 20),
    gpureq::VramTierName(tier), g_preset.source.c_str(), g_preset.csmResolutionCap);
  Logger::info(line);
  WriteGpuSummary(line);
}

} // namespace

void InitVramPreset(const DxvkAdapter& adapter) {
  std::lock_guard lock(g_presetMutex);
  if (g_presetDone)
    return;
  g_presetDone = true;
  try {
    Compute(adapter);
  } catch (...) {
    g_preset = VramPreset();
    g_preset.source = "error";
  }
  g_published.store(&g_preset, std::memory_order_release);
}

const VramPreset* CurrentVramPreset() {
  return g_published.load(std::memory_order_acquire);
}

} // namespace dxvk::war3::platform
