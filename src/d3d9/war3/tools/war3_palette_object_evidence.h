#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

// 2026-09-17 上级裁定（codex 01a02e0b）——对象级 palette 证据的**有界**记录器。
//
// 铁律（逐条来自上级裁定）：
//  1) 默认关闭、受主门约束（子门见 RecorderConfiguration::paletteObject）；关闭时零查询/扫描/锁/日志/记录器操作。
//  2) **字段更多 ≠ 强身份**：S/C/D 在没有可靠对象实例生命周期证明时同样必须保留弱身份限制。
//  3) **取到 palette ≠ 阴影恢复**；**区间重叠 ≠ 错误矩阵被消费**；CPU 阶段链闭合与画面恢复必须分开。
//  4) S 是 CPU caster **候选入队**（不是 GPU 提交完成）；D 是**绘制命令已记录**（不是 GPU 执行/像素正确性）。
//  5) 必须记录**最终实际选中/实际 draw 携带的按值来源**（不得由最近一次 Served 推测）；
//     native override 清空 inputSkinSelection 时记录空值 + 原因，**不得补回**早先尝试值。
//  6) 总预算 **4096 条/会话**，其中 **512 条为终态真实预留**（普通事件不得吃满 4096）。
//  7) 首次替代命中**只推进状态**，不得立即删表；条目只在窗口结束（CloseWindow）、ObjectGone 或 Reset 时结算。
//  8) 身份/帧域未知必须显式标记并可见；未知不是通配符。
//  9) **同对象同帧同阶段最多一条事件**（阶段是不同事实，各自保留）；被合并者计入 droppedDuplicatePerFrame，
//     绝不为凑限额丢掉必需阶段却宣称完整。
//
// 本组件是**纯状态机 + 定长表**（无堆分配、无锁、无 IO），生产与宿主机测试共用；
// 事件发射通过函数指针回调（生产侧转成 evidence::Event，Kind::ShadowState + label "palette-object/v1"）。

namespace dxvk::war3::tools::evidence {

// 对象键（八元组）。identityWeak/epochUnknown 为**必带**标记，用于单列比例；不得用 0 冒充「已知且为零」。
struct PaletteObjectKey {
  uint64_t renderablePart = 0u;
  uint64_t runtimeModelPtr = 0u;
  uint64_t jHandle = 0u;
  uint64_t rawcode = 0u;
  uint64_t sessionGeneration = 0u;
  uint64_t mapEpoch = 0u;
  uint64_t deviceEpoch = 0u;
  uint64_t lifecycleIdentity = 0u;
  bool identityWeak = true;
  bool epochUnknown = true;
};

// 四个帧域**分列**（不得用一个替代另一个）。
struct PaletteObjectFrames {
  uint64_t renderFrame = 0u;
  uint64_t manifestFrameSerial = 0u;
  uint64_t manifestPublishRevision = 0u;
  uint64_t recordFrameSerial = 0u;
  uint64_t nativeFrameTag = 0u;
  bool manifestUnknown = true;
  bool nativeUnknown = true;
  // 2026-09-18 阶段 C（K3）：**一次明确关联的尝试**的序号（按值携带，裁定要求）。
  // 哨兵 `~0ull` = **未知**：本事件无法归属到某一次尝试。
  // ⚠️ 默认未知 ⇒ MarkStage 保持**既有的终身单调判序** ⇒ 生产行为一位不变；
  // 只有调用方**显式**给出已知尝试号时，判序才改为「尝试内单调」。
  // 为什么必须有未知哨兵：若无未知值，实现者只能用 0 冒充，而 0 会与
  // 「第一次尝试」混淆 ⇒ MarkStage 会据此**错误重置**。
  uint64_t attemptSerial = ~0ull;
};

enum class PaletteObjectStage : uint32_t {
  Rejected = 1u,        // A 链拒绝（含具名原因）
  ServedCandidate = 2u, // 替代来源供出 palette（**只推进状态**）
  Enqueued = 3u,        // CPU caster 候选入队（非 GPU 提交完成）
  Drawn = 4u,           // 绘制命令已记录（非 GPU 执行/像素证明）
  // 2026-09-18 独立复审批次 3：**正常观察链**的首阶段（wire 版本 3 专属）。
  // 语义 = 该对象作为 CPU caster 候选**首次进入观察**：它**不是**一次拒绝，
  // 也**不得**被读方解释成「已恢复」。取值 5 是**故意**取的：版本 1/2 读方
  // 的阶段表不含它，因而会直接拒绝整份导出，而不是静默忽略（fail-visible）。
  //
  // ⚠️ 2026-09-18 补记（这句话曾**有一段时间是假的**，别让它再悄悄失真）：
  //   读方原先的 `STAGES` 是**版本无关**的，于是 v1/v2 载荷里出现 stage=5 时只是被加一个
  //   软标记、**并不拒绝整份导出** —— 与上面这句话相反（独立对抗性审计以此判"③不成立"）。
  //   现已补回版本门控：`AutoTest/analyze_palette_object_evidence.py` 增加 `LEGACY_STAGES`
  //   （只含 1..4），并按 `version >= PALETTE_OBJECT_FIRST_SIGHT_VERSION` **选表**，
  //   因此 v1/v2 遇 stage=5 会在 `lookup()` 的 `require` 抛 `ValueError: unknown palette stage 5`。
  //   该行为由 `test_palette_object_evidence_analysis_static.py` 的
  //   `test_first_sight_stage_is_rejected_wholesale_under_legacy_version` **锁住**
  //   （对 v∈{None,1,2} 断言整份拒绝、对 v=3 断言接受 ⇒ 钉住的是"版本门控"而非"一律拒绝"）。
  // 顺序上它排在 Rejected **之前** —— 见 StageRank()。
  FirstSight = 5u,
};
enum class PaletteObjectSource : uint32_t {
  None = 0u, ArenaSlot = 1u, ProducerSnapshot = 2u, PoseKernel = 3u,
  DrawTimeCaptured = 4u, PublishedRegistry = 5u, OwnedPartSnapshot = 6u, Unknown = 7u,
};
enum class PaletteObjectTerminal : uint32_t {
  None = 0u, Recovered = 1u, WindowExpired = 2u, ObjectGone = 3u,
  TableFull = 4u, EventLost = 5u, Unclosed = 6u,
  // 2026-09-18 阶段 C（复核 Q3）：**Observation 链的结算终态**，替代 `Unclosed`。
  // 含义**仅**为「观察已结算」：**不表示**阶段齐全、**不表示**身份已证明、
  // **不表示** palette 已消费、更**不表示**画面已恢复。
  // 取 7：0..6 一位不动；v1/v2/v3 读方的终态表不含 7 ⇒ 遇到即整份拒绝。
// 2026-09-18 P0-4 修正：**本注释此前与实现相反**（它写「WindowExpired 先于本终态判定」，
// 而实现已按裁定改为观察链优先）。现行顺序见 CloseWindow：closedChain ⇒ Recovered；
// chainType == Observation ⇒ ObservationClosed；否则 hitCount==0 && !sawServed ⇒ WindowExpired。
// 「仅有链首后关闭的观察链」现在是 ObservationClosed（诚实报告缺绘制证据），不再是 WindowExpired。
  ObservationClosed = 7u,
};
enum class PaletteObjectRejectReason : uint32_t {
  R0 = 0u, R1 = 1u, R2 = 2u, R3 = 3u,
  NotChecked = 0xFEu, // 该检查未执行（**不得**用 R0 冒充）
  Unknown = 0xFFu,
};

// 2026-09-17 上级裁定 ⑦：**载明的**身份证明种类（版本 2 的 wire 载体 words32[15]）。
// 必须与读方 AutoTest/analyze_palette_object_evidence.py 的 IDENTITY_PROOF_KINDS 登记表逐项一致
// （0 = NoIdentityProof，1 = InstanceLifecycleIdentityProof）。**字段存在 ≠ 身份已证明**：
// 只有 PaletteObjectEvidence::DeriveIdentityProofKind() 这一处推导规则能产生非零值，
// 任何采集点都不得直接写入（不得用 identityWeak=false 冒充证明，也不加 setter 后门）。
enum class PaletteObjectIdentityProofKind : uint32_t {
  NoIdentityProof = 0u,
  InstanceLifecycleIdentityProof = 1u,
};

// 2026-09-18 阶段 C（复核 D 定案）：诊断链的**类型**。
// 两类链拥有**不同的事实依据**，因此互不冒充、互不改类：
//   · RejectionRecovery —— 依据「真的发生了拒绝」（NoteReject 建立）；
//   · Observation       —— 依据「真的观测到了 caster 候选」（NoteFirstSight 建立）。
// 两者都是事实，故都不是伪造；但一条事实链不得因为另一类事件到达而改变自己的类型。
// 位置说明：完整定义必须**早于**记录结构（PaletteObjectEventRecord 携带链型且带默认值）。
enum class PaletteObjectChainType : uint32_t {
  RejectionRecovery = 0u,
  Observation = 1u,
};
struct PaletteObjectEventRecord {
  PaletteObjectStage stage = PaletteObjectStage::Rejected;
  PaletteObjectTerminal terminal = PaletteObjectTerminal::None;
  PaletteObjectRejectReason rejectReason = PaletteObjectRejectReason::NotChecked;
  PaletteObjectSource source = PaletteObjectSource::None;
  PaletteObjectKey key{};
  PaletteObjectFrames frames{};
  uint64_t hitKey = 0u;
  uint64_t firstRejectFrame = 0u;
  // **仅终态**携带：该对象从首次拒绝到结算的帧跨度；非终态事件恒 0（冻结读方契约）。
  uint64_t deltaFrames = 0u;
  uint32_t hitCount = 0u;
  uint32_t chainSequence = 0u;
  // 2026-09-18 阶段 C（Q2）：事件的**链型**（MakeRecord 盖章）。
  // **已在 wire 上**：编码槽位 `data[4]`（sink.cpp 的 `event.data[4] = record.chainType`）
  // 与读方分组键均已落地，并由宿主机用例 27 / 28 / 35 覆盖。
  PaletteObjectChainType chainType = PaletteObjectChainType::RejectionRecovery;
  bool sawSubmit = false;
  bool sawDraw = false;
  // native draw-time override 清空 inputSkinSelection 时为 true（记录空值 + 原因，不补回早先值）。
  bool selectionClearedByNativeOverride = false;
  // 2026-09-17 上级裁定 ⑧：**记录级**窗口/Reset 分段量（发出时写一次；冻结后不再变化）。
  // 版本 2 的 wire 载体是 data[3]；每条必须 >= 1 且整份导出非递减。清表 ≠ 分段：
  // Reset 开第 1 个窗口，ResetForSessionTransition 递增（见两个 Reset 的注释）。
  uint64_t windowSegment = 1u;
  // 2026-09-17 上级裁定 ⑦：**载明的**身份证明种类（0 = NoIdentityProof）。只由
  // DeriveIdentityProofKind() 按「非零实例生命周期身份 + !identityWeak + !epochUnknown」推导；
  // 三个生产采集点都载不明实例生命周期证明 ⇒ 生产恒为 NoIdentityProof(0)。
  uint32_t identityProofKind = 0u;
};

class PaletteObjectEvidence {
 public:
  static constexpr uint32_t kWatchCapacity = 1024u;
  static constexpr uint64_t kTotalBudget = 4096u;
  static constexpr uint64_t kTerminalReserve = 512u;
  // 审计 D7："从未观测"必须与"帧 0"区分开 —— 帧标记用哨兵而不是 0，
  // 否则 renderFrame==0 的首条观测会被同帧去重吞掉（且不落任何 LOSS 计数）。
  static constexpr uint64_t kNoFrame = ~0ull;
  static constexpr uint32_t kPerFrameBudget = 64u;
  // 普通事件的真实上限：终态预留**从总额中扣出**，普通过程不得吃满 4096。
  static constexpr uint64_t kNormalBudget = kTotalBudget - kTerminalReserve;

  using EmitFn = void (*)(void* context, const PaletteObjectEventRecord& record);

  struct Counters {
    uint64_t emitted = 0u;
    uint64_t terminalEmitted = 0u;
    uint64_t droppedPerFrame = 0u;
    uint64_t droppedPerSession = 0u;
    uint64_t droppedTableFull = 0u;
    uint64_t droppedProbeLimit = 0u;
    uint64_t droppedDuplicatePerFrame = 0u;
    // D6（上级 2026-09-17 裁定）：同键同帧同阶段但**载荷冲突**（来源/hitKey/清空标志不同）
    // ⇒ 不得当作「已证明等价的重复」压缩掉，必须计数（缺链必须可见）。
    uint64_t droppedPayloadConflict = 0u;
    // 终态预留（512）耗尽而未能写入的终态数：必须可见（不得静默丢弃）。
    uint64_t droppedTerminalReserve = 0u;
    uint64_t ringEvictedAfterRecord = 0u; // 由上层在证据环淘汰时累加（缺链判定用）
    uint64_t closedRecovered = 0u;
    uint64_t closedWindowExpired = 0u;
    uint64_t closedObjectGone = 0u;
    uint64_t closedTableFull = 0u;
    uint64_t closedEventLost = 0u;
    uint64_t closedUnclosed = 0u;
    // 2026-09-18 阶段 C（窗口维度）：查找时遇到"同 key+链型 但别的窗口"的次数。
    // **故意不写入 wire**（裁定要求新写方一律 v4；加计数器会触版本表）。
    uint64_t windowMismatchLookups = 0u;
    // 2026-09-18 P0-1：因**会话代际不匹配**（含初始化前）被拒绝接纳的事件数。
    // 与 windowMismatchLookups 同理：**内部计数、不上 wire**（加 wire 字段会触版本表）。
    uint64_t droppedStaleSession = 0u;
    // 2026-09-18 P0-3（复审 R4）：因第一条终态的 emit 触发嵌套结算/冻结而**丢失**的
    // 第二条链 ObjectGone 条数。同样**内部计数、不上 wire**（补 wire 字段须抬版本）。
    uint64_t objectGoneLostToReentrancy = 0u;
    // 2026-09-18 P0-6（C1）：第一条链的 emit 触发嵌套结算/冻结后，**第二条链的路径事实被跳过**的次数。
    // 与其它内部计数同理：不上 wire。宁可具名丢失，也不静默（本项目的最高纪律）。
    uint64_t pathObservationSkippedAfterReentrantClose = 0u;
    // 2026-09-18 P0-6（复审 41e85203 的**丢弃路径清单**）：两个仍然无声的丢弃类 ⇒ 具名计数。
    // (a) 窗口已关闭之后到达的采集调用（6 个入口各一处早退）：冻结/重入期间可达。
    //     ⚠️ 注意 `CloseWindow` 自己的 `if (m_windowClosed) return;` 是**幂等返回**，不是丢弃，不计。
    // (b) 一条路径事实（S/E/D）在**两条链都不存在**时无处可记。
    //     复审推断可达：FirstSight 用 `runtimeModelPtr=nullptr` 造键，而 S/D 用
    //     `draw.shadowRuntimeModelPtr` 造键，`SameKey` 要求八字段全等 ⇒ 键不等时事件会无声消失。
    //     与其它内部计数同理：不上 wire（补 wire 字段须抬版本）。
    uint64_t droppedWindowClosedEntry = 0u;
    uint64_t droppedNoChainForPathFact = 0u;
    // 2026-09-18 阶段 C：ObservationClosed 的**独立**结算桶。
    // 不得让它落进 closedUnclosed —— 那是一个**错误的标签**（它恰恰不是 unclosed）。
    uint64_t closedObservationClosed = 0u;
    uint64_t weakIdentityRecords = 0u;
    uint64_t epochUnknownRecords = 0u;
    // 2026-09-18 批次 3（正常观察链）：首见**建立新条目**的次数（正常链是否真被建立）
    // 与首见事件**真正发射**的次数。二者分离，才能区分「采样到了但被预算拒」与「根本没采样到」。
    uint64_t firstSightInserted = 0u;
    uint64_t firstSightEmitted = 0u;
  };

  // 并发所有者协议（2026-09-17 审计 S1 裁定）：R（CPU 构建/入队路径）、S（设备侧候选入队）、
  // D（渲染命令路径）与控制面（arm/freeze/export/discard）可能来自不同线程，而记录器原先对
  // 观察表/预算/链状态**没有任何同步**。这里用一把**递归**互斥量（std::recursive_mutex）把全部*状态*访问串行化 —— 递归是**必需的**：
 // 发射回调（sink）会在持锁路径内同线程重入记录器（Case 20：环预冻结钩子回调 CloseWindow），
 // 非递归锁会在那里自死锁。
  //  - 采集点与 Reset / CloseWindow 互斥：在途观察要么在关闭前完成、要么在窗口关闭后早退；
  //  - 关闭协议 = 持锁停止接纳（m_windowClosed）→ 持锁结算终态（发射）→ 上层随后才冻结证据环；
  //  - 该锁只在诊断路径上（子门默认关，关闭时采集点第一条语句即短路），因此不在生产热路径。
  class StateLock {
   public:
    explicit StateLock(const PaletteObjectEvidence& owner) : m_owner(owner) {
      m_owner.m_mutex.lock();
    }
    ~StateLock() { m_owner.m_mutex.unlock(); }
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;

   private:
    const PaletteObjectEvidence& m_owner;
  };

  void Configure(EmitFn emit, void* context) {
    StateLock guard(*this);
    m_emit = emit;
    m_context = context;
  }

  // 会话/地图切换：清空并重置预算与计数（**代际来自既有采集会话/地图隔离机制**）。
  void Reset(uint64_t sessionGeneration, uint64_t mapEpoch) {
    StateLock guard(*this);
    // 2026-09-17 上级裁定 ⑧：新会话 = 第 1 个窗口。清表**并且**开新窗口（两者不是一回事）。
    m_windowSegment = 1u;
    for (uint32_t i = 0u; i < kWatchCapacity; ++i)
      m_entries[i] = Entry{};
    for (uint32_t i = 0u; i < kBloomWords; ++i)
      m_bloom[i] = 0ull;
    m_watchCount = 0u;
    m_frameBudgetUsed = 0u;
    m_currentFrame = 0u;
    m_sessionGeneration = sessionGeneration;
    m_mapEpoch = mapEpoch;
    m_counters = Counters{};
    m_ringLosses.store(0u, std::memory_order_relaxed);
    m_windowClosed = false;
    m_tableFullAnnounced = false;
  }

  // 审计 D9：换图 / 设备代际切换（在会话中途）只清**观察表**，**保留会话计数** ——
  // 否则 freeze→export 窗口内发生换图会让导出头块归零，"导出期间头块必须可读"不成立。
  // 新会话（arm）必须用上面的 Reset（连计数一起清）。
  void ResetForSessionTransition(uint64_t sessionGeneration, uint64_t mapEpoch) {
    StateLock guard(*this);
    // 2026-09-17 上级裁定 ⑧：清表 ≠ 分段 —— 换图 / 设备代际 / 显式清表在这里同时**开启一个新窗口**
    // （递增，不回绕到 1）。否则同会话同地图且 deviceEpoch 未知时，Reset 前后的记录八元组可以完全
    // 相同，离线分组会把两个窗口的记录合成一条链。
    ++m_windowSegment;
    for (uint32_t i = 0u; i < kWatchCapacity; ++i)
      m_entries[i] = Entry{};
    for (uint32_t i = 0u; i < kBloomWords; ++i)
      m_bloom[i] = 0ull;
    m_watchCount = 0u;
    m_frameBudgetUsed = 0u;
    m_currentFrame = 0u;
    m_sessionGeneration = sessionGeneration;
    m_mapEpoch = mapEpoch;
    m_windowClosed = false;
    m_tableFullAnnounced = false;
  }

  // 上层在证据环淘汰/丢失时累加（缺链必须可见）。
  // **无锁**：发射器是在 EmitUnchecked 内（即已持 StateLock 的同一线程）回调进来的，
  // 若这里再取同一把不可重入的锁就会自死锁；因此它只做一次原子累加。
  void NoteRingEviction(uint64_t count) {
    m_ringLosses.fetch_add(count, std::memory_order_relaxed);
  }

  // 窗口结束：为仍留在表内的条目结算终态。
  // **Recovered 的判定必须完整**：拒绝 + 替代命中 + 候选入队 + 绘制命令已记录，
  // 顺序无违规，且该次绘制**没有**因 native override 清空语义 palette（否则本次 draw 并未消费语义 palette）。
  void CloseWindow(uint64_t frame) {
    StateLock guard(*this);
    if (m_windowClosed)
      return; // 幂等：窗口只结算一次
    m_windowClosed = true;
    for (uint32_t i = 0u; i < kWatchCapacity; ++i) {
      Entry& e = m_entries[i];
      if (!e.used)
        continue;
      PaletteObjectTerminal terminal = PaletteObjectTerminal::Unclosed;
      // 2026-09-18 阶段 C（复核 K2 定案）：`Recovered` 的语义是「**拒绝之后又被接住**」，
      // 因此它必须由**真实拒绝事实**支撑（`firstReason != NotChecked`）。
      // 原条件里**没有任何拒绝项** ⇒ 一条**从未被拒绝过**的链只要走到 S/E/D 就会被标成
      // Recovered —— 而「恢复」恰恰预设了「先失去」。观察链（由 NoteFirstSight 建立，
      // firstReason=NotChecked）因此**永不**可以使用 Recovered。
      // 这不是收紧既有拒绝恢复链的行为：由 NoteReject 建立的条目其 firstReason 恒为
      // 真实拒绝原因，故它们的判定一位不变。
      const bool hasRejectFact =
          e.firstReason != PaletteObjectRejectReason::NotChecked;
      const bool closedChain = hasRejectFact && e.sawServed && e.sawSubmit &&
                               e.sawDraw && !e.orderViolation &&
                               !e.drawSelectionCleared &&
                               e.drawSource != PaletteObjectSource::None;
      if (closedChain)
        terminal = PaletteObjectTerminal::Recovered;
      // 2026-09-18 P0-4（Astra 裁定修正）：**观察链优先**。原顺序让 `hitCount == 0 && !sawServed`
      // 先落 WindowExpired ⇒ 只走到链首的观察链被结算成「窗口过期」，而那是在**借用拒绝链的
      // 含义**（该对象从未被拒绝）。裁定明确：「仅有链首后关闭，也应使用 ObservationClosed，
      // 同时诚实报告没有完整绘制证据」。生产 d3d9_device.cpp:23539-23548 **故意不发 Served**
      //（无选中的 palette 不得凭空造事实）⇒ FirstSight→Enqueued→Close 这类真实序列此前被误判。
      // 旧 Case 24 的期望是**基于该错误顺序**写下的，已随本裁定有意更新（这是修契约，不是放宽判据）。
      else if (e.chainType == PaletteObjectChainType::Observation)
        terminal = PaletteObjectTerminal::ObservationClosed;
// WindowExpired 现在**只在拒绝恢复链**上成立（`chainType == RejectionRecovery`）。
// 注意措辞：它**不**声称「必然发生过具名拒绝」—— `NoteReject(NotChecked)` 的条目同样落此桶（复审 #6，
// 当前生产不可达，因为生产拒绝点经 ShouldNotifyPaletteSlotReject 拒绝 NotChecked/Unknown）。
// 「拒绝事实被预算吞掉后链型静默退化成观察」这一可观测缺口见 P0-4 记录的反例 A，**尚未修**。
      else if (e.hitCount == 0u && !e.sawServed)
        terminal = PaletteObjectTerminal::WindowExpired;
      PaletteObjectEventRecord record = MakeRecord(e);
      // 2026-09-18 批次 3：终态记录原先**无条件**写 stage=Drawn —— 对只推进到入队的
      // 首见链，那等于把「候选已入队」谎报成「绘制命令已记录」。此处按该条目**实际
      // 达到的最高语义秩**回填；但为遵守「旧版本含义不变」，**只对首见链**启用，
      // 拒绝恢复链（版本 1/2 冻结行为）仍写 Drawn，一位不改。
      record.stage = e.sawFirstSight ? StageOfRank(e.maxStage)
                                     : PaletteObjectStage::Drawn;
      record.terminal = terminal;
      record.frames.renderFrame = frame;
      record.hitCount = e.hitCount;
      record.sawSubmit = e.sawSubmit;
      record.sawDraw = e.sawDraw;
      record.source = e.drawSource;
      record.hitKey = e.drawHitKey;
      record.selectionClearedByNativeOverride = e.drawSelectionCleared;
      record.deltaFrames = frame >= e.firstRejectFrame ? frame - e.firstRejectFrame : 0u;
      if (!CanEmit(true)) {
        AccountDrop(true);
        e = Entry{};
        continue;
      }
      record.chainSequence = ++e.chainSequence;
      EmitUnchecked(record, true);
      // F2：结算桶只在终态**真的发出**时递增（否则 closed* 会远大于实际终态数）。
      if (terminal == PaletteObjectTerminal::Recovered)
        m_counters.closedRecovered++;
      else if (terminal == PaletteObjectTerminal::WindowExpired)
        m_counters.closedWindowExpired++;
      else if (terminal == PaletteObjectTerminal::ObservationClosed)
        m_counters.closedObservationClosed++;
      else
        m_counters.closedUnclosed++;
      e = Entry{};
    }
    m_watchCount = 0u;
    // 注意：**不**在此清零每帧预算 —— 否则同帧继续记录即可绕过 64 条上限（上级 03:58 指出的缺口）。
  }

  void NoteReject(const PaletteObjectKey& key, PaletteObjectRejectReason reason,
                  const PaletteObjectFrames& frames) {
    StateLock guard(*this);
    if (m_windowClosed) {
      // 2026-09-18 P0-6：窗口关闭后到达的采集调用 —— 具名计损（不得静默）。
      m_counters.droppedWindowClosedEntry++;
      return;
    }
    if (RejectIfStaleSession(key))
      return;
    BeginFrame(frames.renderFrame);
    Entry* e = FindChain(key, PaletteObjectChainType::RejectionRecovery);
    // 2026-09-18 复核（Astra）P1 / 本计划阶段 C：**预算预检必须提前到 Insert 之前**。
    // 原实现先建条目、后判非终态预算 ⇒ 预算拒发时留下**无链首条目**（该条目最终只有一个
    // stage=Drawn 的终态；复核给出的第 65 键反例）。重排后「条目存在 ⇒ 链首已发出」对
    // **两类入口**都成立，即两类链共用同一条准入规则。
    // 顺序要点：**去重 → 表满 → 预算 → 建条目 → 发射**。表满必须**先于**预算判定，
    // 因为 TableFull 是**终态**，受终态预留（CanEmit(true)）而非非终态预算约束（F1）；
    // 若先判非终态预算就返回，会在非终态预算耗尽时**悄悄不再公告表满**。
    // 2026-09-18 阶段 C：**"先到先得"已被证伪并撤回**（见 Case 8 的 4 处失败 —— 拒绝建立的
    // 条目再也发不出观察链首，等于把观察链整条丢掉）。裁定要求的是**另建一条独立条目**
    // （inserted=1），因此本条目的类型化**查找键**（对象键 × 链型）与 v4 链型导出必须同时落地，
    // **已落地（2026-09-18 阶段 C）**：`chainType` 现已**参与查找与结算** —— 查找谓词见
    // `FindChain`（`SameKey(e.key,key) && e.chainType == type`）、结算分支见 `CloseWindow`
    // 的 Observation 分档，并由宿主机用例 27 / 28 / 35（两链独立、单链形状、双链路由）覆盖。
    // 上句的"必须同时落地"因此**已满足**；本条注释在被改写前曾长期写着"尚未参与查找"（已过期）。
    if (e != nullptr && e->lastRejectFrame == frames.renderFrame) {
      // D6：同类拒绝原因才算「已证明等价的重复」；原因不同则计入可见冲突。
      if (e->lastRejectReason == reason)
        m_counters.droppedDuplicatePerFrame++;
      else
        m_counters.droppedPayloadConflict++;
      return;
    }
    if (e == nullptr && m_watchCount >= kWatchCapacity) {
      // 表满 / 窗口内无可插入槽：该对象无法被跟踪 ⇒ 记 TableFull 终态（仍受终态预留约束）。
      // droppedTableFull 是"表满"这一事实本身（与预算无关）；closedTableFull 表示"终态真的发出"。
      m_counters.droppedTableFull++;
      // 审计 D1：表满时该对象**没有条目**，无法逐对象去重；而解析器要求「每个对象至多一条终态」。
      // 因此 TableFull 终态**每个窗口只公告一次**（表满是会话级条件），其余只累加 droppedTableFull。
      if (m_tableFullAnnounced)
        return;
      m_tableFullAnnounced = true;
      PaletteObjectEventRecord record{};
      record.stage = PaletteObjectStage::Rejected;
      record.terminal = PaletteObjectTerminal::TableFull;
      record.rejectReason = reason;
      record.key = key;
      // 2026-09-17 上级裁定 ⑦⑧：这条一次性（每窗口至多一次）TableFull 终态**不经过**
      // MakeRecord()，因此必须在这里同样盖上记录级分段与载明的身份证明种类（同一推导规则）。
      record.windowSegment = m_windowSegment;
      record.identityProofKind = DeriveIdentityProofKind(key);
      record.frames = frames;
      record.firstRejectFrame = frames.renderFrame;
      record.deltaFrames = 0u; // 终态契约：renderFrame - firstRejectFrame（同一帧 ⇒ 0）
      record.chainSequence = 1u;
      if (!CanEmit(true)) {
        AccountDrop(true);
        return;
      }
      // F4：只有真的发出的终态才计入 closedTableFull（与 CloseWindow 的 F2 对齐）。
      m_counters.closedTableFull++;
      EmitUnchecked(record, true);
      return;
    }
    if (!CanEmit(false)) {
      AccountDrop(false);
      return;
    }
    if (e == nullptr) {
      // 表未满且预算已通过 ⇒ 建条目**必然成功**（Insert 的唯一失败原因是表满，上面已判）。
      // 保留防御分支：若将来 Insert 新增失败原因，这里必须一并更新，不得静默继续。
      e = Insert(key, frames.renderFrame, reason, frames,
                 PaletteObjectChainType::RejectionRecovery);
      if (e == nullptr) {
        m_counters.droppedTableFull++;
        return;
      }
    }
    e->lastRejectFrame = frames.renderFrame;
    e->lastRejectReason = reason;
    MarkStage(e, frames.attemptSerial, PaletteObjectStage::Rejected);
    PaletteObjectEventRecord record = MakeRecord(*e);
    record.stage = PaletteObjectStage::Rejected;
    record.rejectReason = reason;
    record.frames = frames; // 保留调用点口径
    record.firstRejectFrame = e->firstRejectFrame;
    record.hitCount = e->hitCount;
    // 冻结读方契约（G3）：Rejected / ServedCandidate 与**终态**携带 renderFrame-firstRejectFrame；
    // live 的 Enqueued / Drawn 必须为 0。
    record.deltaFrames = frames.renderFrame >= e->firstRejectFrame
                             ? frames.renderFrame - e->firstRejectFrame
                             : 0u;
    record.chainSequence = ++e->chainSequence;
    EmitUnchecked(record, false);
  }

  // 2026-09-18 独立复审批次 3（**正常观察链**，wire 版本 3）：
  // 语义 = 该对象作为 CPU caster 候选**首次进入观察**。
  //   · 它**不是**拒绝，不得伪装成 Rejected，也不得让读方解释成「已恢复」；
  //   · 它是**唯一**允许为「从未被拒绝过的对象」建立条目的入口（NoteReject 建条目
  //     依据的是「真的发生了拒绝」这一事实，本函数依据的是「真的观测到了候选」这一事实，
  //     两者都是事实，故都不是伪造）；
  //   · 采样与上限沿用同一套表/同一套预算（kWatchCapacity / CanEmit），不新增容量。
  // 与 NoteReject 的**唯一**差别：表满时只计 droppedTableFull，**不发** TableFull 终态 ——
  // 终态是「拒绝后未能接住」的事实，首见无权宣告它。
  void NoteFirstSight(const PaletteObjectKey& key,
                      const PaletteObjectFrames& frames) {
    StateLock guard(*this);
    if (m_windowClosed) {
      // 2026-09-18 P0-6：窗口关闭后到达的采集调用 —— 具名计损（不得静默）。
      m_counters.droppedWindowClosedEntry++;
      return;
    }
    if (RejectIfStaleSession(key))
      return;
    BeginFrame(frames.renderFrame);
    Entry* e = FindChain(key, PaletteObjectChainType::Observation);
    // 先处理「已发过链首」的重复观测（最廉价、不消耗配额）。
    // 修正② 把预算预检与此合并到 Insert 之前，见下方说明。
    // 2026-09-18 阶段 C：**"先到先得"已撤回**（Case 8 的 4 处失败证明它会丢掉整条观察链）。
    // 正确修法 = 类型化查找键（对象键 × 链型）+ v4 链型导出，使两类链**各自独立存在**；
    // 在那之前，本函数保持既有行为（对已存在的拒绝条目复用），缺陷**如实记为未修**。
    if (e != nullptr && e->sawFirstSight) {
      // 2026-09-18 独立复审 D 定案：去重维度**原本是 (对象键, renderFrame)**，
      // 而 `lastFirstSightFrame` 只在**真的发射**时更新 ⇒ 跨帧不去重 ⇒ 同一对象每帧再发一条
      // 「FirstSight」。实测后果：32 个对象发出 1304 条（≈41 帧/对象），
      // 每对象每帧固定吃掉 2 个非终态配额（首见+入队），于是：
      //   * 每帧只有 64/2 = 32 个对象能拿到链首；
      //   * 每会话 kNormalBudget=3584 被迅速打满（实测 emitted−terminal 恰好 = 3584），
      //     之后的 75 个对象**有条目、有终态、零阶段事件** ⇒ 解析器只能看到 ["Drawn"]。
      // 阶段名是 FirstSight（**首次**观察），行为却是「本帧又看到一次」——名实不符。
      // 改成按条目去重后，每对象每窗口恰一条链首，语义与 h:58-63 的文档一致。
      // 计数器沿用 droppedDuplicatePerFrame（不新增 wire 字段，避免又一次三方读方同步）；
      // 它在首见链上的含义自此是「已发过链首的条目又被重复观测」。
      m_counters.droppedDuplicatePerFrame++;
      return;
    }
    // 2026-09-18 独立复审 D 建议的**修正②**：预算预检**提前到 Insert 之前**。
    // 原先「先建条目、后判预算」⇒ 预算拒发时条目已建（firstSightInserted++）却零阶段事件；
    // 这些条目最终只剩终态，而终态对 !sawFirstSight 的条目把 stage 回填为 Drawn（:258-259）
    // ⇒ 读方看到 ["Drawn"] 的**假链**（实机实测 75 条）。
    // 提前后「表里有条目 ⇒ 链首一定发过」成为**不变量**。
    // 代价：预算紧张时该对象**完全不可见**（连终态也没有）—— 这是「不伪造事实」的方向；
    // 丢失已由 AccountDrop 显式计入 droppedPerFrame/droppedPerSession，**不是静默丢失**。
    if (!CanEmit(false)) {
      AccountDrop(false);
      return;
    }
    if (e == nullptr) {
      // 首见＝该键的链首，故 firstReason 记 NotChecked（本链**没有**拒绝事实）。
      // 放在预算预检**之后**：只有确实要发射时才建条目。
      e = Insert(key, frames.renderFrame, PaletteObjectRejectReason::NotChecked, frames,
                 PaletteObjectChainType::Observation);
      if (e == nullptr) {
        m_counters.droppedTableFull++;
        return;
      }
      m_counters.firstSightInserted++;
    }
    e->sawFirstSight = true;
    PaletteObjectEventRecord record =
        PrepareStage(e, PaletteObjectStage::FirstSight, frames);
    record.source = PaletteObjectSource::None;
    record.hitKey = 0u;
    record.hitCount = e->hitCount;
    record.sawSubmit = e->sawSubmit;
    record.sawDraw = e->sawDraw;
    record.firstRejectFrame = e->firstRejectFrame;
    // 版本 3 契约：首见链的 firstRejectFrame 槽位承载的是**链首帧**（= 首见帧）；
    // 该链不存在拒绝帧。deltaFrames 因而 = 本帧 - 链首帧，含义自洽。
    record.deltaFrames = frames.renderFrame >= e->firstRejectFrame
                             ? frames.renderFrame - e->firstRejectFrame
                             : 0u;
    record.chainSequence = ++e->chainSequence;
    m_counters.firstSightEmitted++;
    EmitUnchecked(record, false);
  }

  // 替代来源供出 palette：**只推进状态**，不移除条目（首次命中不得删表）。
  void NoteServed(const PaletteObjectKey& key, PaletteObjectSource source,
                  uint64_t hitKey, const PaletteObjectFrames& frames) {
    StateLock guard(*this);
    if (m_windowClosed) {
      // 2026-09-18 P0-6：窗口关闭后到达的采集调用 —— 具名计损（不得静默）。
      m_counters.droppedWindowClosedEntry++;
      return;
    }
    if (RejectIfStaleSession(key))
      return;
    BeginFrame(frames.renderFrame);
    // 2026-09-18 P0-6（复审 C1-1 修正）：**两条链指针都在第一次 advance 之前取好**。
    // 旧写法在第一次 advance **之后**才取观察链指针 ⇒ 「第一条 emit 触发嵌套 CloseWindow 把
    // 观察链条目清掉」这条**生产可达**路径会走进 nullptr 裸 return ⇒ **静默丢失且零计数**
    // （独立复审实测：该路径上 pathObservationSkippedAfterReentrantClose 恒为 0）。
    // 另一条旧判据 `m_windowClosed` 同样**不可达**：CloseWindow 在置该标志前会清掉每个 used 条目
    // 并阻塞后续 Insert ⇒ 「窗口已关且 FindChain 仍非空」不可能。
    // ⇒ 判据与 NoteObjectGone（P0-3/C3）统一为：**该链是否仍可结算**（重新 FindChain）。
    Entry* const rejectChain = FindChain(key, PaletteObjectChainType::RejectionRecovery);
    Entry* const observationChain = FindChain(key, PaletteObjectChainType::Observation);
    // 复审 266e35c4：判据必须是**同一条链实例**，不只是「条目仍在」。
    const uint64_t observationInstance =
        observationChain != nullptr ? observationChain->instanceId : 0u;
    if (rejectChain == nullptr && observationChain == nullptr) {
      // 两条链都不存在 ⇒ 这条路径事实**无处可记**（复审丢弃路径清单第 8 条）⇒ 具名计损。
      m_counters.droppedNoChainForPathFact++;
      return;
    }
    // 2026-09-18 P0-6（Astra 裁定 C1）：对**两条链各自**记一份；关联依据 = FindChain 的
    // **完整键相等**（八元组）+ **链型相等** + 同窗口 —— 记录器真正拥有的显式证据，
    // **不是**「同键无条件广播」。顺序**显式固定**：先拒绝恢复链、后观察链。
    AdvanceServedOnChain(rejectChain, source, hitKey, frames);
    if (observationChain != nullptr) {
      // 第一条的 emit 可能触发嵌套结算/冻结（生产可达：环预冻结钩子回调 CloseWindow）⇒
      // 必须重取并确认它**仍可结算**；被嵌套结算吃掉时**具名计损**，绝不静默。
      Entry* const stillSettleable = FindChain(key, PaletteObjectChainType::Observation);
      if (stillSettleable != nullptr && stillSettleable->instanceId == observationInstance)
        AdvanceServedOnChain(stillSettleable, source, hitKey, frames);
      else
        m_counters.pathObservationSkippedAfterReentrantClose++;
    }
  }

  // CPU caster 候选入队（**不是 GPU 提交完成**）；来源取**该次候选实际携带的按值诊断来源**。
  void NoteEnqueued(const PaletteObjectKey& key, PaletteObjectSource source,
                    uint64_t hitKey, const PaletteObjectFrames& frames,
                    bool selectionClearedByNativeOverride) {
    StateLock guard(*this);
    if (m_windowClosed) {
      // 2026-09-18 P0-6：窗口关闭后到达的采集调用 —— 具名计损（不得静默）。
      m_counters.droppedWindowClosedEntry++;
      return;
    }
    if (RejectIfStaleSession(key))
      return;
    BeginFrame(frames.renderFrame);
    // 2026-09-18 P0-6（复审 C1-1 修正）：**两条链指针都在第一次 advance 之前取好**。
    // 旧写法在第一次 advance **之后**才取观察链指针 ⇒ 「第一条 emit 触发嵌套 CloseWindow 把
    // 观察链条目清掉」这条**生产可达**路径会走进 nullptr 裸 return ⇒ **静默丢失且零计数**
    // （独立复审实测：该路径上 pathObservationSkippedAfterReentrantClose 恒为 0）。
    // 另一条旧判据 `m_windowClosed` 同样**不可达**：CloseWindow 在置该标志前会清掉每个 used 条目
    // 并阻塞后续 Insert ⇒ 「窗口已关且 FindChain 仍非空」不可能。
    // ⇒ 判据与 NoteObjectGone（P0-3/C3）统一为：**该链是否仍可结算**（重新 FindChain）。
    Entry* const rejectChain = FindChain(key, PaletteObjectChainType::RejectionRecovery);
    Entry* const observationChain = FindChain(key, PaletteObjectChainType::Observation);
    // 复审 266e35c4 的潜伏缺口：判据必须是**同一条链实例**，不只是「条目仍在」。
    const uint64_t observationInstance =
        observationChain != nullptr ? observationChain->instanceId : 0u;
    if (rejectChain == nullptr && observationChain == nullptr) {
      // 两条链都不存在 ⇒ 这条路径事实**无处可记**（复审丢弃路径清单第 8 条）⇒ 具名计损。
      m_counters.droppedNoChainForPathFact++;
      return;
    }
    // 2026-09-18 P0-6（Astra 裁定 C1）：对**两条链各自**记一份；关联依据 = FindChain 的
    // **完整键相等**（八元组）+ **链型相等** + 同窗口 —— 记录器真正拥有的显式证据，
    // **不是**「同键无条件广播」。顺序**显式固定**：先拒绝恢复链、后观察链。
    AdvanceEnqueuedOnChain(rejectChain, source, hitKey, frames, selectionClearedByNativeOverride);
    if (observationChain != nullptr) {
      // 第一条的 emit 可能触发嵌套结算/冻结（生产可达：环预冻结钩子回调 CloseWindow）⇒
      // 必须重取并确认它**仍可结算**；被嵌套结算吃掉时**具名计损**，绝不静默。
      Entry* const stillSettleable = FindChain(key, PaletteObjectChainType::Observation);
      if (stillSettleable != nullptr && stillSettleable->instanceId == observationInstance)
        AdvanceEnqueuedOnChain(stillSettleable, source, hitKey, frames, selectionClearedByNativeOverride);
      else
        m_counters.pathObservationSkippedAfterReentrantClose++;
    }
  }


  // 绘制命令已记录（**不是 GPU 执行或像素正确性证明**）；来源取**该次 draw 实际携带的按值来源**。
  void NoteDrawn(const PaletteObjectKey& key, PaletteObjectSource source,
                 uint64_t hitKey, const PaletteObjectFrames& frames,
                 bool selectionClearedByNativeOverride) {
    StateLock guard(*this);
    if (m_windowClosed) {
      // 2026-09-18 P0-6：窗口关闭后到达的采集调用 —— 具名计损（不得静默）。
      m_counters.droppedWindowClosedEntry++;
      return;
    }
    if (RejectIfStaleSession(key))
      return;
    BeginFrame(frames.renderFrame);
    // 2026-09-18 P0-6（复审 C1-1 修正）：**两条链指针都在第一次 advance 之前取好**。
    // 旧写法在第一次 advance **之后**才取观察链指针 ⇒ 「第一条 emit 触发嵌套 CloseWindow 把
    // 观察链条目清掉」这条**生产可达**路径会走进 nullptr 裸 return ⇒ **静默丢失且零计数**
    // （独立复审实测：该路径上 pathObservationSkippedAfterReentrantClose 恒为 0）。
    // 另一条旧判据 `m_windowClosed` 同样**不可达**：CloseWindow 在置该标志前会清掉每个 used 条目
    // 并阻塞后续 Insert ⇒ 「窗口已关且 FindChain 仍非空」不可能。
    // ⇒ 判据与 NoteObjectGone（P0-3/C3）统一为：**该链是否仍可结算**（重新 FindChain）。
    Entry* const rejectChain = FindChain(key, PaletteObjectChainType::RejectionRecovery);
    Entry* const observationChain = FindChain(key, PaletteObjectChainType::Observation);
    // 复审 266e35c4 的潜伏缺口：判据必须是**同一条链实例**，不只是「条目仍在」。
    const uint64_t observationInstance =
        observationChain != nullptr ? observationChain->instanceId : 0u;
    if (rejectChain == nullptr && observationChain == nullptr) {
      // 两条链都不存在 ⇒ 这条路径事实**无处可记**（复审丢弃路径清单第 8 条）⇒ 具名计损。
      m_counters.droppedNoChainForPathFact++;
      return;
    }
    // 2026-09-18 P0-6（Astra 裁定 C1）：对**两条链各自**记一份；关联依据 = FindChain 的
    // **完整键相等**（八元组）+ **链型相等** + 同窗口 —— 记录器真正拥有的显式证据，
    // **不是**「同键无条件广播」。顺序**显式固定**：先拒绝恢复链、后观察链。
    AdvanceDrawnOnChain(rejectChain, source, hitKey, frames, selectionClearedByNativeOverride);
    if (observationChain != nullptr) {
      // 第一条的 emit 可能触发嵌套结算/冻结（生产可达：环预冻结钩子回调 CloseWindow）⇒
      // 必须重取并确认它**仍可结算**；被嵌套结算吃掉时**具名计损**，绝不静默。
      Entry* const stillSettleable = FindChain(key, PaletteObjectChainType::Observation);
      if (stillSettleable != nullptr && stillSettleable->instanceId == observationInstance)
        AdvanceDrawnOnChain(stillSettleable, source, hitKey, frames, selectionClearedByNativeOverride);
      else
        m_counters.pathObservationSkippedAfterReentrantClose++;
    }
  }


  // 对象已从 manifest / 场景消失的可证依据（由调用方给出依据后调用）。
  void NoteObjectGone(const PaletteObjectKey& key) {
    StateLock guard(*this);
    if (m_windowClosed) {
      // 2026-09-18 P0-6：窗口关闭后到达的采集调用 —— 具名计损（不得静默）。
      m_counters.droppedWindowClosedEntry++;
      return;
    }
    if (RejectIfStaleSession(key))
      return;
    // 2026-09-18 P0-6（复审 128c3826 指出的**第四个同类静默丢弃**）：两条链都不存在时，
    // `SettleObjectGone(nullptr)` 直接返回 ⇒ 此前是**零计数、零事件、零终态**（与 S/E/D 的同类缺口一致）。
    // 改为**具名计损**（内部计数、不上 wire，与其它内部计数同理）。
    if (FindChain(key, PaletteObjectChainType::RejectionRecovery) == nullptr &&
        FindChain(key, PaletteObjectChainType::Observation) == nullptr) {
      m_counters.droppedNoChainForPathFact++;
      return;
    }
    // 2026-09-18 P0-3（Astra 实测修正）：**两条独立链都要结算**。
    // 关联依据是**显式**的：「该对象已消失」对**每一条**关于该对象的诊断链都是终止事实 ——
    // 这不是把某条链的事件广播给另一条，而是两条链共同拥有的一个事实（与 P0-6 要求的
    // 「明确关联后的逐链记录」一致）。修复前只处理 `FindOwner()` 的一条（全局优先观察链）
    // ⇒ 拒绝链被滞留表内，最终以 WindowExpired 结算（Astra 实测）。
    // 2026-09-18 P0-3（复审 R4）：emit 可能触发**嵌套结算/冻结**（生产可达）⇒ 必须**先**记录
    // 第二条链是否存在（emit 之后表可能已被清空，届时 FindChain 就查不到了），
    // 否则第二条链的 ObjectGone 会被静默丢弃。
    // 2026-09-18 P0-3（复审 C1/C3）：先取两条链，再逐条结算。
    Entry* const rejectChain = FindChain(key, PaletteObjectChainType::RejectionRecovery);
    Entry* const observationChain = FindChain(key, PaletteObjectChainType::Observation);
    // 复审 266e35c4 反例 5：判据必须是**同一条链实例** —— 否则重入期间**新建**的观察链会
    // 拿到「对象已消失」终态（复审实测 obsTerm=1、objectGoneLostToReentrancy=0）。
    const uint64_t observationInstance =
        observationChain != nullptr ? observationChain->instanceId : 0u;
    SettleObjectGone(rejectChain);
    if (observationChain != nullptr) {
      // 第一条结算的 emit 可能触发嵌套结算/冻结（生产可达）⇒ 必须**重新确认第二条仍然可结算**。
      // 判据**不用** `m_windowClosed`（复审 C3）：ResetForSessionTransition 会清表却不置该标志，
      // 而 Reset 连计数一起清零 ⇒ 用窗口标志会既漏判又丢计数。FindChain 本身会跳过墓碑与已失效条目。
      Entry* const stillSettleable = FindChain(key, PaletteObjectChainType::Observation);
      if (stillSettleable != nullptr && stillSettleable->instanceId == observationInstance)
        SettleObjectGone(stillSettleable);
      else
        m_counters.objectGoneLostToReentrancy++;  // 具名 fail-visible：第二条链的 ObjectGone 被重入吃掉
    }
  }

  // 计数读取：环外丢失是**无锁**累加的（发射器在持锁路径内回调，不能重入同一把锁）。
  Counters counters() const {
    StateLock guard(*this);
    Counters copy = m_counters;
    copy.ringEvictedAfterRecord += m_ringLosses.load(std::memory_order_relaxed);
    return copy;
  }
  uint32_t watchCount() const { StateLock guard(*this); return m_watchCount; }
  // 2026-09-18 批次 3：本会话是否发过首见事件 ⇒ 写方据此把块版本抬到 3
  // （版本 1/2 读方的阶段表不含 FirstSight，抬版本是为了让它们**明确拒绝**而非静默忽略）。
  bool firstSightUsed() const {
    StateLock guard(*this);
    return m_counters.firstSightEmitted != 0u;
  }
  uint64_t sessionGeneration() const { StateLock guard(*this); return m_sessionGeneration; }
  // 最近一次推进到的渲染帧（供 CloseWindow 结算终态使用）。
  uint64_t currentFrame() const { StateLock guard(*this); return m_currentFrame; }
  uint64_t mapEpoch() const { StateLock guard(*this); return m_mapEpoch; }
  // 一次持锁读出计数 + 观察数（导出头块必须来自同一个一致快照）。
  void SnapshotCounters(Counters& outCounters, uint64_t& outWatchCount) const {
    StateLock guard(*this);
    outCounters = m_counters;
    outCounters.ringEvictedAfterRecord += m_ringLosses.load(std::memory_order_relaxed);
    outWatchCount = m_watchCount;
  }

  // 键包含**生命周期身份**（工单 §2.5.1）：指针复用时不得把两个对象实例合并。
  static bool SameKey(const PaletteObjectKey& a, const PaletteObjectKey& b) {
    return a.renderablePart == b.renderablePart &&
           a.runtimeModelPtr == b.runtimeModelPtr && a.jHandle == b.jHandle &&
           a.rawcode == b.rawcode && a.sessionGeneration == b.sessionGeneration &&
           a.mapEpoch == b.mapEpoch && a.deviceEpoch == b.deviceEpoch &&
           a.lifecycleIdentity == b.lifecycleIdentity;
  }
  static uint64_t HashKey(const PaletteObjectKey& k) {
    uint64_t h = 1469598103934665603ull;
    const uint64_t parts[8] = {k.renderablePart, k.runtimeModelPtr, k.jHandle,
                               k.rawcode, k.sessionGeneration, k.mapEpoch,
                               k.deviceEpoch, k.lifecycleIdentity};
    for (uint32_t i = 0u; i < 8u; ++i) {
      h ^= parts[i];
      h *= 1099511628211ull;
    }
    // 2026-09-17 离线成本测量发现（上一轮实测）：FNV-1a 的**低位**在「part 只差几个低位」的键族上严重聚集，
    // 而索引是 h % kWatchCapacity（1024）⇒ 只用到低 10 位：1024 个键只落 6 个起始槽，
    // 最坏探测 1022 次、最坏查找 ~800 ns（最好 8 ns，约 100 倍）。取索引前必须做一次强混淆。
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
  }

 private:
  // 精简条目（不嵌入完整记录），1024 槽无堆分配。
  struct Entry {
    // 2026-09-18 阶段 C（窗口维度）：条目所属窗口（建立时落定）。
    uint64_t windowSegment = 0u;
    // 2026-09-18 P0-6（复审 266e35c4 的潜伏缺口）：**链实例身份**。
    // 现行判据曾只证明「同键+同链型+同窗口的条目仍在」，而不证明**是同一条链** ——
    // 重入期间若重新开窗并重建同名链，仅按存在性判断会把事实**静默改挂到新链**（复审实测）。
    // instanceId 在每次 Insert 时从单调计数器分配、**跨 Reset 不重置** ⇒ 重建后的新条目必然不同号。
    uint64_t instanceId = 0u;
    PaletteObjectKey key{};
    PaletteObjectFrames firstFrames{};
    // 2026-09-18 阶段 C（复核 D 定案）：**链型在条目建立时确定，此后不得更改**。
    // 背景：修复前 `NoteFirstSight` 对"已由拒绝建立的条目"**不 Insert**，直接在该条目上
    // 发链首 ⇒ 拒绝恢复链被**改类**成观察链（复核实测 inserted=0 / emitted=1）。
    // 两类链的事实依据不同（"真的发生了拒绝" vs "真的观测到了候选"），不得互相冒充，
    // 因此本轮采用**先到先得**：某键先由哪类入口建立，其后另一类入口**不得改类**，
    // 只能被**显式计入**（droppedPayloadConflict = 同对象的可见冲突）。
    // 【历史】当时先采用「**先到先得**」；该方案**已被证伪并撤回**（见本文件上方"先到先得已被
    // 证伪"一段：它会让拒绝建立的条目再也发不出观察链首，等于把观察链整条丢掉）。
    // 【现行】同类键的**两条独立条目可以并存**（各自 chainType 不可改类）；链型**已在 wire 的**
    // `data[4]`（sink.cpp 的 `event.data[4] = record.chainType`），读方按「对象键 × 链型」分组。
    // 由宿主机用例 27（两链并存）/ 28（单链形状）/ 35（双链路由）覆盖。
    PaletteObjectChainType chainType = PaletteObjectChainType::RejectionRecovery;
    uint64_t firstRejectFrame = 0u;
    uint64_t lastRejectFrame = kNoFrame;
    // D6：同帧去重必须比较**载荷**（拒绝原因），否则会把不同事实当成重复吞掉。
    PaletteObjectRejectReason lastRejectReason = PaletteObjectRejectReason::Unknown;
    // 2026-09-18：`lastFirstSightFrame` 已删除。它原先是首见链的**按帧去重标记**，
    // 但那个维度本身就是缺陷（跨帧不去重 ⇒ 每帧重发 ⇒ 烧光配额）。去重现按条目
    // （`sawFirstSight`）进行，该标记不再被读取；留着它会让读者以为去重仍按帧进行。
    uint64_t lastServedFrame = kNoFrame;
    uint64_t lastSubmitFrame = kNoFrame;
    uint64_t lastDrawFrame = kNoFrame;
    uint64_t lastHitKey = 0u;
    uint64_t submitHitKey = 0u;
    uint64_t drawHitKey = 0u;
    PaletteObjectRejectReason firstReason = PaletteObjectRejectReason::NotChecked;
    PaletteObjectSource lastSource = PaletteObjectSource::None;
    PaletteObjectSource submitSource = PaletteObjectSource::None;
    PaletteObjectSource drawSource = PaletteObjectSource::None;
    uint32_t hitCount = 0u;
    uint32_t chainSequence = 0u;
    uint32_t maxStage = 0u;      // 既有：**终身单调**（未知尝试号时仍按它判序）
  uint64_t attemptSerial = ~0ull; // 最近一次**已知**尝试号
  uint32_t attemptStage = 0u;     // **本次尝试内**的最高秩（K3）
    bool sawFirstSight = false;
    bool sawServed = false;
    bool sawSubmit = false;
    bool sawDraw = false;
    bool orderViolation = false;
    bool submitSelectionCleared = false;
    bool drawSelectionCleared = false;
    bool used = false;
    bool tombstone = false; // 删除留下的空洞：Find 必须跨过它继续探测
  };


  // 2026-09-18 P0-6（C1）：把「一条链上推进 ServedCandidate」抽成单链操作。
  // 原 `NoteServed` 内联体逐字搬入（含 D6 载荷去重、CanEmit 预检、hitCount 只在真的发出时递增）。
  void AdvanceServedOnChain(Entry* e, PaletteObjectSource source, uint64_t hitKey,
                            const PaletteObjectFrames& frames) {
    if (e == nullptr)
      return;
    if (e->lastServedFrame == frames.renderFrame) {
      // D6：只有**载荷可证等价**（来源 + hitKey 相同）才允许压缩；否则计入可见冲突。
      if (e->lastSource == source && e->lastHitKey == hitKey)
        m_counters.droppedDuplicatePerFrame++;
      else
        m_counters.droppedPayloadConflict++;
      return;
    }
    if (!CanEmit(false)) {
      AccountDrop(false);
      return;
    }
    e->lastServedFrame = frames.renderFrame;
    e->lastSource = source;
    e->lastHitKey = hitKey;
    PaletteObjectEventRecord record = PrepareStage(e, PaletteObjectStage::ServedCandidate,
                                                    frames);
    // hitCount 只在**事件真的发出**时递增（此处已通过预检）：
    // 否则解析器会看到 hitCount 大于 Served 事件数而误判截断。
    e->hitCount++;
    e->sawServed = true;
    record.source = source;
    record.hitKey = hitKey;
    record.hitCount = e->hitCount;
    record.sawSubmit = e->sawSubmit;
    record.sawDraw = e->sawDraw;
    record.firstRejectFrame = e->firstRejectFrame;
    // 冻结读方契约（G3）：Rejected / ServedCandidate 与**终态**携带 renderFrame-firstRejectFrame；
    // live 的 Enqueued / Drawn 必须为 0。
    record.deltaFrames = frames.renderFrame >= e->firstRejectFrame
                             ? frames.renderFrame - e->firstRejectFrame
                             : 0u;
    record.chainSequence = ++e->chainSequence;
    EmitUnchecked(record, false);
  }

  // 2026-09-18 P0-6（C1）：单链操作（原内联体逐字搬入）。
  void AdvanceDrawnOnChain(Entry* e, PaletteObjectSource source, uint64_t hitKey,
                    const PaletteObjectFrames& frames,
                    bool selectionClearedByNativeOverride) {
    if (e == nullptr)
      return;
    if (e->lastDrawFrame == frames.renderFrame) {
      // D6：**这正是上级给的反例** —— 同帧第二次 D 已清空语义 palette，载荷不等价 ⇒ 计冲突。
      if (e->drawSource == source && e->drawHitKey == hitKey &&
          e->drawSelectionCleared == selectionClearedByNativeOverride)
        m_counters.droppedDuplicatePerFrame++;
      else
        m_counters.droppedPayloadConflict++;
      return;
    }
    if (!CanEmit(false)) {
      AccountDrop(false);
      return;
    }
    e->lastDrawFrame = frames.renderFrame;
    PaletteObjectEventRecord record = PrepareStage(e, PaletteObjectStage::Drawn,
                                                    frames);
    e->sawDraw = true;
    e->drawSource = source;
    e->drawHitKey = hitKey;
    e->drawSelectionCleared = selectionClearedByNativeOverride;
    record.source = source;
    record.hitKey = hitKey;
    record.sawSubmit = e->sawSubmit;
    record.sawDraw = true;
    record.selectionClearedByNativeOverride = selectionClearedByNativeOverride;
    record.firstRejectFrame = e->firstRejectFrame;
    record.hitCount = e->hitCount;
    // G3：live 事件 deltaFrames 必须为 0。
    record.deltaFrames = 0u;
    record.chainSequence = ++e->chainSequence;
    EmitUnchecked(record, false);
  }

  // 2026-09-18 P0-6（C1）：单链操作（原内联体逐字搬入）。
  void AdvanceEnqueuedOnChain(Entry* e, PaletteObjectSource source, uint64_t hitKey,
                    const PaletteObjectFrames& frames,
                    bool selectionClearedByNativeOverride) {
    if (e == nullptr)
      return;
    if (e->lastSubmitFrame == frames.renderFrame) {
      // D6：来源 + hitKey + 清空标志全同才算等价重复。
      if (e->submitSource == source && e->submitHitKey == hitKey &&
          e->submitSelectionCleared == selectionClearedByNativeOverride)
        m_counters.droppedDuplicatePerFrame++;
      else
        m_counters.droppedPayloadConflict++;
      return;
    }
    if (!CanEmit(false)) {
      AccountDrop(false);
      return;
    }
    e->lastSubmitFrame = frames.renderFrame;
    e->submitSource = source;
    e->submitHitKey = hitKey;
    PaletteObjectEventRecord record = PrepareStage(e, PaletteObjectStage::Enqueued,
                                                    frames);
    e->sawSubmit = true;
    e->submitSelectionCleared = selectionClearedByNativeOverride;
    record.source = source;
    record.hitKey = hitKey;
    record.sawSubmit = true;
    record.sawDraw = e->sawDraw;
    record.selectionClearedByNativeOverride = selectionClearedByNativeOverride;
    record.firstRejectFrame = e->firstRejectFrame;
    record.hitCount = e->hitCount;
    // G3：**deltaFrames 只由终态携带**（live 事件必须为 0，否则被判 frozenZeroDeltaViolated）。
    record.deltaFrames = 0u;
    record.chainSequence = ++e->chainSequence;
    EmitUnchecked(record, false);
  }

  // P0-3：把「一条链的 ObjectGone 结算」抽成**单条链**的操作。
  // 原内联体逐字保留，唯一语义修正见下方 stage 两行。
  void SettleObjectGone(Entry* e) {
    if (e == nullptr)
      return;
    PaletteObjectEventRecord record = MakeRecord(*e);
    // 2026-09-18 P0-3（Astra 实测修正）：**不得硬写 Rejected** ——
    // 观察链从未被拒绝过，给它写 Rejected 就是伪造事实，且与本方读方规则
    // 「观察链不得携带 Rejected 阶段」直接冲突（写方会产出自相矛盾的内容）。
    // 采用与 CloseWindow 相同的原则：按该条目**实际达到的最高语义秩**回填；
    // 拒绝恢复链保留原冻结语义 Rejected，一位不改。
    record.stage = e->sawFirstSight ? StageOfRank(e->maxStage)
                                    : PaletteObjectStage::Rejected;
    record.terminal = PaletteObjectTerminal::ObjectGone;
    // 契约显式化：终态 delta 必须等于 renderFrame - firstRejectFrame（此处 renderFrame 仍取首帧 ⇒ 0）。
    record.deltaFrames = record.frames.renderFrame >= record.firstRejectFrame
                            ? record.frames.renderFrame - record.firstRejectFrame
                            : 0u;
    record.sawSubmit = e->sawSubmit;
    record.sawDraw = e->sawDraw;
    record.source = e->drawSource;
    record.hitKey = e->drawHitKey;
    if (!CanEmit(true)) {
      AccountDrop(true);
      e->used = false;
      e->tombstone = true;
      // 2026-09-18 P0-3（复审 R4）：**不得盲目减计数** —— 生产可达的嵌套结算（环预冻结钩子
      // 在 emit 内回调 CloseWindow）会把 m_watchCount 清 0，无条件 -- 会下溢到 0xFFFFFFFF，
      // 而读方硬要求 watchCount <= 1024 ⇒ **整份导出被拒**。护栏是幂等的、不会减成负数。
      if (m_watchCount > 0u)
        m_watchCount--;
      return;
    }
    // F4：只有真的发出的终态才计入 closedObjectGone（与 CloseWindow 的 F2 对齐）。
    m_counters.closedObjectGone++;
    record.chainSequence = ++e->chainSequence;
    // 2026-09-18 P0-3（复审 C1）：**必须在 emit 之前认领该条目** —— 否则 emit 内的嵌套结算
    // （生产可达：环预冻结钩子回调 CloseWindow；复审用 Case 20 形状复现）会看到它仍然 used，
    // 于是**同一条链拿到两个终态**；读方 AutoTest/analyze_palette_object_evidence.py:514
    // 硬要求『一个对象至多一个终态事件』⇒ **整份导出被拒**（这是抽取前就有的形状）。
    // 现在：先失效 + 减计数，再发射；发射后不再读 e。
    e->used = false;
    e->tombstone = true;
    if (m_watchCount > 0u)
      m_watchCount--;
    EmitUnchecked(record, true);
  }

  // 2026-09-17 上级裁定 ⑦：载明的身份证明种类的**唯一**推导规则（缺一不可）：
  //   非零实例生命周期身份 && !identityWeak && !epochUnknown  ⇒ InstanceLifecycleIdentityProof
  //   其余一切情况（含 identityWeak=0 但身份值为 0 / 代际未知）⇒ NoIdentityProof。
  // 静态纯函数：不读任何状态，因此无需持锁；三个生产采集点由 MakePaletteObjectKey() 一律
  // 写 lifecycleIdentity=0 + identityWeak=true + epochUnknown=true ⇒ 生产恒为 NoIdentityProof。
  // **无 setter、无测试专用后门**：调用点不得直接给 record.identityProofKind 赋值。
  static uint32_t DeriveIdentityProofKind(const PaletteObjectKey& key) {
    return (key.lifecycleIdentity != 0u && !key.identityWeak && !key.epochUnknown)
               ? static_cast<uint32_t>(
                     PaletteObjectIdentityProofKind::InstanceLifecycleIdentityProof)
               : static_cast<uint32_t>(PaletteObjectIdentityProofKind::NoIdentityProof);
  }

  // 记录级分段/证明种类必须**在此处**盖一次（发出时写一次，冻结后不再变化）。
  // 由 static 改为 const 成员：它只多读 m_windowSegment，不写任何状态。
  PaletteObjectEventRecord MakeRecord(const Entry& e) const {
    PaletteObjectEventRecord record{};
    record.windowSegment = m_windowSegment;
    record.identityProofKind = DeriveIdentityProofKind(e.key);
    record.key = e.key;
    record.frames = e.firstFrames;
    record.firstRejectFrame = e.firstRejectFrame;
    record.rejectReason = e.firstReason;
    record.hitCount = e.hitCount;
    record.hitKey = e.lastHitKey;
    record.source = e.lastSource;
    record.sawSubmit = e.sawSubmit;
    record.sawDraw = e.sawDraw;
    record.chainSequence = e.chainSequence;
    record.chainType = e.chainType; // 链型随事件导出（记录层）
    return record;
  }

  void BeginFrame(uint64_t frame) {
    if (frame != m_currentFrame) {
      m_currentFrame = frame;
      m_frameBudgetUsed = 0u;
    }
  }

  // 2026-09-18 批次 3：阶段顺序按 **StageRank() 的语义秩**，不按枚举数值。
  // 原因：FirstSight 的枚举值（5）为让版本 1/2 读方直接拒绝而故意取大，但它的
  // 语义秩排在 Rejected 之前；若直接用数值比较，首见之后的 ServedCandidate/
  // Enqueued/Drawn（2/3/4）全都小于 maxStage(5) 而被**误判 orderViolation**。
  static uint32_t StageRank(PaletteObjectStage stage) {
    switch (stage) {
      case PaletteObjectStage::FirstSight:
        return 0u;
      case PaletteObjectStage::Rejected:
        return 1u;
      case PaletteObjectStage::ServedCandidate:
        return 2u;
      case PaletteObjectStage::Enqueued:
        return 3u;
      case PaletteObjectStage::Drawn:
        return 4u;
    }
    return 0u;
  }
  // 语义秩 ⇒ 阶段（终态记录回填用；见 CloseWindow 里的 `e.sawFirstSight ? StageOfRank(e.maxStage) : Drawn`）。
  static PaletteObjectStage StageOfRank(uint32_t rank) {
    switch (rank) {
      case 4u:
        return PaletteObjectStage::Drawn;
      case 3u:
        return PaletteObjectStage::Enqueued;
      case 2u:
        return PaletteObjectStage::ServedCandidate;
      case 1u:
        return PaletteObjectStage::Rejected;
      default:
        return PaletteObjectStage::FirstSight;
    }
  }
  // 阶段顺序：只允许单向推进；回退记为顺序违规（该条目不得被判 Recovered）。
  void MarkStage(Entry* e, uint64_t attemptSerial, PaletteObjectStage stage) {
    const uint32_t s = StageRank(stage);
    // 2026-09-18 P0-2（Astra 实测修正）：**maxStage 是「该条目实际达到的最高语义秩」，
    // 与尝试关联无关**（终态阶段回填用它，见 CloseWindow）。修复前它只在「尝试号未知」
    // 分支被更新 ⇒ 各点携带同一个（或不同）已知尝试号时永不提升 ⇒ 已走到 Drawn 的链
    // 被回填成 FirstSight（Astra 实测 terminal=ObservationClosed stage=FirstSight sawDraw=true）。
    if (s > e->maxStage)
      e->maxStage = s;
    // 2026-09-18 阶段 C（K3）：尝试号**未知** ⇒ 沿用终身单调判序。
    // 2026-09-18 P0-2（复审 R3 更正）：**「行为不变」这句话已不成立** —— 上面的无条件提升
    // 让 maxStage 会被**已知尝试号**的事件抬高，因此未知号事件现在是在与「混合了已知尝试秩的
    // 终身最高秩」比较：已知号 R/S/E/D 走完后再来一条未知号 Rejected ⇒ 终态从 Recovered 变
    // Unclosed（Astra 复审独立复现 S4）。当前生产唯一的未知号站点是 D 点（秩 4 = 最高秩，
    // 永不回退）⇒ 现不可达；但**新增任何未知号低秩站点都会让条目永久失去 Recovered**。
    // 彻底修法属 P0-5（关联标识）：未知关联边界应当「不判回退但计数」，并显式 fail-visible。
    if (attemptSerial == ~0ull) {
      if (s < e->maxStage)
        e->orderViolation = true;
      if (s > e->maxStage)
        e->maxStage = s;
      return;
    }
    if (attemptSerial != e->attemptSerial) {
      // **一次新的明确关联尝试** ⇒ 重置本次尝试基线，且**不得**据此判违规。
      // 这正是反例（帧 N 走完 R/S/E/D，帧 N+1 又一次合法 Rejected）的修法：
      // 旧实现拿终身 maxStage(=4) 与新 Rejected(秩1) 比较 ⇒ 误判回退。
      e->attemptSerial = attemptSerial;
      e->attemptStage = s;
      return;
    }
    // 同一尝试内：回退**仍然**违规（裁定明确否定"每帧清零"）。
    if (s < e->attemptStage)
      e->orderViolation = true;
    if (s > e->attemptStage)
      e->attemptStage = s;
  }
  // 供阶段事件共用的准备：顺序标记 + 帧域清洗。emitted=false 表示因预算被拒（已计数）。
  PaletteObjectEventRecord PrepareStage(Entry* e, PaletteObjectStage stage,
                                        const PaletteObjectFrames& frames) {
    PaletteObjectEventRecord record = MakeRecord(*e);
    record.stage = stage;
    record.frames = frames; // 保留调用点口径（已知的域不得被强制写成 unknown）
    record.terminal = PaletteObjectTerminal::None;
    MarkStage(e, frames.attemptSerial, stage);
    return record;
  }

  // 2026-09-17 成本测量：线性探测在**接近满表**时必然退化（1024 键 / 1024 槽 ⇒ 最坏仍 ~930 次探测、~700 ns，
  // 这是负载因子而非哈希质量问题）。这里加一个 1024 位（128 B，无分配）布隆**前置否定**：
  // 因此「表满 + 新键」这类负查找、以及表满后每次 NoteReject 的全表走查都变成常数。
  // 位数必须远大于「键数 × 每位键设置的位数」（1024 键 × 2 位）：1024 位会被填满而**失去过滤能力**
  // （实测：1024 位时 1001 个未知键里仍有 771 个走满全表）。16384 位（2 KB）下填充率 ~12.5%、
  // 假阳性约 1.5% ⇒ 未知键基本都走常数级负查找。
  static constexpr uint32_t kBloomBits = 16384u;
  static constexpr uint32_t kBloomWords = kBloomBits / 64u; // 256 个字 = 2 KB
  static uint32_t BloomBitA(uint64_t h) {
    return static_cast<uint32_t>(h & (kBloomBits - 1u));
  }
  static uint32_t BloomBitB(uint64_t h) {
    return static_cast<uint32_t>((h >> 32u) & (kBloomBits - 1u));
  }
  void BloomSet(uint64_t h) {
    const uint32_t a = BloomBitA(h), b = BloomBitB(h);
    m_bloom[a >> 6u] |= 1ull << (a & 63u);
    m_bloom[b >> 6u] |= 1ull << (b & 63u);
  }
  bool BloomMight(uint64_t h) const {
    const uint32_t a = BloomBitA(h), b = BloomBitB(h);
    return (m_bloom[a >> 6u] >> (a & 63u) & 1ull) != 0ull &&
           (m_bloom[b >> 6u] >> (b & 63u) & 1ull) != 0ull;
  }

  // 探测范围必须**与 Insert 一致**（否则能插到永远查不到的位置）。
  Entry* Insert(const PaletteObjectKey& key, uint64_t frame,
                PaletteObjectRejectReason reason, const PaletteObjectFrames& frames,
                PaletteObjectChainType chainType) {
    if (m_watchCount >= kWatchCapacity)
      return nullptr;
    const uint32_t start = static_cast<uint32_t>(HashKey(key) % kWatchCapacity);
    // 与 Find 相同的完整探测范围，并优先复用墓碑。
    for (uint32_t probe = 0u; probe < kWatchCapacity; ++probe) {
      Entry& e = m_entries[(start + probe) % kWatchCapacity];
      if (e.used)
        continue;
      BloomSet(HashKey(key));
      e = Entry{};
      e.used = true;
      e.instanceId = m_nextInstanceId++; // 链实例身份（见 Entry::instanceId）
      e.key = key;
      e.firstRejectFrame = frame;
      e.chainType = chainType; // 建立时落定，此后不得更改（见 Entry::chainType 说明）
      // **不要**在这里写 lastRejectFrame：那是"本帧已发过拒绝事件"的标记，
      // 写入会让新建对象的第一条 Rejected 被同帧去重吞掉（上级 03:58 后自查发现的缺陷）。
      e.windowSegment = m_windowSegment; // 窗口也是查找到的一个分量（建立时落定）
      e.firstFrames = frames;
      e.firstReason = reason;
      m_watchCount++;
      if (key.identityWeak)
        m_counters.weakIdentityRecords++;
      if (key.epochUnknown)
        m_counters.epochUnknownRecords++;
      return &e;
    }
    return nullptr;
  }

  // 2026-09-18 阶段 C（Q2）：**按链型**查找。
  // 为什么不能写成「Find(key) + 事后类型检查」：同一对象键的两条条目**共享同一个 HashKey**，
  // 而 Find 从该起点探测并返回**第一个** SameKey 条目 ⇒ 事后过滤会永远拿到先插入的那条，
  // 目标链型**恒为 nullptr**（round 16 的实测缺陷）。因此链型必须进入**匹配谓词**：
  //   ★ 「同键但链型不同」必须 **continue 继续探测** —— 目标条目可能在更后的探测位上，
  //     不得把它当作「键不存在」而提前返回 nullptr。
  Entry* FindChain(const PaletteObjectKey& key, PaletteObjectChainType type) {
    const uint64_t h = HashKey(key);
    if (!BloomMight(h))
      return nullptr;
    const uint32_t start = static_cast<uint32_t>(h % kWatchCapacity);
    for (uint32_t probe = 0u; probe < kWatchCapacity; ++probe) {
      Entry& e = m_entries[(start + probe) % kWatchCapacity];
      if (e.used) {
        if (SameKey(e.key, key) && e.chainType == type) {
          if (e.windowSegment == m_windowSegment)
            return &e;
          // 2026-09-18 阶段 C（窗口维度）：同一 key+链型 但属于**别的窗口** ⇒ 不复用。
          // 正常情况下不可能发生（两条 re-arm 路径都清表）；一旦发生，说明有人把 re-arm
          // 改成了"保留条目" —— 此时**必须可见**（fail-visible），否则该违规是**静默**的：
          // record.windowSegment 取自当前值，导出层根本看不出条目属于哪个窗口。
          ++m_counters.windowMismatchLookups;
          continue;
        }
        continue; // 同键异链型（或异键）：继续探测
      }
      if (e.tombstone)
        continue;
      return nullptr;
    }
    // 2026-09-18 P0-6（复审 128c3826）：探测耗尽**必须计数**。`droppedProbeLimit` 是**上 wire 的
    // LOSS_FIELD**，而它唯一的自增点随旧 `Find()` 一起被删除 ⇒ 该字段恒 0、并使 cost_test 的
    // 布隆守卫（walkDelta*10 <= kFullCalls）**恒真、失去效力**（删掉布隆前置否定也照样通过）。
    // 恢复它在**唯一剩下的**键查找上的计数。
    m_counters.droppedProbeLimit++;
    return nullptr;
  }
  // 2026-09-18 P0-6（复审 dcd8c729 指出：这里遗留了**被删 FindOwner() 的相反规则**）。
  // 【现行归属规则（Astra 裁定 C1）】S/E/D 对**两条链各自**记一份（关联 = FindChain 的完整键相等 +
  // 链型相等 + 同窗口），顺序**显式固定**为先拒绝恢复链、后观察链；`NoteObjectGone` 则对两条链**都**结算。
  // 【已废弃（原文）】「优先 Observation，只有没有观察链时才挂到拒绝恢复链」—— 那正是被删除的
  // `FindOwner()` 的规则，它会让双链并存时拒绝链拿不到恢复事实（必然结算成 WindowExpired）。
  // `FindOwner`/`Find` 已删除，静态门禁 §11n(b) 断言二者**不存在**以防回归。
  bool AdmitsSession(const PaletteObjectKey& key) const {
    // 【为何不在此处用「未初始化(session==0) ⇒ 拒绝」】：记录器**不替调用方做 arm 门** ——
    // 那会破坏「default/unconfigured 仍推进状态机」的既有契约（Case 1）。
    // 「发布早于初始化」那个窗口由**控制路径的顺序**关闭（Arm 必须早于 active.store），
    // 由静态锁 test_palette_object_arm_order_static.py 钉住，而不是在这里重复实现。
    // (2) 键未携带会话代际（0）：**只在记录器自身也没有会话时**放行（未配置用法，Case 1）。
    //     这是收窄后的正确契约 —— 0 代际键在**有会话**的记录器上必须被拒。理由（Astra 复审实测形状）：
    //     sink 用 ActiveSession() 写顶层 session，而键里的 bits[10]/[29] 为 0，读方
    //     AutoTest/analyze_palette_object_evidence.py 的 `require(bits[10]/[29] == event.session)`
    //     会**拒绝整份导出**（palette session generation mirror mismatch）。由 Case 39 覆盖。
    //     更正：此前这里把保证归因到 `war3_palette_object_capture.h:159`，那是**指错了证据位置** ——
    //     该行只是把形参复制进结构体，并不保证它等于当前会话。
    if (key.sessionGeneration == 0u)
      return m_sessionGeneration == 0u;
    return key.sessionGeneration == m_sessionGeneration;
  }

  // 接纳失败时统一记账并返回 true 表示「已处理（应当 return）」。
  bool RejectIfStaleSession(const PaletteObjectKey& key) {
    if (AdmitsSession(key))
      return false;
    m_counters.droppedStaleSession++;
    return true;
  }

  // 2026-09-18 P0-6（复审 C1-1 的连带清理）：这里原有 `FindOwner()`（全局优先观察链）与
  // `Find()`（**链型无关**查找）。C1 之后 S/E/D 必须对**两条链各自**记录，二者都已无任何调用者
  // （全树只剩注释引用），故删除。**修复前的两条静默失效正是它们的后果**：
  //   · 用 `FindOwner()` ⇒ 拒绝链永远拿不到恢复事实（双链并存时必然结算成 WindowExpired）；
  //   · 用 `Find()`（类型无关）⇒ 观察链一条路径事实都拿不到，而它**能编译通过**，
  //     独立复审据此把路由退回去做变异时是**静默**成功的 —— 这正是必须删掉而不是留着的理由。
  // ⚠️ 新增采集事实时**必须**用 `FindChain(key, 链型)` 并显式处理两条链的丢失路径（具名计数）。

  // 2026-09-18 阶段 C（Q2）：**按链型**查找。两类链拥有独立条目（裁定），
  // 因此"对象键 ⇒ 条目"不再是一对一：链型必须进入查找维度。
  // **先预检**：被预算拒绝时调用方不得修改任何状态（否则 hitCount/序号会与实际事件不符）。
  bool CanEmit(bool terminal) const {
    if (m_counters.emitted >= kTotalBudget)
      return false;
    // 终态真实预留：普通事件不得占用留给终态的 512 条。
    if (!terminal && m_counters.emitted >= kNormalBudget)
      return false;
    if (terminal && m_counters.terminalEmitted >= kTerminalReserve)
      return false;
    // F1（单窗口终态上界）：**终态不受每帧 64 上限约束** —— 窗口结算是结算突发，由 512 预留约束；
    // 否则单窗口终态永远 ≤64，512 预留与 4096 总额都不可达（旧断言不可满足）。
    if (!terminal && m_frameBudgetUsed >= kPerFrameBudget)
      return false;
    return true;
  }
  void AccountDrop(bool terminal) {
    if (terminal && m_counters.terminalEmitted >= kTerminalReserve) {
      m_counters.droppedTerminalReserve++;
      return;
    }
    if (!terminal && m_frameBudgetUsed >= kPerFrameBudget) {
      m_counters.droppedPerFrame++;
      return;
    }
    m_counters.droppedPerSession++;
  }
  void EmitUnchecked(const PaletteObjectEventRecord& record, bool terminal) {
    m_frameBudgetUsed++;
    m_counters.emitted++;
    if (terminal)
      m_counters.terminalEmitted++;
    if (m_emit != nullptr)
      m_emit(m_context, record);
  }

  Entry m_entries[kWatchCapacity] = {};
  Counters m_counters = {};
  EmitFn m_emit = nullptr;
  void* m_context = nullptr;
  uint32_t m_watchCount = 0u;
  uint32_t m_frameBudgetUsed = 0u;
  uint64_t m_currentFrame = 0u;
  uint64_t m_sessionGeneration = 0u;
  uint64_t m_mapEpoch = 0u;
  // 窗口/Reset 分段序号：Reset() 置 1，ResetForSessionTransition() 递增（见两处注释）。
  // 0 = 从未开过窗口（生产路径在 arm→Reset 之前不会发射；这是一个 fail-visible 的哨兵，
  // 因为版本 2 的读方要求每条分段 >= 1）。
  uint64_t m_windowSegment = 0u;
  // 2026-09-18 P0-6（复审 266e35c4）：链实例号分配器。**永不重置**（含 Reset/ResetForSessionTransition），
  // 否则重建的条目可能与旧条目同号，判据就退化成「存在性判断」。
  uint64_t m_nextInstanceId = 1u;
  bool m_windowClosed = false;
  // 全部*状态*访问都由这把锁串行化（诊断专用路径，见 StateLock 说明）。
  // **必须是递归锁**：发射器在持锁路径内回调，而该次 append 若触发证据环的容量冻结，
  // 环会调用预冻结钩子 → ClosePaletteObjectWindow() 再次进入记录器（同一线程）。
  // 非递归锁会在「小 capacity + 大 postPresents + 该时刻恰有 palette 观测」这一窄窗口自死锁；
  // 递归锁下嵌套结算照常进行，而嵌套 append 因环已 Frozen 返回 0 并被记入 ringEvictedAfterRecord（fail-visible）。
  mutable std::recursive_mutex m_mutex;
  // 唯一无锁字段：发射器在持锁路径内回调时累加（见 NoteRingEviction）。
  std::atomic<uint64_t> m_ringLosses{0u};
  // 布隆前置否定（见 Find 说明）：只在插入置位、只在 Reset 清空。
  uint64_t m_bloom[kBloomWords] = {};
  // 表满终态是否已在本窗口公告（会话级条件，不逐对象重复）。
  bool m_tableFullAnnounced = false;
};

} // namespace dxvk::war3::tools::evidence
