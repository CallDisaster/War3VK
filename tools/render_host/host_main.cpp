#include "win32_transport.h"
#include <cstdio>
#include <cwchar>
#include <limits>

using namespace warvk::host;
static_assert(sizeof(void*)==8,"RH0 host must be an actual 64-bit executable");
namespace {
uint64_t Number(const wchar_t* text) {
  if(!text||!*text)throw lab::Failure(lab::Fault::System,lab::Stage::Setup,ERROR_INVALID_PARAMETER);
  uint64_t value=0;
  for(;*text;++text){
    if(*text<L'0'||*text>L'9'||value>(UINT64_MAX-uint64_t(*text-L'0'))/10)
      throw lab::Failure(lab::Fault::System,lab::Stage::Setup,ERROR_INVALID_PARAMETER);
    value=value*10+uint64_t(*text-L'0');
  }
  return value;
}
}
int wmain(int argc,wchar_t** argv) {
  lab::IoStats stats;bool verified=false;
  uint64_t accepted=0;const char* state="fault";
  const char* fault="none";const char* stage="none";const char* protocol="None";DWORD win32=0;
  int code=2;
  try {
    if(argc!=4)throw lab::Failure(lab::Fault::System,lab::Stage::Setup,ERROR_INVALID_PARAMETER);
    auto nonce=lab::ParseNonce(argv[1]);auto parentNumber=Number(argv[2]);
    if(!parentNumber||parentNumber>MAXDWORD)
      throw lab::Failure(lab::Fault::System,lab::Stage::Setup,ERROR_INVALID_PARAMETER);
    auto parent=lab::OpenPeer(DWORD(parentNumber),Number(argv[3]));
    auto pipe=lab::CreateServer(nonce);lab::Accept(pipe.get(),parent.get(),stats);
    lab::VerifyPeer(pipe.get(),true,DWORD(parentNumber));verified=true;
    ipc::ServerSession session(nonce,64,32);lab::Packet input{},output{};
    try {
      for(;;){
        auto size=lab::ReadPacket(pipe.get(),parent.get(),input,stats);
        ipc::Reply reply;auto error=session.receive(input.data(),size,reply);
        accepted=session.accepted();
        if(error!=ipc::Error::None){protocol=ipc::ErrorName(error);break;}
        size_t written=0;
        error=ipc::Encode(reply.header,reply.payload(),reply.header.payloadBytes,output.data(),output.size(),written);
        if(error!=ipc::Error::None){session.transportFailed();protocol=ipc::ErrorName(error);break;}
        lab::WriteBytes(pipe.get(),parent.get(),output.data(),written,stats);
        if(reply.header.op==ipc::Op::Close){
          error=session.closeReplySent(reply.header.sequence);
          if(error!=ipc::Error::None){protocol=ipc::ErrorName(error);break;}
          state="closed";code=0;break;
        }
      }
    }catch(...){session.transportFailed();accepted=session.accepted();throw;}
  }catch(const lab::Failure& f){fault=lab::FaultName(f.fault);stage=lab::StageName(f.stage);
    win32=f.win32;protocol=ipc::ErrorName(f.protocol);
  }catch(...){fault="unexpected-exception";}
  std::printf("{\"bits\":64,\"peerVerified\":%s,\"state\":\"%s\",\"accepted\":%llu,"
    "\"fault\":\"%s\",\"stage\":\"%s\",\"protocol\":\"%s\",\"win32\":%lu,"
    "\"reads\":%llu,\"writes\":%llu,\"shortReads\":%llu,\"shortWrites\":%llu,"
    "\"cancelRequests\":%llu,\"cancelCompletions\":%llu}\n",
    verified?"true":"false",state,static_cast<unsigned long long>(accepted),fault,stage,protocol,
    static_cast<unsigned long>(win32),static_cast<unsigned long long>(stats.reads),
    static_cast<unsigned long long>(stats.writes),static_cast<unsigned long long>(stats.shortReads),
    static_cast<unsigned long long>(stats.shortWrites),static_cast<unsigned long long>(stats.cancelRequests),
    static_cast<unsigned long long>(stats.cancelCompletions));
  std::fflush(stdout);return code;
}
