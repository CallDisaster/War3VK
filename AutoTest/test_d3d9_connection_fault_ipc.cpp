// Real development Connection/worker/owned child/authenticated pipe. The x64
// peer is a protocol adversary, not DXVK or a GPU. No fake parent acceptReply.
#define WARVK_D3D9_CONNECTION_CPU_TEST 1
#include "d3d9_connection.h"
#include <array>
#include <cwchar>

using namespace warvk::host;
namespace {
#ifdef WARVK_X1A_FAULT_BATCH
constexpr bool DrawBatchFault=true;
#else
constexpr bool DrawBatchFault=false;
#endif
#ifdef WARVK_X1A_FAULT_EVENT
constexpr bool EventWaitFault=true;
#else
constexpr bool EventWaitFault=false;
#endif
void need(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::wstring setting(const wchar_t* key) {
  wchar_t value[256]{};const auto size=GetEnvironmentVariableW(key,value,256);
  need(size>0&&size<256,"fault fixture environment missing");return value;
}
std::wstring scenario() {
  const auto value=setting(L"WARVK_X1A_FAULT_CASE");
  need(value==L"sfalse"||value==L"payload"||value==L"nonce"||value==L"peer-exit"
    ||value==L"partial-header","unknown fault case");return value;
}
std::wstring eventName() {
  const auto token=setting(L"WARVK_X1A_FAULT_TOKEN");
  need(token.size()==32&&std::all_of(token.begin(),token.end(),[](wchar_t ch){
    return (ch>=L'0'&&ch<=L'9')||(ch>=L'a'&&ch<=L'f');}),"exact local test token");
  return L"Local\\WarVK_X1aFault_"+token;
}
void witness(const char* role) {
  const auto isolated=d9::RequireIsolatedDesktop();
  need(d9::CurrentDesktop()==setting(L"WARVK_X1A_EXPECT_DESKTOP")
    &&d9::InputDesktop()==setting(L"WARVK_X1A_EXPECT_INPUT"),"fault thread Desktop mismatch");
  std::printf("{\"schema\":\"warvk-x1a-fault-desktop/v1\",\"role\":\"%s\","
    "\"pid\":%lu,\"tid\":%lu,\"bits\":%u,\"desktop\":%s,\"inputDesktop\":%s,"
    "\"station\":%s,\"isolatedDesktop\":%s,\"vulkanAbsent\":%s}\n",
    role,GetCurrentProcessId(),GetCurrentThreadId(),unsigned(sizeof(void*)*8),
    d9::NameJson(d9::CurrentDesktop()).c_str(),d9::NameJson(d9::InputDesktop()).c_str(),
    d9::NameJson(d9::ObjectName(GetProcessWindowStation())).c_str(),d9::NameJson(isolated).c_str(),
    GetModuleHandleW(L"vulkan-1.dll")?"false":"true");std::fflush(stdout);
}
}

#ifdef WARVK_X1A_FAULT_PEER
static_assert(sizeof(void*)==8,"protocol adversary must be x64");
int wmain(int argc,wchar_t** argv) {
  try {
    witness("adversary");need(!GetModuleHandleW(L"Game.dll"),"game forbidden");
    need(argc==4,"nonce/PID/creation required");const auto nonce=lab::ParseNonce(argv[1]);
    const auto pid=std::stoull(argv[2]);need(pid>0&&pid<=UINT32_MAX,"peer pid");
    const auto created=std::stoull(argv[3]);auto parent=lab::OpenPeer(DWORD(pid),created);
    const auto mode=scenario();
    lab::Handle barrier(OpenEventW(SYNCHRONIZE,FALSE,eventName().c_str()));need(bool(barrier),"open barrier");
    auto pipe=lab::CreateServer(nonce,lab::Channel::D3D9Dev5);lab::IoStats io;
    lab::Accept(pipe.get(),parent.get(),io);lab::VerifyPeer(pipe.get(),true,DWORD(pid));
    d9::Packet request{},response{};d9::Header clear{};
    for(uint64_t sequence=1;sequence<=2;++sequence) {
      lab::ReadFirstByteAfterIdle(pipe.get(),parent.get(),request.data(),io);
      const lab::ReadWindow deadline;
      lab::ReadExact(pipe.get(),parent.get(),request.data()+1,d9::HeaderBytes-1,deadline,lab::Stage::Header,io);
      d9::Header h;need(d9::DecodeHeader(request.data(),h)&&h.nonce==nonce&&h.sequence==sequence,
        "actual request identity");
      need(h.payloadBytes==(DrawBatchFault&&sequence==2?32*d9::DrawBytes:0),"exact adversarial payload extent");
      if(h.payloadBytes)lab::ReadExact(pipe.get(),parent.get(),request.data()+d9::HeaderBytes,
        h.payloadBytes,deadline,lab::Stage::Body,io);
      if(sequence==1) {
        d9::Initialization init;need(d9::DecodeInitialization(h,init)
          &&init.profile==d9::PresentationProfile::Offscreen,"actual Initialize");
        d9::EncodeReply(h,d9::Reply{DWORD(D3D_OK),0,0,0,0,0},response);
        lab::WriteBytes(pipe.get(),parent.get(),response.data(),d9::ReplyBytes,io);
      } else {
        if(DrawBatchFault) {
          need(h.op==d9::Op::DrawBatch&&h.args[0]==32,"actual async batch seq2");
          for(unsigned i=0;i<32;++i)for(unsigned j=0;j<d9::DrawBytes;++j)
            need(request[d9::HeaderBytes+i*d9::DrawBytes+j]==uint8_t(i),"owned draw bytes changed in real pipe");
        } else if(EventWaitFault)need(h.op==d9::Op::WaitEvent&&h.id==7&&h.generation==1
            &&h.args[0]==2&&h.args[2]==9,"actual pending GPU event wait seq2");
        else need(h.op==d9::Op::Clear&&h.args[0]==0xff123456&&h.args[2]==D3DCLEAR_TARGET,"actual Clear seq2");
        clear=h;
      }
    }
    need(WaitForSingleObject(barrier.get(),4000)==WAIT_OBJECT_0,"pending Readback handshake");
    bool eof=false;
    if(mode!=L"peer-exit") {
      auto sent=clear;
      if(mode==L"nonce")sent.nonce.low^=1;
      const uint32_t bytes=mode==L"payload"?4u:0u;
      d9::EncodeReply(sent,d9::Reply{DWORD(mode==L"sfalse"?S_FALSE:D3D_OK),0,0,0,0,bytes},response);
      for(uint32_t i=0;i<bytes;++i)response[d9::ReplyBytes+i]=uint8_t(0xa0+i);
      const auto count=mode==L"partial-header"?1u:d9::ReplyBytes+bytes;
      lab::WriteBytes(pipe.get(),parent.get(),response.data(),count,io);
      // Stay alive until the actual Connection closes its failed pipe. This
      // keeps a planned malformed reply distinct from racing peer termination.
      try { uint8_t unexpected=0;lab::ReadFirstByteAfterIdle(pipe.get(),parent.get(),&unexpected,io); }
      catch(const lab::Failure& e) {
        eof=e.fault==lab::Fault::EndOfStream
          ||(e.fault==lab::Fault::System&&(e.win32==ERROR_BROKEN_PIPE||e.win32==ERROR_NO_DATA));
        if(!eof)throw;
      }
      need(eof,"Readback unexpectedly reached failed adversary");
    }
    std::printf("{\"schema\":\"warvk-x1a-fault-peer/v1\",\"case\":%s,"
      "\"pid\":%lu,\"creation\":%llu,\"parentPid\":%lu,\"parentCreation\":%llu,"
      "\"nonceLow\":%llu,\"nonceHigh\":%llu,\"requests\":2,\"lastSequence\":2,"
      "\"readbackHandshake\":true,\"endOfStreamObserved\":%s,\"intentionalExit\":%s,"
      "\"peerVerified\":true,\"gpuTested\":false,\"productOwnerComplete\":false}\n",
      d9::NameJson(mode).c_str(),GetCurrentProcessId(),lab::CreationTime(GetCurrentProcess()),DWORD(pid),created,
      nonce.low,nonce.high,eof?"true":"false",mode==L"peer-exit"?"true":"false");
    std::fflush(stdout);return 0;
  } catch(const std::exception& e) {std::fprintf(stderr,"fault adversary failure: %s\n",e.what());return 1;}
}
#else
static_assert(sizeof(void*)==4,"actual Connection fixture must be x86");
namespace warvk::host::d3d9proxy {
struct ConnectionCpuTest {
  static void observe(Connection& c,std::atomic<bool>& flag){c.replyWaitObservedForTest=&flag;}
  static bool waitingPrefix(const Connection& c) {
    // The controller acquired actual call()'s waiting flag; peer still waits
    // for its event. No producer write can follow until this controller signals.
    return c.sequence==3&&c.done.load(std::memory_order_acquire)==1;
  }
  static void join(Connection& c){c.closeProducer();if(c.worker.joinable())c.worker.join();}
  static void report(const Connection& c,const std::wstring& mode,ULONGLONG elapsed,bool guard) {
    const auto q=c.queue.countersAfterJoin();
    need(!c.worker.joinable()&&c.failed.load()&&!c.healthy()&&c.sequence==3&&c.done.load()==1,
      "fault must retain exact published/settled prefix");
    need(q.faulted&&q.published==3&&q.cpuAcknowledged==1&&q.queued==1&&q.pendingCpuAck,
      "failed pending work must be retained, not called GPU retirement");
    need(c.peerVerified&&c.exchanges==1&&!c.remoteTerminal&&!c.deviceLost.load(),"no invented GPU DEVICELOST reply");
    const bool replyFault=mode==L"sfalse"||mode==L"payload";
    need(c.asyncFailureSequence==(replyFault?2u:0u),"failure stage identity");
    std::printf("{\"schema\":\"warvk-x1a-fault-call/v1\",\"case\":%s,"
      "\"pid\":%lu,\"creation\":%llu,\"childPid\":%lu,\"childCreation\":%llu,"
      "\"nonceLow\":%llu,\"nonceHigh\":%llu,\"published\":%llu,\"acknowledged\":%llu,"
      "\"queued\":%u,\"pendingCpuAck\":true,\"faulted\":true,\"lastPublished\":%u,\"done\":%u,"
      "\"asyncFailureSequence\":%llu,\"asyncFailureResult\":%lu,\"barrierHresult\":%lu,"
      "\"guardUnchanged\":%s,\"guardBytes\":32,\"resultBytes\":16,\"waitingHandshake\":true,"
      "\"subsequentClearRefused\":true,\"barrierElapsedMs\":%llu,\"ipcTested\":true,"
      "\"gpuTested\":false,\"publicComTested\":false,\"productOwnerComplete\":false}\n",
      d9::NameJson(mode).c_str(),GetCurrentProcessId(),lab::CreationTime(GetCurrentProcess()),c.childPid,c.childCreation,
      c.nonce.low,c.nonce.high,q.published,q.cpuAcknowledged,q.queued,c.sequence,c.done.load(),
      c.asyncFailureSequence,c.asyncFailureResult,DWORD(D3DERR_DEVICELOST),guard?"true":"false",elapsed);
    std::fflush(stdout);
  }
};
}
int wmain(int argc,wchar_t**) {
  try {
    need(argc==1,"no parent arguments");witness("owner");
    const auto mode=scenario();
    SetLastError(ERROR_SUCCESS);lab::Handle event(CreateEventW(nullptr,TRUE,FALSE,eventName().c_str()));
    const auto created=GetLastError();need(bool(event)&&created!=ERROR_ALREADY_EXISTS,"fresh non-inherited barrier");
    std::atomic<bool> waiting{false},controllerOkay{false},stop{false};
    // true enables the real worker's Desktop guard and explicit child Desktop;
    // the wire profile remains offscreen and this adversary creates no GPU.
    d3d9proxy::Connection c(true);d9::Header init;init.op=d9::Op::Initialize;init.args[0]=64;init.args[1]=64;
    need(c.call(init)==D3D_OK,"real authenticated Initialize reply");
    d3d9proxy::ConnectionCpuTest::observe(c,waiting);
    std::thread controller([&] {
      try {
        witness("controller");const auto until=GetTickCount64()+5000;
        while(!waiting.load(std::memory_order_acquire)&&!stop.load(std::memory_order_acquire)
          &&GetTickCount64()<until)Sleep(1);
        const bool ready=waiting.load(std::memory_order_acquire)
          &&d3d9proxy::ConnectionCpuTest::waitingPrefix(c);
        controllerOkay.store(ready,std::memory_order_release);
      } catch(...) {controllerOkay.store(false,std::memory_order_release);}
      // Error paths also release the test pause, but cannot pass the predicate.
      SetEvent(event.get());
    });
    d9::Header clear;clear.op=d9::Op::Clear;clear.args[0]=0xff123456;
    clear.args[1]=d9::FloatBits(1.0f);clear.args[2]=D3DCLEAR_TARGET;
    HRESULT admitted=D3D_OK;
    if(DrawBatchFault) {
      d9::DrawPacket packet{};
      for(unsigned i=0;i<32;++i){packet.fill(uint8_t(i));admitted=c.submitDraw(packet);need(admitted==D3D_OK,"async draw admission");}
    } else if(EventWaitFault) {
      d9::Header event;event.op=d9::Op::Poll;event.id=7;event.generation=1;event.args[0]=2;event.args[2]=9;
      admitted=c.pollEvent(event);
    } else admitted=c.enqueueClear(clear);
    std::array<uint8_t,32> guard{};guard.fill(0xcd);const auto before=guard;
    d9::Header read;read.op=d9::Op::Readback;const auto began=GetTickCount64();
    const auto result=c.call(read,nullptr,guard.data()+8,16);const auto elapsed=GetTickCount64()-began;
    stop.store(true,std::memory_order_release);controller.join();
    const auto refused=c.enqueueClear(clear);
    d3d9proxy::ConnectionCpuTest::join(c);
    need(admitted==(EventWaitFault?S_FALSE:D3D_OK)&&result==D3DERR_DEVICELOST&&refused==D3DERR_DEVICELOST,"actual call results");
    need(controllerOkay.load(std::memory_order_acquire)&&guard==before,"waiting/guard invariant");
    d3d9proxy::ConnectionCpuTest::report(c,mode,elapsed,guard==before);
    return 0;
  } catch(const std::exception& e) {std::fprintf(stderr,"fault Connection fixture failure: %s\n",e.what());return 1;}
}
#endif
