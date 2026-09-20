#include "../tools/render_host/owned_child.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <algorithm>
#include <vector>

using namespace warvk::host;
static_assert(sizeof(void*)==4,"RH0 transport gate must be an actual Win32 client");
namespace {
void CheckAt(bool value,unsigned line){
  if(!value)throw std::runtime_error("RH0 client assertion line="+std::to_string(line));
}
#define Check(value) CheckAt(bool(value),__LINE__)
struct DependencyInit {DWORD pid=0,before=0,after=0;uint64_t creation=0;};
struct CycleReceipt {DWORD pid=0,exit=0,handles=0;uint64_t creation=0;unsigned exchanges=0;};
struct Client {
  HANDLE pipe,peer;ipc::Nonce nonce;lab::IoStats stats;
  uint64_t sequence=0,map=0,device=0,frame=0;size_t fragment=ipc::MaxMessageBytes;
  unsigned exchanges=0;
  ipc::Header header(ipc::Op op,uint32_t size=0) {
    ipc::Header h;h.op=op;h.nonce=nonce;h.sequence=++sequence;h.payloadBytes=size;
    h.mapEpoch=map;h.deviceEpoch=device;return h;
  }
  void send(const ipc::Header& h,const uint8_t* payload=nullptr){
    lab::Packet packet{};size_t bytes=0;
    Check(ipc::Encode(h,payload,h.payloadBytes,packet.data(),packet.size(),bytes)==ipc::Error::None);
    lab::WriteBytes(pipe,peer,packet.data(),bytes,stats,fragment);
  }
  void exchange(ipc::Header h,const uint8_t* payload=nullptr){
    send(h,payload);lab::Packet actual{},expected{};
    const auto size=lab::ReadPacket(pipe,peer,actual,stats);h.flags=1;
    auto hello=ipc::EncodeHello({64,32,1,1});
    size_t bytes=0;
    Check(ipc::Encode(h,h.op==ipc::Op::Hello?hello.data():payload,h.payloadBytes,
      expected.data(),expected.size(),bytes)==ipc::Error::None);
    Check(bytes==size&&!std::memcmp(expected.data(),actual.data(),size));++exchanges;
  }
  void hello(){auto p=ipc::EncodeHello({32,64,1,1});exchange(header(ipc::Op::Hello,24),p.data());}
  void begin(uint64_t m,uint64_t d){map=m;device=d;frame=0;exchange(header(ipc::Op::BeginEpoch));}
  void sample(uint32_t count,bool read=true){
    std::array<uint8_t,ipc::MaxPayloadBytes> data{};
    for(size_t i=0;i<count;++i)data[i]=uint8_t((i*37+19)^frame);
    auto h=header(ipc::Op::SampleEcho,count);h.frameId=++frame;
    if(read)exchange(h,data.data());else send(h,data.data());
  }
  void end(){exchange(header(ipc::Op::EndEpoch));}
  void close(){map=device=0;exchange(header(ipc::Op::Close));}
};
void Receipt(const char* name,DWORD pid,uint64_t creation,DWORD exit,unsigned exchanges,
             bool verified,DWORD before,DWORD after,const lab::IoStats& stats,bool abrupt,
             const DependencyInit& init,const std::vector<CycleReceipt>& cycles={}) {
  std::printf("{\"case\":\"%s\",\"bits\":32,\"childPid\":%lu,\"childCreation\":%llu,"
    "\"childExit\":%lu,\"exchanges\":%u,\"peerVerified\":%s,\"handlesBefore\":%lu,"
    "\"handlesAfter\":%lu,\"writes\":%llu,\"abruptParentExit\":%s,"
    "\"dependencyInit\":{\"pid\":%lu,\"creation\":%llu,\"handlesBefore\":%lu,\"handlesAfter\":%lu},\"cycles\":[",name,
    static_cast<unsigned long>(pid),static_cast<unsigned long long>(creation),
    static_cast<unsigned long>(exit),exchanges,verified?"true":"false",
    static_cast<unsigned long>(before),static_cast<unsigned long>(after),
    static_cast<unsigned long long>(stats.writes),abrupt?"true":"false",
    static_cast<unsigned long>(init.pid),static_cast<unsigned long long>(init.creation),
    static_cast<unsigned long>(init.before),static_cast<unsigned long>(init.after));
  for(size_t i=0;i<cycles.size();++i){const auto& c=cycles[i];
    std::printf("%s{\"pid\":%lu,\"creation\":%llu,\"exit\":%lu,\"handles\":%lu,\"exchanges\":%u}",
      i?",":"",static_cast<unsigned long>(c.pid),static_cast<unsigned long long>(c.creation),
      static_cast<unsigned long>(c.exit),static_cast<unsigned long>(c.handles),c.exchanges);
  }
  std::printf("]}\n");
  std::fflush(stdout);
}
}
int wmain(int argc,wchar_t** argv) {
  try {
    if(argc==2&&!std::wcscmp(argv[1],L"--dependency-init")){
      std::printf("{\"dependencyInit\":true,\"bits\":32}\n");return 0;
    }
    if(argc==5&&!std::wcscmp(argv[1],L"--intruder")){
      auto nonce=lab::ParseNonce(argv[2]);
      wchar_t* end=nullptr;auto pid=std::wcstoul(argv[3],&end,10);Check(end&&!*end&&pid);
      auto creation=std::wcstoull(argv[4],&end,10);Check(end&&!*end&&creation);
      auto peer=lab::OpenPeer(DWORD(pid),creation);auto pipe=lab::Connect(nonce,peer.get(),DWORD(pid));
      Check(WaitForSingleObject(peer.get(),7000)==WAIT_OBJECT_0);
      std::printf("{\"intruder\":true,\"bits\":32}\n");return 0;
    }
    Check(argc==4);
    const std::wstring mode=argv[2];
    const char* name=nullptr;
    for(const char* candidate:{"normal","fragment","bad-nonce","bad-sequence","stale-epoch",
      "bad-capability","oversize","disconnect-header","timeout-header","timeout-body",
      "nonreading-peer","wrong-peer","parent-exit","first-instance","connect-timeout","lifecycle-soak"}){
      std::wstring wide(candidate,candidate+std::strlen(candidate));if(mode==wide)name=candidate;
    }
    Check(name!=nullptr);
    // System RNG provider may lazily initialize process-owned handles. Establish
    // the transport lifetime baseline after this unrelated one-time dependency.
    auto nonce=lab::RandomNonce();
    for(unsigned i=1;i<8;++i)lab::RandomNonce();
    DependencyInit init;Check(GetProcessHandleCount(GetCurrentProcess(),&init.before)!=0);
    // Warm the same Win32 process-creation dependencies without any IPC. This
    // is a separately logged, owned, exited child, not a hidden reconnect.
    {
      lab::OwnedChild warmup(argv[0],L"--dependency-init",std::wstring(argv[3])+L".dependency-init");
      init.pid=warmup.pid();init.creation=warmup.creation();
      Check(warmup.wait(7000)==0);
    }
    {auto securityInit=lab::CreateServer(nonce);}
    Check(GetProcessHandleCount(GetCurrentProcess(),&init.after)!=0);
    DWORD before=0,after=0;Check(GetProcessHandleCount(GetCurrentProcess(),&before)!=0);
    DWORD pid=0,exit=STILL_ACTIVE;uint64_t creation=0;unsigned exchanges=0;bool verified=false;
    lab::IoStats stats;std::vector<CycleReceipt> cycles;
    if(mode==L"lifecycle-soak"){
      for(unsigned i=0;i<64;++i){
        CycleReceipt cycle;
        DWORD afterNonce=0,afterArgs=0,afterSpawn=0,afterConnect=0,afterWait=0;
        {
          auto cycleNonce=lab::RandomNonce();
          GetProcessHandleCount(GetCurrentProcess(),&afterNonce);
          auto args=lab::NonceText(cycleNonce)+L" "+std::to_wstring(GetCurrentProcessId())+L" "+
            std::to_wstring(lab::CreationTime(GetCurrentProcess()));
          auto log=std::wstring(argv[3])+(i?L".cycle-"+std::to_wstring(i):L"");
          GetProcessHandleCount(GetCurrentProcess(),&afterArgs);
          lab::OwnedChild child(argv[1],args,log);cycle.pid=child.pid();cycle.creation=child.creation();
          GetProcessHandleCount(GetCurrentProcess(),&afterSpawn);
          auto pipe=lab::Connect(cycleNonce,child.process(),child.pid());
          GetProcessHandleCount(GetCurrentProcess(),&afterConnect);
          Client c{pipe.get(),child.process(),cycleNonce,{}};
          if(i%3==1){
            auto h=c.header(ipc::Op::Hello,24);h.nonce.low^=1;
            auto p=ipc::EncodeHello({32,64,1,1});c.send(h,p.data());
          }else {c.hello();c.begin(i+1,1);c.sample(19);c.end();c.close();}
          cycle.exit=child.wait(7000);Check(cycle.exit==(i%3==1?2u:0u));
          GetProcessHandleCount(GetCurrentProcess(),&afterWait);
          cycle.exchanges=c.exchanges;stats.writes+=c.stats.writes;
        }
        Check(GetProcessHandleCount(GetCurrentProcess(),&cycle.handles)!=0);
        if(cycle.handles!=before)throw std::runtime_error("soak handles cycle="+std::to_string(i)+
          " baseline="+std::to_string(before)+" actual="+std::to_string(cycle.handles)+
          " nonce="+std::to_string(afterNonce)+" args="+std::to_string(afterArgs)+
          " spawn="+std::to_string(afterSpawn)+" connect="+std::to_string(afterConnect)+
          " wait="+std::to_string(afterWait));
        cycles.push_back(cycle);exchanges+=cycle.exchanges;
        pid=cycle.pid;creation=cycle.creation;exit=cycle.exit;
      }
      verified=true;
    }else {
      lab::Handle squat;
      if(mode==L"first-instance")squat=lab::CreateServer(nonce);
      auto arguments=lab::NonceText(nonce)+L" "+std::to_wstring(GetCurrentProcessId())+L" "+
        std::to_wstring(lab::CreationTime(GetCurrentProcess()));
      lab::OwnedChild child(argv[1],arguments,argv[3]);pid=child.pid();creation=child.creation();
      if(mode==L"first-instance"||mode==L"connect-timeout"){
        exit=child.wait(7000);Check(exit==2);
      }else if(mode==L"wrong-peer"){
        auto args=L"--intruder "+lab::NonceText(nonce)+L" "+std::to_wstring(pid)+L" "+std::to_wstring(creation);
        lab::OwnedChild intruder(argv[0],args,std::wstring(argv[3])+L".intruder");
        exit=child.wait(7000);Check(exit==2);Check(intruder.wait(7000)==0);
      }else {
        auto pipe=lab::Connect(nonce,child.process(),pid);verified=true;
        Client c{pipe.get(),child.process(),nonce,{}};
        if(mode==L"fragment")c.fragment=7;
        if(mode==L"bad-nonce"){
          auto h=c.header(ipc::Op::Hello,24);h.nonce.low^=1;
          auto p=ipc::EncodeHello({32,64,1,1});c.send(h,p.data());
        }else if(mode==L"bad-capability"){
          auto p=ipc::EncodeHello({32,64,3,3});c.send(c.header(ipc::Op::Hello,24),p.data());
        }else if(mode==L"oversize"){
          lab::Packet data{};size_t bytes=0;auto h=c.header(ipc::Op::Hello);
          Check(ipc::Encode(h,nullptr,0,data.data(),data.size(),bytes)==ipc::Error::None);
          ipc::WriteLe(data.data()+16,ipc::MaxPayloadBytes+1,4);
          lab::WriteBytes(pipe.get(),child.process(),data.data(),bytes,c.stats);
        }else if(mode==L"disconnect-header"||mode==L"timeout-header"||mode==L"timeout-body"){
          lab::Packet data{};size_t bytes=0;auto h=c.header(ipc::Op::Hello,24);
          auto p=ipc::EncodeHello({32,64,1,1});
          Check(ipc::Encode(h,p.data(),24,data.data(),data.size(),bytes)==ipc::Error::None);
          lab::WriteBytes(pipe.get(),child.process(),data.data(),mode==L"timeout-body"?ipc::HeaderBytes:8,c.stats);
          if(mode==L"disconnect-header")pipe.reset();
        }else {
          c.hello();
          if(mode==L"parent-exit"){
            Receipt(name,pid,creation,STILL_ACTIVE,c.exchanges,true,before,0,c.stats,true,init);
            // Deliberately bypass destructors. Sole non-inherited Job handle
            // closes at process teardown; supervisor checks exact child exit.
            ExitProcess(0);
          }
          if(mode==L"bad-sequence"){
            auto h=c.header(ipc::Op::BeginEpoch);h.sequence=3;h.mapEpoch=3;h.deviceEpoch=7;c.send(h);
          }else {
            c.begin(3,7);
            if(mode==L"stale-epoch"){
              auto h=c.header(ipc::Op::SampleEcho);h.mapEpoch=2;h.frameId=1;c.send(h);
            }else if(mode==L"nonreading-peer")c.sample(ipc::MaxPayloadBytes,false);
            else {
              if(mode==L"fragment")c.fragment=257;
              for(uint32_t count:{0u,1u,4096u,ipc::MaxPayloadBytes})c.sample(count);
              c.end();c.begin(3,8);c.sample(19);c.end();c.close();
            }
          }
        }
        exchanges=c.exchanges;stats=c.stats;
        exit=child.wait(9000);
        Check(exit==((mode==L"normal"||mode==L"fragment")?0u:2u));
      }
    }
    Check(GetProcessHandleCount(GetCurrentProcess(),&after)!=0);
    Receipt(name,pid,creation,exit,exchanges,verified,before,after,stats,false,init,cycles);return 0;
  }catch(const lab::Failure& f){
    std::printf("{\"clientFailure\":\"%s\",\"stage\":\"%s\",\"win32\":%lu}\n",
      lab::FaultName(f.fault),lab::StageName(f.stage),static_cast<unsigned long>(f.win32));return 1;
  }catch(const std::exception& e){std::printf("client assertion/exception: %s\n",e.what());return 1;}
}
