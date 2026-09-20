#pragma once
#include "win32_transport.h"

namespace warvk::host::lab {
// No borrowed PID authority: creation-time identity plus an owned process handle.
// Windows atomically assigns the child to a non-inheritable kill-on-close Job.
// Final destruction drains actual process exit, not a guessed grace period.
// Lab-only: an external process watchdog is mandatory for kernel stalls.
class OwnedChild {
public:
  OwnedChild(const std::wstring& executable,const std::wstring& arguments,
             const std::wstring& createNewLog);
  ~OwnedChild();
  OwnedChild(const OwnedChild&)=delete;
  OwnedChild& operator=(const OwnedChild&)=delete;
  HANDLE process() const noexcept {return m_process.get();}
  DWORD pid() const noexcept {return m_pid;}
  uint64_t creation() const noexcept {return m_creation;}
  DWORD wait(DWORD milliseconds);
  void terminateAndWait();
private:
  Handle m_job,m_process;
  DWORD m_pid=0;
  uint64_t m_creation=0;
};
std::wstring QuoteAbsoluteFile(const std::wstring& path);
} // namespace warvk::host::lab
