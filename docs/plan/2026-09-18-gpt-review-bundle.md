# WarVK v1.22 阶段 A–E：**外部审核包**（自包含）

> 生成：2026-09-18。工作树：`E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914`（WarVK = 面向 Warcraft III 1.27a 的 32 位 `d3d9.dll`，DXVK 派生）。
> **本包自包含**：审核者不需要访问仓库。代码差异、门禁数字、未决问题与我的分析都在这里。

---

## 0. 给审核者的说明（**先读这一节**）

### 0.1 这是什么

一次对**对象级 palette 证据子系统**（CPU 侧诊断记录链）的改动，分 5 个阶段 A–E，
目标是让「一条链的结算」这件事在**证据层面可识别、可解释、不撒谎**。

### 0.2 我请你审什么（**5 个具体问题**，见 §7 展开）

1. **v4 版本契约**是否自洽：恒定 v4 写方 + v1/v2/v3 仍按各自版本解析 + 终态表/阶段表按版本校验，
   是否存在**我漏掉的版本无关用法**（这是本项目已犯过 4 次的缺陷类）？
2. **双链语义**：我的结论「只有『两条链各自记一份』(选项 C) 能同时满足两条链的结算条件」
   是否成立？见 §5.2（含**对照/处理实测**）。
3. **窗口维度**：它的守卫**只在记录器内部可见、不上 wire**，我把它记为「未闭合」。
   见 §5.1 —— 你认为这是否必须导出才对得起「查找维度含窗口」这一要求？
4. **我的两处代码改动**是否**超出了「只改证据采集」的边界**（我在 §3.4 主动标出 E 阶段动了渲染文件里的局部代码）？
5. **我列出的"不声称"**（§6）是否有**遗漏的过度声称**？即：我是否在某个地方把"观察到位"说成了"问题已解决"？

### 0.3 可信度分级（**请按这个分级使用本包**）

| 内容 | 可信度 |
| --- | --- |
| §4 的门禁数字 | **可复跑验证**（我给了确切命令与测试名），同一棵树同日实测 |
| §5.2 的对照/处理实测 | **可复跑**（宿主机用例 35，纯 print + 特征化断言） |
| §3 的 diff | **逐字来自 `git diff --no-index`** 对不可变基线包 |
| §5 的"应该怎么选" | **我的分析，不是事实**；我**没有**自行决定（本项目 round 66 我自行决定过一次，结论是错的并已撤回） |
| 任何**实机/玩家前台**结论 | **不存在** —— 本改动**零实机采集**，见 §6 |

---

## 1. 背景：这条链在做什么

游戏在 CPU 侧记录「某个地图对象被拒绝成为阴影 caster、之后又被接住 / 或只是被正常观察到」的过程。
每次记录写成 wire 事件；一个对象的一段过程叫**一条链**；链结束时给一个**终态**。

```
阶段(Stage)  : Rejected=1  ServedCandidate=2  Enqueued=3  Drawn=4  FirstSight=5
终态(Terminal): None=0 Recovered=1 WindowExpired=2 ObjectGone=3 TableFull=4
                EventLost=5 Unclosed=6  ← 本次新增 ObservationClosed=7（0..6 一位不动）
链型(ChainType): RejectionRecovery=0  Observation=1
查找维度      : 对象键 × 链型 × 窗口
```

**语义要点（本次的判据来源）**

- `Recovered` 的语义是「**先失去、后接住**」⇒ 必须由**真实拒绝事实**支撑；
  由 `NoteFirstSight` 建立的**观察链**因此**永不**可以使用 `Recovered`。
- 新增 `ObservationClosed` 只表示「**观察结算了**」——它**不**表示对象已证明、更**不**表示阴影恢复。

---

## 2. 硬约束（改动不得越过）

```
1. 不改变 palette 准入判据 / 几何选择 / 渲染优先级
2. 不以「关闭动态 caster」或「减少正常阴影」作为修复
3. 两个 P0（B/C 与 D）完成前：不新增实机因果结论、不晋升稳定候选
4. 不声称「全门禁通过」
5. 不把隔离桌面数据当作玩家前台性能
6. 不使用 git 写操作
7. 不得为通过而放宽判据（不改期望/不删断言/不登记错误期望）
8. 一张表不代表一种事实；一条链结算不代表观察完整；观察完整不代表对象已证明；
   更不代表阴影已经恢复
```

---

## 3. 变更清单（对照**不可变基线包**，`git diff --no-index`）

### 3.1 规模：产品侧**只有 9 个文件**被改，零新增/删除

```
d3d9_device.cpp                              19    4     ← 渲染路径（见 §3.4）
war3_shadow_renderer_core.cpp                 4    1     ← 渲染路径（见 §3.4）
war3_palette_object_evidence.h              219   72     ← 证据记录器（核心）
war3_palette_object_evidence_sink.cpp        20   10     ← 证据发射到 wire
war3_palette_object_capture.h                12    1     ← 帧域工厂
war3_frame_evidence.cpp                      14    8     ← 会话/发布协议 + 头块
war3_palette_object_evidence_test.cpp       597    0     ← 宿主机测试（34 用例）
war3_palette_object_wire_roundtrip_test.cpp  24    0     ← 往返测试载体
war3_palette_object_evidence_cost_test.cpp    9    2     ← 成本测试夹具
（AutoTest 侧：6 个脚本被改 + 5 个新增静态锁/装置；未逐一列 diff）
```

### 3.2 产品 diff（6 个产品文件，**自带**）

```diff
===== FILE: src/d3d9/war3/tools/war3_palette_object_evidence.h =====
diff --git "a/BASELINE" "b/TREE"
index d1096ab..925ac54 100644
--- a/BASELINE
+++ b/TREE
@@ -48,6 +48,13 @@ struct PaletteObjectFrames {
   uint64_t nativeFrameTag = 0u;
   bool manifestUnknown = true;
   bool nativeUnknown = true;
+  // 2026-09-18 阶段 C（K3）：**一次明确关联的尝试**的序号（按值携带，裁定要求）。
+  // 哨兵 `~0ull` = **未知**：本事件无法归属到某一次尝试。
+  // ⚠️ 默认未知 ⇒ MarkStage 保持**既有的终身单调判序** ⇒ 生产行为一位不变；
+  // 只有调用方**显式**给出已知尝试号时，判序才改为「尝试内单调」。
+  // 为什么必须有未知哨兵：若无未知值，实现者只能用 0 冒充，而 0 会与
+  // 「第一次尝试」混淆 ⇒ MarkStage 会据此**错误重置**。
+  uint64_t attemptSerial = ~0ull;
 };
 
 enum class PaletteObjectStage : uint32_t {
@@ -79,6 +86,12 @@ enum class PaletteObjectSource : uint32_t {
 enum class PaletteObjectTerminal : uint32_t {
   None = 0u, Recovered = 1u, WindowExpired = 2u, ObjectGone = 3u,
   TableFull = 4u, EventLost = 5u, Unclosed = 6u,
+  // 2026-09-18 阶段 C（复核 Q3）：**Observation 链的结算终态**，替代 `Unclosed`。
+  // 含义**仅**为「观察已结算」：**不表示**阶段齐全、**不表示**身份已证明、
+  // **不表示** palette 已消费、更**不表示**画面已恢复。
+  // 取 7：0..6 一位不动；v1/v2/v3 读方的终态表不含 7 ⇒ 遇到即整份拒绝。
+  // ⚠️ 分支顺序（Case 24 的约束）：「从未被接住」仍是 WindowExpired，**先于**本终态判定。
+  ObservationClosed = 7u,
 };
 enum class PaletteObjectRejectReason : uint32_t {
   R0 = 0u, R1 = 1u, R2 = 2u, R3 = 3u,
@@ -96,6 +109,16 @@ enum class PaletteObjectIdentityProofKind : uint32_t {
   InstanceLifecycleIdentityProof = 1u,
 };
 
+// 2026-09-18 阶段 C（复核 D 定案）：诊断链的**类型**。
+// 两类链拥有**不同的事实依据**，因此互不冒充、互不改类：
+//   · RejectionRecovery —— 依据「真的发生了拒绝」（NoteReject 建立）；
+//   · Observation       —— 依据「真的观测到了 caster 候选」（NoteFirstSight 建立）。
+// 两者都是事实，故都不是伪造；但一条事实链不得因为另一类事件到达而改变自己的类型。
+// 位置说明：完整定义必须**早于**记录结构（PaletteObjectEventRecord 携带链型且带默认值）。
+enum class PaletteObjectChainType : uint32_t {
+  RejectionRecovery = 0u,
+  Observation = 1u,
+};
 struct PaletteObjectEventRecord {
   PaletteObjectStage stage = PaletteObjectStage::Rejected;
   PaletteObjectTerminal terminal = PaletteObjectTerminal::None;
@@ -109,6 +132,10 @@ struct PaletteObjectEventRecord {
   uint64_t deltaFrames = 0u;
   uint32_t hitCount = 0u;
   uint32_t chainSequence = 0u;
+  // 2026-09-18 阶段 C（Q2）：事件的**链型**。本轮只到记录层（MakeRecord 盖章），
+  // **尚未上 wire** —— 编码槽位 data[4] 与读方分组键是下一步，二者必须同时落地，
+  // 否则同一对象的两条条目会被读方按对象键并成一条链（见 Entry::chainType 说明）。
+  PaletteObjectChainType chainType = PaletteObjectChainType::RejectionRecovery;
   bool sawSubmit = false;
   bool sawDraw = false;
   // native draw-time override 清空 inputSkinSelection 时为 true（记录空值 + 原因，不补回早先值）。
@@ -157,6 +184,12 @@ class PaletteObjectEvidence {
     uint64_t closedTableFull = 0u;
     uint64_t closedEventLost = 0u;
     uint64_t closedUnclosed = 0u;
+    // 2026-09-18 阶段 C（窗口维度）：查找时遇到"同 key+链型 但别的窗口"的次数。
+    // **故意不写入 wire**（裁定要求新写方一律 v4；加计数器会触版本表）。
+    uint64_t windowMismatchLookups = 0u;
+    // 2026-09-18 阶段 C：ObservationClosed 的**独立**结算桶。
+    // 不得让它落进 closedUnclosed —— 那是一个**错误的标签**（它恰恰不是 unclosed）。
+    uint64_t closedObservationClosed = 0u;
     uint64_t weakIdentityRecords = 0u;
     uint64_t epochUnknownRecords = 0u;
     // 2026-09-18 批次 3（正常观察链）：首见**建立新条目**的次数（正常链是否真被建立）
@@ -252,14 +285,27 @@ class PaletteObjectEvidence {
       if (!e.used)
         continue;
       PaletteObjectTerminal terminal = PaletteObjectTerminal::Unclosed;
-      const bool closedChain = e.sawServed && e.sawSubmit && e.sawDraw &&
-                               !e.orderViolation &&
+      // 2026-09-18 阶段 C（复核 K2 定案）：`Recovered` 的语义是「**拒绝之后又被接住**」，
+      // 因此它必须由**真实拒绝事实**支撑（`firstReason != NotChecked`）。
+      // 原条件里**没有任何拒绝项** ⇒ 一条**从未被拒绝过**的链只要走到 S/E/D 就会被标成
+      // Recovered —— 而「恢复」恰恰预设了「先失去」。观察链（由 NoteFirstSight 建立，
+      // firstReason=NotChecked）因此**永不**可以使用 Recovered。
+      // 这不是收紧既有拒绝恢复链的行为：由 NoteReject 建立的条目其 firstReason 恒为
+      // 真实拒绝原因，故它们的判定一位不变。
+      const bool hasRejectFact =
+          e.firstReason != PaletteObjectRejectReason::NotChecked;
+      const bool closedChain = hasRejectFact && e.sawServed && e.sawSubmit &&
+                               e.sawDraw && !e.orderViolation &&
                                !e.drawSelectionCleared &&
                                e.drawSource != PaletteObjectSource::None;
       if (closedChain)
         terminal = PaletteObjectTerminal::Recovered;
       else if (e.hitCount == 0u && !e.sawServed)
         terminal = PaletteObjectTerminal::WindowExpired;
+      // 阶段 C：**必须排在 WindowExpired 之后** —— Case 24 断言「从未被拒绝且从未被接住」
+      // 的对象以 WindowExpired 结算。本分支只接管过去会落到 `Unclosed` 的那一档。
+      else if (e.chainType == PaletteObjectChainType::Observation)
+        terminal = PaletteObjectTerminal::ObservationClosed;
       PaletteObjectEventRecord record = MakeRecord(e);
       // 2026-09-18 批次 3：终态记录原先**无条件**写 stage=Drawn —— 对只推进到入队的
       // 首见链，那等于把「候选已入队」谎报成「绘制命令已记录」。此处按该条目**实际
@@ -288,6 +334,8 @@ class PaletteObjectEvidence {
         m_counters.closedRecovered++;
       else if (terminal == PaletteObjectTerminal::WindowExpired)
         m_counters.closedWindowExpired++;
+      else if (terminal == PaletteObjectTerminal::ObservationClosed)
+        m_counters.closedObservationClosed++;
       else
         m_counters.closedUnclosed++;
       e = Entry{};
@@ -302,42 +350,22 @@ class PaletteObjectEvidence {
     if (m_windowClosed)
       return;
     BeginFrame(frames.renderFrame);
-    Entry* e = Find(key);
-    if (e == nullptr) {
-      e = Insert(key, frames.renderFrame, reason, frames);
-      if (e == nullptr) {
-        // 表满 / 窗口内无可插入槽：该对象无法被跟踪 ⇒ 记 TableFull 终态（仍受终态预留约束）。
-        // droppedTableFull 是"表满"这一事实本身（与预算无关）；closedTableFull 表示"终态真的发出"。
-        m_counters.droppedTableFull++;
-        // 审计 D1：表满时该对象**没有条目**，无法逐对象去重；而解析器要求「每个对象至多一条终态」。
-        // 因此 TableFull 终态**每个窗口只公告一次**（表满是会话级条件），其余只累加 droppedTableFull。
-        if (m_tableFullAnnounced)
-          return;
-        m_tableFullAnnounced = true;
-        PaletteObjectEventRecord record{};
-        record.stage = PaletteObjectStage::Rejected;
-        record.terminal = PaletteObjectTerminal::TableFull;
-        record.rejectReason = reason;
-        record.key = key;
-        // 2026-09-17 上级裁定 ⑦⑧：这条一次性（每窗口至多一次）TableFull 终态**不经过**
-        // MakeRecord()，因此必须在这里同样盖上记录级分段与载明的身份证明种类（同一推导规则）。
-        record.windowSegment = m_windowSegment;
-        record.identityProofKind = DeriveIdentityProofKind(key);
-        record.frames = frames;
-        record.firstRejectFrame = frames.renderFrame;
-        record.deltaFrames = 0u; // 终态契约：renderFrame - firstRejectFrame（同一帧 ⇒ 0）
-        record.chainSequence = 1u;
-        if (!CanEmit(true)) {
-          AccountDrop(true);
-          return;
-        }
-        // F4：只有真的发出的终态才计入 closedTableFull（与 CloseWindow 的 F2 对齐）。
-        m_counters.closedTableFull++;
-        EmitUnchecked(record, true);
-        return;
-      }
-    }
-    if (e->lastRejectFrame == frames.renderFrame) {
+    Entry* e = FindChain(key, PaletteObjectChainType::RejectionRecovery);
+    // 2026-09-18 复核（Astra）P1 / 本计划阶段 C：**预算预检必须提前到 Insert 之前**。
+    // 原实现先建条目、后判非终态预算 ⇒ 预算拒发时留下**无链首条目**（该条目最终只有一个
+    // stage=Drawn 的终态；复核给出的第 65 键反例）。重排后「条目存在 ⇒ 链首已发出」对
+    // **两类入口**都成立，即两类链共用同一条准入规则。
+    // 顺序要点：**去重 → 表满 → 预算 → 建条目 → 发射**。表满必须**先于**预算判定，
+    // 因为 TableFull 是**终态**，受终态预留（CanEmit(true)）而非非终态预算约束（F1）；
+    // 若先判非终态预算就返回，会在非终态预算耗尽时**悄悄不再公告表满**。
+    // 2026-09-18 阶段 C：**"先到先得"已被证伪并撤回**（见 Case 8 的 4 处失败 —— 拒绝建立的
+    // 条目再也发不出观察链首，等于把观察链整条丢掉）。裁定要求的是**另建一条独立条目**
+    // （inserted=1），因此本条目的类型化**查找键**（对象键 × 链型）与 v4 链型导出必须同时落地，
+    // **已落地（2026-09-18 阶段 C）**：`chainType` 现已**参与查找与结算** —— 查找谓词见
+    // `FindChain`（`SameKey(e.key,key) && e.chainType == type`）、结算分支见 `CloseWindow`
+    // 的 Observation 分档，并由宿主机用例 27 / 28 / 35（两链独立、单链形状、双链路由）覆盖。
+    // 上句的"必须同时落地"因此**已满足**；本条注释在被改写前曾长期写着"尚未参与查找"（已过期）。
+    if (e != nullptr && e->lastRejectFrame == frames.renderFrame) {
       // D6：同类拒绝原因才算「已证明等价的重复」；原因不同则计入可见冲突。
       if (e->lastRejectReason == reason)
         m_counters.droppedDuplicatePerFrame++;
@@ -345,13 +373,54 @@ class PaletteObjectEvidence {
         m_counters.droppedPayloadConflict++;
       return;
     }
+    if (e == nullptr && m_watchCount >= kWatchCapacity) {
+      // 表满 / 窗口内无可插入槽：该对象无法被跟踪 ⇒ 记 TableFull 终态（仍受终态预留约束）。
+      // droppedTableFull 是"表满"这一事实本身（与预算无关）；closedTableFull 表示"终态真的发出"。
+      m_counters.droppedTableFull++;
+      // 审计 D1：表满时该对象**没有条目**，无法逐对象去重；而解析器要求「每个对象至多一条终态」。
+      // 因此 TableFull 终态**每个窗口只公告一次**（表满是会话级条件），其余只累加 droppedTableFull。
+      if (m_tableFullAnnounced)
+        return;
+      m_tableFullAnnounced = true;
+      PaletteObjectEventRecord record{};
+      record.stage = PaletteObjectStage::Rejected;
+      record.terminal = PaletteObjectTerminal::TableFull;
+      record.rejectReason = reason;
+      record.key = key;
+      // 2026-09-17 上级裁定 ⑦⑧：这条一次性（每窗口至多一次）TableFull 终态**不经过**
+      // MakeRecord()，因此必须在这里同样盖上记录级分段与载明的身份证明种类（同一推导规则）。
+      record.windowSegment = m_windowSegment;
+      record.identityProofKind = DeriveIdentityProofKind(key);
+      record.frames = frames;
+      record.firstRejectFrame = frames.renderFrame;
+      record.deltaFrames = 0u; // 终态契约：renderFrame - firstRejectFrame（同一帧 ⇒ 0）
+      record.chainSequence = 1u;
+      if (!CanEmit(true)) {
+        AccountDrop(true);
+        return;
+      }
+      // F4：只有真的发出的终态才计入 closedTableFull（与 CloseWindow 的 F2 对齐）。
+      m_counters.closedTableFull++;
+      EmitUnchecked(record, true);
+      return;
+    }
     if (!CanEmit(false)) {
       AccountDrop(false);
       return;
     }
+    if (e == nullptr) {
+      // 表未满且预算已通过 ⇒ 建条目**必然成功**（Insert 的唯一失败原因是表满，上面已判）。
+      // 保留防御分支：若将来 Insert 新增失败原因，这里必须一并更新，不得静默继续。
+      e = Insert(key, frames.renderFrame, reason, frames,
+                 PaletteObjectChainType::RejectionRecovery);
+      if (e == nullptr) {
+        m_counters.droppedTableFull++;
+        return;
+      }
+    }
     e->lastRejectFrame = frames.renderFrame;
     e->lastRejectReason = reason;
-    MarkStage(e, PaletteObjectStage::Rejected);
+    MarkStage(e, frames.attemptSerial, PaletteObjectStage::Rejected);
     PaletteObjectEventRecord record = MakeRecord(*e);
     record.stage = PaletteObjectStage::Rejected;
     record.rejectReason = reason;
@@ -382,9 +451,12 @@ class PaletteObjectEvidence {
     if (m_windowClosed)
       return;
     BeginFrame(frames.renderFrame);
-    Entry* e = Find(key);
+    Entry* e = FindChain(key, PaletteObjectChainType::Observation);
     // 先处理「已发过链首」的重复观测（最廉价、不消耗配额）。
     // 修正② 把预算预检与此合并到 Insert 之前，见下方说明。
+    // 2026-09-18 阶段 C：**"先到先得"已撤回**（Case 8 的 4 处失败证明它会丢掉整条观察链）。
+    // 正确修法 = 类型化查找键（对象键 × 链型）+ v4 链型导出，使两类链**各自独立存在**；
+    // 在那之前，本函数保持既有行为（对已存在的拒绝条目复用），缺陷**如实记为未修**。
     if (e != nullptr && e->sawFirstSight) {
       // 2026-09-18 独立复审 D 定案：去重维度**原本是 (对象键, renderFrame)**，
       // 而 `lastFirstSightFrame` 只在**真的发射**时更新 ⇒ 跨帧不去重 ⇒ 同一对象每帧再发一条
@@ -414,8 +486,8 @@ class PaletteObjectEvidence {
     if (e == nullptr) {
       // 首见＝该键的链首，故 firstReason 记 NotChecked（本链**没有**拒绝事实）。
       // 放在预算预检**之后**：只有确实要发射时才建条目。
-      e = Insert(key, frames.renderFrame, PaletteObjectRejectReason::NotChecked,
-                 frames);
+      e = Insert(key, frames.renderFrame, PaletteObjectRejectReason::NotChecked, frames,
+                 PaletteObjectChainType::Observation);
       if (e == nullptr) {
         m_counters.droppedTableFull++;
         return;
@@ -448,7 +520,7 @@ class PaletteObjectEvidence {
     if (m_windowClosed)
       return;
     BeginFrame(frames.renderFrame);
-    Entry* e = Find(key);
+    Entry* e = FindOwner(key);
     if (e == nullptr)
       return;
     if (e->lastServedFrame == frames.renderFrame) {
@@ -495,7 +567,7 @@ class PaletteObjectEvidence {
     if (m_windowClosed)
       return;
     BeginFrame(frames.renderFrame);
-    Entry* e = Find(key);
+    Entry* e = FindOwner(key);
     if (e == nullptr)
       return;
     if (e->lastSubmitFrame == frames.renderFrame) {
@@ -540,7 +612,7 @@ class PaletteObjectEvidence {
     if (m_windowClosed)
       return;
     BeginFrame(frames.renderFrame);
-    Entry* e = Find(key);
+    Entry* e = FindOwner(key);
     if (e == nullptr)
       return;
     if (e->lastDrawFrame == frames.renderFrame) {
@@ -583,7 +655,7 @@ class PaletteObjectEvidence {
     StateLock guard(*this);
     if (m_windowClosed)
       return;
-    Entry* e = Find(key);
+    Entry* e = FindOwner(key);
     if (e == nullptr)
       return;
     PaletteObjectEventRecord record = MakeRecord(*e);
@@ -671,8 +743,19 @@ class PaletteObjectEvidence {
  private:
   // 精简条目（不嵌入完整记录），1024 槽无堆分配。
   struct Entry {
+    // 2026-09-18 阶段 C（窗口维度）：条目所属窗口（建立时落定）。
+    uint64_t windowSegment = 0u;
     PaletteObjectKey key{};
     PaletteObjectFrames firstFrames{};
+    // 2026-09-18 阶段 C（复核 D 定案）：**链型在条目建立时确定，此后不得更改**。
+    // 背景：修复前 `NoteFirstSight` 对"已由拒绝建立的条目"**不 Insert**，直接在该条目上
+    // 发链首 ⇒ 拒绝恢复链被**改类**成观察链（复核实测 inserted=0 / emitted=1）。
+    // 两类链的事实依据不同（"真的发生了拒绝" vs "真的观测到了候选"），不得互相冒充，
+    // 因此本轮采用**先到先得**：某键先由哪类入口建立，其后另一类入口**不得改类**，
+    // 只能被**显式计入**（droppedPayloadConflict = 同对象的可见冲突）。
+    // 完整形态（同一对象**同时**持有两条独立条目 + v4 导出链型）是下一刀；
+    // 在链型尚未上 wire 之前不得让两条同类键条目并存，否则读方会按对象键把它们并成一条链。
+    PaletteObjectChainType chainType = PaletteObjectChainType::RejectionRecovery;
     uint64_t firstRejectFrame = 0u;
     uint64_t lastRejectFrame = kNoFrame;
     // D6：同帧去重必须比较**载荷**（拒绝原因），否则会把不同事实当成重复吞掉。
@@ -692,7 +775,9 @@ class PaletteObjectEvidence {
     PaletteObjectSource drawSource = PaletteObjectSource::None;
     uint32_t hitCount = 0u;
     uint32_t chainSequence = 0u;
-    uint32_t maxStage = 0u;
+    uint32_t maxStage = 0u;      // 既有：**终身单调**（未知尝试号时仍按它判序）
+  uint64_t attemptSerial = ~0ull; // 最近一次**已知**尝试号
+  uint32_t attemptStage = 0u;     // **本次尝试内**的最高秩（K3）
     bool sawFirstSight = false;
     bool sawServed = false;
     bool sawSubmit = false;
@@ -733,6 +818,7 @@ class PaletteObjectEvidence {
     record.sawSubmit = e.sawSubmit;
     record.sawDraw = e.sawDraw;
     record.chainSequence = e.chainSequence;
+    record.chainType = e.chainType; // 链型随事件导出（记录层）
     return record;
   }
 
@@ -762,7 +848,7 @@ class PaletteObjectEvidence {
     }
     return 0u;
   }
-  // 语义秩 ⇒ 阶段（终态记录回填用；见 CloseWindow 的版本 3 分支）。
+  // 语义秩 ⇒ 阶段（终态记录回填用；见 CloseWindow 里的 `e.sawFirstSight ? StageOfRank(e.maxStage) : Drawn`）。
   static PaletteObjectStage StageOfRank(uint32_t rank) {
     switch (rank) {
       case 4u:
@@ -778,12 +864,29 @@ class PaletteObjectEvidence {
     }
   }
   // 阶段顺序：只允许单向推进；回退记为顺序违规（该条目不得被判 Recovered）。
-  void MarkStage(Entry* e, PaletteObjectStage stage) {
+  void MarkStage(Entry* e, uint64_t attemptSerial, PaletteObjectStage stage) {
     const uint32_t s = StageRank(stage);
-    if (s < e->maxStage)
+    // 2026-09-18 阶段 C（K3）：尝试号**未知** ⇒ 保持既有终身单调判序（行为不变）。
+    if (attemptSerial == ~0ull) {
+      if (s < e->maxStage)
+        e->orderViolation = true;
+      if (s > e->maxStage)
+        e->maxStage = s;
+      return;
+    }
+    if (attemptSerial != e->attemptSerial) {
+      // **一次新的明确关联尝试** ⇒ 重置本次尝试基线，且**不得**据此判违规。
+      // 这正是反例（帧 N 走完 R/S/E/D，帧 N+1 又一次合法 Rejected）的修法：
+      // 旧实现拿终身 maxStage(=4) 与新 Rejected(秩1) 比较 ⇒ 误判回退。
+      e->attemptSerial = attemptSerial;
+      e->attemptStage = s;
+      return;
+    }
+    // 同一尝试内：回退**仍然**违规（裁定明确否定"每帧清零"）。
+    if (s < e->attemptStage)
       e->orderViolation = true;
-    if (s > e->maxStage)
-      e->maxStage = s;
+    if (s > e->attemptStage)
+      e->attemptStage = s;
   }
   // 供阶段事件共用的准备：顺序标记 + 帧域清洗。emitted=false 表示因预算被拒（已计数）。
   PaletteObjectEventRecord PrepareStage(Entry* e, PaletteObjectStage stage,
@@ -792,7 +895,7 @@ class PaletteObjectEvidence {
     record.stage = stage;
     record.frames = frames; // 保留调用点口径（已知的域不得被强制写成 unknown）
     record.terminal = PaletteObjectTerminal::None;
-    MarkStage(e, stage);
+    MarkStage(e, frames.attemptSerial, stage);
     return record;
   }
 
@@ -823,27 +926,9 @@ class PaletteObjectEvidence {
   }
 
   // 探测范围必须**与 Insert 一致**（否则能插到永远查不到的位置）。
-  Entry* Find(const PaletteObjectKey& key) {
-    const uint64_t h = HashKey(key);
-    if (!BloomMight(h))
-      return nullptr; // 两位都未置位 ⇒ 键**肯定**不存在（负查找常数化）
-    const uint32_t start = static_cast<uint32_t>(h % kWatchCapacity);
-    for (uint32_t probe = 0u; probe < kWatchCapacity; ++probe) {
-      Entry& e = m_entries[(start + probe) % kWatchCapacity];
-      if (e.used) {
-        if (SameKey(e.key, key))
-          return &e;
-        continue;
-      }
-      if (e.tombstone)
-        continue; // 删除空洞：必须继续探测，否则后继条目不可达
-      return nullptr; // 真正的空槽：键不存在
-    }
-    m_counters.droppedProbeLimit++;
-    return nullptr;
-  }
   Entry* Insert(const PaletteObjectKey& key, uint64_t frame,
-                PaletteObjectRejectReason reason, const PaletteObjectFrames& frames) {
+                PaletteObjectRejectReason reason, const PaletteObjectFrames& frames,
+                PaletteObjectChainType chainType) {
     if (m_watchCount >= kWatchCapacity)
       return nullptr;
     const uint32_t start = static_cast<uint32_t>(HashKey(key) % kWatchCapacity);
@@ -857,8 +942,10 @@ class PaletteObjectEvidence {
       e.used = true;
       e.key = key;
       e.firstRejectFrame = frame;
+      e.chainType = chainType; // 建立时落定，此后不得更改（见 Entry::chainType 说明）
       // **不要**在这里写 lastRejectFrame：那是"本帧已发过拒绝事件"的标记，
       // 写入会让新建对象的第一条 Rejected 被同帧去重吞掉（上级 03:58 后自查发现的缺陷）。
+      e.windowSegment = m_windowSegment; // 窗口也是查找到的一个分量（建立时落定）
       e.firstFrames = frames;
       e.firstReason = reason;
       m_watchCount++;
@@ -870,6 +957,69 @@ class PaletteObjectEvidence {
     }
     return nullptr;
   }
+
+  // 2026-09-18 阶段 C（Q2）：**按链型**查找。
+  // 为什么不能写成「Find(key) + 事后类型检查」：同一对象键的两条条目**共享同一个 HashKey**，
+  // 而 Find 从该起点探测并返回**第一个** SameKey 条目 ⇒ 事后过滤会永远拿到先插入的那条，
+  // 目标链型**恒为 nullptr**（round 16 的实测缺陷）。因此链型必须进入**匹配谓词**：
+  //   ★ 「同键但链型不同」必须 **continue 继续探测** —— 目标条目可能在更后的探测位上，
+  //     不得把它当作「键不存在」而提前返回 nullptr。
+  Entry* FindChain(const PaletteObjectKey& key, PaletteObjectChainType type) {
+    const uint64_t h = HashKey(key);
+    if (!BloomMight(h))
+      return nullptr;
+    const uint32_t start = static_cast<uint32_t>(h % kWatchCapacity);
+    for (uint32_t probe = 0u; probe < kWatchCapacity; ++probe) {
+      Entry& e = m_entries[(start + probe) % kWatchCapacity];
+      if (e.used) {
+        if (SameKey(e.key, key) && e.chainType == type) {
+          if (e.windowSegment == m_windowSegment)
+            return &e;
+          // 2026-09-18 阶段 C（窗口维度）：同一 key+链型 但属于**别的窗口** ⇒ 不复用。
+          // 正常情况下不可能发生（两条 re-arm 路径都清表）；一旦发生，说明有人把 re-arm
+          // 改成了"保留条目" —— 此时**必须可见**（fail-visible），否则该违规是**静默**的：
+          // record.windowSegment 取自当前值，导出层根本看不出条目属于哪个窗口。
+          ++m_counters.windowMismatchLookups;
+          continue;
+        }
+        continue; // 同键异链型（或异键）：继续探测
+      }
+      if (e.tombstone)
+        continue;
+      return nullptr;
+    }
+    return nullptr;
+  }
+  // S/E/D/NoteObjectGone 的**归属规则**（显式，不得靠默认）：优先 Observation，其次
+  // RejectionRecovery。理由：S/E/D 记录的是「候选被服务/入队/绘制」这一**路径观察**事实，
+  // 观察链正是它的载体；只有该对象**没有**观察链时，这些事实才挂到拒绝恢复链上。
+  Entry* FindOwner(const PaletteObjectKey& key) {
+    Entry* observed = FindChain(key, PaletteObjectChainType::Observation);
+    return observed != nullptr ? observed
+                               : FindChain(key, PaletteObjectChainType::RejectionRecovery);
+  }
+  Entry* Find(const PaletteObjectKey& key) {
+    const uint64_t h = HashKey(key);
+    if (!BloomMight(h))
+      return nullptr; // 两位都未置位 ⇒ 键**肯定**不存在（负查找常数化）
+    const uint32_t start = static_cast<uint32_t>(h % kWatchCapacity);
+    for (uint32_t probe = 0u; probe < kWatchCapacity; ++probe) {
+      Entry& e = m_entries[(start + probe) % kWatchCapacity];
+      if (e.used) {
+        if (SameKey(e.key, key))
+          return &e;
+        continue;
+      }
+      if (e.tombstone)
+        continue; // 删除空洞：必须继续探测，否则后继条目不可达
+      return nullptr; // 真正的空槽：键不存在
+    }
+    m_counters.droppedProbeLimit++;
+    return nullptr;
+  }
+
+  // 2026-09-18 阶段 C（Q2）：**按链型**查找。两类链拥有独立条目（裁定），
+  // 因此"对象键 ⇒ 条目"不再是一对一：链型必须进入查找维度。
   // **先预检**：被预算拒绝时调用方不得修改任何状态（否则 hitCount/序号会与实际事件不符）。
   bool CanEmit(bool terminal) const {
     if (m_counters.emitted >= kTotalBudget)
===== FILE: src/d3d9/war3/tools/war3_palette_object_evidence_sink.cpp =====
diff --git "a/BASELINE" "b/TREE"
index 62e81d6..8fd34c7 100644
--- a/BASELINE
+++ b/TREE
@@ -13,6 +13,8 @@
 //   session -> key.sessionGeneration（并在 words32[10]/[29] 镜像）
 //   frame -> frames.renderFrame；mapEpoch/deviceEpoch 直通；thread == words32[12]
 //   data[0] renderablePart  data[1] runtimeModelPtr  data[2] lifecycleIdentity
+//   data[4] chainType（2026-09-18 阶段 C：版本 4 声明的**记录级**链型载体；版本 ≤3 读方
+//             仍要求该槽为保留零 ⇒ 旧产物合同一位不放宽）
 //   data[3] windowSegment（2026-09-17 上级裁定 ⑧：版本 2 声明的**记录级**分段载体；版本 1 读方
 //   仍把该槽当保留零，所以只有头块 version=2 的导出才按分段合同解析）  data[4..8] 必须为 0
 //   data[9] manifestFrameSerial  data[10] source  data[11] hitKey
@@ -29,7 +31,11 @@ namespace dxvk::war3::tools::evidence {
 namespace {
 
 PaletteObjectEvidence g_paletteObjectEvidence;
-bool g_paletteObjectArmed = false;
+// 2026-09-18 阶段 D（批次 2）：**同步域**。原为裸 `bool`，而记录器的初始化是在它自己的
+// `StateLock` 下完成的 ⇒ 写者（ArmPaletteObjectEvidence 末尾）与 7 个读者之间**没有
+// acquire/release 配对** ⇒ 弱内存序下读者可能看到 armed==true 却看不到记录器已初始化。
+// 改为 atomic + release/acquire 配对：armed 的发布**同时发布**记录器的初始化。
+std::atomic<bool> g_paletteObjectArmed{false};
 // 2026-09-18 独立复审 R3：这五个到达计数器由**渲染线程写、控制面客户线程读**，
 // 原为普通 uint64_t 且无共同锁（记录器内部那把锁不保护它们）。改为原子，
 // 读写一律 relaxed（只作诊断计数，不参与任何决策/发布判定）。
@@ -86,6 +92,10 @@ bool EncodePaletteObjectEvent(const PaletteObjectEventRecord& record,
   event.data[2] = record.key.lifecycleIdentity;
   // 2026-09-17 上级裁定 ⑧：窗口/Reset 分段量（记录级，发出时写一次；版本 2 的正式语义）。
   event.data[3] = record.windowSegment;
+  // 2026-09-18 阶段 C：**链型**。v4 起 data[4] 从保留零改为链型载体
+  // （0 = RejectionRecovery，1 = Observation）；v1/v2/v3 的读方仍要求它为 0。
+  // 本槽由 MakeRecord 从**条目建立时落定**的 chainType 盖章，调用点不得给值。
+  event.data[4] = static_cast<uint32_t>(record.chainType);
   event.data[9] = record.frames.manifestFrameSerial;
   event.data[10] = static_cast<uint64_t>(record.source);
   event.data[11] = record.hitKey;
@@ -131,7 +141,7 @@ bool EncodePaletteObjectEvent(const PaletteObjectEventRecord& record,
 }
 
 void ClosePaletteObjectWindow() noexcept {
-  if (!g_paletteObjectArmed)
+  if (!g_paletteObjectArmed.load(std::memory_order_acquire))
     return;
   g_paletteObjectEvidence.CloseWindow(g_paletteObjectEvidence.currentFrame());
 }
@@ -142,7 +152,7 @@ void PaletteObjectPreFreezeHook() noexcept {
 }
 
 void ClearPaletteObjectWatchlist() noexcept {
-  if (!g_paletteObjectArmed)
+  if (!g_paletteObjectArmed.load(std::memory_order_acquire))
     return;
   g_paletteObjectResetOrClear.fetch_add(1u, std::memory_order_relaxed);
   g_paletteObjectEvidence.ResetForSessionTransition(ActiveSession(),
@@ -161,29 +171,29 @@ void ArmPaletteObjectEvidence(uint64_t sessionGeneration,
   g_paletteObjectEvidence.Configure(&EmitPaletteObjectEvent, nullptr);
   // 新会话：**连计数一起清**（换图中的 transition 变体只清表、保留会话计数）。
   g_paletteObjectEvidence.Reset(sessionGeneration, mapEpoch);
-  g_paletteObjectArmed = true;
+  g_paletteObjectArmed.store(true, std::memory_order_release);
 }
 
 void DisarmPaletteObjectEvidence() noexcept {
-  g_paletteObjectArmed = false;
+  g_paletteObjectArmed.store(false, std::memory_order_release);
 }
 
 // 2026-09-18 最小诊断：入队块到达计数与 arm 状态（只读）。
 // 2026-09-18 独立复审 R3：全部改为原子 fetch_add/load（relaxed）；
 // 调用点一律位于子门/armed 判断之内，关闭诊断时不产生任何写入。
 void NotePaletteObjectEnqueueBlockReached() noexcept {
-  if (!g_paletteObjectArmed)
+  if (!g_paletteObjectArmed.load(std::memory_order_acquire))
     return;
   g_paletteObjectEnqueueBlockReached.fetch_add(1u, std::memory_order_relaxed);
 }
 bool PaletteObjectEvidenceArmed() noexcept {
-  return g_paletteObjectArmed;
+  return g_paletteObjectArmed.load(std::memory_order_acquire);
 }
 uint64_t PaletteObjectEnqueueBlockReachedCount() noexcept {
   return g_paletteObjectEnqueueBlockReached.load(std::memory_order_relaxed);
 }
 void NotePaletteObjectAppendEntered() noexcept {
-  if (!g_paletteObjectArmed)
+  if (!g_paletteObjectArmed.load(std::memory_order_acquire))
     return;
   g_paletteObjectAppendEntered.fetch_add(1u, std::memory_order_relaxed);
 }
@@ -213,14 +223,14 @@ uint64_t PaletteObjectResetOrClearCount() noexcept {
 
 void ResetPaletteObjectEvidence(uint64_t sessionGeneration,
                                 uint64_t mapEpoch) noexcept {
-  if (!g_paletteObjectArmed)
+  if (!g_paletteObjectArmed.load(std::memory_order_acquire))
     return;
   g_paletteObjectResetOrClear.fetch_add(1u, std::memory_order_relaxed);
   g_paletteObjectEvidence.ResetForSessionTransition(sessionGeneration, mapEpoch);
 }
 
 void QueryPaletteObjectEvidenceHeader(PaletteObjectEvidenceHeader& out) noexcept {
-  if (!g_paletteObjectArmed) {
+  if (!g_paletteObjectArmed.load(std::memory_order_acquire)) {
     out.watchCount = 0u;
     out.counters = PaletteObjectEvidence::Counters{};
     return;
===== FILE: src/d3d9/war3/tools/war3_palette_object_capture.h =====
diff --git "a/BASELINE" "b/TREE"
index 4e84f32..1113986 100644
--- a/BASELINE
+++ b/TREE
@@ -173,14 +173,25 @@ inline PaletteObjectKey MakePaletteObjectKey(
 // nativeFrameTag 由调用方给出**当场已读到**的值（R 点 = QueryCurrentPaletteFrameTag 的
 // 既有局部结果；S 点 = 最终 Selection 的 frameTag；D 点 = caster 上的 Selection frameTag），
 // 读不到 / 已被 native override 清空时 nativeKnown = false ⇒ 记 0 + nativeUnknown = true。
+// 2026-09-18 阶段 C（K3 **生产侧接线**）：`attemptSerial` **按值携带**。
+//
+// 语义 = **一条清单记录所关联的那一次尝试**（对象的清单记录更新 ⇒ 新尝试），
+// **不是**帧级计数（每帧清零已被裁定禁止，且帧内同对象可能被多次尝试）。
+// 生产三站点各自已经有这个值（= 它们本来就在传的 recordFrameSerial），因此接线
+// **不引入任何新的数据流**：
+//   Served    : draw.shadowRecordFrameSerial（:21516 证明 == packet.renderable.frameSerial）
+//   FirstSight: manifestFrame
+//   Reject    : renderable.frameSerial
+// 默认哨兵 ⇒ 既有调用方（测试）行为**逐位不变**（见 Case 30 的哨兵契约锁）。
 inline PaletteObjectFrames MakePaletteObjectFrames(
     uint64_t renderFrame, uint64_t recordFrameSerial, uint64_t nativeFrameTag,
-    bool nativeKnown) noexcept {
+    bool nativeKnown, uint64_t attemptSerial = ~0ull) noexcept {
   PaletteObjectFrames frames{};
   frames.renderFrame = renderFrame;
   frames.manifestFrameSerial = 0u;
   frames.manifestPublishRevision = 0u;
   frames.recordFrameSerial = recordFrameSerial;
+  frames.attemptSerial = attemptSerial;
   frames.nativeFrameTag = nativeKnown ? nativeFrameTag : 0u;
   frames.manifestUnknown = true;
   frames.nativeUnknown = !nativeKnown;
===== FILE: src/d3d9/d3d9_device.cpp =====
diff --git "a/BASELINE" "b/TREE"
index 7a8f4ab..253495e 100644
--- a/BASELINE
+++ b/TREE
@@ -20862,12 +20862,23 @@ bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(
   // The selected header must describe the very vector supplied to canonical.
   // Strict mode additionally checks native frame freshness below, so a failed
   // live refresh cannot silently authorize an older packet's palette.
+  // 2026-09-18 阶段 E：**原因字段按值携带**，在真实分支赋值，**不从 frameTag 反推**。
+  // nativeKnown ⟺ (1) 来源确实是本帧现场的 live 刷新（不是 packet 回退）
+  //                 ∧ (2) 新鲜度检查没有判它陈旧
+  //                 ∧ (3) 未被 native draw-time override 清空（后者在 D 点赋值）。
+  // 旧实现只用 selectedPalette.frameTag != 0u 反推：packet 回退时它带的是 packet 的 tag，
+  // 非零 ⇒ 会把"packet 的帧"谎报成"native 帧已知"。
+  bool paletteObjectSelectionFromLiveNative = liveRuntimeGroupPaletteReady;
   if (!liveRuntimeGroupPaletteReady)
     selectedPalette = packet.paletteSelection;
   const auto attemptedPaletteSelection = selectedPalette;
   if (skinned && dxvk::war3::render::skin::ContractEnabled() &&
-      !dxvk::war3::model::IsSkinPaletteSelectionCurrent(selectedPalette))
+      !dxvk::war3::model::IsSkinPaletteSelectionCurrent(selectedPalette)) {
+    // 2026-09-18 阶段 E：这里**观测到**陈旧 ⇒ 现场帧不可信。
+    // 旧实现的 frameTag 反推在此恰好也为 0，但那是"碰巧一致"；现在它是被赋值的事实。
+    paletteObjectSelectionFromLiveNative = false;
     selectedPalette = {};
+  }
   fallbackAppendTiming.enter(War3FallbackAppendPhase::Canonical);
   dxvk::war3::render::CanonicalShadowDrawItem canonicalItem = {};
   {
@@ -22135,12 +22146,14 @@ bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(
       // native 帧为最终 Selection 的 frameTag；native draw-time override 已把
       // draw.inputSkinSelection 清空（:24224）时不得再用它 ⇒ 记 unknown。
       const bool paletteObjectNativeKnown =
-          !drawTimeVBOverrideApplied && selectedPalette.frameTag != 0u;
+          !drawTimeVBOverrideApplied && paletteObjectSelectionFromLiveNative;
       paletteObjectFrames = dxvk::war3::tools::evidence::MakePaletteObjectFrames(
           uint64_t(
               dxvk::war3::state::RenderState::instance().getFrameIndex()),
           draw.shadowRecordFrameSerial, uint64_t(selectedPalette.frameTag),
-          paletteObjectNativeKnown);
+          paletteObjectNativeKnown,
+          // K3：本次尝试 = 服务点所依据的那条清单记录序号。
+          draw.shadowRecordFrameSerial);
       if (drawTimeVBOverrideApplied) {
         // native draw-time override 已清空 draw.inputSkinSelection：
         // 记**空值** + selectionClearedByNativeOverride=true，**不得补回**早先尝试的
@@ -23517,7 +23530,9 @@ uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer(
                 // 2026-09-18 独立复审 R2：本处**拿不到** native 帧，必须记
                 // nativeKnown=false（⇒ nativeUnknown=true）；此前的 true 把
                 // "未知"写成了"已知且等于 0"。
-                0u, false);
+                0u, false,
+                // K3：首见点没有实例身份的 draw；以它当场使用的 manifest 帧记录序号为尝试号。
+                uint64_t(manifestFrame));
         auto& paletteObjectRecorder =
             dxvk::war3::tools::evidence::PaletteObjectRecorder();
         dxvk::war3::tools::evidence::NotePaletteObjectProductionNoteCalled();
===== FILE: src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp =====
diff --git "a/BASELINE" "b/TREE"
index 2de5019..4e093cb 100644
--- a/BASELINE
+++ b/TREE
@@ -6824,7 +6824,10 @@ bool TryBuildRuntimeGroupPalette(const ShadowModelResourceRecord& resource,
                       dxvk::war3::state::RenderState::instance().getFrameIndex()),
                   renderable.frameSerial,
                   uint64_t(recheckEvidence.currentPaletteFrameTag),
-                  recheckEvidence.currentPaletteFrameTag != 0u);
+                  recheckEvidence.currentPaletteFrameTag != 0u,
+                  // K3：拒绝点用**当场清单记录**自己的序号 —— 与 Served 点同族
+                  // （packet.renderable.frameSerial），故同一次尝试在两点得到同一个号。
+                  renderable.frameSerial);
           dxvk::war3::tools::evidence::PaletteObjectRecorder().NoteReject(
               key, namedRejectReason, frames);
         }
===== FILE: src/d3d9/war3/tools/war3_frame_evidence.cpp =====
diff --git "a/BASELINE" "b/TREE"
index fd8c322..a2a5e2b 100644
--- a/BASELINE
+++ b/TREE
@@ -56,18 +56,25 @@ json PaletteObjectHeaderJson() {
   counters["closedTableFull"]=std::to_string(c.closedTableFull);
   counters["closedEventLost"]=std::to_string(c.closedEventLost);
   counters["closedUnclosed"]=std::to_string(c.closedUnclosed);
+  counters["closedObservationClosed"]=std::to_string(c.closedObservationClosed);
   counters["weakIdentityRecords"]=std::to_string(c.weakIdentityRecords);
   counters["epochUnknownRecords"]=std::to_string(c.epochUnknownRecords);
   // 2026-09-18 批次 3（正常观察链）：首见建条目 / 首见发射 分离计数。
-  // 2026-09-18（批次 4 回归修复）：首见计数器**只在版本 3** 出现。
-  // 版本 2 的 counters 是注册集合，追加字段会让生产解析器以
-  // "paletteObject counter fields mismatch" 拒绝整份导出（实测 A/B/C/D 四场景全败）。
-  // 未使用首见链时该块仍是版本 2，因此这两个键也必须缺席。
-  if (PaletteObjectRecorder().firstSightUsed()) {
-    counters["firstSightInserted"]=std::to_string(c.firstSightInserted);
-    counters["firstSightEmitted"]=std::to_string(c.firstSightEmitted);
-  }
+  // 2026-09-18（批次 4 回归修复）—— **该规则已被阶段 C 取代，见下方 :67-70**：
+  // 当时首见计数器**只在版本 3** 出现，因为版本 2 的 counters 是注册集合，
+  // 追加字段会让生产解析器以 "paletteObject counter fields mismatch" 拒绝整份导出
+  // （实测 A/B/C/D 四场景全败），且未使用首见链的块仍是版本 2。
+  // ⇒ 现在写方**恒定 v4**：v4 的 counters **始终**含这两个键（不再有"缺席"的情形）。
+  // 2026-09-18 阶段 C：**恒定 v4**。原实现按 `firstSightUsed()` 决定是否写入这两个计数器、
+  // 并把块版本在 2/3 之间**动态**选择 —— 同一个二进制会产生两种块形状，读方必须同时接受，
+  // 而"版本"本应是**契约**而不是**内容摘要**。裁定要求：新写方一律输出 v4。
+  // v4 的 counters 集**始终**含这两个键（读方按版本取表，见 analyze_palette_object_evidence.py）。
+  counters["firstSightInserted"]=std::to_string(c.firstSightInserted);
+  counters["firstSightEmitted"]=std::to_string(c.firstSightEmitted);
   json result=json::object();
+  // 【历史 × 3 代】下面这段（D2）→ 批次 3 → 阶段 C 是**同一处的三代叙述**，它们互相矛盾是**正常的历史**：
+  //   第 1 代说"因此头块版本必须是 2"；第 2 代说"抬到 3"；第 3 代说"恒定 4"。
+  //   **现行事实只有一条：本函数恒定写 `version=4`（见下方 result["version"]=4）。**
   // 2026-09-17 上级裁定 D2（⑧分段 + ⑦身份证明种类）：生产写入侧现在**确实**写分段（记录级
   // data[3]=windowSegment，Reset 开第 1 个窗口、ResetForSessionTransition 递增）与载明的身份证明
   // 种类（words32[15]=identityProofKind），因此头块版本必须是 **2**（版本 2 = 现行生产形状）。
@@ -76,7 +83,10 @@ json PaletteObjectHeaderJson() {
   // 2026-09-18 独立复审批次 3：正常观察链引入阶段 FirstSight(5)，版本 1/2 读方的阶段表
   // 不含该值（会拒绝整份导出，而不是静默忽略）。写方**只在真的发过首见事件时**把块版本
   // 抬到 3；未使用首见链的导出仍是 2，旧读方行为一位不变（版本 1 = 无 version 的冻结形状）。
-  result["version"]=PaletteObjectRecorder().firstSightUsed() ? 3 : 2;
+  // 2026-09-18 阶段 C：**恒定 v4**（链型 + 统一终态 ObservationClosed）。
+  // 不再按 `firstSightUsed()` 在 2/3 之间动态选择；v1/v2/v3 仍按各自契约被读方解析，
+  // 但**新写方不再产生它们**。旧产物（无 version 的冻结形状）仍按 v1 读取，一位不放宽。
+  result["version"]=4;
   // 2026-09-17 往返测试暴露的 G1：读方要求这里是 JSON **整数**（counters 仍是规范十进制字符串）。
   result["watchCount"]=header.watchCount;
   result["counters"]=counters;
@@ -93,7 +103,7 @@ json PaletteObjectHeaderJson() {
   //
   // R3 的验收要求是「计数器原子化 + 位于子门判断之后 + 关闭后不再更新」，这三条
   // 由 sink 内部的 std::atomic 与调用点 gating 保证，**不依赖**把它们暴露到线上 wire。
-  // 因此这里保持版本 2 的注册形状不变。
+  // 因此这里保持**同一套注册字段集合**不变（版本号现为 4；不因这些诊断量而增删字段）。
   //
   // 若将来确实需要这些诊断量：必须**抬版本**（新版本号 + 写方/读方/测试三方同步），
   // 绝不可在既有版本块里追加字段，也不可放宽读方注册表来接纳它们。
@@ -347,14 +357,14 @@ json Control(const json& payload,const RecorderControlLease* localLease,const st
     }catch(const std::bad_alloc&){
       return {{"ok",false},{"error","recorder-cpu-ring-allocation-failed"}};
     }
+    // 上级裁定 3a：证据环**自动**冻结（post-window / 容量 / 序列回绕）时也要先结算终态，
+    // 因此把钩子挂到环上（手动 freeze 路径同样会先调用 ClosePaletteObjectWindow）。
+    s->ring.setPreFreezeHook(&PaletteObjectPreFreezeHook);
     ++s->generation; s->lossAtArm=s->lost.load();
     s->active.store(s->generation,std::memory_order_release);
     // 2026-09-17 上级裁定（Step 1③）：证据会话开始 ⇒ 绑定对象级 palette 记录器的发射器并清空观察表。
     // 子门关闭时该调用**不做任何事**（记录器侧零操作）。
     ArmPaletteObjectEvidence(s->generation,0u);
-    // 上级裁定 3a：证据环**自动**冻结（post-window / 容量 / 序列回绕）时也要先结算终态，
-    // 因此把钩子挂到环上（手动 freeze 路径同样会先调用 ClosePaletteObjectWindow）。
-    s->ring.setPreFreezeHook(&PaletteObjectPreFreezeHook);
   } else if(action=="trigger") {
     uint32_t post=0;
     if(!integer(payload,"postPresents",8,0,120,post)||!s->ring.trigger(generation,post))
```

### 3.3 我**主动标出**的两类越界风险

| 风险 | 位置 | 我的处置 |
| --- | --- | --- |
| 动了**渲染文件**里的局部代码 | `d3d9_device.cpp` E 阶段：引入一个局部布尔并把既有的 | 条件表达式与赋值**未变**（见 §3.4），
|  | `if (skinned && … && !IsSkinPaletteSelectionCurrent(…)) selectedPalette = {};` 改为带花括号的同一条件块 | 但「改动了该行周边」是事实，**不淡化** |

### 3.4 两个渲染路径文件的**逐 hunk** 核对（我自己做的，请你独立复核）

```
war3_shadow_renderer_core.cpp  唯一 hunk（TryBuildRuntimeGroupPalette 内）
  -  recheckEvidence.currentPaletteFrameTag != 0u);
  +  recheckEvidence.currentPaletteFrameTag != 0u,
  +  // K3：拒绝点用当场清单记录自己的序号 …
  +  renderable.frameSerial);            ← 只给证据采集调用**追加一个实参**

d3d9_device.cpp  6 个 hunk，全部在证据区域：
  :20865(+7) E：声明 paletteObjectSelectionFromLiveNative（携带值）
  :20876(+4) E：新鲜度分支改为带花括号 + 置假（**条件本身未改**）
  :20881(+1) E：补右花括号
  :22149(1↔1) E：nativeKnown 改用携带值（**不再用 frameTag 反推**）
  :22154(+3) K3：Served 点传 attemptSerial
  :23533(+3) K3：FirstSight 点传 attemptSerial
```

---

## 4. 证据：门禁与实测（**同一棵树、同日、可复跑**）

```
ninja -C build32 -n                      → no work to do
AutoTest 静态门禁（test_*_static.py）      → 263 / 0
meson test（宿主机）                      → Ok: 85 / Fail: 0
palette 宿主机证据测试                    → 34 passed, 0 failed
wire 原生往返                            → ENCODER checks=73 failures=0 PASS
                                          ROUNDTRIP checks=126 failures=0 PASS
wire 门禁驱动                            → CHECKS=1160 FAILURES=0
读方 analysis static（手工补跑）           → OK
读方 analyze_frame_evidence（手工补跑）    → OK
```

### 4.1 ⚠️ 「263/0」**不等于**「AutoTest 全绿」

```
AutoTest 下 test_*.py 共 303；门禁内 test_*_static.py 共 263；
**门禁外 40 个（13%）从不参与任何门禁运行**。我实跑了其中 36 个：33 绿、3 红、4 个未跑（需实机/会启动外部进程/需 EXE 参数）。

3 个红（**经与不可变基线逐字比对 ⇒ 非本计划造成**，且已逐个具名诊断）：
  test_gpu_skin_static_snapshot_share_offline.py   FAIL: test_gpu_queue_and_resource_share_snapshot
      → 诊断：机制已从 gpu_skin_resources.cpp 搬迁到 gpu_skin_manager.cpp，测试锚点未更新
  test_d3d9_memory_chunk_tail_offline.py           ERROR + FAIL（零请求守卫 / 4KiB 尾段）
      → 诊断：**不是回归**。源码 Alloc 里 `alignedSize = align(Size, CACHE_LINE_SIZE)` 使零请求
        守卫不再需要；尾段吞并带解释性注释 ⇒ 是**有意演进**（这一步我**读了函数体**才下结论）
  test_ydhost_adapter.py                           ERROR×3
      → 诊断：环境性（8.3 短路径 ADMINI~1 vs 长路径 Administrator 的临时目录不匹配）
```

### 4.2 两个我**没有**自行处置、留待裁定的覆盖/一致性问题

- **`d3d9_mem.cpp` 的分配契约零门禁覆盖**：Python 门禁 263 与宿主机 meson 76 里**都 0 命中**，
  只有两个**门禁外**测试覆盖它（census 绿 / tail 红）⇒ 未来该路径的真实回归不会被抓住；
- 我没有修那 3 个红：改断言＝放宽判据（被硬约束禁止）；改源码＝可能掩盖回归。

---

## 5. 六个未决问题（**我的分析 + 选项 + 后果；我没有自行决定**）

### 5.1 窗口维度：它的守卫**不可见**于导出

- 已落地：`Entry::windowSegment` 进入查找谓词（查找维度＝对象键×链型×**窗口**），
  并有**载荷用例**（宿主机用例 34，含"跨窗口保留条目"的完整变异 ⇒ **具名失败**）；
- **未闭合**：违规计数 `windowMismatchLookups` **故意不写入 wire** ⇒
  **实机上的跨窗口违规不会出现在导出里**；
- 选项：**(a)** 新增 wire v5（窗口段进条目并导出）**(b)** v4 加字段（与"恒定 v4 + 版本表"有张力）
  **(c)** 维持仅内部可见（则实机不可证）。

### 5.2 ★ 双链路由：我认为**只有选项 C** 成立（有对照/处理实测）

**实测（宿主机用例 35，同一完整序列 `Reject → Served → Enqueued → Drawn → CloseWindow`）**

```
对照（只有拒绝链）: recovered=1 unclosed=0 windowExpired=0 obsClosed=0   ✅
处理（两链并存）  : recovered=0 unclosed=0 windowExpired=1 obsClosed=1   ❌
  rec[2] stage=2 chain=1    ← Served   → 观察链
  rec[3] stage=3 chain=1    ← Enqueued → 观察链
  rec[4] stage=4 chain=1    ← Drawn    → 观察链
  rec[5] stage=4 terminal=2 chain=0    ← 拒绝链终态 = WindowExpired
```

⇒ **同样的完整恢复序列，只因多了一条观察链，拒绝链就丢失全部恢复事实、永远无法 `Recovered`。**

**为什么"两条链都需要这些事实"（不是偏好）** —— 见结算逻辑：

```cpp
if (closedChain)                            terminal = Recovered;       // 需 served+submit+draw
else if (e.hitCount == 0u && !e.sawServed)  terminal = WindowExpired;   // ← 没有任何事实 ⇒ 过期
else if (chainType == Observation)          terminal = ObservationClosed;
```

⇒ 观察链**也需要 served** 才能避开 `WindowExpired` 落到 `ObservationClosed` ⇒
  **选项 B（改为优先拒绝链）已实测无效**：它只是把假阴性搬到观察链（探针实测 `recovered=1 windowExpired=1`）。

**现状的处置**：我用**特征化断言**（characterization）锁住当前行为，断言消息里明确写
*"This is NOT a claim that it is correct; if you change the routing to record on both chains,
update this assertion deliberately and cite the ruling."* —— 任何人改路由都会被这条断言抓住。

**我已备好可直接应用的补丁提案**（未应用）：C1（逐条链判预算）/ C2（单次预算、双写原子）两方案，
含**必须同步的 6 项验证**（含 C1 独有的新风险：预算紧张时两链可能不一致）。

### 5.3 六个待裁定项一览

| # | 事项 | 选项 |
| --- | --- | --- |
| ① | 窗口维度是否导出层可见 | v5 / v4 加字段 / 维持内部 |
| ② | 双链路由语义 | 维持现状 / **选项 C**（C1 或 C2） |
| ③ | E 实机采集授权 | 授权 / 不授权（AGENTS.md 要求用户明确请求） |
| ④ | 是否让构建可复现 | 是 / 否 |
| ⑤ | 门禁外 3 红 + `d3d9_mem.cpp` 覆盖缺口 | 纳入门禁（需先更新锚点）/ 维持门禁外 |
| ⑥ | 裁定「删除 `firstSightUsed()`」的解释分叉 | 删**用法**（已做）/ 删**函数本体**（未做，且门禁锁要求其存在） |

### 5.4 关于 ④ 的事实（我实测的）

```
把源码**逐字不变**地重写（只更新 mtime）后重建：
  DLL = 514266DB…  →  重建  →  DLL = E2E0270A…      （同内容，不同哈希）
两个可指名原因：
  ① PE 头 TimeDateStamp = 链接时刻（实测与 DLL mtime 相差 **0 秒**）⇒ **每次链接都改变字节**
  ② war3_perf_monitor.cpp:3839  snapshot.meta.buildTimestamp = __DATE__ " " __TIME__;
     实测在 DLL 字节里搜到 "Sep 18 2026" 与 "14:52:09"
⇒ 结论：**DLL 哈希不是源码指纹**，交付包的身份依据必须是**源文件哈希集合**；
  复核时**不得**用"DLL 哈希与包内不一致"推断"源码被改过"。
```

---

## 6. 已知局限与「不声称」（**请你检查我有没有漏掉过度声称**）

```
✗ 不声称 A/B/C/D 已「完成验收」（离线证据齐备 ≠ 实机 / 玩家前台验收）
✗ 不声称 E 有任何实机数据（装置 ≠ 采集；**零实机采集、零实机观测**）
✗ 不声称「全门禁通过」（263/0 不含门禁外 40 个文件里的 3 个红）
✗ 不声称任何症状已修复、阴影已恢复，或候选可晋升
✗ 不声称隔离桌面数据等于玩家前台性能（**没有产生任何隔离桌面性能数据**）
✗ 不声称源码演进（零请求归一化 / 尾段吞并）"正确"（只证明存在、有注释、差异解释得通）
✗ 不声称审计穷尽（多处已标注覆盖界限）
```

**已撤回的自我误诊（如实记录）**

- round 20/21：一条 ownership 规则叙事被证伪并撤回；
- round 66：我声称「`FindOwner` 导致恢复假阴性」，结论**实质正确**但当时的实验**没有对照组**、
  且断言把未验证的语义假设当成已知 ⇒ **已撤回并重做**（round 67 用对照/处理重新确证）。

---

## 7. 请你回答的问题（**具体、可回答**）

1. §3.2 的 diff 里，**v4 版本契约**是否有漏洞？特别是：
   是否存在**应该按版本分支但实际没有**的地方（本项目已犯过 4 次同类缺陷）？
2. §5.1：窗口维度只在记录器内部可见 —— 你认为这**必须**导出（v5/v4 加字段）吗？请给理由。
3. §5.2：我的「只有 C 成立」推论与结算逻辑的分析是否成立？
   若成立，你倾向 **C1**（逐链判预算、可能不一致）还是 **C2**（单次预算、双写原子、需新接口）？
4. §3.3/§3.4：E 阶段改动了渲染文件里的局部代码（条件未变）。
   以"只改证据采集、不改渲染行为"的标准看，**这算越界吗**？
5. §6：我是否漏掉了某个**过度声称**（把"观察到位"说成"问题已解决"）？
6. 还有**别的**你会在这次改动里追查的东西吗？

---

## 8. 本包**不做**什么

- 本包**不含**任何二进制交付物（打包的 DLL 另有其包；且 DLL 哈希含构建时刻，不是源码指纹）；
- 本包**不含**实机数据（**没有**）；
- 本包**不**声称上面任何一条"应该"就是结论 —— §5 是选项与后果，不是决定。