#pragma once
#include "win32_transport.h"

namespace warvk::host::lab {
enum class SecurityObject { Pipe, Mapping, Mutex };
// One logon-SID policy factory; never null/default DACL, execute, global names
// or inheritable IPC objects. Kernel copies this descriptor during creation.
class LogonSecurity {
public:
  explicit LogonSecurity(SecurityObject object);
  ~LogonSecurity();
  LogonSecurity(const LogonSecurity&) = delete;
  LogonSecurity& operator=(const LogonSecurity&) = delete;
  SECURITY_ATTRIBUTES* get() noexcept { return &m_attributes; }
private:
  void* m_descriptor = nullptr;
  SECURITY_ATTRIBUTES m_attributes{};
};
} // namespace warvk::host::lab
