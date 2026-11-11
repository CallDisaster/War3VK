#include "war3_module_loader.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if !defined(WARVK_JAPI_PROTOCOL_TEST)
#include "../core/war3_file_manager.h"
#define WARVK_MODULES_HAVE_GAME_FS 1
#else
#define WARVK_MODULES_HAVE_GAME_FS 0
#endif

#include "warvk_module_abi.h"

namespace dxvk::war3::japi::modules {
namespace {

// 协议单测目标不链接游戏文件系统；测试里只走本地文件。
bool ReadViaGameFilesystem(const std::string& path, std::vector<uint8_t>& out) {
#if WARVK_MODULES_HAVE_GAME_FS
  auto& fileManager = FileManager::get();
  return fileManager.isInitialized() && fileManager.readFile(path, out) &&
         !out.empty();
#else
  (void)path;
  (void)out;
  return false;
#endif
}

constexpr size_t kMaxPayloadBytes = 512u;
constexpr size_t kMaxArgs = 16u;
constexpr size_t kMaxModuleBytes = 64u * 1024u * 1024u;
constexpr uint32_t kMaxCommands = 256u;
constexpr size_t kMaxNameBytes = 32u;

struct Arg {
  int type = 0;
  int32_t i = 0;
  float r = 0.0f;
  std::string s;
};

struct LoadedCommand {
  std::string name;
  std::string argTypes;
  int32_t resultKind = WARVK_RESULT_VOID;
  WarVKModuleInvokeV1 invoke = nullptr;
  void* user = nullptr;
};

struct LoadedModule {
  std::string name;
  HMODULE handle = nullptr;
  std::wstring tempPath;
  const WarVKModuleDescV1* desc = nullptr;
  std::vector<LoadedCommand> commands;
};

std::mutex g_mutex;
std::vector<LoadedModule> g_modules;
std::string g_lastError;
uint32_t g_mapEpoch = 1u;
bool g_policyWarned = false;

// ---------------------------------------------------------------- 基础工具

void SetError(std::string text) { g_lastError = std::move(text); }

void HostLog(void*, int32_t level, const char* text) {
  if (!text) return;
  char line[1024];
  std::snprintf(line, sizeof(line), "[WarVK module][%s] %s",
                level >= 2 ? "error" : (level == 1 ? "warn" : "info"), text);
  ::OutputDebugStringA(line);
  ::OutputDebugStringA("\n");
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const unsigned char ca = static_cast<unsigned char>(a[i]);
    const unsigned char cb = static_cast<unsigned char>(b[i]);
    if (std::tolower(ca) != std::tolower(cb)) return false;
  }
  return true;
}

bool IsValidModuleName(std::string_view name) {
  if (name.empty() || name.size() > kMaxNameBytes) return false;
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    const bool ok = (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') ||
                    u == '_' || u == '-';
    if (!ok) return false;
  }
  return true;
}

bool IsArgTypeChar(char c) {
  return c == 'i' || c == 'r' || c == 'd' || c == 'b' || c == 's';
}

// ---------------------------------------------------------------- 协议解析

// "warvk:v1;<command>;<t>:<v>..."
bool ParsePayload(std::string_view payload, std::string& command,
                  std::vector<Arg>& args) {
  if (payload.size() < 8u || payload.size() > kMaxPayloadBytes) return false;
  if (payload.substr(0u, 8u) != "warvk:v1") return false;

  size_t pos = 8u;
  bool first = true;
  while (pos < payload.size()) {
    if (payload[pos] != ';') return false;
    ++pos;
    const size_t end = payload.find(';', pos);
    const std::string_view token =
        payload.substr(pos, end == std::string_view::npos ? std::string_view::npos
                                                         : end - pos);
    if (token.empty()) return false;

    if (first) {
      command.assign(token);
      first = false;
    } else {
      if (args.size() >= kMaxArgs) return false;
      if (token.size() < 3u || token[1] != ':') return false;
      Arg arg;
      const std::string_view value = token.substr(2u);
      switch (token[0]) {
        case 'i':
        case 'd': {
          try {
            arg.type = (token[0] == 'i') ? WARVK_ARG_INT : WARVK_ARG_HANDLE;
            arg.i = static_cast<int32_t>(std::stoll(std::string(value)));
          } catch (...) {
            return false;
          }
          break;
        }
        case 'b':
          arg.type = WARVK_ARG_BOOL;
          arg.i = (value == "1" || value == "true") ? 1 : 0;
          break;
        case 'r': {
          try {
            arg.type = WARVK_ARG_REAL;
            arg.r = std::stof(std::string(value));
          } catch (...) {
            return false;
          }
          break;
        }
        case 's':
          if (value.find(';') != std::string_view::npos) return false;
          arg.type = WARVK_ARG_TEXT;
          arg.s.assign(value);
          break;
        default:
          return false;
      }
      args.push_back(std::move(arg));
    }

    if (end == std::string_view::npos) break;
    pos = end;
  }
  return !first;
}

const char ArgTypeToChar(int type) {
  switch (type) {
    case WARVK_ARG_INT: return 'i';
    case WARVK_ARG_REAL: return 'r';
    case WARVK_ARG_HANDLE: return 'd';
    case WARVK_ARG_BOOL: return 'b';
    case WARVK_ARG_TEXT: return 's';
    default: return '?';
  }
}

// ---------------------------------------------------------------- Reply 构造

Reply OkVoid() {
  Reply reply;
  reply.consumed = true;
  reply.error = ErrorCode::None;
  reply.kind = ResultKind::Void;
  return reply;
}

Reply OkInt(int32_t value) {
  Reply reply = OkVoid();
  reply.kind = ResultKind::Integer;
  reply.integer = value;
  return reply;
}

Reply OkReal(float value) {
  Reply reply = OkVoid();
  reply.kind = ResultKind::Real;
  reply.real = value;
  return reply;
}

Reply OkText(std::string value) {
  Reply reply = OkVoid();
  reply.kind = ResultKind::Text;
  reply.text = std::move(value);
  return reply;
}

Reply Fail(ErrorCode code) {
  Reply reply;
  reply.consumed = true;
  reply.error = code;
  return reply;
}

// ---------------------------------------------------------------- SHA-256（CNG，动态加载避免新增链接依赖）

struct Sha256 {
  using OpenAlg = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, ULONG);
  using GetProp = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, LPCWSTR, PUCHAR, ULONG,
                                    ULONG*, ULONG);
  using CreateHash = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE*,
                                       PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
  using HashData = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, PUCHAR,
                                     ULONG, ULONG);
  using FinishHash = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
  using DestroyHash = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE);
  using CloseAlg = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, ULONG);
};

bool ComputeSha256(const std::vector<uint8_t>& data, std::string& outHex) {
  HMODULE bcrypt = ::LoadLibraryW(L"bcrypt.dll");
  if (!bcrypt) return false;

  auto openAlg = reinterpret_cast<Sha256::OpenAlg>(
      ::GetProcAddress(bcrypt, "BCryptOpenAlgorithmProvider"));
  auto getProp = reinterpret_cast<Sha256::GetProp>(
      ::GetProcAddress(bcrypt, "BCryptGetProperty"));
  auto createHash = reinterpret_cast<Sha256::CreateHash>(
      ::GetProcAddress(bcrypt, "BCryptCreateHash"));
  auto hashData = reinterpret_cast<Sha256::HashData>(
      ::GetProcAddress(bcrypt, "BCryptHashData"));
  auto finishHash = reinterpret_cast<Sha256::FinishHash>(
      ::GetProcAddress(bcrypt, "BCryptFinishHash"));
  auto destroyHash = reinterpret_cast<Sha256::DestroyHash>(
      ::GetProcAddress(bcrypt, "BCryptDestroyHash"));
  auto closeAlg = reinterpret_cast<Sha256::CloseAlg>(
      ::GetProcAddress(bcrypt, "BCryptCloseAlgorithmProvider"));

  bool ok = false;
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::vector<uint8_t> object;
  std::vector<uint8_t> digest(32u);

  if (openAlg && getProp && createHash && hashData && finishHash && destroyHash &&
      closeAlg) {
    ULONG objectSize = 0, produced = 0;
    if (openAlg(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        getProp(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                sizeof(objectSize), &produced, 0) >= 0 &&
        objectSize > 0 && objectSize < 4096u) {
      object.resize(objectSize);
      if (createHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0) {
        const PUCHAR slice = const_cast<PUCHAR>(data.data());
        const ULONG size = static_cast<ULONG>(data.size());
        if (hashData(hash, slice, size, nullptr, 0, 0) >= 0 &&
            finishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >=
                0) {
          char hex[65];
          for (size_t i = 0; i < digest.size(); ++i)
            std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
          hex[64] = '\0';
          outHex.assign(hex, 64);
          ok = true;
        }
      }
    }
  }
  if (hash) destroyHash(hash);
  if (alg) closeAlg(alg, 0);
  ::FreeLibrary(bcrypt);
  return ok;
}

// ---------------------------------------------------------------- 策略

std::wstring GameDirectory() {
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
  if (n == 0) return L".";
  std::wstring full(path, n);
  const size_t slash = full.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : full.substr(0, slash);
}

// 允许清单：<游戏目录>\WarVK\modules\allowed.txt
// 每行 "<模块名> <sha256>"。
//
// 地图可以从任何地方下载，所以没有允许清单时默认拒绝；否则任何地图都能
// 让 WarVK 加载并执行它自带的 DLL。本地开发可设置环境变量
// DXVK_WAR3_MODULE_DEV_MODE=1 放行（地图脚本改不了进程环境变量）。
bool ModuleDevModeRequested() {
  char value[4] = {};
  const DWORD n = ::GetEnvironmentVariableA("DXVK_WAR3_MODULE_DEV_MODE", value, sizeof(value));
  return n == 1 && value[0] == '1';
}

bool PassesPolicy(const std::string& name, const std::vector<uint8_t>& data) {
  const std::wstring path = GameDirectory() + L"\\WarVK\\modules\\allowed.txt";
  std::ifstream in(path.c_str());
  if (!in.is_open()) {
    if (ModuleDevModeRequested()) {
      if (!g_policyWarned) {
        g_policyWarned = true;
        HostLog(nullptr, 1,
                "DXVK_WAR3_MODULE_DEV_MODE=1 且未找到 allowed.txt：模块加载处于开发模式（不做完整性校验）");
      }
      return true;
    }
    SetError("模块加载默认关闭：需要在 WarVK\\modules\\allowed.txt 中写入模块名和 SHA-256: " + name);
    return false;
  }

  std::string digest;
  if (!ComputeSha256(data, digest)) {
    SetError("无法计算模块摘要");
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t space = line.find_first_of(" \t");
    if (space == std::string::npos) continue;
    if (EqualsIgnoreCase(line.substr(0u, space), name) &&
        EqualsIgnoreCase(line.substr(space + 1u), digest))
      return true;
  }
  SetError("模块未在 allowed.txt 中授权: " + name);
  return false;
}

// ---------------------------------------------------------------- 字节解析

bool ReadModuleBytes(const std::string& name, std::vector<uint8_t>& out) {
  const std::string candidates[] = {
      "warvk_modules\\" + name + ".dll",
      "WarVK\\modules\\" + name + ".dll",
      name + ".dll",
  };

  // 1) MPQ（地图内导入）优先，其次本地。
  for (const std::string& path : candidates) {
    if (ReadViaGameFilesystem(path, out)) return true;
  }

  // 2) FileManager 不可用时的本地回退（相对游戏目录 / 绝对路径）。
  for (const std::string& path : candidates) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open()) continue;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > kMaxModuleBytes) continue;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), size);
    if (in) return true;
    out.clear();
  }
  SetError("找不到模块文件: " + name);
  return false;
}

struct PeInfo {
  uint32_t machine = 0;
  uint32_t sizeOfImage = 0;
  uint32_t timestamp = 0;
  uint32_t characteristics = 0;
};

bool ParsePeHeader(const std::vector<uint8_t>& data, PeInfo& info) {
  if (data.size() < 0x40u) return false;
  if (data[0] != 'M' || data[1] != 'Z') return false;

  const uint32_t lfanew = *reinterpret_cast<const uint32_t*>(&data[0x3C]);
  if (lfanew < 0x40u || lfanew + 24u > data.size()) return false;
  if (std::memcmp(&data[lfanew], "PE\0\0", 4) != 0) return false;

  const uint16_t machine = *reinterpret_cast<const uint16_t*>(&data[lfanew + 4u]);
  const uint32_t timestamp =
      *reinterpret_cast<const uint32_t*>(&data[lfanew + 8u]);
  const uint16_t optSize = *reinterpret_cast<const uint16_t*>(&data[lfanew + 20u]);
  const uint16_t characteristics =
      *reinterpret_cast<const uint16_t*>(&data[lfanew + 22u]);

  const uint32_t opt = lfanew + 24u;
  if (opt + 60u > data.size()) return false;
  const uint16_t magic = *reinterpret_cast<const uint16_t*>(&data[opt]);
  if (magic != 0x010Bu && magic != 0x020Bu) return false;  // PE32 / PE32+
  const uint32_t sizeOfImage = *reinterpret_cast<const uint32_t*>(&data[opt + 56u]);

  info.machine = machine;
  info.timestamp = timestamp;
  info.characteristics = characteristics;
  info.sizeOfImage = sizeOfImage;
  (void)optSize;
  return true;
}

// ---------------------------------------------------------------- 模块表操作

LoadedModule* FindLocked(std::string_view name) {
  for (LoadedModule& module : g_modules)
    if (EqualsIgnoreCase(module.name, name)) return &module;
  return nullptr;
}

const LoadedCommand* FindCommandLocked(const LoadedModule& module,
                                       std::string_view command) {
  for (const LoadedCommand& candidate : module.commands)
    if (candidate.name == command) return &candidate;
  return nullptr;
}

void UnloadLocked(size_t index) {
  LoadedModule& module = g_modules[index];
  if (module.desc && module.desc->shutdown)
    module.desc->shutdown(module.desc->moduleUser);
  if (module.handle) ::FreeLibrary(module.handle);
  if (!module.tempPath.empty()) ::DeleteFileW(module.tempPath.c_str());
  g_modules.erase(g_modules.begin() + static_cast<std::ptrdiff_t>(index));
}

bool WriteTempFile(const std::string& name, const std::vector<uint8_t>& data,
                   std::wstring& outPath) {
  wchar_t dir[MAX_PATH * 2] = {};
  const DWORD n = ::GetTempPathW(MAX_PATH * 2, dir);
  if (n == 0 || n >= MAX_PATH * 2) return false;

  std::wstring file(dir);
  if (!file.empty() && file.back() != L'\\' && file.back() != L'/') file += L'\\';
  file += L"wvkmod_";
  file += std::wstring(name.begin(), name.end());
  file += L"_";
  file += std::to_wstring(::GetCurrentProcessId());
  file += L"_";
  file += std::to_wstring(::GetTickCount());
  file += L".dll";

  ::DeleteFileW(file.c_str());
  HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = ::WriteFile(handle, data.data(),
                              static_cast<DWORD>(data.size()), &written, nullptr);
  ::CloseHandle(handle);
  if (!ok || written != data.size()) {
    ::DeleteFileW(file.c_str());
    return false;
  }
  outPath = std::move(file);
  return true;
}

// ---------------------------------------------------------------- 宿主服务

int32_t __cdecl HostReadFile(void*, const char* path, void** outData,
                             uint32_t* outSize) {
  if (!path || !outData || !outSize) return 2;
  std::vector<uint8_t> buffer;
  bool ok = ReadViaGameFilesystem(path, buffer);
  if (!ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return 1;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(kMaxModuleBytes)) return 1;
    in.seekg(0, std::ios::beg);
    buffer.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(buffer.data()), size);
    ok = static_cast<bool>(in);
  }
  if (!ok || buffer.empty()) return 1;

  void* memory = std::malloc(buffer.size());
  if (!memory) return 3;
  std::memcpy(memory, buffer.data(), buffer.size());
  *outData = memory;
  *outSize = static_cast<uint32_t>(buffer.size());
  return 0;
}

void __cdecl HostFreeFile(void*, void* data) { std::free(data); }

int32_t __cdecl HostGetGameInfo(void*, WarVKGameInfoV1* out) {
  if (!out) return 2;
  out->gameVersion = 0x01070000u;
  out->isInGame = 1u;
  out->isReplay = 0u;
  out->mapEpoch = g_mapEpoch;
  return 0;
}

uint32_t __cdecl HostFeatureFlags(void*) { return GetFeatureFlags(); }

WarVKModuleHostV1 MakeHost() {
  WarVKModuleHostV1 host{};
  host.abiVersion = WARVK_MODULE_ABI_VERSION;
  host.structSize = static_cast<uint32_t>(sizeof(WarVKModuleHostV1));
  host.host = nullptr;
  host.log = &HostLog;
  host.readFile = &HostReadFile;
  host.freeFile = &HostFreeFile;
  host.getGameInfo = &HostGetGameInfo;
  host.getHostFeatureFlags = &HostFeatureFlags;
  return host;
}

// 模块允许长期持有 host 指针（见 warvk_module_abi.h 的生命周期约定），
// 因此这里必须是进程级单例，不能传栈上的临时对象。
const WarVKModuleHostV1& HostSingleton() {
  static const WarVKModuleHostV1 host = MakeHost();
  return host;
}

// ---------------------------------------------------------------- 加载 / 卸载

bool LoadLocked(const std::string& name) {
  if (!IsValidModuleName(name)) {
    SetError("模块名非法: " + name);
    return false;
  }
  if (FindLocked(name)) return true;  // 幂等

  std::vector<uint8_t> bytes;
  if (!ReadModuleBytes(name, bytes)) return false;
  if (bytes.size() > kMaxModuleBytes) {
    SetError("模块过大");
    return false;
  }

  PeInfo info;
  if (!ParsePeHeader(bytes, info)) {
    SetError("模块不是合法 PE: " + name);
    return false;
  }
  if (info.machine != 0x014Cu) {  // IMAGE_FILE_MACHINE_I386
    SetError("模块必须是 32 位 x86 DLL");
    return false;
  }
  if ((info.characteristics & 0x2000u) == 0u) {  // IMAGE_FILE_DLL
    SetError("模块不是 DLL");
    return false;
  }
  if (!PassesPolicy(name, bytes)) return false;

  std::wstring tempPath;
  if (!WriteTempFile(name, bytes, tempPath)) {
    SetError("无法写出临时模块文件");
    return false;
  }

  HMODULE handle = ::LoadLibraryExW(tempPath.c_str(), nullptr,
                                    LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!handle) {
    const DWORD error = ::GetLastError();
    ::DeleteFileW(tempPath.c_str());
    SetError("LoadLibrary 失败，错误码 " + std::to_string(error));
    return false;
  }

  auto init = reinterpret_cast<WarVKModuleInitV1Fn>(
      ::GetProcAddress(handle, WARVK_MODULE_INIT_EXPORT));
  if (!init) {
    ::FreeLibrary(handle);
    ::DeleteFileW(tempPath.c_str());
    SetError("模块缺少导出 WarVKModuleInit");
    return false;
  }

  const WarVKModuleHostV1& host = HostSingleton();
  const WarVKModuleDescV1* desc = nullptr;
  const int32_t status = init(&host, &desc);
  if (status != WARVK_MODULE_OK || !desc) {
    ::FreeLibrary(handle);
    ::DeleteFileW(tempPath.c_str());
    SetError("模块初始化被拒绝，状态 " + std::to_string(status));
    return false;
  }
  if (desc->abiVersion != WARVK_MODULE_ABI_VERSION || !desc->name ||
      !EqualsIgnoreCase(desc->name, name) || desc->commandCount > kMaxCommands) {
    ::FreeLibrary(handle);
    ::DeleteFileW(tempPath.c_str());
    SetError("模块描述非法（ABI/名称/命令数）");
    return false;
  }

  LoadedModule module;
  module.name = name;
  module.handle = handle;
  module.tempPath = tempPath;
  module.desc = desc;
  module.commands.reserve(desc->commandCount);

  for (uint32_t i = 0; i < desc->commandCount; ++i) {
    const WarVKModuleCommandV1& command = desc->commands[i];
    if (!command.name || !command.invoke) {
      ::FreeLibrary(handle);
      ::DeleteFileW(tempPath.c_str());
      SetError("命令表存在空项");
      return false;
    }
    const std::string full(command.name);
    const size_t dot = full.find('.');
    if (dot == std::string::npos || dot + 1u >= full.size() ||
        !EqualsIgnoreCase(full.substr(0u, dot), name)) {
      ::FreeLibrary(handle);
      ::DeleteFileW(tempPath.c_str());
      SetError("命令未按 <模块名>.<函数名> 命名: " + full);
      return false;
    }
    if (command.argTypes) {
      const size_t length = std::strlen(command.argTypes);
      if (length > kMaxArgs) {
        ::FreeLibrary(handle);
        ::DeleteFileW(tempPath.c_str());
        SetError("参数类型过长: " + full);
        return false;
      }
      for (size_t k = 0; k < length; ++k)
        if (!IsArgTypeChar(command.argTypes[k])) {
          ::FreeLibrary(handle);
          ::DeleteFileW(tempPath.c_str());
          SetError("参数类型含非法字符: " + full);
          return false;
        }
    }
    if (FindCommandLocked(module, full)) {
      ::FreeLibrary(handle);
      ::DeleteFileW(tempPath.c_str());
      SetError("命令重复: " + full);
      return false;
    }
    LoadedCommand entry;
    entry.name = full;
    entry.argTypes = command.argTypes ? command.argTypes : "";
    entry.resultKind = command.resultKind;
    entry.invoke = command.invoke;
    entry.user = command.user;
    module.commands.push_back(std::move(entry));
  }

  // 跨模块同名命令必须拒绝。
  for (const LoadedModule& other : g_modules) {
    for (const LoadedCommand& candidate : module.commands) {
      if (FindCommandLocked(other, candidate.name)) {
        ::FreeLibrary(handle);
        ::DeleteFileW(tempPath.c_str());
        SetError("命令与其它模块冲突: " + candidate.name);
        return false;
      }
    }
  }

  HostLog(nullptr, 0, ("模块已加载: " + name).c_str());
  g_modules.push_back(std::move(module));
  return true;
}

std::string DescribeModule(const LoadedModule& module) {
  std::string text = module.name;
  text += "|";
  text += module.desc && module.desc->version ? module.desc->version : "";
  text += "|";
  text += std::to_string(module.commands.size());
  return text;
}

std::string ListCommands(const LoadedModule& module) {
  std::string text;
  for (size_t i = 0; i < module.commands.size(); ++i) {
    if (i) text += ",";
    text += module.commands[i].name;
  }
  return text;
}

Reply DispatchControl(std::string_view command, const std::vector<Arg>& args) {
  if (command == "module.load") {
    if (args.size() != 1u || args[0].type != WARVK_ARG_TEXT)
      return Fail(ErrorCode::ArgumentCountMismatch);
    return LoadLocked(args[0].s) ? OkInt(1) : Fail(ErrorCode::BackendRejected);
  }
  if (command == "module.unload") {
    if (args.size() != 1u || args[0].type != WARVK_ARG_TEXT)
      return Fail(ErrorCode::ArgumentCountMismatch);
    if (LoadedModule* module = FindLocked(args[0].s)) {
      const size_t index = static_cast<size_t>(module - g_modules.data());
      UnloadLocked(index);
      return OkInt(1);
    }
    return OkInt(0);
  }
  if (command == "module.count") {
    if (!args.empty()) return Fail(ErrorCode::ArgumentCountMismatch);
    return OkInt(static_cast<int32_t>(g_modules.size()));
  }
  if (command == "module.info") {
    if (args.size() != 1u) return Fail(ErrorCode::ArgumentCountMismatch);
    const int32_t index = args[0].i;
    if (args[0].type == WARVK_ARG_TEXT) {
      const LoadedModule* module = FindLocked(args[0].s);
      return module ? OkText(DescribeModule(*module))
                    : Fail(ErrorCode::InvalidId);
    }
    if (index < 0 || static_cast<size_t>(index) >= g_modules.size())
      return Fail(ErrorCode::InvalidId);
    return OkText(DescribeModule(g_modules[static_cast<size_t>(index)]));
  }
  if (command == "module.commands") {
    if (args.size() != 1u) return Fail(ErrorCode::ArgumentCountMismatch);
    const int32_t index = args[0].i;
    if (index < 0 || static_cast<size_t>(index) >= g_modules.size())
      return Fail(ErrorCode::InvalidId);
    return OkText(ListCommands(g_modules[static_cast<size_t>(index)]));
  }
  if (command == "module.lastError") {
    if (!args.empty()) return Fail(ErrorCode::ArgumentCountMismatch);
    return OkText(g_lastError);
  }
  if (command == "module.reset") {
    while (!g_modules.empty()) UnloadLocked(g_modules.size() - 1u);
    return OkInt(1);
  }
  return Fail(ErrorCode::UnknownCommand);
}

Reply DispatchModuleCommand(Carrier carrier, std::string_view command,
                            const std::vector<Arg>& args) {
  const size_t dot = command.find('.');
  if (dot == std::string_view::npos) return Fail(ErrorCode::UnknownCommand);

  LoadedModule* module = FindLocked(command.substr(0u, dot));
  if (!module) return Fail(ErrorCode::UnknownCommand);

  const LoadedCommand* entry = FindCommandLocked(*module, command);
  if (!entry) return Fail(ErrorCode::UnknownCommand);
  if (args.size() != entry->argTypes.size())
    return Fail(ErrorCode::ArgumentCountMismatch);

  std::vector<WarVKModuleArgV1> native(args.size());
  for (size_t i = 0; i < args.size(); ++i) {
    if (ArgTypeToChar(args[i].type) != entry->argTypes[i])
      return Fail(ErrorCode::InvalidArgumentType);
    native[i].type = args[i].type;
    native[i].i = args[i].i;
    native[i].r = args[i].r;
    native[i].s = (args[i].type == WARVK_ARG_TEXT) ? args[i].s.c_str() : nullptr;
  }

  WarVKModuleValueV1 value{};
  value.kind = WARVK_RESULT_VOID;
  const int32_t status =
      entry->invoke(entry->user, native.data(),
                    static_cast<int32_t>(native.size()), &value);
  if (status != WARVK_MODULE_OK) {
    SetError("模块命令失败: " + std::string(command) + " 状态 " +
             std::to_string(status));
    return Fail(status == WARVK_MODULE_E_ARGUMENT
                    ? ErrorCode::InvalidArgumentType
                    : (status == WARVK_MODULE_E_RANGE ? ErrorCode::RealOutOfRange
                                                      : ErrorCode::BackendRejected));
  }

  switch (value.kind) {
    case WARVK_RESULT_INT:
      return OkInt(value.i);
    case WARVK_RESULT_REAL:
      if (carrier == Carrier::LocalizedString) {
        char text[64];
        std::snprintf(text, sizeof(text), "%.6g", static_cast<double>(value.r));
        return OkText(text);
      }
      return OkReal(value.r);
    case WARVK_RESULT_TEXT:
      return OkText(value.s ? std::string(value.s) : std::string());
    default:
      return OkVoid();
  }
}

}  // namespace

// ---------------------------------------------------------------- 对外接口

bool IsModulePayload(std::string_view payload) noexcept {
  if (payload.size() < 10u || payload.substr(0u, 8u) != "warvk:v1") return false;

  std::lock_guard<std::mutex> lock(g_mutex);
  const size_t commandStart = payload.find(';', 8u);
  if (commandStart == std::string_view::npos || commandStart + 1u >= payload.size())
    return false;
  const std::string_view command = payload.substr(commandStart + 1u);
  if (command.size() >= 7u && command.substr(0u, 7u) == "module.") return true;

  const size_t dot = command.find('.');
  if (dot == std::string_view::npos) return false;
  const std::string_view prefix = command.substr(0u, dot);
  for (const LoadedModule& module : g_modules)
    if (EqualsIgnoreCase(module.name, prefix)) return true;
  return false;
}

Reply Dispatch(Carrier carrier, std::string_view payload) noexcept {
  try {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string command;
    std::vector<Arg> args;
    if (!ParsePayload(payload, command, args)) return {};

    if (command.size() >= 7u && command.compare(0u, 7u, "module.") == 0)
      return DispatchControl(command, args);

    // 不是模块命令就不要消费：调用方仍需把它交给内置命令表。
    const size_t dot = command.find('.');
    if (dot == std::string::npos || !FindLocked(command.substr(0u, dot)))
      return {};

    return DispatchModuleCommand(carrier, command, args);
  } catch (...) {
    return Fail(ErrorCode::InternalError);
  }
}

void ResetAuthorState() noexcept {
  try {
    std::lock_guard<std::mutex> lock(g_mutex);
    while (!g_modules.empty()) UnloadLocked(g_modules.size() - 1u);
    ++g_mapEpoch;
  } catch (...) {
  }
}

uint32_t LoadedCount() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  return static_cast<uint32_t>(g_modules.size());
}

const char* LastErrorText() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_lastError.c_str();
}

}  // namespace dxvk::war3::japi::modules
