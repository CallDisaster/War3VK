#include "war3_frame_evidence.h"
#include "war3_frame_evidence_control.h"
#include "war3_frame_recorder_config.h"
#include "war3_frame_recorder_profile.h"
#include "war3_frame_recorder_memory.h"
#include "war3_frame_inputs_core.h"
#include "war3_palette_object_evidence_sink.h"
#include "war3_frame_recorder_build.h"
#include "../render/war3_skin_palette_selection.h"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace dxvk::war3::tools::evidence {
namespace {
using json=nlohmann::json;
struct Store {
  Ring ring;
  std::mutex mutex;
  std::atomic<uint64_t> active{0},lost{0};
  uint64_t generation=0,lossAtArm=0,nonce=0,frequency=0;
  Store() {
    LARGE_INTEGER t{},f{}; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    nonce=uint64_t(t.QuadPart); frequency=uint64_t(f.QuadPart);
  }
};
std::atomic<Store*> store{nullptr};
std::atomic<uint64_t> gameTrigger{0};
std::mutex controlMutex;
RecorderControlAuthority controlAuthority; // controlMutex; independent of ring generation
std::atomic<InputExporter> inputExporter{nullptr};

json PaletteObjectHeaderJson() {
  PaletteObjectEvidenceHeader header{};
  QueryPaletteObjectEvidenceHeader(header);
  const auto& c=header.counters;
  json counters=json::object();
  counters["emitted"]=std::to_string(c.emitted);
  counters["terminalEmitted"]=std::to_string(c.terminalEmitted);
  counters["droppedPerFrame"]=std::to_string(c.droppedPerFrame);
  counters["droppedPerSession"]=std::to_string(c.droppedPerSession);
  counters["droppedTableFull"]=std::to_string(c.droppedTableFull);
  counters["droppedProbeLimit"]=std::to_string(c.droppedProbeLimit);
  counters["droppedDuplicatePerFrame"]=std::to_string(c.droppedDuplicatePerFrame);
  counters["droppedPayloadConflict"]=std::to_string(c.droppedPayloadConflict);
  // 终态预留耗尽而未能写入的终态（工单 §2.2/§2.5.4.5：必须可见）。
  counters["droppedTerminalReserve"]=std::to_string(c.droppedTerminalReserve);
  counters["ringEvictedAfterRecord"]=std::to_string(c.ringEvictedAfterRecord);
  counters["closedRecovered"]=std::to_string(c.closedRecovered);
  counters["closedWindowExpired"]=std::to_string(c.closedWindowExpired);
  counters["closedObjectGone"]=std::to_string(c.closedObjectGone);
  counters["closedTableFull"]=std::to_string(c.closedTableFull);
  counters["closedEventLost"]=std::to_string(c.closedEventLost);
  counters["closedUnclosed"]=std::to_string(c.closedUnclosed);
  counters["closedObservationClosed"]=std::to_string(c.closedObservationClosed);
  counters["weakIdentityRecords"]=std::to_string(c.weakIdentityRecords);
  counters["epochUnknownRecords"]=std::to_string(c.epochUnknownRecords);
  // 2026-09-18 批次 3（正常观察链）：首见建条目 / 首见发射 分离计数。
  // 2026-09-18（批次 4 回归修复）—— **该规则已被阶段 C 取代，见下方 :67-70**：
  // 当时首见计数器**只在版本 3** 出现，因为版本 2 的 counters 是注册集合，
  // 追加字段会让生产解析器以 "paletteObject counter fields mismatch" 拒绝整份导出
  // （实测 A/B/C/D 四场景全败），且未使用首见链的块仍是版本 2。
  // ⇒ 现在写方**恒定 v4**：v4 的 counters **始终**含这两个键（不再有"缺席"的情形）。
  // 2026-09-18 阶段 C：**恒定 v4**。原实现按 `firstSightUsed()` 决定是否写入这两个计数器、
  // 并把块版本在 2/3 之间**动态**选择 —— 同一个二进制会产生两种块形状，读方必须同时接受，
  // 而"版本"本应是**契约**而不是**内容摘要**。裁定要求：新写方一律输出 v4。
  // v4 的 counters 集**始终**含这两个键（读方按版本取表，见 analyze_palette_object_evidence.py）。
  counters["firstSightInserted"]=std::to_string(c.firstSightInserted);
  counters["firstSightEmitted"]=std::to_string(c.firstSightEmitted);
  json result=json::object();
  // 【历史 × 3 代】下面这段（D2）→ 批次 3 → 阶段 C 是**同一处的三代叙述**，它们互相矛盾是**正常的历史**：
  //   第 1 代说"因此头块版本必须是 2"；第 2 代说"抬到 3"；第 3 代说"恒定 4"。
  //   **现行事实只有一条：本函数恒定写 `version=4`（见下方 result["version"]=4）。**
  // 2026-09-17 上级裁定 D2（⑧分段 + ⑦身份证明种类）：生产写入侧现在**确实**写分段（记录级
  // data[3]=windowSegment，Reset 开第 1 个窗口、ResetForSessionTransition 递增）与载明的身份证明
  // 种类（words32[15]=identityProofKind），因此头块版本必须是 **2**（版本 2 = 现行生产形状）。
  // 读方（analyze_frame_evidence.extension_version()）对版本 1/2 分别按旧合同 / 分段合同解析；
  // 旧产物（无 version 的冻结形状）仍按版本 1 读取，一位不放宽。见 docs/plan/2026-09-17-p0-object-evidence-window-segmentation.md §4 D2。
  // 2026-09-18 独立复审批次 3：正常观察链引入阶段 FirstSight(5)，版本 1/2 读方的阶段表
  // 不含该值（会拒绝整份导出，而不是静默忽略）。写方**只在真的发过首见事件时**把块版本
  // 抬到 3；未使用首见链的导出仍是 2，旧读方行为一位不变（版本 1 = 无 version 的冻结形状）。
  // 2026-09-18 阶段 C：**恒定 v4**（链型 + 统一终态 ObservationClosed）。
  // 不再按 `firstSightUsed()` 在 2/3 之间动态选择；v1/v2/v3 仍按各自契约被读方解析，
  // 但**新写方不再产生它们**。旧产物（无 version 的冻结形状）仍按 v1 读取，一位不放宽。
  result["version"]=4;
  // 2026-09-17 往返测试暴露的 G1：读方要求这里是 JSON **整数**（counters 仍是规范十进制字符串）。
  result["watchCount"]=header.watchCount;
  result["counters"]=counters;
  // 2026-09-18（批次 4 回归修复）：**不得**把诊断计数器写进本块。
  //
  // 实测回归：版本 2 的注册形状只允许 version / watchCount / counters。此前把
  // armed / activeSession / enqueueBlockReached / appendEntered /
  // productionInsertReached / productionNoteCalled / resetOrClearCount 写进块头，
  // 导致三处读方**全部拒绝整份导出**：
  //   the production block must be the registered version-2 shape
  //   the generic root reader must accept the real palette export
  //   the production parser must accept the real export
  //   （"paletteObject extension version 2 has unknown fields: [...]"）
  //
  // R3 的验收要求是「计数器原子化 + 位于子门判断之后 + 关闭后不再更新」，这三条
  // 由 sink 内部的 std::atomic 与调用点 gating 保证，**不依赖**把它们暴露到线上 wire。
  // 因此这里保持**同一套注册字段集合**不变（版本号现为 4；不因这些诊断量而增删字段）。
  //
  // 若将来确实需要这些诊断量：必须**抬版本**（新版本号 + 写方/读方/测试三方同步），
  // 绝不可在既有版本块里追加字段，也不可放宽读方注册表来接纳它们。
  return result;
}
json summary(const Snapshot& s,const Store& owner) {
  return {{"schema",7},{"state",uint32_t(s.state)},{"reason",uint32_t(s.reason)},
    {"session",std::to_string(s.session)},{"processId",GetCurrentProcessId()},
    {"processNonce",std::to_string(owner.nonce)},{"qpcFrequency",std::to_string(owner.frequency)},
    {"accepted",std::to_string(s.accepted)},{"evicted",std::to_string(s.evicted)},
    {"triggerSequence",std::to_string(s.triggerSequence)},
    {"producerLosses",std::to_string(owner.lost.load()-owner.lossAtArm+s.reserved-s.accepted)},
    {"reserved",std::to_string(s.reserved)},
    {"capacity",s.capacity},{"postRemaining",s.postRemaining},
    {"effectiveConfiguration",{{"frameEvidence",Enabled()},{"rawInputs",InputsEnabled()},
      {"paletteObjectEvidence",PaletteObjectEvidenceEnabled()},
      {"skinPaletteContract",war3::render::skin::ContractEnabled()},{"localRecorderOwner",controlAuthority.occupied()}}},
    // 2026-09-17 上级裁定（Step 1③）：对象级 palette 证据的会话级记账头块。
    // **始终输出**（子门关闭时为 0）：读方必须能区分"子门关闭"与"已开启但无事件"，
    // 不得把"没有事件"读成"没有错误矩阵"。计数器为规范十进制 u64 字符串。
    {"paletteObject",PaletteObjectHeaderJson()},
    {"captureComplete",false},{"rootCauseReady",false},
    {"capabilities",{{"cpuBoundaryEvents",true},{"gpuCompletion",false},
      {"pixelEvidence",false},{"videoFrameMapping",false},{"actualCasterInputs",false},
      {"exactModuleMapIdentity",false}}}};
}
json eventJson(const Event& e) {
  json data=json::array(); for(auto v:e.data) data.push_back(std::to_string(v));
  return {{"sequence",std::to_string(e.sequence)},{"session",std::to_string(e.session)},
    {"parent",std::to_string(e.parent)},{"qpc",std::to_string(e.qpc)},
    {"thread",e.thread},{"kind",uint32_t(e.kind)},
    {"owner",std::to_string(e.key.owner)},{"frame",std::to_string(e.key.frame)},
    {"mapEpoch",std::to_string(e.key.mapEpoch)},{"deviceEpoch",std::to_string(e.key.deviceEpoch)},
    {"label",e.label.data()},{"data",data},{"words32",e.bits}};
}
bool directory(const std::wstring& p) {
  return CreateDirectoryW(p.c_str(),nullptr) || GetLastError()==ERROR_ALREADY_EXISTS;
}
std::wstring outputPath(const Store& owner,uint64_t generation) {
  std::wstring path=OutputDirectory();if(path.empty())return {};
  return path+L"\\cpu-"+std::to_wstring(GetCurrentProcessId())+L"-"+
    std::to_wstring(owner.nonce)+L"-"+std::to_wstring(generation)+L".json";
}
bool createNew(const std::wstring& path,const json& header,const Ring& ring,
               const std::atomic<bool>* stopping) {
  HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,
                      FILE_ATTRIBUTE_NORMAL,nullptr);
  if(f==INVALID_HANDLE_VALUE) return false;
  bool ok=false;
  try {
    // Frozen export only. Stream records in bounded batches instead of building
    // a second nlohmann tree containing tens of millions of allocated nodes.
    std::string pending;pending.reserve(128*1024);
    const auto flush=[&](){DWORD written=0;
      if(pending.empty())return true;
      const bool result=WriteFile(f,pending.data(),DWORD(pending.size()),&written,nullptr)&&written==pending.size();
      pending.clear();return result;};
    pending=header.dump();pending.pop_back();pending+=" ,\"events\":[";
    bool first=true;
    ok=ring.visitFrozen([&](const Event& event){
      if(stopping&&stopping->load(std::memory_order_relaxed))return false;
      if(!first)pending+=',';first=false;pending+=eventJson(event).dump();
      return pending.size()<64*1024||flush();});
    if(ok){pending+="]}";ok=flush()&&FlushFileBuffers(f);}
  }catch(...){ok=false;}
  ok=CloseHandle(f) && ok;
  // A partial file is retained. Retry cannot overwrite it; export is not success.
  return ok;
}
std::string utf8(const std::wstring& s) {
  int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);
  std::string out(n,'\0');
  if(n) WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),out.data(),n,nullptr,nullptr);
  return out;
}
bool token(const json& p,uint64_t& value) {
  if(!p.contains("session") || !p["session"].is_string()) return false;
  auto s=p["session"].get<std::string>();
  if(s.empty() || s.size()>20 || s[0]=='0') return false;
  value=0;
  for(char c:s) {
    if(c<'0'||c>'9'||value>(UINT64_MAX-uint64_t(c-'0'))/10) return false;
    value=value*10+uint64_t(c-'0');
  }
  return value!=0;
}
bool integer(const json& p,const char* name,uint32_t fallback,uint32_t lo,uint32_t hi,uint32_t& out) {
  if(!p.contains(name)) {out=fallback;return true;}
  const auto& v=p[name];
  if(!v.is_number_integer() || v.is_boolean() || v<lo || v>hi) return false;
  out=v.get<uint32_t>();return true;
}
} // namespace

namespace {
const RecorderConfiguration& recorderConfiguration() noexcept {
  static const auto config=ResolveRecorderConfiguration(WARVK_INTERNAL_FRAME_RECORDER_DEFAULT!=0,
    std::getenv("DXVK_WAR3_FRAME_EVIDENCE"),std::getenv("DXVK_WAR3_FRAME_EVIDENCE_INPUTS"),
    std::getenv("DXVK_WAR3_FRAME_EVIDENCE_DRAWS"),std::getenv("DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED"),
    std::getenv("DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT"));
  return config;
}
} // namespace
bool Enabled() noexcept {return recorderConfiguration().enabled;}
bool DrawsEnabled() noexcept {return recorderConfiguration().draws;}
bool LocalRecorderEnabled() noexcept {return recorderConfiguration().local;}
bool InternalRecorderBuild() noexcept {return recorderConfiguration().internalBuild;}
void RegisterInputExporter(InputExporter exporter) noexcept {inputExporter.store(exporter,std::memory_order_release);}
bool InputsEnabled() noexcept {
  return recorderConfiguration().inputs;
}
// 2026-09-17 上级裁定（Step 1③）：对象级 palette 证据子门（默认关、受主门约束）。
bool PaletteObjectEvidenceEnabled() noexcept {
  return recorderConfiguration().paletteObject;
}
std::wstring OutputDirectory() {
  std::array<wchar_t,32768> b{};
  DWORD n=GetEnvironmentVariableW(L"DXVK_WAR3_FRAME_EVIDENCE_OUTPUT",b.data(),DWORD(b.size()));
  std::wstring p;
  if(n){
    if(n>=b.size())return {};
    p.assign(b.data(),n);
    if(p.size()<4||p[1]!=L':'||(p[2]!=L'\\'&&p[2]!=L'/')||p.find(L"..")!=p.npos)return {};
  }else{
    n=GetModuleFileNameW(nullptr,b.data(),DWORD(b.size()));if(!n||n>=b.size())return {};
    p.assign(b.data(),n);auto i=p.find_last_of(L"\\/");if(i==p.npos)return {};p.resize(i);
    for(const auto* suffix:{L"\\WarVK",L"\\Log",L"\\FrameEvidence"}){
      p+=suffix;if(!directory(p))return {};
    }
  }
  DWORD attr=GetFileAttributesW(p.c_str());
  if(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_DIRECTORY)||(attr&FILE_ATTRIBUTE_REPARSE_POINT))return {};
  return p;
}
uint64_t ActiveSession() noexcept {
  if(!Enabled()) return 0;
  auto* s=store.load(std::memory_order_acquire);
  return s?s->active.load(std::memory_order_acquire):0;
}
uint64_t ProcessNonce() noexcept {
  auto* s=store.load(std::memory_order_acquire);return s?s->nonce:0;
}
bool FrozenForExport(uint64_t session) noexcept {
  auto* s=store.load(std::memory_order_acquire);
  return session&&s&&s->ring.session()==session&&s->ring.frozenReady();
}
uint64_t Record(uint64_t session,Event event) noexcept {
  if(!session) return 0;
  auto* s=store.load(std::memory_order_acquire);
  if(!s || s->active.load(std::memory_order_acquire)!=session) return 0;
  try {
    uint64_t trigger=session;
    if(gameTrigger.compare_exchange_strong(trigger,0)) s->ring.trigger(session,16);
    LARGE_INTEGER t{}; QueryPerformanceCounter(&t);
    event.qpc=uint64_t(t.QuadPart); event.thread=GetCurrentThreadId();
    event.label.back()='\0';
    auto id=s->ring.append(session,event);
    if(s->ring.state()==State::Frozen) s->active.store(0,std::memory_order_release);
    return id;
  } catch (...) {s->lost.fetch_add(1);return 0;}
}

bool RequestTriggerFromGame() noexcept {
  auto token=ActiveSession();if(!token)return false;
  gameTrigger.store(token,std::memory_order_release);return true;
}

Scope::Scope(Kind kind,const Key& key,const char* label,uint64_t parent) noexcept {
  m_session=ActiveSession(); if(!m_session) return;
  m_kind=kind; m_key=key;
  if(label) {
    size_t i=0;
    for(;i+1<m_label.size() && label[i];++i) m_label[i]=label[i];
    m_truncated=label[i]?1:0;
  }
  Event event{}; event.kind=kind; event.key=key; event.parent=parent;
  event.label=m_label; event.data[11]=m_truncated;
  m_id=Record(m_session,event); m_exceptions=std::uncaught_exceptions();
}
Scope::~Scope() {
  if(!m_id) return;
  Event event{};
  if(m_kind==Kind::PresentBegin) event.kind=Kind::PresentEnd;
  else if(m_kind==Kind::PipelineBegin) event.kind=Kind::PipelineEnd;
  else if(m_kind==Kind::PassBegin) event.kind=Kind::PassEnd;
  else return;
  event.key=m_key; event.label=m_label; event.parent=m_id;
  for(size_t i=0;i<m_data.size();++i) event.data[i]=m_data[i];
  event.data[11]=m_truncated;
  event.data[1]=std::uncaught_exceptions()>m_exceptions?1:0;
  Record(m_session,event);
}

bool ClaimLocalRecorder(RecorderControlLease& lease) noexcept {
  std::lock_guard<std::mutex> serial(controlMutex);
  const auto* s=store.load(std::memory_order_acquire);
  return controlAuthority.claim(lease,!s||s->ring.state()==State::Idle);
}
bool ReleaseLocalRecorder(RecorderControlLease& lease) noexcept {
  std::lock_guard<std::mutex> serial(controlMutex);return controlAuthority.release(lease);
}
bool OwnsLocalRecorder(const RecorderControlLease& lease) noexcept {
  std::lock_guard<std::mutex> serial(controlMutex);return controlAuthority.owns(lease);
}
json Control(const json& payload,const RecorderControlLease* localLease,const std::atomic<bool>* stopping) {
  if(!Enabled()) return {{"ok",false},{"error","frame evidence gate disabled"}};
  if(!payload.is_object() || !payload.contains("action") || !payload["action"].is_string())
    return {{"ok",false},{"error","action required"}};
  const auto action=payload["action"].get<std::string>();
  if(action!="arm" && action!="status" && action!="trigger" && action!="freeze" && action!="export" && action!="discard")
    return {{"ok",false},{"error","unknown action"}};
  for(auto it=payload.begin();it!=payload.end();++it) {
    if(it.key()!="action" && it.key()!="session" && !(action=="arm" && it.key()=="capacity") &&
       !(action=="trigger" && it.key()=="postPresents"))
      return {{"ok",false},{"error","unknown field"}};
  }
  std::lock_guard<std::mutex> serial(controlMutex);
  if(!controlAuthority.allows(localLease,action=="status"))
    return {{"ok",false},{"error","recorder control ownership mismatch"}};
  if(stopping&&stopping->load())return {{"ok",false},{"error","recorder stopping"}};
  auto* s=store.load(std::memory_order_acquire);
  if(!s && action!="arm") return {{"ok",false},{"error","no session"}};
  if(!s) {s=new Store;store.store(s,std::memory_order_release);} // process lifetime; no GPU refs/worker
  std::unique_lock<std::mutex> lock(s->mutex);
  uint64_t generation=0;
  if(action!="arm" && action!="status" && (!token(payload,generation)||generation!=s->ring.session()))
    return {{"ok",false},{"error","session mismatch"}};
  if(action=="arm") {
    uint32_t cap=0;
    if(payload.contains("session") || !integer(payload,"capacity",8192,256,262144,cap) || s->generation==UINT64_MAX)
      return {{"ok",false},{"error","invalid capacity or exhausted generation"}};
    if(s->ring.state()!=State::Idle)
      return {{"ok",false},{"error","explicit frozen discard required before arm"}};
    const auto memory=QueryRecorderMemory(true);
    const auto bytes=Ring::storageBytes(cap);
    const auto profile=WithEnvOverrides(DefaultRecorderProfile(InternalRecorderBuild()));
    const auto inputBudget=InputsEnabled()?inputs::HostPayloadBudgetForSlots(profile.inputSlots):0;
    const auto additional=bytes+inputBudget;
    // Allow for allocator alignment/header rather than claiming a byte-exact
    // free VM region necessarily satisfies new[]. This is not a reservation.
    const auto rejected=RecorderMemoryAdmission(memory,additional,bytes+65536);
    if(rejected!=RecorderMemoryReject::None)
      return {{"ok",false},{"error",RecorderMemoryReason(rejected)},
        {"memoryReject",uint32_t(rejected)},
        {"requiredPayloadBytes",std::to_string(additional)},
        {"availableVirtual",std::to_string(memory.availableVirtual)},
        {"availableCommit",std::to_string(memory.availableCommit)},
        {"largestFreeRegion",std::to_string(memory.largestFreeRegion)},
        // 2026-09-19：totalVirtual 是 LAA 的判据（32 位进程 ~2GiB=非 LAA，~4GiB=LAA）。
        {"totalVirtual",std::to_string(memory.totalVirtual)}};
    try {
      if(!s->ring.arm(s->generation+1,cap)) return {{"ok",false},{"error","explicit frozen discard required before arm"}};
    }catch(const std::bad_alloc&){
      return {{"ok",false},{"error","recorder-cpu-ring-allocation-failed"}};
    }
    // 上级裁定 3a：证据环**自动**冻结（post-window / 容量 / 序列回绕）时也要先结算终态，
    // 因此把钩子挂到环上（手动 freeze 路径同样会先调用 ClosePaletteObjectWindow）。
    s->ring.setPreFreezeHook(&PaletteObjectPreFreezeHook);
    ++s->generation; s->lossAtArm=s->lost.load();
    // 2026-09-18 P0-1（Astra 实测修正）：**记录器初始化必须早于 active 发布**。
    // 原顺序是 hook → active.store → Arm，导致发布后、初始化前的窗口里到达的事件被
    // 未初始化的记录器接纳，随后 Reset 清掉计数且不落丢失计数 ⇒ **无声丢失**。
    ArmPaletteObjectEvidence(s->generation,0u);
    s->active.store(s->generation,std::memory_order_release);
    // 2026-09-17 上级裁定（Step 1③）：证据会话开始 ⇒ 绑定对象级 palette 记录器的发射器并清空观察表。
    // 子门关闭时该调用**不做任何事**（记录器侧零操作）。
  } else if(action=="trigger") {
    uint32_t post=0;
    if(!integer(payload,"postPresents",8,0,120,post)||!s->ring.trigger(generation,post))
      return {{"ok",false},{"error","invalid trigger state/window"}};
  } else if(action=="freeze") {
    // 2026-09-17 往返测试暴露的 G4：**必须先结算终态再冻结环** —— 终态事件同样经 Record()→Ring::append()，
    // 环一旦 Frozen，append 返回 0，导出里就永远没有终态（且当时不落任何丢失计数，非 fail-visible）。
    ClosePaletteObjectWindow();
    if(!s->ring.finish(generation)) return {{"ok",false},{"error","not recording"}};
  } else if(action=="discard") {
    if(!s->ring.discard(generation)) return {{"ok",false},{"error","only frozen evidence may be discarded"}};
    // 2026-09-17 往返测试暴露的 G2：对象级证据的会话计数必须在**导出期间仍可读**
    // （freeze 时 disarm 会让 export 的头块恒为 0），因此 disarm 推迟到证据被丢弃。
    s->ring.setPreFreezeHook(nullptr);
    DisarmPaletteObjectEvidence();
  }
  // 2026-09-17 往返测试暴露的 G2：**不得在 summary() 之前 disarm** ——
  // QueryPaletteObjectEvidenceHeader() 未 armed 时一律返回 0，会把每次真实导出的计数块清零。
  const bool leavingRecorderWindow =
      s->ring.state()!=State::Armed && s->ring.state()!=State::Triggered;
  if(action=="export") {
    // 导出前再结算一次（幂等：表已空则无事件）。
    ClosePaletteObjectWindow();
  }
  if(action=="export" && s->ring.state()!=State::Frozen)
    return {{"ok",false},{"error","freeze before export"}};
  auto snapshot=s->ring.snapshot(false);
  auto result=summary(snapshot,*s);
  if(leavingRecorderWindow) {
    s->active.store(0);
  }
  lock.unlock();
  if(action=="export") {
    auto path=outputPath(*s,generation);
    if(path.empty() || !createNew(path,result,s->ring,stopping))
      return {{"ok",false},{"error","CreateNew export failed; frozen evidence retained"}};
    result["path"]=utf8(path);
    const auto exporter=inputExporter.load(std::memory_order_acquire);
    result["inputs"]=!InputsEnabled()?json{{"ok",true},{"enabled",false}}:
        exporter?exporter(generation,path,stopping):json{{"ok",false},{"error","input provider never initialized"}};
  }
  result["ok"]=true; return result;
}
} // namespace dxvk::war3::tools::evidence
