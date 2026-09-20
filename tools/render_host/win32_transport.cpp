#include "win32_transport.h"
#include "win32_security.h"
#include <bcrypt.h>
#include <algorithm>

namespace warvk::host::lab {
namespace {
[[noreturn]] void System(Stage stage) { throw Failure(Fault::System,stage,GetLastError()); }
DWORD Remaining(ULONGLONG deadline) {
  const auto now=GetTickCount64();
  return now>=deadline?0:DWORD(deadline-now);
}
// An OVERLAPPED never outlives its event or caller-owned buffer. Cancellation
// is a request, not a release permission. External lab watchdog bounds a stuck
// kernel drain by terminating the entire owned test process, never by UAF.
DWORD Complete(HANDLE pipe,HANDLE peer,OVERLAPPED& ov,ULONGLONG deadline,
               Stage stage,IoStats& stats) {
  HANDLE waits[]={ov.hEvent,peer};
  DWORD result=WaitForMultipleObjects(2,waits,FALSE,Remaining(deadline));
  DWORD transferred=0;
  if(result==WAIT_OBJECT_0){
    if(!GetOverlappedResult(pipe,&ov,&transferred,FALSE))System(stage);
    return transferred;
  }
  DWORD error=result==WAIT_FAILED?GetLastError():0;
  ++stats.cancelRequests;
  // ERROR_NOT_FOUND can mean completion raced cancellation. In all cases drain.
  CancelIoEx(pipe,&ov);
  BOOL completed=GetOverlappedResult(pipe,&ov,&transferred,TRUE);
  DWORD completionError=completed?ERROR_SUCCESS:GetLastError();
  if(completed||completionError==ERROR_OPERATION_ABORTED||
      completionError==ERROR_BROKEN_PIPE||completionError==ERROR_PIPE_NOT_CONNECTED)
    ++stats.cancelCompletions;
  else throw Failure(Fault::System,stage,completionError);
  if(result==WAIT_TIMEOUT)throw Failure(Fault::Timeout,stage,WAIT_TIMEOUT);
  if(result==WAIT_OBJECT_0+1)throw Failure(Fault::PeerExited,stage);
  throw Failure(Fault::System,stage,error);
}
void Transfer(HANDLE pipe,HANDLE peer,uint8_t* bytes,size_t count,bool writing,
              ULONGLONG deadline,Stage stage,IoStats& stats,size_t fragment) {
  if(!fragment)throw Failure(Fault::System,stage,ERROR_INVALID_PARAMETER);
  while(count){
    if(!Remaining(deadline))throw Failure(Fault::Timeout,stage,WAIT_TIMEOUT);
    const DWORD requested=DWORD(std::min(count,fragment));
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if(!event)System(stage);
    OVERLAPPED ov{};ov.hEvent=event.get();DWORD transferred=0;
    BOOL done;
    if(writing){++stats.writes;done=WriteFile(pipe,bytes,requested,&transferred,&ov);}
    else {++stats.reads;done=ReadFile(pipe,bytes,requested,&transferred,&ov);}
    if(!done){
      if(GetLastError()!=ERROR_IO_PENDING)System(stage);
      transferred=Complete(pipe,peer,ov,deadline,stage,stats);
    }
    if(!transferred||transferred>requested)throw Failure(Fault::EndOfStream,stage);
    if(transferred<requested){if(writing)++stats.shortWrites;else ++stats.shortReads;}
    bytes+=transferred;count-=transferred;
  }
}
}
uint64_t CreationTime(HANDLE process) {
  FILETIME created{},exit{},kernel{},user{};
  if(!GetProcessTimes(process,&created,&exit,&kernel,&user))System(Stage::Setup);
  return uint64_t(created.dwHighDateTime)<<32|created.dwLowDateTime;
}
Handle OpenPeer(DWORD pid,uint64_t creation) {
  Handle result(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
  if(!result)System(Stage::Setup);
  if(CreationTime(result.get())!=creation||WaitForSingleObject(result.get(),0)!=WAIT_TIMEOUT)
    throw Failure(Fault::PeerIdentity,Stage::Setup);
  return result;
}
ipc::Nonce RandomNonce() {
  ipc::Nonce nonce;
  if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&nonce),sizeof(nonce),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0||nonce.empty())
    throw Failure(Fault::System,Stage::Setup,ERROR_GEN_FAILURE);
  return nonce;
}
std::wstring NonceText(ipc::Nonce n) {
  static constexpr wchar_t digits[]=L"0123456789abcdef";
  std::wstring text(32,L'0');
  for(unsigned i=0;i<16;++i){text[i]=digits[(n.high>>(60-i*4))&15];
    text[i+16]=digits[(n.low>>(60-i*4))&15];}
  return text;
}
ipc::Nonce ParseNonce(const std::wstring& text) {
  if(text.size()!=32)throw Failure(Fault::System,Stage::Setup,ERROR_INVALID_PARAMETER);
  ipc::Nonce n;
  for(size_t i=0;i<32;++i){wchar_t c=text[i];
    if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))
      throw Failure(Fault::System,Stage::Setup,ERROR_INVALID_PARAMETER);
    auto& half=i<16?n.high:n.low;half=(half<<4)|uint64_t(c<=L'9'?c-L'0':c-L'a'+10);
  }
  if(n.empty())throw Failure(Fault::System,Stage::Setup,ERROR_INVALID_PARAMETER);
  return n;
}
std::wstring PipeName(ipc::Nonce n, Channel channel) {
  if (channel == Channel::Echo0) return L"\\\\.\\pipe\\warvk-rh0-" + NonceText(n);
  if (channel == Channel::Slots1) return L"\\\\.\\pipe\\warvk-rh1-" + NonceText(n);
  throw Failure(Fault::System, Stage::Setup, ERROR_INVALID_PARAMETER);
}
Handle CreateServer(ipc::Nonce nonce, Channel channel) {
  LogonSecurity security(SecurityObject::Pipe);
  Handle pipe(CreateNamedPipeW(PipeName(nonce, channel).c_str(),PIPE_ACCESS_DUPLEX|
    FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,
    1,4096,4096,DeadlineMs,security.get()));
  if(!pipe)System(Stage::Setup);
  return pipe;
}
void Accept(HANDLE pipe,HANDLE peer,IoStats& stats) {
  Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event)System(Stage::Connect);
  OVERLAPPED ov{};ov.hEvent=event.get();
  if(ConnectNamedPipe(pipe,&ov))return;
  const DWORD error=GetLastError();
  if(error==ERROR_PIPE_CONNECTED)return;
  if(error!=ERROR_IO_PENDING)throw Failure(Fault::System,Stage::Connect,error);
  Complete(pipe,peer,ov,GetTickCount64()+DeadlineMs,Stage::Connect,stats);
}
void VerifyPeer(HANDLE pipe,bool server,DWORD expected) {
  ULONG actual=0;
  BOOL ok=server?GetNamedPipeClientProcessId(pipe,&actual):GetNamedPipeServerProcessId(pipe,&actual);
  if(!ok)System(Stage::Connect);
  if(!expected||actual!=expected)throw Failure(Fault::PeerIdentity,Stage::Connect);
}
Handle Connect(ipc::Nonce nonce,HANDLE peer,DWORD expected, Channel channel) {
  ULONGLONG deadline=GetTickCount64()+DeadlineMs;
  for(;;){
    if(WaitForSingleObject(peer,0)!=WAIT_TIMEOUT)throw Failure(Fault::PeerExited,Stage::Connect);
    Handle pipe(CreateFileW(PipeName(nonce, channel).c_str(),0x0012019B,0,nullptr,OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
    if(pipe){VerifyPeer(pipe.get(),false,expected);return pipe;}
    const DWORD error=GetLastError();
    if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PIPE_BUSY)throw Failure(Fault::System,Stage::Connect,error);
    const DWORD remaining=Remaining(deadline);
    if(!remaining)throw Failure(Fault::Timeout,Stage::Connect,WAIT_TIMEOUT);
    WaitForSingleObject(peer,std::min<DWORD>(remaining,10));
  }
}
size_t ReadPacket(HANDLE pipe,HANDLE peer,Packet& packet,IoStats& stats) {
  const auto deadline=GetTickCount64()+DeadlineMs;
  Transfer(pipe,peer,packet.data(),ipc::HeaderBytes,false,deadline,Stage::Header,stats,ipc::HeaderBytes);
  ipc::Header header;
  auto error=ipc::DecodeHeader(packet.data(),ipc::HeaderBytes,header);
  if(error!=ipc::Error::None)throw Failure(Fault::HeaderInvalid,Stage::Header,0,error);
  Transfer(pipe,peer,packet.data()+ipc::HeaderBytes,header.payloadBytes,false,deadline,Stage::Body,stats,
    ipc::MaxPayloadBytes);
  return ipc::HeaderBytes+header.payloadBytes;
}
void WriteBytes(HANDLE pipe,HANDLE peer,const uint8_t* data,size_t bytes,IoStats& stats,size_t fragment) {
  if(!data||bytes>ipc::MaxMessageBytes)throw Failure(Fault::System,Stage::Write,ERROR_INVALID_PARAMETER);
  Transfer(pipe,peer,const_cast<uint8_t*>(data),bytes,true,GetTickCount64()+DeadlineMs,
    Stage::Write,stats,fragment);
}
void ReadExact(HANDLE pipe,HANDLE peer,uint8_t* bytes,size_t count,const ReadWindow& window,
               Stage stage,IoStats& stats) {
  if (!bytes || count > ipc::MaxMessageBytes || (stage != Stage::Header && stage != Stage::Body))
    throw Failure(Fault::System, stage, ERROR_INVALID_PARAMETER);
  Transfer(pipe, peer, bytes, count, false, window.m_until, stage, stats, ipc::MaxMessageBytes);
}
const char* StageName(Stage s) noexcept {
  switch(s){case Stage::Setup:return "setup";case Stage::Connect:return "connect";
    case Stage::Header:return "header";case Stage::Body:return "body";case Stage::Write:return "write";
    case Stage::SlotMutex:return "slot-mutex";case Stage::SlotCopy:return "slot-copy";}
  return "unknown";
}
const char* FaultName(Fault f) noexcept {
  switch(f){case Fault::System:return "system";case Fault::Timeout:return "timeout";
    case Fault::PeerExited:return "peer-exited";case Fault::PeerIdentity:return "peer-identity";
    case Fault::HeaderInvalid:return "header-invalid";case Fault::EndOfStream:return "end-of-stream";
    case Fault::Abandoned:return "abandoned";case Fault::SlotLease:return "slot-lease";}
  return "unknown";
}
} // namespace warvk::host::lab
