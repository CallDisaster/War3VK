// 2026-09-17 对象级 palette 证据记录器的宿主机测试（纯 CPU：无 IO、无 Vulkan、无游戏）。
//
// 目标：把 src/d3d9/war3/tools/war3_palette_object_evidence.h 的**既有**字段映射、状态推进、
// 预算与终态语义逐条钉死，供离线分析器（AutoTest/analyze_palette_object_evidence.py）对账。
// 本文件**不修改**记录器 / 判定 / 发布语义；与工单不一致的既有行为只在 [OBSERVATION]/[NOTE]
// 与 main() 末尾的打印中报告，供上级裁定。
//
// 2026-09-17 03:58 上级复审后的适配（记录器已重写）：
//   1) NoteEnqueued / NoteDrawn 新增 (source, hitKey) 形参 ⇒ 事件必须携带**当时传入的实际来源**，
//      不再由"最近一次 Served"推测；Case19 逐阶段断言来源切换后三者各自保留。
//   2) 探测范围统一 + 墓碑：Case13 构造同槽碰撞、删除前项、断言后项仍可达且同键不产生第二条链；
//   3) kNormalBudget = kTotalBudget - kTerminalReserve = 3584：Case7 断言普通事件到不了 4096，
//      终态预留 512 仍可达，且总计数绝不出现 4096+512；
//   4) CloseWindow 不再清零每帧预算：Case14 断言同帧继续记录仍被 64 条上限挡住；
//   5) 先预检、再改状态、再发射：Case15 断言被拒事件不推进 chainSequence / hitCount / saw*；
//   6) 同对象同帧同阶段最多一条（不同阶段各自保留）：Case16；
//   7) chainSequence 每次发事件递增（终态也带递增序号）：Case2 断言 1,2,3,4,5；
//   8) Recovered 判定收紧：Case18 用六个反例（缺 Served / 缺 Enqueued / 缺 Drawn / 顺序回退 /
//      draw 时 selectionCleared / drawSource==None）逐一断言不得 Recovered；
//   9) hitCount == ServedCandidate 事件数：Case17（含预算拒绝场景）。
//
// 2026-09-17 往返测试（G2 后半 / G3）之后由主线程在生产头落地的两处新契约，本文件同步期望：
//   G3) deltaFrames 只由 Rejected / ServedCandidate / **终态**携带（= renderFrame-firstRejectFrame）；
//       **live 的 Enqueued / Drawn 恒 0**（冻结读方规则：非终态里只有 R/Served 带 delta）。
//       ⇒ Case2 钉死 R/Served/终态的正向值，并新增"live E/D 必须为 0"的正面见证。
//   G2) CloseWindow 置窗口关闭位且**幂等**；五个 Note* 入口（Reject/Served/Enqueued/Drawn/
//       ObjectGone）在窗口关闭后一律早退，只有 Reset 能开新窗口。
//       ⇒ Case12 / Case14 按新契约改写（"关闭后拒绝新记录"比旧的预算记账更强）。
//   ⇒ 该契约曾暴露一条**结构性发现**（单窗口内终态受每帧预算约束 ⇒ 512 预留与 4096 总额不可达），
//      主线程已按 F1/F2/F3 修复：终态只受 kTerminalReserve 约束、结算桶只在终态真的发出时递增、
//      ObjectGone/TableFull 显式写 delta。本文件同步翻转：
//      Case7(c) 正面见证 512 预留可达 + 4096 总额可达（3584 普通 + 512 终态）+ 第 513 条起记
//      droppedTerminalReserve；末尾 [FINDING] 行改写为修复后的正面见证（保留一句历史说明）。
//
// 2026-09-17 主线程两处**有意修正**（产品头已改；本文件同步更新期望，不再把它当缺陷报告）：
//   A) Insert 不再预写 Entry::lastRejectFrame（旧行为会让新建对象的第一条 Rejected 在同一次调用里
//      被"同帧去重"吞掉）。现在"建条目"那一次调用**会发射**对象的第一个 Rejected 事件，并占用
//      chainSequence 1；同一键**同一帧的第二次** Reject 仍计入 droppedDuplicatePerFrame。
//      ⇒ 凡是跨"建条目"边界的绝对计数/绝对序号一律 +1；本文件改为**相对基准**断言
//      （Case1 的 emitted=3、Case2/Case3 的序号基准、Case8 的建条目发射见证、Case14/15 的增量）。
//   B) PrepareStage / NoteReject / CloseWindow 不再把 manifestUnknown/nativeUnknown 强制写成 true，
//      改为**保留调用点口径**：调用方说 known 就 known、说 unknown 就 unknown，帧域**数值**原样保留。
//      ⇒ Case2/Case12 的"强制 unknown (fail-visible)"断言改为"保留调用点口径"，
//      同时保留"帧域数值原样保留"与"未知零 vs 已知零可分辨"两条。
//
// 2026-09-17 D6（上级裁定，主线程已落地的生产改动）：同键同帧同阶段**只有在载荷可证等价时**
// 才能压缩成一条；载荷冲突必须计数并阻止认证（不得为了让报告好看而忽略）。
//   载荷等价 ⇒ droppedDuplicatePerFrame++（按原样，不算损失）；
//   载荷冲突 ⇒ droppedPayloadConflict++（缺链必须可见）。
//   四个阶段的载荷定义（Case16 逐阶段正面见证）：
//     Rejected = lastRejectReason；ServedCandidate = lastSource + lastHitKey；
//     Enqueued = submitSource + submitHitKey + submitSelectionCleared；
//     Drawn = drawSource + drawHitKey + drawSelectionCleared。
//   ⇒ Case8 拆成"同 reason（等价）/ 不同 reason（冲突）"两形态并各自断言（**修正 A 之后
//     同一键同帧第二次 Reject 的载荷基准是该帧第一条发射事件写入的 lastRejectReason**）；
//     Case16 四个阶段各加一对（等价 + 冲突）正面见证；
//     新增案例 22 = 上级反例（同帧第一次 D 合法、第二次 D 已清空语义 palette）的直接见证，
//     必须 droppedPayloadConflict >= 1 并打印实测行；SUMMARY 之前打印含新计数的总计数。
//
// 2026-09-17 结构性重入（本次新增案例 20；主线程已把生产头的 StateLock 底层锁改为
// std::recursive_mutex）：
//   发射器是在**持记录器状态锁**的路径内回调的；生产链路上该次 append 若触发证据环的容量/窗口
//   冻结，Ring::freeze() 会调用预冻结钩子 → ClosePaletteObjectWindow() → **同一线程**再次进入
//   记录器。案例 20 用"回调里真重入 CloseWindow"的发射器做**运行时反向证据**（带看门狗）：
//   形态 A = 每次发射都重入（工单要求的最小形态，一条会发射的 NoteReject 触发），
//   形态 B = 第 4 次发射（live Drawn）时重入 ⇒ 嵌套结算一条完整链 ⇒ 终态必须是 Recovered。
//   断言：不挂死（看门狗超时即判失败）、重入的关闭确实结算出可见终态、emitted 与收到的事件数一致
//   且没有静默丢链。把 m_mutex 改回 std::mutex 时本用例必须在看门狗超时内判失败（非零退出）。

// 事件由测试内回调收集：emit 回调把 record 存进定长测试数组并计数（不参与记录器状态）。

#include "../../tools/war3_frame_evidence_core.h"  // 案例 21：真实生产证据环（Ring）
#include "../../tools/war3_palette_object_capture.h"  // 案例 23：生产口径键（MakePaletteObjectKey）
#include "../../tools/war3_palette_object_evidence.h"

#include <atomic>
#include <chrono>  // 案例 20：看门狗超时
#include <cstdint>
#include <cstdio>
#include <cstdlib>  // 仅为统计式 operator new / operator delete 的 malloc / free 转发实现
#include <cstring>
#include <thread>  // 案例 20：重入场景在独立工作线程上跑，主线程按超时做看门狗

// ---------------------------------------------------------------------------
// 零堆分配证据：统计本 TU 内的全局 operator new / delete。
// 回调、事件数组、记录器本体全部为静态/自动存储；记录器自身必须一次都不分配。
// ---------------------------------------------------------------------------
namespace {
std::atomic<uint64_t> g_newCount{0u};
std::atomic<uint64_t> g_newArrayCount{0u};
std::atomic<uint64_t> g_deleteCount{0u};
std::atomic<uint64_t> g_deleteArrayCount{0u};
} // namespace

void* operator new(std::size_t size) {
  g_newCount.fetch_add(1u, std::memory_order_relaxed);
  void* memory = std::malloc(size ? size : 1u);
  if (memory == nullptr)
    std::abort();
  return memory;
}
void* operator new[](std::size_t size) {
  g_newArrayCount.fetch_add(1u, std::memory_order_relaxed);
  void* memory = std::malloc(size ? size : 1u);
  if (memory == nullptr)
    std::abort();
  return memory;
}
void operator delete(void* memory) noexcept {
  if (memory == nullptr)
    return;
  g_deleteCount.fetch_add(1u, std::memory_order_relaxed);
  std::free(memory);
}
void operator delete[](void* memory) noexcept {
  if (memory == nullptr)
    return;
  g_deleteArrayCount.fetch_add(1u, std::memory_order_relaxed);
  std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete[](memory); }

namespace {

using dxvk::war3::tools::evidence::PaletteObjectEventRecord;
using dxvk::war3::tools::evidence::PaletteObjectEvidence;
using dxvk::war3::tools::evidence::PaletteObjectIdentityProofKind;
using dxvk::war3::tools::evidence::PaletteObjectFrames;
using dxvk::war3::tools::evidence::PaletteObjectKey;
using dxvk::war3::tools::evidence::PaletteObjectRejectReason;
using dxvk::war3::tools::evidence::PaletteObjectSource;
using dxvk::war3::tools::evidence::PaletteObjectStage;
using dxvk::war3::tools::evidence::PaletteObjectTerminal;

using Counters = PaletteObjectEvidence::Counters;

constexpr uint32_t kCollectedCapacity = 8192u;

// 定长测试收集器：不分配、不参与记录器状态。
PaletteObjectEventRecord g_collected[kCollectedCapacity];
uint32_t g_stored = 0u;
uint64_t g_emitCalls = 0u;
uint64_t g_collectorOverflow = 0u;
uint32_t g_contextMarker = 0u;
uint64_t g_contextMatches = 0u;

void Collect(void* context, const PaletteObjectEventRecord& record) {
  ++g_emitCalls;
  if (context == &g_contextMarker)
    ++g_contextMatches;
  if (g_stored < kCollectedCapacity)
    g_collected[g_stored++] = record;
  else
    ++g_collectorOverflow;
}

void ResetCollector() {
  g_stored = 0u;
  g_emitCalls = 0u;
  g_collectorOverflow = 0u;
  g_contextMatches = 0u;
}

// 单个共享记录器（sizeof(PaletteObjectEvidence) 约 224 KiB，避免每例占用栈帧）。
PaletteObjectEvidence& Recorder() {
  static PaletteObjectEvidence instance;
  return instance;
}

// 案例 1 专用：从未 Configure / Reset / Note 的实例。
PaletteObjectEvidence& UntouchedRecorder() {
  static PaletteObjectEvidence instance;
  return instance;
}

// 探测/观测专用实例（不参与任何门禁断言）。
PaletteObjectEvidence& ObservationRecorder() {
  static PaletteObjectEvidence instance;
  return instance;
}

struct CaseResult {
  uint32_t checks = 0u;
  uint32_t failures = 0u;
};
CaseResult g_case;
uint32_t g_casePassed = 0u;
uint32_t g_caseFailed = 0u;

bool Require(bool condition, const char* what) {
  ++g_case.checks;
  if (!condition) {
    ++g_case.failures;
    std::printf("    FAIL: %s\n", what);
  }
  return condition;
}

uint32_t CountStage(PaletteObjectStage stage) {
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < g_stored; ++i) {
    if (g_collected[i].stage == stage)
      ++count;
  }
  return count;
}

uint32_t CountTerminal(PaletteObjectTerminal terminal) {
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < g_stored; ++i) {
    if (g_collected[i].terminal == terminal)
      ++count;
  }
  return count;
}

const PaletteObjectEventRecord* LastEvent() {
  return g_stored == 0u ? nullptr : &g_collected[g_stored - 1u];
}

bool CountersAllZero(const Counters& c) {
  // D6（2026-09-17 上级裁定）：droppedPayloadConflict 是**可见冲突**计数，必须与既有丢失计数
  // 一起纳入"全零"判定 —— 漏掉它会让"未配置即惰性"的正面见证失去意义。
  return (c.emitted | c.terminalEmitted | c.droppedPerFrame | c.droppedPerSession |
          c.droppedTableFull | c.droppedProbeLimit | c.droppedDuplicatePerFrame |
          c.droppedPayloadConflict | c.ringEvictedAfterRecord | c.closedRecovered |
          c.closedWindowExpired | c.closedObjectGone | c.closedTableFull |
          c.closedEventLost | c.closedUnclosed | c.weakIdentityRecords |
          c.epochUnknownRecords | c.droppedTerminalReserve) == 0u;
}

PaletteObjectKey MakeKey(uint64_t part) {
  PaletteObjectKey key;
  key.renderablePart = part;
  key.runtimeModelPtr = part + 0x1000u;
  key.jHandle = part + 7u;
  key.rawcode = 0x68303031u + (part & 0xFFu);
  // 2026-09-18 P0-1：键的会话代际必须来自**当前记录器会话**（与生产一致：
  // 生产四点均用 ActiveSession() 构造键，见 d3d9_device.cpp:22142/23524 与 d3d9_war3_shadow.cpp:5234）。
  // 原夹具硬编码 1000u，测的是一个现实中不存在的会话组合。
  key.sessionGeneration = Recorder().sessionGeneration();
  key.mapEpoch = 2000u;
  key.deviceEpoch = 3000u;
  key.lifecycleIdentity = part * 3u + 1u;
  key.identityWeak = false;
  key.epochUnknown = false;
  return key;
}

PaletteObjectFrames MakeFrames(uint64_t frame) {
  PaletteObjectFrames frames;
  frames.renderFrame = frame;
  return frames; // 其余帧域与 unknown 标记保持默认（manifestUnknown/nativeUnknown = true）
}

bool SameKeyFull(const PaletteObjectKey& a, const PaletteObjectKey& b) {
  return a.renderablePart == b.renderablePart && a.runtimeModelPtr == b.runtimeModelPtr &&
         a.jHandle == b.jHandle && a.rawcode == b.rawcode &&
         a.sessionGeneration == b.sessionGeneration && a.mapEpoch == b.mapEpoch &&
         a.deviceEpoch == b.deviceEpoch && a.lifecycleIdentity == b.lifecycleIdentity &&
         a.identityWeak == b.identityWeak && a.epochUnknown == b.epochUnknown;
}

// 比较帧域**数值**与 unknown 标记。2026-09-17 起记录器**保留调用点口径**（不再强制 unknown），
// 因此调用方给的 unknown 标记必须原样出现在事件里；帧域断言必须走这个**完整**比较，
// 不允许再退回"只比数值、忽略 unknown 标记"的弱比较（否则修正 B 的回归不会被杀死）。
bool SameFrames(const PaletteObjectFrames& a, const PaletteObjectFrames& b) {
  return a.renderFrame == b.renderFrame && a.manifestFrameSerial == b.manifestFrameSerial &&
         a.manifestPublishRevision == b.manifestPublishRevision &&
         a.recordFrameSerial == b.recordFrameSerial && a.nativeFrameTag == b.nativeFrameTag &&
         a.manifestUnknown == b.manifestUnknown && a.nativeUnknown == b.nativeUnknown;
}

// 探测起点槽：与记录器 Find/Insert 的 HashKey % kWatchCapacity 一致。
uint32_t SlotOf(const PaletteObjectKey& key) {
  return static_cast<uint32_t>(PaletteObjectEvidence::HashKey(key) %
                               PaletteObjectEvidence::kWatchCapacity);
}

// 在给定键集合里找一对哈希到同一槽的键（后者必然要被探测到前者的下一个槽）。
// 8192 个候选 + 1024 槽 ⇒ 抽屉原理保证必有碰撞，无需依赖运气。
bool FindCollidingKeys(PaletteObjectKey& front, PaletteObjectKey& back) {
  int32_t firstIndex[PaletteObjectEvidence::kWatchCapacity];
  for (uint32_t i = 0u; i < PaletteObjectEvidence::kWatchCapacity; ++i)
    firstIndex[i] = -1;
  for (uint32_t i = 0u; i < 8192u; ++i) {
    const PaletteObjectKey candidate = MakeKey(0xC10000u + i);
    const uint32_t slot = SlotOf(candidate);
    if (firstIndex[slot] >= 0) {
      front = MakeKey(0xC10000u + static_cast<uint32_t>(firstIndex[slot]));
      back = candidate;
      return true;
    }
    firstIndex[slot] = static_cast<int32_t>(i);
  }
  return false;
}

uint32_t CountStageForKey(PaletteObjectStage stage, const PaletteObjectKey& key,
                          uint32_t from = 0u) {
  uint32_t count = 0u;
  for (uint32_t i = from; i < g_stored; ++i) {
    if (g_collected[i].stage == stage && SameKeyFull(g_collected[i].key, key))
      ++count;
  }
  return count;
}

const PaletteObjectEventRecord* FindTerminalForKey(const PaletteObjectKey& key,
                                                   uint32_t from) {
  for (uint32_t i = from; i < g_stored; ++i) {
    if (g_collected[i].terminal != PaletteObjectTerminal::None &&
        SameKeyFull(g_collected[i].key, key))
      return &g_collected[i];
  }
  return nullptr;
}

// 让记录器为 key 建条目：Insert 会把 firstRejectFrame 设为该帧，且**该次调用自身也会发射**
// 对象的第一个 Rejected 事件（修正 A）。因此它占用一条事件与一个 chainSequence；
// 需要绝对序号时，用建条目那条事件（g_collected[base - 1u]）作为基准，不要写死 1。
void TrackObjectFrames(const PaletteObjectKey& key,
                       const PaletteObjectFrames& frames,
                       PaletteObjectRejectReason reason) {
  Recorder().NoteReject(key, reason, frames);
}

void TrackObject(const PaletteObjectKey& key, uint64_t frame,
                 PaletteObjectRejectReason reason) {
  TrackObjectFrames(key, MakeFrames(frame), reason);
}

// ---------------------------------------------------------------------------
// 案例 1：默认 / 未配置 —— 关闭时组件侧零操作、零计数。
// ---------------------------------------------------------------------------
bool Case1DefaultUnconfigured() {
  bool ok = true;
  PaletteObjectEvidence& rec = UntouchedRecorder();
  ok &= Require(CountersAllZero(rec.counters()),
                "an unconfigured recorder must keep every counter at zero");
  ok &= Require(rec.watchCount() == 0u, "an unconfigured recorder must keep watchCount at zero");
  ok &= Require(rec.sessionGeneration() == 0u && rec.mapEpoch() == 0u,
                "an unconfigured recorder must not invent a session/map generation");

  // 无 emit 回调时仍不得崩溃、不得产生回调。
  PaletteObjectEvidence& silent = Recorder();
  silent.Configure(nullptr, nullptr);
  silent.Reset(11u, 22u);
  ResetCollector();
  const PaletteObjectKey key = MakeKey(0x900u);
  silent.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(1u)); // 建条目
  silent.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(2u)); // 可观察的 Rejected
  silent.CloseWindow(3u);                                                // 终态
  // 修正 A：建条目那一次也发射（1），第二帧可观察 Rejected（2），CloseWindow 终态（3）。
  ok &= Require(silent.counters().emitted == 3u,
                "state machine must still count events when no emitter is configured");
  ok &= Require(g_emitCalls == 0u, "a null emitter must never be called");
  std::printf("    note: 关闭/未配置时不调用 Configure/Reset/Note，记录器侧无查询、无扫描、无 IO\n");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 2：链闭合 —— Reject → Served → Enqueued → Drawn → CloseWindow ⇒ Recovered，
//          且 chainSequence 在每次发事件时递增（1,2,3,4,5；终态也带递增序号）。
// ---------------------------------------------------------------------------
bool Case2ClosedChain() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  const PaletteObjectKey key = MakeKey(0x1000u);
  // 条目建立帧携带"首次观察到的非渲染帧域"（终态结算会沿用它们）。
  PaletteObjectFrames firstFrames = MakeFrames(499u);
  firstFrames.manifestFrameSerial = 11u;
  firstFrames.manifestPublishRevision = 22u;
  firstFrames.recordFrameSerial = 33u;
  firstFrames.nativeFrameTag = 44u;
  firstFrames.manifestUnknown = false;
  firstFrames.nativeUnknown = false;
  TrackObjectFrames(key, firstFrames, PaletteObjectRejectReason::R1);
  const uint32_t base = g_stored;

  PaletteObjectFrames rejectFrames = MakeFrames(500u);
  rejectFrames.manifestFrameSerial = 111u;
  rejectFrames.manifestPublishRevision = 222u;
  rejectFrames.recordFrameSerial = 333u;
  rejectFrames.nativeFrameTag = 444u;
  rejectFrames.manifestUnknown = false;
  rejectFrames.nativeUnknown = false;

  PaletteObjectFrames servedFrames = MakeFrames(503u);
  PaletteObjectFrames enqueuedFrames = MakeFrames(504u);
  PaletteObjectFrames drawnFrames = MakeFrames(505u);

  rec.NoteReject(key, PaletteObjectRejectReason::R1, rejectFrames);
  rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 0xABCDu, servedFrames);
  rec.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, 0xABCDu,
                   enqueuedFrames, false);
  rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0xEEEEu, drawnFrames,
                false);
  rec.CloseWindow(600u);

  bool ok = true;
  ok &= Require(g_stored - base == 5u, "a closed chain must emit exactly 5 events");
  ok &= Require(g_emitCalls == g_stored, "emit callback must be called once per accepted event");
  ok &= Require(g_collectorOverflow == 0u, "test collector must not overflow");
  ok &= Require(g_contextMatches == g_emitCalls, "Configure context must reach every emit");
  if (g_stored - base == 5u) {
    const PaletteObjectEventRecord& rejected = g_collected[base + 0u];
    const PaletteObjectEventRecord& served = g_collected[base + 1u];
    const PaletteObjectEventRecord& enqueued = g_collected[base + 2u];
    const PaletteObjectEventRecord& drawn = g_collected[base + 3u];
    const PaletteObjectEventRecord& terminal = g_collected[base + 4u];

    ok &= Require(rejected.stage == PaletteObjectStage::Rejected, "event0 stage Rejected");
    ok &= Require(rejected.rejectReason == PaletteObjectRejectReason::R1, "event0 reason R1");
    ok &= Require(rejected.terminal == PaletteObjectTerminal::None, "event0 terminal None");
    ok &= Require(rejected.source == PaletteObjectSource::None, "event0 source None");
    ok &= Require(rejected.hitCount == 0u, "event0 hitCount 0 before any alternative hit");
    ok &= Require(rejected.deltaFrames == 1u, "event0 deltaFrames 500-499");
    ok &= Require(SameKeyFull(rejected.key, key), "event0 must carry the full key");
    ok &= Require(rejected.frames.renderFrame == 500u, "event0 renderFrame");
    ok &= Require(SameFrames(rejected.frames, rejectFrames),
                  "a stage event must carry its own call-site frames verbatim, "
                  "including the call-site unknown flags");

    ok &= Require(served.stage == PaletteObjectStage::ServedCandidate, "event1 stage ServedCandidate");
    ok &= Require(served.source == PaletteObjectSource::ProducerSnapshot, "event1 selected source");
    ok &= Require(served.hitKey == 0xABCDu, "event1 hitKey");
    ok &= Require(served.hitCount == 1u, "event1 hitCount 1");
    ok &= Require(served.deltaFrames == 4u, "event1 deltaFrames 503-499");
    ok &= Require(served.frames.renderFrame == 503u, "event1 renderFrame is the served frame");
    ok &= Require(served.sawSubmit == false && served.sawDraw == false,
                  "event1 must not claim submit/draw");

    ok &= Require(enqueued.stage == PaletteObjectStage::Enqueued, "event2 stage Enqueued");
    ok &= Require(enqueued.sawSubmit, "event2 records the CPU candidate enqueue");
    ok &= Require(enqueued.sawDraw == false, "event2 must not claim a draw");
    // G3 新契约（2026-09-17）：live 的 Enqueued / Drawn 必须恒 0 —— deltaFrames 只由
    // Rejected / ServedCandidate / 终态携带（冻结读方的 frozenZeroDelta 规则）。
    ok &= Require(enqueued.deltaFrames == 0u,
                  "live Enqueued deltaFrames must be 0 (only R/Served/terminal carry the delta)");
    ok &= Require(enqueued.source == PaletteObjectSource::ProducerSnapshot,
                  "event2 carries its own source");
    ok &= Require(enqueued.hitKey == 0xABCDu, "event2 carries its own hitKey");

    ok &= Require(drawn.stage == PaletteObjectStage::Drawn, "event3 stage Drawn");
    ok &= Require(drawn.sawDraw, "event3 records the draw-command stage");
    ok &= Require(drawn.frames.renderFrame == 505u, "event3 renderFrame");
    ok &= Require(drawn.source == PaletteObjectSource::DrawTimeCaptured,
                  "event3 source is the draw-time value, not the last Served value");
    ok &= Require(drawn.hitKey == 0xEEEEu, "event3 hitKey is the draw-time value");
    ok &= Require(drawn.deltaFrames == 0u,
                  "live Drawn deltaFrames must be 0 as well (the delta belongs to the terminal)");

    ok &= Require(terminal.stage == PaletteObjectStage::Drawn, "terminal event stage is Drawn");
    ok &= Require(terminal.terminal == PaletteObjectTerminal::Recovered, "terminal must be Recovered");
    ok &= Require(terminal.hitCount == 1u, "terminal hitCount");
    ok &= Require(terminal.deltaFrames == 101u, "terminal deltaFrames 600-499");
    ok &= Require(terminal.sawSubmit && terminal.sawDraw, "terminal carries submit+draw closure");
    ok &= Require(terminal.frames.renderFrame == 600u, "terminal uses the window-close frame");
    ok &= Require(terminal.frames.manifestFrameSerial == 11u &&
                      terminal.frames.manifestPublishRevision == 22u &&
                      terminal.frames.recordFrameSerial == 33u &&
                      terminal.frames.nativeFrameTag == 44u,
                  "terminal keeps the first observed frames for non-render domains");
    // 修正 B：CloseWindow 只替换 renderFrame，其余帧域（含 unknown 标记）保留首次观察到的口径。
    PaletteObjectFrames expectedTerminal = firstFrames;
    expectedTerminal.renderFrame = 600u;
    ok &= Require(SameFrames(terminal.frames, expectedTerminal),
                  "the window-close terminal must keep the call-site manifest/native domains "
                  "(known stays known, unknown stays unknown)");
    // 修正 A：TrackObjectFrames 那一次自身也发事件并占用 chainSequence 1，
    // base 之后的链序号是 2,3,4,5,6；这里断言"相对建条目事件逐条 +1"，不再写死 1..5。
    ok &= Require(base >= 1u,
                  "the entry-creation Rejected event must be inside the collected range");
    const uint32_t baseSequence = base >= 1u ? g_collected[base - 1u].chainSequence : 0u;
    ok &= Require(baseSequence == 1u,
                  "the entry-creation Rejected event must be the chain's first emitted event");
    ok &= Require(rejected.chainSequence == baseSequence + 1u &&
                      served.chainSequence == baseSequence + 2u &&
                      enqueued.chainSequence == baseSequence + 3u &&
                      drawn.chainSequence == baseSequence + 4u &&
                      terminal.chainSequence == baseSequence + 5u,
                  "chainSequence must advance by one per emitted event, relative to the "
                  "entry-creation event");
  }
  ok &= Require(CountStage(PaletteObjectStage::Rejected) == 2u,
                "two Rejected stage events: the entry-creation one (fix A) plus the explicit one");
  ok &= Require(CountStage(PaletteObjectStage::ServedCandidate) == 1u,
                "one ServedCandidate stage event");
  ok &= Require(CountStage(PaletteObjectStage::Enqueued) == 1u, "one Enqueued stage event");
  ok &= Require(CountStage(PaletteObjectStage::Drawn) == 2u,
                "the live Drawn event plus the terminal event carry the Drawn stage");
  const Counters& c = rec.counters();
  ok &= Require(c.closedRecovered == 1u, "closedRecovered");
  ok &= Require(c.closedUnclosed == 0u && c.closedWindowExpired == 0u, "no other terminal bucket");
  ok &= Require(rec.watchCount() == 0u, "CloseWindow releases the settled entry");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 3：首次替代命中不得删表；同一对象多来源命中累加 hitCount。
// ---------------------------------------------------------------------------
bool Case3FirstHitKeepsEntry() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  const PaletteObjectKey key = MakeKey(0x2000u);
  TrackObject(key, 699u, PaletteObjectRejectReason::R2);
  const uint32_t base = g_stored;
  rec.NoteReject(key, PaletteObjectRejectReason::R2, MakeFrames(700u));
  bool ok = true;
  ok &= Require(rec.watchCount() == 1u, "reject inserts exactly one watch entry");
  ok &= Require(g_stored - base == 1u, "the observable reject emits one event");

  rec.NoteServed(key, PaletteObjectSource::ArenaSlot, 7u, MakeFrames(700u));
  ok &= Require(rec.watchCount() == 1u, "first alternative hit must NOT delete the watch entry");
  ok &= Require(g_stored - base == 2u, "the first alternative hit emits one event");
  if (g_stored - base == 2u)
    ok &= Require(g_collected[base + 1u].hitCount == 1u, "first hit sets hitCount 1");

  rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 9u, MakeFrames(701u));
  ok &= Require(rec.watchCount() == 1u, "the entry survives a repeated alternative hit");
  ok &= Require(g_stored - base == 3u, "the second alternative hit emits one event");
  if (g_stored - base == 3u) {
    ok &= Require(g_collected[base + 2u].hitCount == 2u, "second hit increments hitCount to 2");
    ok &= Require(g_collected[base + 2u].source == PaletteObjectSource::ProducerSnapshot,
                  "the last actually selected source is recorded");
    ok &= Require(g_collected[base + 2u].hitKey == 9u, "the last hit payload is recorded");
    ok &= Require(g_collected[base + 2u].deltaFrames == 2u, "second hit deltaFrames 701-699");
    ok &= Require(g_collected[base + 2u].chainSequence == 4u,
                  "chainSequence advances once per emitted event (entry-creation + reject + "
                  "first served = 3, so this second served is 4)");
  }
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 4：链不完整（只 Reject + Served）⇒ Unclosed，绝不是 Recovered。
// ---------------------------------------------------------------------------
bool Case4IncompleteChainUnclosed() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  const PaletteObjectKey key = MakeKey(0x3000u);
  TrackObject(key, 798u, PaletteObjectRejectReason::R1);
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(799u));
  rec.NoteServed(key, PaletteObjectSource::PoseKernel, 1u, MakeFrames(800u));
  rec.CloseWindow(900u);

  bool ok = true;
  const PaletteObjectEventRecord* last = LastEvent();
  ok &= Require(last != nullptr, "an incomplete chain must still close");
  if (last != nullptr) {
    ok &= Require(last->terminal == PaletteObjectTerminal::Unclosed,
                  "a caught-but-not-drawn chain must be Unclosed");
    ok &= Require(last->terminal != PaletteObjectTerminal::Recovered,
                  "a palette hit alone must never be Recovered");
    ok &= Require(last->hitCount == 1u, "Unclosed keeps the observed hitCount");
    ok &= Require(last->sawDraw == false, "Unclosed must not claim a draw");
    ok &= Require(last->source == PaletteObjectSource::None,
                  "an un-drawn entry has no draw-time source to report");
  }
  ok &= Require(rec.counters().closedUnclosed == 1u, "closedUnclosed");
  ok &= Require(rec.counters().closedRecovered == 0u, "closedRecovered must stay 0");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 5：从未被接住 ⇒ WindowExpired（与 Unclosed 分列）。
// ---------------------------------------------------------------------------
bool Case5WindowExpired() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  const PaletteObjectKey key = MakeKey(0x4000u);
  TrackObject(key, 800u, PaletteObjectRejectReason::R3);
  rec.CloseWindow(900u);

  bool ok = true;
  const PaletteObjectEventRecord* last = LastEvent();
  ok &= Require(last != nullptr, "window expiry must emit a terminal event");
  if (last != nullptr) {
    ok &= Require(last->terminal == PaletteObjectTerminal::WindowExpired,
                  "a never-caught object must close as WindowExpired");
    ok &= Require(last->hitCount == 0u, "WindowExpired keeps hitCount 0");
    ok &= Require(last->deltaFrames == 100u, "WindowExpired deltaFrames");
  }
  ok &= Require(rec.counters().closedWindowExpired == 1u, "closedWindowExpired");
  ok &= Require(rec.counters().closedUnclosed == 0u,
                "WindowExpired must not be charged to Unclosed");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 6：对象消失 ⇒ ObjectGone 终态 + 表项归零。
// ---------------------------------------------------------------------------
bool Case6ObjectGone() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  const PaletteObjectKey key = MakeKey(0x5000u);
  TrackObject(key, 810u, PaletteObjectRejectReason::R0);
  const uint32_t before = g_stored;
  const uint64_t terminalBefore = rec.counters().terminalEmitted;
  rec.NoteObjectGone(key);

  bool ok = true;
  ok &= Require(g_stored == before + 1u, "ObjectGone must emit exactly one terminal event");
  const PaletteObjectEventRecord* last = LastEvent();
  if (last != nullptr) {
    ok &= Require(last->terminal == PaletteObjectTerminal::ObjectGone, "terminal ObjectGone");
    ok &= Require(SameKeyFull(last->key, key), "ObjectGone carries the object key");
    ok &= Require(last->frames.renderFrame == 810u, "ObjectGone keeps the first frames");
    // F3：终态 delta 契约（renderFrame - firstRejectFrame）。本路径 renderFrame 仍取首帧 ⇒ 必为 0；
    // "显式写"这件事由静态门禁 §11k(g) 钉死，这里钉死读得到的契约值。
    ok &= Require(last->deltaFrames == 0u,
                  "ObjectGone must carry the terminal deltaFrames contract value (F3)");
  }
  ok &= Require(rec.watchCount() == 0u, "ObjectGone releases the watch entry");
  ok &= Require(rec.counters().closedObjectGone == 1u, "closedObjectGone");
  ok &= Require(rec.counters().terminalEmitted == terminalBefore + 1u,
                "ObjectGone is a terminal inside the total");

  const uint32_t after = g_stored;
  rec.NoteObjectGone(key);
  ok &= Require(g_stored == after, "a released object cannot emit a second terminal");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 7：预算 —— 每帧 64 / 普通事件 3584（kNormalBudget）/ 总额 4096 / 终态预留 512。
// ---------------------------------------------------------------------------
bool Case7Budgets() {
  bool ok = true;

  { // (a) 每帧上限：同一帧内只允许 64 条事件。
    PaletteObjectEvidence& rec = Recorder();
    rec.Configure(&Collect, &g_contextMarker);
    rec.Reset(1000u, 2000u);
    ResetCollector();
    for (uint32_t i = 0u; i < 100u; ++i)
      rec.NoteReject(MakeKey(0x10000u + i), PaletteObjectRejectReason::R1,
                     MakeFrames(1u + i)); // 各自独立帧建条目
    const uint32_t storedBefore = g_stored;
    const uint64_t emittedBefore = rec.counters().emitted;
    const uint64_t droppedFrameBefore = rec.counters().droppedPerFrame;
    const uint64_t droppedSessionBefore = rec.counters().droppedPerSession;

    for (uint32_t i = 0u; i < 100u; ++i)
      rec.NoteReject(MakeKey(0x10000u + i), PaletteObjectRejectReason::R1,
                     MakeFrames(500u));

    ok &= Require(g_stored - storedBefore == 64u,
                  "per-frame budget must stop at 64 emitted events");
    ok &= Require(rec.counters().emitted == emittedBefore + 64u,
                  "emitted must match the per-frame budget");
    ok &= Require(rec.counters().droppedPerFrame == droppedFrameBefore + 36u,
                  "the 36 refused events must be counted as droppedPerFrame");
    ok &= Require(rec.counters().droppedPerSession == droppedSessionBefore,
                  "per-frame refusal must not be charged to the session budget");
    ok &= Require(rec.watchCount() == 100u,
                  "per-frame refusal must not silently drop the watch entry");
  }

  { // (b) 普通事件上限 = kNormalBudget = 4096-512 = 3584；终态预留 512 仍可达。
    PaletteObjectEvidence& rec = Recorder();
    rec.Configure(&Collect, &g_contextMarker);
    rec.Reset(1000u, 2000u);
    ResetCollector();
    const PaletteObjectKey key = MakeKey(0x20000u);
    TrackObject(key, 1u, PaletteObjectRejectReason::R1); // 建条目
    uint64_t frame = 1u;
    while (rec.counters().emitted < PaletteObjectEvidence::kNormalBudget &&
           frame < 100000u) {
      ++frame;
      rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 0x55u,
                     MakeFrames(frame));
    }
    ok &= Require(rec.counters().emitted == PaletteObjectEvidence::kNormalBudget,
                  "normal events must stop exactly at kNormalBudget (3584)");
    ok &= Require(PaletteObjectEvidence::kNormalBudget ==
                      PaletteObjectEvidence::kTotalBudget -
                          PaletteObjectEvidence::kTerminalReserve,
                  "kNormalBudget must be kTotalBudget - kTerminalReserve");
    ok &= Require(PaletteObjectEvidence::kNormalBudget <
                      PaletteObjectEvidence::kTotalBudget,
                  "the terminal reserve must be really subtracted from the total");
    ok &= Require(rec.counters().droppedPerFrame == 0u,
                  "one served event per frame keeps the per-frame budget open");
    ok &= Require(rec.counters().droppedPerSession == 0u,
                  "no session drop while under the normal budget");

    // 第 3585 条起：普通事件被终态预留挡下 ⇒ droppedPerSession 增长且 emitted 不再增长。
    const uint64_t normalEmitted = rec.counters().emitted;
    const uint64_t droppedBefore = rec.counters().droppedPerSession;
    for (uint32_t i = 0u; i < 3u; ++i) {
      ++frame;
      rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(frame));
      rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 0x55u,
                     MakeFrames(frame));
    }
    ok &= Require(rec.counters().emitted == normalEmitted,
                  "normal events must not grow past kNormalBudget");
    ok &= Require(rec.counters().droppedPerSession == droppedBefore + 6u,
                  "events refused by the terminal reserve must be counted as droppedPerSession");
    ok &= Require(rec.counters().emitted < PaletteObjectEvidence::kTotalBudget,
                  "normal events alone must never fill the whole 4096 budget");

    // 终态在普通预算耗尽后仍然可达（终态预留的真实作用）。
    ++frame;
    rec.CloseWindow(frame);
    ok &= Require(rec.counters().terminalEmitted == 1u,
                  "a terminal must still be emitted after the normal budget is exhausted");
    ok &= Require(rec.counters().emitted == normalEmitted + 1u,
                  "terminals are charged to the same total budget");
    ok &= Require(CountTerminal(PaletteObjectTerminal::None) < g_stored,
                  "the run must contain terminal events");

    // G2 后半新契约：窗口关闭后**拒绝一切新记录**（五个 Note* 入口早退），CloseWindow 幂等。
    // 这是比"同帧继续记录仍受 64 条上限"更强的 fail-closed 保证。
    const uint32_t storedAtClose = g_stored;
    const uint64_t emittedAtClose = rec.counters().emitted;
    const uint64_t terminalAtClose = rec.counters().terminalEmitted;
    const uint64_t perFrameAtClose = rec.counters().droppedPerFrame;
    const uint64_t perSessionAtClose = rec.counters().droppedPerSession;
    const uint64_t duplicateAtClose = rec.counters().droppedDuplicatePerFrame;
    for (uint32_t i = 0u; i < 2u; ++i) {
      const PaletteObjectKey closed = MakeKey(0x30000u + i);
      rec.NoteReject(closed, PaletteObjectRejectReason::R1, MakeFrames(frame + 1u + i));
      rec.NoteServed(closed, PaletteObjectSource::ProducerSnapshot, i, MakeFrames(frame + 2u + i));
      rec.NoteEnqueued(closed, PaletteObjectSource::ArenaSlot, i, MakeFrames(frame + 3u + i),
                       false);
      rec.NoteDrawn(closed, PaletteObjectSource::DrawTimeCaptured, i, MakeFrames(frame + 4u + i),
                    false);
      rec.NoteObjectGone(closed);
      rec.CloseWindow(frame + 5u + i);
    }
    ok &= Require(g_stored == storedAtClose && rec.counters().emitted == emittedAtClose &&
                      rec.counters().terminalEmitted == terminalAtClose,
                  "no Note* entry and no repeated CloseWindow may emit after the window closed");
    ok &= Require(rec.counters().droppedPerFrame == perFrameAtClose &&
                      rec.counters().droppedPerSession == perSessionAtClose &&
                      rec.counters().droppedDuplicatePerFrame == duplicateAtClose,
                  "a closed window must not charge any drop");
    ok &= Require(rec.watchCount() == 0u, "a closed window must not track a new object");
  }

  { // (c) F1/F2 修复后的**正面见证**（2026-09-17 主线程）：
    //     单窗口内终态只受 kTerminalReserve(512) 约束、**不再**受每帧 64 上限约束（F1），
    //     因此 512 预留可达、预留记账分支可达（第 513 条起记 droppedTerminalReserve），
    //     4096 总额可达（3584 普通 + 512 终态）；结算桶只在终态真的发出时递增（F2）。
    //     历史（修复前、已被 F1 推翻的旧契约）：终态与普通事件共用每帧预算且窗口只结算一次，
    //     曾使单窗口 terminalEmitted <= 64、512 预留与 4096 总额都不可达 —— 该结论已撤回。
    PaletteObjectEvidence& rec = Recorder();
    rec.Configure(&Collect, &g_contextMarker);
    rec.Reset(1000u, 2000u);
    ResetCollector();
    // 分帧建满 1024 条在途条目（每帧正好 64 条普通事件）。
    for (uint32_t i = 0u; i < PaletteObjectEvidence::kWatchCapacity; ++i)
      rec.NoteReject(MakeKey(0x50000u + i * 0x40u), PaletteObjectRejectReason::R1,
                     MakeFrames(1000u + i / PaletteObjectEvidence::kPerFrameBudget));
    ok &= Require(rec.watchCount() == PaletteObjectEvidence::kWatchCapacity,
                  "the reserve witness needs a completely full watch table");
    ok &= Require(rec.counters().emitted == PaletteObjectEvidence::kWatchCapacity,
                  "one entry-creation Rejected event per tracked object");
    // 同一个键跨帧继续普通事件，把普通预算补到 kNormalBudget（每帧 1 条，不触发同帧去重）。
    const PaletteObjectKey filler = MakeKey(0x50000u);
    uint64_t frame =
        1000u + PaletteObjectEvidence::kWatchCapacity / PaletteObjectEvidence::kPerFrameBudget;
    while (rec.counters().emitted < PaletteObjectEvidence::kNormalBudget && frame < 100000u) {
      ++frame;
      rec.NoteReject(filler, PaletteObjectRejectReason::R1, MakeFrames(frame));
    }
    ok &= Require(rec.counters().emitted == PaletteObjectEvidence::kNormalBudget,
                  "normal events must stop exactly at kNormalBudget (3584)");
    ok &= Require(rec.counters().terminalEmitted == 0u,
                  "no terminal may exist before the window closes");
    ok &= Require(rec.counters().droppedPerFrame == 0u,
                  "one normal event per frame keeps the per-frame budget open");
    // 关闭窗口：1024 条在途条目全部结算；终态不受每帧 64 限制（F1），只受 512 预留约束。
    // 注意最后一次 BeginFrame 的每帧预算只用了 1 条 ⇒ 能发出 512 条终态本身就是 F1 的见证。
    rec.CloseWindow(frame + 1u);
    ok &= Require(rec.counters().terminalEmitted == PaletteObjectEvidence::kTerminalReserve,
                  "the terminal reserve (512) must be fully reachable in one window");
    ok &= Require(rec.counters().terminalEmitted > PaletteObjectEvidence::kPerFrameBudget,
                  "terminals must not be constrained by the per-frame budget (F1)");
    ok &= Require(rec.counters().emitted == PaletteObjectEvidence::kTotalBudget,
                  "total emitted must be exactly kTotalBudget");
    ok &= Require(rec.counters().emitted ==
                      PaletteObjectEvidence::kNormalBudget +
                          PaletteObjectEvidence::kTerminalReserve,
                  "the total must be reachable as 3584 normal + 512 terminal (F1)");
    ok &= Require(g_stored == PaletteObjectEvidence::kTotalBudget,
                  "exactly 4096 accepted events must reach the emitter");
    // 第 513 条起：被预留拒绝，且必须可见（F1：记 droppedTerminalReserve，**不再**记 droppedPerFrame）。
    ok &= Require(rec.counters().droppedTerminalReserve ==
                      PaletteObjectEvidence::kWatchCapacity -
                          PaletteObjectEvidence::kTerminalReserve,
                  "the 513th terminal onward must be counted in droppedTerminalReserve");
    ok &= Require(rec.counters().droppedPerFrame == 0u,
                  "terminals must never be charged to the per-frame budget (F1)");
    ok &= Require(rec.counters().droppedPerSession == 0u,
                  "terminals refused by the reserve must not be charged to the session budget");
    // F2：结算桶只在终态真的发出时递增 ⇒ 只有前 512 条被结算；
    // 其余 512 条因预留耗尽被丢弃并计数，**不得**声称全部已结算。
    ok &= Require(rec.counters().closedWindowExpired ==
                      PaletteObjectEvidence::kTerminalReserve,
                  "only the 512 emitted terminals may be counted as settled (F2)");
    ok &= Require(rec.counters().closedRecovered == 0u &&
                      rec.counters().closedUnclosed == 0u &&
                      rec.counters().closedObjectGone == 0u &&
                      rec.counters().closedTableFull == 0u,
                  "no other settlement bucket may move on this never-caught run");
    ok &= Require(rec.counters().emitted <= PaletteObjectEvidence::kTotalBudget,
                  "emitted must never exceed the session budget");
    ok &= Require(rec.counters().emitted !=
                      PaletteObjectEvidence::kTotalBudget +
                          PaletteObjectEvidence::kTerminalReserve,
                  "totals must never be reported as 4096+512");
    ok &= Require(rec.watchCount() == 0u, "CloseWindow releases every settled entry");
    // 关闭后仍拒绝新记录（G2 后半），且不再改任何计数（含结算桶）。
    const uint32_t storedAfterClose = g_stored;
    const uint64_t emittedAfterClose = rec.counters().emitted;
    const uint64_t reserveAfterClose = rec.counters().droppedTerminalReserve;
    rec.NoteReject(MakeKey(0xFFFF00u), PaletteObjectRejectReason::R1, MakeFrames(frame + 2u));
    rec.CloseWindow(frame + 3u);
    ok &= Require(g_stored == storedAfterClose && rec.counters().emitted == emittedAfterClose &&
                      rec.counters().droppedTerminalReserve == reserveAfterClose,
                  "a closed window must stay closed (no emit, no re-settlement)");
  }
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 8：同一键同帧同阶段只允许压缩成一条，**且必须先比较载荷**（D6，2026-09-17 上级裁定）。
//   载荷可证等价（Rejected 的 lastRejectReason 相同）⇒ droppedDuplicatePerFrame（按原样，不算损失）；
//   载荷冲突（reason 不同）⇒ droppedPayloadConflict（**不得**被压缩成"什么都没发生"）。
// 冲突形态的正向要求：g_stored（导出可见条数）不增长，但 droppedPayloadConflict 必须增长（可见）。
// ---------------------------------------------------------------------------
bool Case8DuplicateRejectPerFrame() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  const PaletteObjectKey key = MakeKey(0x6000u);
  const uint32_t beforeEntry = g_stored;
  const uint64_t duplicateBeforeEntry = rec.counters().droppedDuplicatePerFrame;
  TrackObject(key, 6u, PaletteObjectRejectReason::R1);
  const uint32_t base = g_stored;

  bool ok = true;
  // 修正 A 见证：建条目那一次调用本身必须发射第一条 Rejected（Insert 不得预写 lastRejectFrame），
  // 而且不得被记成"同帧重复"（旧行为会把它计入 droppedDuplicatePerFrame 并吞掉事件）。
  ok &= Require(g_stored - beforeEntry == 1u,
                "the object's FIRST Rejected event must be emitted when the entry is created");
  ok &= Require(g_collected[beforeEntry].stage == PaletteObjectStage::Rejected &&
                    g_collected[beforeEntry].chainSequence == 1u,
                "the entry-creation event is the chain's first Rejected with chainSequence 1");
  ok &= Require(rec.counters().droppedDuplicatePerFrame == duplicateBeforeEntry,
                "creating an entry must not be charged as a same-frame duplicate");

  // 同帧的**第一条** Reject（帧号变化 ⇒ 正常发射，并把载荷指纹 lastRejectReason 写进条目）。
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(7u));
  ok &= Require(g_stored - base == 1u, "the first reject of a new frame must be emitted");
  const uint64_t duplicateBase = rec.counters().droppedDuplicatePerFrame;
  const uint64_t conflictBase = rec.counters().droppedPayloadConflict;
  // 同帧同阶段的**载荷可证等价**第二次 Reject（reason 与条目里的 lastRejectReason 相同）
  // ⇒ 等价重复：只累加 droppedDuplicatePerFrame，且**不得**记成冲突。
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(7u));
  ok &= Require(g_stored - base == 1u, "same key in the same frame must emit only once");
  ok &= Require(rec.counters().droppedDuplicatePerFrame == duplicateBase + 1u,
                "a payload-equivalent duplicate must be counted as droppedDuplicatePerFrame");
  ok &= Require(rec.counters().droppedPayloadConflict == conflictBase,
                "a payload-equivalent duplicate must NOT be charged as a payload conflict");

  // 同帧同阶段的**载荷冲突**第三次 Reject（reason 与首次不同）⇒ 不同事实，必须可见：
  // 导出条数不增长（同帧同阶段仍只有一条），而 droppedPayloadConflict 必须 +1。
  const uint32_t storedBeforeConflict = g_stored;
  rec.NoteReject(key, PaletteObjectRejectReason::R2, MakeFrames(7u));
  ok &= Require(g_stored == storedBeforeConflict,
                "a payload-conflicting same-frame reject still may not add a second event");
  ok &= Require(rec.counters().droppedPayloadConflict == conflictBase + 1u,
                "a same-frame reject with a DIFFERENT reason must be counted as "
                "droppedPayloadConflict (a conflict may never look like nothing happened)");
  ok &= Require(rec.counters().droppedDuplicatePerFrame == duplicateBase + 1u,
                "the conflict must not be charged as a payload-equivalent duplicate");
  ok &= Require(rec.counters().droppedPerFrame == 0u &&
                    rec.counters().droppedPerSession == 0u,
                "a same-frame payload conflict is not a budget drop");
  ok &= Require(rec.counters().droppedTerminalReserve == 0u,
                "a same-frame payload conflict must not touch the terminal reserve bucket");
  ok &= Require(g_collected[base].rejectReason == PaletteObjectRejectReason::R1,
                "the compressed-away conflict must not overwrite the emitted event's payload");

  // 换帧后仍然正常发射，且携带该调用点自己的 reason（冲突计数不得掩盖后续事实）。
  rec.NoteReject(key, PaletteObjectRejectReason::R2, MakeFrames(8u));
  ok &= Require(g_stored - base == 2u, "a later frame emits the reject again");
  if (g_stored - base == 2u) {
    ok &= Require(g_collected[base + 1u].rejectReason == PaletteObjectRejectReason::R2,
                  "the later reject carries its own reason");
    ok &= Require(g_collected[base + 1u].deltaFrames == 2u, "the later reject deltaFrames 8-6");
  }
  ok &= Require(rec.counters().droppedDuplicatePerFrame == duplicateBase + 1u &&
                    rec.counters().droppedPayloadConflict == conflictBase + 1u,
                "a next-frame reject must not grow either same-frame bucket");
  std::printf("    D6 reject two-way witness: emitted=%u droppedDuplicatePerFrame=%llu "
              "droppedPayloadConflict=%llu (equivalent=+1 for the same reason, conflict=+1 "
              "for a different reason; the conflict added no event)\n",
              static_cast<unsigned>(g_stored - base),
              static_cast<unsigned long long>(rec.counters().droppedDuplicatePerFrame -
                                              duplicateBase),
              static_cast<unsigned long long>(rec.counters().droppedPayloadConflict -
                                              conflictBase));

  // 2026-09-18 独立复审 D 定案 + 写方修正①：首见链的去重必须按**条目**，不得按帧。
  // 修复前去重维度是 (对象键, renderFrame)，而记录标记只在真的发射时更新 ⇒ 跨帧不去重 ⇒
  // 同一对象每帧再发一条 FirstSight。实机后果：32 个对象发出 1304 条（≈41 帧/对象），
  // 每对象每帧吃掉 2 个非终态配额 ⇒ 每帧只有 64/2 = 32 个对象能拿到链首，
  // 且 kNormalBudget=3584 被迅速打满（实测 emitted − terminal 恰好 = 3584），
  // 之后 75 个对象「有条目、有终态、零阶段事件」⇒ 解析器只能看到 ["Drawn"]。
  // 本用例是那条修复的**回归锁**：现有夹具都跑单帧，无法区分"发一次"与"每帧发"。
  {
    const uint32_t storedBeforeFirstSight = g_stored;
    rec.NoteFirstSight(key, MakeFrames(7u));
    ok &= Require(g_stored - storedBeforeFirstSight == 1u,
                  "the first NoteFirstSight of an entry must be emitted");
    const uint64_t dupBeforeFirstSight = rec.counters().droppedDuplicatePerFrame;
    // **跨帧**的重复首见：必须一条都不再发（修复前这里会多发一条）。
    rec.NoteFirstSight(key, MakeFrames(9u));
    ok &= Require(g_stored - storedBeforeFirstSight == 1u,
                  "a repeated NoteFirstSight on a LATER frame must NOT emit again "
                  "(the chain head is per entry, not per frame)");
    ok &= Require(rec.counters().droppedDuplicatePerFrame == dupBeforeFirstSight + 1u,
                  "the repeated first sight must be counted as droppedDuplicatePerFrame");
    ok &= Require(rec.counters().firstSightEmitted == 1u,
                  "exactly one first-sight emission per entry");
    std::printf("    firstSight once-per-entry witness: stored=%u firstSightEmitted=%llu "
                "droppedDuplicatePerFrame=%llu\n",
                static_cast<unsigned>(g_stored - storedBeforeFirstSight),
                static_cast<unsigned long long>(rec.counters().firstSightEmitted),
                static_cast<unsigned long long>(rec.counters().droppedDuplicatePerFrame));
  }
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 9：表满（1024 槽）⇒ 一条 TableFull 终态 + droppedTableFull。
// ---------------------------------------------------------------------------
bool Case9TableFull() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  bool ok = true;
  for (uint32_t i = 0u; i < PaletteObjectEvidence::kWatchCapacity; ++i)
    rec.NoteReject(MakeKey(0x40000u + i * 0x40u), PaletteObjectRejectReason::R1,
                   MakeFrames(6000u + i));
  ok &= Require(rec.watchCount() == PaletteObjectEvidence::kWatchCapacity,
                "the fixed watch table must fill to 1024 entries");
  ok &= Require(rec.counters().droppedProbeLimit == 0u,
                "filling the table must not exhaust the probe range");

  const PaletteObjectKey extra = MakeKey(0x59000u);
  const uint32_t before = g_stored;
  const uint64_t terminalBefore = rec.counters().terminalEmitted;
  rec.NoteReject(extra, PaletteObjectRejectReason::R3, MakeFrames(9000u));
  ok &= Require(g_stored == before + 1u, "table full must emit exactly one terminal event");
  // 2026-09-17 成本测量后新增**布隆前置否定**：表满 + 未知键必须走常数级负查找，
  // **不得**再走满 1024 槽（加布隆前这里是 1 次全表走查、约 719 ns）。
  ok &= Require(rec.counters().droppedProbeLimit == 0u,
                "a full table plus an unknown key must be a bloom-miss O(1) negative lookup "
                "(no full probe-range walk since the bloom pre-filter was added)");
  const PaletteObjectEventRecord* last = LastEvent();
  if (last != nullptr) {
    ok &= Require(last->terminal == PaletteObjectTerminal::TableFull, "terminal TableFull");
    ok &= Require(last->stage == PaletteObjectStage::Rejected,
                  "TableFull is a reject-path terminal");
    ok &= Require(last->rejectReason == PaletteObjectRejectReason::R3, "TableFull keeps the reason");
    ok &= Require(SameKeyFull(last->key, extra), "TableFull carries the untracked object key");
    ok &= Require(last->firstRejectFrame == 9000u, "TableFull carries the reject frame");
    // F3：TableFull 也是终态 ⇒ 必须显式携带 delta 契约值（同一帧 ⇒ 0）。
    ok &= Require(last->deltaFrames == 0u,
                  "TableFull must carry the terminal deltaFrames contract value (F3)");
  }
  ok &= Require(rec.counters().droppedTableFull == 1u, "droppedTableFull");
  ok &= Require(rec.counters().closedTableFull == 1u, "closedTableFull");
  ok &= Require(rec.counters().terminalEmitted == terminalBefore + 1u,
                "TableFull is a terminal inside the total");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 10：弱身份 / 代际未知必须可见（可单列比例）。
// ---------------------------------------------------------------------------
bool Case10WeakIdentityVisible() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  bool ok = true;
  for (uint32_t i = 0u; i < 10u; ++i) {
    PaletteObjectKey weak = MakeKey(0x70000u + i);
    weak.identityWeak = true;
    weak.epochUnknown = true;
    rec.NoteReject(weak, PaletteObjectRejectReason::R1, MakeFrames(100u + i));
  }
  ok &= Require(rec.counters().weakIdentityRecords == 10u, "weakIdentityRecords");
  ok &= Require(rec.counters().epochUnknownRecords == 10u, "epochUnknownRecords");

  const uint32_t base = g_stored;
  for (uint32_t i = 0u; i < 10u; ++i) {
    PaletteObjectKey weak = MakeKey(0x70000u + i);
    weak.identityWeak = true;
    weak.epochUnknown = true;
    rec.NoteReject(weak, PaletteObjectRejectReason::R1, MakeFrames(200u + i));
  }
  ok &= Require(g_stored - base == 10u, "each weak object still emits its reject event");
  if (g_stored - base == 10u) {
    ok &= Require(g_collected[base].key.identityWeak, "identityWeak must be visible on the event");
    ok &= Require(g_collected[base].key.epochUnknown, "epochUnknown must be visible on the event");
  }

  for (uint32_t i = 0u; i < 5u; ++i)
    rec.NoteReject(MakeKey(0x80000u + i), PaletteObjectRejectReason::R1, MakeFrames(300u + i));
  const uint32_t strongBase = g_stored;
  for (uint32_t i = 0u; i < 5u; ++i)
    rec.NoteReject(MakeKey(0x80000u + i), PaletteObjectRejectReason::R1, MakeFrames(400u + i));
  ok &= Require(g_stored - strongBase == 5u, "strong-identity objects emit one event each");
  ok &= Require(rec.counters().weakIdentityRecords == 10u,
                "strong-identity objects must not inflate the weak bucket");
  ok &= Require(rec.counters().epochUnknownRecords == 10u,
                "known-epoch objects must not inflate the unknown bucket");
  if (g_stored > strongBase)
    ok &= Require(!g_collected[g_stored - 1u].key.identityWeak &&
                      !g_collected[g_stored - 1u].key.epochUnknown,
                  "strong keys must carry false flags, not a 0-value stand-in");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 11：记录器自身零堆分配（统计本 TU 的全局 operator new/delete）。
// ---------------------------------------------------------------------------
bool Case11ZeroHeapAllocation() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1234u, 5678u);
  ResetCollector();

  const uint64_t newBefore = g_newCount.load(std::memory_order_relaxed);
  const uint64_t newArrayBefore = g_newArrayCount.load(std::memory_order_relaxed);
  const uint64_t deleteBefore = g_deleteCount.load(std::memory_order_relaxed);
  const uint64_t deleteArrayBefore = g_deleteArrayCount.load(std::memory_order_relaxed);

  // 覆盖全部入口：Configure / Reset / 四个 Note* / NoteObjectGone / NoteRingEviction /
  // CloseWindow / counters / watchCount / 会话与地图查询。
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(9999u, 8888u);
  const PaletteObjectKey key = MakeKey(0x90000u);
  for (uint32_t i = 0u; i < 8u; ++i) {
    const PaletteObjectFrames frames = MakeFrames(10u + i);
    rec.NoteReject(key, PaletteObjectRejectReason::R1, frames);
    rec.NoteServed(key, PaletteObjectSource::ArenaSlot, i, frames);
    rec.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, uint64_t(i), frames,
                     (i & 1u) != 0u);
    rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, uint64_t(i) + 0x100u,
                  frames, (i & 1u) != 0u);
  }
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(20u));
  rec.NoteReject(key, PaletteObjectRejectReason::R2, MakeFrames(20u)); // 同帧重复拒绝
  const PaletteObjectKey gone = MakeKey(0x91000u);
  rec.NoteReject(gone, PaletteObjectRejectReason::R3, MakeFrames(30u));
  rec.NoteObjectGone(gone);
  rec.NoteRingEviction(3u);
  for (uint32_t i = 0u; i < 70u; ++i)
    rec.NoteReject(MakeKey(0xA0000u + i), PaletteObjectRejectReason::R1, MakeFrames(40u));
  rec.CloseWindow(50u);
  const uint64_t emitted = rec.counters().emitted;
  const uint32_t watched = rec.watchCount();
  const uint64_t session = rec.sessionGeneration();
  const uint64_t mapEpoch = rec.mapEpoch();

  const uint64_t newAfter = g_newCount.load(std::memory_order_relaxed);
  const uint64_t newArrayAfter = g_newArrayCount.load(std::memory_order_relaxed);
  const uint64_t deleteAfter = g_deleteCount.load(std::memory_order_relaxed);
  const uint64_t deleteArrayAfter = g_deleteArrayCount.load(std::memory_order_relaxed);

  bool ok = true;
  ok &= Require(newAfter == newBefore, "the recorder must not call operator new");
  ok &= Require(newArrayAfter == newArrayBefore, "the recorder must not call operator new[]");
  ok &= Require(deleteAfter == deleteBefore, "the recorder must not call operator delete");
  ok &= Require(deleteArrayAfter == deleteArrayBefore,
                "the recorder must not call operator delete[]");
  ok &= Require(emitted > 0u && watched == 0u && session == 9999u && mapEpoch == 8888u,
                "the measured window must have exercised the whole state machine");
  ok &= Require(sizeof(PaletteObjectEvidence) >= 64u * 1024u,
                "the fixed watch table must be at least the frozen 64 KiB budget");
  std::printf("    sizeof(PaletteObjectEvidence) = %llu bytes (fixed table, no heap)\n",
              static_cast<unsigned long long>(sizeof(PaletteObjectEvidence)));
  std::printf("    operator new calls during window: %llu (baseline %llu)\n",
              static_cast<unsigned long long>(newAfter),
              static_cast<unsigned long long>(newBefore));
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 12：四个帧域（+ recordFrameSerial）互不覆盖；unknown 不用 0 冒充。
// ---------------------------------------------------------------------------
bool Case12FrameDomains() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(4242u, 4343u);
  ResetCollector();

  const PaletteObjectKey key = MakeKey(0xB0000u);
  // 条目建立帧携带终态结算会沿用的"首次观察到的非渲染帧域"。
  PaletteObjectFrames insertFrames = MakeFrames(1u);
  insertFrames.manifestFrameSerial = 0x200000002ull;
  insertFrames.manifestPublishRevision = 0x300000003ull;
  insertFrames.recordFrameSerial = 0x400000004ull;
  insertFrames.nativeFrameTag = 0x500000005ull;
  insertFrames.manifestUnknown = true;
  insertFrames.nativeUnknown = true;
  rec.NoteReject(key, PaletteObjectRejectReason::R1, insertFrames);
  const uint32_t base = g_stored;

  PaletteObjectFrames unknownFrames;
  unknownFrames.renderFrame = 0x100000001ull;
  unknownFrames.manifestFrameSerial = 0x600000006ull;
  unknownFrames.manifestPublishRevision = 0x700000007ull;
  unknownFrames.recordFrameSerial = 0x800000008ull;
  unknownFrames.nativeFrameTag = 0x900000009ull;
  unknownFrames.manifestUnknown = true;
  unknownFrames.nativeUnknown = true;

  rec.NoteReject(key, PaletteObjectRejectReason::R1, unknownFrames);
  bool ok = true;
  ok &= Require(g_stored - base == 1u, "frame-domain reject must emit one event");
  if (g_stored - base == 1u) {
    ok &= Require(SameFrames(g_collected[base].frames, unknownFrames),
                  "every frame domain (numbers and unknown flags) must round-trip verbatim");
    ok &= Require(g_collected[base].frames.manifestUnknown &&
                      g_collected[base].frames.nativeUnknown,
                  "unknown frame domains must stay flagged, not be written as known zero");
  } else {
    ok &= Require(false, "frame-domain reject must emit one event");
  }

  // 后续阶段的帧域必须来自各自调用点（不得沿用首次拒绝的帧）。
  PaletteObjectFrames served = MakeFrames(0x2100000021ull);
  served.manifestFrameSerial = 0x6ull;
  served.manifestPublishRevision = 0x7ull;
  served.recordFrameSerial = 0x8ull;
  served.nativeFrameTag = 0x9ull;
  served.manifestUnknown = false;
  served.nativeUnknown = false;
  rec.NoteServed(key, PaletteObjectSource::ArenaSlot, 3u, served);
  PaletteObjectFrames enqueued = MakeFrames(0x2200000022ull);
  enqueued.manifestFrameSerial = 0xAull;
  enqueued.manifestPublishRevision = 0xBull;
  enqueued.recordFrameSerial = 0xCull;
  enqueued.nativeFrameTag = 0xDull;
  enqueued.manifestUnknown = false;
  enqueued.nativeUnknown = false;
  rec.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, 0x3ull, enqueued, true);
  PaletteObjectFrames drawn = MakeFrames(0x2300000023ull);
  drawn.manifestFrameSerial = 0xEull;
  drawn.manifestPublishRevision = 0xF00ull;
  drawn.recordFrameSerial = 0x10ull;
  drawn.nativeFrameTag = 0x11ull;
  drawn.manifestUnknown = false;
  drawn.nativeUnknown = false;
  rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x4ull, drawn, false);
  rec.CloseWindow(0x3000000030ull);

  ok &= Require(g_stored - base == 5u, "frame-domain case must emit five events");
  if (g_stored - base == 5u) {
    ok &= Require(SameFrames(g_collected[base + 1u].frames, served),
                  "served frames must match the call site verbatim (including known flags)");
    ok &= Require(SameFrames(g_collected[base + 2u].frames, enqueued),
                  "enqueued frames must match the call site verbatim (including known flags)");
    ok &= Require(g_collected[base + 2u].selectionClearedByNativeOverride,
                  "native override clearing must be recorded, not replaced by an earlier value");
    ok &= Require(SameFrames(g_collected[base + 3u].frames, drawn),
                  "drawn frames must match the call site verbatim (including known flags)");
    ok &= Require(g_collected[base + 4u].frames.renderFrame == 0x3000000030ull,
                  "the terminal uses the window-close frame");
    ok &= Require(g_collected[base + 4u].frames.manifestFrameSerial == 0x200000002ull &&
                      g_collected[base + 4u].frames.manifestPublishRevision == 0x300000003ull &&
                      g_collected[base + 4u].frames.recordFrameSerial == 0x400000004ull &&
                      g_collected[base + 4u].frames.nativeFrameTag == 0x500000005ull,
                  "the terminal keeps the first observed non-render frame domains");
    // 修正 B：终态保留**首次观察到的**帧域口径（该用例的首次帧显式为 unknown），只替换 renderFrame。
    PaletteObjectFrames expectedTerminal = insertFrames;
    expectedTerminal.renderFrame = 0x3000000030ull;
    ok &= Require(SameFrames(g_collected[base + 4u].frames, expectedTerminal),
                  "the window-close terminal must keep the first observed manifest/native "
                  "domains verbatim (numbers and unknown flags)");
  }

  // G2 后半新契约（2026-09-17）：上面的 CloseWindow 已把该窗口**永久关闭**（幂等），
  // 关闭后五个 Note* 入口一律早退 ⇒ 已知零/未知零这一节必须先 Reset 开新窗口。
  rec.Reset(4242u, 4343u);

  // 修正 B（保留调用点口径）：已知零必须保持 known，且不能被写成 unknown。
  const PaletteObjectKey knownKey = MakeKey(0xC0000u);
  rec.NoteReject(knownKey, PaletteObjectRejectReason::R2, MakeFrames(2u)); // 建条目（也发射）
  PaletteObjectFrames knownZero;  // 五个帧域都是 0，但显式标记为已知
  knownZero.manifestUnknown = false;
  knownZero.nativeUnknown = false;
  const uint32_t knownBase = g_stored;
  rec.NoteReject(knownKey, PaletteObjectRejectReason::R2, knownZero);
  const PaletteObjectEventRecord* last = LastEvent();
  ok &= Require(g_stored - knownBase == 1u, "known-zero reject must emit one event");
  ok &= Require(last != nullptr && SameFrames(last->frames, knownZero),
                "known-zero frame domains must round-trip as zero with known flags preserved");
  ok &= Require(last != nullptr && !last->frames.manifestUnknown && !last->frames.nativeUnknown,
                "a call-site known manifest/native frame domain must stay known "
                "(no forced unknown)");
  ok &= Require(last != nullptr && last->rejectReason == PaletteObjectRejectReason::R2,
                "the known-zero event still carries its own reject reason");

  // 同一组数值、但显式标记为未知：必须保持 unknown，且与已知零可分辨。
  const PaletteObjectKey unknownKey = MakeKey(0xC1000u);
  rec.NoteReject(unknownKey, PaletteObjectRejectReason::R2, MakeFrames(3u)); // 建条目（也发射）
  const PaletteObjectFrames unknownZero;  // 五个帧域也是 0，但（默认）标记为未知
  const uint32_t unknownBase = g_stored;
  rec.NoteReject(unknownKey, PaletteObjectRejectReason::R2, unknownZero);
  const PaletteObjectEventRecord* unknownLast = LastEvent();
  ok &= Require(g_stored - unknownBase == 1u, "unknown-zero reject must emit one event");
  ok &= Require(unknownLast != nullptr && SameFrames(unknownLast->frames, unknownZero),
                "unknown-zero frame domains must round-trip verbatim");
  ok &= Require(unknownLast != nullptr && unknownLast->frames.manifestUnknown &&
                    unknownLast->frames.nativeUnknown,
                "a call-site unknown frame domain must stay flagged unknown "
                "(not silently reported as known)");
  ok &= Require(last != nullptr && unknownLast != nullptr &&
                    last->frames.manifestFrameSerial == unknownLast->frames.manifestFrameSerial &&
                    last->frames.manifestPublishRevision ==
                        unknownLast->frames.manifestPublishRevision &&
                    last->frames.recordFrameSerial == unknownLast->frames.recordFrameSerial &&
                    last->frames.nativeFrameTag == unknownLast->frames.nativeFrameTag &&
                    last->frames.manifestUnknown != unknownLast->frames.manifestUnknown &&
                    last->frames.nativeUnknown != unknownLast->frames.nativeUnknown,
                "known-zero and unknown-zero must stay distinguishable "
                "(same numeric domains, different unknown flags)");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 13：探测 / 墓碑 —— 删除前项不得让同一探测链上的后项不可达。
// ---------------------------------------------------------------------------
bool Case13TombstoneProbing() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  bool ok = true;
  PaletteObjectKey front;
  PaletteObjectKey back;
  ok &= Require(FindCollidingKeys(front, back),
                "the case needs two keys hashing to the same watch slot");
  if (!ok)
    return ok;
  ok &= Require(SlotOf(front) == SlotOf(back) && !SameKeyFull(front, back),
                "collision precondition: same hash slot, different keys");

  // 前项占据 start 槽；后项只能被线性探测放到 start+1。
  rec.NoteReject(front, PaletteObjectRejectReason::R1, MakeFrames(10u));
  rec.NoteReject(back, PaletteObjectRejectReason::R1, MakeFrames(11u));
  ok &= Require(rec.watchCount() == 2u, "two colliding keys must occupy two distinct slots");

  // 删除前项 ⇒ 留下墓碑。
  const uint32_t beforeGone = g_stored;
  rec.NoteObjectGone(front);
  ok &= Require(g_stored - beforeGone == 1u, "NoteObjectGone emits one terminal event");
  ok &= Require(rec.watchCount() == 1u, "the front entry is released");

  // 后项必须仍然可达：否则 NoteServed 会在 Find 上返回 nullptr 且不发事件。
  const uint32_t beforeServed = g_stored;
  rec.NoteServed(back, PaletteObjectSource::ArenaSlot, 0x77u, MakeFrames(12u));
  ok &= Require(g_stored - beforeServed == 1u,
                "the back entry must stay reachable across the tombstone left by the front entry");
  if (g_stored - beforeServed == 1u) {
    ok &= Require(SameKeyFull(LastEvent()->key, back), "the served event belongs to the back key");
    ok &= Require(LastEvent()->source == PaletteObjectSource::ArenaSlot &&
                      LastEvent()->hitKey == 0x77u,
                  "the tombstone must not merge the two chains' payloads");
  }

  // 重复插入同键不得产生第二条链。
  const uint32_t watchBefore = rec.watchCount();
  rec.NoteReject(back, PaletteObjectRejectReason::R2, MakeFrames(13u));
  ok &= Require(rec.watchCount() == watchBefore && watchBefore == 1u,
                "re-inserting the same key must not create a second chain");

  // NoteDrawn 对后项同样必须生效（同一 Find 路径）。
  const uint32_t beforeDrawn = g_stored;
  rec.NoteDrawn(back, PaletteObjectSource::DrawTimeCaptured, 0x88u, MakeFrames(14u),
                false);
  ok &= Require(g_stored - beforeDrawn == 1u,
                "NoteDrawn must also reach the entry behind the tombstone");
  if (g_stored - beforeDrawn == 1u) {
    ok &= Require(LastEvent()->stage == PaletteObjectStage::Drawn &&
                      LastEvent()->hitKey == 0x88u,
                  "the drawn event carries the back entry's own payload");
  }
  ok &= Require(CountStageForKey(PaletteObjectStage::ServedCandidate, back) == 1u &&
                    CountStageForKey(PaletteObjectStage::Drawn, back) == 1u,
                "the back key must own exactly one served event and one live drawn event");
  ok &= Require(CountStageForKey(PaletteObjectStage::ServedCandidate, front) == 0u,
                "the released front key must not have produced a served event");

  rec.NoteObjectGone(back);
  ok &= Require(rec.watchCount() == 0u, "the last entry releases with a tombstone");
  ok &= Require(LastEvent() != nullptr &&
                    LastEvent()->terminal == PaletteObjectTerminal::ObjectGone &&
                    LastEvent()->stage == PaletteObjectStage::Rejected &&
                    SameKeyFull(LastEvent()->key, back),
                "the back key closes as its own ObjectGone terminal, not as a merged chain");
  ok &= Require(CountStageForKey(PaletteObjectStage::Drawn, back) == 1u,
                "an ObjectGone terminal must not be charged to the Drawn stage");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 14：窗口关闭后的记录纪律（G2 后半，2026-09-17 主线程）。
//   CloseWindow 置窗口关闭位且**幂等**；五个 Note* 入口在窗口关闭后一律早退
//   （不再发射、不再改条目状态、也不再记账）；只有 Reset 能开新窗口。
// 旧口径"CloseWindow 之后同帧继续记录仍受 64 条上限"在新契约下不可达：记录被整体拒绝，
// 这是比预算记账更强的 fail-closed 保证。每帧预算"按帧重开"的正面见证移到关闭之前。
// ---------------------------------------------------------------------------
bool Case14CloseWindowClosesTheWindow() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  bool ok = true;
  constexpr uint32_t kKeys = 16u; // 16 键 × 4 阶段 = 64 条，正好吃满每帧预算
  for (uint32_t i = 0u; i < kKeys; ++i)
    rec.NoteReject(MakeKey(0x140000u + i), PaletteObjectRejectReason::R1, MakeFrames(1000u));

  const uint32_t storedBeforeFrame = g_stored;
  for (uint32_t i = 0u; i < kKeys; ++i) {
    const PaletteObjectKey key = MakeKey(0x140000u + i);
    const PaletteObjectFrames frames = MakeFrames(1001u);
    rec.NoteReject(key, PaletteObjectRejectReason::R1, frames);
    rec.NoteServed(key, PaletteObjectSource::ArenaSlot, i, frames);
    rec.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, i, frames, false);
    rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, i, frames, false);
  }
  ok &= Require(g_stored - storedBeforeFrame == PaletteObjectEvidence::kPerFrameBudget,
                "the frame must emit exactly the 64-event per-frame budget");
  ok &= Require(rec.counters().droppedPerFrame == 0u,
                "filling the per-frame budget exactly must not drop anything");

  // 对照（仍然有效）：换到新帧后预算必须重新开放（上限是"每帧"而不是"已永久关闭"）。
  const uint32_t storedBeforeNextFrame = g_stored;
  const PaletteObjectKey fresh = MakeKey(0x150000u);
  rec.NoteReject(fresh, PaletteObjectRejectReason::R1, MakeFrames(1002u)); // 建条目 + 发射
  ok &= Require(g_stored - storedBeforeNextFrame == 1u,
                "the per-frame budget must reopen on the next frame");
  ok &= Require(rec.counters().droppedPerFrame == 0u,
                "a successful next-frame event must not add further per-frame drops");

  // 关闭窗口：结算全部 17 条在途条目（本帧预算只用了 1 条 ⇒ 终态全部发得出去）。
  const uint32_t storedBeforeClose = g_stored;
  const uint64_t emittedBeforeClose = rec.counters().emitted;
  rec.CloseWindow(1002u);
  ok &= Require(g_stored - storedBeforeClose == kKeys + 1u,
                "CloseWindow must settle every live entry exactly once");
  ok &= Require(rec.counters().emitted == emittedBeforeClose + kKeys + 1u,
                "terminals are charged to the same total budget");
  ok &= Require(rec.counters().terminalEmitted == kKeys + 1u,
                "every settled entry emits one terminal");
  ok &= Require(rec.counters().closedRecovered == kKeys &&
                    rec.counters().closedWindowExpired == 1u,
                "16 complete chains close as Recovered and the last one as WindowExpired");
  ok &= Require(rec.counters().droppedPerFrame == 0u,
                "terminals within the remaining per-frame budget must not be charged as drops");
  ok &= Require(rec.watchCount() == 0u, "CloseWindow still releases the settled entries");

  // 关闭之后：五个入口 + 重复 CloseWindow 都必须完全不改状态。
  const uint32_t storedAfterClose = g_stored;
  const uint64_t emittedAfterClose = rec.counters().emitted;
  const uint64_t terminalAfterClose = rec.counters().terminalEmitted;
  const uint64_t droppedAfterClose = rec.counters().droppedPerFrame;
  const uint64_t perSessionAfterClose = rec.counters().droppedPerSession;
  const uint64_t duplicateAfterClose = rec.counters().droppedDuplicatePerFrame;
  const uint64_t reserveAfterClose = rec.counters().droppedTerminalReserve;
  const PaletteObjectKey closed = MakeKey(0x160000u);
  const PaletteObjectFrames closedFrames = MakeFrames(1003u);
  rec.NoteReject(closed, PaletteObjectRejectReason::R1, closedFrames);
  rec.NoteServed(closed, PaletteObjectSource::ArenaSlot, 1u, closedFrames);
  rec.NoteEnqueued(closed, PaletteObjectSource::ProducerSnapshot, 2u, closedFrames, false);
  rec.NoteDrawn(closed, PaletteObjectSource::DrawTimeCaptured, 3u, closedFrames, false);
  rec.NoteObjectGone(closed);
  rec.CloseWindow(1004u); // 幂等：窗口只结算一次
  ok &= Require(g_stored == storedAfterClose && rec.counters().emitted == emittedAfterClose,
                "every Note* entry and a repeated CloseWindow must be a no-op after the window "
                "closed");
  ok &= Require(rec.counters().terminalEmitted == terminalAfterClose &&
                    rec.counters().droppedPerFrame == droppedAfterClose &&
                    rec.counters().droppedPerSession == perSessionAfterClose &&
                    rec.counters().droppedDuplicatePerFrame == duplicateAfterClose &&
                    rec.counters().droppedTerminalReserve == reserveAfterClose,
                "a closed window must not emit any terminal nor charge any drop");
  ok &= Require(rec.watchCount() == 0u, "a closed window must not track a new object");

  // 只有 Reset 能开新窗口。
  rec.Reset(1000u, 2000u);
  const uint32_t reopenBefore = g_stored;
  rec.NoteReject(MakeKey(0x170000u), PaletteObjectRejectReason::R1, MakeFrames(2000u));
  ok &= Require(g_stored - reopenBefore == 1u,
                "Reset must open a new window that accepts records again");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 15：先预检、再改状态 —— 被预算拒绝的事件不得修改任何条目状态。
// ---------------------------------------------------------------------------
bool Case15RefusalDoesNotMutateState() {
  bool ok = true;

  { // (a) 被拒的 Served 不得推进 chainSequence / hitCount / saw*。
    PaletteObjectEvidence& rec = Recorder();
    rec.Configure(&Collect, &g_contextMarker);
    rec.Reset(1000u, 2000u);
    ResetCollector();
    const PaletteObjectKey target = MakeKey(0xD00000u + 64u);
    const uint32_t entryBase = g_stored;
    for (uint32_t i = 0u; i < 65u; ++i)
      rec.NoteReject(MakeKey(0xD00000u + i), PaletteObjectRejectReason::R1,
                     MakeFrames(2000u + i)); // 各自独立帧建条目（修正 A：每次都会发射）
    // 修正 A：条目建立那次调用本身发了一条 Rejected 事件并占用 chainSequence 1，
    // 因此后续 Served 的序号必须**相对它**递增；被拒的 Served 不得推进它。
    const PaletteObjectEventRecord* targetEntry = nullptr;
    for (uint32_t i = entryBase; i < g_stored; ++i) {
      if (SameKeyFull(g_collected[i].key, target))
        targetEntry = &g_collected[i];
    }
    ok &= Require(targetEntry != nullptr &&
                      targetEntry->stage == PaletteObjectStage::Rejected &&
                      targetEntry->chainSequence == 1u,
                  "the target entry must emit its first Rejected event with chainSequence 1");
    const uint32_t targetBaseSequence = targetEntry != nullptr ? targetEntry->chainSequence : 0u;
    for (uint32_t i = 0u; i < 64u; ++i)
      rec.NoteServed(MakeKey(0xD00000u + i), PaletteObjectSource::ArenaSlot, i,
                     MakeFrames(3000u)); // 吃满帧 3000 的 64 条预算

    const uint32_t storedFull = g_stored;
    const uint64_t emittedFull = rec.counters().emitted;
    const uint64_t droppedFrameFull = rec.counters().droppedPerFrame;
    rec.NoteServed(target, PaletteObjectSource::ArenaSlot, 0x99u, MakeFrames(3000u));
    ok &= Require(g_stored == storedFull,
                  "a refused ServedCandidate must not reach the emitter");
    ok &= Require(rec.counters().emitted == emittedFull,
                  "a refused event must not be charged to the total");
    ok &= Require(rec.counters().droppedPerFrame == droppedFrameFull + 1u,
                  "a refused event must be counted as droppedPerFrame");

    // 放宽预算（换到新帧）后序号必须从正确的值继续：被拒的那次没有推进任何状态。
    rec.NoteServed(target, PaletteObjectSource::ProducerSnapshot, 0x1234u, MakeFrames(3001u));
    ok &= Require(g_stored == storedFull + 1u,
                  "the entry must still be servable after the refusal");
    if (g_stored == storedFull + 1u) {
      const PaletteObjectEventRecord& firstServe = g_collected[storedFull];
      ok &= Require(firstServe.stage == PaletteObjectStage::ServedCandidate,
                    "the post-refusal event is a ServedCandidate");
      ok &= Require(firstServe.chainSequence == targetBaseSequence + 1u,
                    "a refused event must not advance chainSequence "
                    "(expected entry+1, not entry+2)");
      ok &= Require(firstServe.hitCount == 1u,
                    "hitCount must increment only when the event is really emitted");
      ok &= Require(!firstServe.sawSubmit && !firstServe.sawDraw,
                    "a refused event must not set sawSubmit/sawDraw");
      ok &= Require(firstServe.source == PaletteObjectSource::ProducerSnapshot &&
                        firstServe.hitKey == 0x1234u,
                    "the emitted event must carry the post-refusal call-site payload");
    }
    rec.NoteServed(target, PaletteObjectSource::PoseKernel, 0x5678u, MakeFrames(3002u));
    ok &= Require(g_stored == storedFull + 2u, "the second served event emits");
    if (g_stored == storedFull + 2u) {
      ok &= Require(g_collected[storedFull + 1u].chainSequence == targetBaseSequence + 2u,
                    "chainSequence must continue from the emitted value (entry+1 -> entry+2)");
      ok &= Require(g_collected[storedFull + 1u].hitCount == 2u,
                    "hitCount must follow the emitted ServedCandidate count");
    }
    ok &= Require(CountStageForKey(PaletteObjectStage::ServedCandidate, target) == 2u,
                  "exactly two ServedCandidate events belong to the target key");
  }

  { // (b) 被拒的 Served 不得设置 sawServed / hitCount（用终态判定观察）。
    PaletteObjectEvidence& rec = Recorder();
    rec.Configure(&Collect, &g_contextMarker);
    rec.Reset(1000u, 2000u);
    ResetCollector();
    const PaletteObjectKey target = MakeKey(0xE00000u);
    for (uint32_t i = 0u; i < 64u; ++i)
      rec.NoteReject(MakeKey(0xE00100u + i), PaletteObjectRejectReason::R1,
                     MakeFrames(4000u + i));
    rec.NoteReject(target, PaletteObjectRejectReason::R1, MakeFrames(4100u));
    for (uint32_t i = 0u; i < 64u; ++i)
      rec.NoteServed(MakeKey(0xE00100u + i), PaletteObjectSource::ArenaSlot, i,
                     MakeFrames(5000u)); // 吃满帧 5000

    const uint32_t storedBeforeRefusal = g_stored;
    rec.NoteServed(target, PaletteObjectSource::ProducerSnapshot, 0xAAu, MakeFrames(5000u));
    ok &= Require(g_stored == storedBeforeRefusal,
                  "the refused ServedCandidate must not be emitted");

    // 同帧释放填充条目：终态本身被每帧预算拒，但条目必须被释放（墓碑）。
    for (uint32_t i = 0u; i < 64u; ++i)
      rec.NoteObjectGone(MakeKey(0xE00100u + i));
    ok &= Require(rec.watchCount() == 1u, "only the target entry must remain tracked");

    rec.NoteEnqueued(target, PaletteObjectSource::PoseKernel, 0xBBu, MakeFrames(6000u),
                     false);
    rec.NoteDrawn(target, PaletteObjectSource::DrawTimeCaptured, 0xCCu, MakeFrames(6001u),
                  false);
    const uint32_t storedBeforeClose = g_stored;
    rec.CloseWindow(7000u);

    const PaletteObjectEventRecord* terminal = FindTerminalForKey(target, storedBeforeClose);
    ok &= Require(terminal != nullptr, "the target must still close with a terminal event");
    if (terminal != nullptr) {
      ok &= Require(terminal->terminal == PaletteObjectTerminal::WindowExpired,
                    "a refused ServedCandidate must not set sawServed/hitCount "
                    "(expected WindowExpired, not Unclosed)");
      ok &= Require(terminal->terminal != PaletteObjectTerminal::Recovered,
                    "a refused ServedCandidate must never make the chain look Recovered");
      ok &= Require(terminal->hitCount == 0u,
                    "a refused ServedCandidate must not increment hitCount");
      ok &= Require(terminal->sawSubmit && terminal->sawDraw,
                    "the later enqueue/draw stages must still be recorded");
    }
    ok &= Require(CountStageForKey(PaletteObjectStage::ServedCandidate, target) == 0u,
                  "the target key must have no emitted ServedCandidate event");
  }
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 16：同对象同帧同阶段最多一条事件；不同阶段各自保留。
//   D6（2026-09-17 上级裁定）：四个阶段的同帧去重都**必须先比较载荷** ——
//   每个阶段各有一对正面见证：载荷等价 ⇒ droppedDuplicatePerFrame（按原样，不算损失）；
//   载荷冲突 ⇒ droppedPayloadConflict（同帧不再有第二条事件，但冲突必须可见）。
//   阶段载荷定义：Rejected = lastRejectReason；ServedCandidate = lastSource + lastHitKey；
//   Enqueued = submitSource + submitHitKey + submitSelectionCleared；
//   Drawn = drawSource + drawHitKey + drawSelectionCleared。
// ---------------------------------------------------------------------------
bool Case16SameFrameSameStageAggregation() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  const PaletteObjectKey key = MakeKey(0xF00000u);
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(10u)); // 建条目（Rejected@10）
  const uint32_t base = g_stored;
  uint64_t expectedDuplicate = rec.counters().droppedDuplicatePerFrame;
  uint64_t expectedConflict = rec.counters().droppedPayloadConflict;
  constexpr uint64_t kFrame = 20u;

  bool ok = true;

  // ---------------- 阶段 Rejected（载荷 = lastRejectReason）----------------
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(kFrame)); // 本帧第一条 ⇒ 发射
  ok &= Require(g_stored == base + 1u, "Rejected: the frame's first stage event must be emitted");
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(kFrame)); // 载荷等价 ⇒ 重复
  ok &= Require(g_stored == base + 1u,
                "Rejected: a payload-equivalent second event must be merged");
  ok &= Require(rec.counters().droppedDuplicatePerFrame == ++expectedDuplicate,
                "Rejected: the payload-equivalent merge must be counted as "
                "droppedDuplicatePerFrame");
  rec.NoteReject(key, PaletteObjectRejectReason::R2, MakeFrames(kFrame)); // 载荷冲突（reason）
  ok &= Require(g_stored == base + 1u,
                "Rejected: a payload conflict must not add a second event");
  ok &= Require(rec.counters().droppedPayloadConflict == ++expectedConflict,
                "Rejected: a same-frame reject with a DIFFERENT reason must be counted as "
                "droppedPayloadConflict");
  ok &= Require(g_collected[base].rejectReason == PaletteObjectRejectReason::R1,
                "Rejected: the merged stage event must keep the first payload");

  // ------------- 阶段 ServedCandidate（载荷 = source + hitKey）-------------
  rec.NoteServed(key, PaletteObjectSource::ArenaSlot, 0x11u, MakeFrames(kFrame));
  ok &= Require(g_stored == base + 2u,
                "ServedCandidate: the first stage event must be emitted");
  rec.NoteServed(key, PaletteObjectSource::ArenaSlot, 0x11u, MakeFrames(kFrame)); // 等价
  ok &= Require(g_stored == base + 2u,
                "ServedCandidate: a payload-equivalent second event must be merged");
  ok &= Require(rec.counters().droppedDuplicatePerFrame == ++expectedDuplicate,
                "ServedCandidate: the payload-equivalent merge must be counted as "
                "droppedDuplicatePerFrame");
  rec.NoteServed(key, PaletteObjectSource::PoseKernel, 0x11u, MakeFrames(kFrame)); // 冲突（source）
  ok &= Require(g_stored == base + 2u,
                "ServedCandidate: a payload conflict must not add a second event");
  ok &= Require(rec.counters().droppedPayloadConflict == ++expectedConflict,
                "ServedCandidate: a same-frame serve with a DIFFERENT source must be counted as "
                "droppedPayloadConflict");
  ok &= Require(g_collected[base + 1u].source == PaletteObjectSource::ArenaSlot &&
                    g_collected[base + 1u].hitKey == 0x11u,
                "ServedCandidate: the merged event must keep the first source/hitKey");

  // ---- 阶段 Enqueued（载荷 = submitSource + submitHitKey + submitSelectionCleared）----
  rec.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, 0x33u, MakeFrames(kFrame), false);
  ok &= Require(g_stored == base + 3u,
                "Enqueued: the first stage event must be emitted in the same frame as "
                "ServedCandidate (different stages are kept separately)");
  rec.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, 0x33u, MakeFrames(kFrame), false);
  ok &= Require(g_stored == base + 3u,
                "Enqueued: a payload-equivalent second event must be merged");
  ok &= Require(rec.counters().droppedDuplicatePerFrame == ++expectedDuplicate,
                "Enqueued: the payload-equivalent merge must be counted as "
                "droppedDuplicatePerFrame");
  rec.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, 0x33u, MakeFrames(kFrame), true);
  ok &= Require(g_stored == base + 3u,
                "Enqueued: a payload conflict must not add a second event");
  ok &= Require(rec.counters().droppedPayloadConflict == ++expectedConflict,
                "Enqueued: a same-frame enqueue with a DIFFERENT selectionCleared flag must be "
                "counted as droppedPayloadConflict");
  ok &= Require(g_collected[base + 2u].source == PaletteObjectSource::ProducerSnapshot &&
                    g_collected[base + 2u].hitKey == 0x33u &&
                    !g_collected[base + 2u].selectionClearedByNativeOverride,
                "Enqueued: the merged event must keep the first payload "
                "(source + hitKey + selectionCleared)");

  // ---- 阶段 Drawn（载荷 = drawSource + drawHitKey + drawSelectionCleared）----
  rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x55u, MakeFrames(kFrame), false);
  ok &= Require(g_stored == base + 4u, "Drawn: a Drawn stage is kept in the same frame");
  rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x55u, MakeFrames(kFrame), false);
  ok &= Require(g_stored == base + 4u, "Drawn: a payload-equivalent second event must be merged");
  ok &= Require(rec.counters().droppedDuplicatePerFrame == ++expectedDuplicate,
                "Drawn: the payload-equivalent merge must be counted as "
                "droppedDuplicatePerFrame");
  rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x66u, MakeFrames(kFrame), false);
  ok &= Require(g_stored == base + 4u, "Drawn: a payload conflict must not add a second event");
  ok &= Require(rec.counters().droppedPayloadConflict == ++expectedConflict,
                "Drawn: a same-frame draw with a DIFFERENT hitKey must be counted as "
                "droppedPayloadConflict");
  ok &= Require(g_collected[base + 3u].source == PaletteObjectSource::DrawTimeCaptured &&
                    g_collected[base + 3u].hitKey == 0x55u &&
                    !g_collected[base + 3u].selectionClearedByNativeOverride,
                "Drawn: the merged event must keep the first payload "
                "(source + hitKey + selectionCleared)");

  // 同帧的**四个阶段**各自保留，且都带该帧的帧域。
  uint32_t stagesInFrame = 0u;
  for (uint32_t i = base; i < g_stored; ++i) {
    if (g_collected[i].frames.renderFrame == kFrame &&
        g_collected[i].terminal == PaletteObjectTerminal::None)
      ++stagesInFrame;
  }
  ok &= Require(stagesInFrame == 4u,
                "one frame must keep at most one event per stage (4 distinct stages here)");
  ok &= Require(g_stored - base == 4u,
                "four stages must survive in one frame; no conflict may be compressed into a "
                "second event");
  ok &= Require(rec.counters().droppedPerFrame == 0u &&
                    rec.counters().droppedPerSession == 0u,
                "same-stage merging must not be charged to the per-frame/session budget");
  ok &= Require(rec.counters().droppedDuplicatePerFrame ==
                        expectedDuplicate &&
                    rec.counters().droppedPayloadConflict == expectedConflict,
                "the four equivalent merges and the four conflicts must be exactly the "
                "positive witnesses asserted above");
  std::printf("    D6 four-stage two-way witness: stagesInFrame=4 "
              "droppedDuplicatePerFrame=+4 droppedPayloadConflict=+4\n");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 17：hitCount 必须等于该对象真正发出的 ServedCandidate 事件数（含预算拒绝）。
// ---------------------------------------------------------------------------
bool Case17HitCountMatchesServedEvents() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  bool ok = true;
  const PaletteObjectKey key = MakeKey(0x1100000u);
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(100u)); // 建条目
  for (uint32_t i = 0u; i < 10u; ++i)
    rec.NoteServed(key, PaletteObjectSource::ArenaSlot, i, MakeFrames(101u + i));

  ok &= Require(CountStageForKey(PaletteObjectStage::ServedCandidate, key) == 10u,
                "ten served events must be emitted across ten frames");
  // 每条 Served 事件自带的 hitCount 必须等于它的序号（1..10）。
  uint32_t ordinal = 0u;
  for (uint32_t i = 0u; i < g_stored; ++i) {
    if (g_collected[i].stage != PaletteObjectStage::ServedCandidate ||
        !SameKeyFull(g_collected[i].key, key))
      continue;
    ++ordinal;
    if (g_collected[i].hitCount != ordinal) {
      ok &= Require(false, "every ServedCandidate event must carry its own hitCount ordinal");
      break;
    }
  }
  ok &= Require(ordinal == 10u, "the ordinal walk must visit all ten served events");

  // 预算拒绝场景：吃满一帧的 64 条后，对同一对象再服务一次必须被拒且不改 hitCount。
  for (uint32_t i = 0u; i < 16u; ++i)
    rec.NoteReject(MakeKey(0x1200000u + i), PaletteObjectRejectReason::R1,
                   MakeFrames(150u + i));
  for (uint32_t i = 0u; i < 16u; ++i) {
    const PaletteObjectKey filler = MakeKey(0x1200000u + i);
    const PaletteObjectFrames frames = MakeFrames(200u);
    rec.NoteReject(filler, PaletteObjectRejectReason::R1, frames);
    rec.NoteServed(filler, PaletteObjectSource::ArenaSlot, i, frames);
    rec.NoteEnqueued(filler, PaletteObjectSource::ProducerSnapshot, i, frames, false);
    rec.NoteDrawn(filler, PaletteObjectSource::DrawTimeCaptured, i, frames, false);
  }
  const uint32_t storedBeforeRefusal = g_stored;
  rec.NoteServed(key, PaletteObjectSource::PoseKernel, 0x77u, MakeFrames(200u));
  ok &= Require(g_stored == storedBeforeRefusal,
                "the served event of the target must be refused while the frame is full");
  ok &= Require(CountStageForKey(PaletteObjectStage::ServedCandidate, key) == 10u,
                "a refused ServedCandidate must not be counted as an event");
  ok &= Require(g_collected[storedBeforeRefusal - 1u].stage == PaletteObjectStage::Drawn &&
                    g_collected[storedBeforeRefusal - 1u].hitCount == 1u,
                "the last emitted event before the refusal is the last filler's Drawn event");

  // 释放填充条目（终态被每帧预算拒，但条目被释放），只留目标对象结算终态。
  for (uint32_t i = 0u; i < 16u; ++i)
    rec.NoteObjectGone(MakeKey(0x1200000u + i));
  ok &= Require(rec.watchCount() == 1u, "only the target must remain tracked");
  rec.NoteReject(key, PaletteObjectRejectReason::R2, MakeFrames(399u)); // 新帧重新开放预算
  const uint32_t storedBeforeClose = g_stored;
  rec.CloseWindow(400u);

  const PaletteObjectEventRecord* terminal = FindTerminalForKey(key, storedBeforeClose);
  ok &= Require(terminal != nullptr, "the target must close with a terminal event");
  if (terminal != nullptr) {
    ok &= Require(terminal->hitCount == 10u,
                  "the terminal hitCount must equal the number of ServedCandidate events");
    ok &= Require(terminal->terminal == PaletteObjectTerminal::Unclosed,
                  "a served-but-not-enqueued chain closes as Unclosed");
  }
  ok &= Require(CountStageForKey(PaletteObjectStage::ServedCandidate, key) == 10u,
                "hitCount and the served-event count must agree after the refusal");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 18：Recovered 判定收紧 —— 缺 Served / 缺 Enqueued / 缺 Drawn / 顺序回退 /
//          draw 时 selectionCleared / drawSource==None 全部不得 Recovered。
// ---------------------------------------------------------------------------
struct ScenarioOutcome {
  bool found = false;
  PaletteObjectTerminal terminal = PaletteObjectTerminal::None;
  uint32_t hitCount = 0u;
  uint32_t chainSequence = 0u;
  bool sawSubmit = false;
  bool sawDraw = false;
  PaletteObjectSource source = PaletteObjectSource::None;
  uint64_t hitKey = 0u;
  bool selectionCleared = false;
};

ScenarioOutcome RunRecoveredScenario(uint32_t scenario, const PaletteObjectKey& key) {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(10u)); // 建条目
  switch (scenario) {
  case 0: // 正对照：完整链
    rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 1u, MakeFrames(20u));
    rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 2u, MakeFrames(21u), false);
    rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 3u, MakeFrames(22u), false);
    break;
  case 1: // 缺 Served
    rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 2u, MakeFrames(21u), false);
    rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 3u, MakeFrames(22u), false);
    break;
  case 2: // 缺 Enqueued
    rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 1u, MakeFrames(20u));
    rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 3u, MakeFrames(22u), false);
    break;
  case 3: // 缺 Drawn
    rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 1u, MakeFrames(20u));
    rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 2u, MakeFrames(21u), false);
    break;
  case 4: // 顺序回退：Drawn 之后再来一条 Served
    rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 1u, MakeFrames(20u));
    rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 2u, MakeFrames(21u), false);
    rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 3u, MakeFrames(22u), false);
    rec.NoteServed(key, PaletteObjectSource::PoseKernel, 4u, MakeFrames(23u));
    break;
  case 5: // draw 时 native override 清空了语义 palette
    rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 1u, MakeFrames(20u));
    rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 2u, MakeFrames(21u), false);
    rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 3u, MakeFrames(22u), true);
    break;
  case 6: // drawSource == None（本次 draw 没有携带任何来源）
    rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 1u, MakeFrames(20u));
    rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 2u, MakeFrames(21u), false);
    rec.NoteDrawn(key, PaletteObjectSource::None, 0u, MakeFrames(22u), false);
    break;
  default:
    break;
  }
  rec.CloseWindow(100u);

  ScenarioOutcome out;
  const PaletteObjectEventRecord* terminal = FindTerminalForKey(key, 0u);
  if (terminal != nullptr) {
    out.found = true;
    out.terminal = terminal->terminal;
    out.hitCount = terminal->hitCount;
    out.chainSequence = terminal->chainSequence;
    out.sawSubmit = terminal->sawSubmit;
    out.sawDraw = terminal->sawDraw;
    out.source = terminal->source;
    out.hitKey = terminal->hitKey;
    out.selectionCleared = terminal->selectionClearedByNativeOverride;
  }
  return out;
}

bool Case18RecoveredTightening() {
  bool ok = true;
  // 正对照：完整链必须仍然是 Recovered（收紧不得把合法链一起否掉）。
  const ScenarioOutcome control = RunRecoveredScenario(0u, MakeKey(0x1300000u));
  ok &= Require(control.found && control.terminal == PaletteObjectTerminal::Recovered,
                "control: a fully closed chain must still be Recovered");

  const ScenarioOutcome missingServed = RunRecoveredScenario(1u, MakeKey(0x1310000u));
  ok &= Require(missingServed.found, "counterexample 1 must close with a terminal event");
  ok &= Require(missingServed.terminal != PaletteObjectTerminal::Recovered,
                "counterexample 1 (no Served): must not be Recovered");
  ok &= Require(missingServed.terminal == PaletteObjectTerminal::WindowExpired,
                "counterexample 1 (no Served): must be WindowExpired (never caught)");
  ok &= Require(missingServed.hitCount == 0u,
                "counterexample 1 must not fabricate a hitCount");

  const ScenarioOutcome missingEnqueued = RunRecoveredScenario(2u, MakeKey(0x1320000u));
  ok &= Require(missingEnqueued.found &&
                    missingEnqueued.terminal == PaletteObjectTerminal::Unclosed,
                "counterexample 2 (no Enqueued): must be Unclosed, not Recovered");
  ok &= Require(missingEnqueued.sawSubmit == false,
                "counterexample 2 must not claim a candidate enqueue");

  const ScenarioOutcome missingDrawn = RunRecoveredScenario(3u, MakeKey(0x1330000u));
  ok &= Require(missingDrawn.found && missingDrawn.terminal == PaletteObjectTerminal::Unclosed,
                "counterexample 3 (no Drawn): must be Unclosed, not Recovered");
  ok &= Require(missingDrawn.sawDraw == false, "counterexample 3 must not claim a draw");

  const ScenarioOutcome orderRegression = RunRecoveredScenario(4u, MakeKey(0x1340000u));
  ok &= Require(orderRegression.found,
                "counterexample 4 (Drawn then Served) must close with a terminal event");
  ok &= Require(orderRegression.terminal == PaletteObjectTerminal::Unclosed,
                "counterexample 4 (stage regression): must be Unclosed, not Recovered");
  ok &= Require(orderRegression.terminal != PaletteObjectTerminal::Recovered,
                "counterexample 4 (stage regression): must never be Recovered");

  const ScenarioOutcome clearedAtDraw = RunRecoveredScenario(5u, MakeKey(0x1350000u));
  ok &= Require(clearedAtDraw.found &&
                    clearedAtDraw.terminal == PaletteObjectTerminal::Unclosed,
                "counterexample 5 (selection cleared at draw): must be Unclosed, not Recovered");
  ok &= Require(clearedAtDraw.selectionCleared,
                "counterexample 5 must record that the native override cleared the selection");

  const ScenarioOutcome noneAtDraw = RunRecoveredScenario(6u, MakeKey(0x1360000u));
  ok &= Require(noneAtDraw.found && noneAtDraw.terminal == PaletteObjectTerminal::Unclosed,
                "counterexample 6 (drawSource == None): must be Unclosed, not Recovered");
  ok &= Require(noneAtDraw.source == PaletteObjectSource::None,
                "counterexample 6 must report the None draw source, not the last Served source");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 19：Enqueued / Drawn 必须携带**当时传入的实际来源**（含来源切换）。
// ---------------------------------------------------------------------------
bool Case19ActualSourcePassthrough() {
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  const PaletteObjectKey key = MakeKey(0x1600000u);
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(10u)); // 建条目
  const uint32_t base = g_stored;
  rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 0x111u, MakeFrames(20u));
  rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 0x222u, MakeFrames(21u), false);
  rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x333u, MakeFrames(22u), true);
  rec.CloseWindow(30u);

  bool ok = true;
  ok &= Require(g_stored - base == 4u, "the source-switch chain must emit four events");
  if (g_stored - base == 4u) {
    const PaletteObjectEventRecord& served = g_collected[base + 0u];
    const PaletteObjectEventRecord& enqueued = g_collected[base + 1u];
    const PaletteObjectEventRecord& drawn = g_collected[base + 2u];
    const PaletteObjectEventRecord& terminal = g_collected[base + 3u];

    ok &= Require(served.source == PaletteObjectSource::ProducerSnapshot &&
                      served.hitKey == 0x111u,
                  "Served must carry its own ProducerSnapshot source/hitKey");
    ok &= Require(enqueued.source == PaletteObjectSource::ArenaSlot &&
                      enqueued.hitKey == 0x222u,
                  "Enqueued must carry the enqueue-time ArenaSlot source/hitKey, "
                  "not the most recent Served value");
    ok &= Require(drawn.source == PaletteObjectSource::DrawTimeCaptured &&
                      drawn.hitKey == 0x333u,
                  "Drawn must carry the draw-time source/hitKey");
    ok &= Require(drawn.selectionClearedByNativeOverride,
                  "Drawn must carry the draw-time native-override flag");
    // 终态沿用 draw 时的按值来源（缺失的 draw 来源不得用 Served 的补回）。
    ok &= Require(terminal.source == PaletteObjectSource::DrawTimeCaptured &&
                      terminal.hitKey == 0x333u,
                  "the terminal must report the draw-time source/hitKey");
    ok &= Require(terminal.selectionClearedByNativeOverride,
                  "the terminal must keep the draw-time native-override flag");
    ok &= Require(terminal.terminal == PaletteObjectTerminal::Unclosed,
                  "a draw that cleared the semantic palette must not be Recovered");
  }
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 20：结构性重入 —— 发射器在**持记录器状态锁**的路径内回调，回调里再次进入同一记录器
//          （CloseWindow，即生产侧预冻结钩子 ClosePaletteObjectWindow() 的等价形态）。
//
// 生产链路同构（2026-09-17 子代理报告的结构性风险；主线程已把底层锁改为 std::recursive_mutex）：
//   Note*（已持 StateLock）→ EmitUnchecked → 发射器 → 证据环 Record() → 环容量/窗口冻结
//   → Ring::freeze() → 预冻结钩子 → ClosePaletteObjectWindow() → **同一线程再次取 StateLock**。
// 本用例是该修复的**运行时反向证据**：只证明**这一条重入路径**在递归锁下能走通且计数自洽，
// 并证明把 m_mutex 改回 std::mutex 会被本用例杀死（不在断言里宣称任何更广的结论）。
//
// 形态 A：发射器**每次**发射都重入 CloseWindow；由一条会发射的 NoteReject 触发（工单要求的最小形态）。
// 形态 B：第 4 次发射（live Drawn）时重入一次 ⇒ 嵌套结算一条**完整链** ⇒ 终态必须是 Recovered。
// 两个形态都在独立工作线程里跑；主线程按超时等待（看门狗），超时即打印失败并终止进程
//（非递归锁会在重入处永久阻塞该工作线程，无法 join，也不能让门禁挂死）。
// ---------------------------------------------------------------------------
constexpr uint64_t kReentryCloseFrame = 777u;
constexpr long long kReentryWatchdogTimeoutMs = 10000;  // 远大于场景本身（微秒级）

std::atomic<bool> g_caseWorkerFinished{false};
uint32_t g_reentryDepth = 0u;
uint32_t g_reentryMaxDepth = 0u;
uint32_t g_reentryCloseCalls = 0u;
int32_t g_reentryCountdown = 0;
uint32_t g_storedDepth[kCollectedCapacity] = {};

// 形态 A 的发射器：**每次**收到事件都同线程重入 CloseWindow。
void ReentrantCloseEmitterEvery(void* context, const PaletteObjectEventRecord& record) {
  const uint32_t depth = ++g_reentryDepth;
  if (depth > g_reentryMaxDepth)
    g_reentryMaxDepth = depth;
  Collect(context, record);
  if (g_stored > 0u)
    g_storedDepth[g_stored - 1u] = depth;
  ++g_reentryCloseCalls;
  Recorder().CloseWindow(kReentryCloseFrame);  // 持锁路径内的同线程重入点
  --g_reentryDepth;
}

// 形态 B 的发射器：第 g_reentryCountdown 次发射（此处为第 4 次 = live Drawn）时重入一次，
// 因此嵌套关闭时该条目已具备 served + submit + draw 的完整证据。
void ReentrantCloseEmitterOnFourth(void* context, const PaletteObjectEventRecord& record) {
  const uint32_t depth = ++g_reentryDepth;
  if (depth > g_reentryMaxDepth)
    g_reentryMaxDepth = depth;
  Collect(context, record);
  if (g_stored > 0u)
    g_storedDepth[g_stored - 1u] = depth;
  if (g_reentryCountdown > 0 && --g_reentryCountdown == 0) {
    ++g_reentryCloseCalls;
    Recorder().CloseWindow(kReentryCloseFrame);  // 持锁路径内的同线程重入点
  }
  --g_reentryDepth;
}

struct ReentryEventSummary {
  uint32_t stage = 0u;
  uint32_t terminal = 0u;
  uint32_t chainSequence = 0u;
  uint32_t hitCount = 0u;
  uint64_t deltaFrames = 0u;
  uint64_t source = 0u;
  uint64_t hitKey = 0u;
  bool sawSubmit = false;
  bool sawDraw = false;
  bool selectionCleared = false;
  uint32_t storeDepth = 0u;  // 收集该事件时所处的发射嵌套深度（2 ⇒ 嵌套发射）
};

struct ReentryScenarioOutcome {
  uint64_t emitted = 0u;
  uint64_t terminalEmitted = 0u;
  uint64_t closedRecovered = 0u;
  uint64_t closedWindowExpired = 0u;
  uint64_t closedUnclosed = 0u;
  uint64_t closedOther = 0u;
  uint64_t droppedTotal = 0u;
  uint64_t ringEvicted = 0u;
  uint32_t watch = 0u;
  uint32_t stored = 0u;
  uint64_t emitCalls = 0u;
  uint32_t reentryCloseCalls = 0u;
  uint32_t maxDepth = 0u;
  bool collectorOverflow = false;
  int32_t terminalIndex = -1;
  ReentryEventSummary first{};
  ReentryEventSummary terminal{};
};

ReentryEventSummary SummarizeReentryEvent(uint32_t index) {
  ReentryEventSummary summary;
  if (index >= g_stored)
    return summary;
  const PaletteObjectEventRecord& record = g_collected[index];
  summary.stage = static_cast<uint32_t>(record.stage);
  summary.terminal = static_cast<uint32_t>(record.terminal);
  summary.chainSequence = record.chainSequence;
  summary.hitCount = record.hitCount;
  summary.deltaFrames = record.deltaFrames;
  summary.source = static_cast<uint64_t>(record.source);
  summary.hitKey = record.hitKey;
  summary.sawSubmit = record.sawSubmit;
  summary.sawDraw = record.sawDraw;
  summary.selectionCleared = record.selectionClearedByNativeOverride;
  summary.storeDepth = g_storedDepth[index];
  return summary;
}

void CaptureReentryOutcome(PaletteObjectEvidence& rec, ReentryScenarioOutcome* out) {
  const Counters counters = rec.counters();
  out->emitted = counters.emitted;
  out->terminalEmitted = counters.terminalEmitted;
  out->closedRecovered = counters.closedRecovered;
  out->closedWindowExpired = counters.closedWindowExpired;
  out->closedUnclosed = counters.closedUnclosed;
  out->closedOther =
      counters.closedObjectGone + counters.closedTableFull + counters.closedEventLost;
  out->droppedTotal = counters.droppedPerFrame + counters.droppedPerSession +
                      counters.droppedTableFull + counters.droppedProbeLimit +
                      counters.droppedDuplicatePerFrame + counters.droppedTerminalReserve;
  out->ringEvicted = counters.ringEvictedAfterRecord;
  out->watch = rec.watchCount();
  out->stored = g_stored;
  out->emitCalls = g_emitCalls;
  out->reentryCloseCalls = g_reentryCloseCalls;
  out->maxDepth = g_reentryMaxDepth;
  out->collectorOverflow = g_collectorOverflow != 0u;
  out->first = SummarizeReentryEvent(0u);
  out->terminalIndex = -1;
  for (uint32_t i = 0u; i < g_stored; ++i) {
    if (g_collected[i].terminal != PaletteObjectTerminal::None) {
      out->terminalIndex = static_cast<int32_t>(i);
      break;
    }
  }
  if (out->terminalIndex >= 0)
    out->terminal = SummarizeReentryEvent(static_cast<uint32_t>(out->terminalIndex));
}

void ResetReentryBookkeeping() {
  ResetCollector();
  for (uint32_t i = 0u; i < kCollectedCapacity; ++i)
    g_storedDepth[i] = 0u;
  g_reentryDepth = 0u;
  g_reentryMaxDepth = 0u;
  g_reentryCloseCalls = 0u;
  g_reentryCountdown = 0;
}

// 形态 A：一条会发射的 NoteReject ⇒ 建条目发射第一条 Rejected（持锁）⇒ 发射器里重入 CloseWindow。
void RunReentryScenarioA(ReentryScenarioOutcome* out) {
  PaletteObjectEvidence& rec = Recorder();
  ResetReentryBookkeeping();
  rec.Configure(&ReentrantCloseEmitterEvery, &g_contextMarker);
  rec.Reset(0x20260917u, 0x20260918u);
  const PaletteObjectKey key = MakeKey(0x1A00000u);
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(10u));
  CaptureReentryOutcome(rec, out);
}

// 形态 B：完整链（Rejected → Served → Enqueued → Drawn）在第 4 次发射处嵌套关窗。
void RunReentryScenarioB(ReentryScenarioOutcome* out) {
  PaletteObjectEvidence& rec = Recorder();
  ResetReentryBookkeeping();
  g_reentryCountdown = 4;
  rec.Configure(&ReentrantCloseEmitterOnFourth, &g_contextMarker);
  rec.Reset(0x20260917u, 0x20260918u);
  const PaletteObjectKey key = MakeKey(0x1B00000u);
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(10u));
  rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 0x501u, MakeFrames(11u));
  rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 0x502u, MakeFrames(12u), false);
  rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x503u, MakeFrames(13u), false);
  // 窗口已在重入里关闭 ⇒ 以下两个观测必须早退（不得再产生事件、不得改变任何计数）。
  rec.NoteReject(key, PaletteObjectRejectReason::R2, MakeFrames(14u));
  rec.NoteServed(key, PaletteObjectSource::PoseKernel, 0x504u, MakeFrames(15u));
  CaptureReentryOutcome(rec, out);
}

struct ReentryWorkerOutput {
  ReentryScenarioOutcome a{};
  ReentryScenarioOutcome b{};
};

ReentryWorkerOutput& ReentryOutput() {
  static ReentryWorkerOutput instance;
  return instance;
}

void RunReentryWorker(void* argument) {
  ReentryWorkerOutput* out = static_cast<ReentryWorkerOutput*>(argument);
  RunReentryScenarioA(&out->a);
  RunReentryScenarioB(&out->b);
  g_caseWorkerFinished.store(true, std::memory_order_release);
}

// 通用看门狗：在独立工作线程上跑一个重入场景，主线程按超时等待；超时即打印失败并终止进程。
// 非递归锁会在重入处永久阻塞工作线程（无法 join），因此超时只能判失败并终止进程 —— 这正是
// "变异必须被杀死"的判据（非零退出 + 明确打印），而不是让整个门禁挂死。
bool RunCaseUnderWatchdog(const char* label, void (*body)(void*), void* argument,
                          long long timeoutMs) {
  g_caseWorkerFinished.store(false, std::memory_order_release);
  const auto started = std::chrono::steady_clock::now();
  std::thread worker(body, argument);
  while (!g_caseWorkerFinished.load(std::memory_order_acquire)) {
    const long long elapsedMs = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    if (elapsedMs >= timeoutMs) {
      std::printf("    FAIL: watchdog fired after %lld ms (limit %lld ms) in [%s]: the emitter "
                  "callback re-entered the recorder while this same thread already held its state "
                  "lock and never returned -- non-recursive state lock => self-deadlock "
                  "(e.g. m_mutex reverted to std::mutex).\n", elapsedMs, timeoutMs, label);
      std::printf("[FAIL] %s (watchdog timeout %lld ms)\n", label, timeoutMs);
      std::fflush(stdout);
      std::_Exit(2);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  worker.join();
  const long long elapsedMs = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started).count());
  std::printf("    %s watchdog: finished in %lld ms (limit %lld ms)\n", label, elapsedMs, timeoutMs);
  return true;
}

bool Case20ReentrantCloseFromEmitter() {
  bool ok = true;
  ReentryWorkerOutput& out = ReentryOutput();
  out = ReentryWorkerOutput{};
  RunCaseUnderWatchdog("20 emitter-callback reentry into CloseWindow must not deadlock",
                       &RunReentryWorker, &out, kReentryWatchdogTimeoutMs);
  // 恢复普通收集器，避免影响其后的观测打印。
  Recorder().Configure(&Collect, &g_contextMarker);

  // ---- 形态 A：每次发射都重入（工单要求的最小形态）----
  ok &= Require(out.a.emitted == 2u && out.a.stored == 2u,
                "20A: one emitting NoteReject plus its nested terminal must be exactly two events");
  ok &= Require(out.a.emitCalls == out.a.emitted && out.a.stored == out.a.emitCalls,
                "20A: counters emitted must equal the number of events actually delivered "
                "(no silently swallowed path)");
  ok &= Require(!out.a.collectorOverflow, "20A: the event collector must not overflow");
  ok &= Require(out.a.reentryCloseCalls >= 2u && out.a.maxDepth == 2u,
                "20A: the emitter callback really re-entered the recorder (CloseWindow) while the "
                "state lock was already held on the same thread");
  ok &= Require(out.a.first.stage == static_cast<uint32_t>(PaletteObjectStage::Rejected) &&
                    out.a.first.chainSequence == 1u,
                "20A: the triggering NoteReject must emit the entry-creation Rejected event first");
  ok &= Require(out.a.terminalIndex == 1 &&
                    out.a.terminal.terminal ==
                        static_cast<uint32_t>(PaletteObjectTerminal::WindowExpired),
                "20A: the re-entrant CloseWindow must settle a visible terminal (WindowExpired)");
  ok &= Require(out.a.terminal.storeDepth == 2u,
                "20A: the terminal must be emitted from inside the emitter callback (nested emission)");
  ok &= Require(out.a.terminalEmitted == 1u && out.a.closedWindowExpired == 1u,
                "20A: exactly one terminal must be emitted and counted in closedWindowExpired");
  ok &= Require(out.a.watch == 0u && out.a.closedRecovered == 0u && out.a.closedUnclosed == 0u &&
                    out.a.closedOther == 0u,
                "20A: the re-entrant close must empty the watch table and touch no other bucket");
  ok &= Require(out.a.droppedTotal == 0u && out.a.ringEvicted == 0u,
                "20A: the re-entrant path must not silently drop events or ring records");
  ok &= Require(out.a.terminal.deltaFrames == kReentryCloseFrame - 10u,
                "20A: the nested terminal must carry renderFrame-firstRejectFrame");

  // ---- 形态 B：第 4 次发射（live Drawn）处重入 ⇒ 嵌套结算完整链 ----
  ok &= Require(out.b.emitted == 5u && out.b.stored == 5u && out.b.emitCalls == 5u,
                "20B: four live stage events plus the nested terminal must be exactly five events");
  ok &= Require(out.b.reentryCloseCalls == 1u && out.b.maxDepth == 2u &&
                    out.b.terminal.storeDepth == 2u,
                "20B: the re-entry must happen exactly once, on the live Drawn event, and the "
                "terminal must be emitted while that outer emission is still on the stack");
  ok &= Require(out.b.terminalIndex == 4 &&
                    out.b.terminal.terminal == static_cast<uint32_t>(PaletteObjectTerminal::Recovered),
                "20B: the nested close must settle a fully closed chain as Recovered");
  ok &= Require(out.b.terminal.stage == static_cast<uint32_t>(PaletteObjectStage::Drawn) &&
                    out.b.terminal.chainSequence == 5u,
                "20B: the nested terminal must be the chain's fifth event");
  ok &= Require(out.b.terminal.sawSubmit && out.b.terminal.sawDraw &&
                    !out.b.terminal.selectionCleared,
                "20B: the nested terminal must keep the submit/draw evidence of the settled chain");
  ok &= Require(out.b.terminal.source ==
                        static_cast<uint64_t>(PaletteObjectSource::DrawTimeCaptured) &&
                    out.b.terminal.hitKey == 0x503u && out.b.terminal.hitCount == 1u,
                "20B: the nested terminal must carry the draw-time by-value source/hitKey and the "
                "Served hitCount");
  ok &= Require(out.b.terminalEmitted == 1u && out.b.closedRecovered == 1u && out.b.watch == 0u,
                "20B: exactly one terminal must be emitted on the Recovered bucket and the watch "
                "table must be emptied");
  ok &= Require(out.b.closedWindowExpired == 0u && out.b.closedUnclosed == 0u &&
                    out.b.closedOther == 0u,
                "20B: no other terminal bucket may be touched by the nested close");
  ok &= Require(out.b.droppedTotal == 0u && out.b.ringEvicted == 0u && !out.b.collectorOverflow,
                "20B: the nested close must be loss-free and fully delivered");
  ok &= Require(out.b.terminal.deltaFrames == kReentryCloseFrame - 10u,
                "20B: the nested terminal must carry renderFrame-firstRejectFrame");
  ok &= Require(out.b.first.stage == static_cast<uint32_t>(PaletteObjectStage::Rejected) &&
                    out.b.first.storeDepth == 1u,
                "20B: the outer chain must start at the top-level emission depth");

  std::printf("    20A every-emit re-entry: emitted=%llu stored=%llu emitCalls=%llu maxEmitDepth=%u "
              "reentrantCloseCalls=%u terminal=%u atDepth=%u\n",
              static_cast<unsigned long long>(out.a.emitted),
              static_cast<unsigned long long>(out.a.stored),
              static_cast<unsigned long long>(out.a.emitCalls),
              static_cast<unsigned>(out.a.maxDepth),
              static_cast<unsigned>(out.a.reentryCloseCalls),
              static_cast<unsigned>(out.a.terminal.terminal),
              static_cast<unsigned>(out.a.terminal.storeDepth));
  std::printf("    20B fourth-emit re-entry: emitted=%llu stored=%llu emitCalls=%llu maxEmitDepth=%u "
              "reentrantCloseCalls=%u terminal=%u chainSeq=%u hitCount=%u deltaFrames=%llu "
              "sawSubmit=%s sawDraw=%s\n",
              static_cast<unsigned long long>(out.b.emitted),
              static_cast<unsigned long long>(out.b.stored),
              static_cast<unsigned long long>(out.b.emitCalls),
              static_cast<unsigned>(out.b.maxDepth),
              static_cast<unsigned>(out.b.reentryCloseCalls),
              static_cast<unsigned>(out.b.terminal.terminal),
              static_cast<unsigned>(out.b.terminal.chainSequence),
              static_cast<unsigned>(out.b.terminal.hitCount),
              static_cast<unsigned long long>(out.b.terminal.deltaFrames),
              out.b.terminal.sawSubmit ? "true" : "false",
              out.b.terminal.sawDraw ? "true" : "false");
  std::printf("    => the emitter callback re-entered the recorder while its state lock was already "
              "held on the same thread; the nested settlement is visible, emitted equals the number "
              "of delivered events and no drop counter moved. This proves only this re-entry path "
              "(under std::recursive_mutex), not any broader lock-freedom claim.\n");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 21：**容量冻结**（Ring::freeze(Reason::Capacity)）恰好发生在一条 palette 观测的发射路径内。
//
// 这正是 2026-09-17 子代理报告的结构性风险的**真实触发条件**：
//   arm(capacity=256) → trigger(postPresents=100) → post 区恰好被喂满
//   → 下一条 palette 观测的 Note* 持 StateLock 发射 → 发射器 append() 的 index 溢出
//   → Ring::freeze(Reason::Capacity) → 预冻结钩子 → ClosePaletteObjectWindow() → 同线程 CloseWindow。
// 本用例用**真实生产 Ring**（war3_frame_evidence_core.h，未做任何复制/改写）与真实记录器驱动该路径：
//   (a) 不挂死（与案例 20 同一个看门狗）；
//   (b) 环确实因 Capacity 冻结、预冻结钩子恰好跑一次（m_preFreezeRunning 重入保护生效）；
//   (c) 冻结发生在**发射回调内**：post 区溢出由 palette 观测触发，而不是由 Present 触发；
//   (d) **失败可见**：本次容量收不下这些事件 ⇒ 触发事件与两条终态都进不了环，该丢失必须计入
//       ringEvictedAfterRecord，不得被静默吞掉（终态在记录器计数里仍然结算，故仍是 fail-visible）；
//   (e) 记录器记账自洽：emitted == 实际送达环的事件数 + ringEvictedAfterRecord，且无 dropped*。
// 边界说明：本用例**不链接** wire 发射器 / 导出 / 解析器（也不需要子门 env），因此它证明的是
// "环容量冻结 → 预冻结钩子 → 同线程重入记录器"这条 CPU 路径的机制与可见性；wire 与导出契约
// 由生产往返测试 A..F 负责。
// ---------------------------------------------------------------------------
using dxvk::war3::tools::evidence::Event;
using dxvk::war3::tools::evidence::Key;
using dxvk::war3::tools::evidence::Kind;
using dxvk::war3::tools::evidence::Reason;
using dxvk::war3::tools::evidence::Ring;
using dxvk::war3::tools::evidence::State;

constexpr uint64_t kCapacityFreezeSession = 0x20260917ull;
constexpr uint32_t kCapacityFreezeCapacity = 256u;
constexpr uint32_t kCapacityFreezePostPresents = 100u;
// capacity=256 ⇒ m_pre=192、post 槽位 = capacity/4 = 64；一个 Present 跨度 = PresentBegin + PresentEnd。
constexpr uint32_t kCapacityFreezePostSlots = kCapacityFreezeCapacity / 4u;
constexpr uint32_t kCapacityFreezePostPresentsFed = kCapacityFreezePostSlots / 2u;
constexpr uint64_t kCapacityFreezeCloseFrame = 888u;

Ring& CapacityFreezeRing() {
  static Ring instance;
  return instance;
}

uint32_t g_capacityFreezeHookRuns = 0u;
uint32_t g_capacityFreezeHookEmitDepth = 0u;
uint32_t g_capacityFreezeEmitDepth = 0u;
uint32_t g_capacityFreezeAppendAttempts = 0u;
uint32_t g_capacityFreezeAppendAccepted = 0u;

// 预冻结钩子：与生产侧 PaletteObjectPreFreezeHook() 同构（先结算终态，再由环置 Frozen）。
void CapacityFreezePreFreezeHook() noexcept {
  ++g_capacityFreezeHookRuns;
  g_capacityFreezeHookEmitDepth = g_capacityFreezeEmitDepth;  // 1 ⇒ 冻结时确实有一条观测在飞
  Recorder().CloseWindow(kCapacityFreezeCloseFrame);
}

// 发射器：与生产 EmitPaletteObjectEvent() 的 append/NoteRingEviction 记账同构。
void CapacityFreezeEmitter(void* context, const PaletteObjectEventRecord& record) {
  Collect(context, record);  // 测试内仪器：把记录器发出的事件留证（不参与记录器状态）
  ++g_capacityFreezeEmitDepth;
  ++g_capacityFreezeAppendAttempts;
  Event event{};
  event.kind = Kind::ShadowState;  // 生产侧 palette 事件为 ShadowState + label "palette-object/v1"
  event.label[0] = 'p';
  event.data[0] = 1u;
  const uint64_t sequence = CapacityFreezeRing().append(kCapacityFreezeSession, event);
  if (sequence == 0u)
    Recorder().NoteRingEviction(1u);  // fail-visible：环没收下这条（同生产发射器）
  else
    ++g_capacityFreezeAppendAccepted;
  --g_capacityFreezeEmitDepth;
}

void CapacityFreezeFeedPresent(uint64_t frame) {
  Event begin{};
  begin.kind = Kind::PresentBegin;
  begin.key = Key{0u, frame, 0u, 0u};
  CapacityFreezeRing().append(kCapacityFreezeSession, begin);
  Event end{};
  end.kind = Kind::PresentEnd;
  end.key = Key{0u, frame, 0u, 0u};
  CapacityFreezeRing().append(kCapacityFreezeSession, end);
}

struct CapacityFreezeOutcome {
  bool armed = false;
  bool triggered = false;
  uint32_t hookRuns = 0u;
  uint32_t hookEmitDepth = 0u;
  uint32_t appendAttempts = 0u;
  uint32_t appendAccepted = 0u;
  uint32_t stored = 0u;
  uint32_t watch = 0u;
  uint64_t emitted = 0u;
  uint64_t emitCalls = 0u;
  uint64_t terminalEmitted = 0u;
  uint64_t closedRecovered = 0u;
  uint64_t closedWindowExpired = 0u;
  uint64_t droppedTotal = 0u;
  uint64_t ringEvicted = 0u;
  uint32_t ringState = 0u;
  uint32_t ringReason = 0u;
  uint64_t ringAccepted = 0u;
  uint32_t ringCapacity = 0u;
  uint32_t ringPostRemaining = 0u;
  bool recoveredTerminalVisible = false;
  bool expiredTerminalVisible = false;
  uint64_t recoveredTerminalDelta = 0u;
};

CapacityFreezeOutcome& CapacityFreezeOutput() {
  static CapacityFreezeOutcome instance;
  return instance;
}

void RunCapacityFreezeWorker(void* argument) {
  CapacityFreezeOutcome* out = static_cast<CapacityFreezeOutcome*>(argument);
  Ring& ring = CapacityFreezeRing();
  PaletteObjectEvidence& rec = Recorder();
  g_capacityFreezeHookRuns = 0u;
  g_capacityFreezeHookEmitDepth = 0u;
  g_capacityFreezeEmitDepth = 0u;
  g_capacityFreezeAppendAttempts = 0u;
  g_capacityFreezeAppendAccepted = 0u;
  ResetCollector();

  // 1) 真实生产 arm 入口（等价 Control(action=arm, capacity=256)）+ 安装预冻结钩子（arm 分支同构）。
  out->armed = ring.arm(kCapacityFreezeSession, kCapacityFreezeCapacity);
  ring.setPreFreezeHook(&CapacityFreezePreFreezeHook);
  rec.Configure(&CapacityFreezeEmitter, &g_contextMarker);
  rec.Reset(kCapacityFreezeSession, 0x20260918u);

  // 2) 预热 pre 区（生产 RunAutoFreezeScenario 的 warmup present 同构）。
  CapacityFreezeFeedPresent(1u);

  // 3) pre 区先建一条**完整链**：容量冻结时它的终态必须结算为 Recovered。
  const PaletteObjectKey recoveredKey = MakeKey(0x2100000u);
  rec.NoteReject(recoveredKey, PaletteObjectRejectReason::R1, MakeFrames(10u));
  rec.NoteServed(recoveredKey, PaletteObjectSource::ProducerSnapshot, 0x601u, MakeFrames(11u));
  rec.NoteEnqueued(recoveredKey, PaletteObjectSource::ArenaSlot, 0x602u, MakeFrames(12u), false);
  rec.NoteDrawn(recoveredKey, PaletteObjectSource::DrawTimeCaptured, 0x603u, MakeFrames(13u),
                false);

  // 4) 真实生产 trigger 入口（等价 Control(action=trigger, postPresents=100)）。
  out->triggered = ring.trigger(kCapacityFreezeSession, kCapacityFreezePostPresents);

  // 5) 恰好喂满 post 区（64 条 = 32 个 Present 跨度）⇒ 下一条 post append 必然 index 溢出。
  for (uint32_t i = 0u; i < kCapacityFreezePostPresentsFed; ++i)
    CapacityFreezeFeedPresent(100u + i);

  // 6) 此刻再来一条 palette 观测：持 StateLock → 发射 → append index 溢出
  //    → Ring::freeze(Capacity) → 预冻结钩子 → 同线程再次进入记录器（CloseWindow）。
  const PaletteObjectKey inFlightKey = MakeKey(0x2200000u);
  rec.NoteReject(inFlightKey, PaletteObjectRejectReason::R2, MakeFrames(20u));

  // 7) 读回：环快照（状态/计数）+ 记录器计数 + 收集到的事件。
  const auto snapshot = ring.snapshot(true);
  const Counters counters = rec.counters();
  out->hookRuns = g_capacityFreezeHookRuns;
  out->hookEmitDepth = g_capacityFreezeHookEmitDepth;
  out->appendAttempts = g_capacityFreezeAppendAttempts;
  out->appendAccepted = g_capacityFreezeAppendAccepted;
  out->stored = g_stored;
  out->emitCalls = g_emitCalls;
  out->watch = rec.watchCount();
  out->emitted = counters.emitted;
  out->terminalEmitted = counters.terminalEmitted;
  out->closedRecovered = counters.closedRecovered;
  out->closedWindowExpired = counters.closedWindowExpired;
  out->droppedTotal = counters.droppedPerFrame + counters.droppedPerSession +
                      counters.droppedTableFull + counters.droppedProbeLimit +
                      counters.droppedDuplicatePerFrame + counters.droppedTerminalReserve;
  out->ringEvicted = counters.ringEvictedAfterRecord;
  out->ringState = static_cast<uint32_t>(snapshot.state);
  out->ringReason = static_cast<uint32_t>(snapshot.reason);
  out->ringAccepted = snapshot.accepted;
  out->ringCapacity = snapshot.capacity;
  out->ringPostRemaining = snapshot.postRemaining;
  for (uint32_t i = 0u; i < g_stored; ++i) {
    if (g_collected[i].terminal == PaletteObjectTerminal::Recovered) {
      out->recoveredTerminalVisible = true;
      out->recoveredTerminalDelta = g_collected[i].deltaFrames;
    }
    if (g_collected[i].terminal == PaletteObjectTerminal::WindowExpired)
      out->expiredTerminalVisible = true;
  }
  ring.setPreFreezeHook(nullptr);
  ring.discard(kCapacityFreezeSession);
  g_caseWorkerFinished.store(true, std::memory_order_release);
}

bool Case21CapacityFreezeDuringPaletteEmission() {
  bool ok = true;
  CapacityFreezeOutcome& out = CapacityFreezeOutput();
  out = CapacityFreezeOutcome{};
  RunCaseUnderWatchdog("21 capacity freeze during a palette emission must not deadlock",
                       &RunCapacityFreezeWorker, &out, kReentryWatchdogTimeoutMs);
  Recorder().Configure(&Collect, &g_contextMarker);

  ok &= Require(out.armed && out.triggered,
                "21: the real ring must arm(capacity=256) and accept trigger(postPresents=100)");
  ok &= Require(out.hookRuns == 1u,
                "21: the pre-freeze hook must run exactly once (m_preFreezeRunning reentry guard)");
  ok &= Require(out.hookEmitDepth == 1u,
                "21: the Capacity freeze must happen while a palette emission is in flight (the "
                "overflowing append came from the palette emitter, not from a Present)");
  ok &= Require(out.ringState == static_cast<uint32_t>(State::Frozen) &&
                    out.ringReason == static_cast<uint32_t>(Reason::Capacity),
                "21: the ring must freeze with reason Capacity");
  ok &= Require(out.ringCapacity == kCapacityFreezeCapacity &&
                    out.ringPostRemaining == kCapacityFreezePostPresents - kCapacityFreezePostPresentsFed,
                "21: the ring must still be inside its post window at the overflow (not PostWindow)");
  ok &= Require(out.ringAccepted == 70u,
                "21: only the 2 warmup + 4 pre-region chain + 64 post-present appends may be stored");
  ok &= Require(out.appendAttempts == 7u && out.appendAccepted == 4u,
                "21: seven palette events attempted, only the four pre-region events accepted");
  ok &= Require(out.emitCalls == out.emitted && out.stored == out.emitted,
                "21: every emitted event must be observed by the collector (no swallowed path)");
  ok &= Require(out.emitted == static_cast<uint64_t>(out.appendAttempts) &&
                    out.emitted == static_cast<uint64_t>(out.appendAccepted) + out.ringEvicted,
                "21: emitted must split exactly into ring-accepted + ringEvictedAfterRecord");
  ok &= Require(out.ringEvicted == 3u,
                "21: the failure must stay visible: the triggering event plus both terminals must be "
                "counted in ringEvictedAfterRecord (fail-visible, never silent)");
  ok &= Require(out.terminalEmitted == 2u && out.closedRecovered == 1u &&
                    out.closedWindowExpired == 1u,
                "21: the re-entrant CloseWindow must settle both entries (Recovered + WindowExpired)");
  ok &= Require(out.recoveredTerminalVisible && out.expiredTerminalVisible,
                "21: both nested terminals must be visible to the recorder's event consumer");
  ok &= Require(out.recoveredTerminalDelta == kCapacityFreezeCloseFrame - 10u,
                "21: the nested Recovered terminal must carry renderFrame-firstRejectFrame");
  ok &= Require(out.watch == 0u && out.droppedTotal == 0u,
                "21: the watch table must be emptied and no recorder drop counter may move");

  std::printf("    21 capacity-freeze re-entry: hookRuns=%u hookEmitDepth=%u ringState=%u ringReason=%u "
              "accepted=%llu postRemaining=%u appendAttempts=%u appendAccepted=%u emitted=%llu "
              "ringEvictedAfterRecord=%llu terminalEmitted=%llu closedRecovered=%llu "
              "closedWindowExpired=%llu watch=%u\n",
              static_cast<unsigned>(out.hookRuns), static_cast<unsigned>(out.hookEmitDepth),
              static_cast<unsigned>(out.ringState), static_cast<unsigned>(out.ringReason),
              static_cast<unsigned long long>(out.ringAccepted),
              static_cast<unsigned>(out.ringPostRemaining),
              static_cast<unsigned>(out.appendAttempts), static_cast<unsigned>(out.appendAccepted),
              static_cast<unsigned long long>(out.emitted),
              static_cast<unsigned long long>(out.ringEvicted),
              static_cast<unsigned long long>(out.terminalEmitted),
              static_cast<unsigned long long>(out.closedRecovered),
              static_cast<unsigned long long>(out.closedWindowExpired),
              static_cast<unsigned>(out.watch));
  std::printf("    => the capacity freeze fired inside a palette emission; the nested settlement is "
              "visible in the recorder, and the events the ring could not take are counted in "
              "ringEvictedAfterRecord (3) rather than disappearing silently.\n");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 22（2026-09-17 上级反例的**直接见证**）：同一键同帧的第二次 Drawn 已经
// **清空了语义 palette**（native override：来源不再是 draw-time 捕获、hitKey 不同、
// selectionClearedByNativeOverride = true）。若按"同帧重复"吞掉，导出里就只剩下一条
// "看上去正常"的 draw 记录 —— 这正是上级给出的反例。载荷不等价 ⇒ 必须 droppedPayloadConflict。
// 本案例同时打印实测行（[OBSERVATION] / [FINDING]），供取证与解析器对账。
// ---------------------------------------------------------------------------
bool Case22DrawnPayloadConflictWitness() {
  bool ok = true;
  constexpr uint64_t kConflictFrame = 20u;

  { // (a) 上级反例本体：同帧第一次 D 合法、第二次 D 已清空语义 palette。
    PaletteObjectEvidence& rec = Recorder();
    rec.Configure(&Collect, &g_contextMarker);
    rec.Reset(1000u, 2000u);
    ResetCollector();

    const PaletteObjectKey key = MakeKey(0x1C0000u);
    rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(10u)); // 建条目
    const uint32_t base = g_stored;
    const uint64_t conflictBase = rec.counters().droppedPayloadConflict;
    const uint64_t duplicateBase = rec.counters().droppedDuplicatePerFrame;

    const PaletteObjectFrames frames = MakeFrames(kConflictFrame);
    // 第一次 D：draw 当场仍然携带合法的 draw-time 语义来源。
    rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x5150u, frames, false);
    const uint32_t afterFirstDrawn = g_stored;
    // 第二次 D：native override 已经把语义 palette 清空（记录空值 + 原因，不补回早先值）。
    rec.NoteDrawn(key, PaletteObjectSource::Unknown, 0x1BADu, frames, true);

    ok &= Require(afterFirstDrawn == base + 1u,
                  "the frame's first Drawn (healthy payload) must be emitted");
    ok &= Require(g_stored == afterFirstDrawn,
                  "the second Drawn of the same frame must not add an event");
    ok &= Require(rec.counters().droppedPayloadConflict == conflictBase + 1u,
                  "the same-frame Drawn payload conflict (cleared palette / other hitKey) must be "
                  "counted in droppedPayloadConflict");
    ok &= Require(rec.counters().droppedPayloadConflict >= 1u,
                  "droppedPayloadConflict must be >= 1 for the 上级 counterexample");
    ok &= Require(rec.counters().droppedDuplicatePerFrame == duplicateBase,
                  "a Drawn payload conflict must NOT be charged as a payload-equivalent duplicate");
    // 可见性：第一条事件不得被冲突载荷覆盖 —— 冲突被压缩掉的只是**第二条**。
    ok &= Require(g_collected[base].stage == PaletteObjectStage::Drawn &&
                      g_collected[base].source == PaletteObjectSource::DrawTimeCaptured &&
                      g_collected[base].hitKey == 0x5150u &&
                      !g_collected[base].selectionClearedByNativeOverride,
                  "the emitted Drawn must keep the first (healthy) payload verbatim");
    ok &= Require(rec.counters().droppedPerFrame == 0u &&
                      rec.counters().droppedPerSession == 0u,
                  "a Drawn payload conflict is not a budget drop");
    std::printf("[OBSERVATION] D6 上级反例（同帧 D 载荷冲突）：第一次 D source=DrawTimeCaptured "
                "hitKey=0x5150 cleared=false 已发射；第二次 D source=Unknown hitKey=0x1BAD "
                "cleared=true 未新增事件，droppedPayloadConflict +%llu、"
                "droppedDuplicatePerFrame +%llu（实测 droppedPayloadConflict=%llu）\n",
                static_cast<unsigned long long>(rec.counters().droppedPayloadConflict -
                                                conflictBase),
                static_cast<unsigned long long>(rec.counters().droppedDuplicatePerFrame -
                                                duplicateBase),
                static_cast<unsigned long long>(rec.counters().droppedPayloadConflict));
    std::printf("[FINDING] 同键同帧同阶段的第二次 Drawn 一旦携带不同载荷（来源/hitKey/清空标志），"
                "就**不得**被当成『已证明等价的重复』吞掉：它计入 droppedPayloadConflict，"
                "并阻止该会话被判为覆盖完整（解析器把该计数列为损失）。\n");
  }

  { // (b) 对照：同帧第二次 D 载荷**完全等价** ⇒ 走 droppedDuplicatePerFrame，不产生冲突。
    PaletteObjectEvidence& rec = Recorder();
    rec.Configure(&Collect, &g_contextMarker);
    rec.Reset(1000u, 2000u);
    ResetCollector();

    const PaletteObjectKey key = MakeKey(0x1D0000u);
    rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(10u));
    const uint32_t base = g_stored;
    const uint64_t duplicateBase = rec.counters().droppedDuplicatePerFrame;
    const uint64_t conflictBase = rec.counters().droppedPayloadConflict;
    const PaletteObjectFrames frames = MakeFrames(kConflictFrame);
    rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x5150u, frames, false);
    rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x5150u, frames, false);
    ok &= Require(g_stored == base + 1u,
                  "an equivalent second Drawn must be merged, not emitted");
    ok &= Require(rec.counters().droppedDuplicatePerFrame == duplicateBase + 1u,
                  "an equivalent second Drawn must be counted as droppedDuplicatePerFrame");
    ok &= Require(rec.counters().droppedPayloadConflict == conflictBase,
                  "an equivalent second Drawn must not be charged as a payload conflict");
    std::printf("[OBSERVATION] D6 对照（同帧 D 载荷等价）：第二次 D 载荷逐字段相同 ⇒ "
                "droppedDuplicatePerFrame +1、droppedPayloadConflict +0（不被误判为冲突）\n");
  }
  return ok;
}

// ---------------------------------------------------------------------------
// 取证用总计数打印（在 SUMMARY 之前）：共享记录器的全字段计数快照 + 各阶段两向见证的
// 独立复算。D6 新计数必须逐项可见，不得只打印子集。
// ---------------------------------------------------------------------------
void PrintCounterTotals() {
  const Counters live = Recorder().counters();
  std::printf("[COUNTERS] shared-recorder snapshot before SUMMARY: emitted=%llu "
              "terminalEmitted=%llu droppedPerFrame=%llu droppedPerSession=%llu "
              "droppedTableFull=%llu droppedProbeLimit=%llu droppedDuplicatePerFrame=%llu "
              "droppedPayloadConflict=%llu droppedTerminalReserve=%llu "
              "ringEvictedAfterRecord=%llu closedRecovered=%llu closedWindowExpired=%llu "
              "closedObjectGone=%llu closedTableFull=%llu closedEventLost=%llu "
              "closedUnclosed=%llu weakIdentityRecords=%llu epochUnknownRecords=%llu "
              "watchCount=%u\n",
              static_cast<unsigned long long>(live.emitted),
              static_cast<unsigned long long>(live.terminalEmitted),
              static_cast<unsigned long long>(live.droppedPerFrame),
              static_cast<unsigned long long>(live.droppedPerSession),
              static_cast<unsigned long long>(live.droppedTableFull),
              static_cast<unsigned long long>(live.droppedProbeLimit),
              static_cast<unsigned long long>(live.droppedDuplicatePerFrame),
              static_cast<unsigned long long>(live.droppedPayloadConflict),
              static_cast<unsigned long long>(live.droppedTerminalReserve),
              static_cast<unsigned long long>(live.ringEvictedAfterRecord),
              static_cast<unsigned long long>(live.closedRecovered),
              static_cast<unsigned long long>(live.closedWindowExpired),
              static_cast<unsigned long long>(live.closedObjectGone),
              static_cast<unsigned long long>(live.closedTableFull),
              static_cast<unsigned long long>(live.closedEventLost),
              static_cast<unsigned long long>(live.closedUnclosed),
              static_cast<unsigned long long>(live.weakIdentityRecords),
              static_cast<unsigned long long>(live.epochUnknownRecords),
              static_cast<unsigned>(Recorder().watchCount()));

  // 独立复算：四个阶段各 8 次"载荷等价"与 8 次"载荷冲突"，逐阶段打印两向计数。
  PaletteObjectEvidence& probe = ObservationRecorder();
  probe.Configure(nullptr, nullptr);
  probe.Reset(0xD6u, 0xD6u);
  const PaletteObjectKey key = MakeKey(0x1E0000u);
  probe.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(1u)); // 建条目
  constexpr uint64_t kFrame = 2u;
  probe.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(kFrame)); // 发射
  probe.NoteServed(key, PaletteObjectSource::ArenaSlot, 0x11u, MakeFrames(kFrame));
  probe.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 0x22u, MakeFrames(kFrame), false);
  probe.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x33u, MakeFrames(kFrame), false);
  uint64_t duplicateStage[4] = {};
  uint64_t conflictStage[4] = {};
  for (uint32_t i = 0u; i < 8u; ++i) {
    uint64_t before = probe.counters().droppedDuplicatePerFrame;
    probe.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(kFrame));
    duplicateStage[0] += probe.counters().droppedDuplicatePerFrame - before;
    before = probe.counters().droppedDuplicatePerFrame;
    probe.NoteServed(key, PaletteObjectSource::ArenaSlot, 0x11u, MakeFrames(kFrame));
    duplicateStage[1] += probe.counters().droppedDuplicatePerFrame - before;
    before = probe.counters().droppedDuplicatePerFrame;
    probe.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 0x22u, MakeFrames(kFrame), false);
    duplicateStage[2] += probe.counters().droppedDuplicatePerFrame - before;
    before = probe.counters().droppedDuplicatePerFrame;
    probe.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x33u, MakeFrames(kFrame), false);
    duplicateStage[3] += probe.counters().droppedDuplicatePerFrame - before;

    before = probe.counters().droppedPayloadConflict;
    probe.NoteReject(key, PaletteObjectRejectReason::R2, MakeFrames(kFrame));
    conflictStage[0] += probe.counters().droppedPayloadConflict - before;
    before = probe.counters().droppedPayloadConflict;
    probe.NoteServed(key, PaletteObjectSource::PoseKernel, 0x11u, MakeFrames(kFrame));
    conflictStage[1] += probe.counters().droppedPayloadConflict - before;
    before = probe.counters().droppedPayloadConflict;
    probe.NoteEnqueued(key, PaletteObjectSource::PoseKernel, 0x22u, MakeFrames(kFrame), false);
    conflictStage[2] += probe.counters().droppedPayloadConflict - before;
    before = probe.counters().droppedPayloadConflict;
    probe.NoteDrawn(key, PaletteObjectSource::ArenaSlot, 0x33u, MakeFrames(kFrame), false);
    conflictStage[3] += probe.counters().droppedPayloadConflict - before;
  }
  const Counters probeCounters = probe.counters();
  const char* stages[4] = {"Rejected", "ServedCandidate", "Enqueued", "Drawn"};
  for (uint32_t i = 0u; i < 4u; ++i)
    std::printf("[COUNTERS] D6 two-way per stage %s: payload-equivalent -> "
                "droppedDuplicatePerFrame=%llu ; payload-conflict -> droppedPayloadConflict=%llu\n",
                stages[i], static_cast<unsigned long long>(duplicateStage[i]),
                static_cast<unsigned long long>(conflictStage[i]));
  std::printf("[COUNTERS] D6 independent probe totals: droppedDuplicatePerFrame=%llu "
              "droppedPayloadConflict=%llu (8 equivalent + 8 conflict per stage x 4 stages)\n",
              static_cast<unsigned long long>(probeCounters.droppedDuplicatePerFrame),
              static_cast<unsigned long long>(probeCounters.droppedPayloadConflict));
}

// ---------------------------------------------------------------------------
// 观测（不参与门禁）：lifecycleIdentity 已纳入探测身份；修正 A/B 之后的当场行为。
// ---------------------------------------------------------------------------
void PrintObservations() {
  PaletteObjectKey a = MakeKey(0xD0000u);
  PaletteObjectKey b = a;
  b.lifecycleIdentity = a.lifecycleIdentity + 0x1234u;
  const bool collapsed = PaletteObjectEvidence::SameKey(a, b) &&
                         PaletteObjectEvidence::HashKey(a) == PaletteObjectEvidence::HashKey(b);
  // 现行头把 lifecycleIdentity 算进 SameKey 与 HashKey（八元组全比较）：
  // 只有 lifecycleIdentity 不同的两个键不会被折叠。
  std::printf("[OBSERVATION] SameKey/HashKey cover all eight identity fields "
              "(lifecycleIdentity included): differing-lifecycleIdentity collapsed=%s\n",
              collapsed ? "true" : "false");
  PaletteObjectKey same = a;
  std::printf("[OBSERVATION] identical keys collapse=%s\n",
              (PaletteObjectEvidence::SameKey(a, same) &&
               PaletteObjectEvidence::HashKey(a) == PaletteObjectEvidence::HashKey(same))
                  ? "true"
                  : "false");
  std::printf("[OBSERVATION] manifestUnknown/nativeUnknown are taken from the call site "
              "(fix B): a call-site known frame domain is reported as known, and an unknown one "
              "stays flagged unknown.\n");
  std::printf("[OBSERVATION] sizeof(PaletteObjectEventRecord)=%llu bytes, sizeof(PaletteObjectKey)=%llu\n",
              static_cast<unsigned long long>(sizeof(PaletteObjectEventRecord)),
              static_cast<unsigned long long>(sizeof(PaletteObjectKey)));

  // 修正 A 的正面见证（只观察、不判定；门禁断言在 Case1/Case8）：
  // 建条目自身发射第一条 Rejected；同一帧的第二次 Reject 只累加 droppedDuplicatePerFrame。
  PaletteObjectEvidence& probe = ObservationRecorder();
  probe.Configure(nullptr, nullptr);
  probe.Reset(1u, 2u);
  const PaletteObjectKey fresh = MakeKey(0xF00000u);
  const uint64_t emittedBefore = probe.counters().emitted;
  probe.NoteReject(fresh, PaletteObjectRejectReason::R1, MakeFrames(100u));
  const uint64_t emittedAfterInsert = probe.counters().emitted - emittedBefore;
  const uint64_t duplicateAfterInsert = probe.counters().droppedDuplicatePerFrame;
  probe.NoteReject(fresh, PaletteObjectRejectReason::R1, MakeFrames(100u));
  const uint64_t duplicateAfterSameFrame = probe.counters().droppedDuplicatePerFrame;
  probe.NoteReject(fresh, PaletteObjectRejectReason::R1, MakeFrames(101u));
  const uint64_t emittedAfterNextFrame = probe.counters().emitted - emittedBefore;
  std::printf("[OBSERVATION] fresh NoteReject: emitted=%llu droppedDuplicatePerFrame=%llu "
              "watch=%u; the same-frame second reject adds %llu duplicate drop(s); "
              "a next-frame reject raises emitted to %llu\n",
              static_cast<unsigned long long>(emittedAfterInsert),
              static_cast<unsigned long long>(duplicateAfterInsert),
              static_cast<unsigned>(probe.watchCount()),
              static_cast<unsigned long long>(duplicateAfterSameFrame - duplicateAfterInsert),
              static_cast<unsigned long long>(emittedAfterNextFrame));
  std::printf("[OBSERVATION] => Insert does not pre-set lastRejectFrame (fix A): the object's "
              "FIRST Rejected event is emitted, and only the second reject in the same frame is "
              "deduplicated.\n");

  // [FINDING] 2026-09-17 F1/F2 修复后的**正面见证**（只观察，不改记录器语义）：
  // 单窗口内终态只受 kTerminalReserve(512) 约束、**不再**受每帧 64 上限约束 ⇒ 512 预留可达、
  // 第 513 条起记 droppedTerminalReserve（预留记账分支可达）、4096 总额可达
  // （3584 普通 + 512 终态，Case7(c) 断言）。
  // 历史（修复前、已被 F1 推翻的旧结论）：终态与普通事件共用每帧预算且窗口只结算一次，
  // 曾使单窗口 terminalEmitted <= 64、512 预留与 4096 总额都不可达 —— 该结论已撤回。
  PaletteObjectEvidence& cap = ObservationRecorder();
  cap.Configure(nullptr, nullptr);
  cap.Reset(3u, 4u);
  for (uint32_t i = 0u; i < PaletteObjectEvidence::kWatchCapacity; ++i)
    cap.NoteReject(MakeKey(0xF10000u + i * 0x40u), PaletteObjectRejectReason::R1,
                   MakeFrames(1000u + i / PaletteObjectEvidence::kPerFrameBudget));
  const uint64_t emittedAtFill = cap.counters().emitted;
  // 不存在的键把每帧预算清零（BeginFrame 已推进，Find 返回 nullptr，不发射）：
  // 关闭时每帧预算只剩 0 条，仍能发出 512 条终态，这本身就是 F1 的见证。
  cap.NoteServed(MakeKey(0xFFFF00u), PaletteObjectSource::ArenaSlot, 0u,
                 MakeFrames(1000u + PaletteObjectEvidence::kWatchCapacity /
                                       PaletteObjectEvidence::kPerFrameBudget));
  cap.CloseWindow(2000u);
  std::printf("[FINDING] single-window terminal reserve reachable: watch-entries=%u "
              "emitted-at-fill=%llu emitted-after-close=%llu terminalEmitted=%llu "
              "droppedPerFrame=%llu droppedTerminalReserve=%llu "
              "(kPerFrameBudget=%u kTerminalReserve=%llu kTotalBudget=%llu) => terminals are "
              "bounded only by the 512 reserve (F1), the 513th terminal onward is counted in "
              "droppedTerminalReserve and the 4096 total is reachable as 3584 normal + 512 "
              "terminal; the pre-F1 claim that one window could never reach the 512 reserve nor "
              "the 4096 total is retracted. Reported, not changed.\n",
              static_cast<unsigned>(PaletteObjectEvidence::kWatchCapacity),
              static_cast<unsigned long long>(emittedAtFill),
              static_cast<unsigned long long>(cap.counters().emitted),
              static_cast<unsigned long long>(cap.counters().terminalEmitted),
              static_cast<unsigned long long>(cap.counters().droppedPerFrame),
              static_cast<unsigned long long>(cap.counters().droppedTerminalReserve),
              static_cast<unsigned>(PaletteObjectEvidence::kPerFrameBudget),
              static_cast<unsigned long long>(PaletteObjectEvidence::kTerminalReserve),
              static_cast<unsigned long long>(PaletteObjectEvidence::kTotalBudget));

  // [OBSERVATION] F4（2026-09-17 复查发现的缺陷，**已修复**）：F2 原先只落在 CloseWindow 路径，
  // TableFull / ObjectGone 两条终态路径曾在预算门**之前**递增 closed*（可超过实际终态数）。
  // 生产侧已改为「先 CanEmit(true) 再递增」，因此下面的 closedTableFull 必须恰好等于发出的终态数。
  PaletteObjectEvidence& f4 = ObservationRecorder();
  f4.Configure(nullptr, nullptr);
  f4.Reset(5u, 6u);
  for (uint32_t i = 0u; i < PaletteObjectEvidence::kWatchCapacity; ++i)
    f4.NoteReject(MakeKey(0xE10000u + i * 0x40u), PaletteObjectRejectReason::R1,
                  MakeFrames(1000u + i / PaletteObjectEvidence::kPerFrameBudget));
  const uint64_t f4EmittedAtFill = f4.counters().emitted;
  const uint32_t f4Rounds = static_cast<uint32_t>(PaletteObjectEvidence::kTerminalReserve) + 1u;
  for (uint32_t i = 0u; i < f4Rounds; ++i)
    f4.NoteReject(MakeKey(0xE90000u + i * 0x40u), PaletteObjectRejectReason::R1,
                  MakeFrames(2000u + i));
  std::printf("[OBSERVATION] F4 closed* accounting on the TableFull path: emitted-at-fill=%llu "
              "tableFull-calls=%u terminalEmitted=%llu closedTableFull=%llu "
              "droppedTerminalReserve=%llu watch=%u => closedTableFull == terminalEmitted (F4 fixed "
              "2026-09-17: only emitted terminals are counted on this path too); the pre-fix claim "
              "that refused terminals were still counted is retracted.\n",
              static_cast<unsigned long long>(f4EmittedAtFill),
              static_cast<unsigned>(f4Rounds),
              static_cast<unsigned long long>(f4.counters().terminalEmitted),
              static_cast<unsigned long long>(f4.counters().closedTableFull),
              static_cast<unsigned long long>(f4.counters().droppedTerminalReserve),
              static_cast<unsigned>(f4.watchCount()));
}

// ---------------------------------------------------------------------------
// 案例 23（2026-09-17 上级裁定 ⑦⑧，D2 生产写入侧）：**记录级**分段量 + 载明的身份证明种类。
//   生产写入侧本轮首次真的写 data[3]=windowSegment 与 words32[15]=identityProofKind，因此这里从
//   **记录器出口**（不是源码文本）钉死：
//     (a) Reset() ⇒ 第 1 个窗口：该窗口内**每条**发射（live + 终态）的 windowSegment 都必须是 1；
//     (b) ResetForSessionTransition() ⇒ **递增**（1 → 2 → 3）：清表 ≠ 分段；
//     (c) 生产口径键（MakePaletteObjectKey ⇒ lifecycleIdentity=0 + identityWeak=true +
//         epochUnknown=true）⇒ identityProofKind == NoIdentityProof(0)：生产路径无法认证；
//     (d) 合成认证键（非零实例生命周期身份 + identityWeak=false + epochUnknown=false）⇒
//         identityProofKind == InstanceLifecycleIdentityProof(1)：**认证路径仍可执行**
//         （合成键可达、生产不可达 —— 上级明确要求保留的可测试性）；
//     (e) 三个条件缺一不可：identityWeak=true / epochUnknown=true / 身份值为 0 各自都回到 0。
//   打印 [OBSERVATION]/[FINDING] 实测行（含两个窗口的实际 segment 值与各键的实际 proof kind）。
// ---------------------------------------------------------------------------
bool Case23WindowSegmentAndIdentityProofKind() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);

  rec.Reset(7001u, 8001u);  // 新会话（arm 口径）⇒ 第 1 个窗口
  ResetCollector();

  // (c) 生产口径键：三个身份/代际字段都是"未知 + 弱身份"，载明的证明种类必须恒 0。
  const PaletteObjectKey productionKey = dxvk::war3::tools::evidence::MakePaletteObjectKey(
      reinterpret_cast<void*>(uintptr_t(0x23001u)),
      reinterpret_cast<void*>(uintptr_t(0x23002u)), 0x23u, 0x6831u, 7001u, 8001u);
  rec.NoteReject(productionKey, PaletteObjectRejectReason::R1, MakeFrames(5000u));
  const PaletteObjectEventRecord* productionReject = LastEvent();
  ok &= Require(productionReject != nullptr &&
                    productionReject->identityProofKind ==
                        static_cast<uint32_t>(PaletteObjectIdentityProofKind::NoIdentityProof),
                "a production-shaped key must carry NoIdentityProof(0)");
  ok &= Require(productionReject != nullptr && productionReject->windowSegment == 1u,
                "the first window after Reset() must be windowSegment 1");

  // (a) 同窗口内 live 阶段与终态都必须盖同一个分段（记录级、发出时写一次）。
  rec.NoteServed(productionKey, PaletteObjectSource::ArenaSlot, 0xA1u, MakeFrames(5001u));
  rec.NoteEnqueued(productionKey, PaletteObjectSource::ArenaSlot, 0xB1u, MakeFrames(5002u), false);
  rec.NoteDrawn(productionKey, PaletteObjectSource::DrawTimeCaptured, 0xC1u, MakeFrames(5003u),
                false);
  rec.CloseWindow(5004u);
  const uint32_t windowOneEvents = g_stored;
  uint32_t windowOneWrongSegment = 0u;
  uint32_t windowOneWrongKind = 0u;
  for (uint32_t i = 0u; i < windowOneEvents; ++i) {
    if (g_collected[i].windowSegment != 1u)
      ++windowOneWrongSegment;
    if (g_collected[i].identityProofKind != 0u)
      ++windowOneWrongKind;
  }
  ok &= Require(windowOneEvents == 5u,
                "the first window must emit the four stages plus its settled terminal");
  ok &= Require(windowOneWrongSegment == 0u,
                "every record of the first window (live and terminal) must carry windowSegment 1");
  ok &= Require(windowOneWrongKind == 0u,
                "no production-shaped record may carry a non-zero identity proof kind");

  // (b) 清表 ≠ 分段：ResetForSessionTransition 必须**递增**分段，同时**保留**会话计数。
  const uint64_t emittedBeforeTransition = rec.counters().emitted;
  rec.ResetForSessionTransition(7001u, 8001u);
  const uint32_t windowTwoBase = g_stored;

  // (d) 合成认证键（生产不可达、合成键可达）：三个条件同时成立 ⇒ 载明的证明种类必须为 1。
  PaletteObjectKey synthetic = MakeKey(0x23010u);
  rec.NoteReject(synthetic, PaletteObjectRejectReason::R1, MakeFrames(6000u));
  rec.CloseWindow(6001u);
  const uint32_t windowTwoEvents = g_stored - windowTwoBase;
  ok &= Require(windowTwoEvents == 2u,
                "the transition window must emit its reject plus the settled terminal");
  if (windowTwoEvents == 2u) {
    ok &= Require(g_collected[windowTwoBase].windowSegment == 2u &&
                      g_collected[windowTwoBase + 1u].windowSegment == 2u,
                  "ResetForSessionTransition must open window 2 (incremented, never back to 1)");
    ok &= Require(g_collected[windowTwoBase].identityProofKind ==
                      static_cast<uint32_t>(
                          PaletteObjectIdentityProofKind::InstanceLifecycleIdentityProof) &&
                      g_collected[windowTwoBase + 1u].identityProofKind ==
                          static_cast<uint32_t>(
                              PaletteObjectIdentityProofKind::InstanceLifecycleIdentityProof),
                  "a synthetic certified key must carry InstanceLifecycleIdentityProof(1) "
                  "(including its settled terminal)");
  }
  ok &= Require(rec.counters().emitted > emittedBeforeTransition,
                "ResetForSessionTransition must keep the session counters (only the table clears)");

  // (e) 三个条件缺一不可：只把其中一个条件打破，证明种类必须回到 0。
  rec.ResetForSessionTransition(7001u, 8001u);  // 窗口 3
  const uint32_t windowThreeBase = g_stored;
  PaletteObjectKey weakKey = MakeKey(0x23020u);
  weakKey.identityWeak = true;  // 打破 identityWeak=false
  PaletteObjectKey unknownEpochKey = MakeKey(0x23030u);
  unknownEpochKey.epochUnknown = true;  // 打破 epochUnknown=false
  PaletteObjectKey zeroIdentityKey = MakeKey(0x23040u);
  zeroIdentityKey.lifecycleIdentity = 0u;  // 打破 lifecycleIdentity != 0
  rec.NoteReject(weakKey, PaletteObjectRejectReason::R1, MakeFrames(7000u));
  rec.NoteReject(unknownEpochKey, PaletteObjectRejectReason::R1, MakeFrames(7001u));
  rec.NoteReject(zeroIdentityKey, PaletteObjectRejectReason::R1, MakeFrames(7002u));
  const uint32_t windowThreeEvents = g_stored - windowThreeBase;
  ok &= Require(windowThreeEvents == 3u, "the third window must emit three reject events");
  if (windowThreeEvents == 3u) {
    ok &= Require(g_collected[windowThreeBase].windowSegment == 3u &&
                      g_collected[windowThreeBase + 1u].windowSegment == 3u &&
                      g_collected[windowThreeBase + 2u].windowSegment == 3u,
                  "each further transition must open the next window (3 after two transitions)");
    ok &= Require(g_collected[windowThreeBase].identityProofKind == 0u &&
                      g_collected[windowThreeBase + 1u].identityProofKind == 0u &&
                      g_collected[windowThreeBase + 2u].identityProofKind == 0u,
                  "identityProofKind requires ALL THREE conditions "
                  "(non-zero value && !identityWeak && !epochUnknown)");
  }

  std::printf("[OBSERVATION] D2 window segment / identity proof kind: window1-records=%u "
              "wrong-segment=%u window1-terminal.segment=%llu ; transition -> window2.segment=%llu "
              "(events=%u) ; window3.segment=%llu (events=%u) ; "
              "production-key kind=%u synthetic-certified-key kind=%u weak-flag kind=%u "
              "epoch-unknown kind=%u zero-identity kind=%u\n",
              static_cast<unsigned>(windowOneEvents),
              static_cast<unsigned>(windowOneWrongSegment),
              windowOneEvents == 0u
                  ? 0ull
                  : static_cast<unsigned long long>(g_collected[windowOneEvents - 1u].windowSegment),
              windowTwoEvents == 0u
                  ? 0ull
                  : static_cast<unsigned long long>(g_collected[windowTwoBase].windowSegment),
              static_cast<unsigned>(windowTwoEvents),
              windowThreeEvents == 0u
                  ? 0ull
                  : static_cast<unsigned long long>(g_collected[windowThreeBase].windowSegment),
              static_cast<unsigned>(windowThreeEvents),
              productionReject == nullptr ? 0u : productionReject->identityProofKind,
              windowTwoEvents == 0u ? 0u : g_collected[windowTwoBase].identityProofKind,
              windowThreeEvents < 3u ? 0u : g_collected[windowThreeBase].identityProofKind,
              windowThreeEvents < 3u ? 0u : g_collected[windowThreeBase + 1u].identityProofKind,
              windowThreeEvents < 3u ? 0u : g_collected[windowThreeBase + 2u].identityProofKind);
  std::printf("[FINDING] 2026-09-17 D2 production write side: every emitted record now carries its "
              "own **window segment** (Reset => 1, ResetForSessionTransition => +1, so clearing the "
              "table is no longer conflated with opening a window) and a **carried** identity proof "
              "kind derived only from (lifecycleIdentity != 0 && !identityWeak && !epochUnknown). "
              "The three production capture points write lifecycleIdentity=0 + identityWeak=true + "
              "epochUnknown=true, so production always carries NoIdentityProof(0) and version 2 "
              "refuses to certify a same-object recovery; the synthetic certified key still reaches "
              "InstanceLifecycleIdentityProof(1), i.e. the certified path stays executable in tests. "
              "Reported, not changed.\n");

  rec.CloseWindow(7003u);
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 24（2026-09-18 独立复审批次 3）：**两类链**的最小集成见证。
//   正常观察链：FirstSight -> Enqueued -> CloseWindow
//   拒绝恢复链：Rejected -> ServedCandidate -> Enqueued -> Drawn -> CloseWindow
// 断言要点：
//   · 首见链**确实建立条目并发射事件**（watchCount==1 / firstSightInserted==1）；
//   · 链首是 FirstSight，rejectReason=NotChecked（首见**不是**拒绝），非终态；
//   · 未知身份**保持未知**：identityProofKind 必须为 NoIdentityProof(0)；
// 2026-09-18 P0-4 修正：**本注释此前与断言矛盾**（它写「首见链终态 = WindowExpired」）。
// 裁定已把「仅有链首后关闭的观察链」定为 ObservationClosed（诚实报告缺绘制证据）；
// 下面 :3023 的断言已同步更新 —— 注释与断言必须说同一件事。
//   · 恢复链一字不变：链首 Rejected、终态 Recovered、终态 stage=Drawn（版本 1/2 冻结形状）；
//   · 只跑恢复链的窗口 firstSightUsed()==false ⇒ 写方仍按版本 2 发射。
// ---------------------------------------------------------------------------
bool Case24FirstSightObservationChain() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  // ---------- 正常观察链（对象从未被拒绝） ----------
  // 键取**生产形状**：与三个生产采集点一致（lifecycleIdentity=0 + identityWeak=true +
  // epochUnknown=true）。注意 MakeKey() 造的是已认证键（kind=1），不能用来验证"未知不得升级"。
  PaletteObjectKey normalKey = MakeKey(0x9000u);
  normalKey.lifecycleIdentity = 0u;
  normalKey.identityWeak = true;
  normalKey.epochUnknown = true;
  ok &= Require(rec.watchCount() == 0u, "fresh window starts empty");
  rec.NoteFirstSight(normalKey, MakeFrames(700u));
  ok &= Require(rec.watchCount() == 1u,
                "FirstSight must create the entry (this is the whole point)");
  ok &= Require(rec.counters().firstSightInserted == 1u, "firstSightInserted");
  ok &= Require(rec.counters().firstSightEmitted == 1u, "firstSightEmitted");
  ok &= Require(g_stored == 1u, "FirstSight emits exactly one event");
  const PaletteObjectEventRecord* firstSight = LastEvent();
  if (firstSight != nullptr) {
    ok &= Require(firstSight->stage == PaletteObjectStage::FirstSight,
                  "normal chain head must be FirstSight");
    ok &= Require(firstSight->rejectReason == PaletteObjectRejectReason::NotChecked,
                  "FirstSight must not fabricate a rejection reason");
    ok &= Require(firstSight->terminal == PaletteObjectTerminal::None,
                  "FirstSight is not a terminal");
    ok &= Require(firstSight->identityProofKind == 0u,
                  "unknown identity must stay NoIdentityProof (never upgraded)");
    ok &= Require(firstSight->key.identityWeak,
                  "the weak-identity flag must be carried, not cleaned up");
    ok &= Require(firstSight->key.epochUnknown,
                  "the unknown-epoch flag must be carried, not cleaned up");
    ok &= Require(firstSight->key.lifecycleIdentity == 0u,
                  "FirstSight must not invent a lifecycle identity");
  }
  rec.NoteEnqueued(normalKey, PaletteObjectSource::Unknown, 0u, MakeFrames(701u),
                   false);
  ok &= Require(g_stored == 2u, "Enqueued must follow FirstSight");
  ok &= Require(rec.firstSightUsed(),
                "a window that used FirstSight must ask for version 3");
  rec.CloseWindow(702u);
  const PaletteObjectEventRecord* normalTerminal = LastEvent();
  ok &= Require(normalTerminal != nullptr, "normal chain must settle");
  if (normalTerminal != nullptr) {
    // 2026-09-18 P0-4（Astra 裁定）：**这条旧期望被有意更新**。
    // 旧期望（WindowExpired）是**基于错误的结算顺序**写下的 —— 它让「只走到链首/入队的观察链」
    // 借用**拒绝链**的含义「窗口过期」。裁定明确：「仅有链首后关闭，也应使用 ObservationClosed，
    // 同时诚实报告没有完整绘制证据」。这不是放宽判据：新期望更强，它要求终态**只**表达
    // 「观察结算」，既不得冒充 Recovered，也不得冒充拒绝链的 WindowExpired。
    ok &= Require(normalTerminal->terminal == PaletteObjectTerminal::ObservationClosed,
                  "an observation chain that closed without full draw evidence must settle as "
                  "ObservationClosed - never Recovered, and never the rejection-chain meaning "
                  "WindowExpired (updated per the 2026-09-18 ruling; the old expectation was "
                  "written on the wrong settlement order)");
    ok &= Require(normalTerminal->stage == PaletteObjectStage::Enqueued,
                  "terminal must not claim stage=Drawn for a FirstSight chain");
  }
  ok &= Require(rec.counters().closedRecovered == 0u,
                "a normal chain must NEVER be reported as Recovered");

  // ---------- 正向对照：首见**不得降级**一个真正已认证的键 ----------
  rec.Reset(2500u, 2600u);
  ResetCollector();
  const PaletteObjectKey certifiedKey = MakeKey(0x9500u);
  rec.NoteFirstSight(certifiedKey, MakeFrames(750u));
  const PaletteObjectEventRecord* certifiedSight = LastEvent();
  ok &= Require(certifiedSight != nullptr, "certified key must also get a FirstSight");
  if (certifiedSight != nullptr) {
    ok &= Require(certifiedSight->identityProofKind == 1u,
                  "a genuinely certified key keeps InstanceLifecycleIdentityProof");
  }

  // ---------- 拒绝恢复链（版本 1/2 冻结形状必须一字不变） ----------
  rec.Reset(3000u, 4000u);
  ResetCollector();
  const PaletteObjectKey recoveryKey = MakeKey(0x9100u);
  TrackObject(recoveryKey, 800u, PaletteObjectRejectReason::R1);
  ok &= Require(rec.counters().firstSightEmitted == 0u,
                "recovery chain must not emit FirstSight");
  rec.NoteServed(recoveryKey, PaletteObjectSource::OwnedPartSnapshot, 0x77u,
                 MakeFrames(801u));
  rec.NoteEnqueued(recoveryKey, PaletteObjectSource::OwnedPartSnapshot, 0x78u,
                   MakeFrames(802u), false);
  rec.NoteDrawn(recoveryKey, PaletteObjectSource::DrawTimeCaptured, 0x79u,
                MakeFrames(803u), false);
  rec.CloseWindow(804u);
  const PaletteObjectEventRecord* recoveryTerminal = LastEvent();
  ok &= Require(recoveryTerminal != nullptr, "recovery chain must settle");
  if (recoveryTerminal != nullptr) {
    ok &= Require(recoveryTerminal->terminal == PaletteObjectTerminal::Recovered,
                  "recovery chain still settles as Recovered");
    ok &= Require(recoveryTerminal->stage == PaletteObjectStage::Drawn,
                  "recovery chain keeps the frozen stage=Drawn terminal shape");
  }
  ok &= Require(!rec.firstSightUsed(),
                "a recovery-only window must keep the version 2 shape");
  return ok;
}
void RunCase(const char* name, bool (*body)()) {
  g_case = CaseResult{};
  const bool ok = body();
  const bool passed = ok && g_case.failures == 0u;
  std::printf("[%s] %s (%u checks, %u failures)\n", passed ? "PASS" : "FAIL", name,
              static_cast<unsigned>(g_case.checks), static_cast<unsigned>(g_case.failures));
  if (passed)
    ++g_casePassed;
  else
    ++g_caseFailed;
}

} // namespace

// ---------------------------------------------------------------------------
// 2026-09-18 复核（Astra）P1 / 本计划阶段 C 的回归锁：
// 帧预算耗尽后到达的**新键** Reject **不得**建立条目。
//
// 反例形状（复核给出）：同帧先用 64 条新键 Rejected 吃满帧预算，再对第 65 个新键 NoteReject，
// 随后关窗 ⇒ 修复前第 65 键会**留下一个条目**，它只有一个 stage=Drawn 的终态、**没有任何链首**。
// 修复后（预算预检提前到 Insert 之前）该键必须**完全不出现**，且丢失被计入 droppedPerFrame。
//
// 注意本用例**不能**由 Case7Budgets(a) 代替：那里 100 个条目是在**各自独立帧**建立的，
// 因此"建条目时预算被拒"这条路径从未被走到（旧断言 watchCount==100 恒真）。
bool Case25RejectBudgetOrderNoHeadlessEntry() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();

  // ① 同一帧内用 kPerFrameBudget 个**新键**的 Rejected 吃满帧预算。
  const uint32_t full = PaletteObjectEvidence::kPerFrameBudget;
  for (uint32_t i = 0u; i < full; ++i)
    rec.NoteReject(MakeKey(0x30000u + i), PaletteObjectRejectReason::R1, MakeFrames(900u));
  ok &= Require(rec.watchCount() == full,
                "framing: the frame budget must be filled by 64 new-key rejects");
  ok &= Require(g_stored == full, "framing: exactly kPerFrameBudget events must be emitted");
  const uint64_t droppedFrameBefore = rec.counters().droppedPerFrame;
  const uint32_t watchBefore = rec.watchCount();

  // ② 第 65 个**新键**：预算已满 ⇒ 不得建条目，且计入 droppedPerFrame。
  const PaletteObjectKey refused = MakeKey(0x3FFFFu);
  rec.NoteReject(refused, PaletteObjectRejectReason::R1, MakeFrames(900u));
  ok &= Require(rec.watchCount() == watchBefore,
                "a budget-refused reject must NOT create a watch entry");
  ok &= Require(rec.counters().droppedPerFrame == droppedFrameBefore + 1u,
                "the refused reject must be charged to droppedPerFrame");
  ok &= Require(g_stored == full, "a budget-refused reject must emit no event");

  // ③ 关窗后，被拒的键不得以任何形状出现（尤其不得"只有终态、没有链首"）。
  rec.CloseWindow(901u);
  uint32_t refusedEvents = 0u;
  for (uint32_t i = 0u; i < g_stored; ++i)
    if (SameKeyFull(g_collected[i].key, refused))
      refusedEvents++;
  ok &= Require(refusedEvents == 0u,
                "the budget-refused key must stay entirely absent (no headless terminal)");
  return ok;
}

// ---------------------------------------------------------------------------
// 2026-09-18 阶段 C（复核 K2）的回归锁：`Recovered` 必须由**真实拒绝事实**支撑。
// 两条**完全相同**的 S/E/D 链，唯一差别是条目由哪个入口建立：
//   · NoteFirstSight（观察链，firstReason = NotChecked）⇒ **不得**报 Recovered；
//   · NoteReject（firstReason = 真实原因）      ⇒ **仍**必须报 Recovered（对照）。
// 对照不可省：没有它，一个"把所有链都算成 Unclosed"的错误实现也会让本用例通过。
// 2026-09-18 阶段 C（步骤④ 定案）：`PaletteObjectChainType` 与 `PaletteObjectTerminal` /
// `PaletteObjectRejectReason` / `PaletteObjectIdentityProofKind` 一样，定义在
// **namespace** `dxvk::war3::tools::evidence` 的 scope（**不是** `PaletteObjectEvidence` 的成员，
// 见 war3_palette_object_evidence.h:26/79/89/100/105）。本文件原本按底层值比较来回避，
// 现在改为**按名字**比较 —— 枚举子改名时编译器会报错，底层值比较不会。
using dxvk::war3::tools::evidence::PaletteObjectChainType;

bool Case26RecoveredRequiresRejectFact() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();
  const PaletteObjectFrames served = MakeFrames(503u);
  const PaletteObjectFrames enqueued = MakeFrames(504u);
  const PaletteObjectFrames drawn = MakeFrames(505u);

  // ① 观察链：无拒绝事实，却走完了 S/E/D。
  const PaletteObjectKey observed = MakeKey(0x40000u);
  rec.NoteFirstSight(observed, MakeFrames(500u));
  rec.NoteServed(observed, PaletteObjectSource::ProducerSnapshot, 0xABCDu, served);
  rec.NoteEnqueued(observed, PaletteObjectSource::ProducerSnapshot, 0xABCDu, enqueued, false);
  rec.NoteDrawn(observed, PaletteObjectSource::DrawTimeCaptured, 0xEEEEu, drawn, false);
  rec.CloseWindow(600u);
  bool observedSettled = false;
  bool observedRecovered = false;
  uint32_t observedTerminalCount = 0u;
  PaletteObjectTerminal observedTerminal = PaletteObjectTerminal::None;
  for (uint32_t i = 0u; i < g_stored; ++i)
    if (g_collected[i].terminal != PaletteObjectTerminal::None) {
      observedSettled = true;
      observedTerminal = g_collected[i].terminal;
      observedTerminalCount++;
      if (g_collected[i].terminal == PaletteObjectTerminal::Recovered)
        observedRecovered = true;
    }
  ok &= Require(observedSettled, "the observation chain must be settled at close");
  ok &= Require(!observedRecovered,
                "a chain with NO reject fact must never close as Recovered (K2)");
  // 2026-09-18 阶段 C（探针 #3，**直接**见证）：观察链走到 S/E/D 后必须是
  // ObservationClosed。此前该用例只断言"不得是 Recovered"，一个"什么都判成 Unclosed"
  // 的实现也能通过；这里把**实际终态**钉死。
  ok &= Require(observedTerminal == PaletteObjectTerminal::ObservationClosed,
                "an observation chain that reached S/E/D must settle as ObservationClosed");
  // 2026-09-18 阶段 C（探针 #2，桶标签）：结算桶必须等于该类终态的**实际条数**。
  // 没有这条，(a) 把 ObservationClosed 偷偷记进 closedUnclosed、或
  // (b) 根本不记任何桶，都能让其余用例通过。
  ok &= Require(rec.counters().closedObservationClosed == observedTerminalCount,
                "the ObservationClosed bucket must equal the number of such terminals");
  ok &= Require(rec.counters().closedUnclosed == 0u,
                "ObservationClosed must NOT be counted in the closedUnclosed bucket");
  // 2026-09-18 阶段 C（Q2 第一步）：链型必须**随事件**盖出（本轮只到记录层；wire 是下一步）。
  // 两类链的每条事件都必须带自己那一类的链型 —— 这是"两类链互不冒充"的记录层基础。
  bool observedTypeWrong = false;
  for (uint32_t i = 0u; i < g_stored; ++i)
    if (SameKeyFull(g_collected[i].key, observed) &&
        g_collected[i].chainType != PaletteObjectChainType::Observation)
      observedTypeWrong = true;
  ok &= Require(!observedTypeWrong,
                "every event of an observation chain must carry chainType=Observation");

  // ② 对照：同一形状的**拒绝恢复链**仍必须 Recovered。
  rec.Reset(1000u, 2000u);
  ResetCollector();
  const PaletteObjectKey rejected = MakeKey(0x50000u);
  rec.NoteReject(rejected, PaletteObjectRejectReason::R1, MakeFrames(500u));
  rec.NoteServed(rejected, PaletteObjectSource::ProducerSnapshot, 0xABCDu, served);
  rec.NoteEnqueued(rejected, PaletteObjectSource::ProducerSnapshot, 0xABCDu, enqueued, false);
  rec.NoteDrawn(rejected, PaletteObjectSource::DrawTimeCaptured, 0xEEEEu, drawn, false);
  rec.CloseWindow(600u);
  bool rejectedRecovered = false;
  for (uint32_t i = 0u; i < g_stored; ++i)
    if (g_collected[i].terminal == PaletteObjectTerminal::Recovered)
      rejectedRecovered = true;
  bool rejectedTypeWrong = false;
  for (uint32_t i = 0u; i < g_stored; ++i)
    if (SameKeyFull(g_collected[i].key, rejected) &&
        g_collected[i].chainType != PaletteObjectChainType::RejectionRecovery)
      rejectedTypeWrong = true;
  ok &= Require(!rejectedTypeWrong,
                "every event of a reject-recovery chain must carry chainType=RejectionRecovery");
  ok &= Require(rejectedRecovered,
                "a chain WITH a reject fact must still close as Recovered");
  return ok;
}

// ---------------------------------------------------------------------------
// 2026-09-18 阶段 C（Q2 裁定核心）：同一对象**同时**持有观察链与拒绝恢复链两条**独立**条目。
//
// 这条锁直接针对复核的 R→FirstSight 改类缺陷：修复前 NoteFirstSight 对「已由拒绝建立的条目」
// 不 Insert，而是在那条条目上发链首（实测 inserted=0 / emitted=1）⇒ 拒绝恢复链被**改类**。
// 现在两类链各自独立：该键关窗时必须结算出**两个**终态。
bool Case27TwoIndependentChainsPerObject() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1000u, 2000u);
  ResetCollector();
  const PaletteObjectKey key = MakeKey(0x60000u);

  // ① 拒绝恢复链：由 NoteReject 建立（真实拒绝事实），并推进到 Served。
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(500u));
  rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 0xABCDu, MakeFrames(503u));
  // ② 观察链：**同一对象**的另一条条目。修复前这里会复用上面的拒绝条目（inserted=0）。
  rec.NoteFirstSight(key, MakeFrames(510u));
  rec.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, 0xABCDu, MakeFrames(511u), false);
  rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0xEEEEu, MakeFrames(512u), false);
  rec.CloseWindow(600u);

  uint32_t terminals = 0u;
  uint32_t observationEvents = 0u;
  uint32_t rejectEvents = 0u;
  for (uint32_t i = 0u; i < g_stored; ++i) {
    if (!SameKeyFull(g_collected[i].key, key))
      continue;
    if (g_collected[i].chainType == PaletteObjectChainType::Observation)
      observationEvents++;
    else
      rejectEvents++;
    if (g_collected[i].terminal != PaletteObjectTerminal::None)
      terminals++;
  }
  // 关键断言：两条独立条目 ⇒ 两个终态，且两类事件都存在。
  ok &= Require(terminals == 2u,
                "one object must settle TWO independent chains at close");
  ok &= Require(observationEvents > 0u && rejectEvents > 0u,
                "events of BOTH chain types must be exported for the same object");
  // 2026-09-18 阶段 C（Q2 ⑥）：**归属规则**的实测断言。
  // 形状：Served 在观察链建立**之前**（⇒ 拒绝链）；Enqueued/Drawn 在**之后**（⇒ 观察链）。
  // 期望值来自实测对照（同形状下切换 Find(key)/FindOwner 各跑一次）：
  //   Find(key)  : servedRej=1 enqRej=1 drawRej=2 otherObs=2 otherRej=1
  //   FindOwner  : servedRej=1 enqObs=1 drawObs=2 drawRej=1 otherObs=1 otherRej=1
  // ⇒ 这两组数不同，故下面的断言**是载荷性的**（改回 Find(key) 会失败）。
  // 2026-09-18 P0-6（Astra 裁定 C1）后**第三组数**（实测）：
  //   C1（逐链记录）: servedObs=0 servedRej=1 enqObs=1 enqRej=1 drawObs>=1 drawRej=1
  // 差别正是裁定要的：两链并存时 E/D **两条链各记一份**（关联=完整键相等+链型相等+同窗口），
  // 于是拒绝恢复链不再丢掉恢复事实（Case 35 实测：它现在能认证 Recovered）。
  // ⚠️ 旧断言（enqRej==0 / drawRej<=1）锁的是**选项 A 的所有权规则**，已按裁定有意更新。
  // 注意 drawObs=2：观察链上既有一条**真实** NoteDrawn，也有一条终态回填的 Drawn
  // （round 20 我把两者合计写成了 2，实际是 enqObs(1)+drawObs(2)=3 —— 那次失败是**我的期望错**，
  //  不是规则错）。
  {
    uint32_t servedObs = 0u, servedRej = 0u, enqObs = 0u, enqRej = 0u,
             drawObs = 0u, drawRej = 0u;
    for (uint32_t i = 0u; i < g_stored; ++i) {
      if (!SameKeyFull(g_collected[i].key, key))
        continue;
      const bool obs =
          g_collected[i].chainType == PaletteObjectChainType::Observation;
      switch (g_collected[i].stage) {
        case PaletteObjectStage::ServedCandidate: if (obs) servedObs++; else servedRej++; break;
        case PaletteObjectStage::Enqueued:        if (obs) enqObs++;   else enqRej++;   break;
        case PaletteObjectStage::Drawn:           if (obs) drawObs++;  else drawRej++;  break;
        default: break;
      }
    }
    std::printf("    [OWNERSHIP-27] servedObs=%u servedRej=%u enqObs=%u enqRej=%u "
                "drawObs=%u drawRej=%u\n",
                servedObs, servedRej, enqObs, enqRej, drawObs, drawRej);
    ok &= Require(servedRej == 1u && servedObs == 0u,
                  "ownership: Served before the observation chain exists belongs to the REJECT chain");
    ok &= Require(enqObs == 1u && enqRej == 1u,
                  "ownership (P0-6/C1 已更新): with BOTH chains present the Enqueued fact must be "
                  "recorded on BOTH chains (explicit association = full key equality + same window), "
                  "not only on the observation chain");
    ok &= Require(drawObs >= 1u && drawRej >= 1u,
                  "ownership (P0-6/C1 已更新): the real NoteDrawn must be recorded on BOTH chains");
  }
  // 观察链的链首必须是**新建条目**（修复前 inserted=0，即复用了拒绝条目）。
  ok &= Require(rec.counters().firstSightInserted == 1u,
                "the observation chain head must be a NEW entry, not a reused reject entry");
  return ok;
}

// ---------------------------------------------------------------------------
// 2026-09-18 阶段 C（Q2 ⑥ 补齐）：归属规则的**另两种形状**。
// Case 27 只覆盖「两类链并存」。本用例覆盖：
//   · 只有**拒绝**链 ⇒ S/E/D 必须全部带 RejectionRecovery（不得被"优先观察"规则弄丢）；
//   · 只有**观察**链 ⇒ 链首必须带 Observation，且**不得凭空造出 Served 阶段**。
bool Case28OwnershipRuleSingleChainShapes() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(3000u, 4000u);
  ResetCollector();

  // 形状一：**只有拒绝链**。
  const PaletteObjectKey rejectOnly = MakeKey(0x70000u);
  rec.NoteReject(rejectOnly, PaletteObjectRejectReason::R2, MakeFrames(700u));
  rec.NoteServed(rejectOnly, PaletteObjectSource::ProducerSnapshot, 0x11u, MakeFrames(701u));
  rec.NoteEnqueued(rejectOnly, PaletteObjectSource::ProducerSnapshot, 0x11u, MakeFrames(702u), false);
  rec.NoteDrawn(rejectOnly, PaletteObjectSource::DrawTimeCaptured, 0x22u, MakeFrames(703u), false);

  // 形状二：**只有观察链**（没有任何拒绝事实）。
  const PaletteObjectKey obsOnly = MakeKey(0x71000u);
  rec.NoteFirstSight(obsOnly, MakeFrames(710u));

  uint32_t rejectOnlyObs = 0u, rejectOnlyRej = 0u;
  uint32_t obsOnlyObs = 0u, obsOnlyRej = 0u, obsOnlyServed = 0u;
  for (uint32_t i = 0u; i < g_stored; ++i) {
    const bool obs =
        g_collected[i].chainType == PaletteObjectChainType::Observation;
    if (SameKeyFull(g_collected[i].key, rejectOnly)) {
      if (obs) rejectOnlyObs++; else rejectOnlyRej++;
    }
    if (SameKeyFull(g_collected[i].key, obsOnly)) {
      if (obs) obsOnlyObs++; else obsOnlyRej++;
      if (g_collected[i].stage == PaletteObjectStage::ServedCandidate)
        obsOnlyServed++;
    }
  }
  std::printf("    [OWNERSHIP-28] rejectOnlyObs=%u rejectOnlyRej=%u obsOnlyObs=%u "
              "obsOnlyRej=%u obsOnlyServed=%u\n",
              rejectOnlyObs, rejectOnlyRej, obsOnlyObs, obsOnlyRej, obsOnlyServed);
  ok &= Require(rejectOnlyObs == 0u && rejectOnlyRej == 4u,
                "ownership: with ONLY a reject chain, S/E/D must all stay on RejectionRecovery");
  ok &= Require(obsOnlyObs == 1u && obsOnlyRej == 0u,
                "ownership: with ONLY an observation chain, its head must carry Observation");
  ok &= Require(obsOnlyServed == 0u,
                "an observation chain must never fabricate a Served stage");
  return ok;
}

// ---------------------------------------------------------------------------
// 2026-09-18 阶段 C（Q2 裁定）：**两类入口都必须先 CanEmit 再 Insert**。
// Case 25 已为 NoteReject 上锁；本用例是它在 NoteFirstSight 上的**对称件** ——
// 此前 firstSightInserted 只有正向断言（==1），"被预算拒绝时不得建条目"**未上锁**。
bool Case29FirstSightMustNotInsertWhenBudgetRefused() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(5000u, 6000u);
  ResetCollector();
  const uint64_t frame = 900u;
  // 同一帧内用**不同对象键**把每帧非终态预算（kPerFrameBudget == 64）打满。
  for (uint32_t i = 0u; i < 64u; ++i)
    rec.NoteFirstSight(MakeKey(0x80000u + i), MakeFrames(frame));
  const uint64_t insertedBefore = rec.counters().firstSightInserted;
  const uint32_t storedBefore = g_stored;
  // 第 65 个**全新**对象：预算已满 ⇒ 必须被拒，且**不得**建条目（不得留下无头条目）。
  rec.NoteFirstSight(MakeKey(0x90000u), MakeFrames(frame));
  ok &= Require(rec.counters().firstSightInserted == insertedBefore,
                "a budget-refused NoteFirstSight must NOT insert an entry (CanEmit must precede Insert)");
  ok &= Require(g_stored == storedBefore,
                "a budget-refused NoteFirstSight must NOT emit any event");
  std::printf("    [BUDGET-29] insertedBefore=%llu afterAttempt=%llu storedBefore=%u storedAfter=%u\n",
              static_cast<unsigned long long>(insertedBefore),
              static_cast<unsigned long long>(rec.counters().firstSightInserted),
              storedBefore, g_stored);
  return ok;
}

// ---------------------------------------------------------------------------
// 2026-09-18 阶段 C（K3）**诊断观测**（只打印，不断言）：
// 同一对象走完**两次完整尝试**（R→S→E→D 各一次），第二轮的 Rejected(秩1) 是否会
// 因为终身单调的 maxStage(=4) 被判成 orderViolation ⇒ 该条目不得判 Recovered？
// 先把终态打印出来，再决定怎么改（round 22 的教训：先看数，再写断言）。
bool Case30DiagnoseSecondAttemptOrdering() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(7000u, 8000u);
  ResetCollector();
  const PaletteObjectKey key = MakeKey(0xA0000u);
  // 第 1 次尝试：完整走完。
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(1000u));
  rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 0x1u, MakeFrames(1001u));
  rec.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, 0x1u, MakeFrames(1002u), false);
  rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x2u, MakeFrames(1003u), false);
  // 第 2 次尝试：**合法的新一轮**（不同帧）。
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(1010u));
  rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 0x1u, MakeFrames(1011u));
  rec.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, 0x1u, MakeFrames(1012u), false);
  rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x2u, MakeFrames(1013u), false);
  rec.CloseWindow(1100u);
  const char* terminal = "NoTerminal";
  uint32_t terminals = 0u;
  for (uint32_t i = 0u; i < g_stored; ++i) {
    if (!SameKeyFull(g_collected[i].key, key))
      continue;
    if (g_collected[i].terminal != PaletteObjectTerminal::None) {
      terminals++;
      switch (g_collected[i].terminal) {
        case PaletteObjectTerminal::Recovered:      terminal = "Recovered"; break;
        case PaletteObjectTerminal::WindowExpired:  terminal = "WindowExpired"; break;
        case PaletteObjectTerminal::ObjectGone:     terminal = "ObjectGone"; break;
        case PaletteObjectTerminal::Unclosed:       terminal = "Unclosed"; break;
        default:                                    terminal = "Other"; break;
      }
    }
  }
  // 2026-09-18 阶段 C（K3）：**哨兵契约的回归锁**。
  //
  // 本用例的帧域**没有**给 attemptSerial（保持哨兵 ~0ull）⇒ 判序必须与 K3 之前
  // **逐位相同**（终身单调）。这是兼容契约：在生产调用方尚未由裁定指定如何递增之前，
  // 实现不得悄悄改变未给号路径的行为。
  //
  // ⚠️ 断言值 Unclosed 描述的是**当前（未闭合的）实机行为**，不是期望的终态；
  //    它之所以被锁住，是因为「未知 ⇒ 不变」本身就是要保护的性质。
  //    期望的正确终态由 Case 31（显式尝试号）覆盖。
  ok &= Require(terminals == 1u,
                "the unknown-sentinel path must still settle exactly one chain");
  ok &= Require(std::strcmp(terminal, "Unclosed") == 0 &&
                rec.counters().closedRecovered == 0u,
                "unknown attempt serial MUST preserve the pre-K3 ordering verbatim "
                "(two attempts therefore still look like one violating attempt); "
                "the attempt-scoped behaviour is covered by Case 31");
  std::printf("    [K3-30] twoAttempts terminal=%s terminals=%u emitted=%llu "
              "closedRecovered=%llu closedUnclosed=%llu closedWindowExpired=%llu\n",
              terminal, terminals,
              static_cast<unsigned long long>(rec.counters().emitted),
              static_cast<unsigned long long>(rec.counters().closedRecovered),
              static_cast<unsigned long long>(rec.counters().closedUnclosed),
              static_cast<unsigned long long>(rec.counters().closedWindowExpired));
  return ok;
}

// ---------------------------------------------------------------------------
// 2026-09-18 阶段 C（K3）：**尝试内判序**的两侧断言。
//
// 背景（round 30 实测的反例）：同一对象两轮完整尝试后终态是 Unclosed ——
// 因为 Entry::maxStage 终身单调，第二轮的 Rejected(秩1) 小于它(=4) 被误判回退。
//
// 修法：调用方按值携带 attemptSerial；新尝试号 ⇒ 重置**本次尝试**基线、不判违规；
// 同一尝试号内回退 ⇒ **仍判违规**（裁定明确否定"每帧清零"）。
//
// 未知哨兵(~0ull) 时必须保持既有行为 ⇒ 生产在未显式给号前一位不变（由 Case 30 观测）。
bool Case31AttemptScopedOrdering() {
  bool ok = true;
  // 本用例专用的两个小工具（不污染其它用例的作用域）。
  const auto TerminalOfKey = [](const PaletteObjectKey& k) -> PaletteObjectTerminal {
    for (uint32_t i = 0u; i < g_stored; ++i) {
      if (!SameKeyFull(g_collected[i].key, k))
        continue;
      if (g_collected[i].terminal != PaletteObjectTerminal::None)
        return g_collected[i].terminal;
    }
    return PaletteObjectTerminal::None;
  };
  const auto TerminalName = [](PaletteObjectTerminal v) -> const char* {
    switch (v) {
      case PaletteObjectTerminal::Recovered:     return "Recovered";
      case PaletteObjectTerminal::WindowExpired: return "WindowExpired";
      case PaletteObjectTerminal::ObjectGone:    return "ObjectGone";
      case PaletteObjectTerminal::Unclosed:      return "Unclosed";
      default:                                   return "NoneOrOther";
    }
  };
  PaletteObjectEvidence& rec = Recorder();

  // ---- 形状一：**两个不同尝试号** ⇒ 必须判 Recovered（K3 的修法） ----
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(9000u, 10000u);
  ResetCollector();
  {
    const PaletteObjectKey key = MakeKey(0xB0000u);
    PaletteObjectFrames f = MakeFrames(1000u);
    f.attemptSerial = 7u;
    rec.NoteReject(key, PaletteObjectRejectReason::R1, f);
    f.renderFrame = 1001u; rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 0x1u, f);
    f.renderFrame = 1002u; rec.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, 0x1u, f, false);
    f.renderFrame = 1003u; rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x2u, f, false);
    // 第二次尝试：**新的明确关联尝试号**。
    f.renderFrame = 1010u; f.attemptSerial = 8u;
    rec.NoteReject(key, PaletteObjectRejectReason::R1, f);
    f.renderFrame = 1011u; rec.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 0x1u, f);
    f.renderFrame = 1012u; rec.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, 0x1u, f, false);
    f.renderFrame = 1013u; rec.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x2u, f, false);
    rec.CloseWindow(1100u);
    const PaletteObjectTerminal term = TerminalOfKey(key);
    std::printf("    [K3-31] twoAttemptsDistinctSerial terminal=%s closedRecovered=%llu\n",
                TerminalName(term),
                static_cast<unsigned long long>(rec.counters().closedRecovered));
    ok &= Require(term == PaletteObjectTerminal::Recovered,
                  "two distinct attempt serials must NOT be an order violation "
                  "(the second attempt is legitimate, so the entry must settle as Recovered)");
  }

  // ---- 形状二：**同一个尝试号**内的回退 ⇒ 仍必须违规（不得让检查失效） ----
  {
    PaletteObjectEvidence& rec2 = Recorder();
    rec2.Configure(&Collect, &g_contextMarker);
    rec2.Reset(11000u, 12000u);
    ResetCollector();
    const PaletteObjectKey key = MakeKey(0xB1000u);
    PaletteObjectFrames f = MakeFrames(2000u);
    f.attemptSerial = 42u;   // **全程同一个尝试号**
    rec2.NoteReject(key, PaletteObjectRejectReason::R1, f);
    f.renderFrame = 2001u; rec2.NoteServed(key, PaletteObjectSource::ProducerSnapshot, 0x1u, f);
    f.renderFrame = 2002u; rec2.NoteEnqueued(key, PaletteObjectSource::ProducerSnapshot, 0x1u, f, false);
    f.renderFrame = 2003u; rec2.NoteDrawn(key, PaletteObjectSource::DrawTimeCaptured, 0x2u, f, false);
    // 同一尝试内**回退**到 Rejected ⇒ 必须仍被判违规 ⇒ 不得判 Recovered。
    f.renderFrame = 2004u;
    rec2.NoteReject(key, PaletteObjectRejectReason::R1, f);
    rec2.CloseWindow(2100u);
    const PaletteObjectTerminal term2 = TerminalOfKey(key);
    std::printf("    [K3-31] sameSerialRollback terminal=%s\n", TerminalName(term2));
    ok &= Require(term2 != PaletteObjectTerminal::Recovered,
                  "a rollback WITHIN the same attempt must still be an order violation "
                  "(attempt scoping must not disable the check)");
  }
  return ok;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 2026-09-18 阶段 C（K3 **生产调用方传参**）：工厂 → 记录器的真实传参路径。
//
// Case 31 是直接给帧域赋 attemptSerial；本用例走 `MakePaletteObjectFrames`，
// 即**生产三站点实际使用的那条路**，证明值真的被工厂带进判序，
// 且**省略参数时保持未知哨兵**（否则既有调用方会被静默改成"第一次尝试"）。
bool Case33FactoryCarriesAttemptSerial() {
  bool ok = true;
  const PaletteObjectFrames withSerial =
      dxvk::war3::tools::evidence::MakePaletteObjectFrames(900u, 777u, 0u, false, 777u);
  ok &= Require(withSerial.attemptSerial == 777u,
                "K3: the factory must carry attemptSerial BY VALUE");
  ok &= Require(withSerial.recordFrameSerial == 777u,
                "K3: the attempt serial must not disturb recordFrameSerial");
  const PaletteObjectFrames withoutSerial =
      dxvk::war3::tools::evidence::MakePaletteObjectFrames(900u, 777u, 0u, false);
  ok &= Require(withoutSerial.attemptSerial == ~0ull,
                "K3: omitting the argument must keep the UNKNOWN sentinel "
                "(otherwise legacy callers silently become \"first attempt\")");
  return ok;
}

// ---------------------------------------------------------------------------
// 2026-09-18 阶段 C（窗口维度）：**锁住"真空满足"所依赖的那个条件**。
//
// 事实：两条重新 arm 的路径（Reset / ResetForSessionTransition）都**清空整张表**
// ⇒ 重新 arm 后不存在陈旧条目 ⇒ 「查找维度含窗口」在当前 API 下**真空满足**。
// 因此不能用一个用例去证明窗口分量有效（无可观测差异）；但可以保护那个使它真空成立的条件。
// 本用例断言的是**清表 + 新段号**，不是"窗口维度已实现"。两者不可混淆。
bool Case34ReArmClearsTable() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Reset(11u, 22u);
  const PaletteObjectKey key = MakeKey(0x900u);
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(1u));
  ok &= Require(rec.watchCount() == 1u,
                "window-dimension precondition: a reject must occupy one observation entry");
  rec.Reset(12u, 22u);
  ok &= Require(rec.watchCount() == 0u,
                "WINDOW DIMENSION: Reset MUST clear the observation table; if a re-arm ever "
                "PRESERVES entries the lookup key silently needs an explicit window component");
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(2u));
  rec.ResetForSessionTransition(13u, 22u);
  ok &= Require(rec.watchCount() == 0u,
                "WINDOW DIMENSION: ResetForSessionTransition MUST clear the observation table");
  // **真正载荷的守卫**：查找时遇到"同 key+链型 但别的窗口"的次数必须为 0。
  // 这条是可失败的：若有人把 re-arm 改成"保留条目"，旧条目会被窗口分量挡下 ⇒ 计数 > 0。
  ok &= Require(rec.counters().windowMismatchLookups == 0u,
                "WINDOW DIMENSION: a lookup must never meet an entry from ANOTHER window. "
                "A non-zero count means a re-arm PRESERVED entries (the lookup is window-aware "
                "only because of this component) -- see the window-dimension docs.");
  return ok;
}

// 2026-09-18：**纯测量**（0 断言）。问：拒绝链在**双链并存**时还能不能结算为 Recovered？
//
// 形状 2 = **对照**：只有拒绝链 + 完整恢复序列（served→enqueued→drawn）⇒ 期望 Recovered。
// 形状 1 = **处理**：两链并存 + 同样的完整恢复序列 ⇒ 观察实际终态。
//
// 只有"对照 Recovered 而处理非 Recovered"才构成**实测**的缺陷证据。
bool Case35BothChainsServedRouting() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  const PaletteObjectSource src = PaletteObjectSource::ArenaSlot;

  // ---- 形状 2（对照）：只有拒绝链 ----
  rec.Reset(31u, 41u);
  {
    const PaletteObjectKey k = MakeKey(0x951u);
    rec.NoteReject(k, PaletteObjectRejectReason::R1, MakeFrames(20u));
    rec.NoteServed(k, src, 7u, MakeFrames(21u));
    rec.NoteEnqueued(k, src, 7u, MakeFrames(21u), false);
    rec.NoteDrawn(k, src, 7u, MakeFrames(21u), false);
    rec.CloseWindow(22u);
    const auto& c = rec.counters();
    std::printf("    [OWN-35-control  ] recovered=%llu unclosed=%llu windowExpired=%llu obsClosed=%llu\n",
                static_cast<unsigned long long>(c.closedRecovered),
                static_cast<unsigned long long>(c.closedUnclosed),
                static_cast<unsigned long long>(c.closedWindowExpired),
                static_cast<unsigned long long>(c.closedObservationClosed));
    // ✅ **真正的正确性断言**：完整恢复序列（served→enqueued→drawn）**必须**结算为 Recovered。
    ok &= Require(c.closedRecovered == 1u && c.closedWindowExpired == 0u,
                  "a full served->enqueued->drawn sequence after a reject MUST settle as "
                  "Recovered (the single-chain control); otherwise Recovered is unreachable");
  }

  // ---- 形状 1（处理）：两链并存 + 同样的完整恢复序列 ----
  rec.Reset(32u, 42u);
  ResetCollector();
  {
    const PaletteObjectKey k = MakeKey(0x952u);
    rec.NoteReject(k, PaletteObjectRejectReason::R1, MakeFrames(30u));
    rec.NoteFirstSight(k, MakeFrames(31u));
    rec.NoteServed(k, src, 7u, MakeFrames(32u));
    rec.NoteEnqueued(k, src, 7u, MakeFrames(32u), false);
    rec.NoteDrawn(k, src, 7u, MakeFrames(32u), false);
    rec.CloseWindow(33u);
    const auto& c = rec.counters();
    std::printf("    [OWN-35-dualchain] recovered=%llu unclosed=%llu windowExpired=%llu obsClosed=%llu\n",
                static_cast<unsigned long long>(c.closedRecovered),
                static_cast<unsigned long long>(c.closedUnclosed),
                static_cast<unsigned long long>(c.closedWindowExpired),
                static_cast<unsigned long long>(c.closedObservationClosed));
    // ⚠️ **特征化断言（characterization）—— 不是正确性主张**。
    // ✅ **2026-09-18 P0-6（Astra 裁定 C1：明确关联后的逐链记录）—— 本断言已按裁定更新。**
    //
    // 旧特征化断言（选项 A）说：双链并存时拒绝恢复链会结算成 `WindowExpired`，
    // 因为 `FindOwner` 全局优先观察链 ⇒ 拒绝链拿不到 served/enqueued/drawn 事实。
    // C1 实施后，S/E/D 对**两条链各自**记一份（关联 = 完整键相等 + 链型相等 + 同窗口），
    // 于是拒绝链既能认证 `Recovered`（真的被接住并绘制），也不再**谎称「窗口内从未被接住」**。
    // （有 served 事实、但缺完整的 submit/draw 证据 ⇒ 无法认证恢复）。
    //
    // 这不是放宽，而是**修正一个会误导的标签**：`WindowExpired` 断言「真的发生过拒绝、窗口内从未被接住」，
    // 而同一份导出里另一条链证明它被接住并绘制了（独立复审 B 项）。
    ok &= Require(c.closedRecovered == 1u && c.closedWindowExpired == 0u && c.closedUnclosed == 0u,
                  "CHARACTERIZATION (P0-6/C1 已更新): with BOTH chains present the rejection-recovery "
                  "chain must be able to certify Recovered (it now receives the served/enqueued/drawn "
                  "facts), and must NOT claim WindowExpired (which would deny being served while "
                  "another chain proves it was)");
    // 2026-09-18 P0-6（复审的载荷性判定）：旧断言只**描述终态桶** ⇒ 对「把路由改成 Find(key)」
    // 这种回归**仍会通过**（那条路由下观察链一条路径事实都没有）。现改为**同时**断言观察链上
    // 真的存在 **live 的 Served/Enqueued 事实** ⇒ 对两种回归都具载荷性。
    uint32_t obsServed = 0u, obsEnqueued = 0u, obsDrawn = 0u;
    for (uint32_t i = 0u; i < g_stored; ++i) {
      if (g_collected[i].chainType != PaletteObjectChainType::Observation)
        continue;
      switch (g_collected[i].stage) {
        case PaletteObjectStage::ServedCandidate: obsServed++; break;
        case PaletteObjectStage::Enqueued: obsEnqueued++; break;
        case PaletteObjectStage::Drawn: obsDrawn++; break;
        default: break;
      }
    }
    ok &= Require(c.closedObservationClosed == 1u && obsServed >= 1u && obsEnqueued >= 1u &&
                      obsDrawn >= 1u,
                  "CHARACTERIZATION (P0-6/C1): the observation chain must own the LIVE "
                  "served/enqueued/drawn facts AND settle as ObservationClosed -- asserting only "
                  "the terminal bucket would still pass for a routing that hands the observation "
                  "chain no path fact at all");
    for (uint32_t i = 0u; i < g_stored; ++i) {
      std::printf("      dual rec[%u] stage=%u terminal=%u chain=%u\n", i,
                  static_cast<unsigned>(g_collected[i].stage),
                  static_cast<unsigned>(g_collected[i].terminal),
                  static_cast<unsigned>(g_collected[i].chainType));
    }
  }
  return ok;
}

// 2026-09-18 P0-1：**会话接纳边界**（Astra 实测：旧会话在途键进入新会话，
// 线里出现「顶层 session=新 / 对象键 sessionGeneration=旧」的串会话事件）。
bool Case36StaleSessionRejected() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  // (a) 旧会话键必须被拒绝（真实旧会话号 = 70-1）
  rec.Reset(70u, 41u);
  ResetCollector();
  PaletteObjectKey stale = MakeKey(0xA00u);
  stale.sessionGeneration = 69u;
  rec.NoteFirstSight(stale, MakeFrames(10u));
  rec.NoteServed(stale, PaletteObjectSource::ArenaSlot, 1u, MakeFrames(11u));
  rec.CloseWindow(12u);
  const uint64_t staleDrops = rec.counters().droppedStaleSession;
  std::printf("    [SESS-36] staleDrops=%llu stored=%u\n",
              static_cast<unsigned long long>(staleDrops), g_stored);
  ok &= Require(staleDrops == 2u,
                "P0-1: BOTH stale-session calls must be rejected (FirstSight + Served)");
  ok &= Require(g_stored == 0u,
                "P0-1: a stale-session key must produce NO event at all");
  // (b) 对照：当前会话键必须被接纳（否则上面的拒绝可能只是\"什么都拒\"）
  rec.Reset(71u, 42u);
  ResetCollector();
  const PaletteObjectKey fresh = MakeKey(0xA01u);
  rec.NoteFirstSight(fresh, MakeFrames(20u));
  rec.CloseWindow(21u);
  std::printf("    [SESS-36] fresh stored=%u\n", g_stored);
  ok &= Require(g_stored >= 1u,
                "P0-1: a CURRENT-session key MUST be admitted (control for the rejection above)");
  return ok;
}

// 2026-09-18 P0-2（Astra 实测反例）：终态阶段回填必须是**该条目实际达到的最高语义秩**，
// 与尝试号是否每次相同**无关**。五个点使用**同一个已知**尝试号时，旧实现首点走
// 「新尝试」、其后走「同尝试」，**两条分支都不更新 maxStage** ⇒ 回填成 FirstSight。
bool Case37TerminalStageBackfill() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Reset(90u, 60u);
  ResetCollector();
  const PaletteObjectKey key = MakeKey(0xC00u);
  const uint64_t serial = 0x37u;
  PaletteObjectFrames f0 = MakeFrames(100u); f0.attemptSerial = serial;
  PaletteObjectFrames f1 = MakeFrames(101u); f1.attemptSerial = serial;
  PaletteObjectFrames f2 = MakeFrames(102u); f2.attemptSerial = serial;
  PaletteObjectFrames f3 = MakeFrames(103u); f3.attemptSerial = serial;
  rec.NoteFirstSight(key, f0);
  rec.NoteServed(key, PaletteObjectSource::ArenaSlot, 1u, f1);
  rec.NoteEnqueued(key, PaletteObjectSource::ArenaSlot, 1u, f2, false);
  rec.NoteDrawn(key, PaletteObjectSource::ArenaSlot, 1u, f3, false);
  rec.CloseWindow(104u);
  uint32_t terminalStage = 0xFFFFu;
  bool found = false;
  for (uint32_t i = 0u; i < g_stored; ++i)
    if (g_collected[i].terminal != PaletteObjectTerminal::None) {
      terminalStage = static_cast<uint32_t>(g_collected[i].stage);
      found = true;
    }
  std::printf("    [P0-2] sameKnownSerial terminalStage=%u (Drawn=%u) stored=%u\n",
              terminalStage, static_cast<unsigned>(PaletteObjectStage::Drawn), g_stored);
  ok &= Require(found, "P0-2: a closed window must emit a terminal record");
  ok &= Require(terminalStage == static_cast<uint32_t>(PaletteObjectStage::Drawn),
                "P0-2: the terminal stage must reflect the highest rank ACTUALLY reached "
                "(Drawn) even when every capture point carries the SAME known attempt serial; "
                "before the fix maxStage was never raised in either known-serial branch, so a "
                "chain that did reach Drawn was written back as FirstSight (Astra measured "
                "terminal=ObservationClosed stage=FirstSight sawDraw=true)");
  return ok;
}

// 2026-09-18 P0-3（Astra 实测修正）：ObjectGone 既不得**伪造阶段**，也不得只处理一条链。
bool Case38ObjectGoneBothChains() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  // (a) 只存在观察链：终态**不得**被盖上 Rejected（该链从未被拒绝过）
  rec.Reset(100u, 70u);
  ResetCollector();
  const PaletteObjectKey k1 = MakeKey(0xD00u);
  rec.NoteFirstSight(k1, MakeFrames(200u));
  rec.NoteObjectGone(k1);
  uint32_t stage1 = 0xFFFFu;
  bool found1 = false;
  for (uint32_t i = 0u; i < g_stored; ++i)
    if (g_collected[i].terminal == PaletteObjectTerminal::ObjectGone) {
      stage1 = static_cast<uint32_t>(g_collected[i].stage);
      found1 = true;
    }
  std::printf("    [P0-3] observationOnly stage=%u (Rejected=1 FirstSight=5) gone=%llu\n",
              stage1,
              static_cast<unsigned long long>(rec.counters().closedObjectGone));
  ok &= Require(found1, "P0-3: ObjectGone on an observation chain must emit a terminal record");
  ok &= Require(stage1 != static_cast<uint32_t>(PaletteObjectStage::Rejected),
                "P0-3: an OBSERVATION chain must NOT be stamped with the Rejected stage - that "
                "is a fabricated fact (the chain was never rejected) and it contradicts our own "
                "reader rule 'observationChainMustNotCarryRejectedStage'");
  ok &= Require(stage1 == static_cast<uint32_t>(PaletteObjectStage::FirstSight),
                "P0-3: the observation terminal stage must be the highest rank actually reached "
                "(FirstSight=5 for a chain that only observed)");
  // (b) 两条链并存：两条**都要**结算（原来只处理 FindOwner 的一条）
  rec.Reset(101u, 71u);
  ResetCollector();
  const PaletteObjectKey k2 = MakeKey(0xD01u);
  rec.NoteReject(k2, PaletteObjectRejectReason::R1, MakeFrames(300u));
  rec.NoteFirstSight(k2, MakeFrames(301u));
  rec.NoteObjectGone(k2);
  std::printf("    [P0-3] bothChains gone=%llu windowExpired=%llu watchCount=%llu\n",
              static_cast<unsigned long long>(rec.counters().closedObjectGone),
              static_cast<unsigned long long>(rec.counters().closedWindowExpired),
              static_cast<unsigned long long>(rec.watchCount()));
  ok &= Require(rec.counters().closedObjectGone == 2u,
                "P0-3: with BOTH chains present, ObjectGone must settle EACH chain (2 terminals); "
                "before the fix only FindOwner's one chain was settled and the other was stranded");
  ok &= Require(rec.counters().closedWindowExpired == 0u,
                "P0-3: the stranded chain must not later expire as WindowExpired");
  ok &= Require(rec.watchCount() == 0u,
                "P0-3: both entries must be tombstoned/removed after ObjectGone");
  return ok;
}

// 2026-09-18 P0-1（收窄）：0 代际键**只在记录器自身也没有会话时**才放行。
// 理由：sink 用 ActiveSession() 写顶层 session，而键的 bits[10]/[29] 取 record.key.sessionGeneration，
// 读方硬性要求两者相等，否则拒绝整份导出（palette session generation mirror mismatch）。
bool Case39ZeroGenerationKeyNeedsSessionlessRecorder() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  // (a) 有会话 + 0 代际键 ⇒ 必须拒绝（否则会产出让读方拒整份导出的错配事件）
  rec.Reset(110u, 80u);
  ResetCollector();
  PaletteObjectKey zero = MakeKey(0xE00u);
  zero.sessionGeneration = 0u;
  rec.NoteFirstSight(zero, MakeFrames(400u));
  const uint64_t drops = rec.counters().droppedStaleSession;
  std::printf("    [P0-39] sessionKnown zeroKeyDrops=%llu stored=%u\n",
              static_cast<unsigned long long>(drops), g_stored);
  ok &= Require(drops == 1u,
                "P0-39: a 0-generation key must be REJECTED while the recorder has a session "
                "- otherwise the wire carries session=N with key bits[10]/[29]=0 and the reader "
                "rejects the WHOLE export (session generation mirror mismatch)");
  ok &= Require(g_stored == 0u, "P0-39: that key must produce no event at all");
  // (b) 对照：记录器自身也没有会话（未配置用法，Case 1 的契约）⇒ 必须放行
  rec.Reset(0u, 0u);
  ResetCollector();
  PaletteObjectKey zero2 = MakeKey(0xE01u);
  zero2.sessionGeneration = 0u;
  rec.NoteFirstSight(zero2, MakeFrames(401u));
  std::printf("    [P0-39] sessionless stored=%u\n", g_stored);
  ok &= Require(g_stored >= 1u,
                "P0-39: a session-less recorder (default/unconfigured usage) must still accept "
                "0-generation keys, otherwise the existing 'inert but still counts' contract breaks");
  return ok;
}

// 2026-09-18 P0-4（Astra 裁定）：仅链首 / 只到入队的**观察链**必须结算为 ObservationClosed；
// 而**拒绝链**从未被接住仍必须是 WindowExpired —— 防止重排把两种含义混起来。
bool Case40ObservationClosedVsWindowExpired() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  uint32_t t1 = 0xFFFFu, t2 = 0xFFFFu, t3 = 0xFFFFu;
  // (a) 仅链首的观察链
  rec.Reset(130u, 100u);
  ResetCollector();
  const PaletteObjectKey k1 = MakeKey(0x1000u);
  rec.NoteFirstSight(k1, MakeFrames(800u));
  rec.CloseWindow(801u);
  for (uint32_t i = 0u; i < g_stored; ++i)
    if (g_collected[i].terminal != PaletteObjectTerminal::None)
      t1 = static_cast<uint32_t>(g_collected[i].terminal);
  // (b) 只到入队 —— **生产真实序列**：该路径（d3d9_device.cpp:23539-23548）故意不发 Served
  rec.Reset(131u, 101u);
  ResetCollector();
  const PaletteObjectKey k2 = MakeKey(0x1001u);
  rec.NoteFirstSight(k2, MakeFrames(810u));
  rec.NoteEnqueued(k2, PaletteObjectSource::Unknown, 0u, MakeFrames(811u), false);
  rec.CloseWindow(812u);
  for (uint32_t i = 0u; i < g_stored; ++i)
    if (g_collected[i].terminal != PaletteObjectTerminal::None)
      t2 = static_cast<uint32_t>(g_collected[i].terminal);
  // (c) 对照：**拒绝链**从未被接住
  rec.Reset(132u, 102u);
  ResetCollector();
  const PaletteObjectKey k3 = MakeKey(0x1002u);
  rec.NoteReject(k3, PaletteObjectRejectReason::R1, MakeFrames(820u));
  rec.CloseWindow(821u);
  for (uint32_t i = 0u; i < g_stored; ++i)
    if (g_collected[i].terminal != PaletteObjectTerminal::None)
      t3 = static_cast<uint32_t>(g_collected[i].terminal);
  std::printf("    [P0-40] headOnly=%u enqueuedOnly=%u rejectNoCatch=%u (ObservationClosed=%u WindowExpired=%u)\n",
              t1, t2, t3,
              static_cast<unsigned>(PaletteObjectTerminal::ObservationClosed),
              static_cast<unsigned>(PaletteObjectTerminal::WindowExpired));
  ok &= Require(t1 == static_cast<uint32_t>(PaletteObjectTerminal::ObservationClosed),
                "P0-4: an observation chain closed right after its head must settle as "
                "ObservationClosed (honestly reporting that draw evidence is missing), not "
                "WindowExpired - that is the rejection chain's meaning");
  ok &= Require(t2 == static_cast<uint32_t>(PaletteObjectTerminal::ObservationClosed),
                "P0-4: the REAL production sequence FirstSight->Enqueued->Close must also settle as "
                "ObservationClosed; that capture path deliberately never emits Served, so demanding "
                "Served here would mean inventing a fact");
  ok &= Require(t3 == static_cast<uint32_t>(PaletteObjectTerminal::WindowExpired),
                "P0-4 control: a REJECTION chain that was never caught must still settle as "
                "WindowExpired - the reorder must not blur the two meanings");
  return ok;
}

// 2026-09-18 P0-3（复审 C1）：在 ObjectGone 终态的 emit 内**重入 NoteObjectGone**。
// 修复前实测：同一条链拿到**两个终态**，而读方硬要求「一个对象至多一个终态事件」⇒ 整份导出被拒。
PaletteObjectKey g_goneReentryKey{};
std::atomic<bool> g_goneReentered{false};
void ReentrantGoneEmitter(void* context, const PaletteObjectEventRecord& record) {
  Collect(context, record);
  if (record.terminal == PaletteObjectTerminal::ObjectGone &&
      !g_goneReentered.exchange(true))
    Recorder().NoteObjectGone(g_goneReentryKey);
}

bool Case41ObjectGoneReentrancyNoDoubleTerminal() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Reset(140u, 110u);
  g_goneReentryKey = MakeKey(0x1100u);
  g_goneReentered = false;
  rec.Configure(&ReentrantGoneEmitter, &g_contextMarker);
  ResetCollector();
  rec.NoteReject(g_goneReentryKey, PaletteObjectRejectReason::R1, MakeFrames(900u));
  rec.NoteObjectGone(g_goneReentryKey);
  rec.Configure(&Collect, &g_contextMarker);
  uint32_t terminals = 0u;
  for (uint32_t i = 0u; i < g_stored; ++i)
    if (g_collected[i].terminal != PaletteObjectTerminal::None)
      terminals++;
  std::printf("    [P0-41] reentered=%d terminals=%u watchCount=%u lostToReentrancy=%llu\n",
              g_goneReentered.load() ? 1 : 0, terminals, rec.watchCount(),
              static_cast<unsigned long long>(rec.counters().objectGoneLostToReentrancy));
  ok &= Require(g_goneReentered.load(),
                "P0-41: the emitter must really re-enter NoteObjectGone while the state lock is held");
  ok &= Require(terminals == 1u,
                "P0-3/C1: a re-entrant NoteObjectGone must NOT produce a SECOND terminal for the same "
                "chain - the reader hard-requires at most one terminal event per object "
                "(analyze_palette_object_evidence.py: one object may carry at most one terminal event) "
                "and otherwise rejects the WHOLE export");
  ok &= Require(rec.watchCount() <= 1024u,
                "P0-3/R4: watchCount must never underflow (reader requires watchCount <= 1024)");
  return ok;
}

// 2026-09-18 P0-6（复审 41e85203 的**丢弃路径清单**）：两个此前**无声**的丢弃类现在必须具名计数。
// 它们的共同点：事件已经产生、却因为「窗口已关」或「没有链可记」而消失 —— 在本项目里，
// 「不落计数」就是无声丢失（读方会看到 emitted != exported 却查不出原因）。
bool Case42SilentDropCounters() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(150u, 120u);
  ResetCollector();
  // (a) 两条链都不存在 ⇒ 这条路径事实无处可记（复审推断可达：FirstSight 与 S/D 的构造键不同源）。
  const uint64_t beforeNoChain = rec.counters().droppedNoChainForPathFact;
  rec.NoteServed(MakeKey(0x2200u), PaletteObjectSource::OwnedPartSnapshot, 3u,
                 MakeFrames(700u));
  const uint64_t afterNoChain = rec.counters().droppedNoChainForPathFact;
  ok &= Require(afterNoChain == beforeNoChain + 1u,
                "P0-6: a path fact with NO chain at all must be counted (droppedNoChainForPathFact); "
                "dropping it silently makes the export look complete while the fact is gone");
  ok &= Require(g_stored == 0u, "P0-6: nothing may be emitted when there is no chain to record into");
  // (b) 窗口关闭之后到达的采集调用 ⇒ 必须计入 droppedWindowClosedEntry（6 个入口各一处早退）。
  // (c) 复审 128c3826 的**第四个同类静默丢弃**：两条链都不存在时的 ObjectGone 此前零计数、零终态。
  const uint64_t beforeGoneNoChain = rec.counters().droppedNoChainForPathFact;
  rec.NoteObjectGone(MakeKey(0x2202u));
  ok &= Require(rec.counters().droppedNoChainForPathFact == beforeGoneNoChain + 1u,
                "P0-6: ObjectGone with NO chain at all must be counted too -- it used to be "
                "zero-count and zero-terminal (the same silent-drop class as S/E/D)");
  rec.NoteReject(MakeKey(0x2201u), PaletteObjectRejectReason::R1, MakeFrames(701u));
  rec.CloseWindow(702u);
  const uint64_t beforeClosed = rec.counters().droppedWindowClosedEntry;
  rec.NoteServed(MakeKey(0x2201u), PaletteObjectSource::OwnedPartSnapshot, 3u,
                 MakeFrames(703u));
  rec.NoteEnqueued(MakeKey(0x2201u), PaletteObjectSource::OwnedPartSnapshot, 3u,
                   MakeFrames(703u), false);
  const uint64_t afterClosed = rec.counters().droppedWindowClosedEntry;
  std::printf("    [P0-42] noChain=%llu windowClosed=%llu\n",
              static_cast<unsigned long long>(afterNoChain - beforeNoChain),
              static_cast<unsigned long long>(afterClosed - beforeClosed));
  ok &= Require(afterClosed == beforeClosed + 2u,
                "P0-6: capture calls arriving after CloseWindow must be counted "
                "(droppedWindowClosedEntry), not silently ignored");
  return ok;
}

// 2026-09-18 P0-3（复审 C3 的**载荷用例**；复审实测该修正此前零覆盖）：
// 在 ObjectGone 终态的 emit 内**重入 CloseWindow**（生产可达：环预冻结钩子回调 CloseWindow）。
// 嵌套结算会把第二条（观察）链结算掉 ⇒ 回到 NoteObjectGone 时它已不可结算。
// C3 的判据因此必须是「重新 FindChain 确认**仍可结算**」，而不是 `m_windowClosed`
//（ResetForSessionTransition 类失效会清表却不置该标志 ⇒ 用窗口标志会既漏判又丢计数）。
std::atomic<bool> g_goneCloseReentered{false};
void ReentrantGoneCloseEmitter(void* context, const PaletteObjectEventRecord& record) {
  Collect(context, record);
  if (record.terminal == PaletteObjectTerminal::ObjectGone &&
      !g_goneCloseReentered.exchange(true))
    Recorder().CloseWindow(999u);
}

bool Case43ObjectGoneObservationLostToReentrantClose() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Reset(160u, 130u);
  const PaletteObjectKey key = MakeKey(0x3300u);
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(800u));
  rec.NoteFirstSight(key, MakeFrames(801u));
  g_goneCloseReentered = false;
  rec.Configure(&ReentrantGoneCloseEmitter, &g_contextMarker);
  ResetCollector();
  rec.NoteObjectGone(key);
  rec.Configure(&Collect, &g_contextMarker);
  uint32_t terminalsPerChain[2] = {0u, 0u};
  for (uint32_t i = 0u; i < g_stored; ++i) {
    if (g_collected[i].terminal == PaletteObjectTerminal::None)
      continue;
    const uint32_t chain =
        g_collected[i].chainType == PaletteObjectChainType::Observation ? 1u : 0u;
    terminalsPerChain[chain]++;
  }
  std::printf("    [P0-43] reentered=%d rejTerm=%u obsTerm=%u lostToReentrancy=%llu "
              "watchCount=%u\n",
              g_goneCloseReentered.load() ? 1 : 0, terminalsPerChain[0],
              terminalsPerChain[1],
              static_cast<unsigned long long>(rec.counters().objectGoneLostToReentrancy),
              rec.watchCount());
  ok &= Require(g_goneCloseReentered.load(),
                "P0-43: the emitter must really re-enter CloseWindow on the ObjectGone terminal "
                "(otherwise this case proves nothing)");
  ok &= Require(terminalsPerChain[0] <= 1u && terminalsPerChain[1] <= 1u,
                "P0-3/C1: the nested close must not give either chain a SECOND terminal "
                "(the reader hard-requires at most one terminal per chain)");
  ok &= Require(rec.counters().objectGoneLostToReentrancy == 1u,
                "P0-3/C3: the observation chain's ObjectGone swallowed by a nested close must be "
                "COUNTED -- the criterion must be 'is it still settle-able' (re-FindChain), not "
                "m_windowClosed, which Reset-class invalidation does not set");
  ok &= Require(rec.watchCount() <= 1024u,
                "P0-3/R4: watchCount must not underflow");
  return ok;
}

// 2026-09-18 P0-6（复审 C1-1 的**载荷用例**）：两条链 + 在**拒绝链**的 ServedCandidate emit 内
// 同线程重入 CloseWindow（生产可达：环预冻结钩子 → sink → CloseWindow）。
// 嵌套 CloseWindow 会清掉观察链条目 ⇒ 「第一次 advance 之后再取观察链指针」的旧写法会走
// nullptr 裸 return：**静默丢失且零计数**（复审实测该计数恒 0）。
std::atomic<bool> g_servedCloseReentered{false};
void ReentrantServedCloseEmitter(void* context, const PaletteObjectEventRecord& record) {
  Collect(context, record);
  if (record.stage == PaletteObjectStage::ServedCandidate &&
      !g_servedCloseReentered.exchange(true))
    Recorder().CloseWindow(998u);
}

bool Case44DualChainReentrantCloseCountsLostPathFact() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Reset(170u, 140u);
  const PaletteObjectKey key = MakeKey(0x4400u);
  rec.NoteReject(key, PaletteObjectRejectReason::R1, MakeFrames(810u));
  rec.NoteFirstSight(key, MakeFrames(811u));
  g_servedCloseReentered = false;
  rec.Configure(&ReentrantServedCloseEmitter, &g_contextMarker);
  ResetCollector();
  rec.NoteServed(key, PaletteObjectSource::OwnedPartSnapshot, 5u, MakeFrames(812u));
  rec.Configure(&Collect, &g_contextMarker);
  uint32_t servedObs = 0u, servedRej = 0u;
  for (uint32_t i = 0u; i < g_stored; ++i) {
    if (g_collected[i].stage != PaletteObjectStage::ServedCandidate)
      continue;
    if (g_collected[i].chainType == PaletteObjectChainType::Observation)
      servedObs++;
    else
      servedRej++;
  }
  const uint64_t lost = rec.counters().pathObservationSkippedAfterReentrantClose;
  std::printf("    [P0-44] reentered=%d servedRej=%u servedObs=%u lostPathFact=%llu\n",
              g_servedCloseReentered.load() ? 1 : 0, servedRej, servedObs,
              static_cast<unsigned long long>(lost));
  ok &= Require(g_servedCloseReentered.load(),
                "P0-44: the emitter must really re-enter CloseWindow on the reject chain's "
                "ServedCandidate (otherwise this case proves nothing)");
  ok &= Require(servedRej == 1u,
                "P0-44: the rejection-recovery chain must still receive its served fact");
  ok &= Require(servedObs == 0u,
                "P0-44: the observation chain must NOT receive it -- the nested close settled it first");
  ok &= Require(lost == 1u,
                "P0-6/C1-1: the second chain's path fact lost to a re-entrant close must be COUNTED "
                "(criterion = is it still settle-able, re-FindChain) -- the old m_windowClosed / "
                "nullptr-after-first-advance form dropped it silently with a zero counter");
  return ok;
}

// 2026-09-18 P0-6（复审 C1-2 的**载荷用例**）：预算只剩**一个**槽时的顺序与计损。
// C1 把顺序**显式固定**为「先拒绝恢复链、后观察链」⇒ 最后一个槽归拒绝链，
// 观察链那份事实经 AccountDrop **具名计入 droppedPerFrame**（上 wire，读方报
// objectLevelEvidenceDropped ⇒ chainMissing）——复审实测了这个**优先级反转**
//（旧 FindOwner 路由下观察链拿到最后一个槽），但当时**没有任何用例**把它钉住。
bool Case45DualChainLastBudgetSlotOrder() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Configure(&Collect, &g_contextMarker);
  rec.Reset(1100u, 2100u);
  ResetCollector();
  const uint32_t budget = PaletteObjectEvidence::kPerFrameBudget;
  // ① 双链对象本身占 2 个槽（Rejected + FirstSight）。
  const PaletteObjectKey dual = MakeKey(0x4500u);
  rec.NoteReject(dual, PaletteObjectRejectReason::R1, MakeFrames(950u));
  rec.NoteFirstSight(dual, MakeFrames(950u));
  // ② 用单链对象的 Rejected 把当帧预算填到**只剩一个槽**。
  for (uint32_t i = 0u; i + 3u <= budget - 1u; ++i)
    rec.NoteReject(MakeKey(0x46000u + i), PaletteObjectRejectReason::R1, MakeFrames(950u));
  const uint64_t droppedFrameBefore = rec.counters().droppedPerFrame;
  const uint32_t storedBefore = g_stored;
  // ③ 双链对象发一条 Served：拒绝链占最后一个槽；观察链那份被 AccountDrop（具名，不静默）。
  rec.NoteServed(dual, PaletteObjectSource::OwnedPartSnapshot, 9u, MakeFrames(950u));
  uint32_t servedRej = 0u, servedObs = 0u;
  for (uint32_t i = storedBefore; i < g_stored; ++i) {
    if (g_collected[i].stage != PaletteObjectStage::ServedCandidate ||
        !SameKeyFull(g_collected[i].key, dual))
      continue;
    if (g_collected[i].chainType == PaletteObjectChainType::Observation)
      servedObs++;
    else
      servedRej++;
  }
  const uint64_t droppedFrameDelta = rec.counters().droppedPerFrame - droppedFrameBefore;
  std::printf("    [P0-45] budget=%u servedRej=%u servedObs=%u droppedFrameDelta=%llu\n",
              budget, servedRej, servedObs,
              static_cast<unsigned long long>(droppedFrameDelta));
  ok &= Require(servedRej == 1u,
                "P0-6/C1: with exactly one slot left, the EXPLICIT order (rejection-recovery "
                "first, observation second) must give that slot to the rejection chain");
  ok &= Require(servedObs == 0u,
                "P0-6/C1: the observation chain's copy must NOT be emitted when no slot is left");
  ok &= Require(droppedFrameDelta == 1u,
                "P0-6/C1: the dropped second-chain fact must be charged to droppedPerFrame "
                "(wire-visible => reader reports objectLevelEvidenceDropped), never silently");
  return ok;
}

// 2026-09-18 P0-6（复审 266e35c4 **反例 5**）：ObjectGone 侧的**实例身份**判据载荷用例。
// 形状：两条链 → 在**拒绝链**的 ObjectGone 终态 emit 内同线程重入 `ResetForSessionTransition`
// 并 `NoteFirstSight(同键)` **重建一条新的观察链**（旧链被清表销毁）。
// 旧判据只证明「存在同键+同链型+同窗口的条目」⇒ 会把这个**在对象消失之后才建立**的新链
// 判为 ObjectGone 并发出终态（复审实测 obsTerm=1、objectGoneLostToReentrancy=0）。
// 新判据要求 `instanceId` 相等 ⇒ 新链不得被结算，且必须具名计损。
std::atomic<bool> g_goneRearmDone{false};
PaletteObjectKey g_goneRearmKey{};
void ReentrantGoneRearmEmitter(void* context, const PaletteObjectEventRecord& record) {
  Collect(context, record);
  if (record.terminal != PaletteObjectTerminal::ObjectGone)
    return;
  // 2026-09-18 P0-6（自我更正）：原写法 `if (… || !g_goneRearmDone.exchange(true)) return;` 是**错的** ——
  // `exchange` 返回**前值**，首次为 false ⇒ `!false` = true ⇒ **首次即返回、不做重臂**，
  // 重臂实际落在**第二个**同类事件上 ⇒ 实例判据从未被咨询（Case 46 同错）。
  // 注意：Case 43/44 的 `if (stage==S && !g.exchange(true)) 动作;` 形式是**对的**（首次即动作）。
  if (g_goneRearmDone.exchange(true))
    return;
  // 同线程重入：重开窗（清表，旧观察链无终态销毁）后**重建同名观察链**。
  // 注：本回调由发射路径同步调用；实测可安全调用记录器 API（未触发自死锁）。
  Recorder().ResetForSessionTransition(Recorder().sessionGeneration(), 0u);
  Recorder().NoteFirstSight(g_goneRearmKey, MakeFrames(860u));
}

bool Case47ObjectGoneRebuiltChainMustNotBeSettled() {
  bool ok = true;
  PaletteObjectEvidence& rec = Recorder();
  rec.Reset(190u, 160u);
  g_goneRearmKey = MakeKey(0x4700u);
  rec.NoteReject(g_goneRearmKey, PaletteObjectRejectReason::R1, MakeFrames(850u));
  rec.NoteFirstSight(g_goneRearmKey, MakeFrames(851u));
  g_goneRearmDone = false;
  rec.Configure(&ReentrantGoneRearmEmitter, &g_contextMarker);
  ResetCollector();
  rec.NoteObjectGone(g_goneRearmKey);
  rec.Configure(&Collect, &g_contextMarker);

  uint32_t rejTerm = 0u, obsTerm = 0u, obsFirstSight = 0u;
  uint32_t lastLiveFirstSight = 0u;
  bool sawLiveFirstSight = false;
  for (uint32_t i = 0u; i < g_stored; ++i) {
    const bool obs = g_collected[i].chainType == PaletteObjectChainType::Observation;
    if (obs && g_collected[i].stage == PaletteObjectStage::FirstSight) {
      obsFirstSight++;
      if (g_collected[i].terminal == PaletteObjectTerminal::None) {
        sawLiveFirstSight = true;
        lastLiveFirstSight = i;
      }
    }
    if (g_collected[i].terminal == PaletteObjectTerminal::None)
      continue;
    if (obs)
      obsTerm++;
    else
      rejTerm++;
  }
  // 重建链的 live 链首之后不得再出现终态（否则「对象已消失」被施加到一条在对象消失之后才建立的链上）。
  uint32_t terminalAfterRebuiltHead = 0u;
  if (sawLiveFirstSight) {
    for (uint32_t i = lastLiveFirstSight + 1u; i < g_stored; ++i)
      if (g_collected[i].terminal != PaletteObjectTerminal::None)
        terminalAfterRebuiltHead++;
  }

  std::printf("    [P0-47] rearmed=%d rejTerm=%u obsTerm=%u obsFirstSight=%u afterHeadTerm=%u "
              "lostToReentrancy=%llu watchCount=%u stored=%u\n",
              g_goneRearmDone.load() ? 1 : 0, rejTerm, obsTerm, obsFirstSight,
              terminalAfterRebuiltHead,
              static_cast<unsigned long long>(rec.counters().objectGoneLostToReentrancy),
              rec.watchCount(), g_stored);

  ok &= Require(g_goneRearmDone.load(),
                "P0-47: the emitter must really re-arm and rebuild the observation chain");
  // 自我更正后的期望：重臂发生在**拒绝链的 ObjectGone** emit 内 ⇒ ResetForSessionTransition 清表
  // 销毁**旧观察链**（它拿不到 ObjectGone），随后重建一条新链 ⇒ 观察侧只剩重建链的 **live 链首**。
  ok &= Require(obsFirstSight == 1u,
                "P0-47: only the REBUILT chain's live FirstSight head may remain on the observation "
                "side -- the old chain was destroyed by the re-arm (a second head would mean the "
                "re-arm landed on the WRONG event, e.g. the `|| !exchange(true)` early-return bug)");
  ok &= Require(obsTerm == 0u,
                "P0-6/266e35c4: the observation side must have NO terminal -- the old chain's "
                "ObjectGone is lost to the re-arm and the rebuilt chain must not be settled");
  ok &= Require(rec.counters().objectGoneLostToReentrancy == 1u,
                "P0-6/266e35c4: that loss must be named-counted (the criterion is the same chain "
                "INSTANCE; an existence-only criterion would settle the rebuilt chain instead)");
  ok &= Require(sawLiveFirstSight,
                "P0-47: the rebuilt chain's head must be LIVE (a rebuilt chain is not settled)");
  ok &= Require(terminalAfterRebuiltHead == 0u,
                "P0-6/266e35c4: no terminal may be recorded AFTER the rebuilt chain's live "
                "FirstSight head -- the rebuilt chain did not exist when the object disappeared");
  ok &= Require(rejTerm == 1u,
                "P0-47: the rejection-recovery chain must still get its ObjectGone terminal");
  ok &= Require(rec.watchCount() == 1u,
                "P0-47: after the re-arm only the rebuilt chain may remain in the table");
  return ok;
}

int main() {
  std::printf("war3_palette_object_evidence_test: host-side recorder contracts\n");
  std::printf("sizeof(PaletteObjectEvidence) = %llu bytes\n",
              static_cast<unsigned long long>(sizeof(PaletteObjectEvidence)));
  std::printf("kTotalBudget=%llu kTerminalReserve=%llu kNormalBudget=%llu kPerFrameBudget=%u "
              "kWatchCapacity=%u\n",
              static_cast<unsigned long long>(PaletteObjectEvidence::kTotalBudget),
              static_cast<unsigned long long>(PaletteObjectEvidence::kTerminalReserve),
              static_cast<unsigned long long>(PaletteObjectEvidence::kNormalBudget),
              static_cast<unsigned>(PaletteObjectEvidence::kPerFrameBudget),
              static_cast<unsigned>(PaletteObjectEvidence::kWatchCapacity));
  RunCase("1 default/unconfigured is inert", &Case1DefaultUnconfigured);
  RunCase("2 closed chain => Recovered + chainSequence relative +1", &Case2ClosedChain);
  RunCase("3 first alternative hit keeps the entry", &Case3FirstHitKeepsEntry);
  RunCase("4 incomplete chain => Unclosed", &Case4IncompleteChainUnclosed);
  RunCase("5 never caught => WindowExpired", &Case5WindowExpired);
  RunCase("6 object gone terminal", &Case6ObjectGone);
  RunCase("7 budgets (64/frame, 3584 normal, 4096 total, 512 reserve)", &Case7Budgets);
  RunCase("8 duplicate reject in one frame", &Case8DuplicateRejectPerFrame);
  RunCase("9 full watch table => TableFull", &Case9TableFull);
  RunCase("10 weak identity / unknown epoch visible", &Case10WeakIdentityVisible);
  RunCase("11 recorder performs zero heap allocation", &Case11ZeroHeapAllocation);
  RunCase("12 frame domains stay separate", &Case12FrameDomains);
  RunCase("13 tombstone keeps later entries reachable", &Case13TombstoneProbing);
  RunCase("14 CloseWindow closes the window (idempotent, Note* refused until Reset)",
          &Case14CloseWindowClosesTheWindow);
  RunCase("15 refused events must not mutate entry state", &Case15RefusalDoesNotMutateState);
  RunCase("16 same frame / same stage aggregation", &Case16SameFrameSameStageAggregation);
  RunCase("17 hitCount == ServedCandidate event count", &Case17HitCountMatchesServedEvents);
  RunCase("18 Recovered tightening counterexamples", &Case18RecoveredTightening);
  RunCase("19 actual source/hitKey passthrough per stage", &Case19ActualSourcePassthrough);
  RunCase("20 emitter-callback reentry into CloseWindow must not deadlock",
          &Case20ReentrantCloseFromEmitter);
  RunCase("21 capacity freeze during a palette emission is visible, not silent",
          &Case21CapacityFreezeDuringPaletteEmission);
  RunCase("22 same-frame Drawn payload conflict is visible (the 上级 counterexample)",
          &Case22DrawnPayloadConflictWitness);
  RunCase("23 window segment + carried identity proof kind (D2)",
          &Case23WindowSegmentAndIdentityProofKind);
  RunCase("24 first-sight observation chain vs reject-recovery chain",
          &Case24FirstSightObservationChain);
  RunCase("25 budget-refused reject must not create a headless entry",
          &Case25RejectBudgetOrderNoHeadlessEntry);
  RunCase("26 Recovered requires a real reject fact (K2)",
          &Case26RecoveredRequiresRejectFact);
  RunCase("27 same object holds two independent chains (Q2)",
          &Case27TwoIndependentChainsPerObject);
  RunCase("28 ownership rule: single-chain shapes (Q2)",
          &Case28OwnershipRuleSingleChainShapes);
  RunCase("29 budget-refused first sight must not insert (Q2)",
          &Case29FirstSightMustNotInsertWhenBudgetRefused);
  RunCase("30 diagnose second-attempt ordering (K3)", &Case30DiagnoseSecondAttemptOrdering);
  RunCase("31 attempt-scoped ordering (K3)", &Case31AttemptScopedOrdering);
  RunCase("33 factory carries attempt serial (K3)", &Case33FactoryCarriesAttemptSerial);
  RunCase("34 re-arm clears table (window precondition)", &Case34ReArmClearsTable);
  RunCase("35 diagnose both-chains served routing", &Case35BothChainsServedRouting);
  RunCase("36 stale-session admission boundary (P0-1)", &Case36StaleSessionRejected);
  RunCase("37 terminal stage backfill (P0-2)", &Case37TerminalStageBackfill);
  RunCase("38 ObjectGone both chains + no fabricated stage (P0-3)", &Case38ObjectGoneBothChains);
  RunCase("39 zero-generation key needs session-less recorder (P0-1 narrowed)", &Case39ZeroGenerationKeyNeedsSessionlessRecorder);
  RunCase("40 observation chain settles ObservationClosed, rejection chain WindowExpired (P0-4)", &Case40ObservationClosedVsWindowExpired);
  RunCase("41 ObjectGone reentrancy: no double terminal, no underflow (P0-3/C1)", &Case41ObjectGoneReentrancyNoDoubleTerminal);
  RunCase("42 silent-drop counters: no-chain path fact and post-close capture calls (P0-6)", &Case42SilentDropCounters);
  RunCase("43 ObjectGone: observation chain lost to a re-entrant close is counted (P0-3/C3)", &Case43ObjectGoneObservationLostToReentrantClose);
  RunCase("44 dual-chain path fact lost to a re-entrant close is counted (P0-6/C1-1)", &Case44DualChainReentrantCloseCountsLostPathFact);
  RunCase("45 dual-chain last budget slot: explicit order and named drop (P0-6/C1)", &Case45DualChainLastBudgetSlotOrder);
  RunCase("47 ObjectGone: a chain rebuilt during the re-entrant emit must not be settled (P0-6/266e35c4)", &Case47ObjectGoneRebuiltChainMustNotBeSettled);
  PrintObservations();
  // 任务 A-4：把总计数（含 D6 新计数 droppedPayloadConflict）打印在 SUMMARY 之前，便于取证。
  PrintCounterTotals();
  std::printf("SUMMARY: %u passed, %u failed\n",
              static_cast<unsigned>(g_casePassed), static_cast<unsigned>(g_caseFailed));
  return g_caseFailed == 0u ? 0 : 1;
}
