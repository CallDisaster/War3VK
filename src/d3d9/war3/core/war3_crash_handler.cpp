#include "../../d3d9_war3_debug.h"

#include <dbghelp.h>
#include <tlhelp32.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace dxvk {
namespace war3dbg {
namespace {

struct ModuleInfo {
  uintptr_t base = 0;
  uintptr_t end = 0;
  std::wstring path;
};

LONG g_handlerInstalled = 0;
LONG g_dumpInProgress = 0;
PVOID g_vectoredHandler = nullptr;

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty())
    return {};
  const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1,
                                           nullptr, 0, nullptr, nullptr);
  if (required <= 1)
    return {};
  std::string result(static_cast<size_t>(required - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), required,
                      nullptr, nullptr);
  return result;
}

std::wstring GetProcessDirectory() {
  std::array<wchar_t, MAX_PATH * 2> path = {};
  const DWORD len = GetModuleFileNameW(nullptr, path.data(),
                                       static_cast<DWORD>(path.size()));
  if (len == 0 || len >= path.size())
    return L".";
  std::wstring result(path.data(), len);
  const size_t slash = result.find_last_of(L"\\/");
  if (slash == std::wstring::npos)
    return L".";
  return result.substr(0, slash);
}

std::wstring GetProcessPath() {
  std::array<wchar_t, MAX_PATH * 2> path = {};
  const DWORD len = GetModuleFileNameW(nullptr, path.data(),
                                       static_cast<DWORD>(path.size()));
  if (len == 0 || len >= path.size())
    return {};
  return std::wstring(path.data(), len);
}

bool EnsureDirectory(const std::wstring& path) {
  if (path.empty())
    return false;
  if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
    return true;

  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos)
    return false;
  if (!EnsureDirectory(path.substr(0, slash)))
    return false;
  return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring MakeCrashDirectory() {
  std::wstring dir = GetProcessDirectory();
  if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
    dir += L"\\";
  dir += L"WarVK\\Crash";
  EnsureDirectory(dir);
  return dir;
}

std::string JsonEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 16);
  for (const char c : text) {
    switch (c) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8] = {};
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out += c;
      }
      break;
    }
  }
  return out;
}

std::string Hex64(uint64_t value) {
  char buf[32] = {};
  std::snprintf(buf, sizeof(buf), "0x%llX",
                static_cast<unsigned long long>(value));
  return buf;
}

std::string TimestampForFile(SYSTEMTIME st) {
  char buf[64] = {};
  std::snprintf(buf, sizeof(buf),
                "%04u_%02u_%02u_%02u_%02u_%02u_%03u",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  return buf;
}

std::string TimestampIso(SYSTEMTIME st) {
  char buf[64] = {};
  std::snprintf(buf, sizeof(buf),
                "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  return buf;
}

void WriteInstallMarker(const std::wstring& crashDir, PVOID vectoredHandler) {
  const std::wstring path = crashDir + L"\\crash_handler_status.json";
  FILE* file = nullptr;
  _wfopen_s(&file, path.c_str(), L"wb");
  if (!file)
    return;

  SYSTEMTIME now = {};
  GetLocalTime(&now);
  std::fprintf(file, "{\n");
  std::fprintf(file, "  \"ok\": true,\n");
  std::fprintf(file, "  \"timestamp\": \"%s\",\n", TimestampIso(now).c_str());
  std::fprintf(file, "  \"pid\": %lu,\n", GetCurrentProcessId());
  std::fprintf(file, "  \"tid\": %lu,\n", GetCurrentThreadId());
  std::fprintf(file, "  \"processPath\": \"%s\",\n",
               JsonEscape(WideToUtf8(GetProcessPath())).c_str());
  std::fprintf(file, "  \"vectoredHandler\": \"%s\",\n",
               Hex64(reinterpret_cast<uintptr_t>(vectoredHandler)).c_str());
  std::fprintf(file, "  \"unhandledFilter\": true,\n");
  std::fprintf(file, "  \"dumpDir\": \"%s\"\n",
               JsonEscape(WideToUtf8(crashDir)).c_str());
  std::fprintf(file, "}\n");
  std::fclose(file);
}

ModuleInfo FindModuleForAddress(uintptr_t address) {
  ModuleInfo result;
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                                   GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE)
    return result;

  MODULEENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snapshot, &entry)) {
    do {
      const uintptr_t base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
      const uintptr_t end = base + static_cast<uintptr_t>(entry.modBaseSize);
      if (address >= base && address < end) {
        result.base = base;
        result.end = end;
        result.path = entry.szExePath;
        break;
      }
    } while (Module32NextW(snapshot, &entry));
  }

  CloseHandle(snapshot);
  return result;
}

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty())
    return {};
  const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1,
                                           nullptr, 0);
  if (required <= 1)
    return {};
  std::wstring result(static_cast<size_t>(required - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), required);
  return result;
}

bool CopyFileBestEffort(const std::wstring& from, const std::wstring& to) {
  return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE;
}

void WriteSummaryJson(const std::wstring& path,
                      const std::wstring& latestPath,
                      EXCEPTION_POINTERS* exceptionPointers,
                      const std::wstring& dumpPath,
                      bool dumpOk,
                      const std::string& dumpError,
                      bool firstChance) {
  const auto* record = exceptionPointers ? exceptionPointers->ExceptionRecord : nullptr;
  const auto* context = exceptionPointers ? exceptionPointers->ContextRecord : nullptr;
  const uintptr_t exceptionAddress = record
    ? reinterpret_cast<uintptr_t>(record->ExceptionAddress)
    : 0;
  const ModuleInfo module = FindModuleForAddress(exceptionAddress);

  FILE* file = nullptr;
  _wfopen_s(&file, path.c_str(), L"wb");
  if (!file)
    return;

  SYSTEMTIME now = {};
  GetLocalTime(&now);

  std::fprintf(file, "{\n");
  std::fprintf(file, "  \"ok\": true,\n");
  std::fprintf(file, "  \"firstChance\": %s,\n", firstChance ? "true" : "false");
  std::fprintf(file, "  \"timestamp\": \"%s\",\n", TimestampIso(now).c_str());
  std::fprintf(file, "  \"pid\": %lu,\n", GetCurrentProcessId());
  std::fprintf(file, "  \"tid\": %lu,\n", GetCurrentThreadId());
  std::fprintf(file, "  \"processPath\": \"%s\",\n",
               JsonEscape(WideToUtf8(GetProcessPath())).c_str());
  std::fprintf(file, "  \"exceptionCode\": \"%s\",\n",
               Hex64(record ? record->ExceptionCode : 0).c_str());
  std::fprintf(file, "  \"exceptionFlags\": %lu,\n",
               record ? record->ExceptionFlags : 0);
  std::fprintf(file, "  \"exceptionAddress\": \"%s\",\n",
               Hex64(exceptionAddress).c_str());
  std::fprintf(file, "  \"exceptionParameters\": [");
  if (record) {
    for (DWORD i = 0; i < record->NumberParameters; ++i) {
      if (i)
        std::fprintf(file, ", ");
      std::fprintf(file, "\"%s\"", Hex64(record->ExceptionInformation[i]).c_str());
    }
  }
  std::fprintf(file, "],\n");
  std::fprintf(file, "  \"crashPoint\": {\n");
  std::fprintf(file, "    \"moduleBase\": \"%s\",\n", Hex64(module.base).c_str());
  std::fprintf(file, "    \"moduleOffset\": \"%s\",\n",
               Hex64(module.base ? exceptionAddress - module.base : 0).c_str());
  std::fprintf(file, "    \"modulePath\": \"%s\"\n",
               JsonEscape(WideToUtf8(module.path)).c_str());
  std::fprintf(file, "  },\n");
  std::fprintf(file, "  \"registers\": {\n");
#if defined(_M_IX86) || defined(__i386__)
  std::fprintf(file, "    \"Eax\": \"%s\",\n", Hex64(context ? context->Eax : 0).c_str());
  std::fprintf(file, "    \"Ebx\": \"%s\",\n", Hex64(context ? context->Ebx : 0).c_str());
  std::fprintf(file, "    \"Ecx\": \"%s\",\n", Hex64(context ? context->Ecx : 0).c_str());
  std::fprintf(file, "    \"Edx\": \"%s\",\n", Hex64(context ? context->Edx : 0).c_str());
  std::fprintf(file, "    \"Esi\": \"%s\",\n", Hex64(context ? context->Esi : 0).c_str());
  std::fprintf(file, "    \"Edi\": \"%s\",\n", Hex64(context ? context->Edi : 0).c_str());
  std::fprintf(file, "    \"Ebp\": \"%s\",\n", Hex64(context ? context->Ebp : 0).c_str());
  std::fprintf(file, "    \"Esp\": \"%s\",\n", Hex64(context ? context->Esp : 0).c_str());
  std::fprintf(file, "    \"Eip\": \"%s\"\n", Hex64(context ? context->Eip : 0).c_str());
#else
  std::fprintf(file, "    \"Rip\": \"%s\",\n", Hex64(context ? context->Rip : 0).c_str());
  std::fprintf(file, "    \"Rsp\": \"%s\"\n", Hex64(context ? context->Rsp : 0).c_str());
#endif
  std::fprintf(file, "  },\n");
  std::fprintf(file, "  \"dumpWriteOk\": %s,\n", dumpOk ? "true" : "false");
  std::fprintf(file, "  \"dumpWriteError\": \"%s\",\n", JsonEscape(dumpError).c_str());
  std::fprintf(file, "  \"dumpPath\": \"%s\",\n", JsonEscape(WideToUtf8(dumpPath)).c_str());
  std::fprintf(file, "  \"summaryPath\": \"%s\"\n", JsonEscape(WideToUtf8(path)).c_str());
  std::fprintf(file, "}\n");
  std::fclose(file);

  CopyFileBestEffort(path, latestPath);
}

LONG HandleCrash(EXCEPTION_POINTERS* exceptionPointers, bool firstChance) {
  if (!exceptionPointers || !exceptionPointers->ExceptionRecord)
    return EXCEPTION_CONTINUE_SEARCH;

  const DWORD code = exceptionPointers->ExceptionRecord->ExceptionCode;
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
  case EXCEPTION_DATATYPE_MISALIGNMENT:
  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_IN_PAGE_ERROR:
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_STACK_OVERFLOW:
    break;
  default:
    return EXCEPTION_CONTINUE_SEARCH;
  }

  if (InterlockedCompareExchange(&g_dumpInProgress, 1, 0) != 0)
    return EXCEPTION_CONTINUE_SEARCH;

  SYSTEMTIME now = {};
  GetLocalTime(&now);

  const std::wstring crashDir = MakeCrashDirectory();
  const std::string stem = "war3_crash_" + TimestampForFile(now) +
    "_pid" + std::to_string(GetCurrentProcessId()) +
    "_tid" + std::to_string(GetCurrentThreadId());
  const std::wstring wideStem = Utf8ToWide(stem);
  const std::wstring dumpPath = crashDir + L"\\" + wideStem + L".dmp";
  const std::wstring summaryPath = crashDir + L"\\" + wideStem + L".json";
  const std::wstring latestPath = crashDir + L"\\latest_crash.json";

  bool dumpOk = false;
  std::string dumpError;
  const HANDLE dumpFile = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (dumpFile == INVALID_HANDLE_VALUE) {
    dumpError = "CreateFileW failed: " + std::to_string(GetLastError());
  } else {
    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo = {};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = exceptionPointers;
    exceptionInfo.ClientPointers = FALSE;

    const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
      MiniDumpNormal |
      MiniDumpWithDataSegs |
      MiniDumpWithHandleData |
      MiniDumpWithIndirectlyReferencedMemory |
      MiniDumpScanMemory |
      MiniDumpWithThreadInfo);

    dumpOk = MiniDumpWriteDump(GetCurrentProcess(),
                               GetCurrentProcessId(),
                               dumpFile,
                               dumpType,
                               &exceptionInfo,
                               nullptr,
                               nullptr) != FALSE;
    if (!dumpOk)
      dumpError = "MiniDumpWriteDump failed: " + std::to_string(GetLastError());
    CloseHandle(dumpFile);
  }

  WriteSummaryJson(summaryPath, latestPath, exceptionPointers, dumpPath,
                   dumpOk, dumpError, firstChance);

  Print("DXVK War3Crash: captured exception code=0x%08lX addr=%p dump=%ls ok=%d firstChance=%d\n",
        code,
        exceptionPointers->ExceptionRecord->ExceptionAddress,
        dumpPath.c_str(),
        dumpOk ? 1 : 0,
        firstChance ? 1 : 0);

  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI War3VectoredExceptionHandler(EXCEPTION_POINTERS* exceptionPointers) {
  return HandleCrash(exceptionPointers, true);
}

LONG WINAPI War3UnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionPointers) {
  HandleCrash(exceptionPointers, false);
  return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void InstallCrashHandlerOnce() {
  if (InterlockedCompareExchange(&g_handlerInstalled, 1, 0) != 0)
    return;

  const std::wstring crashDir = MakeCrashDirectory();
  SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
  g_vectoredHandler = AddVectoredExceptionHandler(1, War3VectoredExceptionHandler);
  SetUnhandledExceptionFilter(War3UnhandledExceptionFilter);
  WriteInstallMarker(crashDir, g_vectoredHandler);

  Print("DXVK War3Crash: handler installed veh=%p dir=%ls\n",
        g_vectoredHandler,
        crashDir.c_str());

  char selfTest[32] = {};
  if (GetEnvironmentVariableA("DXVK_WAR3_CRASH_HANDLER_SELFTEST",
                              selfTest,
                              static_cast<DWORD>(sizeof(selfTest))) > 0 &&
      std::strcmp(selfTest, "force") == 0) {
    Print("DXVK War3Crash: forcing self-test access violation\n");
    volatile int* crash = reinterpret_cast<volatile int*>(0x925BEE02u);
    (void)*crash;
  }
}

} // namespace war3dbg
} // namespace dxvk
