#include "war3_frame_history.h"
#include "war3_frame_evidence.h"
#include "war3_frame_evidence_control.h"
#include "war3_frame_history_core.h"
#include "war3_frame_history_export_core.h"
#include "war3_frame_recorder_session.h"
#include "war3_frame_recorder_config.h"
#include "war3_frame_recorder_memory.h"
#include "war3_frame_recorder_profile.h"
#include "war3_diagnostics_hub.h"
#include <windows.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace dxvk::war3::tools {
namespace {
using json=nlohmann::json;
struct Registry {std::mutex mutex;std::shared_ptr<FrameHistory> owner;};
Registry& registry(){static auto* r=new Registry;return *r;}
struct Hud {
  std::atomic<uint32_t> state{0},bufferedMs{0},requiredMs{0},saved{0},total{0},notice{0};
  std::atomic<uint64_t> watcherTick{0};std::atomic<bool> packageReady{false};
  std::atomic<bool> selfContained{false};
  history::TriggerMailbox shortcut;
  std::atomic<const char*> error{""};
};
Hud& hud(){static auto* h=new Hud;return *h;}
bool localRecorderEnabled(){
  return evidence::LocalRecorderEnabled();
}
uint64_t qpc(){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return uint64_t(t.QuadPart);}
bool mkdir(const std::wstring& p){return CreateDirectoryW(p.c_str(),nullptr)||GetLastError()==ERROR_ALREADY_EXISTS;}
std::string utf8(const std::wstring& s){
  int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);
  std::string out(n,'\0');if(n)WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),out.data(),n,nullptr,nullptr);return out;
}
std::wstring createDirectory(uint64_t session,uint64_t nonce){
  std::wstring path=evidence::OutputDirectory();if(path.empty())return {};
  path+=L"\\history-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(nonce)+L"-"+std::to_wstring(session);
  if(!CreateDirectoryW(path.c_str(),nullptr))return {};return path;
}
bool saveNew(const std::wstring& path,const json& value){
  const auto bytes=value.dump(2);
  HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)return false;
  DWORD n=0;bool ok=WriteFile(file,bytes.data(),DWORD(bytes.size()),&n,nullptr)&&n==bytes.size();
  ok=FlushFileBuffers(file)&&ok;ok=CloseHandle(file)&&ok;return ok;
}
} // namespace

struct FrameHistory::Impl : history::CaptureLifecycle {
  struct Frame {Rc<DxvkImage> image;std::shared_ptr<HistoryReadback> request;};
  Rc<DxvkDevice> device;
  std::mutex mutex;
  std::atomic<State> state{Idle};
  std::atomic<bool> trigger{false},cancelled{false};
  // 2026-09-19 玩家实机：区分交换链重置 / 历史拷贝失败 / 源缺失，而不是笼统一句。
  const char* cancelReason=nullptr;
  std::atomic<bool> recording{false};
  bool local=false;
  evidence::RecorderControlLease controlLease;
  std::atomic<bool> stopping{false};
  std::mutex wakeMutex;
  std::condition_variable wake;
  std::thread worker;
  std::atomic<uint64_t> lockDrops{0};
  uint64_t session=0,nonce=0,serial=0,evicted=0,triggerOrdinal=0,epoch=0,deviceEpoch=0;
  uint32_t pre=64,post=4,filled=0,head=0,postWritten=0,width=0,height=0;
  uint32_t preMillis=0,retainedPre=0,trimmed=0;uint64_t frequency=0,preSpanTicks=0;
  VkFormat format=VK_FORMAT_UNDEFINED;
  std::array<Frame,history::MaxSlots> frames{};
  std::array<uint32_t,history::MaxSlots> order{};
  uint32_t orderCount=0;
  bool manifestWritten=false;
  std::wstring directory;
  const char* fault="";
  explicit Impl(const Rc<DxvkDevice>& d):device(d){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);frequency=f.QuadPart;}
  void fail(const char* reason){state=Fault;fault=reason;recording=false;hud().shortcut.disarm();hud().state=Fault;hud().error=reason;}
  void gather(){
    retainedPre=filled;preSpanTicks=0;
    if(filled){
      const auto newest=frames[(head+pre-1)%pre].request->qpc;
      if(preMillis)for(uint32_t count=1;count<=filled;++count){
        const auto oldest=frames[(head+pre-count)%pre].request->qpc;
        if(newest-oldest>=uint64_t(preMillis)*frequency/1000){retainedPre=count;break;}}
      preSpanTicks=newest-frames[(head+pre-retainedPre)%pre].request->qpc;
    }
    trimmed=filled-retainedPre;
    orderCount=history::Gather(pre,retainedPre,head,postWritten,order);hud().total=orderCount;
  }
  struct ExportSettle {
    history::ExportSettleResult shared{};
    uint32_t nextPending=history::MaxSlots; // frames[] index; MaxSlots when none
  };
  // Unique image-export settlement. Caller holds mutex. The bounded sample,
  // frozen classification, gate and the single commit step all run in the
  // shared header verbatim; this method only binds the real Impl/HUD cells and
  // maps the next un-queued job to its frame slot. Fault and Discard are never
  // revived, repeated queries stay idempotent, and invalid progress only raises
  // an explicit CPU fault: it never releases or reuses in-flight readback
  // resources, and nothing here signs packageReady.
  history::ExportCommitTargets exportCommitTargets() noexcept {
    history::ExportCommitTargets t;
    t.state=&state;t.cancelled=&cancelled;t.stopping=&stopping;
    t.fault=&fault;t.recording=&recording;
    t.hudState=&hud().state;t.hudSaved=&hud().saved;t.hudError=&hud().error;
    t.shortcutMailbox=&hud().shortcut;
    t.disarmShortcut=[](void* m)noexcept{static_cast<history::TriggerMailbox*>(m)->disarm();};
    return t;
  }
  ExportSettle settleExportProgressLocked(){
    ExportSettle out;
    out.shared=history::SettleExportProgress(orderCount,[this](uint32_t i)noexcept{
      history::ExportJobView view;
      if(i>=orderCount||order[i]>=history::MaxSlots)return view;
      const auto& request=frames[order[i]].request;
      if(!request)return view;
      view.queued=&request->queued;view.done=&request->done;view.success=&request->success;
      return view;},exportCommitTargets());
    if(out.shared.nextPending!=history::MaxSlots)out.nextPending=order[out.shared.nextPending];
    return out;
  }
  json status(){
    const auto settled=settleExportProgressLocked();
    return {{"schema",2},{"state",uint32_t(state.load())},{"session",std::to_string(session)},
      {"selfContained",local},
      {"preMilliseconds",preMillis},{"retainedPreFrames",retainedPre},{"trimmedPreFrames",trimmed},
      {"preSpanTicks",std::to_string(preSpanTicks)},{"durationSatisfied",preMillis==0||preSpanTicks>=uint64_t(preMillis)*frequency/1000},
      {"directory",utf8(directory)},{"preFrames",pre},{"postFrames",post},{"captured",std::to_string(serial)},
      {"evicted",std::to_string(evicted)},{"retained",orderCount},{"queued",settled.shared.progress.queued},{"saved",settled.shared.decision.saved},
      {"failed",settled.shared.progress.failed},{"lockDrops",std::to_string(lockDrops.load())},{"triggerOrdinal",std::to_string(triggerOrdinal)},
      {"width",width},{"height",height},{"error",fault},{"captureComplete",false},{"rootCauseReady",false}};
  }
};

FrameHistory::FrameHistory(const Rc<DxvkDevice>& d):m(new Impl(d)){}
FrameHistory::~FrameHistory(){stopRecorder();}
std::shared_ptr<FrameHistory> FrameHistory::Create(const Rc<DxvkDevice>& d){
  if(!evidence::Enabled())return {};
  auto& r=registry();std::lock_guard<std::mutex> l(r.mutex);
  if(r.owner){r.owner->cancel();return {};}
  auto owner=std::shared_ptr<FrameHistory>(new FrameHistory(d));
  owner->m->local=localRecorderEnabled();hud().selfContained=owner->m->local;
  if(owner->m->local){
    if(!evidence::ClaimLocalRecorder(owner->m->controlLease)){
      owner->m->fail("cpu-recorder-already-owned");return {};
    }
  }
  r.owner=owner;
  if(owner->m->local){
    try {owner->m->worker=std::thread([p=owner.get()]{p->runRecorder();});}
    catch(...){owner->m->fail("recorder-worker-start-failed");}
  }
  return owner;
}
void FrameHistory::Release(const std::shared_ptr<FrameHistory>& p){
  if(!p)return;
  auto& r=registry();
  {std::lock_guard<std::mutex> l(r.mutex);if(r.owner!=p)return;p->cancel();}
  // Never join under registry/owner locks: a finishing export uses those locks.
  // Keep registry ownership until joined so a new device cannot steal the session.
  p->stopRecorder();
  std::lock_guard<std::mutex> l(r.mutex);
  if(p&&r.owner==p){r.owner.reset();hud().shortcut.disarm();}
}
void FrameHistory::cancel(const char* reason) noexcept{
  // 2026-09-19 玩家实机：原来只有 reset-or-owner-change 一个笼统原因，无法区分交换链重置/拷贝失败/源缺失。
  m->cancelReason=reason;m->cancelled.store(true);m->recording=false;}
bool FrameHistory::selfContained() const noexcept{return m->local;}
void FrameHistory::stopRecorder() noexcept {
  m->stopping.store(true);m->wake.notify_all();
  if(m->worker.joinable())m->worker.join();
  evidence::ReleaseLocalRecorder(m->controlLease);
}
void FrameHistory::runRecorder() noexcept {
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
  history::RecorderSession session;
  const char* failure="local-recorder-worker-failed";
  try {
    for(;;){
      const auto action=session.next(IsInGameRenderReady(),m->state.load(),
        evidence::FrozenForExport(session.session()),
        m->stopping.load()||session.resetEndsSession(m->cancelled.load()));
      if(action==history::RecorderSession::Action::Stop)break;
      if(action==history::RecorderSession::Action::Arm){
        const auto profile=evidence::WithEnvOverrides(evidence::DefaultRecorderProfile(evidence::InternalRecorderBuild()));
        failure="insufficient-output-disk-headroom (6 GiB required)";
        const auto output=evidence::OutputDirectory();ULARGE_INTEGER available{};
        const bool diskOk=!output.empty()&&GetDiskFreeSpaceExW(output.c_str(),&available,nullptr,nullptr);
        if(!evidence::RecorderDiskBudget(diskOk,available.QuadPart))throw std::runtime_error("disk-budget");
        failure="local-recorder-cpu-arm-failed";
        const auto cpu=evidence::Control({{"action","arm"},{"capacity",profile.cpuEvents}},&m->controlLease,&m->stopping);
        if(!cpu.value("ok",false)){
          const auto rejected=cpu.value("memoryReject",0u);
          if(rejected>=1&&rejected<=4)
            {
              // 2026-09-19 玩家实机报错（address-space-headroom）：把 arm 拒绝时的实测数字带上，
              // 否则用户/我只看到一句原因，无法判断差多少。字符串存 thread_local，fail() 会拷贝。
              static thread_local std::string memoryReason;
              memoryReason=evidence::RecorderMemoryReason(evidence::RecorderMemoryReject(rejected));
              const auto num=[&cpu](const char* k){return cpu.value(k,std::string("?"));};
              memoryReason+=" (required="+num("requiredPayloadBytes")
                  +" availVirtual="+num("availableVirtual")
                  +" availCommit="+num("availableCommit")
                  +" largestFree="+num("largestFreeRegion")
                  +" totalVirtual="+num("totalVirtual")+")";
              failure=memoryReason.c_str();
            }
          throw std::runtime_error("cpu-arm-failed");
        }
        const auto token=cpu.at("session").get<std::string>();
        session.armed(std::stoull(token));
        failure="local-recorder-image-arm-failed (check output directory/free space)";
        const auto result=FrameHistoryControl({{"action","arm"},{"session",token},
          {"preFrames",profile.imagePreFrames},{"postFrames",profile.imagePostFrames},
          {"preMilliseconds",profile.preMilliseconds}},&m->controlLease);
        if(!result.value("ok",false))throw std::runtime_error("image-arm-failed");
      }else if(action==history::RecorderSession::Action::Export){
        failure="local-recorder-image-manifest-failed";
        const auto token=std::to_string(session.session());
        // Images have completed real readback fences; CPU post-window is frozen.
        // No formatting/readback/file operation is introduced into Present.
        const auto images=FrameHistoryControl({{"action","export_manifest"},{"session",token}},&m->controlLease);
        if(!images.value("ok",false))throw std::runtime_error("image-manifest-failed");
        // 2026-09-19 用户实机报错归因：原来这里只有**一个笼统标签**，真实原因（未冻结/写盘失败/输入未初始化）
        // 全被外层丢掉，用户只能看到 local-recorder-raw-export-failed。现在按 CPU 侧返回的 error 具名。
        failure="local-recorder-raw-export-failed; partial files retained";
        const auto cpu=evidence::Control({{"action","export"},{"session",token}},&m->controlLease,&m->stopping);
        const bool cpuOk=cpu.value("ok",false);
        const bool ok=cpuOk&&cpu.contains("inputs")&&cpu["inputs"].value("ok",false);
        if(!ok){
          const std::string reason=cpu.value("error",std::string());
          if(reason.find("freeze before export")!=std::string::npos)
            failure="local-recorder-raw-export-failed(freeze-before-export)";
          else if(reason.find("CreateNew")!=std::string::npos)
            failure="local-recorder-raw-export-failed(write-failed; check free space >= 6 GiB)";
          else if(cpu.contains("inputs")&&!cpu["inputs"].value("ok",false))
            failure="local-recorder-raw-export-failed(input-provider)";
          else if(!cpuOk)
            failure="local-recorder-raw-export-failed(cpu-control-error)";
        }
        json incident={{"schema",1},{"session",token},{"processId",GetCurrentProcessId()},
          {"processNonce",std::to_string(evidence::ProcessNonce())},{"selfContained",true},
          {"rawExportComplete",ok},{"evidenceValidated",false},{"rootCauseReady",false},
          {"history",images},{"cpu",cpu}};
        if(m->stopping.load()||!saveNew(m->directory+L"\\incident.json",incident)||!ok)
          throw std::runtime_error("raw-export-failed");
        hud().packageReady=true;session.finished(true);
      }
      std::unique_lock<std::mutex> sleepLock(m->wakeMutex);
      m->wake.wait_for(sleepLock,std::chrono::milliseconds(100),[&]{return m->stopping.load();});
    }
  }catch(...){std::lock_guard<std::mutex> l(m->mutex);m->fail(failure);}
  // Stop new CPU/input writes on reset/exit or arm/export failure. Never discard
  // frozen evidence implicitly, and never operate GPU refs from this worker.
  if(session.session()&&evidence::ActiveSession()==session.session()){
    try {evidence::Control({{"action","freeze"},{"session",std::to_string(session.session())}},&m->controlLease);}
    catch(...){}
  }
}

FrameHistory::Capture FrameHistory::capture(const Rc<DxvkImage>& source,evidence::Key key,uint64_t present){
  std::unique_lock<std::mutex> l(m->mutex,std::try_to_lock);
  if(!l.owns_lock()){if(m->recording.load())++m->lockDrops;return {};}
  if(m->state==Impl::Discard){for(auto& f:m->frames)f={};m->state=Impl::Idle;m->cancelled=false;hud().state=Impl::Idle;return {};}
  if(m->cancelled.load()){if(m->state!=Impl::Idle)m->fail(m->cancelReason?m->cancelReason:"reset-or-owner-change");return {};}
  if(m->state!=Impl::PendingArm&&m->state!=Impl::Armed&&m->state!=Impl::Triggered)return {};
  if((m->state==Impl::PendingArm||m->state==Impl::Armed)&&
      evidence::ActiveSession()!=m->session){m->fail("cpu-session-ended-before-trigger");return {};}
  if(!source||m->device->getDeviceStatus()!=VK_SUCCESS){m->fail("missing-source-or-device-lost");return {};}
  const auto& info=source->info();
  if(info.type!=VK_IMAGE_TYPE_2D||info.sampleCount!=VK_SAMPLE_COUNT_1_BIT||info.numLayers!=1||info.mipLevels!=1||
     !(info.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)||
     (info.format!=VK_FORMAT_B8G8R8A8_UNORM&&info.format!=VK_FORMAT_B8G8R8A8_SRGB)){
    m->fail("unsupported-source");return {};
  }
  if(m->state==Impl::PendingArm){
    uint64_t bytes=0;
    if(!history::Budget(info.extent.width,info.extent.height,m->pre,m->post,bytes)){
      m->fail("four-GiB-image-budget");return {};}
    const auto memory=m->device->adapter()->getMemoryHeapInfo();uint64_t available=0;
    for(uint32_t i=0;i<memory.heapCount;++i){const auto& h=memory.heaps[i];
      if(h.heapFlags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT){const auto budget=h.memoryBudget?h.memoryBudget:h.heapSize;
        if(budget>h.memoryAllocated)available=(std::max)(available,uint64_t(budget-h.memoryAllocated));}}
    if(available<bytes+1024ull*1024*1024){m->fail("insufficient-video-memory-headroom");return {};}
    try {
      DxvkImageCreateInfo image{};image.type=VK_IMAGE_TYPE_2D;image.format=info.format;
      image.sampleCount=VK_SAMPLE_COUNT_1_BIT;image.extent=info.extent;image.numLayers=1;image.mipLevels=1;
      image.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      image.stages=VK_PIPELINE_STAGE_TRANSFER_BIT;image.access=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
      image.layout=VK_IMAGE_LAYOUT_GENERAL;image.debugName="WarVK frame evidence history";
      for(uint32_t i=0;i<m->pre+m->post;++i){
        // Driver bookkeeping also uses host VA. Check between growth calls;
        // GPU heap budget alone cannot authorize a 32-bit process allocation.
        const auto rejected=evidence::RecorderMemoryAdmission(evidence::QueryRecorderMemory(false),0);
        if(rejected!=evidence::RecorderMemoryReject::None){
          // PendingArm images have never been submitted. Drop only this private
          // failed allocation batch, not earlier frozen evidence or GPU-in-use images.
          for(auto& frame:m->frames)frame={};
          m->fail(evidence::RecorderMemoryReason(rejected));return {};
        }
        m->frames[i].image=m->device->createImage(image,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        m->frames[i].request=std::make_shared<HistoryReadback>();
        m->frames[i].request->path=m->directory+L"\\slot-"+std::to_wstring(i)+L".tga";
      }
      m->width=info.extent.width;m->height=info.extent.height;m->format=info.format;
      m->epoch=key.mapEpoch;m->deviceEpoch=key.deviceEpoch;m->state=Impl::Armed;
      hud().shortcut.arm(m->session);
    }catch(...){for(auto& frame:m->frames)frame={};m->fail("allocation-failed");return {};}
  }
  if(m->width!=info.extent.width||m->height!=info.extent.height||m->format!=info.format||
     m->epoch!=key.mapEpoch||m->deviceEpoch!=key.deviceEpoch){m->fail("extent-or-epoch-change");return {};}
  const bool shortcut=hud().shortcut.consume(m->session);
  if((m->trigger.exchange(false)||shortcut)&&m->state==Impl::Armed){
    hud().shortcut.disarm();evidence::RequestTriggerFromGame();
    m->state=Impl::Triggered;m->triggerOrdinal=m->serial;hud().state=Impl::Triggered;}
  uint32_t index=0;
  if(m->state==Impl::Armed){index=m->head;m->head=(m->head+1)%m->pre;if(m->filled<m->pre)++m->filled;else ++m->evicted;}
  else {if(m->postWritten>=m->post){m->gather();m->state=Impl::Frozen;return {};}
    index=m->pre+m->postWritten++;}
  auto& job=*m->frames[index].request;job.key=key;job.session=m->session;
  job.present=present;job.ordinal=++m->serial;job.qpc=qpc();job.width=m->width;job.height=m->height;
  if(m->state==Impl::Armed&&m->filled){auto oldest=m->frames[(m->head+m->pre-m->filled)%m->pre].request->qpc;
    hud().bufferedMs=uint32_t((job.qpc-oldest)*1000/m->frequency);hud().state=Impl::Armed;}
  if(m->state==Impl::Triggered&&m->postWritten==m->post){m->gather();m->state=Impl::Frozen;m->recording=false;}
  return {m->frames[index].image,key,m->session,m->serial,present};
}
FrameHistory::Export FrameHistory::nextExport(){
  std::unique_lock<std::mutex> l(m->mutex,std::try_to_lock);if(!l.owns_lock())return {};
  if(m->cancelled.load())return {};
  if(m->state==Impl::Frozen){m->state=Impl::Exporting;hud().state=Impl::Exporting;}
  if(m->state!=Impl::Exporting)return {};
  // Same unique settlement entry as status(): one sampled fact set drives the
  // shared gate/commit; this function only forwards a still-unqueued job.
  const auto settled=m->settleExportProgressLocked();
  if(!history::ShouldForwardExportJob(settled.shared))return {};
  return {m->frames[settled.nextPending].image,m->frames[settled.nextPending].request};
}

bool TriggerFrameHistoryFromGame() noexcept {
  return hud().shortcut.request();
}

bool HandleFrameHistoryShortcut(uint32_t message,uint64_t key,bool ctrl,bool shift,bool repeat) noexcept {
  if(!evidence::Enabled()||!history::IsShortcut(message,key,ctrl,shift,repeat))return false;
  hud().notice=TriggerFrameHistoryFromGame()?1:2;return true;
}
FrameHistoryHud QueryFrameHistoryHud() noexcept {
  FrameHistoryHud s;s.enabled=evidence::Enabled();if(!s.enabled)return s;auto& h=hud();
  s.state=h.state.load();s.bufferedMs=h.bufferedMs.load();s.requiredMs=h.requiredMs.load();
  s.saved=h.saved.load();s.total=h.total.load();s.notice=h.notice.load();s.error=h.error.load();
  s.watcherAlive=h.watcherTick.load()&&GetTickCount64()-h.watcherTick.load()<5000;
  s.packageReady=h.packageReady.load();s.selfContained=h.selfContained.load();return s;
}

json FrameHistoryControl(const json& p,const evidence::RecorderControlLease* localLease){
  if(!evidence::Enabled()||!p.is_object()||!p.contains("action")||!p["action"].is_string())
    return {{"ok",false},{"error","history gate/action"}};
  // Validate before taking registry: Create uses registry -> controlMutex.
  // No controlMutex remains held here. The bound-owner check below also rejects
  // an old owner if a handoff occurred between these two checks.
  if(localLease&&!evidence::OwnsLocalRecorder(*localLease))
    return {{"ok",false},{"error","history control ownership mismatch"}};
  // Hold registry ownership until the control copy is destroyed. The final GPU
  // owner can consequently only be released by normal swapchain teardown.
  auto& r=registry();std::unique_lock<std::mutex> registryLock(r.mutex);auto owner=r.owner;
  if(!owner)return {{"ok",false},{"error","no unique history owner"}};
  auto& m=*owner->m;const auto action=p["action"].get<std::string>();
  if(localLease?(!m.local||localLease!=&m.controlLease):
      (m.local&&action!="status"&&action!="peek"))
    return {{"ok",false},{"error","history control ownership mismatch"}};
  if(localLease&&m.stopping.load())return {{"ok",false},{"error","history recorder stopping"}};
  if(action!="arm"&&action!="trigger"&&action!="status"&&action!="peek"&&action!="export_manifest"&&action!="discard"&&action!="acknowledge")
    return {{"ok",false},{"error","unknown history action"}};
  for(auto it=p.begin();it!=p.end();++it)if(it.key()!="action"&&it.key()!="session"&&
      !(action=="arm"&&(it.key()=="preFrames"||it.key()=="postFrames"||it.key()=="preMilliseconds")))
    return {{"ok",false},{"error","unknown history field"}};
  if(action=="peek"){hud().watcherTick=GetTickCount64();return {{"ok",true},{"state",uint32_t(m.state.load(std::memory_order_acquire))},
    {"bufferedMs",hud().bufferedMs.load()},{"session",std::to_string(m.session)},
    {"processNonce",std::to_string(evidence::ProcessNonce())}};}
  std::lock_guard<std::mutex> l(m.mutex);
  if(action!="status"){
    if(!p.contains("session")||!p["session"].is_string())return {{"ok",false},{"error","session required"}};
    const auto wanted=action=="arm"?evidence::ActiveSession():m.session;
    if(!wanted||p["session"].get<std::string>()!=std::to_string(wanted))return {{"ok",false},{"error","history session mismatch"}};
  }
  if(action=="arm"){
    if(m.state!=FrameHistory::Impl::Idle)return {{"ok",false},{"error","history must be idle"}};
    const auto pre=p.value("preFrames",json(256)),post=p.value("postFrames",json(4)),ms=p.value("preMilliseconds",json(1000));
    if(!pre.is_number_integer()||pre.is_boolean()||pre<4||pre>history::MaxPreFrames||!post.is_number_integer()||post.is_boolean()||post<1||post>16||pre.get<uint32_t>()+post.get<uint32_t>()>history::MaxSlots||
       !ms.is_number_integer()||ms.is_boolean()||ms<0||ms>2000)
      return {{"ok",false},{"error","history frame bounds"}};
    m.session=evidence::ActiveSession();m.nonce=qpc();m.directory=createDirectory(m.session,m.nonce);
    if(m.directory.empty())return {{"ok",false},{"error","CreateNew history directory failed"}};
    m.pre=pre.get<uint32_t>();m.post=post.get<uint32_t>();m.serial=m.evicted=m.triggerOrdinal=0;
    m.preMillis=ms.get<uint32_t>();m.retainedPre=m.trimmed=0;m.preSpanTicks=0;
    m.head=m.filled=m.postWritten=m.orderCount=0;m.lockDrops=0;m.manifestWritten=false;
    m.trigger=false;m.cancelled=false;m.fault="";m.state=FrameHistory::Impl::PendingArm;m.recording=true;
    hud().state=FrameHistory::Impl::PendingArm;hud().bufferedMs=0;hud().requiredMs=m.preMillis;hud().notice=0;
    hud().saved=0;hud().total=0;hud().error="";hud().packageReady=false;hud().watcherTick=GetTickCount64();
  }else if(action=="trigger"){
    if(m.state!=FrameHistory::Impl::Armed)return {{"ok",false},{"error","history not armed"}};
    m.trigger=true;
  }else if(action=="discard"){
    if(m.state!=FrameHistory::Impl::Complete&&m.state!=FrameHistory::Impl::Fault)return {{"ok",false},{"error","history not settled"}};
    m.state=FrameHistory::Impl::Discard;
  }else if(action=="acknowledge"){
    if(m.state!=FrameHistory::Impl::Complete||!m.manifestWritten)return {{"ok",false},{"error","package not exported"}};
    hud().packageReady=true;
  }
  auto out=m.status();
  if(action=="export_manifest"){
    if(m.state!=FrameHistory::Impl::Complete||m.manifestWritten)return {{"ok",false},{"error","history incomplete/already exported"}};
    out["frames"]=json::array();LARGE_INTEGER f{};QueryPerformanceFrequency(&f);out["qpcFrequency"]=std::to_string(f.QuadPart);
    for(uint32_t i=0;i<m.orderCount;++i){const auto& j=*m.frames[m.order[i]].request;
      out["frames"].push_back({{"ordinal",std::to_string(j.ordinal)},{"present",std::to_string(j.present)},
        {"owner",std::to_string(j.key.owner)},
        {"frame",std::to_string(j.key.frame)},{"mapEpoch",std::to_string(j.key.mapEpoch)},
        {"deviceEpoch",std::to_string(j.key.deviceEpoch)},{"qpc",std::to_string(j.qpc)},
        {"targetValue",std::to_string(j.targetValue)},{"observedValue",std::to_string(j.observedValue)},
        {"file",utf8(j.path)},{"saved",j.success.load()}});}
    m.manifestWritten=saveNew(m.directory+L"\\manifest.json",out);
    if(!m.manifestWritten)return {{"ok",false},{"error","CreateNew manifest failed"}};
    out.erase("frames");out["manifest"]=utf8(m.directory+L"\\manifest.json");
  }
  out["ok"]=true;return out;
}
} // namespace dxvk::war3::tools
