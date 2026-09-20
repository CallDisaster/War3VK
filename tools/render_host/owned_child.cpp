#include "owned_child.h"
#include <vector>
#include <exception>

namespace warvk::host::lab {
namespace {
[[noreturn]] void Fail() {throw Failure(Fault::System,Stage::Setup,GetLastError());}
// Lab-only final drain, just like overlapped cancellation. A timeout cannot
// grant permission to destroy the transaction's shared container. The outer
// process watchdog contains a stuck kernel and records a FAILED gate.
void DrainChild(HANDLE process) noexcept {
  if (process && WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0)
    std::terminate(); // Never return to release dependent owners without proof.
}
struct Attributes {
  std::vector<uint8_t> bytes;
  LPPROC_THREAD_ATTRIBUTE_LIST list=nullptr;
  Attributes() {
    SIZE_T size=0;
    InitializeProcThreadAttributeList(nullptr,2,0,&size);
    if(!size||size>65536)Fail();
    bytes.resize(size);list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
    if(!InitializeProcThreadAttributeList(list,2,0,&size)){list=nullptr;Fail();}
  }
  ~Attributes(){if(list)DeleteProcThreadAttributeList(list);}
};
}
std::wstring QuoteAbsoluteFile(const std::wstring& path) {
  if(path.size()<3||path[1]!=L':'||(path[2]!=L'\\'&&path[2]!=L'/')||
    path.find_first_of(L"\"\r\n")!=std::wstring::npos)
    throw Failure(Fault::System,Stage::Setup,ERROR_INVALID_NAME);
  return L"\""+path+L"\"";
}
OwnedChild::OwnedChild(const std::wstring& executable,const std::wstring& arguments,
                       const std::wstring& log) {
  auto command=QuoteAbsoluteFile(executable)+L" "+arguments;
  QuoteAbsoluteFile(log);
  if(arguments.find_first_of(L"\r\n")!=std::wstring::npos)
    throw Failure(Fault::System,Stage::Setup,ERROR_INVALID_PARAMETER);
  m_job.reset(CreateJobObjectW(nullptr,nullptr));if(!m_job)Fail();
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if(!SetInformationJobObject(m_job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)))Fail();
  SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
  Handle output(CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,
    FILE_ATTRIBUTE_NORMAL,nullptr));
  if(!output)Fail();
  Attributes attributes;
  HANDLE jobs[]={m_job.get()},inherited[]={output.get()};
  // No CreateProcess-then-AssignProcessToJobObject race or uncontained fallback.
  if(!UpdateProcThreadAttribute(attributes.list,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,
      jobs,sizeof(jobs),nullptr,nullptr)||
     !UpdateProcThreadAttribute(attributes.list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
      inherited,sizeof(inherited),nullptr,nullptr))Fail();
  STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);
  startup.lpAttributeList=attributes.list;
  startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdOutput=output.get();startup.StartupInfo.hStdError=output.get();
  startup.StartupInfo.hStdInput=nullptr;
  PROCESS_INFORMATION info{};
  if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
      CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS|EXTENDED_STARTUPINFO_PRESENT,
      nullptr,nullptr,&startup.StartupInfo,&info))Fail();
  Handle thread(info.hThread);m_process.reset(info.hProcess);m_pid=info.dwProcessId;
  try {
    m_creation=CreationTime(m_process.get());
    BOOL inJob=FALSE;
    if(!IsProcessInJob(m_process.get(),m_job.get(),&inJob)||!inJob)Fail();
  }catch(...){
    // A throwing constructor has no class destructor. Settle the already
    // created child while its exact process handle is still owned here.
    m_job.reset();
    DrainChild(m_process.get());
    throw;
  }
}
OwnedChild::~OwnedChild() {
  // Closing the sole Job handle also covers exception unwinding. Keep process
  // handle alive until its exit is observed, never terminate by process name.
  m_job.reset();
  DrainChild(m_process.get());
}
DWORD OwnedChild::wait(DWORD milliseconds) {
  const DWORD result=WaitForSingleObject(m_process.get(),milliseconds);
  if(result==WAIT_TIMEOUT)throw Failure(Fault::Timeout,Stage::Setup,WAIT_TIMEOUT);
  if(result!=WAIT_OBJECT_0)Fail();
  DWORD code=STILL_ACTIVE;if(!GetExitCodeProcess(m_process.get(),&code))Fail();
  return code;
}
void OwnedChild::terminateAndWait() {m_job.reset();wait(5000);}
} // namespace warvk::host::lab
