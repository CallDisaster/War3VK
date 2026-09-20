#include "win32_security.h"
#include <sddl.h>
#include <vector>

namespace warvk::host::lab {
namespace {
[[noreturn]] void Fail() { throw Failure(Fault::System, Stage::Setup, GetLastError()); }
struct LocalMemory {
  void* value = nullptr;
  ~LocalMemory() { if (value) LocalFree(value); }
};
}
LogonSecurity::LogonSecurity(SecurityObject object) {
  const wchar_t* rights = nullptr;
  switch (object) {
    case SecurityObject::Pipe: rights = L"0x0012019B"; break; // excludes create-instance
    case SecurityObject::Mapping: rights = L"0x00020006"; break; // read/write + READ_CONTROL
    case SecurityObject::Mutex: rights = L"0x00120001"; break; // synchronize/modify + READ_CONTROL
  }
  if (!rights)
    throw Failure(Fault::System, Stage::Setup, ERROR_INVALID_PARAMETER);
  HANDLE raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
    Fail();
  Handle token(raw);
  DWORD size = 0;
  GetTokenInformation(token.get(), TokenGroups, nullptr, 0, &size);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size || size > 65536)
    Fail();
  std::vector<uint8_t> groups(size);
  if (!GetTokenInformation(token.get(), TokenGroups, groups.data(), size, &size))
    Fail();
  auto* info = reinterpret_cast<TOKEN_GROUPS*>(groups.data());
  PSID logon = nullptr;
  for (DWORD i = 0; i < info->GroupCount; ++i)
    if ((info->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
      logon = info->Groups[i].Sid;
  if (!logon)
    throw Failure(Fault::System, Stage::Setup, ERROR_NO_SUCH_LOGON_SESSION);
  LocalMemory sid, descriptor;
  if (!ConvertSidToStringSidW(logon, reinterpret_cast<LPWSTR*>(&sid.value)))
    Fail();
  auto sddl = std::wstring(L"D:P(A;;") + rights + L";;;" + static_cast<wchar_t*>(sid.value) + L")";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
      reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr))
    Fail();
  m_descriptor = descriptor.value;
  descriptor.value = nullptr;
  m_attributes = {sizeof(m_attributes), m_descriptor, FALSE};
}
LogonSecurity::~LogonSecurity() {
  if (m_descriptor)
    LocalFree(m_descriptor);
}
} // namespace warvk::host::lab
