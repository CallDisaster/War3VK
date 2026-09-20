#include "war3_frame_timeline.h"
#include "war3_frame_timeline_core.h"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

namespace dxvk::war3::timeline {
namespace {
constexpr const char* names[BucketCount] = {
  "OutsideScopes", "Present", "Hook_EngineWaitGate", "Hook_EngineSleepGate",
  "Hook_EngineSleepGateInner", "Hook_EngineSelectWorker", "Hook_EnginePrepareWait",
  "Hook_EnginePrepareDispatch", "Hook_EngineRunCallbacks", "Hook_EventMessagePump",
  "Hook_EngineFinalizeDispatch", "Hook_EngineQueueFlush", "Hook_EngineTickUpdate",
  "Hook_EngineFinalizeWorker", "Hook_EngineFinalizeTick", "Hook_EngineComputeWakeDelta",
  "Hook_EngineReschedule", "Hook_EngineTlsPump", "Hook_EventDispatch", "Hook_EventCallback",
  "Hook_WorldFramePrepare", "Hook_WorldRenderScene", "Hook_FlushAndReset",
  "DXVK_D3D9_PresentEx", "D3D9Swapchain/PresentEntry", "War3Pipeline/BeforeUi",
  "Hook_WorldPrepare", "FrameProfiler", "Hook_MainRunner", "Hook_MainRunner_Alt",
  "D3D9/LockImage", "D3D9/ResourceGpuWait", "D3D9/ResourceCsSync",
  "D3D9/LockImage/Readback", "D3D9/ResourceWaitFlush", "D3D9/WaitForResource",
  "Hook_FlushAndReset/NativeOriginal", "Hook_FlushAndReset/EndFrame",
  "Hook_FlushAndReset/ResetCaches", "FrameCapture/Poll"
};
constexpr size_t Capacity = 4096;
struct LockFrame {
  uint64_t calls=0, succeeded=0, failed=0, gameFrameCalls=0, otherCallerCalls=0;
  uint64_t flags810Calls=0, fullSurfaceCalls=0, lockableRequestedCalls=0;
  uint64_t unknownSurfaceCalls=0, resolution1440Calls=0, lockTicks=0;
};
struct NativeSyncFrame {
  uint64_t calls=0, requestOne=0, requestZero=0, requestOther=0;
  uint64_t capture=0, unreadable=0, nested=0, elided=0;
};
struct BackbufferConfig {
  uintptr_t surface=0;
  uint32_t flags=0, width=0, height=0, format=0;
};
struct LockSite { uintptr_t caller=0; uint32_t flags=0; int32_t result=0; uint64_t calls=0; };
struct Frame { Slice slice; uint64_t serial; int32_t result; uint64_t cpu100ns; LockFrame locks; NativeSyncFrame nativeSync; };
struct Store {
  std::atomic<DWORD> tid{0};
  std::atomic<uint64_t> otherPresents{0};
  std::atomic<bool> nativeSyncInstalled{false}, nativeSyncCandidate{false};
  std::mutex mutex;
  std::array<Frame,Capacity> frames{};
  size_t count = 0, next = 0;
  std::array<BackbufferConfig,32> backbuffers{};
  std::array<LockSite,32> lockSites{};
  uint64_t censusOverflow = 0;
};
Store& store() { static Store s; return s; }
struct Local {
  Ledger ledger;
  const void* swapchain = nullptr;
  uint64_t frameSerial = 0, cpu = 0;
  bool previousRecording = false;
  int32_t result = int32_t(0x80004005u);
  LockFrame locks;
  NativeSyncFrame nativeSync;
};
thread_local Local local;
uint64_t ticks() noexcept { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return uint64_t(t.QuadPart); }
uint64_t cpu() noexcept {
  FILETIME c{},e{},k{},u{};
  if (!GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u)) return 0;
  return (uint64_t(k.dwHighDateTime)<<32)+k.dwLowDateTime+(uint64_t(u.dwHighDateTime)<<32)+u.dwLowDateTime;
}
bool owner() noexcept { return Enabled() && store().tid.load(std::memory_order_relaxed)==GetCurrentThreadId(); }
}
bool Enabled() noexcept {
  static const bool value = [] { const char* e = std::getenv("DXVK_WAR3_FRAME_TIMELINE"); return e && std::strcmp(e,"1")==0; }();
  return value;
}
uint64_t Enter(const char* name) noexcept {
  if (!owner() || !name) return 0;
  for (uint32_t i=1; i<BucketCount; ++i)
    if (name[0]==names[i][0] && std::strcmp(name,names[i])==0)
      return local.ledger.enter(i,ticks());
  return 0;
}
void Leave(uint64_t token) noexcept { if (token && owner()) local.ledger.leave(token,ticks()); }
void LegacyWindow(bool inside) noexcept { if (owner()) local.ledger.legacy(inside,ticks()); }
void PresentResult(int32_t result) noexcept { if (owner()) local.result=result; }
void Present(const void* swapchain, bool recording) noexcept {
  if (!Enabled()) return;
  auto& s=store(); DWORD expected=0, tid=GetCurrentThreadId();
  s.tid.compare_exchange_strong(expected,tid,std::memory_order_relaxed);
  if (s.tid.load(std::memory_order_relaxed)!=tid || (local.swapchain && local.swapchain!=swapchain)) {
    ++s.otherPresents; return;
  }
  local.swapchain=swapchain;
  const uint64_t now=ticks(), cpuNow=cpu();
  const Slice slice=local.ledger.cut(now);
  if (local.frameSerial && recording && local.previousRecording) {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.frames[s.next]={slice,local.frameSerial,local.result,cpuNow>=local.cpu ? cpuNow-local.cpu : 0,local.locks,local.nativeSync};
    s.next=(s.next+1)%Capacity; if(s.count<Capacity) ++s.count;
  }
  ++local.frameSerial; local.cpu=cpuNow; local.previousRecording=recording;
  local.result=int32_t(0x80004005u);
  local.locks={};
  local.nativeSync={};
}
void NativeFrameSyncInstalled(bool installed, bool candidate) noexcept {
  store().nativeSyncCandidate.store(candidate);
  store().nativeSyncInstalled.store(installed);
}
bool NativeFrameSyncRecordingOwner() noexcept {
  return owner() && local.previousRecording;
}
void NativeFrameSyncRequest(int32_t request, uint32_t capture,
                           bool readable, bool nested, bool elided) noexcept {
  if (!owner()) return;
  auto& c=local.nativeSync;
  ++c.calls;
  if(request==1) ++c.requestOne; else if(request==0) ++c.requestZero; else ++c.requestOther;
  if(!readable) ++c.unreadable;
  if(readable && capture) ++c.capture;
  if(nested) ++c.nested;
  if(elided) ++c.elided;
}
void BackbufferCreated(const void* surface, uint32_t flags, uint32_t width,
                      uint32_t height, uint32_t format) noexcept {
  if (!Enabled()) return;
  auto& s=store(); std::lock_guard<std::mutex> lock(s.mutex);
  for (auto& c:s.backbuffers) if (c.surface==uintptr_t(surface) || !c.surface) {
    c={uintptr_t(surface),flags,width,height,format}; return;
  }
  ++s.censusOverflow;
}
uint64_t BackbufferLockBegin() noexcept { return owner() ? ticks() : 0; }
void BackbufferLockEnd(const void* surface, uintptr_t caller, uint32_t flags,
                       bool subrect, int32_t result, uint64_t begin) noexcept {
  if (!begin || !owner()) return;
  const uint64_t end=ticks(); auto& c=local.locks;
  ++c.calls; if(result>=0) ++c.succeeded; else ++c.failed;
  static const uintptr_t gameBase=uintptr_t(GetModuleHandleW(L"Game.dll"));
  if(gameBase && caller==gameBase+0xED0F6u) ++c.gameFrameCalls; else ++c.otherCallerCalls;
  if(flags==0x810u) ++c.flags810Calls;
  if(!subrect) ++c.fullSurfaceCalls;
  if(end>=begin) c.lockTicks+=end-begin;
  auto& s=store(); std::lock_guard<std::mutex> lock(s.mutex);
  bool found=false;
  for(const auto& b:s.backbuffers) if(b.surface==uintptr_t(surface)) {
    found=true;
    if(b.flags&1u) ++c.lockableRequestedCalls;
    if(b.width==2560 && b.height==1440) ++c.resolution1440Calls;
    break;
  }
  if(!found) ++c.unknownSurfaceCalls;
  for(auto& site:s.lockSites) if(!site.calls ||
      (site.caller==caller && site.flags==flags && site.result==result)) {
    site.caller=caller; site.flags=flags; site.result=result; ++site.calls; return;
  }
  ++s.censusOverflow;
}
std::string CaptureJson() {
  if(!Enabled()) return "{\"enabled\":false}";
  auto& s=store(); std::vector<Frame> frames;
  std::array<BackbufferConfig,32> backbuffers;
  std::array<LockSite,32> lockSites;
  uint64_t overflow;
  { std::lock_guard<std::mutex> lock(s.mutex);
    frames.reserve(s.count);
    for(size_t i=0;i<s.count;++i) frames.push_back(s.frames[(s.next+Capacity-s.count+i)%Capacity]);
    backbuffers=s.backbuffers; lockSites=s.lockSites; overflow=s.censusOverflow;
  }
  LARGE_INTEGER f{}; QueryPerformanceFrequency(&f);
  std::ostringstream out;
  out<<"{\"enabled\":true,\"schemaVersion\":1,\"clock\":\"QPC\",\"frequency\":"<<f.QuadPart
     <<",\"threadId\":"<<s.tid.load()<<",\"otherPresents\":"<<s.otherPresents.load()
     <<",\"coverageComplete\":false,\"capacity\":"<<Capacity
     <<",\"clockContract\":\"selected-swapchain-entry-to-entry; legacy-window-intersections; CPU and inclusive not additive\",\"buckets\":[";
  for(uint32_t i=0;i<BucketCount;++i) { if(i) out<<','; out<<'"'<<names[i]<<'"'; }
  out<<"],\"backbufferCensus\":{\"schemaVersion\":1,\"overflow\":"<<overflow
     <<",\"gameModuleBase\":"<<uintptr_t(GetModuleHandleW(L"Game.dll"))
     <<",\"sitesWindow\":\"process lifetime after timeline owner established\",\"configurations\":[";
  bool first=true;
  for(const auto& c:backbuffers) if(c.surface) {
    if(!first) out<<','; first=false;
    out<<"{\"surface\":"<<c.surface<<",\"flags\":"<<c.flags<<",\"width\":"<<c.width
       <<",\"height\":"<<c.height<<",\"format\":"<<c.format<<'}';
  }
  out<<"],\"sites\":["; first=true;
  for(const auto& c:lockSites) if(c.calls) {
    if(!first) out<<','; first=false;
    out<<"{\"caller\":"<<c.caller<<",\"flags\":"<<c.flags<<",\"result\":"<<c.result
       <<",\"calls\":"<<c.calls<<'}';
  }
  out<<"]},\"nativeFrameSync\":{\"installed\":"<<(s.nativeSyncInstalled.load()?"true":"false")
     <<",\"candidate\":"<<(s.nativeSyncCandidate.load()?"true":"false")
     <<",\"policy\":\"request-one-only; no-pixel-consumer; recording-owner; native-screenshot-retained\"},\"frames\":[";
  for(size_t i=0;i<frames.size();++i) {
    const auto& r=frames[i]; const auto& a=r.slice; if(i)out<<',';
    out<<"{\"serial\":"<<r.serial<<",\"start\":\""<<a.start<<"\",\"end\":\""<<a.end
       <<"\",\"legacyTicks\":"<<a.legacyTicks<<",\"result\":"<<r.result<<",\"cpu100ns\":"<<r.cpu100ns
       <<",\"events\":"<<a.events<<",\"faults\":"<<a.faults;
    auto emit=[&](const char* name,const auto& values) { out<<",\""<<name<<"\":[";
      for(size_t j=0;j<values.size();++j) { if(j)out<<','; out<<values[j]; } out<<']'; };
    emit("self",a.self); emit("inclusive",a.inclusive); emit("legacySelf",a.legacySelf); emit("calls",a.calls);
    const auto& c=r.locks;
    out<<",\"backbufferLocks\":{\"calls\":"<<c.calls<<",\"succeeded\":"<<c.succeeded
       <<",\"failed\":"<<c.failed<<",\"gameFrameCalls\":"<<c.gameFrameCalls
       <<",\"otherCallerCalls\":"<<c.otherCallerCalls<<",\"flags810Calls\":"<<c.flags810Calls
       <<",\"fullSurfaceCalls\":"<<c.fullSurfaceCalls<<",\"lockableRequestedCalls\":"<<c.lockableRequestedCalls
       <<",\"unknownSurfaceCalls\":"<<c.unknownSurfaceCalls<<",\"resolution1440Calls\":"<<c.resolution1440Calls
       <<",\"lockTicks\":"<<c.lockTicks<<'}';
    const auto& n=r.nativeSync;
    out<<",\"nativeFrameSync\":{\"calls\":"<<n.calls<<",\"requestOne\":"<<n.requestOne
       <<",\"requestZero\":"<<n.requestZero<<",\"requestOther\":"<<n.requestOther
       <<",\"capture\":"<<n.capture<<",\"unreadable\":"<<n.unreadable
       <<",\"nested\":"<<n.nested<<",\"elided\":"<<n.elided<<'}';
    out<<'}';
  }
  out<<"]}"; return out.str();
}
}
