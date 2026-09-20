# 对象级 palette 证据：冻结格式显式版本化 + 窗口/Reset 可识别分段（方案与实测）

> 2026-09-17。工单：上级裁定的最后两项（pair-0 之前必须闭合）：⑦冻结格式显式版本化、⑧窗口与
> Reset 的可识别分段。本文是**方案与论证**，不是状态本身；实现与门禁见文末。
> 边界口径：本文只谈 **CPU 侧诊断记录链** 的可识别性与格式合同。分段标签**不是**
> GPU 提交、**不是**像素证据，也**不证明**发生过设备 Reset。

## 0. 上级裁定原文（本树 DEVELOPMENT_CHANGELOG 2026-09-17 记录）

- ⑦ \`data[2]\` 承载 lifecycleIdentity **批准**，但冻结格式必须显式版本化；
- ⑧ \`ResetForSessionTransition\` 保留计数方向正确，但**清表 ≠ 分段**，需可识别的窗口/Reset 分段。

本轮任务书补充：通用读方、palette 读方与 history/watcher 入口统一接入；旧格式按旧合同读取
（旧实机产物必须仍可读）；未知版本拒绝；并明确「字段存在不等于身份已证明」（identityWeak 的
0 不能认证同对象恢复）。分段是**可识别性**，不得放宽或收紧认证语义。

## 1. 只读调研结论（真实源码位置）

| 事实 | 位置 |
| --- | --- |
| 对象键是**八元组**，identityWeak/epochUnknown 为必带标记 | src/d3d9/war3/tools/war3_palette_object_evidence.h:29-40 |
| SameKey/HashKey 比较/哈希全部八个字段 | 同文件 :511-536 |
| Reset（新会话，连计数一起清） | 同文件 :156-171 |
| ResetForSessionTransition（换图/设备代际：只清观察表与布隆、**保留会话计数**） | 同文件 :176-189 |
| 窗口结束只结算表内条目（Recovered 需 Served+Enqueued+Drawn、未违规、未清空、来源非 None） | 同文件 :201-248 |
| chainSequence 是**每条目**计数器（每次发射 ++e->chainSequence） | 同文件 :228/301/343/383/423/455 |
| 唯一的转换点 EncodePaletteObjectEvent()（记录 -> 冻结 wire） | src/d3d9/war3/tools/war3_palette_object_evidence_sink.cpp:56-113 |
| 生成侧入口：ClearPaletteObjectWatchlist()/ResetPaletteObjectEvidence() 都走 ResetForSessionTransition | 同文件 :126-156 |
| 导出根写点（schema 7 + effectiveConfiguration + paletteObject 块） | src/d3d9/war3/tools/war3_frame_evidence.cpp:37-88 |
| 根块字段（未版本化）：watchCount(JSON int) + counters(18 个规范十进制 u64) | 同文件 :61-65 |
| 三条采集点一律 lifecycleIdentity=0 + identityWeak=true（**没有**可靠实例生命周期证明） | src/d3d9/war3/tools/war3_palette_object_capture.h:148-166 |
| 读方离线分组键 = CHAIN_IDENTITY_FIELDS 八元组 | AutoTest/analyze_palette_object_evidence.py 的 chain_identity() |
| 通用读方（根字段**精确相等**） | AutoTest/analyze_frame_evidence.py 的 analyze() |
| history/watcher 入口只调用通用读方 | AutoTest/analyze_frame_history.py:15、AutoTest/frame_history_watch.py:14,103,104 |

### 1.1 wire 的可用槽位（data[12] / words32[48]）

- data[0] renderablePart、data[1] runtimeModelPtr、data[2] lifecycleIdentity、
  data[9] manifestFrameSerial、data[10] source、data[11] hitKey 已被占用；
  **data[3..8] 是必须为 0 的保留槽**（6 个 u64）。
- words32 已占用：0,1,2,5,10,12,14,16,18,20,21,22,23,24,25,26,27,28,29,30,31,32,33；
  **保留零**：3,4,6,7,8,9,11,13,15,17,19,34..47（16 个 u32）。
- 这两个集合由 AutoTest/test_palette_object_evidence_analysis_static.py 的
  test_reader_slot_declarations_match_the_sink_write_set 从**生产转换点实际写入集合**
  重新推导并逐位钉死（写入集合 与 保留集合 的并集必须恰好铺满 0..11 / 0..47）。

## 2. 能不能用**已有**字段或保留槽承载「窗口序号」？

### 2.1 已有字段为什么不行

| 候选 | 为什么不能当分段量 |
| --- | --- |
| sessionGeneration | 新会话（arm）才换；ResetForSessionTransition **保留**会话计数，正是缺陷场景 |
| mapEpoch | 同上：同地图内 Reset/设备代际变化可以不改地图代际 |
| deviceEpoch | 三个采集点一律记 0 + epochUnknown=true（**未知不是通配符**），无法区分 |
| frames.renderFrame | 由调用点给出；离线无法证明"帧号回绕/跳变" = 一次 Reset（会把普通丢帧当成分段） |
| hitKey / source / stage / terminal | 与窗口无关；同一实例在两个窗口里可以完全相同 |
| chainSequence | 每条目计数器。两次从 1 重新开始**确实**出现在跨 Reset 的两条链里，但它与"wire 撕裂/重排"的签名**完全相同**，而后者正是读方已有的硬拒绝条件。用它来**拆链**等于把一条严格性检查降级成启发式猜测 |
| 头块的 emitted/closed* | 会话级累计（ResetForSessionTransition 有意保留），不是窗口序号；且它们是**导出期**读取的状态，不是记录级 |

结论：**没有任何已有字段可区分同会话、同地图、deviceEpoch 未知时的 Reset 前后记录**。

### 2.2 保留槽可以承载，但那是**新增 wire 语义**（不是"用现成字段"）

保留槽在版本 1 合同下的语义是"保持为 0"。从保留槽里读出一个非零值，就是把一个**未被写方
声明**的位当成语义 —— 这正是 test_reserved_slots_must_stay_zero 与
test_reader_slot_declarations_match_the_sink_write_set 要禁止的（旧实机产物里这些位恒 0，
读方把它们当分段量只会得到"永远第 0 段"，而且等于放宽了保留位检查）。

因此正确做法是：**用显式版本把新语义声明出来**，让"保留槽"在新版本合同里**变成正式载体**，
并且只在声明了那个版本的导出上解码。版本 1（含全部旧实机产物）的保留检查**一位不放宽**。

### 2.3 最小分段方案（已实现，读方侧）

1. **载体**：data[3]（u64，全 64 位）承载 windowSegment（窗口序号，>=1）。
   理由：data[3] 是相邻的保留 u64 槽，不需要 lo/hi 拆分，不动 words32 的位拓扑。
2. **写入时机**：记录器在**发出每条记录时**按当时的窗口号写一次（记录级、写一次）。
   Reset 开第 1 个窗口，ResetForSessionTransition 递增。**不用**"导出时读当前状态"，
   因为那样冻结后的摘要会随后续 Reset 变化 —— 记录级载体保证"冻结后的摘要不再变化"。
3. **离线分组键** = 八元组 **加** windowSegment。同键跨窗口的两条链因此必然分到不同链；
   同键同窗口仍然合并（不得因为分段而放宽合并/去重语义）。
4. **一致性**：分段必须非递减（一个分段块不得在更晚的分段之后再现），否则硬拒绝。
5. **旧格式（版本 1）**：没有分段量，分组键的分段分量是 None，分组结果与旧合同**逐字节相同**。
   读方**绝不**用顺序启发式拆链：真正跨 Reset 的旧产物保持 fail-closed（见 §5 反例 A/B）。

### 2.4 必须新增的 C++ 改动（提案，**本轮未改 C++，等主线程裁定**）

见 §4 的精确 diff。读方侧已完整实现并测试；生产写入侧不写 version 时，导出就是版本 1
（登记在案的冻结形状），全部旧合同行为不变。

## 3. 显式版本化方案（已实现，读方侧）

### 3.1 合同

导出仍是 schema 7。paletteObject 块是**登记在案的根扩展**，版本判定只有一处实现
（AutoTest/analyze_frame_evidence.py 的 extension_version()）：

| 块形状 | 判定 | 语义 |
| --- | --- | --- |
| {watchCount, counters}（**无** version） | 版本 **1**（旧实机产物） | 旧合同：data[3..8]、words32[15] 仍是保留零；flags-only 身份判定语义不变 |
| {version:1, watchCount, counters} | 版本 **1**（显式声明） | 同旧合同 |
| {version:2, watchCount, counters} | 版本 **2** | 分段合同：data[3]=windowSegment（每条 >=1、非递减）、words32[15]=identityProofKind（必须是登记种类）；同对象认证额外要求**载明的**身份证明 |
| 其它任何形状 / 任何未登记整数版本 / 非整数版本 | **拒绝** | ValueError（未知版本、未知形状**显式拒绝**，静默忽略一律不允许） |

### 3.2 统一接入

- **通用读方**（analyze_frame_evidence.analyze()）自己判定扩展与版本（它同时是
  history/watcher 唯一引入的根读方，所以不能依赖扩展自己的读方被 import）；
  根字段检查从"精确等于冻结集合"改成"精确等于冻结集合 加上 **已登记**扩展字段"，
  未登记字段仍是硬错误。
- **palette 读方**（analyze_palette_object_evidence.analyze()）把**整个根**交给通用读方
  （删除了 name != 'paletteObject' 的投影绕行），并从通用读方取版本决定后续合同。
- **history/watcher 入口**（analyze_frame_history.analyze_history()、
  frame_history_watch.watch()）本来就直接调用通用读方，因此**自动**接入 ——
  修前它们对真实 palette 导出会直接 root fields mismatch。
- 旧格式：无 version 的块按旧合同读取；**没有**扩展块的老产物仍然照旧可读。

### 3.3 「字段存在 ≠ 身份已证明」

读方对每条链额外如实报告 identityBasis 与 identityProven：

| 场景 | identityBasis | identityProven | 同对象认证 |
| --- | --- | --- | --- |
| 版本 1（旧合同） | recorderFlagClaimOnly | **False** | 判定语义不变（仍是 flags 合同），但报告**明确写出**这只是记录器自己的声明，**不是**证明 |
| 版本 2 + 未载明证明种类 | wireCarriedInstanceLifecycleProof | False | 具名拒绝 identityNotProven |
| 版本 2 + 载明种类但身份值为 0 | 同上 | False | 具名拒绝 identityValueMissing |
| 版本 2 + 载明种类 + 身份值非零 + identityWeak=false + epochUnknown=false | 同上 | True | 可以认证 |

即：**版本 2 下，把 identityWeak 写成 0 本身永远不足以认证同对象恢复**；必须有"载明的
证明种类 + 按值携带的非零实例生命周期身份"。这与 war3_palette_object_capture.h:148-166
的既有事实一致（三个采集点都没有可靠实例生命周期证明，所以生产导出在版本 2 下**不会**认证
任何同对象恢复链，除非将来某个采集点真的能载明证明）。

## 4. 需要主线程裁定的 C++ 改动（精确 diff，**本轮未应用**）

### D1（1 行，无语义变化，建议先批）：把既成事实写明

src/d3d9/war3/tools/war3_frame_evidence.cpp，PaletteObjectHeaderJson()（:61-65）：

    json result=json::object();
    +  // 2026-09-17 上级裁定 ⑦：冻结格式的扩展块必须**显式**版本化。写 1 = 现行冻结形状
    +  // （data[3..8]/words32[15] 仍是保留零），读方 analyze_frame_evidence.extension_version()
    +  // 已登记该版本；旧产物（无 version 字段）按同一合同读取。
    +  result["version"]=1;
    result["watchCount"]=header.watchCount;
    result["counters"]=counters;
    return result;

影响：无。读方对 {version:1, watchCount, counters} 与 {watchCount, counters} 的判定**逐字段
相同**；往返编排测试的块字段检查已改为同时接受这两种形状并校验版本是登记整数。

### D2（分段 + 身份证明，**语义变化，需单独裁定**）

(a) src/d3d9/war3/tools/war3_palette_object_evidence.h

    struct PaletteObjectEventRecord {
      ...
      bool selectionClearedByNativeOverride = false;
    + // 2026-09-17 上级裁定 ⑧：**记录级**窗口/Reset 分段量（发出时写一次，冻结后不再变化）。
    + // 0 只在"从未开过窗口"时出现；Reset 之后的第一个窗口是 1。
    + uint64_t windowSegment = 0u;
    + // 2026-09-17 上级裁定 ⑦：**载明的**身份证明种类（0 = NoIdentityProof）。字段存在 ≠
    + // 身份已证明：没有真正的实例生命周期证明时**必须**保持 0，不得用 identityWeak=0 冒充。
    + uint32_t identityProofKind = 0u;
    };

    void Reset(uint64_t sessionGeneration, uint64_t mapEpoch) {
      StateLock guard(*this);
    + // 新会话 = 第 1 个窗口（清表**并且**开新窗口：两者不是一回事）。
    + m_windowSegment = 1u;
      for (uint32_t i = 0u; i < kWatchCapacity; ++i)

    void ResetForSessionTransition(uint64_t sessionGeneration, uint64_t mapEpoch) {
      StateLock guard(*this);
    + // 换图/设备代际/显式清表：清表 ≠ 分段 —— 这里同时开启一个新窗口，
    + // 否则同会话同地图且 deviceEpoch 未知时，Reset 前后的记录在离线分组时会相遇。
    + ++m_windowSegment;
      for (uint32_t i = 0u; i < kWatchCapacity; ++i)

    - static PaletteObjectEventRecord MakeRecord(const Entry& e) {
    + PaletteObjectEventRecord MakeRecord(const Entry& e) const {
      PaletteObjectEventRecord record{};
    + record.windowSegment = m_windowSegment;
    + record.identityProofKind = 0u; // 三个采集点都没有可靠实例生命周期证明
      record.key = e.key;

    （MakeRecord 由 static 改为 const 成员：只多读 m_windowSegment，不写状态。）
    另在 NoteReject 的**一次性 TableFull 终态**手写记录处补
    record.windowSegment = m_windowSegment;。
    尾部成员：
      uint64_t m_mapEpoch = 0u;
    + uint64_t m_windowSegment = 0u; // 窗口/Reset 分段序号（见 ResetForSessionTransition）
      bool m_windowClosed = false;

(b) src/d3d9/war3/tools/war3_palette_object_evidence_sink.cpp（EncodePaletteObjectEvent()）

      event.data[2] = record.key.lifecycleIdentity;
    + // 2026-09-17 上级裁定 ⑧：窗口/Reset 分段量（记录级、写一次）。
    + event.data[3] = record.windowSegment;
      event.data[9] = record.frames.manifestFrameSerial;

      event.bits[14] = record.key.identityWeak ? 1u : 0u;
    + // 2026-09-17 上级裁定 ⑦：载明的身份证明种类（0 = 未载明）。
    + event.bits[15] = static_cast<uint32_t>(record.identityProofKind);
      event.bits[16] = record.key.epochUnknown ? 1u : 0u;

(c) src/d3d9/war3/tools/war3_frame_evidence.cpp：result["version"]=2;（替换 D1 的 1）。

**D2 的后果（必须一起裁定）**：版本 2 下三条采集点都写 identityProofKind=0，因此
**生产导出里的任何同对象恢复链都不会被认证**（具名拒绝 identityNotProven），往返场景
A/F 的期望必须同步从"认证"改成"阶段完整但身份未证明"。这与上级"identityWeak 的 0 不能认证
同对象恢复"一致，但**是一次认证语义收紧**；本轮硬约束要求"分段不得收紧认证语义"，所以本轮
把收紧放在**新版本（版本 2）**里、旧版本保持旧合同，等主线程裁定后再决定是否把收紧也搬到
版本 1。若只批准"分段"而不批准"身份证明"，撤掉读方里 4 行的 if segmented 拒绝块即可
（版本 2 的分段、槽位、非递减检查全部不受影响）。

## 5. 反例：同键 + Reset 前后各一条完整链（修前/修后实测）

夹具：同一八元组（renderablePart/runtimeModelPtr/jHandle/rawcode/sessionGeneration/
mapEpoch/lifecycleIdentity 全同，deviceEpoch=3000 已知但**同会话同地图**），窗口 1 帧
500..600、窗口 2 帧 700..800，各自 Rejected->ServedCandidate->Enqueued->Drawn->Recovered
终态，chainSequence 各自 1..5。

| 用例 | 修前（旧读方实测） | 修后（本树读方实测） |
| --- | --- | --- |
| A 旧格式（无 version）同键两条**完整**链 | ValueError: one object may carry at most one terminal event（**整份导出**不可读，连无关链一起） | **不变**（旧合同 fail-closed：没有分段量就不许猜） |
| A2 对照：同形状但**不同键** | 2 条链、都 Recovered | 不变 |
| B 旧格式同键「上半条 + 一条完整」 | 合并成 **1** 条链：objectCount=1、conclusion=Uncovered、假 order issue chainSequenceNotStrictlyIncreasing@6 / firstRejectFrameChangedInsideChain / frozenZeroDeltaViolated@5 / deltaFramesMismatch@6,7,10 | **不变**（旧合同；证明"合并/误判"确实是旧行为） |
| C 版本 2：同键两条完整链，分段 1/2，载明证明 | ValueError: paletteObject block fields mismatch（连版本 2 都读不了） | **2 条链**、windowSegments=[1,2]、两条都 Recovered + sameObjectCertified + identityProven、chainComplete=true |
| D 版本 2 但分段缺失（全 0） | 同上被拒 | ValueError: palette window segment must be >= 1 (got 0)（不得退化成猜） |
| E 版本 2 有分段但未载明身份证明 | 同上被拒 | 2 条链、StageCompleteUncertified、具名拒绝 identityNotProven、recovered=[] |
| 通用读方读真实形状导出 | ValueError: root fields mismatch（每个真实 palette 导出） | extensions={'paletteObject':1}（旧形状）或 {'paletteObject':2}（版本 2） |

复现：仓库外探针 probe_reset_segmentation.py（逐用例 JSON 输出），见报告正文。

## 6. 未做到 / 不确定处

1. **生产写入侧未改**：C++ 未写 version，三个采集点也没有实例生命周期证明，因此**当前生产
   导出仍是版本 1**，Reset 分段在生产里仍然不可识别（只有读方合同与测试就绪）。D1/D2 待裁定。
2. **分段标签不是 Reset 的证明**：它只标识"记录器把这条记录归到哪个窗口"。离线无法从导出
   反推设备是否真的 Reset 过；不得把它写成 Reset 证据。
3. **旧产物跨 Reset 仍不可识别**：这是信息缺失，不是读方能补的。读方选择 fail-closed
   （整份导出被拒）而不是启发式拆链；若上级希望旧产物"部分可读"，那是一次**放宽**，需另行裁定。
4. **identityProven 目前在生产里恒为 False**（版本 1）；它只在版本 2 + 载明证明时为 True。
   不得把 sameObjectCertified（版本 1 的 flags 合同）读成"身份已证明"。
5. 版本 2 的证明种类只登记了 1 InstanceLifecycleIdentityProof，**尚无任何写入方**；这是
   诚实的"未实现能力"，不是已建立的身份域。
