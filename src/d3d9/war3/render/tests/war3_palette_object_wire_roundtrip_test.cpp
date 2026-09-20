// 2026-09-17 生产转换器 ↔ 解析器**往返测试**（上级 codex 01a02e0b 要求）。
//
// 真实链路（无任何复制品）：
//   生产记录器 PaletteObjectRecorder()  →  生产发射器 EmitPaletteObjectEvent()
//   →  生产转换点 EncodePaletteObjectEvent()  →  生产证据环 Record()/Ring
//   →  生产导出 Control({{"action","export"}})  →  真实导出文件
//   →  生产解析器 AutoTest/analyze_palette_object_evidence.py（由 Python 编排测试调用）。
//
// 与 AutoTest/test_frame_evidence_runtime.cpp 同一形态：**不需要 D3D 设备**，
// 只用生产 CPU 控制器 arm/freeze/export，并在 stdout 打印 EXPORT=<路径>。
// 子门（DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1）与主门（DXVK_WAR3_FRAME_EVIDENCE=1）都必须为 1，
// 否则 ArmPaletteObjectEvidence() 不做任何事（记录器零操作），本程序会立刻失败。
//
// 计数是**会话级**的：任何丢失计数非零都会让该次导出里的**所有**链判为未覆盖，
// 因此每个场景都各自 arm → 记录 → freeze → export → discard 一轮，并各自打印 EXPORT 路径。
//
// 本文件**不修改**记录器 / 发射器 / 转换器 / wire 布局：它只驱动生产入口并打印原始判定材料。
// 任何与上级期望不符之处只在 stdout 的 HEADER_*/SPEC 行里如实打印，由 Python 编排测试记录。

#include "../../tools/war3_frame_evidence.h"
#include "../../tools/war3_frame_evidence_control.h"
#include "../../tools/war3_palette_object_capture.h"
#include "../../tools/war3_palette_object_evidence.h"
#include "../../tools/war3_palette_object_evidence_sink.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace dxvk::war3::tools::evidence;

namespace {

unsigned g_checks = 0;
unsigned g_failures = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (ok) return;
  ++g_failures;
  std::cout << "FAIL: " << what << "\n";
}

// 固定场景参数（均为**显式**值：未知域一律 "0 + unknown 标记"，绝不用 0 冒充已知）。
constexpr uint64_t kRenderablePart = 0x0000000000001000ull;
constexpr uint64_t kRuntimeModelPtr = 0x0000000000002000ull;
constexpr uint64_t kJHandle = 0x10ull;
constexpr uint64_t kRawcode = 0x6831ull;      // 'h1' 形状的 rawcode 占位（任意固定值）
constexpr uint64_t kMapEpoch = 0x0Bull;       // 生产侧来自 draw.mapEpoch / resource.mapEpoch
constexpr uint64_t kDeviceEpoch = 0x5A5Aull;  // 仅 A/B/C 的强身份场景显式给出（epochUnknown=false）
constexpr uint64_t kRecordFrameSerialBase = 0x100000000ull;  // 刻意跨 32 位，检验 lo/hi 拆分
// 2026-09-17 上级裁定 ⑦：**已认证**路径必须保持可执行。三个生产采集点一律
// lifecycleIdentity=0 + identityWeak=true + epochUnknown=true（生产**不可达**认证），
// 因此认证只能在**合成键**上覆盖：A/F 场景显式给出非零实例生命周期身份 + 两个强标记为假。
// 这不是"生产也能认证"，也不是写入后门 —— 记录器仍按同一推导规则算出 identityProofKind。
constexpr uint64_t kCertifiedLifecycleIdentity = 0xA5C0F00D00000001ull;

// 四个阶段各自独立的渲染帧（上级要求"四步在不同帧"；帧号按生产时序递增）。
constexpr uint64_t kRejectFrame = 500ull;
constexpr uint64_t kServedFrame = 503ull;
constexpr uint64_t kEnqueuedFrame = 504ull;
constexpr uint64_t kDrawnFrame = 505ull;

PaletteObjectFrames FramesAt(uint64_t renderFrame) {
  // 生产真实口径：manifest 帧号 / 发布 revision 当场不可得 ⇒ 0 + manifestUnknown=true；
  // native 帧号当场不可得 ⇒ 0 + nativeUnknown=true；record 帧号是当场已读到的局部值。
  return MakePaletteObjectFrames(renderFrame, kRecordFrameSerialBase + renderFrame, 0u, false);
}

PaletteObjectKey MakeKey(uint64_t session, bool identityWeak, uint64_t lifecycleIdentity) {
  PaletteObjectKey key = MakePaletteObjectKey(
      reinterpret_cast<void*>(uintptr_t(kRenderablePart)),
      reinterpret_cast<void*>(uintptr_t(kRuntimeModelPtr)),
      uint32_t(kJHandle), uint32_t(kRawcode), session, kMapEpoch);
  // MakePaletteObjectKey 是生产口径（一律 identityWeak=true / epochUnknown=true /
  // lifecycleIdentity=0）。各场景按上级规格显式改写这三个身份字段（其余仍为生产口径）：
  key.identityWeak = identityWeak;
  key.epochUnknown = false;
  // epochUnknown=false 时必须给出**真实**设备代际，不得用 0 冒充"已知且为零"。
  key.deviceEpoch = kDeviceEpoch;
  // 2026-09-17 D2：A/F 用 kCertifiedLifecycleIdentity（合成键，生产不可达）覆盖**已认证**路径；
  // B/C 保持 0 ⇒ 记录器推导出的证明种类恒为 NoIdentityProof(0) ⇒ 版本 2 下不得认证同对象恢复。
  key.lifecycleIdentity = lifecycleIdentity;
  return key;
}

// 直接读生产记录器本体（不经导出头块）：用于把"记录器已结算"与"证据环是否收下该事件"分开。
void PrintRecorderCounters(const char* tag) {
  const auto& c = PaletteObjectRecorder().counters();
  std::cout << "RECORDER_" << tag << " watchCount=" << PaletteObjectRecorder().watchCount()
            << " emitted=" << c.emitted
            << " terminalEmitted=" << c.terminalEmitted
            << " closedRecovered=" << c.closedRecovered
            << " closedWindowExpired=" << c.closedWindowExpired
            << " closedUnclosed=" << c.closedUnclosed
            << " droppedPerFrame=" << c.droppedPerFrame
            << " droppedPerSession=" << c.droppedPerSession
            << " droppedTerminalReserve=" << c.droppedTerminalReserve << "\n";
}

void PrintHeader(const char* tag) {
  PaletteObjectEvidenceHeader header{};
  QueryPaletteObjectEvidenceHeader(header);
  const auto& c = header.counters;
  std::cout << "HEADER_" << tag << " watchCount=" << header.watchCount
            << " emitted=" << c.emitted
            << " terminalEmitted=" << c.terminalEmitted
            << " droppedPerFrame=" << c.droppedPerFrame
            << " droppedPerSession=" << c.droppedPerSession
            << " droppedTableFull=" << c.droppedTableFull
            << " droppedProbeLimit=" << c.droppedProbeLimit
            << " droppedTerminalReserve=" << c.droppedTerminalReserve
            << " ringEvictedAfterRecord=" << c.ringEvictedAfterRecord
            << " closedRecovered=" << c.closedRecovered
            << " weakIdentityRecords=" << c.weakIdentityRecords
            << " epochUnknownRecords=" << c.epochUnknownRecords << "\n";
}

// 场景上下文：session 是记录器/环的会话号，token 是 Control() 要求的规范十进制串。
struct ScenarioIO {
  uint64_t session = 0u;
  std::string token;
  uint32_t capacity = 256u;
};

// 一次完整的生产往返：arm → (调用方记录) → freeze → export → 打印路径 → discard。
// （场景 E 的环由场景自己的控制线程冻结，运行器只核验状态，见 frozenByScenario。）
// fn 在 arm **之后**被调用（arm 会 ArmPaletteObjectEvidence 并清表）。
void RunScenario(const char* name, void (*fn)(const ScenarioIO&), uint32_t capacity,
                 bool frozenByScenario = false) {
  std::cout << "SCENARIO_BEGIN " << name << "\n";
  auto armed = Control({{"action", "arm"}, {"capacity", capacity}});
  Check(armed["ok"].get<bool>(), "arm must succeed");
  if (!armed["ok"].get<bool>()) return;
  const std::string token = armed["session"].get<std::string>();
  const uint64_t session = ActiveSession();
  Check(session != 0u, "arm must publish an active session");
  Check(PaletteObjectEvidenceEnabled(), "palette-object sub-gate must be enabled by env");

  // 生产会话里同样存在的普通帧域事件（Present 成对跨度）：解析器会校验，但不算对象证据。
  {
    const Key presentKey{101ull, 17ull, kMapEpoch, 0ull};
    Scope present(Kind::PresentBegin, presentKey, "PaletteRoundtripPresent");
    present.value(2, 1);
    present.outcome(1);
  }
  ScenarioIO io;
  io.session = session;
  io.token = token;
  io.capacity = capacity;
  if (fn != nullptr) fn(io);
  PrintHeader("ARMED");

  if (frozenByScenario) {
    // 场景 E：环由场景自己的控制线程用生产 Control({{"action","freeze"}}) 冻结；
    // 这里只**核验**它确实处于 Frozen（不再二次 finish，避免把"已冻结"当成失败）。
    auto status = Control({{"action", "status"}});
    Check(status["state"].get<uint32_t>() == 3u,
          "E: the scenario's own control thread must have frozen the ring");
  } else {
    auto frozen = Control({{"action", "freeze"}, {"session", token}});
    Check(frozen["ok"].get<bool>(), "freeze must succeed");
  }
  // freeze 内部已调用生产 ClosePaletteObjectWindow()（结算终态）。此处直接读记录器本体，
  // 以分辨"记录器已判 Recovered"与"该终态事件是否真的进了证据环/导出"。
  PrintRecorderCounters("AFTER_FREEZE");
  auto exported = Control({{"action", "export"}, {"session", token}});
  Check(exported["ok"].get<bool>(), "export must succeed");
  if (exported["ok"].get<bool>())
    std::cout << "SCENARIO=" << name << " EXPORT=" << exported["path"].get<std::string>() << "\n";
  PrintHeader("AFTER_EXPORT");
  auto discarded = Control({{"action", "discard"}, {"session", token}});
  Check(discarded["ok"].get<bool>(), "discard must succeed");
}

void RecordCertifiedShape(uint64_t session, bool identityWeak, uint64_t lifecycleIdentity,
                          bool ringEviction) {
  const PaletteObjectKey key = MakeKey(session, identityWeak, lifecycleIdentity);
  PaletteObjectEvidence& recorder = PaletteObjectRecorder();
  recorder.NoteReject(key, PaletteObjectRejectReason::R1, FramesAt(kRejectFrame));
  // 来源必须是**生产可达**值（审计卫生点）：S 点 switch 只可能给出
  // DrawTimeCaptured / ArenaSlot / OwnedPartSnapshot / PoseKernel / Unknown（override 时 None）。
  recorder.NoteServed(key, PaletteObjectSource::ArenaSlot, 0xA1ull,
                      FramesAt(kServedFrame));
  recorder.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 0xB2ull,
                        FramesAt(kEnqueuedFrame), false);
  recorder.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0xC3ull,
                     FramesAt(kDrawnFrame), false);
  if (ringEviction) recorder.NoteRingEviction(1u);
}

// A：已认证合成键（非零实例生命周期身份 + identityWeak=false + epochUnknown=false）。
void ScenarioA(const ScenarioIO& io) {
  RecordCertifiedShape(io.session, false, kCertifiedLifecycleIdentity, false);
}
// B：同一形状 + identityWeak=true（弱身份 ⇒ 不得认证）。
void ScenarioB(const ScenarioIO& io) { RecordCertifiedShape(io.session, true, 0u, false); }
// C：同一形状 + 身份值为 0（版本 2 下具名拒绝 identityNotProven）+ 证据环淘汰 1 条。
void ScenarioC(const ScenarioIO& io) { RecordCertifiedShape(io.session, false, 0u, true); }
void ScenarioD(const ScenarioIO&) {}  // 空样本：arm 后不记录任何对象证据，直接 freeze/export

// ---------------------------------------------------------------------------
// 场景 G（2026-09-18 批次 3 更正后的**端到端**见证）：**正常观察链的生产入口**。
//
// 与 A/B/C 的关键区别：本场景**没有任何 NoteReject** —— 链路是
//   NoteFirstSight（首次观察即建条目） -> NoteEnqueued
// 因此写方必须把块版本抬到 **3**，而两个读方（通用根读取器 + palette 解析器）
// 必须**接受**整份导出。
//
// 背景：v3 曾经只做了写方（读方只注册 {1,2}），实机一旦走首见链就会整份被拒；
// 这条用例就是为了让那种不一致**无法再悄悄通过** —— 它走的是真实写方路径，
// 不是手工构造的 fixture。
//
// 键的取值（2026-09-18 更正 —— 原文写"与生产采集点的真实取值一致"，**那句话是假的**）：
//
//   字段              G 夹具               生产采集点（war3_palette_object_capture.h:151-166）  一致?
//   identityWeak      true                 true                                            ✅
//   lifecycleIdentity 0                    0                                               ✅
//   epochUnknown      false                **true**                                        ❌
//   deviceEpoch       kDeviceEpoch(非零)    **0**                                           ❌
//
// 即：**G 与生产只在两个字段上一致**。夹具刻意走"设备代际已知且非零"的形状（见 MakeKey:86
// 的说明：epochUnknown=false 时必须给真实代际，不得用 0 冒充"已知且为零"），那是版本 2 合同
// 下的形状；而生产恒为 epochUnknown=true + deviceEpoch=0。
//
// ⇒ **生产身份形状在本驱动中没有任何场景覆盖**。这是一个**已知的覆盖缺口**，不是"已覆盖"。
//   实机导出已自证差异：epochUnknownRecords=0 而 weakIdentityRecords=1（生产必然是两者同时非零）。
void ScenarioG(const ScenarioIO& io) {
  const PaletteObjectKey key = MakeKey(io.session, true, 0u);
  PaletteObjectEvidence& recorder = PaletteObjectRecorder();
  recorder.NoteFirstSight(key, FramesAt(kRejectFrame));
  recorder.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 0xB2ull,
                        FramesAt(kEnqueuedFrame), false);
}

// ---------------------------------------------------------------------------
// 场景 E：**并发所有者协议**（2026-09-17 上级裁定第 1 项）。
//   4 个写者线程各自用自己的对象键并发调用生产 NoteReject/NoteServed/NoteEnqueued/
//   NoteDrawn（第一阶段每线程 100 键 × 4 阶段）；四个写者都到达屏障后，控制线程先放行写者
//   再调用生产 Control({{"action","freeze"}})，因此第二批 Note*（4 × 25 键 × 4 阶段）与冻结
//   **真正同时**竞争记录器状态访问（由记录器自己的锁串行化）。第二批是否被接受是时序相关的，
//   所以判定只钉恒等式（emitted == 导出 + 环损失、零丢失、关闭后计数不变），不钉固定条数。
//   判定：(a) emitted 恰等于导出事件条数；(b) 计数恒等式（含 ringEvictedAfterRecord）；
//   (c) 关闭后到达的观测不改变任何计数；(d) 无崩溃、无断言失败。
//   参数（线程数 / 每线程键数 / 每键阶段数 / 计划事件数）全部打印，由编排测试核对。
// ---------------------------------------------------------------------------
constexpr unsigned kConcurrencyThreads = 4u;
constexpr unsigned kConcurrencyKeysPerThread = 100u;
constexpr unsigned kConcurrencyPostCloseKeysPerThread = 25u;
constexpr unsigned kConcurrencyPostCloseExtraPerStage = 8u;
constexpr uint32_t kConcurrencyCapacity = 8192u;
constexpr uint64_t kConcurrencyFrameBase = 1000000ull;
constexpr uint64_t kConcurrencyPlannedNormal =
    uint64_t(kConcurrencyThreads) * uint64_t(kConcurrencyKeysPerThread) * 4ull;
constexpr uint64_t kConcurrencyPlannedKeys =
    uint64_t(kConcurrencyThreads) * uint64_t(kConcurrencyKeysPerThread);
constexpr uint64_t kConcurrencyPlannedEmitted = kConcurrencyPlannedNormal + kConcurrencyPlannedKeys;
constexpr uint64_t kConcurrencyPostCloseCalls =
    uint64_t(kConcurrencyThreads) * uint64_t(kConcurrencyPostCloseKeysPerThread) * 4ull;

// 全字段头块打印（E/F 专用）：丢失计数必须逐项可见，不得只打印子集。
void PrintCountersFull(const char* tag) {
  PaletteObjectEvidenceHeader header{};
  QueryPaletteObjectEvidenceHeader(header);
  const auto& c = header.counters;
  std::cout << "HEADER_" << tag << " watchCount=" << header.watchCount
            << " emitted=" << c.emitted
            << " terminalEmitted=" << c.terminalEmitted
            << " droppedPerFrame=" << c.droppedPerFrame
            << " droppedPerSession=" << c.droppedPerSession
            << " droppedTableFull=" << c.droppedTableFull
            << " droppedProbeLimit=" << c.droppedProbeLimit
            << " droppedDuplicatePerFrame=" << c.droppedDuplicatePerFrame
            // D6（2026-09-17 上级裁定）：同键同帧同阶段但**载荷冲突**的可见计数。它属于
            // 丢失计数（"全字段头块打印：丢失计数必须逐项可见，不得只打印子集"）。
            << " droppedPayloadConflict=" << c.droppedPayloadConflict
            << " droppedTerminalReserve=" << c.droppedTerminalReserve
            << " ringEvictedAfterRecord=" << c.ringEvictedAfterRecord
            << " closedRecovered=" << c.closedRecovered
            << " closedWindowExpired=" << c.closedWindowExpired
            << " closedObjectGone=" << c.closedObjectGone
            << " closedTableFull=" << c.closedTableFull
            << " closedEventLost=" << c.closedEventLost
            << " closedUnclosed=" << c.closedUnclosed
            << " weakIdentityRecords=" << c.weakIdentityRecords
            << " epochUnknownRecords=" << c.epochUnknownRecords << "\n";
}

// 并发场景的键：线程/键索引完全解耦（不同写者绝不共享同一个键 ⇒ 不存在跨线程链合并）。
PaletteObjectKey MakeConcurrencyKey(uint64_t session, unsigned threadIndex, unsigned keyIndex) {
  PaletteObjectKey key = MakePaletteObjectKey(
      reinterpret_cast<void*>(uintptr_t(0x4000u + uint64_t(threadIndex) * 0x1000u + keyIndex)),
      reinterpret_cast<void*>(uintptr_t(kRuntimeModelPtr)),
      uint32_t(0x100u + threadIndex * 0x100u + keyIndex),
      uint32_t(0x6000u + keyIndex), session, kMapEpoch);
  key.identityWeak = false;
  key.epochUnknown = false;
  key.deviceEpoch = kDeviceEpoch;
  key.lifecycleIdentity = 0u;
  return key;
}

struct ConcurrencyPlan {
  std::atomic<unsigned> phase1Done{0u};
  std::atomic<bool> release{false};
  std::atomic<unsigned> phase1NoteCalls{0u};
  std::atomic<unsigned> phase2NoteCalls{0u};
  std::atomic<unsigned> controlFailures{0u};
};

void ConcurrencyWriter(const ScenarioIO* io, ConcurrencyPlan* plan, unsigned threadIndex) {
  PaletteObjectEvidence& recorder = PaletteObjectRecorder();
  for (unsigned k = 0u; k < kConcurrencyKeysPerThread; ++k) {
    const PaletteObjectKey key = MakeConcurrencyKey(io->session, threadIndex, k);
    const uint64_t base = kConcurrencyFrameBase + uint64_t(threadIndex) * 100000ull +
                          uint64_t(k) * 4ull;
    recorder.NoteReject(key, PaletteObjectRejectReason::R1, FramesAt(base));
    recorder.NoteServed(key, PaletteObjectSource::ArenaSlot, 0xA1ull + k, FramesAt(base + 1ull));
    recorder.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 0xB2ull + k,
                          FramesAt(base + 2ull), false);
    recorder.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0xC3ull + k,
                       FramesAt(base + 3ull), false);
    plan->phase1NoteCalls.fetch_add(4u, std::memory_order_relaxed);
  }
  // 屏障：四个写者都到达后，控制线程才调用 Control(freeze)。
  plan->phase1Done.fetch_add(1u, std::memory_order_release);
  while (!plan->release.load(std::memory_order_acquire)) std::this_thread::yield();
  // freeze 之后继续并发调用 Note*：窗口已关闭 ⇒ 全部必须早退、不得改变任何计数。
  for (unsigned k = 0u; k < kConcurrencyPostCloseKeysPerThread; ++k) {
    const PaletteObjectKey key = MakeConcurrencyKey(
        io->session, threadIndex, kConcurrencyKeysPerThread + k);
    const uint64_t base = kConcurrencyFrameBase + 5000000ull +
                          uint64_t(threadIndex) * 100000ull + uint64_t(k) * 4ull;
    recorder.NoteReject(key, PaletteObjectRejectReason::R1, FramesAt(base));
    recorder.NoteServed(key, PaletteObjectSource::PoseKernel, 0xD4ull + k, FramesAt(base + 1ull));
    recorder.NoteEnqueued(key, PaletteObjectSource::OwnedPartSnapshot, 0xE5ull + k,
                          FramesAt(base + 2ull), false);
    recorder.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0xF6ull + k,
                       FramesAt(base + 3ull), false);
    plan->phase2NoteCalls.fetch_add(4u, std::memory_order_relaxed);
  }
}

void ConcurrencyControl(const ScenarioIO* io, ConcurrencyPlan* plan) {
  while (plan->phase1Done.load(std::memory_order_acquire) < kConcurrencyThreads)
    std::this_thread::yield();
  // 先放行写者，再调用 Control(freeze)：写者的第二批 Note* 与控制线程的冻结**真正同时**
  // 竞争记录器状态（被锁串行化）。因此第二批事件是否被接受是时序相关的，判定只钉
  // 恒等式（emitted == 导出 + 环损失、零丢失、关闭后不再变化），不钉固定条数。
  plan->release.store(true, std::memory_order_release);
  auto frozen = Control({{"action", "freeze"}, {"session", io->token}});
  if (!frozen["ok"].get<bool>()) plan->controlFailures.fetch_add(1u, std::memory_order_relaxed);
}

void ScenarioE(const ScenarioIO& io) {
  ConcurrencyPlan plan;
  std::vector<std::thread> writers;
  writers.reserve(kConcurrencyThreads);
  for (unsigned t = 0u; t < kConcurrencyThreads; ++t)
    writers.emplace_back(ConcurrencyWriter, &io, &plan, t);
  std::thread control(ConcurrencyControl, &io, &plan);
  for (auto& writer : writers) writer.join();
  control.join();

  Check(plan.controlFailures.load() == 0u, "E: the control thread must freeze successfully");
  Check(plan.phase1NoteCalls.load() == kConcurrencyPlannedNormal,
        "E: every phase-1 Note* call must have run");
  Check(plan.phase2NoteCalls.load() == kConcurrencyPostCloseCalls,
        "E: every post-close Note* call must have run (they must all be refused)");
  PrintCountersFull("E_AFTER_FREEZE");

  // (c) 关闭后**再**补发一批观测（主线程串行）：窗口已关闭 ⇒ 计数一个也不得变。
  PaletteObjectEvidence& recorder = PaletteObjectRecorder();
  for (unsigned s = 0u; s < kConcurrencyPostCloseExtraPerStage; ++s) {
    const PaletteObjectKey key = MakeConcurrencyKey(io.session, kConcurrencyThreads, s);
    const uint64_t base = kConcurrencyFrameBase + 8000000ull + uint64_t(s) * 4ull;
    recorder.NoteReject(key, PaletteObjectRejectReason::R2, FramesAt(base));
    recorder.NoteServed(key, PaletteObjectSource::ArenaSlot, 0x11ull + s, FramesAt(base + 1ull));
    recorder.NoteEnqueued(key, PaletteObjectSource::OwnedPartSnapshot, 0x22ull + s,
                          FramesAt(base + 2ull), false);
    recorder.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x33ull + s,
                       FramesAt(base + 3ull), false);
  }
  PrintCountersFull("E_AFTER_POST_CLOSE");
  std::cout << "CONCURRENCY threads=" << kConcurrencyThreads
            << " keysPerThread=" << kConcurrencyKeysPerThread
            << " phase1NoteCalls=" << plan.phase1NoteCalls.load()
            << " phase1Keys=" << kConcurrencyPlannedKeys
            << " phase2NoteCalls=" << plan.phase2NoteCalls.load()
            << " postCloseExtraPerStage=" << kConcurrencyPostCloseExtraPerStage
            << " plannedEmitted=" << kConcurrencyPlannedEmitted
            << " observedEmitted=" << recorder.counters().emitted
            << " observedTerminals=" << recorder.counters().terminalEmitted
            << " watchCountAfterClose=" << recorder.watchCount() << "\n";
}

// ---------------------------------------------------------------------------
// 场景 F：**自动冻结**路径（2026-09-17 上级裁定第 2 项）。
//   arm → 记录 A 形状的强身份四阶段链 → 真实控制路径 Control(trigger, postPresents=N)
//   → 喂入 N 组生产 PresentBegin/PresentEnd 跨度 ⇒ Ring::append 在 post-window 结束时
//   **自动**冻结（不调用 Control(freeze)）。终态只能由预冻结钩子在状态仍为 Triggered 时
//   结算并写入环，因此导出必须含 Recovered 终态。
// ---------------------------------------------------------------------------
constexpr uint32_t kAutoFreezePostPresents = 8u;

void ScenarioF(const ScenarioIO& io) {
  RecordCertifiedShape(io.session, false, kCertifiedLifecycleIdentity, false);
  auto triggered = Control({{"action", "trigger"}, {"session", io.token},
                            {"postPresents", kAutoFreezePostPresents}});
  Check(triggered["ok"].get<bool>(), "F: the production trigger must be accepted");
  for (uint32_t i = 0u; i < kAutoFreezePostPresents; ++i) {
    const Key presentKey{202ull, 30ull + uint64_t(i), kMapEpoch, 0ull};
    Scope present(Kind::PresentBegin, presentKey, "PaletteAutoFreezePresent");
    present.value(2, 1);
    present.outcome(1);
  }
  // 这里**没有** Control(freeze)：环必须已经在 post-window 结束时自动冻结。
  auto status = Control({{"action", "status"}});
  Check(status["state"].get<uint32_t>() == 3u,
        "F: the ring must auto-freeze at the end of the post window (state == Frozen)");
  Check(status["reason"].get<uint32_t>() == 2u,
        "F: the automatic freeze reason must be PostWindow (2), not Requested (1)");
  Check(status["postRemaining"].get<uint32_t>() == 0u,
        "F: the post window must be exhausted");
  std::cout << "AUTOFREEZE postPresents=" << kAutoFreezePostPresents
            << " state=" << status["state"].get<uint32_t>()
            << " reason=" << status["reason"].get<uint32_t>()
            << " triggerSequence=" << status["triggerSequence"].get<std::string>()
            << " reserved=" << status["reserved"].get<std::string>()
            << " accepted=" << status["accepted"].get<std::string>()
            << " postRemaining=" << status["postRemaining"].get<uint32_t>() << "\n";
  PrintCountersFull("F_AFTER_AUTOFREEZE");
}

// 自动冻结场景的运行器：**没有** Control(freeze)（freeze 必须由环自己在 post-window 结束时发生）。
void RunAutoFreezeScenario(const char* name, void (*fn)(const ScenarioIO&), uint32_t capacity) {
  std::cout << "SCENARIO_BEGIN " << name << "\n";
  auto armed = Control({{"action", "arm"}, {"capacity", capacity}});
  Check(armed["ok"].get<bool>(), "arm must succeed");
  if (!armed["ok"].get<bool>()) return;
  const std::string token = armed["session"].get<std::string>();
  const uint64_t session = ActiveSession();
  Check(session != 0u, "arm must publish an active session");
  Check(PaletteObjectEvidenceEnabled(), "palette-object sub-gate must be enabled by env");
  {
    const Key presentKey{303ull, 41ull, kMapEpoch, 0ull};
    Scope present(Kind::PresentBegin, presentKey, "PaletteAutoFreezeWarmup");
    present.value(2, 1);
    present.outcome(1);
  }
  ScenarioIO io;
  io.session = session;
  io.token = token;
  io.capacity = capacity;
  PrintHeader("ARMED");
  if (fn != nullptr) fn(io);
  // 不调用 Control(freeze)：直接导出（导出前还会幂等调用一次 ClosePaletteObjectWindow）。
  auto exported = Control({{"action", "export"}, {"session", token}});
  Check(exported["ok"].get<bool>(), "export must succeed after the automatic freeze");
  if (exported["ok"].get<bool>())
    std::cout << "SCENARIO=" << name << " EXPORT=" << exported["path"].get<std::string>() << "\n";
  PrintHeader("AFTER_EXPORT");
  PrintCountersFull("F_AFTER_EXPORT");
  auto discarded = Control({{"action", "discard"}, {"session", token}});
  Check(discarded["ok"].get<bool>(), "discard must succeed");
}

// ---------------------------------------------------------------------------
// 纯函数单测（上级第 4 项，可选）：EncodePaletteObjectEvent 的 wire 逐位映射。
// 与采集点/环/导出无关，不改变任何生产语义；它证明"生产发射器与解析器用的是同一份映射"。
// ---------------------------------------------------------------------------
void RunEncoderUnitChecks() {
  unsigned checks = 0;
  unsigned failures = 0;
  const auto expect = [&](bool ok, const char* what) {
    ++checks;
    if (!ok) {
      ++failures;
      std::cout << "ENCODER_FAIL: " << what << "\n";
    }
  };

  PaletteObjectEventRecord record{};
  record.stage = PaletteObjectStage::ServedCandidate;
  record.terminal = PaletteObjectTerminal::Recovered;
  record.rejectReason = PaletteObjectRejectReason::R2;
  // 纯编码测试（只验证 wire 能写这个值，不代表生产会给出它）。
  record.source = PaletteObjectSource::ProducerSnapshot;
  record.key.renderablePart = 0x1111ull;
  record.key.runtimeModelPtr = 0x2222ull;
  record.key.jHandle = 0x3333ull;
  record.key.rawcode = 0x4444ull;
  record.key.sessionGeneration = 0x600000011ull;  // 跨 32 位：检验 bits[10]/[29] 拆分
  record.key.mapEpoch = 0x5555ull;
  record.key.deviceEpoch = 0x6666ull;
  record.key.lifecycleIdentity = 0x7777ull;       // data[2] 必须逐位承载
  record.key.identityWeak = true;
  record.key.epochUnknown = true;
  record.frames.renderFrame = 0x8888ull;
  record.frames.manifestFrameSerial = 0x9999ull;
  record.frames.manifestPublishRevision = 0x40000000Bull;  // 跨 32 位：bits[26]/[27]
  record.frames.recordFrameSerial = 0x300000009ull;        // 跨 32 位：bits[24]/[25]
  record.frames.nativeFrameTag = 0x200000007ull;           // 跨 32 位：bits[21]/[32]
  record.frames.manifestUnknown = true;
  record.frames.nativeUnknown = true;
  record.hitKey = 0xAAAAull;
  // 2026-09-17 D2：版本 2 声明的两个载体。记录级分段量取一个**跨 32 位**的值，证明 data[3] 是
  // 完整的 u64 槽（读方按 u64 逐位解码）。identityProofKind 保持记录默认值 0：本纯函数测试
  // **不**从外部写入证明种类 —— 生产值只由记录器 DeriveIdentityProofKind() 推导（无后门）。
  record.windowSegment = 0x123456789ABCDEFull;
  record.firstRejectFrame = 0x50000000Dull;       // 跨 32 位：bits[30]/[31]
  record.deltaFrames = 0x100000005ull;            // 跨 32 位：bits[20]/[33]
  record.hitCount = 7u;
  record.chainSequence = 9u;
  record.sawSubmit = true;
  record.sawDraw = true;
  record.selectionClearedByNativeOverride = true;

  Event out{};
  out.session = 0xEEEEEEEEEEEEEEEEull;  // 哨兵：转换失败时不得留下"看似成功"的半成品
  out.bits[12] = 0xEEEEEEEEu;
  const uint64_t session = 0x600000011ull;
  const bool encoded = EncodePaletteObjectEvent(record, session, out);
  expect(encoded, "encode must succeed for a non-zero session");
  expect(out.session == session, "event.session");
  expect(out.key.frame == record.frames.renderFrame, "event.key.frame");
  expect(out.key.mapEpoch == record.key.mapEpoch, "event.key.mapEpoch");
  expect(out.key.deviceEpoch == record.key.deviceEpoch, "event.key.deviceEpoch");
  expect(out.key.owner == 0u, "key.owner must stay device/record context (0)");
  expect(out.kind == Kind::ShadowState, "event.kind must be ShadowState(12)");
  expect(std::strcmp(out.label.data(), "palette-object/v1") == 0, "event.label");
  expect(out.data[0] == record.key.renderablePart, "data[0] renderablePart");
  expect(out.data[1] == record.key.runtimeModelPtr, "data[1] runtimeModelPtr");
  expect(out.data[2] == record.key.lifecycleIdentity, "data[2] lifecycleIdentity");
  // 2026-09-17 D2（版本 2 声明的语义，取代旧的"data[3] 必须为 0"保留位断言）：
  expect(out.data[3] == record.windowSegment,
         "data[3] windowSegment (declared v2 carrier, full u64)");
  for (unsigned i = 4u; i <= 8u; ++i)
    expect(out.data[i] == 0u, "data[4..8] reserved must stay zero");
  expect(out.data[9] == record.frames.manifestFrameSerial, "data[9] manifestFrameSerial");
  expect(out.data[10] == uint64_t(record.source), "data[10] source");
  expect(out.data[11] == record.hitKey, "data[11] hitKey");
  expect(out.bits[0] == uint32_t(record.rejectReason), "bits[0] rejectReason");
  expect(out.bits[1] == uint32_t(record.key.jHandle), "bits[1] jHandle");
  expect(out.bits[2] == uint32_t(record.key.rawcode), "bits[2] rawcode");
  expect(out.bits[3] == 0u && out.bits[4] == 0u, "bits[3..4] reserved must be zero");
  expect(out.bits[5] == uint32_t(record.stage), "bits[5] stage");
  for (unsigned i = 6u; i <= 9u; ++i)
    expect(out.bits[i] == 0u, "bits[6..9] reserved must be zero");
  expect(out.bits[10] == uint32_t(record.key.sessionGeneration & 0xFFFFFFFFull),
         "bits[10] sessionGeneration lo32");
  expect(out.bits[11] == 0u, "bits[11] reserved must be zero");
  expect(out.bits[12] == uint32_t(::GetCurrentThreadId()),
         "bits[12] must be the recording thread (GetCurrentThreadId)");
  expect(out.bits[13] == 0u, "bits[13] reserved must be zero");
  expect(out.bits[14] == 1u, "bits[14] bit0 identityWeak");
  // 2026-09-17 D2：bits[15] 现在是**声明后的语义**载体（不再是保留零）。
  expect(out.bits[15] == record.identityProofKind,
         "bits[15] identityProofKind (declared v2 carrier)");
  expect(out.bits[16] == 1u, "bits[16] bit0 epochUnknown");
  expect(out.bits[17] == 0u, "bits[17] reserved must be zero");
  expect(out.bits[18] == uint32_t(record.terminal), "bits[18] terminal");
  expect(out.bits[19] == 0u, "bits[19] reserved must be zero");
  expect(out.bits[20] == uint32_t(record.deltaFrames & 0xFFFFFFFFull), "bits[20] delta lo32");
  expect(out.bits[21] == uint32_t(record.frames.nativeFrameTag & 0xFFFFFFFFull),
         "bits[21] nativeFrameTag lo32");
  expect(out.bits[22] == record.hitCount, "bits[22] hitCount");
  expect(out.bits[23] == record.chainSequence, "bits[23] chainSequence");
  expect(out.bits[24] == uint32_t(record.frames.recordFrameSerial & 0xFFFFFFFFull),
         "bits[24] recordFrameSerial lo32");
  expect(out.bits[25] == uint32_t(record.frames.recordFrameSerial >> 32u),
         "bits[25] recordFrameSerial hi32");
  expect(out.bits[26] == uint32_t(record.frames.manifestPublishRevision & 0xFFFFFFFFull),
         "bits[26] manifestPublishRevision lo32");
  expect(out.bits[27] == uint32_t(record.frames.manifestPublishRevision >> 32u),
         "bits[27] manifestPublishRevision hi32");
  expect(out.bits[28] == 0x1Fu, "bits[28] flags2 (submit|draw|cleared|manifestUnknown|nativeUnknown)");
  expect(out.bits[29] == uint32_t(record.key.sessionGeneration >> 32u),
         "bits[29] sessionGeneration hi32");
  expect(out.bits[30] == uint32_t(record.firstRejectFrame & 0xFFFFFFFFull),
         "bits[30] firstRejectFrame lo32");
  expect(out.bits[31] == uint32_t(record.firstRejectFrame >> 32u), "bits[31] firstRejectFrame hi32");
  expect(out.bits[32] == uint32_t(record.frames.nativeFrameTag >> 32u), "bits[32] nativeFrameTag hi32");
  expect(out.bits[33] == uint32_t(record.deltaFrames >> 32u), "bits[33] deltaFrames hi32");
  for (unsigned i = 34u; i < 48u; ++i)
    expect(out.bits[i] == 0u, "bits[34..47] reserved must be zero");

  // 关掉的标志位必须恰好为 0（不得由上一字段残留）。
  PaletteObjectEventRecord clean{};
  clean.key.identityWeak = false;
  clean.key.epochUnknown = false;
  clean.frames.manifestUnknown = false;
  clean.frames.nativeUnknown = false;
  Event cleanOut{};
  expect(EncodePaletteObjectEvent(clean, session, cleanOut), "encode must succeed (clean record)");
  expect(cleanOut.bits[14] == 0u && cleanOut.bits[16] == 0u && cleanOut.bits[28] == 0u,
         "cleared identity/epoch/flag2 bits must be exactly zero");
  // 2026-09-17 D2：两个声明载体的**记录默认值**也必须如实落到 wire 上（不得留未初始化槽）：
  // windowSegment 默认 1（"从未 Reset 的记录"仍只能说第 1 个窗口，绝不写 0）；
  // identityProofKind 默认 0（NoIdentityProof —— 只有记录器能推导出非零值）。
  expect(cleanOut.data[3] == 1u,
         "data[3] must default to windowSegment 1 (never the reserved-zero shape)");
  expect(cleanOut.bits[15] == 0u,
         "bits[15] must default to NoIdentityProof(0) (no test-only write backdoor)");

  // session == 0 ⇒ fail-closed（不得产出不可判读的孤事件）。
  Event zeroOut{};
  zeroOut.session = 0x1234ull;
  zeroOut.bits[12] = 0x1234u;
  expect(!EncodePaletteObjectEvent(record, 0u, zeroOut), "session 0 must be refused");
  expect(zeroOut.session == 0x1234ull && zeroOut.bits[12] == 0x1234u,
         "refused encode must not write the output event");

  std::cout << "ENCODER checks=" << checks << " failures=" << failures
            << (failures == 0u ? " PASS\n" : " FAIL\n");
  g_checks += checks;
  g_failures += failures;
}

}  // namespace

int main() {
  try {
    if (!Enabled()) {
      std::cout << "FAIL: DXVK_WAR3_FRAME_EVIDENCE=1 is required\n";
      return 1;
    }
    if (!PaletteObjectEvidenceEnabled()) {
      std::cout << "FAIL: DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1 is required\n";
      return 1;
    }
    Check(!ActiveSession(), "no session before the first arm");

    RunEncoderUnitChecks();

    RunScenario("A", &ScenarioA, 256u);  // 强身份四阶段链（不同帧）
    RunScenario("B", &ScenarioB, 256u);  // 同一形状 + identityWeak=true
    RunScenario("C", &ScenarioC, 256u);  // 同一形状 + 证据环淘汰 1 条
    RunScenario("D", &ScenarioD, 256u);  // 空样本（子门开启）
    RunScenario("E", &ScenarioE, kConcurrencyCapacity, true);  // 并发所有者协议（环由控制线程冻结）
    RunAutoFreezeScenario("F", &ScenarioF, 256u);  // post-window 自动冻结（无 Control(freeze)）
  // G 必须排在最后：A..F 的序号被外部驱动的钉死值（session 世代等）依赖，插入中间会移动它们。
  RunScenario("G", &ScenarioG, 256u);  // 正常观察链（生产入口 NoteFirstSight）=> 必须导出为 version 3


    // 2026-09-18 阶段 D（批次 2，B1 **运行期半边**）：armed 的发布是**单一同步域**的发布点。
    // ⚠️ 本块属于**会话生命周期屏障**，不是编码往返组用例；它落在这个二进制里
    //    **仅为链接原因**（宿主测试目标不链接 sink 的编译单元）。
    {
      DisarmPaletteObjectEvidence();
      Check(!PaletteObjectEvidenceArmed(),
            "B1: a disarmed recorder must report armed=false");
      ArmPaletteObjectEvidence(kRecordFrameSerialBase, 0u);
      const bool barrierArmed = PaletteObjectEvidenceArmed();
      PaletteObjectEvidenceHeader barrierHeader{};
      QueryPaletteObjectEvidenceHeader(barrierHeader);
      std::cout << "BARRIER armed=" << (barrierArmed ? 1 : 0)
                << " watchCount=" << barrierHeader.watchCount
                << " emitted=" << barrierHeader.counters.emitted
                << " terminalEmitted=" << barrierHeader.counters.terminalEmitted << "\n";
      Check(!barrierArmed || barrierHeader.watchCount <= 1024u,
            "B1: when armed, the header must come from an INITIALISED recorder");
      Check(!barrierArmed ||
                barrierHeader.counters.terminalEmitted <= barrierHeader.counters.emitted,
            "B1: the header snapshot must be self-consistent");
      DisarmPaletteObjectEvidence();
      Check(!PaletteObjectEvidenceArmed(), "B1: disarm must publish armed=false");
    }
    std::cout << "ROUNDTRIP checks=" << g_checks << " failures=" << g_failures
              << (g_failures == 0u ? " PASS\n" : " FAIL\n");
    return g_failures == 0u ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "exception: " << e.what() << "\n";
    return 1;
  }
}
