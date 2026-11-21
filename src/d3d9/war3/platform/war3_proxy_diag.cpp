// war3_proxy_diag.cpp - see war3_proxy_diag.h

#include "war3_proxy_diag.h"

#include "war3_player_settings.h"
#include "war3_proxy_overlay.h"
#include "../war3.h"
#include "../../d3d9_war3_settings.h"
#include "../../../util/log/log.h"
#include "../../../util/util_string.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace dxvk::war3::proxy {

namespace {

namespace d = warvk::diag;

d::Block* g_block = nullptr;
int32_t g_lastEvidenceState = 0;
uint32_t g_testCountdown = 0;

std::wstring GameDirectory() {
  wchar_t path[MAX_PATH + 1] = {};
  const DWORD size = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (size == 0 || size >= MAX_PATH)
    return {};
  std::wstring text(path, size);
  const size_t slash = text.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : text.substr(0, slash);
}

bool Writable(const std::wstring& directory) {
  if (directory.empty())
    return false;
  const std::wstring probe = directory + L"\\.warvk-write-test";
  const HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  ::CloseHandle(file);
  return true;
}

// <game>\WarVK\Evidence: kept across sessions and easy to find. A game folder
// that cannot be written falls back to the session's log folder.
std::wstring ChooseOutputDirectory() {
  const std::wstring game = GameDirectory();
  if (!game.empty()) {
    const std::wstring warvk = game + L"\\WarVK", evidence = warvk + L"\\Evidence";
    ::CreateDirectoryW(warvk.c_str(), nullptr);
    ::CreateDirectoryW(evidence.c_str(), nullptr);
    if (Writable(evidence))
      return evidence;
  }
  wchar_t logs[MAX_PATH + 1] = {};
  const DWORD size = ::GetEnvironmentVariableW(L"WARVK_D3D9_AUTO_LOG", logs, MAX_PATH);
  if (size != 0 && size < MAX_PATH && Writable(logs))
    return logs;
  wchar_t temporary[MAX_PATH + 1] = {};
  const DWORD length = ::GetTempPathW(MAX_PATH, temporary);
  if (length == 0 || length >= MAX_PATH)
    return {};
  std::wstring text(temporary, length);
  while (!text.empty() && (text.back() == L'\\' || text.back() == L'/'))
    text.pop_back();
  return text;
}

std::string Utf8(const wchar_t* text, size_t capacity) {
  size_t length = 0;
  while (length < capacity && text[length] != L'\0')
    ++length;
  if (length == 0)
    return {};
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, int(length), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(size > 0 ? size : 0), '\0');
  if (size > 0)
    ::WideCharToMultiByte(CP_UTF8, 0, text, int(length), out.data(), size, nullptr, nullptr);
  return out;
}

// Sandbox only: `key=value` lines with the names of WarVK\settings.ini,
// applied as the panel's edits are (one run can walk through a day).
void ApplyTestSettings(const std::wstring& path) {
  const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  char text[4096] = {};
  DWORD size = 0;
  const BOOL read = ::ReadFile(file, text, sizeof(text) - 1, &size, nullptr);
  ::CloseHandle(file);
  ::DeleteFileW(path.c_str());
  if (!read)
    return;
  player::Overrides overrides;
  std::string line;
  for (DWORD i = 0; i <= size; ++i) {
    const char c = i < size ? text[i] : '\n';
    if (c != '\n' && c != '\r') {
      line.push_back(c);
      continue;
    }
    const size_t equals = line.find('=');
    if (equals != std::string::npos && equals > 0)
      overrides[line.substr(0, equals)] = line.substr(equals + 1);
    line.clear();
  }
  if (overrides.empty())
    return;
  if (auto settings = GetMutableSettings()) {
    player::Extras extras;
    player::ApplyOverrides(overrides, *settings, extras);
  }
}

// Sandbox only (WARVK_D3D9_EVIDENCE_TEST=1): the isolated desktop has no
// keyboard, so an empty file in WarVK\Temp stands for the key or the button.
void PollTestRequests() noexcept {
  static const bool enabled = [] {
    wchar_t value[4] = {};
    return ::GetEnvironmentVariableW(L"WARVK_D3D9_EVIDENCE_TEST", value, 4) == 1 && value[0] == L'1';
  }();
  if (!enabled)
    return;
  if (g_testCountdown != 0u) {
    --g_testCountdown;
    return;
  }
  g_testCountdown = 15u;
  try {
    const std::wstring temp = GameDirectory() + L"\\WarVK\\Temp\\";
    if (::DeleteFileW((temp + L"evidence_request.txt").c_str()))
      DiagRequestEvidence();
    if (::DeleteFileW((temp + L"perf_start.txt").c_str()))
      DiagSetPerfRecording(true);
    if (::DeleteFileW((temp + L"perf_stop.txt").c_str()))
      DiagSetPerfRecording(false);
    if (::DeleteFileW((temp + L"panel_open.txt").c_str()))
      RoutePanelRequest(true);
    // The sun dial's edit, as the panel makes it: the time locked at noon.
    if (::DeleteFileW((temp + L"sun_noon.txt").c_str())) {
      if (auto settings = GetMutableSettings()) {
        settings->shadows.lockSun = true;
        settings->shadows.lockSunTime = 0.5f;
      }
    }
    ApplyTestSettings(temp + L"settings_request.txt");
  } catch (...) {
  }
}

// The player has no other sign that a package was kept: the console may be
// closed, and the game's window title is left alone.
void AnnounceEvidence() noexcept {
  const int32_t state = g_block->evidenceState;
  if (state == g_lastEvidenceState)
    return;
  g_lastEvidenceState = state;
  try {
    if (state == d::EvidenceSaved) {
      ::MessageBeep(MB_OK);
      Logger::info(str::format("WarVK frame evidence: saved ", Utf8(g_block->evidencePath, d::PathChars)));
    } else if (state == d::EvidenceFailed) {
      ::MessageBeep(MB_ICONHAND);
      Logger::warn(str::format("WarVK frame evidence: failed: ", g_block->evidenceError));
    }
  } catch (...) {
  }
}

} // namespace

void DiagConnect() noexcept {
  if (g_block != nullptr)
    return;
  try {
    wchar_t name[64];
    const DWORD pid = ::GetCurrentProcessId();
    d::BlockName(name, 64, pid);
    const HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                DWORD(sizeof(d::Block)), name);
    if (mapping == nullptr) {
      Logger::warn(str::format("WarVK proxy diagnostics: no block, error ", ::GetLastError()));
      return;
    }
    auto* block = static_cast<d::Block*>(::MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(d::Block)));
    if (block == nullptr) {
      Logger::warn(str::format("WarVK proxy diagnostics: no view, error ", ::GetLastError()));
      ::CloseHandle(mapping);
      return;
    }
    // The mapping stays for the life of the process.
    const std::wstring output = ChooseOutputDirectory();
    const size_t chars = std::min<size_t>(output.size(), d::PathChars - 1u);
    std::memcpy(block->outputDirectory, output.data(), chars * sizeof(wchar_t));
    block->outputDirectory[chars] = L'\0';
    block->version = d::Version;
    block->bytes = uint32_t(sizeof(d::Block));
    block->gamePid = pid;
    ::MemoryBarrier();
    block->magic = d::Magic;
    g_block = block;
    Logger::info(str::format("WarVK proxy diagnostics: block ready, output ", Utf8(block->outputDirectory, d::PathChars)));
  } catch (...) {
  }
}

bool DiagRequestEvidence() noexcept {
  if (g_block == nullptr)
    return false;
  ::InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_block->evidenceRequest));
  ::MessageBeep(MB_ICONASTERISK);
  return g_block->hostPid != 0;
}

void DiagSetPerfRecording(bool recording) noexcept {
  if (g_block == nullptr)
    return;
  if (recording) {
    ::InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_block->perfSession));
    ::MemoryBarrier();
  }
  g_block->perfWanted = recording ? 1 : 0;
}

DiagStatus DiagQuery() {
  DiagStatus status;
  d::Block* block = g_block;
  if (block == nullptr)
    return status;
  status.available = true;
  status.hostPid = block->hostPid;
  status.hostAttached = status.hostPid != 0;
  status.perfWanted = block->perfWanted != 0;
  status.evidenceState = block->evidenceState;
  status.evidenceFrames = block->evidenceFrames;
  status.evidenceMillis = block->evidenceMillis;
  status.evidenceSaved = block->evidenceSaved;
  status.evidenceTotal = block->evidenceTotal;
  status.evidencePackages = block->evidencePackages;
  status.perfState = block->perfState;
  status.perfRows = block->perfRows;
  status.outputDirectory = Utf8(block->outputDirectory, d::PathChars);
  status.evidencePath = Utf8(block->evidencePath, d::PathChars);
  status.perfPath = Utf8(block->perfPath, d::PathChars);
  block->evidenceError[sizeof(block->evidenceError) - 1u] = '\0';
  status.error = block->evidenceError;
  for (int attempt = 0; attempt < 4 && !status.statsValid; ++attempt) {
    const uint32_t before = block->stats.sequence;
    if (before & 1u) {
      ::SwitchToThread();
      continue;
    }
    ::MemoryBarrier();
    std::memcpy(const_cast<void*>(static_cast<const volatile void*>(&status.stats)),
                const_cast<const void*>(static_cast<const volatile void*>(&block->stats)), sizeof(d::HostStats));
    ::MemoryBarrier();
    status.statsValid = before != 0u && block->stats.sequence == before;
  }
  return status;
}

void DiagGameFrame(const d::GameFrame& frame) noexcept {
  d::Block* block = g_block;
  if (block == nullptr)
    return;
  const int32_t head = block->gameFrameHead;
  block->frames[uint32_t(head) % d::GameFrames] = frame;
  ::MemoryBarrier();
  block->gameFrameHead = head + 1;
  AnnounceEvidence();
  PollTestRequests();
}

AddressSpaceSample QueryAddressSpace() noexcept {
  AddressSpaceSample out;
  SYSTEM_INFO system = {};
  ::GetSystemInfo(&system);
  const uint64_t first = uint64_t(reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress));
  const uint64_t end = uint64_t(reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress)) + 1u;
  constexpr uint64_t LowEnd = 0x80000000ull;
  if (first == 0u || end <= first)
    return out;
  out.largeAddress = end > LowEnd;
  out.lowTotal = std::min(end, LowEnd) - first;
  out.highTotal = end > LowEnd ? end - LowEnd : 0u;
  uint64_t lowFree = 0, highFree = 0, at = first;
  while (at < end) {
    MEMORY_BASIC_INFORMATION region = {};
    if (::VirtualQuery(reinterpret_cast<const void*>(uintptr_t(at)), &region, sizeof(region)) == 0)
      break;
    const uint64_t base = uint64_t(reinterpret_cast<uintptr_t>(region.BaseAddress));
    uint64_t next = base + region.RegionSize;
    if (next <= at)
      break;
    next = std::min(next, end);
    if (region.State == MEM_FREE) {
      if (at < LowEnd) {
        const uint64_t size = std::min(next, LowEnd) - at;
        lowFree += size;
        out.lowLargestFree = std::max(out.lowLargestFree, size);
      }
      if (next > LowEnd) {
        const uint64_t size = next - std::max(at, LowEnd);
        highFree += size;
        out.highLargestFree = std::max(out.highLargestFree, size);
      }
    }
    at = next;
  }
  out.lowUsed = out.lowTotal - std::min(out.lowTotal, lowFree);
  out.highUsed = out.highTotal - std::min(out.highTotal, highFree);
  // PROCESS_MEMORY_COUNTERS_EX without psapi: kernel32's K32 entry.
  struct Counters {
    DWORD cb; DWORD pageFaultCount; SIZE_T peakWorkingSetSize; SIZE_T workingSetSize;
    SIZE_T quotaPeakPagedPoolUsage; SIZE_T quotaPagedPoolUsage; SIZE_T quotaPeakNonPagedPoolUsage;
    SIZE_T quotaNonPagedPoolUsage; SIZE_T pagefileUsage; SIZE_T peakPagefileUsage; SIZE_T privateUsage;
  };
  using Query = BOOL (WINAPI*)(HANDLE, Counters*, DWORD);
  static const Query query = reinterpret_cast<Query>(reinterpret_cast<void*>(
      ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo")));
  Counters counters = {};
  counters.cb = sizeof(counters);
  if (query != nullptr && query(::GetCurrentProcess(), &counters, sizeof(counters)))
    out.privateBytes = counters.privateUsage;
  out.valid = true;
  return out;
}

} // namespace dxvk::war3::proxy
