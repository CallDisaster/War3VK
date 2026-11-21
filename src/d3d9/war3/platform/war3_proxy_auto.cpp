// war3_proxy_auto.cpp - see war3_proxy_auto.h

#include "war3_proxy_auto.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace dxvk::war3::proxy::autostart {

namespace {

const wchar_t kState[] = L"WARVK_D3D9_AUTO_STATE";
const wchar_t kLogDirectory[] = L"WARVK_D3D9_AUTO_LOG";
const wchar_t kFront[] = L"WARVK_D3D9_FRONT";
const wchar_t kConfigFile[] = L"DXVK_CONFIG_FILE";
const wchar_t kShaderCache[] = L"DXVK_SHADER_CACHE_PATH";
constexpr size_t kKeptRuns = 20;

// The transport the frozen gates call product-async-delta
// (AutoTest/run_x64_game_proxy_probe.py); a value already set wins.
const wchar_t* const kSettings[][2] = {
  {L"WARVK_D3D9_GAME", L"author-trial-v1"},
  {L"WARVK_D3D9_PROFILE", L"presenter-isolated-v1"},
  {L"WARVK_D3D9_ANNOTATIONS", L"1"},
  {L"WARVK_D3D9_BATCH_DRAWS", L"1"},
  {L"WARVK_D3D9_ASYNC_BATCH", L"1"},
  {L"WARVK_D3D9_ASYNC_EVENTS", L"1"},
  {L"WARVK_D3D9_STREAM", L"1"},
  {L"WARVK_D3D9_FIXED_BATCH", L"1"},
  {L"WARVK_D3D9_COMMAND_RING", L"1"},
  {L"WARVK_D3D9_X86_BACKING_KIB", L"32768"},
  {L"WARVK_D3D9_UPLOAD_WINDOW_KIB", L"1024"},
  {L"WARVK_D3D9_ASYNC_UPLOADS", L"1"},
  {L"WARVK_D3D9_ASYNC_FLUSH", L"1"},
  {L"WARVK_D3D9_ASYNC_SCENE", L"1"},
  {L"WARVK_D3D9_PRESENT_AHEAD", L"1"},
  {L"WARVK_D3D9_FIXED_DELTA", L"1"},
  // Frame evidence (the host's frame ring and per-frame tear audit, about 0.4 ms
  // a frame and up to 1 GiB of video memory) is off for players; the player
  // package's WarVK_EvidenceStart.cmd sets it to 1 before starting the game.
  {L"WARVK_D3D9_EVIDENCE", L"0"},
};

std::atomic<bool> g_fellBack{false};

std::wstring Env(const wchar_t* name) {
  const DWORD size = ::GetEnvironmentVariableW(name, nullptr, 0);
  if (size == 0)
    return {};
  std::wstring value(size, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(name, value.data(), size);
  value.resize(written < size ? written : 0);
  return value;
}

void SetIfUnset(const wchar_t* name, const std::wstring& value) {
  if (::GetEnvironmentVariableW(name, nullptr, 0) == 0)
    ::SetEnvironmentVariableW(name, value.c_str());
}

std::wstring ModulePath(HMODULE module) {
  std::wstring path(32768, L'\0');
  const DWORD size = ::GetModuleFileNameW(module, path.data(), DWORD(path.size()));
  path.resize(size < path.size() ? size : 0);
  return path;
}

std::wstring DirectoryOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool IsFile(const std::wstring& path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool MakeDirectory(const std::wstring& path) {
  return ::CreateDirectoryW(path.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS;
}

// <root>\WarVK\logs\<run>, empty when the root is not writable.
std::wstring MakeRunDirectory(const std::wstring& root, const std::wstring& run) {
  const std::wstring warvk = root + L"\\WarVK", logs = warvk + L"\\logs", directory = logs + L"\\" + run;
  if (root.empty() || !MakeDirectory(warvk) || !MakeDirectory(logs) || !MakeDirectory(directory))
    return {};
  return directory;
}

std::wstring RunName() {
  SYSTEMTIME now{};
  ::GetLocalTime(&now);
  wchar_t name[64]{};
  std::swprintf(name, 64, L"%04u%02u%02u-%02u%02u%02u-%lu", now.wYear, now.wMonth, now.wDay, now.wHour,
                now.wMinute, now.wSecond, ::GetCurrentProcessId());
  return name;
}

bool IsRunName(const std::wstring& name) {
  if (name.size() < 17 || name[8] != L'-' || name[15] != L'-')
    return false;
  for (size_t i = 0; i < name.size(); ++i)
    if (i != 8 && i != 15 && !std::iswdigit(name[i]))
      return false;
  return true;
}

template <class Visit> void ForEachEntry(const std::wstring& directory, Visit visit) {
  WIN32_FIND_DATAW entry{};
  const HANDLE find = ::FindFirstFileW((directory + L"\\*").c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE)
    return;
  do {
    visit(entry);
  } while (::FindNextFileW(find, &entry));
  ::FindClose(find);
}

// Only directories named like a run, only the files directly inside them.
void TrimRunLogs(const std::wstring& current) {
  const std::wstring logs = DirectoryOf(current);
  std::vector<std::wstring> runs;
  ForEachEntry(logs, [&](const WIN32_FIND_DATAW& entry) {
    if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsRunName(entry.cFileName))
      runs.emplace_back(entry.cFileName);
  });
  if (runs.size() <= kKeptRuns)
    return;
  std::sort(runs.begin(), runs.end());
  runs.resize(runs.size() - kKeptRuns);
  for (const auto& run : runs) {
    const std::wstring directory = logs + L"\\" + run;
    if (directory == current)
      continue;
    ForEachEntry(directory, [&](const WIN32_FIND_DATAW& entry) {
      if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        ::DeleteFileW((directory + L"\\" + entry.cFileName).c_str());
    });
    ::RemoveDirectoryW(directory.c_str());
  }
}

Mode Discover() {
  const std::wstring state = Env(kState);
  if (state == L"fallback")
    return Mode::Off;
  if (state == L"active")
    return Mode::Automatic;
  if (!Env(kFront).empty())
    return Mode::Explicit;
  if (Env(L"WARVK_D3D9_AUTO") == L"0")
    return Mode::Off;

  // Only the game itself: the World Editor loads this DLL from the same directory.
  const std::wstring executable = ModulePath(nullptr);
  const size_t slash = executable.find_last_of(L"\\/");
  if (slash == std::wstring::npos || ::_wcsicmp(executable.c_str() + slash + 1, L"war3.exe") != 0)
    return Mode::Off;
  BOOL wow64 = FALSE;
  if (!::IsWow64Process(::GetCurrentProcess(), &wow64) || !wow64)
    return Mode::Off;

  HMODULE self = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&Discover), &self))
    return Mode::Off;
  const std::wstring root = DirectoryOf(ModulePath(self));
  const std::wstring kit = root + L"\\WarVK\\x64";
  const std::wstring front = kit + L"\\warvk-d3d9-front.dll", host = kit + L"\\warvk-d3d9-host.exe";
  if (root.empty() || !IsFile(front) || !IsFile(host))
    return Mode::Off;

  // A game under Program Files cannot write beside itself; the host needs a log.
  const std::wstring run = RunName();
  std::wstring logs = MakeRunDirectory(root, run);
  if (logs.empty()) {
    wchar_t temporary[MAX_PATH + 1]{};
    const DWORD size = ::GetTempPathW(MAX_PATH + 1, temporary);
    std::wstring base(temporary, size <= MAX_PATH ? size : 0);
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/'))
      base.pop_back();
    logs = MakeRunDirectory(base, run);
  }
  if (logs.empty())
    return Mode::Off;

  for (const auto& setting : kSettings)
    SetIfUnset(setting[0], setting[1]);
  ::SetEnvironmentVariableW(kFront, front.c_str());
  ::SetEnvironmentVariableW(L"WARVK_D3D9_HOST_EXE", host.c_str());
  ::SetEnvironmentVariableW(L"WARVK_D3D9_HOST_LOG", (logs + L"\\host.log").c_str());
  ::SetEnvironmentVariableW(kLogDirectory, logs.c_str());
  SetIfUnset(L"DXVK_LOG_PATH", logs);
  if (IsFile(kit + L"\\dxvk.conf"))
    SetIfUnset(kConfigFile, kit + L"\\dxvk.conf");
  const std::wstring cache = root + L"\\WarVK\\cache";
  if (MakeDirectory(cache))
    SetIfUnset(kShaderCache, cache);
  ::SetEnvironmentVariableW(kState, L"active");
  return Mode::Automatic;
}

Mode Decided() noexcept {
  static const Mode mode = []() noexcept {
    try {
      return Discover();
    } catch (...) {
      return Mode::Off;
    }
  }();
  return mode;
}

void ClearIfInside(const wchar_t* name, const std::wstring& directory) {
  const std::wstring value = Env(name);
  if (!directory.empty() && value.rfind(directory + L"\\", 0) == 0)
    ::SetEnvironmentVariableW(name, nullptr);
}

}  // namespace

Mode CurrentMode() noexcept {
  const Mode mode = Decided();
  return mode == Mode::Automatic && g_fellBack.load(std::memory_order_acquire) ? Mode::Off : mode;
}

void PrepareFirstFactory() noexcept {
  static std::atomic<bool> prepared{false};
  if (prepared.exchange(true, std::memory_order_acq_rel))
    return;
  try {
    const std::wstring logs = Env(kLogDirectory);
    if (logs.empty())
      return;
    // A game started from Explorer has no stderr. The C runtime is shared with
    // the front end, so its "D3D9 ..." diagnostics follow this stream.
    if (::_fileno(stderr) < 0 && ::_wfreopen((logs + L"\\game.stderr").c_str(), L"a", stderr) != nullptr)
      std::setvbuf(stderr, nullptr, _IONBF, 0);
    TrimRunLogs(logs);
  } catch (...) {
  }
}

void FallBack(const char* reason) noexcept {
  g_fellBack.store(true, std::memory_order_release);
  ::SetEnvironmentVariableW(kState, L"fallback");
  ::SetEnvironmentVariableW(kFront, nullptr);
  try {
    HMODULE self = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&FallBack), &self)) {
      const std::wstring warvk = DirectoryOf(ModulePath(self)) + L"\\WarVK";
      ClearIfInside(kConfigFile, warvk);
      ClearIfInside(kShaderCache, warvk);
    }
    const std::wstring logs = Env(kLogDirectory);
    if (!logs.empty()) {
      if (FILE* file = ::_wfopen((logs + L"\\fallback.txt").c_str(), L"a")) {
        std::fprintf(file, "proxy mode did not start, running in process: %s\n", reason ? reason : "unknown");
        std::fclose(file);
      }
    }
  } catch (...) {
  }
}

}  // namespace dxvk::war3::proxy::autostart
