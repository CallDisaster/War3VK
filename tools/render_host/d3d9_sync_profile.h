#pragma once
// P0 measurement only. Shared helpers of the front end's synchronous-call
// profile (warvk-d3d9-sync-profile/v1, d3d9_connection.h) and the host's
// profile (warvk-d3d9-host-profile/v1, dxvk_core/d3d9_host_main.cpp). Nothing
// here is sent on the wire, and nothing changes what is sent, when, or how a
// failure is handled: fixed tables, QueryPerformanceCounter ticks, one JSON
// line written with one call.
#include <windows.h>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include "d3d9_wire.h"

namespace warvk::host::d9::profile {
static_assert(uint32_t(Op::Present)==15&&uint32_t(Op::FrameScene)==36&&uint32_t(Op::ContentRead)==43&&uint32_t(Op::DrawFixedDeltaBatch)==44,
  "the op name table follows d9::Op");
constexpr size_t Ops=size_t(Op::DrawFixedDeltaBatch)+1;
inline const char* OpName(size_t op) noexcept {
  static constexpr const char* names[Ops]={"none",
    "Initialize","CreateBuffer","UploadBuffer","CreateTexture","UploadTexture","CreateShader","CreateDeclaration",
    "Clear","Draw","Readback","Issue","Poll","Destroy","Finish","Present","CreateQuery","DiagnosticCheckpoint",
    "DrawBatch","WaitEvent","FlushDraws","ClearRegions","FactoryQuery","DrawFixed","CreateTexture2D",
    "BeginTextureUpload","TextureUploadChunk","CommitTextureUpload","AbortTextureUpload","TextureLod",
    "SetSamplerState","SetEffects","SetTextureStages","DrawFixedBatch","ReadContent","AnnotateDraws","FrameScene",
    "StageMarker","Reset","AttachRing","SessionReset","ContentUpload","ContentRelease","ContentRead","DrawFixedDeltaBatch"};
  return op<Ops?names[op]:"unknown";
}
inline size_t OpIndex(Op op) noexcept { return size_t(op)<Ops?size_t(op):0; }
// The D3D9 or data-layer entry a front-end message was sent for. The outermost
// entry names it; the data layer's own exports always take their own name.
enum class SyncApi : uint8_t {
  Other, Initialize, Present, FrameDump, EndScene, Clear, Draw, DrawUP, BufferLock, BufferUnlock,
  TextureLock, TextureUnlock, TextureLod, CreateBuffer, CreateTexture, CreateShader, CreateDeclaration,
  CreateQuery, Release, GetRenderTargetData, QueryIssue, QueryGetData, Reset, WindowResize, Caps, Factory,
  Checkpoint, DeviceRelease, FrameScene, StageMarker, ContentUpload, ContentRelease, ContentRead, Count
};
constexpr size_t Apis=size_t(SyncApi::Count);
inline const char* ApiName(size_t api) noexcept {
  static constexpr const char* names[Apis]={"Other","Initialize","Present","FrameDump","EndScene","Clear","Draw",
    "DrawUP","BufferLock","BufferUnlock","TextureLock","TextureUnlock","TextureLod","CreateBuffer","CreateTexture",
    "CreateShader","CreateDeclaration","CreateQuery","Release","GetRenderTargetData","QueryIssue","QueryGetData",
    "Reset","WindowResize","Caps","Factory","Checkpoint","DeviceRelease","FrameScene","StageMarker",
    "ContentUpload","ContentRelease","ContentRead"};
  return api<Apis?names[api]:"unknown";
}
inline int64_t Clock() noexcept { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
// WARVK_D3D9_SYNC_PROFILE_TIMING=0 is the instrumentation-off control: every
// hot-path timer reads 0 (no QueryPerformanceCounter call), so all durations
// are 0 while the integer counters still count. Unset or anything else: timed.
// Snapshot timestamps use Clock() either way.
inline bool TimingEnabled() noexcept {
  static const bool enabled=[] {
    wchar_t value[4]{};
    const DWORD size=GetEnvironmentVariableW(L"WARVK_D3D9_SYNC_PROFILE_TIMING",value,4);
    return !(size==1&&value[0]==L'0');
  }();
  return enabled;
}
inline int64_t Ticks() noexcept { return TimingEnabled()?Clock():0; }
inline int64_t Frequency() noexcept { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v.QuadPart>0?v.QuadPart:1; }
inline uint64_t Micros(uint64_t ticks,int64_t frequency) noexcept {
  const auto f=uint64_t(frequency);
  return ticks/f*1000000u+ticks%f*1000000u/f;
}
inline uint64_t UnixMs() noexcept {
  FILETIME t;GetSystemTimeAsFileTime(&t);
  const uint64_t v=(uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;
  return v>116444736000000000ull?(v-116444736000000000ull)/10000u:0;
}
// WARVK_D3D9_SYNC_PROFILE_EVERY: a snapshot every N presents (1..1000000).
// Unset or malformed: 0, the exit snapshot only. Never a refusal.
inline uint64_t Every() noexcept {
  wchar_t value[16]{};
  const DWORD size=GetEnvironmentVariableW(L"WARVK_D3D9_SYNC_PROFILE_EVERY",value,16);
  if(!size||size>7)return 0;
  uint64_t n=0;
  for(DWORD i=0;i<size;++i){if(value[i]<L'0'||value[i]>L'9')return 0;n=n*10+uint64_t(value[i]-L'0');}
  return n<=1000000?n:0;
}
// One JSON line in a caller-owned fixed buffer. Entries that no longer fit
// are dropped whole (the line says so); closing tokens use a reserved tail.
class Line {
public:
  Line(char* buffer,size_t capacity) noexcept : m_buffer(buffer),m_capacity(capacity) { if(capacity)buffer[0]=0; }
#if defined(__GNUC__)||defined(__clang__)
  __attribute__((format(printf,2,3)))
#endif
  void add(const char* format,...) noexcept {
    va_list args;va_start(args,format);append(Reserve,format,args);va_end(args);
  }
#if defined(__GNUC__)||defined(__clang__)
  __attribute__((format(printf,2,3)))
#endif
  void close(const char* format,...) noexcept {
    va_list args;va_start(args,format);append(0,format,args);va_end(args);
  }
  bool truncated() const noexcept { return m_truncated; }
  void write(FILE* stream) const noexcept { if(m_used){std::fwrite(m_buffer,1,m_used,stream);std::fflush(stream);} }
private:
  static constexpr size_t Reserve=96;
  void append(size_t reserve,const char* format,va_list args) noexcept {
    if((reserve&&m_truncated)||m_used+reserve+1>=m_capacity){m_truncated=true;return;}
    const size_t room=m_capacity-m_used-reserve;
    const int n=std::vsnprintf(m_buffer+m_used,room,format,args);
    if(n<0||size_t(n)>=room){m_buffer[m_used]=0;m_truncated=true;return;}
    m_used+=size_t(n);
  }
  char* m_buffer;size_t m_capacity,m_used=0;bool m_truncated=false;
};
// x64 lost-wakeup measurement: how each sleep of one ring side ended. A side
// sleeps on its peer's notification with a RecheckMs timeout; a timeout that
// finds the awaited progress already published means a notification that
// should have woken it did not. Read-only peeks; nothing here changes when a
// side sleeps, signals or fails.
enum class WakeKind : uint32_t {
  TimeoutProgress = 0, // timed out, but the peer had already made the awaited progress
  TimeoutIdle = 1,     // timed out with nothing new (front: the host was busy)
  SlowTurnaround = 2,  // host: >= 500 ms from a synchronous reply to the next record
};
struct WakeEvent {
  int64_t begin=0,end=0;     // QueryPerformanceCounter, comparable across the two processes
  uint64_t sequence=0;       // front: awaited sequence (0: ring full); host: the record that arrived
  uint64_t before=0,after=0; // front: applied before/after; host: tail before/after (bytes)
  uint32_t kind=0,status=0,op=0;
  uint32_t peerFlag=0;       // front: host `parked` at wake; host: producer `waiting` at wake
  uint32_t signaled=0;       // front: the last publish signalled `work`; host: the last reply signalled `progress`
};
struct WakeLog {
  uint64_t sleeps=0,woken=0,wokenOther=0,wokenStale=0,timeouts=0,timeoutProgress=0,timeoutProgressPeerFlag=0;
  uint64_t notifyChecks=0,notified=0,slowTurnarounds=0;
  std::array<WakeEvent,64> events{};uint64_t recorded=0,printed=0;
  void record(const WakeEvent& e) noexcept { events[recorded++%events.size()]=e; }
  // `notifiedIndex`: the peer's event; anything else signalled is `other`.
  void ended(WakeEvent e,bool notifiedWake,bool otherWake,bool progress,bool keepIdle) noexcept {
    ++sleeps;
    if(e.status==WAIT_TIMEOUT) {
      ++timeouts;
      if(progress){++timeoutProgress;timeoutProgressPeerFlag+=e.peerFlag!=0;e.kind=uint32_t(WakeKind::TimeoutProgress);record(e);}
      else if(keepIdle){e.kind=uint32_t(WakeKind::TimeoutIdle);record(e);}
    } else if(notifiedWake){++woken;wokenStale+=!progress;}
    else if(otherWake)++wokenOther;
  }
  void print(Line& line,int64_t frequency) noexcept {
    const auto n=[](uint64_t v){return static_cast<unsigned long long>(v);};
    line.add(",\"wake\":{\"sleeps\":%llu,\"woken\":%llu,\"wokenOther\":%llu,\"wokenStale\":%llu,\"timeouts\":%llu,"
      "\"timeoutProgress\":%llu,\"timeoutProgressPeerFlag\":%llu,\"notifyChecks\":%llu,\"notified\":%llu,"
      "\"slowTurnarounds\":%llu,\"eventsDropped\":%llu,\"events\":[",n(sleeps),n(woken),n(wokenOther),n(wokenStale),
      n(timeouts),n(timeoutProgress),n(timeoutProgressPeerFlag),n(notifyChecks),n(notified),n(slowTurnarounds),
      n(recorded-printed>events.size()?recorded-printed-events.size():0));
    const uint64_t from=recorded-printed>events.size()?recorded-events.size():printed;
    for(uint64_t i=from;i<recorded;++i) {
      const auto& e=events[i%events.size()];
      // [kind,status,op,sequence,before,after,peerFlag,signaled,beginQpc,endQpc,us]
      line.add("%s[%u,%u,%u,%llu,%llu,%llu,%u,%u,%lld,%lld,%llu]",i==from?"":",",e.kind,e.status,e.op,n(e.sequence),
        n(e.before),n(e.after),e.peerFlag,e.signaled,static_cast<long long>(e.begin),static_cast<long long>(e.end),
        n(Micros(uint64_t(e.end-e.begin),frequency)));
    }
    printed=recorded;
    line.close("]}");
  }
};
} // namespace warvk::host::d9::profile
