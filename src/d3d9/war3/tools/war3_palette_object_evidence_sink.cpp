#include "war3_palette_object_evidence_sink.h"

#include "war3_frame_evidence.h"

#include <windows.h>

#include <atomic>
#include <cstring>

// 冻结 wire（与 AutoTest/analyze_palette_object_evidence.py 的 docstring 逐位一致；
// 任何改动必须同时改读方，否则读方会按"保留位非零/未知枚举"直接报错）：
//   kind=ShadowState(12), label="palette-object/v1"
//   session -> key.sessionGeneration（并在 words32[10]/[29] 镜像）
//   frame -> frames.renderFrame；mapEpoch/deviceEpoch 直通；thread == words32[12]
//   data[0] renderablePart  data[1] runtimeModelPtr  data[2] lifecycleIdentity
//   data[4] chainType（2026-09-18 阶段 C：版本 4 声明的**记录级**链型载体；版本 ≤3 读方
//             仍要求该槽为保留零 ⇒ 旧产物合同一位不放宽）
//   data[3] windowSegment（2026-09-17 上级裁定 ⑧：版本 2 声明的**记录级**分段载体；版本 1 读方
//   仍把该槽当保留零，所以只有头块 version=2 的导出才按分段合同解析）  data[4..8] 必须为 0
//   data[9] manifestFrameSerial  data[10] source  data[11] hitKey
//   words32[0] rejectReason [1] jHandle [2] rawcode [3..4]=0 [5] stage [6..9]=0
//   [10] sessionGeneration lo32 [11]=0 [12] 记录线程 [13]=0 [14]bit0 identityWeak
//   [15] identityProofKind（2026-09-17 上级裁定 ⑦：版本 2 声明的身份证明种类；0 = 未载明，
//   只由记录器 DeriveIdentityProofKind() 推导，转换点**只读不写**）
//   [16]bit0 epochUnknown [17]=0 [18] terminal [19]=0 [20] deltaFrames lo32 [21] nativeFrameTag lo32
//   [22] hitCount [23] chainSequence [24..25] recordFrameSerial [26..27] manifestPublishRevision
//   [28] flags2(sawSubmit/sawDraw/selectionCleared/manifestUnknown/nativeUnknown) [29] sessionGeneration hi32
//   [30..31] firstRejectFrame [32] nativeFrameTag hi32 [33] deltaFrames hi32 [34..47]=0

namespace dxvk::war3::tools::evidence {
namespace {

PaletteObjectEvidence g_paletteObjectEvidence;
// 2026-09-18 阶段 D（批次 2）：**同步域**。原为裸 `bool`，而记录器的初始化是在它自己的
// `StateLock` 下完成的 ⇒ 写者（ArmPaletteObjectEvidence 末尾）与 7 个读者之间**没有
// acquire/release 配对** ⇒ 弱内存序下读者可能看到 armed==true 却看不到记录器已初始化。
// 改为 atomic + release/acquire 配对：armed 的发布**同时发布**记录器的初始化。
std::atomic<bool> g_paletteObjectArmed{false};
// 2026-09-18 独立复审 R3：这五个到达计数器由**渲染线程写、控制面客户线程读**，
// 原为普通 uint64_t 且无共同锁（记录器内部那把锁不保护它们）。改为原子，
// 读写一律 relaxed（只作诊断计数，不参与任何决策/发布判定）。
std::atomic<uint64_t> g_paletteObjectEnqueueBlockReached{0u};
std::atomic<uint64_t> g_paletteObjectAppendEntered{0u};
std::atomic<uint64_t> g_paletteObjectProductionInsertReached{0u};
std::atomic<uint64_t> g_paletteObjectProductionNoteCalled{0u};
std::atomic<uint64_t> g_paletteObjectResetOrClear{0u};

// 2026-09-18 P0-5（复审 C2；两位独立验证者都指出）：**发射路径不得无声丢弃**。
// 原实现里「会话未激活」与「编码失败」两条早退**没有任何计数** ⇒ 冻结/重入期间产生的事件
// 会静默消失（真实环路径实测：1 条嵌套 emit 无计数被丢弃，读方据此 emitted != exported ⇒ 整份不覆盖）。
// 与上面几个诊断原子同类：**内部计数、不上 wire**（补 wire 字段须抬版本）。
std::atomic<uint64_t> g_paletteObjectDroppedNoSession{0u};
std::atomic<uint64_t> g_paletteObjectEncodeFailed{0u};

uint32_t Low32(uint64_t value) noexcept {
  return static_cast<uint32_t>(value & 0xFFFFFFFFu);
}
uint32_t High32(uint64_t value) noexcept {
  return static_cast<uint32_t>((value >> 32u) & 0xFFFFFFFFu);
}

// 子门开启但会话未激活时不得发射（fail-closed，不得留下不可判读的孤事件）。
void EmitPaletteObjectEvent(void* /*context*/,
                            const PaletteObjectEventRecord& record) noexcept {
  if (!PaletteObjectEvidenceEnabled())
    return;
  const uint64_t session = ActiveSession();
  if (session == 0u) {
    // C2：不得无声返回 —— 这正对应冻结/重入期间「记录器已发射、环却不可用」的事件。
    g_paletteObjectDroppedNoSession.fetch_add(1u, std::memory_order_relaxed);
    return;
  }
  Event event{};
  if (!EncodePaletteObjectEvent(record, session, event)) {
    g_paletteObjectEncodeFailed.fetch_add(1u, std::memory_order_relaxed);
    return;
  }
  // 2026-09-17 往返测试暴露的 G4 后半：环已冻结/未激活时 append 返回 0，
  // 此时**必须落一个可见的丢失计数**（否则事件静默消失、导出缺链却看不出原因）。
  if (Record(session, event) == 0u)
    g_paletteObjectEvidence.NoteRingEviction(1u);
}

} // namespace

// 唯一的转换点：记录 -> 冻结 wire 的 evidence::Event（纯函数，生产与往返测试共用）。
bool EncodePaletteObjectEvent(const PaletteObjectEventRecord& record,
                              uint64_t session, Event& out) noexcept {
  if (session == 0u)
    return false;
  Event event{};
  event.session = session;
  event.key.frame = record.frames.renderFrame;
  event.key.mapEpoch = record.key.mapEpoch;
  event.key.deviceEpoch = record.key.deviceEpoch;
  // key.owner 保持 device / record 上下文语义：上级明确不得把 part 指针塞进 owner。
  event.key.owner = 0u;
  event.kind = Kind::ShadowState;
  std::memcpy(event.label.data(), "palette-object/v1", 18);
  event.data[0] = record.key.renderablePart;
  event.data[1] = record.key.runtimeModelPtr;
  // 上级 03:58 要求：生命周期身份必须**贯通 wire**（读方按 data[2] 解码并纳入分组键）；
  // 当前采集点一律 0 + identityWeak=true，槽位存在只为将来有可靠实例生命周期证明时使用。
  event.data[2] = record.key.lifecycleIdentity;
  // 2026-09-17 上级裁定 ⑧：窗口/Reset 分段量（记录级，发出时写一次；版本 2 的正式语义）。
  event.data[3] = record.windowSegment;
  // 2026-09-18 阶段 C：**链型**。v4 起 data[4] 从保留零改为链型载体
  // （0 = RejectionRecovery，1 = Observation）；v1/v2/v3 的读方仍要求它为 0。
  // 本槽由 MakeRecord 从**条目建立时落定**的 chainType 盖章，调用点不得给值。
  event.data[4] = static_cast<uint32_t>(record.chainType);
  event.data[9] = record.frames.manifestFrameSerial;
  event.data[10] = static_cast<uint64_t>(record.source);
  event.data[11] = record.hitKey;
  event.bits[0] = static_cast<uint32_t>(record.rejectReason);
  event.bits[1] = Low32(record.key.jHandle);
  event.bits[2] = Low32(record.key.rawcode);
  event.bits[5] = static_cast<uint32_t>(record.stage);
  event.bits[10] = Low32(record.key.sessionGeneration);
  event.bits[12] = static_cast<uint32_t>(::GetCurrentThreadId());
  event.bits[14] = record.key.identityWeak ? 1u : 0u;
  // 2026-09-17 上级裁定 ⑦：载明的身份证明种类（0 = 未载明；版本 2 的正式语义）。
  // 转换点只把记录器已经推导好的值搬到 wire 上，**不得**在这里重新推导或改写。
  event.bits[15] = static_cast<uint32_t>(record.identityProofKind);
  event.bits[16] = record.key.epochUnknown ? 1u : 0u;
  event.bits[18] = static_cast<uint32_t>(record.terminal);
  event.bits[20] = Low32(record.deltaFrames);
  event.bits[21] = Low32(record.frames.nativeFrameTag);
  event.bits[22] = record.hitCount;
  event.bits[23] = record.chainSequence;
  event.bits[24] = Low32(record.frames.recordFrameSerial);
  event.bits[25] = High32(record.frames.recordFrameSerial);
  event.bits[26] = Low32(record.frames.manifestPublishRevision);
  event.bits[27] = High32(record.frames.manifestPublishRevision);
  uint32_t flags2 = 0u;
  if (record.sawSubmit)
    flags2 |= 1u;
  if (record.sawDraw)
    flags2 |= 2u;
  if (record.selectionClearedByNativeOverride)
    flags2 |= 4u;
  if (record.frames.manifestUnknown)
    flags2 |= 8u;
  if (record.frames.nativeUnknown)
    flags2 |= 16u;
  event.bits[28] = flags2;
  event.bits[29] = High32(record.key.sessionGeneration);
  event.bits[30] = Low32(record.firstRejectFrame);
  event.bits[31] = High32(record.firstRejectFrame);
  event.bits[32] = High32(record.frames.nativeFrameTag);
  event.bits[33] = High32(record.deltaFrames);
  out = event;
  return true;
}

void ClosePaletteObjectWindow() noexcept {
  if (!g_paletteObjectArmed.load(std::memory_order_acquire))
    return;
  g_paletteObjectEvidence.CloseWindow(g_paletteObjectEvidence.currentFrame());
}

void PaletteObjectPreFreezeHook() noexcept {
  // 自动冻结（post-window / 容量 / 序列回绕）同样必须先结算终态，否则终态进不了环。
  ClosePaletteObjectWindow();
}

void ClearPaletteObjectWatchlist() noexcept {
  if (!g_paletteObjectArmed.load(std::memory_order_acquire))
    return;
  g_paletteObjectResetOrClear.fetch_add(1u, std::memory_order_relaxed);
  g_paletteObjectEvidence.ResetForSessionTransition(ActiveSession(),
                                g_paletteObjectEvidence.mapEpoch());
}

PaletteObjectEvidence& PaletteObjectRecorder() noexcept {
  return g_paletteObjectEvidence;
}

void ArmPaletteObjectEvidence(uint64_t sessionGeneration,
                              uint64_t mapEpoch) noexcept {
  if (!PaletteObjectEvidenceEnabled())
    return;
  g_paletteObjectResetOrClear.fetch_add(1u, std::memory_order_relaxed);
  g_paletteObjectEvidence.Configure(&EmitPaletteObjectEvent, nullptr);
  // 新会话：**连计数一起清**（换图中的 transition 变体只清表、保留会话计数）。
  g_paletteObjectEvidence.Reset(sessionGeneration, mapEpoch);
  g_paletteObjectArmed.store(true, std::memory_order_release);
}

void DisarmPaletteObjectEvidence() noexcept {
  g_paletteObjectArmed.store(false, std::memory_order_release);
}

// 2026-09-18 最小诊断：入队块到达计数与 arm 状态（只读）。
// 2026-09-18 独立复审 R3：全部改为原子 fetch_add/load（relaxed）；
// 调用点一律位于子门/armed 判断之内，关闭诊断时不产生任何写入。
void NotePaletteObjectEnqueueBlockReached() noexcept {
  if (!g_paletteObjectArmed.load(std::memory_order_acquire))
    return;
  g_paletteObjectEnqueueBlockReached.fetch_add(1u, std::memory_order_relaxed);
}
bool PaletteObjectEvidenceArmed() noexcept {
  return g_paletteObjectArmed.load(std::memory_order_acquire);
}
uint64_t PaletteObjectEnqueueBlockReachedCount() noexcept {
  return g_paletteObjectEnqueueBlockReached.load(std::memory_order_relaxed);
}
uint64_t PaletteObjectDroppedNoSessionCount() noexcept {
  return g_paletteObjectDroppedNoSession.load(std::memory_order_relaxed);
}
uint64_t PaletteObjectEncodeFailedCount() noexcept {
  return g_paletteObjectEncodeFailed.load(std::memory_order_relaxed);
}
void NotePaletteObjectAppendEntered() noexcept {
  if (!g_paletteObjectArmed.load(std::memory_order_acquire))
    return;
  g_paletteObjectAppendEntered.fetch_add(1u, std::memory_order_relaxed);
}
uint64_t PaletteObjectAppendEnteredCount() noexcept {
  return g_paletteObjectAppendEntered.load(std::memory_order_relaxed);
}
void NotePaletteObjectProductionInsertReached() noexcept {
  if (!PaletteObjectEvidenceEnabled())
    return;
  g_paletteObjectProductionInsertReached.fetch_add(1u, std::memory_order_relaxed);
}
uint64_t PaletteObjectProductionInsertReachedCount() noexcept {
  return g_paletteObjectProductionInsertReached.load(std::memory_order_relaxed);
}
void NotePaletteObjectProductionNoteCalled() noexcept {
  g_paletteObjectProductionNoteCalled.fetch_add(1u, std::memory_order_relaxed);
}
uint64_t PaletteObjectProductionNoteCalledCount() noexcept {
  return g_paletteObjectProductionNoteCalled.load(std::memory_order_relaxed);
}
void NotePaletteObjectResetOrClear() noexcept {
  g_paletteObjectResetOrClear.fetch_add(1u, std::memory_order_relaxed);
}
uint64_t PaletteObjectResetOrClearCount() noexcept {
  return g_paletteObjectResetOrClear.load(std::memory_order_relaxed);
}

void ResetPaletteObjectEvidence(uint64_t sessionGeneration,
                                uint64_t mapEpoch) noexcept {
  if (!g_paletteObjectArmed.load(std::memory_order_acquire))
    return;
  g_paletteObjectResetOrClear.fetch_add(1u, std::memory_order_relaxed);
  g_paletteObjectEvidence.ResetForSessionTransition(sessionGeneration, mapEpoch);
}

void QueryPaletteObjectEvidenceHeader(PaletteObjectEvidenceHeader& out) noexcept {
  if (!g_paletteObjectArmed.load(std::memory_order_acquire)) {
    out.watchCount = 0u;
    out.counters = PaletteObjectEvidence::Counters{};
    return;
  }
  // 导出头块必须来自**同一个一致快照**（计数 + 观察数在同一次持锁读取中取得）。
  g_paletteObjectEvidence.SnapshotCounters(out.counters, out.watchCount);
}

} // namespace dxvk::war3::tools::evidence
