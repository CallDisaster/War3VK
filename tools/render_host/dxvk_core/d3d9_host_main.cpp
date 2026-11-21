#include "d3d9_backend.h"
#include "d3d9_factory.h"
#include "host_surface_target.h"
#include "../d3d9_draw_wire.h"
#include "../d3d9_annotation_wire.h"
#include "../d3d9_content_wire.h"
#include "../d3d9_declaration_wire.h"
#include "../d3d9_ff_delta_wire.h"
#include "../d3d9_ff_fast_decode.h"
#include "../d3d9_shader_tokens.h"
#include "../d3d9_window_identity.h"
#include "../win32_transport.h"
#include "../d3d9_ring_channel.h"
#include "../d3d9_sync_profile.h"
#include "../../../src/util/log/log.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cwchar>
#include <limits>
#include <string>

namespace dxvk { Logger Logger::s_instance("warvk-d3d9-host.log"); }
using namespace warvk::host;
using GPU=dxvkcore::D3D9Backend;
static_assert(sizeof(void*)==8,"D3D9 DXVK execution owner must be x64");
namespace {
void Require(bool c,const char* r){if(!c)throw std::runtime_error(r);}
void ExitMilestone(const char* phase,ULONGLONG processStart) noexcept {
  // Cold-path diagnostics only. Use the inherited file handle directly, so a
  // CRT stream lock cannot hide which teardown boundary was last reached.
  char line[256]{};
  const int count=std::snprintf(line,sizeof(line),
    "D3D9 host exit-milestone pid=%lu tid=%lu phase=%s tickMs=%llu processElapsedMs=%llu\n",
    GetCurrentProcessId(),GetCurrentThreadId(),phase,GetTickCount64(),GetTickCount64()-processStart);
  if(count>0&&size_t(count)<sizeof(line)) {
    DWORD written=0;WriteFile(GetStdHandle(STD_ERROR_HANDLE),line,DWORD(count),&written,nullptr);
  }
}
// Exit-delay measurement (docs/research/2026-11-17-x64-host-exit-delay.md):
// the boundaries after wmain returns. Kernel32 calls and a stack buffer only,
// no CRT, heap or lock, so it is usable from a static destructor and from the
// loader's process-detach callback. Each line is one WriteFile to the
// inherited log handle; a later line proves the earlier write returned.
void RawExitMilestone(const char* phase) noexcept {
  char line[192];size_t n=0;
  const auto text=[&](const char* s){while(*s&&n+1<sizeof(line))line[n++]=*s++;};
  const auto number=[&](unsigned long long v){
    char digits[24];size_t d=0;do{digits[d++]=char('0'+v%10);v/=10;}while(v&&d<sizeof(digits));
    while(d&&n+1<sizeof(line))line[n++]=digits[--d];
  };
  LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);
  text("D3D9 host exit-milestone pid=");number(GetCurrentProcessId());
  text(" tid=");number(GetCurrentThreadId());text(" phase=");text(phase);
  text(" tickMs=");number(GetTickCount64());text(" qpc=");number(static_cast<unsigned long long>(qpc.QuadPart));text("\n");
  DWORD written=0;WriteFile(GetStdHandle(STD_ERROR_HANDLE),line,DWORD(n),&written,nullptr);
}
struct MainExitWitness {
  ULONGLONG start;
  ~MainExitWitness(){ExitMilestone("wmain-locals-complete",start);RawExitMilestone("wmain-locals-write-returned");}
};
// Constructed first and destroyed last of the C++ static objects.
struct StaticExitWitness { ~StaticExitWitness(){RawExitMilestone("static-destructors-last");} };
__attribute__((init_priority(101))) StaticExitWitness staticExitWitness;
// The executable's own TLS callback: DLL_PROCESS_DETACH runs inside the
// loader's process shutdown (after the CRT's exit, with the loader lock held).
void NTAPI ExitTlsCallback(PVOID,DWORD reason,PVOID) {
  if(reason==DLL_PROCESS_DETACH)RawExitMilestone("loader-process-detach");
}
__attribute__((section(".CRT$XLY"),used)) const PIMAGE_TLS_CALLBACK warvkExitTlsCallback=ExitTlsCallback;
// Exit-delay switches (docs/research/2026-11-17-x64-host-exit-delay.md), on by
// default since the 2026-11-18 review; `off` names the value that keeps the
// earlier behaviour, for a control arm. Each is read once, at its use.
bool ExitSwitch(const wchar_t* name,const wchar_t* off) noexcept {
  wchar_t v[16]{};const DWORD n=GetEnvironmentVariableW(name,v,16);
  return !(n&&n<16&&!std::wcscmp(v,off));
}
// WARVK_D3D9_HOST_EXIT_BUFFERED (off: "0"): the exit accounts are formatted in memory
// and written a few whole lines at a time, instead of one stderr write per
// fprintf (about 800 for the shadow-host line alone). Same bytes, same order;
// if memory cannot be had the rest goes straight to stderr. What is held is
// written once it passes HoldBytes at the end of a line, so a fault during the
// accounts loses at most the lines since then, and when it goes out of scope.
// `complete` turns false when stderr did not take every byte.
struct ExitText {
  static constexpr size_t HoldBytes=32768;
  bool buffered=false,complete=true;std::string text;
  ExitText()=default;
  ExitText(const ExitText&)=delete;ExitText& operator=(const ExitText&)=delete;
  ~ExitText(){flush();}
#if defined(__GNUC__)||defined(__clang__)
  __attribute__((format(printf,2,3)))
#endif
  void add(const char* format,...) noexcept {
    va_list args;va_start(args,format);
    if(buffered) {
      va_list size;va_copy(size,args);
      const int n=std::vsnprintf(nullptr,0,format,size);va_end(size);
      const size_t at=text.size();
      bool room=n>=0;
      if(room)try{text.resize(at+size_t(n)+1);}catch(...){room=false;}
      if(room) {
        std::vsnprintf(&text[at],size_t(n)+1,format,args);text.resize(at+size_t(n));va_end(args);
        if(text.size()>=HoldBytes&&text.back()=='\n')flush();
        return;
      }
      flush();buffered=false;
    }
    if(std::vfprintf(stderr,format,args)<0)complete=false;
    va_end(args);
  }
  void flush() noexcept {
    if(text.empty())return;
    size_t done=0;
    while(done<text.size()) {
      const size_t n=std::fwrite(text.data()+done,1,text.size()-done,stderr);
      if(!n)break;
      done+=n;
    }
    if(done!=text.size()||std::fflush(stderr)!=0)complete=false;
    text.clear();
  }
};
// WARVK_D3D9_HOST_EXIT_GL_WATCHER (off: "keep"): after the normal teardown and the
// final accounts, while every thread of this process still runs, ask
// opengl32's "Palette Watcher" thread to end the way opengl32 itself does
// (WM_CLOSE to its window; bounded wait). opengl32 starts that thread when a
// pixel format is set on a window of another process (here the game's
// window); left running into ExitProcess it is killed before the loader's
// detach, whose poll for it can then sleep 100 x 50 ms. Nothing is skipped:
// if the window is absent or the thread does not end, exit is as before.
void ClosePaletteWatcher(ULONGLONG processStart) noexcept {
  struct Found { DWORD pid=0;HWND window=nullptr;DWORD thread=0;unsigned windows=0; } found;
  found.pid=GetCurrentProcessId();
  const auto begin=GetTickCount64();
  EnumWindows([](HWND window,LPARAM context)->BOOL {
    auto& f=*reinterpret_cast<Found*>(context);
    DWORD pid=0;const DWORD thread=GetWindowThreadProcessId(window,&pid);
    wchar_t name[32]{};
    if(pid==f.pid&&GetClassNameW(window,name,32)>0&&!std::wcscmp(name,L"Palette Watcher")) {
      ++f.windows;if(!f.window){f.window=window;f.thread=thread;}
    }
    return TRUE;
  },reinterpret_cast<LPARAM>(&found));
  BOOL posted=FALSE;DWORD waited=WAIT_FAILED;
  // Exactly one such window, its thread held open, and the window looked at
  // again right before the message: anything else leaves the exit as it was.
  if(found.windows==1) {
    if(const HANDLE thread=OpenThread(SYNCHRONIZE,FALSE,found.thread)) {
      DWORD pid=0;wchar_t name[32]{};
      const bool same=GetWindowThreadProcessId(found.window,&pid)==found.thread&&pid==found.pid
        &&GetClassNameW(found.window,name,32)>0&&!std::wcscmp(name,L"Palette Watcher");
      if(same)posted=PostMessageW(found.window,WM_CLOSE,0,0);
      if(posted)waited=WaitForSingleObject(thread,500);
      CloseHandle(thread);
    }
  }
  std::fprintf(stderr,"D3D9 host exit gl-watcher windows=%u thread=%lu posted=%d waited=%lu elapsedMs=%llu processElapsedMs=%llu\n",
    found.windows,found.thread,int(posted),waited,GetTickCount64()-begin,GetTickCount64()-processStart);
}
void LogExitIntent(const char* strategy,DWORD code,ULONGLONG processStart) noexcept {
  std::fprintf(stderr,"D3D9 host exit-intent pid=%lu strategy=%s code=%lu processElapsedMs=%llu\n",
    GetCurrentProcessId(),strategy,code,GetTickCount64()-processStart);
  std::fflush(stdout);std::fflush(stderr);
  ExitMilestone("exit-intent-flushed",processStart);
}
[[noreturn]] void SelfFailStop(ULONGLONG processStart) noexcept {
  LogExitIntent("self-fail-stop",4,processStart);
  // Never run process-detach callbacks while an uncertain GPU/driver owner is
  // deliberately retained. This handle can only terminate this host itself.
  const BOOL requested=TerminateProcess(GetCurrentProcess(),4);
  const DWORD error=requested?ERROR_SUCCESS:GetLastError();
  // Successful self-termination does not return. If it unexpectedly does, or
  // the request fails, abort is the final fail-stop fallback, not normal exit.
  std::fprintf(stderr,"D3D9 host self-fail-stop unexpectedly returned requested=%s win32=%lu; abort fallback\n",
    requested?"true":"false",error);std::fflush(stderr);
  std::abort();
}
struct Entry {
  d9::ResourceRef ref{};
  d9::Type type=d9::Type::None;
  uint32_t slot=0,size=0,width=0,height=0,initializedPrefix=0;
  bool uploaded=false,dynamic=false;
  uint64_t queryRevision=0,querySerial=0;
  d9::TextureLayout textureLayout;
  uint32_t textureRevision=0,initializedLevels=0;
  // RetainContent only: vertex-buffer bytes or the packed texture layout.
  std::vector<uint8_t> content;
};
struct HandleStats {
  uint64_t logicalHandlesCreated=0,logicalHandlesDestroyed=0,logicalHandlesReused=0;
  // resourcesReused counts the six GPU author-resource types, not Query handles.
  uint64_t resourcesReused=0,queryHandlesCreated=0,queryHandlesDestroyed=0,queryHandlesReused=0;
  uint64_t queryIssues=0,queryPolls=0,queryWaits=0;
  uint32_t liveLogicalHandles=0,liveQueries=0;
  void print() const {
    std::printf("\"logicalHandlesCreated\":%llu,\"logicalHandlesDestroyed\":%llu,\"logicalHandlesReused\":%llu,"
      "\"resourcesReused\":%llu,\"queryHandlesCreated\":%llu,\"queryHandlesDestroyed\":%llu,\"queryHandlesReused\":%llu,"
      "\"queryIssues\":%llu,\"queryPolls\":%llu,\"queryWaits\":%llu,\"liveLogicalHandles\":%u,\"liveQueries\":%u,",
      logicalHandlesCreated,logicalHandlesDestroyed,logicalHandlesReused,resourcesReused,
      queryHandlesCreated,queryHandlesDestroyed,queryHandlesReused,queryIssues,queryPolls,queryWaits,
      liveLogicalHandles,liveQueries);
  }
};
using Clock=std::chrono::steady_clock;
uint64_t ElapsedNs(Clock::time_point begin) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count());
}
// Opt-in admission account (WARVK_D3D9_ADMISSION_PROFILE=1), cumulative CPU
// wall time. Disabled sessions take no clock reads. Samples are cut only after
// an accepted EVENT Issue, never per message or draw; full is counted dropped.
struct AdmissionCounters {
  uint64_t messages=0,idleNs=0,readNs=0,replyNs=0;
  uint64_t batchMessages=0,batchDraws=0,batchReadNs=0,batchDecodeNs=0,batchApplyNs=0,batchReplyNs=0;
  uint64_t prepareNs=0,recordNs=0,constantNs=0,bindNs=0,samplerNs=0,drawNs=0;
  uint64_t flushApplyNs=0,waitEventApplyNs=0,otherApplyNs=0;
};
struct AdmissionSample {
  uint64_t query=0;
  AdmissionCounters cumulative;
};
// P0 measurement only (warvk-d3d9-host-profile/v1): per wire op the count and
// CPU wall time of its apply; the time spent waiting for the next record; the
// backend's waits and submissions by call site (GPU::WaitProfile). Cumulative,
// QueryPerformanceCounter ticks, fixed tables. One line on stderr (host.log)
// every WARVK_D3D9_SYNC_PROFILE_EVERY Present messages and one at a normal
// exit. The snapshot is written after the Present reply is published.
struct HostProfile {
  int64_t frequency=d9::profile::Frequency(),start=d9::profile::Clock();
  uint64_t every=d9::profile::Every(),presents=0;
  std::array<uint64_t,d9::profile::Ops> count{},ticks{},maxTicks{};
  std::array<std::array<uint64_t,3>,d9::profile::Ops> slow{}; // applies of at least 1 ms, 5 ms, 20 ms
  const uint64_t slowTicks[3]={uint64_t(frequency)/1000u,uint64_t(frequency)/200u,uint64_t(frequency)/50u};
  uint64_t idleTicks=0,applyTicks=0,writeTicks=0;
  // Lost-wakeup measurement: how each park ended, and the last synchronous
  // reply (when, which op, whether it signalled `progress`).
  d9::profile::WakeLog wake;
  int64_t lastReply=0;uint64_t lastReplySequence=0;uint32_t lastReplyOp=0,lastReplySignaled=0;
  uint32_t loopSleeps=0,loopTimeouts=0,loopStatus=0;
  std::array<char,40960> text{};
  void applied(d9::Op op,int64_t begin,int64_t end) noexcept {
    const auto index=d9::profile::OpIndex(op);const auto t=uint64_t(end-begin);
    ++count[index];ticks[index]+=t;maxTicks[index]=std::max(maxTicks[index],t);applyTicks+=t;
    for(size_t i=0;i<3;++i)slow[index][i]+=t>=slowTicks[i];
    if(op==d9::Op::Present)++presents;
  }
  // frameScenes/worldSceneEnters: the annotation channel's frame scenes and
  // world-scene entries (in-game frames), as annotation-host/v1 prints them.
  void snapshot(const char* reason,const GPU::Stats& s,uint64_t records,uint64_t syncRecords,uint64_t parks,
      uint64_t wakes,uint64_t frameScenes,uint64_t worldSceneEnters) noexcept {
    using namespace d9::profile;
    const auto now=d9::profile::Clock();
    const auto us=[this](uint64_t t){return static_cast<unsigned long long>(Micros(t,frequency));};
    Line line(text.data(),text.size());
    line.add("D3D9 host-profile: {\"schema\":\"warvk-d3d9-host-profile/v1\",\"reason\":\"%s\",\"pid\":%lu,"
      "\"every\":%llu,\"timing\":%s,\"presents\":%llu,\"seconds\":%.6f,\"qpc\":%lld,\"qpcFrequency\":%lld,\"unixMs\":%llu,"
      "\"idleUs\":%llu,\"applyUs\":%llu,\"profileWriteUs\":%llu,\"ringRecords\":%llu,\"ringSyncRecords\":%llu,"
      "\"ringParks\":%llu,\"ringWakes\":%llu,\"submitted\":%llu,\"gpuCompleted\":%llu,\"draws\":%llu,"
      "\"presentApiSucceeded\":%llu,\"syncInterval\":%u,\"streamMode\":%s,\"frameScenes\":%llu,"
      "\"worldSceneEnters\":%llu,\"ops\":[",
      reason,GetCurrentProcessId(),static_cast<unsigned long long>(every),TimingEnabled()?"true":"false",
      static_cast<unsigned long long>(presents),double(now-start)/double(frequency),static_cast<long long>(now),static_cast<long long>(frequency),
      static_cast<unsigned long long>(UnixMs()),us(idleTicks),us(applyTicks),us(writeTicks),
      static_cast<unsigned long long>(records),static_cast<unsigned long long>(syncRecords),
      static_cast<unsigned long long>(parks),static_cast<unsigned long long>(wakes),
      static_cast<unsigned long long>(s.submitted),static_cast<unsigned long long>(s.gpuCompleted),
      static_cast<unsigned long long>(s.draws),static_cast<unsigned long long>(s.presentApiSucceeded),
      s.syncInterval,s.streamMode?"true":"false",static_cast<unsigned long long>(frameScenes),
      static_cast<unsigned long long>(worldSceneEnters));
    bool first=true;
    for(size_t i=1;i<Ops;++i) {
      if(!count[i])continue;
      line.add("%s{\"op\":%u,\"name\":\"%s\",\"count\":%llu,\"us\":%llu,\"maxUs\":%llu,\"slow\":[%llu,%llu,%llu]}",
        first?"":",",unsigned(i),OpName(i),static_cast<unsigned long long>(count[i]),us(ticks[i]),us(maxTicks[i]),
        static_cast<unsigned long long>(slow[i][0]),static_cast<unsigned long long>(slow[i][1]),
        static_cast<unsigned long long>(slow[i][2]));
      first=false;
    }
    line.close("],\"waits\":[");first=true;
    const auto& w=s.waitProfile;
    for(size_t site=0;site<w.waits.size();++site)for(size_t kind=0;kind<w.waits[site].size();++kind) {
      const auto& c=w.waits[site][kind];
      if(!c.count)continue;
      line.add("%s{\"site\":\"%s\",\"kind\":\"%s\",\"count\":%llu,\"us\":%llu,\"maxUs\":%llu,\"polls\":%llu,"
        "\"slow\":[%llu,%llu,%llu]}",
        first?"":",",GPU::WaitSiteName(site),GPU::WaitKindName(kind),static_cast<unsigned long long>(c.count),
        us(c.ticks),us(c.maxTicks),static_cast<unsigned long long>(c.polls),
        static_cast<unsigned long long>(c.slow[0]),static_cast<unsigned long long>(c.slow[1]),
        static_cast<unsigned long long>(c.slow[2]));
      first=false;
    }
    line.close("],\"submits\":[");first=true;
    for(size_t site=0;site<w.submits.size();++site) {
      if(!w.submits[site])continue;
      line.add("%s{\"site\":\"%s\",\"submits\":%llu,\"waitForGpu\":%llu}",first?"":",",GPU::WaitSiteName(site),
        static_cast<unsigned long long>(w.submits[site]),static_cast<unsigned long long>(w.gpuSubmits[site]));
      first=false;
    }
    line.close("]");
    {
      // E0: per present-wait mode (0 poll, 1 block); us for the tick fields.
      const auto& p=s.presentWait;
      line.add(",\"presentPipeline\":{\"setting\":%u,\"presents\":%llu,\"clears\":%llu,\"drains\":%llu}",s.presentPipelineSetting,
        static_cast<unsigned long long>(s.presentsPipelined),static_cast<unsigned long long>(s.clearsPipelined),
        static_cast<unsigned long long>(s.presentPipelineDrains));
      line.add(",\"presentWait\":{\"setting\":%u,\"every\":%u,\"modes\":[",p.setting,p.every);
      for(size_t m=0;m<2;++m) {
        const auto& c=p.modes[m];
        line.add("%s{\"presents\":%llu,\"queueUs\":%llu,\"queueSleeps\":%llu,\"retireUs\":%llu,\"retireSleeps\":%llu,"
          "\"intervalUs\":%llu,\"intervals\":%llu,\"intervalMs\":[",m?",":"",static_cast<unsigned long long>(c[0]),us(c[1]),
          static_cast<unsigned long long>(c[2]),us(c[3]),static_cast<unsigned long long>(c[4]),us(c[5]),
          static_cast<unsigned long long>(c[6]));
        for(size_t b=0;b<p.intervalMs[m].size();++b)line.add("%s%u",b?",":"",p.intervalMs[m][b]);
        line.close("]}");
      }
      line.close("]}");
    }
    {
      // Item 4: dynamic storage and its CPU reads by source (0 mapping, 1 CPU copy).
      const auto& d=s.dynamicRead;const auto n=[](uint64_t v){return static_cast<unsigned long long>(v);};
      line.add(",\"dynamicRead\":{\"setting\":%u,\"every\":%u,\"flagsSeen\":%u,\"created\":%llu,\"createdDeviceLocal\":%llu,"
        "\"createdCached\":%llu,\"liveBytes\":%llu,\"peakBytes\":%llu,\"copyBytes\":%llu,\"peakCopyBytes\":%llu,\"modes\":["
        "[%llu,%llu,%llu,%llu,%llu,%llu],[%llu,%llu,%llu,%llu,%llu,%llu]]}",d.setting,d.every,d.flagsSeen,n(d.created),
        n(d.createdDeviceLocal),n(d.createdCached),n(d.liveBytes),n(d.peakBytes),n(d.copyBytes),n(d.peakCopyBytes),
        n(d.modes[0][0]),n(d.modes[0][1]),us(d.modes[0][2]),n(d.modes[0][3]),us(d.modes[0][4]),n(d.modes[0][5]),
        n(d.modes[1][0]),n(d.modes[1][1]),us(d.modes[1][2]),n(d.modes[1][3]),us(d.modes[1][4]),n(d.modes[1][5]));
      // P1 batch 1: staging slices (mode 0 settle/submit, 1 slices).
      const auto& u=s.uploadSlices;
      line.add(",\"uploadSlices\":{\"setting\":%u,\"every\":%u,\"recorded\":%llu,\"recordedBytes\":%llu,\"listFlushes\":%llu,"
        "\"creditWaits\":%llu,\"creditWaitUs\":%llu,\"peakInFlightBytes\":%llu,\"peakInFlightSlices\":%llu,"
        "\"modes\":[[%llu,%llu],[%llu,%llu]]}",u.setting,u.every,n(u.recorded),n(u.recordedBytes),n(u.listFlushes),
        n(u.creditWaits),us(u.creditWaitTicks),n(u.peakInFlightBytes),n(u.peakInFlightSlices),
        n(u.modes[0][0]),us(u.modes[0][1]),n(u.modes[1][0]),us(u.modes[1][1]));
      // Present-to-Present intervals by A/B phase (1 E0, 2 item 4, 3 P1; 0 none).
      line.add(",\"ab\":{\"switch\":%u,\"frames\":[%llu,%llu],\"intervalUs\":[%llu,%llu],\"intervalMs\":[",
        s.abPhaseSwitch,n(s.abFrames[0]),n(s.abFrames[1]),us(s.abIntervalTicks[0]),us(s.abIntervalTicks[1]));
      for(size_t m=0;m<2;++m) {
        line.add("%s[",m?",":"");
        for(size_t b=0;b<s.abIntervalMs[m].size();++b)line.add("%s%u",b?",":"",s.abIntervalMs[m][b]);
        line.close("]");
      }
      line.close("]}");
    }
    wake.print(line,frequency);
    line.close(",\"truncated\":%s}\n",line.truncated()?"true":"false");
    line.write(stderr);
    if(TimingEnabled())writeTicks+=uint64_t(d9::profile::Clock()-now);
  }
};
struct Session {
  explicit Session(DWORD peer,ipc::Nonce nonce,HANDLE process=nullptr,surface::Mode mode=surface::Mode::Peer)
    :peerPid(peer),peerProcess(process),surfaceMode(mode),marker(d9::WindowMarker(nonce)){}
  DWORD peerPid;
  HANDLE peerProcess; // Borrowed from wmain OpenPeer; owned-target start duplicates it.
  surface::Mode surfaceMode;
  std::wstring marker;
  // Allocate no GPU owner before the entire initialization preflight accepts.
  std::unique_ptr<dxvkcore::D3D9Factory> factory;
  std::unique_ptr<GPU> gpu;
  GPU::Stats uninitializedStats{};
  const GPU::Stats& stats() const { return gpu?gpu->stats():uninitializedStats; }
  std::array<Entry,d9::MaxResources+1> entries{};
  // One past the highest id an entry was ever written for: what the scans
  // below cover, instead of the whole bound.
  uint32_t entryLimit=1;
  std::vector<uint8_t> slotScratch; // slot(): which typed slots are taken
  d9::ResourceRegistry registry;
  d9::TextureUpload textureUpload;
  d9::TextureStages textureStages;
  HandleStats handles;
  uint64_t accepted=0,rejected=0,backendCalls=0;
  // An asynchronous scene record (FrameScene / StageMarker) was refused: x86
  // is told through the ring block and sends none once it has read that; the
  // ones already on their way are ignored, as if they had not been sent.
  bool sceneStopped=false;uint64_t sceneRefusals=0,sceneRecordsIgnored=0;
  // Both fixed batch encodings share the existing accepted-batch account.
  uint64_t fixedBatches=0,fixedBatchDraws=0;uint32_t largestFixedBatch=0;
  uint32_t asyncResources=0,textureSlots=GPU::TextureSlots;
  uint64_t asyncDestroys=0,asyncTextureCreates=0,unnegotiatedResources=0;
  bool admitAsyncResource(d9::Op op,uint32_t feature) {
    if(op==d9::Op::Destroy)++asyncDestroys;else ++asyncTextureCreates;
    if(asyncResources&feature)return true;
    ++unnegotiatedResources;return false;
  }
  void reportAsyncResources(ExitText& out) const {
    if(asyncResources)
      out.add("D3D9 async-resource-host: {\"schema\":\"warvk-d3d9-async-resource-host/v1\","
        "\"asyncDestroy\":%s,\"asyncCreateTexture\":%s,\"destroyReceived\":%llu,\"createTextureReceived\":%llu,"
        "\"unnegotiatedRejected\":%llu,\"logicalCreated\":%llu,\"logicalDestroyed\":%llu,\"texturesCreated\":%llu,"
        "\"liveResources\":%u}\n",asyncResources&d9::InitializeAsyncDestroy?"true":"false",
        asyncResources&d9::InitializeAsyncCreateTexture?"true":"false",asyncDestroys,asyncTextureCreates,unnegotiatedResources,
        handles.logicalHandlesCreated,handles.logicalHandlesDestroyed,stats().texturesCreated,stats().liveResources);
  }
  void reportWater(ExitText& out) const {
    const auto& w = stats().water;
    using R = GPU::WaterFallback;
    const auto n = [&](R reason) { return w.fallbacks[size_t(reason)]; };
    out.add("D3D9 in-place water: draws=%llu snapshots=%llu skippedOldPasses=%llu "
      "fallbacks={msaa:%llu,format:%llu,snapshot:%llu,drawState:%llu,projection:%llu,resources:%llu,pipeline:%llu} "
      "gpu=%s gpuSamples=%llu gpuMeanUs=%.2f gpuMaxUs=%.2f\n",
      w.draws, w.snapshots, w.skippedOldPasses, n(R::Msaa), n(R::Format), n(R::Snapshot), n(R::DrawState),
      n(R::Projection), n(R::Resources), n(R::Pipeline), w.gpuSamples ? "sampled" : "unavailable",
      w.gpuSamples, w.gpuSamples ? w.gpuTotalUs / double(w.gpuSamples) : 0.0, w.gpuMaxUs);
  }
  bool fixedDelta=false;
  uint64_t deltaMessages=0,deltaRecords=0,deltaRejected=0,deltaVerified=0;
  void reportFixedDelta(ExitText& out) const {
    if(fixedDelta||deltaMessages)
      out.add("D3D9 fixed-delta-host: {\"schema\":\"warvk-d3d9-fixed-delta-host/v1\","
        "\"messages\":%llu,\"records\":%llu,\"restoredBytes\":%llu,\"rejected\":%llu,\"verified\":%llu}\n",
        deltaMessages,deltaRecords,deltaRecords*d9::FixedDrawBytes,deltaRejected,deltaVerified);
  }
  // Session-owned decode targets (about 60 KiB), never per-message stack copies.
  std::array<GPU::DrawState,d9::MaxFixedDrawBatch> fixedBatchStates{};
  std::array<d9::FixedFunctionState,d9::MaxFixedDrawBatch> fixedBatchFixed{};
  // WARVK_D3D9_FAST_DECODE (off: "0").
  d9::FixedDrawDecoder fixedDecoder;
  const bool fastDecode=ExitSwitch(L"WARVK_D3D9_FAST_DECODE",L"0");
  bool initialized=false,finished=false;
  bool presentedReadback=false;
  // A captured diagnostic frame at the current extent; Reset drops it.
  bool presentedCurrent=false;
  uint32_t targetWidth=0,targetHeight=0; // Negotiated at Initialize, changed by Reset.
  HWND presenterWindow=nullptr; // The authenticated peer window of a WSI session.
  // Negotiated at Initialize. Authoritative x64 CPU copies let the x86 owner
  // drop its own. Index buffers keep a session copy too, apart from the
  // backend's indexBytes, so ReadContent never reaches into the backend.
  bool retainContent=false;
  uint64_t retainedBytes=0,peakRetainedBytes=0,contentReads=0,contentReadBytes=0;
  std::vector<uint8_t> retained(d9::Type type,uint32_t bytes) {
    if(!retainContent||(type!=d9::Type::VertexBuffer&&type!=d9::Type::IndexBuffer&&type!=d9::Type::Texture))return {};
    return std::vector<uint8_t>(bytes);
  }
  void adopt(Entry& entry,std::vector<uint8_t> content) {
    retainedBytes+=content.size();peakRetainedBytes=std::max(peakRetainedBytes,retainedBytes);
    entry.content=std::move(content);
  }
  // Negotiated at Initialize: x86 data-layer records. Until enhancement
  // executors consume them, the recorder proves order, payloads and binding.
  bool annotations=false,annotationsPending=false;
  d9::DrawAnnotations pendingAnnotations;
  d9::AnnotationRecorder annotationRecorder;
  d9::SceneTracker sceneTracker;
  // The frame's scene packet and what the BeforeUi executors did with it.
  d9::scene::FramePacket scenePacket;
  uint64_t effectRecords=0,effectFrames=0,ssaoRecorded=0,snowRecorded=0,aaRecorded=0,bloomRecorded=0,effectInsertionsRefused=0;
  uint32_t lastSsaoStage=0,lastSnowStage=0,lastAaStage=0,lastBloomStage=0;
  uint64_t effectNs=0,effectResourceBytes=0; // copied from the backend after each run
  // The frame's shadow records, and what the shadow pass did with them and
  // with the caster draws (shadow_before_ui.h).
  d9::scene::ShadowPacket shadowPacket;
  uint64_t shadowRecords=0,shadowCasterRecords=0,shadowRuns=0,shadowMapRenders=0,shadowReceiverDraws=0;
  uint64_t shadowCastersUsed=0,shadowCastersRefused=0,shadowCastersDrawn=0,shadowReplayRejected=0;
  std::array<uint64_t,4> shadowRefusals{};
  // Casters the pass took from dynamic buffers, and the bytes it copied;
  // ShadowCasterAlpha records that reached a caster.
  uint64_t shadowCastersCopied=0,shadowCopiedBytes=0,shadowAlphaRecords=0;
  dxvkcore::ShadowOutcome lastShadow;
  // Every run of the pass in order, for a gate that compares two executors
  // frame by frame; a session of more runs than this keeps the first ones.
  static constexpr size_t ShadowRunLogLimit=256;
  std::vector<dxvkcore::ShadowOutcome> shadowRunLog;
  uint64_t shadowPeakResourceBytes=0;
  // The device's allocator after each of those runs, {used, allocated} bytes:
  // what the pass owns is part of it, and so is whatever a map or a device
  // that ended left behind.
  std::vector<std::array<uint64_t,2>> shadowDeviceLog;
  // Content payloads, negotiated with annotations: the only copy of each blob.
  struct ContentStore {
    d9::ContentIndex index;
    std::unordered_map<uint64_t,std::vector<uint8_t>> blobs;
    std::vector<uint8_t> incoming;
    uint64_t chunks=0,uploadedBytes=0,reads=0,readBytes=0,boundRefs=0,refusedRefs=0;
    void drop() {index.abort();std::vector<uint8_t>().swap(incoming);}
  } content;
  // Keep the checked identity at its wire index for both executors. A batch
  // is preflighted as a whole even when its recording switches pipelines.
  std::array<d9::scene::ShadowCaster, GPU::MaxDrawBatch> annotatedCasters{};
  uint32_t casterMask = 0;
  bool inspectAnnotations(uint32_t& waterMask) {
    static_assert(GPU::MaxDrawBatch <= 32 && GPU::MaxFixedDrawBatch <= GPU::MaxDrawBatch);
    casterMask = 0;
    bool bound=true;
    const bool inspectWater = sceneTracker.active && gpu->waterSceneEnabled();
    pendingAnnotations.forEach([&](uint16_t index,uint16_t schema,const uint8_t* data,uint16_t bytes) {
      if(const auto kind=d9::ContentKindOf(schema))bound=bound&&content.index.bound(d9::DecodeContentRef(data),kind);
      if (inspectWater && schema == uint32_t(d9::AnnotationSchema::ShadowCaster) &&
          d9::scene::DecodeShadowCaster(data, bytes, annotatedCasters[index])) {
        casterMask |= 1u << index;
        if (d9::scene::IsWaterSurfaceRecord(annotatedCasters[index]))
          waterMask |= 1u << index;
      }
    });
    return bound;
  }
  // The draws were recorded; a ShadowCaster record hands its draw to the
  // shadow pass, for the frame the next BeforeUi belongs to.
  // prepareDraw writes the checked index span at the same batch index as the
  // annotation. Only a successfully recorded draw can deliver it.
  std::array<GPU::CasterIndexSpan,GPU::MaxDrawBatch> casterSpans;
  void deliverAnnotations(const GPU::DrawState* states) {
    uint32_t last=UINT32_MAX;
    d9::scene::ShadowCaster caster;d9::scene::ShadowCasterAlpha alpha;
    bool hasCaster=false,hasAlpha=false;uint16_t casterIndex=0;
    const auto handCaster=[&] {
      if(!hasCaster)return;
      hasCaster=false;++backendCalls;++shadowCasterRecords;if(hasAlpha)++shadowAlphaRecords;
      // The draw itself was accepted; a caster the pass cannot take is
      // counted by the pass, so nothing is left to refuse here.
      try {gpu->addShadowCaster(states[casterIndex],caster,hasAlpha?&alpha:nullptr,effectFrames+1,
          sceneTracker.generation+1,casterSpans[casterIndex]);}
      catch(const GPU::ProfileRejected&) {}
    };
    pendingAnnotations.forEach([&](uint16_t index,uint16_t schema,const uint8_t* data,uint16_t bytes) {
      const auto& s=states[index];
      if(index!=last){++annotationRecorder.annotatedDraws;last=index;}
      if(d9::ContentKindOf(schema))++content.boundRefs;
      annotationRecorder.draw(schema,data,bytes,uint32_t(s.primitiveType),s.primitiveCount,s.numVertices,s.startIndex);
      // A caster goes to the pass with its draw's companion record, which
      // follows it (records ascend by draw and schema).
      if(hasCaster&&casterIndex!=index)handCaster();
      if(schema==uint32_t(d9::AnnotationSchema::ShadowCaster)) {
        if (casterMask & (1u << index)) {
          caster=annotatedCasters[index];hasCaster=true;
        } else hasCaster=d9::scene::DecodeShadowCaster(data,bytes,caster);
        casterIndex=index;hasAlpha=false;
      } else if(schema==uint32_t(d9::AnnotationSchema::ShadowCasterAlpha)&&hasCaster&&casterIndex==index) {
        hasAlpha=d9::scene::DecodeShadowCasterAlpha(data,bytes,alpha);
      }
    });
    handCaster();
  }
  // The scene packet ends with its frame, and at a session change.
  void clearScene() {scenePacket.clear();shadowPacket.clear();gpu->endShadowFrame();}
  bool admissionProfile=false;
  AdmissionCounters admission;
  std::array<AdmissionSample,128> admissionSamples{};
  uint32_t admissionSampleCount=0,admissionSamplesDropped=0;
  void sampleAdmission() {
    if(admissionSampleCount==admissionSamples.size()){++admissionSamplesDropped;return;}
    auto& sample=admissionSamples[admissionSampleCount++];
    sample.query=handles.queryIssues;sample.cumulative=admission;
    const auto& s=stats();
    sample.cumulative.prepareNs=s.admissionPrepareNs;sample.cumulative.recordNs=s.admissionRecordNs;
    sample.cumulative.constantNs=s.admissionConstantNs;sample.cumulative.bindNs=s.admissionBindNs;
    sample.cumulative.samplerNs=s.admissionSamplerNs;sample.cumulative.drawNs=s.admissionDrawNs;
  }
  Entry* find(d9::ResourceRef ref,d9::Type type) {
    return registry.matches(ref,type)?&entries[ref.id]:nullptr;
  }
  HandleStats handleStats() const {
    auto result=handles;
    result.liveLogicalHandles=registry.liveCount();
    result.liveQueries=0;
    for(uint32_t id=1;id<entryLimit;++id)
      if(registry.matches(entries[id].ref,d9::Type::EventQuery))++result.liveQueries;
    return result;
  }
  static bool buffer(d9::Type t){return t==d9::Type::VertexBuffer||t==d9::Type::IndexBuffer;}
  bool resolveDraw(const d9::DrawCommand& command,GPU::DrawState& state,bool fixed=false) {
    const auto& refs=command.resources;
    auto vb=find(refs[0],d9::Type::VertexBuffer),ib=find(refs[1],d9::Type::IndexBuffer),t=find(refs[2],d9::Type::Texture);
    auto vs=find(refs[3],d9::Type::VertexShader),ps=find(refs[4],d9::Type::PixelShader),d=find(refs[5],d9::Type::Declaration);
    if(!vb||!d||!vb->initializedPrefix
      ||(refs[2].id?(!t||!t->uploaded||textureUpload.owns(refs[2])):refs[2].generation!=0)
      ||(refs[3].id?!vs:(!fixed||refs[3].generation!=0))
      ||(refs[4].id?!ps:(!fixed||refs[4].generation!=0))
      ||(refs[1].id?(!ib||!ib->initializedPrefix):refs[1].generation!=0))return false;
    state=command.state;state.vertexBuffer=vb->slot;state.indexBuffer=ib?ib->slot:0;
    state.texture=t?t->slot:0;state.vertexShader=vs?vs->slot:0;state.pixelShader=ps?ps->slot:0;state.declaration=d->slot;
    for(uint32_t i=0;i<d9::AdditionalTextureStages;++i) {
      const auto ref=textureStages.textures[i];
      if(!ref.id){state.additionalTextures[i]=0;continue;}
      const auto* entry=find(ref,d9::Type::Texture);
      if(!entry||!entry->uploaded||textureUpload.owns(ref))return false;
      state.additionalTextures[i]=entry->slot;
    }
    return true;
  }
  uint32_t slot(d9::Type type){
    if(!d9::ValidType(type)||type==d9::Type::EventQuery)return 0;
    const uint32_t max=buffer(type)?GPU::BufferSlots:type==d9::Type::Texture?textureSlots:type==d9::Type::Declaration?GPU::DeclarationSlots:GPU::ShaderSlots;
    // One pass over the live entries, then the lowest free slot (the tables are
    // game-sized: a scan per candidate slot would be quadratic).
    slotScratch.assign(size_t(max)+1,0);
    for(uint32_t id=1;id<entryLimit;++id) {
      const auto& e=entries[id];
      if(e.slot&&e.slot<=max&&registry.matches(e.ref,e.type)&&(e.type==type||(buffer(type)&&buffer(e.type))))slotScratch[e.slot]=1;
    }
    for(uint32_t n=1;n<=max;++n)if(!slotScratch[n])return n;
    return 0;
  }
  HRESULT apply(const d9::Header& h,const uint8_t* payload,d9::Packet& result,uint32_t& resultBytes){
    resultBytes=0;
    // Annotations bind only to the very next message, which must be the draw
    // message they declared. Anything else is refused and drops them.
    const bool annotated=annotationsPending;annotationsPending=false;
    if(annotated&&!pendingAnnotations.binds(h))return D3DERR_INVALIDCALL;
    // An open content upload admits only its own next chunk; anything else is
    // refused and drops the blob.
    if(content.index.active()&&h.op!=d9::Op::ContentUpload){content.drop();return D3DERR_INVALIDCALL;}
    // A draw is refused whole when one of its records names content that is
    // not there: the executor never sees a dangling reference.
    uint32_t waterMask = 0;
    if(annotated&&!inspectAnnotations(waterMask)){++content.refusedRefs;return D3DERR_INVALIDCALL;}
    // Keep the actual dispatcher safe for direct CPU tests as well as decoded
    // wire packets. No malformed identity or payload reaches a backend call.
    if(!d9::ValidOp(uint32_t(h.op))||h.payloadBytes>d9::MaxPayload
      ||(h.payloadBytes&&!payload)||h.id>d9::MaxResources
      ||(h.id?!h.generation:h.generation!=0))return D3DERR_INVALIDCALL;
    if(finished)return D3DERR_INVALIDCALL;
    if(!initialized&&h.op==d9::Op::Finish&&!gpu) {
      if(h.id||h.payloadBytes||std::any_of(h.args.begin(),h.args.end(),[](uint32_t v){return v!=0;}))
        return D3DERR_INVALIDCALL;
      // Rejected/never-sent Initialize is a CPU-only close, not GPU teardown.
      uninitializedStats.normalTeardown=true;finished=true;return S_OK;
    }
    if(h.op==d9::Op::FactoryQuery) {
      if(!d9::ValidFactoryQuery(h))return D3DERR_INVALIDCALL;
      if(!factory)factory=std::make_unique<dxvkcore::D3D9Factory>();
      return factory->query(h,result.data()+d9::ReplyBytes,resultBytes);
    }
    if(!initialized&&h.op!=d9::Op::Initialize)return D3DERR_INVALIDCALL;
    auto unused=[&](size_t n){for(;n<h.args.size();++n)if(h.args[n])return false;return true;};
    const d9::ResourceRef ref{h.id,h.generation};
    auto commit=[&](d9::Type type,uint32_t index,uint32_t size,uint32_t w=0,uint32_t height=0){
      // The backend has already succeeded. An impossible commit is terminal,
      // never a recoverable rejection that would let the proxy reuse this ID.
      Require(registry.commitCreate(ref,type),"D3D9 resource create commit invariant");
      Entry entry{};entry.ref=ref;entry.type=type;entry.slot=index;entry.size=size;
      entry.width=w;entry.height=height;entries[ref.id]=entry;entryLimit=std::max(entryLimit,ref.id+1);
      ++handles.logicalHandlesCreated;
      if(ref.generation>1)++handles.logicalHandlesReused;
      if(type==d9::Type::EventQuery) {
        ++handles.queryHandlesCreated;
        if(ref.generation>1)++handles.queryHandlesReused;
      } else if(ref.generation>1)++handles.resourcesReused;
    };
    switch(h.op){
      case d9::Op::Initialize:{
        d9::Initialization init;
        if(initialized||!d9::DecodeInitialization(h,init))return D3DERR_INVALIDCALL;
        if(init.profile==d9::PresentationProfile::Offscreen&&surfaceMode!=surface::Mode::Peer)
          return D3DERR_INVALIDCALL; // Explicit mode mismatch is not a fallback.
        if(init.profile!=d9::PresentationProfile::Offscreen) {
          d9::RequireIsolatedDesktop();
          const HWND window=d9::DecodeWindow(init.window);DWORD pid=0;
          const auto thread=GetWindowThreadProcessId(window,&pid);RECT client{};
          if(!IsWindow(window)||pid!=peerPid||thread!=init.thread
            ||d9::ObjectName(GetThreadDesktop(thread))!=d9::CurrentDesktop()
            ||!GetClientRect(window,&client)||client.left||client.top
            ||uint32_t(client.right)!=init.width||uint32_t(client.bottom)!=init.height
            ||GetPropW(window,marker.c_str())!=reinterpret_cast<HANDLE>(uintptr_t(1)))return D3DERR_INVALIDCALL;
          // Host-owned surface windows remain at the 64x64 lab extent.
          if(surfaceMode!=surface::Mode::Peer&&(init.width!=d9::Width||init.height!=d9::Height))
            return D3DERR_INVALIDCALL;
          std::printf("{\"schema\":\"warvk-d3d9-desktop/v1\",\"pid\":%lu,\"threadId\":%lu,\"peerPid\":%lu,"
            "\"windowThreadId\":%lu,\"windowStation\":%s,\"desktop\":%s,\"inputDesktop\":%s,\"beforeGpu\":true}\n",
            GetCurrentProcessId(),GetCurrentThreadId(),peerPid,thread,
            d9::NameJson(d9::ObjectName(GetProcessWindowStation())).c_str(),
            d9::NameJson(d9::CurrentDesktop()).c_str(),d9::NameJson(d9::InputDesktop()).c_str());
          std::fflush(stdout);
          presentedReadback=init.profile==d9::PresentationProfile::IsolatedPresenterReadback;
          if(!factory)factory=std::make_unique<dxvkcore::D3D9Factory>();
          gpu=std::make_unique<GPU>();++backendCalls;
          gpu->initialize(init.width,init.height,window,peerPid,thread,marker.c_str(),presentedReadback,peerProcess,surfaceMode,factory.get(),
            init.opaque,init.interval);
          presenterWindow=window;
        } else {
          // Observe the actual GPU owner desktop even for offscreen work. This
          // is evidence only; it does not change the profile's admission rule.
          std::printf("{\"schema\":\"warvk-d3d9-desktop/v1\",\"pid\":%lu,\"threadId\":%lu,\"peerPid\":%lu,"
            "\"windowThreadId\":0,\"windowStation\":%s,\"desktop\":%s,\"inputDesktop\":%s,\"beforeGpu\":true}\n",
            GetCurrentProcessId(),GetCurrentThreadId(),peerPid,
            d9::NameJson(d9::ObjectName(GetProcessWindowStation())).c_str(),
            d9::NameJson(d9::CurrentDesktop()).c_str(),d9::NameJson(d9::InputDesktop()).c_str());
          std::fflush(stdout);
          if(!factory)factory=std::make_unique<dxvkcore::D3D9Factory>();
          gpu=std::make_unique<GPU>();++backendCalls;
          gpu->initialize(init.width,init.height,nullptr,0,0,nullptr,false,nullptr,surface::Mode::Peer,factory.get(),init.opaque,0);
        }
        targetWidth=init.width;targetHeight=init.height;
        retainContent=init.retainContent;annotations=init.annotations;fixedDelta=init.fixedDelta;
        asyncResources=init.asyncResources;initialized=true;break;
      }
      case d9::Op::CreateBuffer:{
        if(h.payloadBytes||!unused(3)||!h.args[0]||h.args[0]>d9::MaxBuffer||h.args[1]>1||h.args[2]>1
          ||(h.args[1]&&(h.args[0]&1)))return D3DERR_INVALIDCALL;
        const auto type=h.args[1]?d9::Type::IndexBuffer:d9::Type::VertexBuffer;
        if(!registry.canCreate(ref,type))return D3DERR_INVALIDCALL;
        const auto index=slot(type);if(!index)return D3DERR_OUTOFVIDEOMEMORY;
        auto content=retained(type,h.args[0]);
        ++backendCalls;gpu->createBuffer(index,h.args[0],bool(h.args[1]),bool(h.args[2]));commit(type,index,h.args[0]);
        entries[ref.id].dynamic=h.args[2]!=0;adopt(entries[ref.id],std::move(content));break;
      }
      case d9::Op::UploadBuffer:{
        auto* e=find(ref,d9::Type::VertexBuffer);if(!e)e=find(ref,d9::Type::IndexBuffer);
        if(!e||!unused(2)||!d9::ValidUploadMode(h.args[1],e->dynamic)||!h.payloadBytes||h.args[0]>e->initializedPrefix
          ||h.args[0]>e->size||h.payloadBytes>e->size-h.args[0])return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->uploadBuffer(e->slot,h.args[0],payload,h.payloadBytes,d9::UploadMode(h.args[1]));
        if(!e->content.empty())std::memcpy(e->content.data()+h.args[0],payload,h.payloadBytes);
        e->initializedPrefix=std::max(e->initializedPrefix,h.args[0]+h.payloadBytes);break;
      }
      case d9::Op::CreateTexture:{
        if(h.payloadBytes||!unused(2)||!h.args[0]||!h.args[1]||h.args[0]>64||h.args[1]>64
          ||!registry.canCreate(ref,d9::Type::Texture))return D3DERR_INVALIDCALL;
        const auto index=slot(d9::Type::Texture);if(!index)return D3DERR_OUTOFVIDEOMEMORY;
        auto content=retained(d9::Type::Texture,h.args[0]*h.args[1]*4);
        ++backendCalls;gpu->createTexture(index,h.args[0],h.args[1]);commit(d9::Type::Texture,index,h.args[0]*h.args[1]*4,h.args[0],h.args[1]);
        Require(d9::MakeTextureLayout(h.args[0],h.args[1],1,entries[ref.id].textureLayout),"legacy texture layout");
        adopt(entries[ref.id],std::move(content));break;
      }
      case d9::Op::UploadTexture:{
        auto* e=find(ref,d9::Type::Texture);if(!e||!unused(0)||h.payloadBytes!=e->size||e->textureLayout.levels!=1||textureUpload.owns(ref))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->uploadTexture(e->slot,payload,h.payloadBytes);e->uploaded=true;e->initializedLevels=1;
        if(!e->content.empty())std::memcpy(e->content.data(),payload,h.payloadBytes);break;
      }
      case d9::Op::CreateTexture2D:{
        d9::TextureLayout layout;
        if(h.payloadBytes||!unused(4)
            ||!d9::MakeTextureLayout(h.args[0],h.args[1],h.args[2],layout,h.args[3])
            ||!registry.canCreate(ref,d9::Type::Texture))return D3DERR_INVALIDCALL;
        // The original adapter policy must accept the requested format before
        // any GPU allocation or logical resource publication is attempted.
        d9::Header query;query.op=d9::Op::FactoryQuery;
        query.args={uint32_t(d9::FactoryQuery::Format),D3DDEVTYPE_HAL,D3DFMT_X8R8G8B8,0,D3DRTYPE_TEXTURE,layout.format};
        uint8_t ignored[4]{};uint32_t resultBytes=0;
        if(!factory||factory->query(query,ignored,resultBytes)!=D3D_OK)return D3DERR_INVALIDCALL;
        const auto index=slot(d9::Type::Texture);if(!index)return D3DERR_OUTOFVIDEOMEMORY;
        auto content=retained(d9::Type::Texture,layout.bytes);
        ++backendCalls;gpu->createTexture2D(index,layout);commit(d9::Type::Texture,index,layout.bytes,layout.width,layout.height);
        entries[ref.id].textureLayout=layout;adopt(entries[ref.id],std::move(content));break;
      }
      case d9::Op::BeginTextureUpload:{
        auto* e=find(ref,d9::Type::Texture);d9::TextureRegion region;
        if(!e||h.payloadBytes||!unused(6)||textureUpload.active()||e->textureRevision==UINT32_MAX
            ||h.args[0]!=e->textureRevision+1
            ||!d9::MakeTextureRegion(e->textureLayout,h.args[1],h.args[2],h.args[3],h.args[4],h.args[5],region)
            ||(!(e->initializedLevels&(1u<<region.level))&&!d9::FullTextureLevel(e->textureLayout,region)))
          return D3DERR_INVALIDCALL;
        if(!textureUpload.begin(ref,h.args[0],region))return D3DERR_INVALIDCALL;
        e->textureRevision=h.args[0];break;
      }
      case d9::Op::TextureUploadChunk:{
        if(!find(ref,d9::Type::Texture)||!unused(2)
            ||!textureUpload.append(ref,h.args[0],h.args[1],payload,h.payloadBytes))return D3DERR_INVALIDCALL;
        break;
      }
      case d9::Op::CommitTextureUpload:{
        auto* e=find(ref,d9::Type::Texture);
        if(!e||h.payloadBytes||!unused(1)||!textureUpload.complete(ref,h.args[0]))return D3DERR_INVALIDCALL;
        const auto region=textureUpload.region();++backendCalls;
        gpu->uploadTextureRegion(e->slot,region,textureUpload.data());
        if(!e->content.empty())d9::PatchTextureLayout(e->textureLayout,region,textureUpload.data(),e->content.data());
        if(d9::FullTextureLevel(e->textureLayout,region))e->initializedLevels|=1u<<region.level;
        e->uploaded=e->initializedLevels==((1u<<e->textureLayout.levels)-1);
        textureUpload.reset();break;
      }
      case d9::Op::AbortTextureUpload:{
        if(!find(ref,d9::Type::Texture)||h.payloadBytes||!unused(1)||!textureUpload.matches(ref,h.args[0]))return D3DERR_INVALIDCALL;
        textureUpload.reset();break;
      }
      case d9::Op::ReadContent:{
        // Retained CPU copy only: no GPU readback, settle, or ordering change.
        // A texture level is readable once initialized and outside an upload.
        auto* e=find(ref,d9::Type::VertexBuffer);if(!e)e=find(ref,d9::Type::IndexBuffer);
        const bool texture=!e;if(texture)e=find(ref,d9::Type::Texture);
        const auto level=h.args[0],offset=h.args[1],count=h.args[2];
        if(!retainContent||!e||e->content.empty()||h.payloadBytes||!unused(3)||!count||count>d9::MaxContentRead)
          return D3DERR_INVALIDCALL;
        uint32_t base=0,limit=e->initializedPrefix;
        if(texture) {
          if(level>=e->textureLayout.levels||!(e->initializedLevels&(1u<<level))||textureUpload.owns(ref))
            return D3DERR_INVALIDCALL;
          base=e->textureLayout.level[level].offset;limit=e->textureLayout.level[level].bytes;
        } else if(level)return D3DERR_INVALIDCALL;
        if(offset>limit||count>limit-offset)return D3DERR_INVALIDCALL;
        std::memcpy(result.data()+d9::ReplyBytes,e->content.data()+base+offset,count);
        resultBytes=count;++contentReads;contentReadBytes+=count;break;
      }
      case d9::Op::TextureLod:{
        auto* e=find(ref,d9::Type::Texture);
        if(!e||h.payloadBytes||!unused(1)||h.args[0]>=e->textureLayout.levels||textureUpload.owns(ref))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->setTextureLod(e->slot,h.args[0]);break;
      }
      case d9::Op::CreateShader:{
        if(!unused(1)||h.args[0]>1||(h.payloadBytes&3)||!h.payloadBytes||h.payloadBytes>d3d9tokens::MaxWords*4)return D3DERR_INVALIDCALL;
        std::array<uint32_t,d3d9tokens::MaxWords> words{};for(uint32_t i=0;i<h.payloadBytes/4;++i)words[i]=d9::U32(payload+i*4);
        const auto scan=d3d9tokens::Scan(words.data(),h.payloadBytes/4);
        if(!scan.accepted()||scan.stage!=(h.args[0]?d3d9tokens::Stage::Pixel:d3d9tokens::Stage::Vertex))return D3DERR_INVALIDCALL;
        const auto type=h.args[0]?d9::Type::PixelShader:d9::Type::VertexShader;
        if(!registry.canCreate(ref,type))return D3DERR_INVALIDCALL;
        const auto index=slot(type);if(!index)return D3DERR_OUTOFVIDEOMEMORY;
        ++backendCalls;gpu->createShader(index,h.args[0]?GPU::ShaderStage::Pixel:GPU::ShaderStage::Vertex,words.data(),h.payloadBytes/4);
        commit(type,index,h.payloadBytes);break;
      }
      case d9::Op::CreateDeclaration:{
        d9::DeclarationCommand command;
        if(!unused(0)||!registry.canCreate(ref,d9::Type::Declaration)
          ||!d9::DecodeDeclaration(payload,h.payloadBytes,command))return D3DERR_INVALIDCALL;
        const auto index=slot(d9::Type::Declaration);if(!index)return D3DERR_OUTOFVIDEOMEMORY;
        ++backendCalls;gpu->createDeclaration(index,command.elements.data(),command.count);
        commit(d9::Type::Declaration,index,h.payloadBytes);break;
      }
      case d9::Op::Clear:
        if(h.id||h.payloadBytes||!unused(3)||!h.args[2]||(h.args[2]&~uint32_t(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER))
          ||!std::isfinite(d9::Float(h.args[1]))||d9::Float(h.args[1])<0||d9::Float(h.args[1])>1)return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->clear(h.args[0],d9::Float(h.args[1]),h.args[2]);break;
      case d9::Op::Draw:{
        d9::DrawCommand command;if(h.id||!unused(0)||!d9::DecodeDraw(payload,h.payloadBytes,command))return D3DERR_INVALIDCALL;
        GPU::DrawState s;if(!resolveDraw(command,s))return D3DERR_INVALIDCALL;
        ++backendCalls;
        try {
          if (waterMask) gpu->drawBatch(&s,1,casterSpans.data(),waterMask);
          else gpu->draw(s,annotated?casterSpans.data():nullptr);
        }
        catch (const GPU::ProfileRejected&) { return D3DERR_INVALIDCALL; }
        if(annotated)deliverAnnotations(&s);
        break;
      }
      case d9::Op::DrawFixed:{
        d9::DrawCommand command;d9::FixedFunctionState fixed;GPU::DrawState state;
        if(h.id||!unused(0)||!d9::DecodeFixedDraw(payload,h.payloadBytes,command,fixed)
          ||!resolveDraw(command,state,true))return D3DERR_INVALIDCALL;
        ++backendCalls;
        try {
          if (waterMask) gpu->drawFixedBatch(&state,&fixed,1,casterSpans.data(),waterMask);
          else gpu->drawFixed(state,fixed,annotated?casterSpans.data():nullptr);
        }
        catch(const GPU::ProfileRejected&) {return D3DERR_INVALIDCALL;}
        if(annotated)deliverAnnotations(&state);
        break;
      }
      case d9::Op::SetSamplerState:{
        d9::SamplerState state;
        if(h.id||!unused(0)||!d9::DecodeSamplerState(payload,h.payloadBytes,state))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->setSamplerState(state);break;
      }
      case d9::Op::SetEffects:{
        d9::EffectsState state;
        if(h.id||!unused(0)||!d9::DecodeEffects(payload,h.payloadBytes,state))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->setEffects(state);break;
      }
      case d9::Op::SetTextureStages:{
        d9::TextureStages state;
        if(h.id||!unused(0)||!d9::DecodeTextureStages(payload,h.payloadBytes,state))return D3DERR_INVALIDCALL;
        // Validate the whole identity table before either the samplers or the
        // bindings change. Draw resolves generations again after any Destroy.
        for(const auto ref:state.textures)if(ref.id&&!find(ref,d9::Type::Texture))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->setAdditionalSamplers(state.samplers);textureStages=state;break;
      }
      case d9::Op::DrawBatch:{
        const auto count=h.args[0];
        if(h.id||!unused(1)||!count||count>d9::MaxDrawBatch||h.payloadBytes!=count*d9::DrawBytes)
          return D3DERR_INVALIDCALL;
        std::array<GPU::DrawState,d9::MaxDrawBatch> states{};
        const auto decodeBegin=admissionProfile?Clock::now():Clock::time_point{};
        for(uint32_t i=0;i<count;++i) {
          d9::DrawCommand command;
          if(!d9::DecodeDraw(payload+i*d9::DrawBytes,d9::DrawBytes,command))return D3DERR_INVALIDCALL;
          if(!resolveDraw(command,states[i]))return D3DERR_INVALIDCALL;
        }
        if(admissionProfile)admission.batchDecodeNs+=ElapsedNs(decodeBegin);
        ++backendCalls;
        try {
          if (waterMask) gpu->drawBatch(states.data(),count,casterSpans.data(),waterMask);
          else gpu->drawBatch(states.data(),count,annotated?casterSpans.data():nullptr);
        }
        catch(const GPU::ProfileRejected&) { return D3DERR_INVALIDCALL; }
        admission.batchDraws+=count;
        if(annotated)deliverAnnotations(states.data());
        break;
      }
      case d9::Op::DrawFixedBatch:case d9::Op::DrawFixedDeltaBatch:{
        const auto count=h.args[0];const bool delta=h.op==d9::Op::DrawFixedDeltaBatch;
        if(delta) {
          if(!fixedDelta)return D3DERR_INVALIDCALL;
        } else if(h.id||!unused(1)||!count||count>d9::MaxFixedDrawBatch||h.payloadBytes!=count*d9::FixedDrawBytes)
          return D3DERR_INVALIDCALL;
        const auto decodeBegin=admissionProfile?Clock::now():Clock::time_point{};
        const auto stage=[&](uint32_t i,const uint8_t* bytes) {
          d9::DrawCommand command;
          // Same result and outputs either way; the decoder keeps what did not change since the
          // previous record (d3d9_ff_fast_decode.h).
          return (fastDecode?fixedDecoder.decode(bytes,command,fixedBatchFixed[i])
              :d9::DecodeFixedDraw(bytes,d9::FixedDrawBytes,command,fixedBatchFixed[i]))
            &&resolveDraw(command,fixedBatchStates[i],true);
        };
        if(delta) {
          const auto status=d9::StageFixedDeltaBatch(h,payload,stage);
          if(status==d9::FixedDeltaResult::HashMismatch)
            std::fprintf(stderr,"D3D9 fixed delta-mismatch sequence=%llu count=%u\n",h.sequence,count);
          if(status!=d9::FixedDeltaResult::Valid)return D3DERR_INVALIDCALL;
        } else for(uint32_t i=0;i<count;++i)
          if(!stage(i,payload+i*d9::FixedDrawBytes))return D3DERR_INVALIDCALL;
        if(admissionProfile)admission.batchDecodeNs+=ElapsedNs(decodeBegin);
        ++backendCalls;
        try {
          if (waterMask) gpu->drawFixedBatch(fixedBatchStates.data(),fixedBatchFixed.data(),count,casterSpans.data(),waterMask);
          else gpu->drawFixedBatch(fixedBatchStates.data(),fixedBatchFixed.data(),count,annotated?casterSpans.data():nullptr);
        }
        catch(const GPU::ProfileRejected&) { return D3DERR_INVALIDCALL; }
        admission.batchDraws+=count;
        ++fixedBatches;fixedBatchDraws+=count;largestFixedBatch=std::max(largestFixedBatch,count);
        if(annotated)deliverAnnotations(fixedBatchStates.data());
        break;
      }
      case d9::Op::AnnotateDraws:
        if(!annotations||!d9::DecodeDrawAnnotations(h,payload,pendingAnnotations))return D3DERR_INVALIDCALL;
        annotationsPending=true;break;
      case d9::Op::FrameScene:
        if(!annotations||h.id||!unused(1)||!d9::ValidFrameScene(h.args[0],payload,h.payloadBytes))return D3DERR_INVALIDCALL;
        if(sceneStopped){++sceneRecordsIgnored;break;}
        // An unusable camera is not a protocol error: effects report it.
        if(shadowPacket.accept(h.args[0],payload,h.payloadBytes))++shadowRecords;
        else {
          scenePacket.accept(h.args[0],payload,h.payloadBytes);
          if(h.args[0]!=uint32_t(d9::AnnotationSchema::FrameCamera))++effectRecords;
        }
        annotationRecorder.scene(h.args[0],payload,h.payloadBytes);break;
      case d9::Op::StageMarker:{
        if(!annotations||h.id||h.payloadBytes||!unused(1)||!d9::ValidStage(h.args[0]))return D3DERR_INVALIDCALL;
        if(sceneStopped){++sceneRecordsIgnored;break;}
        // Effects go between the completed world scene and whatever follows
        // it, once. Anywhere else the marker is refused and nothing runs.
        const bool insert=h.args[0]==uint32_t(d9::Stage::BeforeUi)&&(scenePacket.effects()||shadowPacket.work());
        if(insert&&!sceneTracker.canInsertBeforeUi()){++effectInsertionsRefused;return D3DERR_INVALIDCALL;}
        if(!sceneTracker.mark(h.args[0],gpu->stats().draws))return D3DERR_INVALIDCALL;
        if (h.args[0] == uint32_t(d9::Stage::WorldSceneEnter)) gpu->beginWaterScene();
        if(insert) {
          ++backendCalls;
          GPU::BeforeUiOutcome outcome;
          try { outcome=gpu->runBeforeUi(scenePacket,shadowPacket,effectFrames+1,sceneTracker.generation+1); }
          catch(const GPU::ProfileRejected&) { return D3DERR_INVALIDCALL; }
          ++effectFrames;sceneTracker.insertedBeforeUi();
          effectNs=gpu->stats().beforeUiNs;effectResourceBytes=gpu->stats().effectResourceBytes;
          lastSsaoStage=outcome.ssaoStage;lastSnowStage=outcome.snowStage;
          lastAaStage=outcome.aaStage;lastBloomStage=outcome.bloomStage;
          ssaoRecorded+=outcome.ssaoRecorded;snowRecorded+=outcome.snowRecorded;
          aaRecorded+=outcome.aaRecorded;bloomRecorded+=outcome.bloomRecorded;
          const auto& shadow=outcome.shadow;
          shadowCastersRefused+=shadow.refused;shadowCastersUsed+=shadow.received-shadow.refused;
          for(size_t i=0;i<shadowRefusals.size();++i)shadowRefusals[i]+=shadow.refusals[i];
          shadowCastersCopied+=shadow.copied;shadowCopiedBytes+=shadow.copiedBytes;
          if(shadow.ran) {
            ++shadowRuns;shadowMapRenders+=shadow.mapExecuted;shadowReceiverDraws+=shadow.receiverExecuted;
            shadowCastersDrawn+=shadow.drawnCasters;shadowReplayRejected+=shadow.replayRejected;lastShadow=shadow;
            shadowPeakResourceBytes=std::max(shadowPeakResourceBytes,shadow.resourceBytes);
            if(shadowRunLog.size()<ShadowRunLogLimit) {
              shadowRunLog.push_back(shadow);
              shadowDeviceLog.push_back({outcome.deviceUsedBytes,outcome.deviceAllocatedBytes});
            }
          }
        }
        if(h.args[0]==uint32_t(d9::Stage::FrameEnd))clearScene();
        annotationRecorder.stage(h.args[0]);break;
      }
      case d9::Op::SessionReset:{
        // Registered and folded only: no host executor owns per-map state yet.
        if(!annotations||!d9::ValidSessionReset(h))return D3DERR_INVALIDCALL;
        const auto generation=uint64_t(h.args[1])|(uint64_t(h.args[2])<<32);
        if(!sceneTracker.reset(generation))return D3DERR_INVALIDCALL;
        annotationRecorder.reset(h.args[0],generation);clearScene();
        // Content identities belong to the map that ended.
        content.index.clear();content.blobs.clear();break;
      }
      case d9::Op::ContentUpload:{
        d9::ContentChunk chunk;
        if(!annotations||!d9::DecodeContentUpload(h,payload,chunk)){content.drop();return D3DERR_INVALIDCALL;}
        const auto admitted=content.index.admit(chunk);
        if(admitted==d9::ContentIndex::Admit::Refused){content.drop();return D3DERR_INVALIDCALL;}
        if(!chunk.offset){content.incoming.clear();content.incoming.reserve(chunk.total);}
        content.incoming.insert(content.incoming.end(),payload,payload+chunk.count);
        ++content.chunks;content.uploadedBytes+=chunk.count;
        if(admitted==d9::ContentIndex::Admit::Complete) {
          annotationRecorder.content(chunk.kind,chunk.key,chunk.revision,content.incoming.data(),chunk.total);
          content.blobs[chunk.key]=std::move(content.incoming);content.incoming=std::vector<uint8_t>();
        }
        break;
      }
      case d9::Op::ContentRelease:{
        d9::ContentRef released;
        if(!annotations||!d9::DecodeContentRelease(h,released)||!content.index.release(released))return D3DERR_INVALIDCALL;
        content.blobs.erase(released.key);annotationRecorder.release(released.key,released.revision);break;
      }
      case d9::Op::ContentRead:{
        // The stored bytes only: no GPU work, settle, or ordering change.
        d9::ContentRef wanted;uint32_t offset=0,count=0;
        if(!annotations||!d9::DecodeContentRead(h,wanted,offset,count))return D3DERR_INVALIDCALL;
        const auto* meta=content.index.find(wanted);
        if(!meta||offset>meta->bytes||count>meta->bytes-offset)return D3DERR_INVALIDCALL;
        std::memcpy(result.data()+d9::ReplyBytes,content.blobs.find(wanted.key)->second.data()+offset,count);
        resultBytes=count;++content.reads;content.readBytes+=count;break;
      }
      case d9::Op::ClearRegions:{
        d9::ClearRegions regions;
        if(!d9::DecodeClearRegions(h,payload,regions,targetWidth,targetHeight))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->clearRegions(regions);break;
      }
      case d9::Op::FlushDraws:
        if(h.id||h.payloadBytes||!unused(0))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->flushDraws();break;
      case d9::Op::Readback:{
        uint32_t first=h.args[0],rows=h.args[1];
        if(h.id||h.payloadBytes||!unused(5)||!d9::ResolveReadbackRows(targetWidth,targetHeight,first,rows))return D3DERR_INVALIDCALL;
        const auto source=d9::ReadbackSource(h.args[2]);
        const auto frame=uint64_t(h.args[3])|(uint64_t(h.args[4])<<32);
        std::vector<uint8_t> bytes;
        if(source==d9::ReadbackSource::Target) {
          if(h.args[3]||h.args[4])return D3DERR_INVALIDCALL;
          ++backendCalls;bytes=gpu->readback(first,rows);
        } else if(source==d9::ReadbackSource::Presented) {
          // Bands of the one frozen diagnostic frame that the last Present
          // reported; an old frame number is refused, never served.
          if(!presentedReadback||!presentedCurrent||!frame||frame!=gpu->stats().presentedFrame)return D3DERR_INVALIDCALL;
          ++backendCalls;bytes=gpu->readPresented(first,rows);
        } else return D3DERR_INVALIDCALL;
        Require(bytes.size()==size_t(targetWidth)*rows*4,"actual readback band extent");
        resultBytes=uint32_t(bytes.size());std::memcpy(result.data()+d9::ReplyBytes,bytes.data(),bytes.size());break;
      }
      case d9::Op::CreateQuery:{
        if(h.payloadBytes||!unused(0)||!registry.canCreate(ref,d9::Type::EventQuery))return D3DERR_INVALIDCALL;
        // A Query is a logical observation handle, not a GPU resource. Its
        // bounded entry is committed without fabricating a backend allocation.
        commit(d9::Type::EventQuery,0,0);break;
      }
      case d9::Op::Issue:{
        auto* e=find(ref,d9::Type::EventQuery);
        const auto revision=uint64_t(h.args[0])|(uint64_t(h.args[1])<<32);
        if(!e||h.payloadBytes||!unused(2)||!revision||e->queryRevision==UINT64_MAX
          ||revision!=e->queryRevision+1||handles.queryIssues==UINT64_MAX)return D3DERR_INVALIDCALL;
        ++backendCalls;const auto serial=gpu->issueEvent();Require(serial!=0,"GPU event serial");
        // A repeated Issue may observe the SAME GPU serial. Only its own
        // monotonically increasing revision replaces the old observation.
        e->queryRevision=revision;e->querySerial=serial;++handles.queryIssues;
        resultBytes=16;d9::P64(result.data()+d9::ReplyBytes,revision);d9::P64(result.data()+d9::ReplyBytes+8,serial);break;
      }
      case d9::Op::Poll:
      case d9::Op::WaitEvent:{
        auto* e=find(ref,d9::Type::EventQuery);
        const auto revision=uint64_t(h.args[0])|(uint64_t(h.args[1])<<32);
        const auto serial=uint64_t(h.args[2])|(uint64_t(h.args[3])<<32);
        if(!e||h.payloadBytes||!unused(4)||!revision||!serial
          ||revision!=e->queryRevision||serial!=e->querySerial||handles.queryPolls==UINT64_MAX
          ||handles.queryWaits==UINT64_MAX)return D3DERR_INVALIDCALL;
        if(h.op==d9::Op::WaitEvent) {
          // This command waits only for an already-issued prefix; no later
          // message is needed to make it progress. The client worker owns it.
          ++backendCalls;gpu->waitEvent(serial);++handles.queryWaits;break;
        }
        ++backendCalls;const bool complete=gpu->eventComplete(serial);++handles.queryPolls;
        resultBytes=4;d9::P32(result.data()+d9::ReplyBytes,complete?1:0);break;
      }
      case d9::Op::Destroy:{
        const auto type=d9::Type(h.args[0]);
        if(h.payloadBytes||!unused(1)||!d9::ValidType(type))return D3DERR_INVALIDCALL;
        auto* e=find(ref,type);if(!e)return D3DERR_INVALIDCALL;
        if(type!=d9::Type::EventQuery) {
          ++backendCalls;gpu->destroy(GPU::ResourceType(h.args[0]-1),e->slot);
        }
        // GPU resources are tombstoned only AFTER the original retirement path
        // succeeds. Query destruction only forgets its observation; no cancel.
        Require(registry.destroy(ref,type),"D3D9 resource destroy commit invariant");
        if(textureUpload.owns(ref))textureUpload.reset();
        retainedBytes-=e->content.size();
        *e=Entry{};++handles.logicalHandlesDestroyed;
        if(type==d9::Type::EventQuery)++handles.queryHandlesDestroyed;
        break;
      }
      case d9::Op::Present:{
        if(h.id||h.payloadBytes||!unused(0))return D3DERR_INVALIDCALL;
        // Offscreen remains unsupported and retains its old command counts.
        if(!gpu->stats().presenterImplemented)return E_NOTIMPL;
        ++backendCalls;const auto hr=gpu->present();sceneTracker.frameEnd();clearScene();
        if(hr==D3D_OK&&presentedReadback) {
          presentedCurrent=true;
          // The frame number, plus the whole frozen image when it fits; larger
          // targets read their bands with Readback(source=Presented, frame).
          d9::P64(result.data()+d9::ReplyBytes,gpu->stats().presentedFrame);resultBytes=8;
          if(d9::PresentedInline(targetWidth,targetHeight)) {
            const auto bytes=gpu->readPresented(0,targetHeight);
            Require(bytes.size()==size_t(targetWidth)*targetHeight*4,"actual presented image extent");
            resultBytes=8+uint32_t(bytes.size());std::memcpy(result.data()+d9::ReplyBytes+8,bytes.data(),bytes.size());
          }
        }
        return hr;
      }
      case d9::Op::Reset:{
        d9::Resize resize;
        const bool presenter=gpu->stats().presenterImplemented;
        if(!d9::DecodeReset(h,presenter,resize))return D3DERR_INVALIDCALL;
        // D3D9 Reset needs every DEFAULT-pool resource released first; buffers
        // are the only ones in this profile. The window's own resize keeps
        // them. Nothing half-created or uploading either way.
        if(!resize.keepResources)
          for(uint32_t id=1;id<entryLimit;++id)
            if(buffer(entries[id].type)&&registry.matches(entries[id].ref,entries[id].type))return D3DERR_INVALIDCALL;
        if(registry.reservedCount()||textureUpload.active())return D3DERR_INVALIDCALL;
        if(presenter) {
          // The client must already have the new extent; host-owned surfaces stay 64x64.
          RECT client{};
          if(!GetClientRect(presenterWindow,&client)||client.left||client.top
            ||uint32_t(client.right)!=resize.width||uint32_t(client.bottom)!=resize.height
            ||(surfaceMode!=surface::Mode::Peer&&(resize.width!=d9::Width||resize.height!=d9::Height)))
            return D3DERR_INVALIDCALL;
        }
        ++backendCalls;gpu->reset(resize.width,resize.height,resize.interval,resize.opaque,resize.keepResources);
        targetWidth=resize.width;targetHeight=resize.height;presentedCurrent=false;break;
      }
      // Attaching is the transport's business (wmain); it is never a session command.
      case d9::Op::AttachRing:return D3DERR_INVALIDCALL;
      case d9::Op::Finish:
        if(h.id||h.payloadBytes||!unused(0))return D3DERR_INVALIDCALL;
        if(registry.liveCount()!=0||registry.reservedCount()!=0)return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->finish();finished=true;break;
      case d9::Op::DiagnosticCheckpoint: {
        if(gpu->hasPendingDraws()||textureUpload.active())return D3DERR_INVALIDCALL;
        if(h.id||h.payloadBytes||!unused(2)||!d9::memory::ValidPhase(h.args[0])
          ||(h.args[0]==uint32_t(d9::memory::Phase::DeviceReady)?h.args[1]!=0:h.args[1]==0))
          return D3DERR_INVALIDCALL;
        // A checkpoint is an explicit observation, not a backend render call.
        // Do not settle GPU work or treat the CPU reply as GPU completion.
        const auto begin=GetTickCount64();auto sample=gpu->diagnosticMemory();
        sample.phase=h.args[0];sample.ordinal=h.args[1];sample.pid=GetCurrentProcessId();
        sample.thread=GetCurrentThreadId();sample.creation=lab::CreationTime(GetCurrentProcess());
        sample.nonceLow=h.nonce.low;sample.nonceHigh=h.nonce.high;sample.sequence=h.sequence;
        sample.tickBegin=begin;sample.fixedPacketPayloadBytes=2ull*sizeof(d9::Packet);
        sample.process=lab::ProbeOwnProcessMemory();sample.tickEnd=GetTickCount64();
        const auto encoded=d9::memory::EncodeHost(sample);resultBytes=uint32_t(encoded.size());
        std::memcpy(result.data()+d9::ReplyBytes,encoded.data(),encoded.size());
        // Exact bounded wire mirror for independent host -> proxy comparison.
        // This is diagnostic text, never a process-memory or image dump.
        std::printf("{\"schema\":\"warvk-d3d9-memory-host/v1\",\"phase\":%u,\"ordinal\":%u,\"sequence\":%llu,\"wireHex\":\"",
          sample.phase,sample.ordinal,sample.sequence);
        for(const auto byte:encoded)std::printf("%02x",unsigned(byte));
        std::printf("\"}\n");std::fflush(stdout);
        // Independent versioned cold observation, not new reserved wire bits.
        // Existing M0 author-storage accounts do NOT include device-lifetime
        // compiler/pipeline caches. Keep their identities and scopes separate.
        const auto cache=gpu->diagnosticShaderCache();
        std::printf("D3D9 shader-cache: {\"schema\":\"warvk-d3d9-shader-cache/v1\","
          "\"pid\":%u,\"thread\":%u,\"creation\":%llu,\"nonceLow\":%llu,\"nonceHigh\":%llu,"
          "\"sequence\":%llu,\"phase\":%u,\"ordinal\":%u,"
          "\"requests\":%llu,\"hits\":%llu,\"compileAttempts\":%llu,\"compilesSucceeded\":%llu,"
          "\"registrations\":%llu,\"raceLosers\":%llu,\"validationFailures\":%llu,"
          "\"registrationFailures\":%llu,\"readyEntries\":%llu,\"failedEntries\":%llu,"
          "\"keyBytes\":%llu,\"keyCapacityBytes\":%llu,\"constantBytes\":%llu,\"constantCapacityBytes\":%llu,"
          "\"vertexCookies\":[",
          sample.pid,sample.thread,sample.creation,sample.nonceLow,sample.nonceHigh,
          sample.sequence,sample.phase,sample.ordinal,
          cache.requests,cache.hits,cache.compileAttempts,cache.compilesSucceeded,
          cache.registrations,cache.raceLosers,cache.validationFailures,
          cache.registrationFailures,cache.readyEntries,cache.failedEntries,
          cache.keyBytes,cache.keyCapacityBytes,cache.constantBytes,cache.constantCapacityBytes);
        for(size_t i=0;i<cache.vertexCookies.size();++i)
          std::printf("%s%llu",i?",":"",cache.vertexCookies[i]);
        std::printf("],\"pixelCookies\":[");
        for(size_t i=0;i<cache.pixelCookies.size();++i)
          std::printf("%s%llu",i?",":"",cache.pixelCookies[i]);
        std::printf("],\"includesPipelineOrDriverBytes\":false}\n");std::fflush(stdout);
        break;
      }
    }
    return S_OK;
  }
};
}
// The window a WSI session presents into belongs to the peer. Both processes
// compare its client extent with the back buffer, and those are the same
// numbers only when both see the window with the same DPI awareness - as the
// in-process device does by living in the game. So this process takes the
// peer's awareness, before it touches a window or the GPU. An unaware peer
// leaves it as it started.
static void AdoptPeerDpiAwareness(HANDLE peer){
  using Get=HRESULT(WINAPI*)(HANDLE,int*);using Set=HRESULT(WINAPI*)(int);
  const HMODULE shcore=LoadLibraryExW(L"shcore.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
  const auto get=shcore?reinterpret_cast<Get>(reinterpret_cast<void*>(GetProcAddress(shcore,"GetProcessDpiAwareness"))):nullptr;
  const auto set=shcore?reinterpret_cast<Set>(reinterpret_cast<void*>(GetProcAddress(shcore,"SetProcessDpiAwareness"))):nullptr;
  // PROCESS_DPI_AWARENESS: 0 unaware, 1 system, 2 per monitor.
  int theirs=0,own=0;HRESULT queried=E_NOTIMPL,applied=S_FALSE;
  if(get&&set) {
    queried=get(peer,&theirs);
    if(SUCCEEDED(queried)&&theirs!=0)applied=set(theirs);
    get(nullptr,&own);
  }
  std::fprintf(stderr,"D3D9 host dpi-awareness peer=%d queried=0x%08lx applied=0x%08lx own=%d\n",
    theirs,static_cast<unsigned long>(queried),static_cast<unsigned long>(applied),own);
  std::fflush(stderr);
}
int wmain(int argc,wchar_t**argv){
  const auto processStart=GetTickCount64();
  // Exit-delay measurement: registered after every static constructor, so
  // the CRT's exit runs it first; keep the TLS callback referenced.
  std::atexit([]{RawExitMilestone("crt-exit-begin");});
  (void)*static_cast<const volatile PIMAGE_TLS_CALLBACK*>(&warvkExitTlsCallback);
  MainExitWitness finalLocals{processStart};
  std::unique_ptr<Session> session;
  const auto reportFailedAccounts=[&]{if(session){ExitText out;session->reportFixedDelta(out);session->reportAsyncResources(out);session->reportWater(out);}};
  try{
    Require(argc>=4&&argc<=6,"nonce parent-pid parent-creation [inherited-parent-handle [resource-fixture]]");
    enum class Fixture {None,OneSlot,Allocation,AfterApply,DeviceLost,Delayed};
    Fixture fixture=Fixture::None;
    if(argc==6) {
      // Only the isolated raw fixture supplies this argument. Neither the
      // product proxy nor any wire message can request a synthetic outcome.
      Require(d9::ReadGameProfile()==d9::GameProfile::None,"resource fixture excludes game profiles");
      d9::RequireIsolatedDesktop();
      const std::wstring arg=argv[5];
      if(arg==L"--test-resource-one-slot")fixture=Fixture::OneSlot;
      else if(arg==L"--test-resource-allocation")fixture=Fixture::Allocation;
      else if(arg==L"--test-resource-after-apply")fixture=Fixture::AfterApply;
      else if(arg==L"--test-resource-device-lost")fixture=Fixture::DeviceLost;
      else if(arg==L"--test-resource-delay")fixture=Fixture::Delayed;
      else Require(false,"unknown resource fixture");
    }
    const auto nonce=lab::ParseNonce(argv[1]);const auto pid=std::stoull(argv[2]);
    Require(pid>0&&pid<=UINT32_MAX,"parent PID range");
    // A parent that shares itself is adopted; otherwise it is opened by PID.
    auto parent=argc>=5?lab::AdoptPeer(std::stoull(argv[4]),DWORD(pid),std::stoull(argv[3]))
      :lab::OpenPeer(DWORD(pid),std::stoull(argv[3]));
    AdoptPeerDpiAwareness(parent.get());
    const auto surfaceMode=surface::ReadStartupMode();
    std::fprintf(stderr,"D3D9 host dev surface-owner mode=%s productIntegrated=false\n",surface::ModeName(surfaceMode));
    std::fflush(stderr);
    auto pipe=lab::CreateServer(nonce,lab::Channel::D3D9Dev5);lab::IoStats io;lab::Accept(pipe.get(),parent.get(),io);lab::VerifyPeer(pipe.get(),true,DWORD(pid));
    Require(!GetModuleHandleW(L"Game.dll"),"game forbidden");
    session=std::make_unique<Session>(DWORD(pid),nonce,parent.get(),surfaceMode);
    if(fixture==Fixture::OneSlot)session->textureSlots=1;
    // The table never grows after this: a create is committed after its GPU
    // object exists, where running out of memory could no longer be refused.
    Require(session->registry.reserveAll(),"D3D9 resource table memory");
    wchar_t admissionMode[8]{};
    const DWORD admissionLength=GetEnvironmentVariableW(L"WARVK_D3D9_ADMISSION_PROFILE",admissionMode,8);
    Require(!admissionLength||(admissionLength==1&&admissionMode[0]==L'1'),"unknown admission diagnostic mode");
    session->admissionProfile=admissionLength!=0;
    const bool profile=session->admissionProfile;auto& admission=session->admission;
    d9::Packet request{},response{};uint64_t sequence=1;bool initializationLogged=false;
    // Command ring (d3d9_command_ring.h): attached by the one message after a
    // successful Initialize; from its reply on, every message arrives here.
    std::unique_ptr<d9::ring::PeerChannel> ringChannel;std::unique_ptr<d9::ring::Consumer> ring;
    bool attachAllowed=false;uint64_t ringRecords=0,ringSyncRecords=0,ringParks=0,ringWakes=0;
    HostProfile hostProfile; // P0 measurement only
    while(!session->finished){
      const auto profileIdle=d9::profile::Ticks();
      const auto idleBegin=profile?Clock::now():Clock::time_point{};
      auto readBegin=idleBegin;d9::Header h;
      const bool fromRing=ring!=nullptr;bool ringSync=false;
      if(!fromRing) {
        // Only D3D9dev opts in: a live idle producer is not a truncated frame.
        // The first received byte starts the unchanged shared 5 s header/body
        // window, so sending one byte cannot hold an incomplete frame forever.
        lab::ReadFirstByteAfterIdle(pipe.get(),parent.get(),request.data(),io);
        if(profile)readBegin=Clock::now();
        const lab::ReadWindow deadline;lab::ReadExact(pipe.get(),parent.get(),request.data()+1,d9::HeaderBytes-1,deadline,lab::Stage::Header,io);
        Require(d9::DecodeHeader(request.data(),h),"D3D9 header invalid");
        Require(h.nonce==nonce&&h.sequence==sequence&&sequence!=UINT64_MAX,"D3D9 scope/sequence");
        lab::ReadExact(pipe.get(),parent.get(),request.data()+d9::HeaderBytes,h.payloadBytes,deadline,lab::Stage::Body,io);
      } else {
        // Poll briefly, then say so and sleep. The producer signals `work`
        // only when it sees `parked`; the timeout is a recheck, not a failure.
        for(uint32_t polls=0;;) {
          const auto status=ring->tryRead(request,h,ringSync);
          if(status==d9::ring::Consumer::Read::Item)break;
          if(status==d9::ring::Consumer::Read::Closed)throw lab::Failure(lab::Fault::EndOfStream,lab::Stage::Header);
          if(status==d9::ring::Consumer::Read::Faulted) {
            // Nothing after a refused record is executed. Tell x86 why, then stop.
            const auto fault=ring->fault();
            ring->publishFault(fault,0,ring->expected());SetEvent(ringChannel->progress());
            std::printf("{\"schema\":\"warvk-d3d9-command-ring-fault/v1\",\"fault\":\"%s\",\"expectedSequence\":%llu,"
              "\"records\":%llu}\n",d9::ring::FaultName(fault),ring->expected(),ringRecords);std::fflush(stdout);
            Require(false,"D3D9 command ring refused");
          }
          if(polls<d9::ring::SpinPolls){++polls;YieldProcessor();continue;}
          ring->setParked(true);
          const auto again=ring->tryRead(request,h,ringSync);
          if(again!=d9::ring::Consumer::Read::Empty) {
            // Item is taken below; Closed and Faulted are met again by the next poll.
            ring->setParked(false);if(again==d9::ring::Consumer::Read::Item)break;
            continue;
          }
          ++ringParks;HANDLE waits[2]={ringChannel->work(),parent.get()};
          const auto sleepBegin=d9::profile::Clock();
          const auto waited=WaitForMultipleObjects(2,waits,FALSE,d9::ring::RecheckMs);
          ring->setParked(false);
          {
            // Lost-wakeup measurement: unvalidated peek; tryRead stays the reader.
            const auto tail=ring->peekTail();const bool progress=tail!=ring->consumed();
            d9::profile::WakeEvent e;e.begin=sleepBegin;e.end=d9::profile::Clock();e.status=waited;
            e.op=hostProfile.lastReplyOp;e.sequence=ring->expected();e.before=ring->consumed();e.after=tail;
            e.peerFlag=ring->producerWaiting()?1u:0u;e.signaled=hostProfile.lastReplySignaled;
            hostProfile.wake.ended(e,waited==WAIT_OBJECT_0,waited==WAIT_OBJECT_0+1,progress,false);
            ++hostProfile.loopSleeps;hostProfile.loopTimeouts+=waited==WAIT_TIMEOUT;hostProfile.loopStatus=waited;
          }
          if(waited==WAIT_OBJECT_0+1)throw lab::Failure(lab::Fault::PeerExited,lab::Stage::Header);
          if(waited!=WAIT_OBJECT_0&&waited!=WAIT_TIMEOUT)throw lab::Failure(lab::Fault::System,lab::Stage::Header,GetLastError());
          polls=0;
        }
        Require(h.sequence==sequence,"D3D9 ring sequence");
        // The read above returned this record's ring space. A producer asleep
        // for space is told now rather than after the record is applied (which
        // may take long); the notification after the apply stays as it is.
        if(ring->producerWaitingSpace())SetEvent(ringChannel->progress());
        ++ringRecords;if(ringSync)++ringSyncRecords;
        if(hostProfile.lastReply) {
          // >= 500 ms from a synchronous reply to the next record: which side waited.
          const auto now=d9::profile::Ticks();
          if(now-hostProfile.lastReply>=hostProfile.frequency/2) {
            ++hostProfile.wake.slowTurnarounds;
            d9::profile::WakeEvent e;e.kind=uint32_t(d9::profile::WakeKind::SlowTurnaround);e.begin=hostProfile.lastReply;e.end=now;
            e.status=hostProfile.loopStatus;e.op=hostProfile.lastReplyOp;e.sequence=hostProfile.lastReplySequence;
            e.before=hostProfile.loopSleeps;e.after=hostProfile.loopTimeouts;
            e.peerFlag=ring->producerWaiting()?1u:0u;e.signaled=hostProfile.lastReplySignaled;
            hostProfile.wake.record(e);
          }
          hostProfile.lastReply=0;
        }
        hostProfile.loopSleeps=hostProfile.loopTimeouts=hostProfile.loopStatus=0;
        if(profile)readBegin=Clock::now();
      }
      const auto profileApply=d9::profile::Ticks();hostProfile.idleTicks+=uint64_t(profileApply-profileIdle);
      const auto applyBegin=profile?Clock::now():Clock::time_point{};
      const bool batchOp=h.op==d9::Op::DrawBatch||d9::IsFixedBatch(h.op);
      const bool deltaOp=h.op==d9::Op::DrawFixedDeltaBatch;
      if(deltaOp)++session->deltaMessages;
      if(profile) {
        const auto read=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(applyBegin-readBegin).count());
        admission.idleNs+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(readBegin-idleBegin).count());
        admission.readNs+=read;
        if(batchOp)admission.batchReadNs+=read;
      }
      uint32_t bytes=0;HRESULT hr=E_UNEXPECTED;
      if(h.op==d9::Op::Initialize&&!initializationLogged) {
        initializationLogged=true;const auto initStart=GetTickCount64();
        std::fprintf(stderr,"D3D9 host init-apply begin pid=%lu sequence=%llu processElapsedMs=%llu\n",
          GetCurrentProcessId(),h.sequence,initStart-processStart);std::fflush(stderr);
        try { hr=session->apply(h,request.data()+d9::HeaderBytes,response,bytes); }
        catch(...) {
          std::fprintf(stderr,"D3D9 host init-apply exception elapsedMs=%llu\n",GetTickCount64()-initStart);
          std::fflush(stderr);throw;
        }
        std::fprintf(stderr,"D3D9 host init-apply complete result=%lu elapsedMs=%llu\n",DWORD(hr),GetTickCount64()-initStart);
        std::fflush(stderr);
      } else if(h.op==d9::Op::AttachRing) {
        // The session's regions are named by its nonce; a refused attach leaves
        // the pipe in use. A region that cannot be opened or is not fresh throws.
        bool exact=attachAllowed&&!fromRing&&!h.id&&!h.payloadBytes&&d9::ring::ValidCapacity(h.args[0])
          &&h.args[1]==d9::ring::ReplyCapacity&&h.args[2]==d9::ring::Version;
        for(size_t i=3;i<h.args.size();++i)exact=exact&&!h.args[i];
        hr=D3DERR_INVALIDCALL;
        if(exact) {
          ringChannel=std::make_unique<d9::ring::PeerChannel>(nonce,h.args[0]);
          ring=std::make_unique<d9::ring::Consumer>(ringChannel->producerBlock(),ringChannel->data(),
            ringChannel->consumerBlock(),ringChannel->reply(),ringChannel->capacity(),nonce,h.sequence);
          hr=D3D_OK;
          std::fprintf(stderr,"D3D9 host command-ring attached capacity=%u sequence=%llu\n",ringChannel->capacity(),h.sequence);
          std::fflush(stderr);
        }
      } else {
        // apply can mutate upload/annotation state even before opcode dispatch.
        const auto feature=d9::AsyncResourceFeature(h.op);
        if(fromRing&&!ringSync&&feature) {
          if(!session->admitAsyncResource(h.op,feature))hr=D3DERR_INVALIDCALL;
          else if(fixture==Fixture::DeviceLost)hr=D3DERR_DEVICELOST;
          else {
            if(fixture==Fixture::Allocation)throw std::bad_alloc();
            if(fixture==Fixture::Delayed)Sleep(2500);
            hr=session->apply(h,request.data()+d9::HeaderBytes,response,bytes);
            if(fixture==Fixture::AfterApply&&hr==D3D_OK)throw std::runtime_error("fixture stopped after resource apply");
          }
        } else hr=session->apply(h,request.data()+d9::HeaderBytes,response,bytes);
      }
      hostProfile.applied(h.op,profileApply,d9::profile::Ticks());
      if(deltaOp) {
        if(hr!=D3D_OK)++session->deltaRejected;
        else {session->deltaRecords+=h.args[0];if(h.args[1])session->deltaVerified+=h.args[0];}
      }
      attachAllowed=h.op==d9::Op::Initialize&&hr==D3D_OK;
      const auto replyBegin=profile?Clock::now():Clock::time_point{};
      if(profile) {
        const auto applied=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(replyBegin-applyBegin).count());
        ++admission.messages;
        if(batchOp){++admission.batchMessages;admission.batchApplyNs+=applied;}
        else if(h.op==d9::Op::FlushDraws)admission.flushApplyNs+=applied;
        else if(h.op==d9::Op::WaitEvent)admission.waitEventApplyNs+=applied;
        else admission.otherApplyNs+=applied;
      }
      if(SUCCEEDED(hr))++session->accepted;else ++session->rejected;
      const auto&s=session->stats();d9::Reply r{uint32_t(hr),s.submitted,s.gpuCompleted,s.retired,s.draws,bytes};
      if(!fromRing) {
        d9::EncodeReply(h,r,response);lab::WriteBytes(pipe.get(),parent.get(),response.data(),d9::ReplyBytes+bytes,io);
      } else {
        // A synchronous record gets its reply; an asynchronous one may only
        // succeed. Either way x86 learns of a lost device before it is told the
        // record was executed, and a refused asynchronous record never is.
        // Two asynchronous outcomes are not refusals. A Present of a window
        // resized ahead of its Reset drops the frame; x86 reports success for
        // that either way. A refused scene record stops the scene records,
        // not the session: the game's own picture does not depend on them.
        const bool skippedPresent=!ringSync&&h.op==d9::Op::Present&&hr==HRESULT(d9::PresentSkipped);
        const bool sceneRefusal=!ringSync&&hr!=D3D_OK&&hr!=D3DERR_DEVICELOST
          &&(h.op==d9::Op::FrameScene||h.op==d9::Op::StageMarker);
        if(sceneRefusal) {
          ring->publishSceneRefused();session->sceneStopped=true;
          if(!session->sceneRefusals++) {
            // A queued WorldSceneExit will be ignored. Abandon the partial
            // frame now so it cannot hold the next map generation open.
            session->clearScene();session->sceneTracker.active=false;session->sceneTracker.frameEnd();
            std::fprintf(stderr,"D3D9 host scene record refused op=%u sequence=%llu result=0x%08lx; later scene records are ignored\n",
              uint32_t(h.op),h.sequence,static_cast<unsigned long>(hr));std::fflush(stderr);
          }
        }
        const bool refused=!ringSync&&hr!=D3D_OK&&!skippedPresent&&!sceneRefusal;
        if(hr==D3DERR_DEVICELOST)ring->publishFault(d9::ring::Fault::DeviceLost,uint32_t(hr),h.sequence);
        else if(refused)ring->publishFault(d9::ring::Fault::AsyncRefused,uint32_t(hr),h.sequence);
        if(ringSync)ring->writeReply(h,r,response.data()+d9::ReplyBytes);
        if(!refused) {
          ring->publishApplied(h.sequence,s.submitted,s.gpuCompleted,s.retired,s.draws);
          if(h.op==d9::Op::Finish&&SUCCEEDED(hr))ring->publishFinished();
        }
        const bool producerWaiting=ring->producerWaiting();
        ++hostProfile.wake.notifyChecks;hostProfile.wake.notified+=producerWaiting;
        if(refused||hr==D3DERR_DEVICELOST||producerWaiting){++ringWakes;SetEvent(ringChannel->progress());}
        if(ringSync) {
          hostProfile.lastReply=d9::profile::Ticks();hostProfile.lastReplySequence=h.sequence;
          hostProfile.lastReplyOp=uint32_t(h.op);hostProfile.lastReplySignaled=producerWaiting?1u:0u;
        }
        if(refused&&hr!=D3DERR_DEVICELOST) {
          std::printf("{\"schema\":\"warvk-d3d9-command-ring-fault/v1\",\"fault\":\"async-refused\",\"expectedSequence\":%llu,"
            "\"records\":%llu}\n",h.sequence,ringRecords);std::fflush(stdout);
          // Which record and why: the row above is pinned by the gates.
          std::fprintf(stderr,"D3D9 host asynchronous record refused op=%u sequence=%llu result=0x%08lx\n",
            uint32_t(h.op),h.sequence,static_cast<unsigned long>(hr));std::fflush(stderr);
          Require(false,"D3D9 asynchronous ring record refused");
        }
      }
      ++sequence;
      if(h.op==d9::Op::Present&&hostProfile.every&&hostProfile.presents%hostProfile.every==0)
        hostProfile.snapshot("periodic",session->stats(),ringRecords,ringSyncRecords,ringParks,ringWakes,
          session->annotationRecorder.frameScenes,session->sceneTracker.enters);
      if(profile) {
        const auto replied=ElapsedNs(replyBegin);
        admission.replyNs+=replied;
        if(batchOp)admission.batchReplyNs+=replied;
        if(h.op==d9::Op::Issue&&SUCCEEDED(hr))session->sampleAdmission();
      }
      if(h.op==d9::Op::Finish)ExitMilestone("finish-reply-written",processStart);
      if(hr==D3DERR_DEVICELOST) {
        std::printf("{\"schema\":\"warvk-d3d9-host-fault/v1\",\"ok\":false,\"fault\":\"gpu-or-window-terminal\","
          "\"faultKind\":\"%s\",\"gpuCleanupCompleted\":%s,\"gpuCleanupMs\":%llu,\"exitStrategy\":\"%s\","
          "\"shutdownQueueDrained\":%s,"
          "\"result\":%lu,\"quarantined\":%s,\"normalTeardown\":false,\"gpuInitialized\":%s,"
          "\"bits\":64,\"actualDxvkCore\":true,\"actualDxso\":true,\"accepted\":%llu,\"rejected\":%llu,"
          "\"backendCalls\":%llu,\"presentAttempts\":%llu,\"presentApiSucceeded\":%llu,\"presentRetired\":%llu,"
          "\"acquiredImages\":%llu,\"presentQueued\":%llu,\"presentGpuCompleted\":%llu,"
          "\"submitted\":%llu,\"gpuCompleted\":%llu,\"retired\":%llu,\"draws\":%llu,\"clears\":%llu,"
          "\"liveResources\":%u,\"liveLogicalBytes\":%llu,"
          "\"windowProcessId\":%u,\"windowThreadId\":%u,"
          "\"presentedReadbacks\":%llu,\"presentedFrame\":%llu,",
          s.windowPreflightFailed?"window-preflight":"gpu-or-window-terminal",s.faultCleanupCompleted?"true":"false",
          s.faultCleanupMs,s.faultCleanupCompleted?"owner-cleanup-return":"self-fail-stop",
          s.shutdownQueueDrained?"true":"false",
          DWORD(hr),s.quarantined?"true":"false",s.initialized?"true":"false",session->accepted,session->rejected,session->backendCalls,
          s.presentAttempts,s.presentApiSucceeded,s.presentRetired,s.acquiredImages,s.presentQueued,s.presentGpuCompleted,
          s.submitted,s.gpuCompleted,s.retired,s.draws,s.clears,s.liveResources,s.liveLogicalBytes,
          s.windowProcessId,s.windowThreadId,s.presentedReadbacks,s.presentedFrame);
        session->handleStats().print();
        std::printf("\"productIntegrated\":false,\"gameTouched\":false,\"memorySavingsProven\":false}\n");
        std::fflush(stdout);std::fflush(stderr);
        reportFailedAccounts();
        if(s.faultCleanupCompleted) {
          session.reset();LogExitIntent("owner-cleanup-return",2,processStart);return 2;
        }
        (void)session.release();SelfFailStop(processStart);
      }
    }
    const auto s=session->stats();const auto accepted=session->accepted,rejected=session->rejected,calls=session->backendCalls;
    const auto handles=session->handleStats();
    Require(s.normalTeardown,"normal DXVK teardown");
    RawExitMilestone("exit-accounts-begin");
    hostProfile.snapshot("exit",s,ringRecords,ringSyncRecords,ringParks,ringWakes,
      session->annotationRecorder.frameScenes,session->sceneTracker.enters);
    RawExitMilestone("exit-profile-written");
    ExitText exitOut;exitOut.buffered=ExitSwitch(L"WARVK_D3D9_HOST_EXIT_BUFFERED",L"0");
    exitOut.add("D3D9 draw-batch-host: {\"schema\":\"warvk-d3d9-draw-batch-host/v1\","
      "\"batches\":%llu,\"batchedDraws\":%llu,\"largestBatch\":%u,\"constantCapacityBytes\":%llu,"
      "\"draws\":%llu,\"submitted\":%llu,\"gpuCompleted\":%llu,\"retired\":%llu,"
      "\"flightCapacity\":%u,\"flightSubmissions\":%llu,\"flightReuses\":%llu,"
      "\"flightRetirements\":%llu,\"maxOccupiedFlightSlots\":%u,\"submissionGroupCapacity\":%u,"
      "\"groupedSubmissions\":%llu,\"nonIndexedDraws\":%llu,\"batchStateReuse\":%u,\"redundantStateBindingsSkipped\":%llu}\n",
      s.drawBatches,s.batchedDraws,s.largestDrawBatch,s.batchConstantBytes,s.draws,s.submitted,s.gpuCompleted,s.retired,
      s.flightCapacity,s.flightSubmissions,s.flightReuses,s.flightRetirements,s.maxOccupiedFlightSlots,
      s.submissionGroupCapacity,s.groupedSubmissions,s.nonIndexedDraws,s.batchStateReuse,s.redundantStateBindingsSkipped);
    // Separate versioned line: the frozen v1 batch schema above is unchanged.
    if (s.streamMode)
      exitOut.add("D3D9 stream-host: {\"schema\":\"warvk-d3d9-stream-host/v1\","
        "\"streamDraws\":%llu,\"streamFixedDraws\":%llu,\"streamSubmissions\":%llu,\"streamCapSubmissions\":%llu,"
        "\"largestStreamList\":%u,\"maxStreamDraws\":%u,\"maxStreamLatency\":%u,\"constantRenames\":%llu,"
        "\"latencyWaits\":%llu,\"ringBytes\":%llu,\"submitted\":%llu,\"gpuCompleted\":%llu,\"retired\":%llu}\n",
        s.streamDraws,s.streamFixedDraws,s.streamSubmissions,s.streamCapSubmissions,s.largestStreamList,
        GPU::MaxStreamDraws,GPU::MaxStreamLatency,s.streamConstantRenames,s.streamLatencyWaits,s.batchConstantBytes,
        s.submitted,s.gpuCompleted,s.retired);
    // Printed for every profile: zero when no dynamic buffer was written.
    exitOut.add("D3D9 dynamic-buffer-host: {\"schema\":\"warvk-d3d9-dynamic-buffer-host/v1\","
      "\"dynamicBuffers\":%llu,\"orderedUploads\":%llu,\"noOverwriteUploads\":%llu,\"discardUploads\":%llu,"
      "\"discardRenames\":%llu,\"discardCarriedBytes\":%llu,\"uploadSettles\":%llu}\n",
      s.dynamicBuffers,s.orderedDynamicUploads,s.noOverwriteUploads,s.discardUploads,
      s.discardRenames,s.discardCarriedBytes,s.uploadSettles);
    // Printed for every profile: zero resets and skips when unused. Window
    // resets are the resets of the window's own resize, which kept every resource.
    exitOut.add("D3D9 reset-host: {\"schema\":\"warvk-d3d9-reset-host/v1\",\"resets\":%llu,"
      "\"windowResets\":%llu,\"presentsSkipped\":%llu,\"width\":%u,\"height\":%u,\"opaqueTarget\":%s,\"syncInterval\":%u}\n",
      s.resets,s.windowResets,s.presentsSkipped,s.width,s.height,s.opaqueTarget?"true":"false",s.syncInterval);
    // Accepted fixed batches; printed for every profile, zero if unused.
    exitOut.add("D3D9 fixed-batch-host: {\"schema\":\"warvk-d3d9-fixed-batch-host/v1\","
      "\"batches\":%llu,\"draws\":%llu,\"largestBatch\":%u,\"capacity\":%u}\n",
      session->fixedBatches,session->fixedBatchDraws,session->largestFixedBatch,d9::MaxFixedDrawBatch);
    session->reportFixedDelta(exitOut);session->reportAsyncResources(exitOut);
    session->reportWater(exitOut);
    exitOut.add("D3D9 backing-host: {\"schema\":\"warvk-d3d9-backing-host/v1\",\"retainContent\":%s,"
      "\"retainedBytes\":%llu,\"peakRetainedBytes\":%llu,\"contentReads\":%llu,\"contentReadBytes\":%llu}\n",
      session->retainContent?"true":"false",session->retainedBytes,session->peakRetainedBytes,
      session->contentReads,session->contentReadBytes);
    if(ring)
      exitOut.add("D3D9 command-ring-host: {\"schema\":\"warvk-d3d9-command-ring-host/v1\",\"capacity\":%u,"
        "\"records\":%llu,\"syncRecords\":%llu,\"parks\":%llu,\"wakes\":%llu}\n",
        ringChannel->capacity(),ringRecords,ringSyncRecords,ringParks,ringWakes);
    // Only a negotiated session prints its annotation account.
    if(session->annotations) {
      const auto& a=session->annotationRecorder;
      const auto& t=session->sceneTracker;
      exitOut.add("D3D9 annotation-host: {\"schema\":\"warvk-d3d9-annotation-host/v1\",\"annotatedDraws\":%llu,"
        "\"drawRecords\":%llu,\"frameScenes\":%llu,\"stageMarkers\":%llu,\"worldSceneEnters\":%llu,"
        "\"worldSceneExits\":%llu,\"worldSceneActive\":%s,\"sessionResets\":%llu,\"sessionGeneration\":%llu,"
        "\"digest\":\"%016llx\"}\n",
        a.annotatedDraws,a.drawRecords,a.frameScenes,a.stageMarkers,t.enters,t.exits,t.active?"true":"false",
        a.sessionResets,t.generation,a.digest);
      // Only a session that received effect records prints their account.
      if(session->effectRecords)
        exitOut.add("D3D9 scene-effects-host: {\"schema\":\"warvk-d3d9-scene-effects-host/v1\",\"effectRecords\":%llu,"
          "\"beforeUiRuns\":%llu,\"ssaoRecorded\":%llu,\"snowRecorded\":%llu,\"aaRecorded\":%llu,"
          "\"bloomRecorded\":%llu,\"insertionsRefused\":%llu,"
          "\"lastSsaoStage\":%u,\"lastSnowStage\":%u,\"lastAaStage\":%u,\"lastBloomStage\":%u,"
          "\"beforeUiNs\":%llu,\"resourceBytes\":%llu}\n",
          session->effectRecords,session->effectFrames,session->ssaoRecorded,session->snowRecorded,
          session->aaRecorded,session->bloomRecorded,
          session->effectInsertionsRefused,session->lastSsaoStage,session->lastSnowStage,
          session->lastAaStage,session->lastBloomStage,
          session->effectNs,session->effectResourceBytes);
      // Only a session that received shadow records or casters prints theirs.
      if(session->shadowRecords||session->shadowCasterRecords) {
        // One run, as the in-process fixture prints its own (test_render_host_d3d9_shadow.cpp).
        const auto printRun=[&exitOut](const dxvkcore::ShadowOutcome& s) {
          uint32_t time=0;std::memcpy(&time,&s.gameTime,4);
          exitOut.add("{\"casters\":%u,\"replayDraws\":%u,\"drawnCasters\":%u,\"cascades\":%u,"
            "\"cascadeDrawn\":[%u,%u,%u,%u],\"mapExecuted\":%u,\"receiverExecuted\":%u,"
            "\"earlyReturn\":%u,\"inputReject\":%u,\"replayRejected\":%u,\"completeMap\":%u,"
            "\"alphaPrepared\":%u,"
            "\"taaModule\":%u,\"taaMode\":%u,\"taaHistory\":%u,\"taaInvalidation\":%u,\"pointShadow\":%u,\"outline\":%u,"
            "\"volumetric\":%u,\"volumetricBackend\":%d,\"volumetricComposite\":%u,\"volumetricBytes\":%llu,"
            "\"volumetricFogVolumes\":%u,\"volumetricWeather\":%u,"
            "\"gameTimeBits\":\"%08x\",\"tombstoneSerial\":%llu,"
            "\"stagePolicyRevision\":%llu,\"resourceBytes\":%llu}",
            s.casters,s.replayDraws,s.drawnCasters,s.cascades,s.cascadeDrawn[0],s.cascadeDrawn[1],
            s.cascadeDrawn[2],s.cascadeDrawn[3],s.mapExecuted,s.receiverExecuted,s.earlyReturn,s.inputReject,
            s.replayRejected,s.completeMap,s.alphaPrepared,s.taaModule,s.taaMode,s.taaHistory,s.taaInvalidation,
            s.pointShadow,s.outline,s.volumetric,s.volumetricBackend,s.volumetricComposite,
            static_cast<unsigned long long>(s.volumetricBytes),s.volumetricFogVolumes,s.volumetricWeather,time,
            static_cast<unsigned long long>(s.tombstoneSerial),static_cast<unsigned long long>(s.stagePolicyRevision),
            static_cast<unsigned long long>(s.resourceBytes));
        };
        exitOut.add("D3D9 shadow-host: {\"schema\":\"warvk-d3d9-shadow-host/v2\",\"shadowRecords\":%llu,"
          "\"casterRecords\":%llu,\"alphaRecords\":%llu,\"runs\":%llu,\"mapRenders\":%llu,\"receiverDraws\":%llu,"
          "\"castersUsed\":%llu,\"castersRefused\":%llu,\"refusedDynamic\":%llu,\"refusedLayout\":%llu,"
          "\"refusedRange\":%llu,\"castersCopied\":%llu,\"copiedBytes\":%llu,"
          "\"castersDrawn\":%llu,\"replayRejected\":%llu,"
          "\"peakResourceBytes\":%llu,\"last\":",
          session->shadowRecords,session->shadowCasterRecords,session->shadowAlphaRecords,session->shadowRuns,
          session->shadowMapRenders,
          session->shadowReceiverDraws,session->shadowCastersUsed,session->shadowCastersRefused,
          session->shadowRefusals[1],session->shadowRefusals[2],session->shadowRefusals[3],
          session->shadowCastersCopied,session->shadowCopiedBytes,
          session->shadowCastersDrawn,session->shadowReplayRejected,
          static_cast<unsigned long long>(session->shadowPeakResourceBytes));
        printRun(session->lastShadow);
        exitOut.add(",\"runLog\":[");
        for(size_t i=0;i<session->shadowRunLog.size();++i){if(i)exitOut.add(",");printRun(session->shadowRunLog[i]);}
        exitOut.add("],\"deviceMemoryLog\":[");
        for(size_t i=0;i<session->shadowDeviceLog.size();++i)
          exitOut.add("%s[%llu,%llu]",i?",":"",static_cast<unsigned long long>(session->shadowDeviceLog[i][0]),
            static_cast<unsigned long long>(session->shadowDeviceLog[i][1]));
        exitOut.add("]}\n");
      }
      const auto& store=session->content;const auto& index=store.index;
      exitOut.add("D3D9 content-host: {\"schema\":\"warvk-d3d9-content-host/v1\",\"blobs\":%llu,\"bytes\":%llu,"
        "\"peakBlobs\":%llu,\"peakBytes\":%llu,\"uploads\":%llu,\"replaced\":%llu,\"released\":%llu,\"aborted\":%llu,"
        "\"cleared\":%llu,\"chunks\":%llu,\"uploadedBytes\":%llu,\"reads\":%llu,\"readBytes\":%llu,\"boundRefs\":%llu,"
        "\"refusedRefs\":%llu,\"lastRevision\":%llu}\n",
        static_cast<unsigned long long>(index.count()),index.bytes(),index.peakBlobs,index.peakBytes,index.committedBlobs,
        index.replacedBlobs,index.releasedBlobs,index.abortedBlobs,index.clearedBlobs,store.chunks,store.uploadedBytes,
        store.reads,store.readBytes,store.boundRefs,store.refusedRefs,index.lastRevision());
    }
    if (s.reuseWaitProfile) {
      for (uint32_t i = 0; i < s.reuseWaitSampleCount; ++i) {
        const auto& sample = s.reuseWaitSamples[i];
        const auto& wait = sample.cumulative;
        exitOut.add("D3D9 reuse-wait: {\"schema\":\"warvk-d3d9-reuse-wait/v1\","
          "\"query\":%llu,\"draws\":%llu,\"episodes\":%llu,\"gpuCalls\":%llu,"
          "\"totalNs\":%llu,\"cpuSubmissionNs\":%llu,\"gpuWaitNs\":%llu,\"retirementNs\":%llu,"
          "\"dropped\":%u}\n", sample.query, sample.draws, wait.episodes, wait.gpuCalls,
          wait.totalNs, wait.cpuSubmissionNs, wait.gpuWaitNs, wait.retirementNs, s.reuseWaitSamplesDropped);
      }
    }
    for(uint32_t i=0;i<session->admissionSampleCount;++i) {
      const auto& sample=session->admissionSamples[i];const auto& a=sample.cumulative;
      exitOut.add("D3D9 admission-host: {\"schema\":\"warvk-d3d9-admission-host/v1\",\"query\":%llu,"
        "\"messages\":%llu,\"idleNs\":%llu,\"readNs\":%llu,\"replyNs\":%llu,\"batchMessages\":%llu,\"batchDraws\":%llu,"
        "\"batchReadNs\":%llu,\"batchDecodeNs\":%llu,\"batchApplyNs\":%llu,\"batchReplyNs\":%llu,"
        "\"prepareNs\":%llu,\"recordNs\":%llu,\"constantNs\":%llu,\"bindNs\":%llu,\"samplerNs\":%llu,\"drawNs\":%llu,"
        "\"flushApplyNs\":%llu,\"waitEventApplyNs\":%llu,\"otherApplyNs\":%llu,\"dropped\":%u}\n",
        sample.query,a.messages,a.idleNs,a.readNs,a.replyNs,a.batchMessages,a.batchDraws,a.batchReadNs,a.batchDecodeNs,
        a.batchApplyNs,a.batchReplyNs,a.prepareNs,a.recordNs,a.constantNs,a.bindNs,a.samplerNs,a.drawNs,a.flushApplyNs,a.waitEventApplyNs,a.otherApplyNs,
        session->admissionSamplesDropped);
    }
    if(s.streamFlushDraws)
      exitOut.add("D3D9 host stream-cadence: flushDraws=%u submissions=%llu\n",s.streamFlushDraws,s.streamCadenceSubmissions);
    if(session->sceneRefusals)
      exitOut.add("D3D9 host scene-refusals: refused=%llu ignored=%llu\n",session->sceneRefusals,session->sceneRecordsIgnored);
    exitOut.flush();
    // The teardown was normal either way; whether its accounts all reached
    // stderr is a separate fact and is said separately.
    if(!exitOut.complete)RawExitMilestone("exit-accounts-incomplete");
    ExitMilestone("before-session-reset",processStart);session.reset();
    ExitMilestone("after-session-reset",processStart);
    std::printf("{\"schema\":\"warvk-d3d9-host/v1\",\"ok\":true,\"bits\":64,\"actualDxvkCore\":true,\"actualDxso\":true,"
      "\"accepted\":%llu,\"rejected\":%llu,\"backendCalls\":%llu,\"submitted\":%llu,\"gpuCompleted\":%llu,\"retired\":%llu,"
      "\"draws\":%llu,\"clears\":%llu,\"uploads\":%llu,\"uploadedBytes\":%llu,\"readbacks\":%llu,\"queries\":%llu,"
      "\"buffersCreated\":%llu,\"texturesCreated\":%llu,\"shadersCompiled\":%llu,\"declarationsCreated\":%llu,"
      "\"liveResources\":%u,\"liveLogicalBytes\":%llu,\"peakLogicalBytes\":%llu,\"vendorId\":%u,\"deviceId\":%u,"
      "\"normalTeardown\":true,\"presentImplemented\":%s,\"presentAttempts\":%llu,\"acquiredImages\":%llu,"
      "\"presentQueued\":%llu,\"presentApiSucceeded\":%llu,\"presentGpuCompleted\":%llu,\"presentRetired\":%llu,"
      "\"lastAcquireVkResult\":%d,\"lastPresentVkResult\":%d,\"windowProcessId\":%u,\"windowThreadId\":%u,"
      "\"gpuInitialized\":%s,\"shutdownQueueDrained\":%s,"
      "\"presentedReadbacks\":%llu,\"presentedFrame\":%llu,",
      accepted,rejected,calls,s.submitted,s.gpuCompleted,s.retired,s.draws,s.clears,s.uploads,s.uploadedBytes,s.readbacks,s.queries,
      s.buffersCreated,s.texturesCreated,s.shadersCompiled,s.declarationsCreated,s.liveResources,s.liveLogicalBytes,s.peakLogicalBytes,s.vendorId,s.deviceId,
      s.presenterImplemented?"true":"false",s.presentAttempts,s.acquiredImages,s.presentQueued,s.presentApiSucceeded,
      s.presentGpuCompleted,s.presentRetired,s.lastAcquireVkResult,s.lastPresentVkResult,s.windowProcessId,s.windowThreadId,
      s.initialized?"true":"false",s.shutdownQueueDrained?"true":"false",s.presentedReadbacks,s.presentedFrame);
    handles.print();
    std::printf("\"productIntegrated\":false,\"gameTouched\":false,\"memorySavingsProven\":false}\n");
    if(ExitSwitch(L"WARVK_D3D9_HOST_EXIT_GL_WATCHER",L"keep"))ClosePaletteWatcher(processStart);
    LogExitIntent("normal-return",0,processStart);return 0;
  }catch(const lab::Failure&e){
    reportFailedAccounts();
    // The transport failure's own coordinates, ahead of the generic line.
    (void)session.release();
    std::fprintf(stderr,"D3D9 host transport: fault=%s stage=%s win32=%lu\n",lab::FaultName(e.fault),lab::StageName(e.stage),e.win32);
    std::fprintf(stderr,"D3D9 host failure: %s\n",e.what());SelfFailStop(processStart);}
  catch(const std::exception&e){reportFailedAccounts();(void)session.release();std::fprintf(stderr,"D3D9 host failure: %s\n",e.what());SelfFailStop(processStart);}
  catch(...){reportFailedAccounts();(void)session.release();std::fprintf(stderr,"D3D9 host failure: unknown exception\n");SelfFailStop(processStart);}
}
