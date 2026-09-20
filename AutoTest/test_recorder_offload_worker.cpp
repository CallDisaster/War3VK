#include "../tools/render_host/owned_child.h"
#include "../tools/render_host/shared_slots.h"
#include "../tools/render_host/slot_channel.h"
#include "../tools/render_host/recorder_ingress.h"
#include "../tools/render_host/recorder_lab_fixture.h"
#include "../tools/render_host/process_memory_probe.h"
#include "render_host_handle_probe.h"
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <memory>
#include <process.h>
#include <psapi.h>

// CPU laboratory only. No HistoryStore linked into this real Win32 process.
// Barriers below coordinate deterministic fixtures, never run inside tryPush.
using namespace warvk::host;
namespace rec = warvk::host::recorder;
static_assert(sizeof(void*) == 4, "actual x86 producer required");
namespace {
void Check(bool b) { if(!b) throw std::runtime_error("recorder fixture assertion"); }
uint64_t Qpc() { LARGE_INTEGER v{}; Check(QueryPerformanceCounter(&v));return v.QuadPart; }
uint64_t Frequency() { LARGE_INTEGER v{};Check(QueryPerformanceFrequency(&v));return v.QuadPart; }
// Optional lab-only, read-only self-thread origin diagnostic. No foreign
// handles are closed, and no private NT ABI is used as an acceptance gate.
void ThreadOrigins() {
  if(!std::getenv("WARVK_HOST_LAB_HANDLE_PROBE"))return;
  using Query=LONG (NTAPI*)(HANDLE,ULONG,void*,ULONG,ULONG*);
  const auto nt=GetModuleHandleW(L"ntdll.dll");
  const auto rp=GetProcAddress(nt,"NtQueryInformationProcess"),rt=GetProcAddress(nt,"NtQueryInformationThread");
  Query query=nullptr,thread=nullptr;std::memcpy(&query,&rp,sizeof(query));std::memcpy(&thread,&rt,sizeof(thread));
  if(!query || !thread)return;
  HMODULE modules[256]{};DWORD moduleBytes=0;
  if(EnumProcessModules(GetCurrentProcess(),modules,sizeof(modules),&moduleBytes) && moduleBytes<=sizeof(modules)) {
    for(size_t i=0;i<moduleBytes/sizeof(HMODULE);++i) {
      wchar_t path[1024]{};
      if(GetModuleFileNameW(modules[i],path,1024))std::fprintf(stderr,"SELF_MODULE base=%p path=%ls\n",modules[i],path);
    }
  }
  struct Entry {HANDLE handle;SIZE_T refs,pointers;ULONG access,type,flags,reserved;};
  struct Header {ULONG_PTR count,reserved;};
  std::vector<uint8_t> data(1u<<20);ULONG used=0;
  if(query(GetCurrentProcess(),51,data.data(),ULONG(data.size()),&used)<0 || used>data.size() || used<sizeof(Header))return;
  Header head{};std::memcpy(&head,data.data(),sizeof(head));
  if(head.count>(used-sizeof(Header))/sizeof(Entry))return;
  for(size_t i=0;i<head.count;++i) {
    Entry e{};std::memcpy(&e,data.data()+sizeof(Header)+i*sizeof(Entry),sizeof(e));
    if(GetProcessIdOfThread(e.handle)!=GetCurrentProcessId())continue;
    void* start=nullptr;if(thread(e.handle,9,&start,sizeof(start),nullptr)<0)continue;
    MEMORY_BASIC_INFORMATION region{};wchar_t module[1024]{};
    if(VirtualQuery(start,&region,sizeof(region))==sizeof(region))
      GetModuleFileNameW(static_cast<HMODULE>(region.AllocationBase),module,1024);
    std::fprintf(stderr,"SELF_THREAD handle=%llu tid=%lu start=%p module=%ls\n",
      (unsigned long long)uintptr_t(e.handle),(unsigned long)GetThreadId(e.handle),start,module);
  }
}
class Thread final {
public:
  ~Thread() { if(h && WaitForSingleObject(h.get(),INFINITE)!=WAIT_OBJECT_0)std::terminate(); }
  void start(std::function<void()> work) {
    Check(!h);fn=std::move(work);
    h.reset(reinterpret_cast<HANDLE>(_beginthreadex(nullptr,0,Entry,this,0,nullptr)));Check(bool(h));
  }
  void join() { Check(h && WaitForSingleObject(h.get(),INFINITE)==WAIT_OBJECT_0);
    DWORD code=0;Check(GetExitCodeThread(h.get(),&code) && code==0);h.reset(); }
  bool joinable() const noexcept {return bool(h);}
private:
  static unsigned __stdcall Entry(void* p) noexcept {try {static_cast<Thread*>(p)->fn();return 0;}catch(...){return 9;}}
  lab::Handle h;std::function<void()> fn;
};
struct WireClient {
  HANDLE pipe,peer;ipc::Nonce nonce;lab::IoStats io;uint64_t sequence=0;
  std::array<uint8_t,128> exchange(slotwire::Op op,const uint8_t* data,uint32_t bytes,uint32_t expected) {
    Check(sequence<UINT64_MAX && expected<=128);
    slotwire::Header h{op,0,bytes,nonce,++sequence};slotwire::Packet packet{};size_t size=0;
    Check(slotwire::Encode(h,data,bytes,packet.data(),packet.size(),size)==slotwire::Error::None);
    lab::WriteBytes(pipe,peer,packet.data(),size,io);
    Check(slotwire::ReadPacket(pipe,peer,packet,size,io)==slotwire::Error::None);
    slotwire::View v;Check(slotwire::Decode(packet.data(),size,v)==slotwire::Error::None);
    Check(v.header.op==op && v.header.flags==1 && v.header.sequence==sequence && v.header.nonce==nonce && v.header.payloadBytes==expected);
    std::array<uint8_t,128> out{};if(expected)std::memcpy(out.data(),v.payload,expected);return out;
  }
  void echo(slotwire::Op op,const uint8_t* data=nullptr,uint32_t bytes=0) {
    const auto out=exchange(op,data,bytes,bytes);Check(!bytes || !std::memcmp(out.data(),data,bytes));
  }
};
struct Signals {
  std::atomic<bool> ready{false},go{false},slowGo{false},done{false},failed{false},abort{false},producerJoined{false};
  std::atomic<bool> triggerRequested{false},triggerApplied{false};
  std::atomic<uint64_t> acked{0};
  uint64_t requestQpc=0,appliedQpc=0,producerStart=0,producerEnd=0;
  uint64_t attempted=0,accepted=0,lost=0;
};
struct Result {
  uint64_t sent=0,acks=0,ackedEvents=0,trigger=rec::NoTrigger,creation=0;
  DWORD pid=0,exit=STILL_ACTIVE;bool fault=false,completed=false,settled=false;
};
bool Is(const std::wstring& m,const wchar_t* n){return m==n;}
template<typename Predicate> bool Wait(Signals& s,Predicate ready) {
  const auto until=GetTickCount64()+25000;
  while(!ready()) {
    if(s.done.load(std::memory_order_acquire) || s.failed.load(std::memory_order_acquire) || s.abort.load(std::memory_order_acquire))return false;
    Check(GetTickCount64()<until);Sleep(1);
  }
  return true;
}
bool DelayStarted(const std::wstring& path) {
  lab::Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
  if(!file)return false;
  char bytes[16384]{};DWORD count=0;Check(ReadFile(file.get(),bytes,sizeof(bytes)-1,&count,nullptr));
  return std::strstr(bytes,"\"delayStart\":")!=nullptr;
}
void Gone(ipc::Nonce n) {
  lab::Handle m(OpenFileMappingW(FILE_MAP_READ,FALSE,lab::SharedSlots::MappingName(n).c_str()));
  Check(!m && GetLastError()==ERROR_FILE_NOT_FOUND);
  for(uint32_t i=0;i<slots::SlotCount;++i) {
    lab::Handle h(OpenMutexW(SYNCHRONIZE,FALSE,lab::SharedSlots::MutexName(n,i).c_str()));
    Check(!h && GetLastError()==ERROR_FILE_NOT_FOUND);
  }
}
void Worker(const std::wstring& exe,const std::wstring& log,const std::wstring& mode,ipc::Nonce nonce,
    uint32_t capacity,rec::EventIngress<>& queue,Signals& s,Result& r) noexcept {
  auto fail=[&] {r.fault=true;s.failed.store(true,std::memory_order_release);queue.close();};
  try {
    lab::SharedSlots mapping(nonce,lab::MappingRole::OwnerWriter);
    auto args=lab::NonceText(nonce)+L" "+std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(lab::CreationTime(GetCurrentProcess()));
    if(!Is(mode,L"e2-to-p2"))args+=L" "+(Is(mode,L"slow")||Is(mode,L"disconnect")?mode:L"normal");
    lab::OwnedChild child(exe,args,log);r.pid=child.pid();r.creation=child.creation();
    try {
      auto pipe=lab::Connect(nonce,child.process(),child.pid(),lab::Channel::Slots1);
      WireClient wire{pipe.get(),child.process(),nonce,{}};
      const auto profile=Is(mode,L"p2-to-e2")?2ull:Is(mode,L"p3-to-e2")?6ull:10ull;
      const auto hello=slotwire::Hello(32,64,profile),expected=slotwire::Hello(64,32,profile);
      auto reply=wire.exchange(slotwire::Op::Hello,hello.data(),hello.size(),expected.size());
      Check(!std::memcmp(reply.data(),expected.data(),expected.size()));
      std::array<uint8_t,16> begin{};ipc::WriteLe(begin.data(),rec::fixture::Map,8);ipc::WriteLe(begin.data()+8,rec::fixture::Device,8);
      wire.echo(slotwire::Op::Begin,begin.data(),begin.size());
      std::array<uint8_t,rec::MaxPacketBytes> bytes{};
      auto send=[&](rec::Header h,const rec::Event* events,bool inject=false) {
        h.ordinal=r.sent+1;size_t count=0;
        Check(rec::Encode(h,events,bytes.data(),bytes.size(),count)==rec::WireError::None);
        if(inject) {
          if(Is(mode,L"wrong-ordinal"))ipc::WriteLe(bytes.data()+32,h.ordinal+1,8);
          if(Is(mode,L"wrong-session")) {
            ipc::WriteLe(bytes.data()+24,h.session+1,8);
            for(uint32_t i=0;i<h.count;++i)ipc::WriteLe(bytes.data()+rec::HeaderBytes+i*rec::EventBytes+8,h.session+1,8);
          }
          if(Is(mode,L"wrong-scope"))ipc::WriteLe(bytes.data()+rec::HeaderBytes+48,rec::fixture::Map+1,8);
          if(Is(mode,L"bad-kind"))ipc::WriteLe(bytes.data()+rec::HeaderBytes+68,0,4);
        }
        std::array<uint8_t,16> reserve{};ipc::WriteLe(reserve.data(),h.ordinal,8);ipc::WriteLe(reserve.data()+8,count,4);
        const auto reserved=wire.exchange(slotwire::Op::Reserve,reserve.data(),reserve.size(),slots::DescriptorBytes);
        slots::Key key;Check(slots::DecodeKey(reserved.data(),slots::DescriptorBytes,key)==slots::Error::None);
        Check(key.connection==nonce && key.map==rec::fixture::Map && key.device==rec::fixture::Device && key.frame==h.ordinal && key.bytes==count);
        mapping.writeOnce(key,bytes.data(),uint32_t(count),child.process());
        ++r.sent;const auto hash=lab::Sha256(bytes.data(),count);
        std::array<uint8_t,96> publish{};Check(slots::EncodeKey(key,publish.data(),publish.size())==slots::Error::None);
        std::memcpy(publish.data()+slots::DescriptorBytes,hash.data(),hash.size());
        wire.echo(slotwire::Op::Publish,publish.data(),publish.size());++r.acks;
        r.ackedEvents+=h.count;s.acked.store(r.ackedEvents,std::memory_order_release);
      };
      rec::Header h;h.session=rec::fixture::Session;h.capacity=capacity;send(h,nullptr);
      s.ready.store(true,std::memory_order_release);
      std::array<rec::Event,rec::MaxEvents> events{};bool first=true,finished=false;
      for(;;) {
        if(s.abort.load(std::memory_order_acquire))throw std::runtime_error("coordinator aborted");
        if(s.triggerRequested.load(std::memory_order_acquire) && !s.triggerApplied.load(std::memory_order_relaxed)) {
          r.trigger=queue.publicationCut();s.appliedQpc=Qpc();s.triggerApplied.store(true,std::memory_order_release);
        }
        uint32_t count=0;
        while(count<rec::MaxEvents) {
          const auto p=queue.tryPop(events[count]);
          if(p==rec::Pop::Item){++count;continue;}
          finished=p==rec::Pop::Finished;break;
        }
        if(count) {h.op=rec::Op::Data;h.count=count;h.trigger=r.trigger;send(h,events.data(),first);first=false;}
        if(finished)break;
        if(!count)Sleep(1);
      }
      Check(Wait(s,[&]{return s.producerJoined.load(std::memory_order_acquire);}));
      if(!Is(mode,L"missing-seal")) {
        h.op=rec::Op::Seal;h.count=0;h.trigger=r.trigger;h.accepted=s.accepted;h.attempted=s.attempted;h.lost=s.lost;h.reason=1;
        if(Is(mode,L"seal-totals")){++h.accepted;++h.attempted;}
        send(h,nullptr);
        if(Is(mode,L"late-data")) {
          auto e=rec::fixture::Make(s.accepted+1);h.op=rec::Op::Data;h.count=1;
          h.accepted=h.attempted=h.lost=h.reason=0;send(h,&e);
        }
      }
      wire.echo(slotwire::Op::End);wire.echo(slotwire::Op::Close);r.completed=true;
    } catch(...) {fail();}
    r.exit=child.wait(7000);r.settled=true;
  } catch(...) {fail();}
  s.done.store(true,std::memory_order_release);
}
void Producer(const std::wstring& mode,uint32_t capacity,rec::EventIngress<>& q,Signals& s) noexcept {
  try {
    if(!Wait(s,[&]{return s.go.load(std::memory_order_acquire);}) || !s.ready.load(std::memory_order_acquire)) {q.close();return;}
    auto push=[&] {
      const auto result=q.tryPush(rec::fixture::Session,rec::fixture::Make(s.accepted+1));
      if(result.outcome==rec::Push::Closed)return false;
      ++s.attempted;
      if(result.outcome==rec::Push::Published) {++s.accepted;Check(result.sequence==s.accepted);return true;}
      ++s.lost;return false;
    };
    auto batch=[&](uint64_t target) {
      while(s.accepted<target) {
        const auto end=std::min(target,s.accepted+rec::MaxEvents);
        while(s.accepted<end) {
          if(s.done.load(std::memory_order_acquire) || s.failed.load(std::memory_order_acquire))return false;
          if(!push()) {if(s.failed.load(std::memory_order_acquire))return false;Check(false);}
        }
        if(!Wait(s,[&]{return s.acked.load(std::memory_order_acquire)>=end;}))return false;
      }return true;
    };
    if(Is(mode,L"slow")) {
      for(unsigned i=0;i<rec::MaxEvents;++i)Check(push());
      if(Wait(s,[&]{return s.slowGo.load(std::memory_order_acquire);})) {
        s.producerStart=Qpc();for(unsigned i=0;i<20000;++i)push();s.producerEnd=Qpc();
        s.requestQpc=Qpc();s.triggerRequested.store(true,std::memory_order_release);
      }
    } else {
      s.producerStart=Qpc();
      const bool positive=Is(mode,L"normal")||Is(mode,L"small");
      const uint64_t cut=positive?capacity-capacity/4+rec::MaxEvents:640;
      if(batch(cut)) {
        s.requestQpc=Qpc();s.triggerRequested.store(true,std::memory_order_release);
        if(Wait(s,[&]{return s.triggerApplied.load(std::memory_order_acquire);}) && positive)batch(cut+capacity/4);
      }
      s.producerEnd=Qpc();
    }
  } catch(...) {s.abort.store(true,std::memory_order_release);}
  q.close();
}
void Run(const std::wstring& exe,const std::wstring& log,const std::wstring& mode) {
  ProbeOwnHandles("baseline");
  DWORD handlesBefore=0,handlesAfter=0;Check(GetProcessHandleCount(GetCurrentProcess(),&handlesBefore));
  const auto before=lab::ProbeOwnProcessMemory();
  ProbeOwnHandles("after-memory-before");
  const auto nonce=lab::RandomNonce();const uint32_t capacity=Is(mode,L"normal")?rec::MaxHistoryEvents:8192;
  Signals s;Result r;rec::IngressCounters counters;lab::ProcessMemory active;
  {
    auto queue=std::make_unique<rec::EventIngress<>>(rec::fixture::Session);
    Thread io,producer;
    io.start([&]{Worker(exe,log,mode,nonce,capacity,*queue,s,r);});
    try {
      Wait(s,[&]{return s.ready.load(std::memory_order_acquire);});active=lab::ProbeOwnProcessMemory();
      producer.start([&]{Producer(mode,capacity,*queue,s);});s.go.store(true,std::memory_order_release);
      if(Is(mode,L"slow") && Wait(s,[&]{return DelayStarted(log);}))s.slowGo.store(true,std::memory_order_release);
      producer.join();s.producerJoined.store(true,std::memory_order_release);io.join();
    } catch(...) {
      s.abort.store(true,std::memory_order_release);queue->close();
      if(producer.joinable())producer.join();
      s.producerJoined.store(true,std::memory_order_release);
      if(io.joinable())io.join();
      throw;
    }
    counters=queue->countersAfterJoin();
    Check(!s.abort.load() && counters.attempted==s.attempted && counters.accepted==s.accepted && counters.lost==s.lost);
  }
  Gone(nonce);Check(GetProcessHandleCount(GetCurrentProcess(),&handlesAfter));
  ProbeOwnHandles("after-worker");
  ThreadOrigins();
  const auto after=lab::ProbeOwnProcessMemory();
  std::printf("{\"mode\":\"%ls\",\"bits\":32,\"nonce\":\"%ls\",\"map\":11,\"device\":17,\"session\":%llu,\"capacity\":%u,"
    "\"attempted\":%llu,\"accepted\":%llu,\"lost\":%llu,\"popped\":%llu,\"queued\":%llu,\"ackedEvents\":%llu,\"sentPackets\":%llu,\"ackPackets\":%llu,\"trigger\":%llu,"
    "\"triggerRequestedQpc\":%llu,\"triggerAppliedQpc\":%llu,\"qpcFrequency\":%llu,\"producerStartQpc\":%llu,\"producerEndQpc\":%llu,"
    "\"workerJoined\":true,\"hostSettled\":%s,\"containersGone\":true,\"hostPid\":%lu,\"hostCreation\":%llu,\"hostExit\":%lu,\"fault\":%s,\"completed\":%s,"
    "\"ingressBytes\":%u,\"poolBytes\":%u,\"handlesBefore\":%lu,\"handlesAfter\":%lu,\"memoryBefore\":",
    mode.c_str(),lab::NonceText(nonce).c_str(),(unsigned long long)rec::fixture::Session,capacity,
    (unsigned long long)counters.attempted,(unsigned long long)counters.accepted,(unsigned long long)counters.lost,
    (unsigned long long)counters.popped,(unsigned long long)counters.queued,(unsigned long long)r.ackedEvents,
    (unsigned long long)r.sent,(unsigned long long)r.acks,(unsigned long long)r.trigger,
    (unsigned long long)s.requestQpc,(unsigned long long)s.appliedQpc,(unsigned long long)Frequency(),
    (unsigned long long)s.producerStart,(unsigned long long)s.producerEnd,r.settled?"true":"false",
    (unsigned long)r.pid,(unsigned long long)r.creation,(unsigned long)r.exit,r.fault?"true":"false",r.completed?"true":"false",
    unsigned(sizeof(rec::EventIngress<>)),slots::PoolBytes,(unsigned long)handlesBefore,(unsigned long)handlesAfter);
  lab::PrintProcessMemory(before);std::printf(",\"memoryActive\":");lab::PrintProcessMemory(active);
  std::printf(",\"memoryAfter\":");lab::PrintProcessMemory(after);std::printf("}\n");std::fflush(stdout);
  Check(handlesBefore==handlesAfter);
}
}
int wmain(int argc,wchar_t** argv) {
  if(argc==2 && !std::wcscmp(argv[1],L"--dependency-init"))return 0;
  try {
    Check(argc==4);const std::wstring mode=argv[2];bool known=false;
    for(auto n:{L"normal",L"small",L"slow",L"disconnect",L"missing-seal",L"late-data",L"wrong-ordinal",L"wrong-session",L"wrong-scope",L"bad-kind",L"seal-totals",L"p2-to-e2",L"p3-to-e2",L"e2-to-p2",L"e2-to-p3"})known|=mode==n;
    Check(known);
    // Cold dependency initialization is separately recorded, not a discarded
    // protocol run or a warm game process. No history/IPC application begins.
    for(unsigned i=0;i<8;++i){lab::RandomNonce();lab::Sha256(reinterpret_cast<const uint8_t*>("a"),1);}
    {const auto n=lab::RandomNonce();auto p=lab::CreateServer(n,lab::Channel::Slots1);
      lab::SharedSlots w(n,lab::MappingRole::OwnerWriter),r(n,lab::MappingRole::Reader);}
    Thread warm;warm.start([]{for(unsigned i=0;i<72;++i)try{throw std::runtime_error("init");}catch(const std::exception&) {}});warm.join();
    for(unsigned i=0;i<2;++i) {
      lab::OwnedChild c(argv[1],L"",std::wstring(argv[3])+L".init-"+std::to_wstring(i));
      const auto code=c.wait(7000);Check(code==(Is(mode,L"e2-to-p2")?2u:3u));
      std::printf("{\"initialization\":true,\"pid\":%lu,\"creation\":%llu,\"exit\":%lu}\n",(unsigned long)c.pid(),(unsigned long long)c.creation(),(unsigned long)code);
    }
    if(std::getenv("WARVK_HOST_LAB_IDLE_PROBE")) {
      // Diagnostic control only: identical dependency startup, no recorder,
      // queue, worker or live application connection during this interval.
      ProbeOwnHandles("baseline");ThreadOrigins();Sleep(55000);
      ProbeOwnHandles("idle-after");ThreadOrigins();return 0;
    }
    // This machine also adds a system thread/event during an otherwise idle
    // 55-second process. Keep that observed startup interval outside the long
    // transaction measurement, disclose it, and still require exact equality
    // around the real transaction. Time alone is NOT cleanup/completion proof.
    DWORD startupBefore=0,startupAfter=0;Check(GetProcessHandleCount(GetCurrentProcess(),&startupBefore));
    const auto startupStart=Qpc();const DWORD startupMs=Is(mode,L"normal")?55000:0;
    if(startupMs)Sleep(startupMs);
    Check(GetProcessHandleCount(GetCurrentProcess(),&startupAfter));
    std::printf("{\"environmentWindow\":true,\"milliseconds\":%lu,\"startQpc\":%llu,\"endQpc\":%llu,\"frequency\":%llu,\"handlesBefore\":%lu,\"handlesAfter\":%lu}\n",
      (unsigned long)startupMs,(unsigned long long)startupStart,(unsigned long long)Qpc(),(unsigned long long)Frequency(),
      (unsigned long)startupBefore,(unsigned long)startupAfter);
    std::fflush(stdout);
    Run(argv[1],argv[3],mode);return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 2;}catch(...){return 3;}
}
