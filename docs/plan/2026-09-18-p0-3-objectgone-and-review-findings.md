# P0-3 ObjectGone + 独立复审发现 — 2026-09-18（未提交 / 未部署 / 未晋升稳定）

## 1. 修复（两点，均有实测依据）

### 1.1 不得伪造阶段
原 `NoteObjectGone` 硬写 `record.stage = PaletteObjectStage::Rejected;` ⇒ **观察链**（从未被拒绝过）
被盖上 Rejected，与本方读方规则「观察链不得携带 Rejected 阶段」**直接冲突**（写方产出自相矛盾内容）。
现在与 `CloseWindow` 同原则：`e->sawFirstSight ? StageOfRank(e->maxStage) : Rejected`
（拒绝恢复链保留原冻结语义，一位不改）。

### 1.2 两条链都要结算
原实现只处理 `FindOwner()` 返回的一条（全局优先观察链）⇒ 另一条滞留表内、最终以 WindowExpired 结算。
现在抽取 `SettleObjectGone(Entry*)` 单链操作，对 `RejectionRecovery` 与 `Observation` **各结算一次**。
关联依据是**显式**的：「该对象已消失」是两条链共同拥有的事实，不是把一条链的事件广播给另一条。

### 1.3 重构踩到的语言规则（记录）
`SettleObjectGone(Entry* e)` 的参数类型要求 `Entry` **在该声明之前**已声明；
成员函数**体**可以用后声明的嵌套类型（完整类上下文），**参数类型不行**。
⇒ helper 定义必须放在 `struct Entry` **之后**（放在方法区会编译失败）。

## 2. 复审 R4（我引入路径上的真实缺陷，已修）

独立复审复现：在 ObjectGone 终态的 emit 内**重入** `CloseWindow`（生产可达，见 Case 20 的环预冻结钩子）时，
`EmitUnchecked` **之后**的 `m_watchCount--` 会把计数从 0 减到 **0xFFFFFFFF**；而读方
`analyze_palette_object_evidence.py` 硬要求 `watchCount <= 1024` ⇒ **整份导出被拒**（fail-visible）。
同时第二条链的 `SettleObjectGone(FindChain(Observation))` 拿到 `nullptr` ⇒ **第二次 ObjectGone 静默丢失**。

修复：
```
① 两处递减加护栏 if (m_watchCount > 0u)（幂等、不下溢）
② NoteObjectGone 在第一条结算**之前**记录 hadObservation；若第一条的 emit 导致窗口关闭，
   而第二条链确实存在 ⇒ m_counters.objectGoneLostToReentrancy++（**具名 fail-visible**，不静默）
```
计数器与 `droppedStaleSession` 同理：**内部计数、不上 wire**（补 wire 字段须抬版本）。

## 3. 复审 R3：我写的注释现在不成立（已更正）

`MarkStage` 未知尝试号分支原注释写「保持既有终身单调判序（**行为不变**）」。
P0-2 的无条件提升让 `maxStage` 会被**已知尝试号**的事件抬高 ⇒ 未知号事件现在是在与
「混合了已知尝试秩的终身最高秩」比较：已知号 R/S/E/D 走完后再来一条未知号 Rejected，
终态从 `Recovered` 变 `Unclosed`（复审独立复现 S4）。
当前生产唯一未知号站点是 **D 点**（秩 4 = 最高秩，永不回退）⇒ 现不可达；
但**新增任何未知号低秩站点都会让条目永久失去 `Recovered`**。彻底修法属 P0-5。

## 4. 复审推翻/修正我的三处说法（如实登记）

```
① 「生产上各点传的就是各点的帧号」**错**：4 个活的生产采集点只有 3 个传 attemptSerial
   （S/E d3d9_device.cpp:22150-22156、FirstSight :23528-23535、Reject war3_shadow_renderer_core.cpp:6822-6830），
   **Drawn 点 d3d9_war3_shadow.cpp:5241-5246 省略第 5 参 ⇒ ~0ull（未知分支）**。
② 因此我引用的 Astra 反例（stage=FirstSight）**无法由当前生产接线产生**（复审 S3：pre/post 都 stage=4）
   ⇒ 该引用必须标注为「**历史 wiring 实测**」，不得当作当前树的可复现证据。
   P0-2 真正补上的是「S/E-only 链被少报成 FirstSight」（S2/S7：pre=FirstSight(5) → post=Enqueued(3)）。
③ wire 往返门禁与本改动**正交**：其夹具帧域恒用未知哨兵（wire_roundtrip_test.cpp:74 只传 4 参）
   ⇒ 它**不可能**发现已知尝试号路径的变化，不得作为 attemptSerial 相关改动的证据。
```

另有两条复审指出的盲区（未修，列为下一步）：
```
· test_palette_object_attempt_serial_caller_static.py 只统计 2 个文件并断言 live_calls==3，
  全树活的生产调用点实为 4（漏 d3d9_war3_shadow.cpp:5241）⇒ **该门禁结构上无法发现 D 点未接线**。
· 构建卫生：palette 测试目标磁盘上无 .obj.d；改 .h 后 ninja 可能报 no work 而交付二进制陈旧
  （复审实测到一次 Fail 1）。⇒ 常设配方：**门禁前先删对应 .obj 强制重编并打印 hash/mtime**。
```

## 5. 验证（全部检查退出码；已强制重编）

```
ninja -C build32 -j4（先删 .obj）    exit 0
ninja -C build32 -n                  no work to do
AutoTest 静态全量                     263 / 0
ninja -C build32 test                Ok 85 / Fail 0
宿主机测试                           38 passed, 0 failed（Case 1..39）
wire 原生 / 驱动                     ROUNDTRIP 126/0 PASS · CHECKS=1160 FAILURES=0
```

### 门禁修正（F3 跟随重构，**加强**而非放宽）
`test_semantic_build_thread_gate_static.py` 原从 `NoteObjectGone` 体里抓 delta 计算；
抽取目标随重构改为 `SettleObjectGone`，并**新增两条更强断言**：
NoteObjectGone 必须对**每条链**各委派一次；观察链的 ObjectGone 阶段必须按实际最高秩回填（不得硬写 Rejected）。

## 6. 不声称

```
· 不声称实机已验证（本轮零实机采集）
· 不声称重入路径已被行为用例覆盖 —— **Case 40（重入下的护栏与具名计数）尚未编写**（明确遗留）
· 不声称 P0-5 的关联语义已修（D 点仍未接线；未知号判序基准仍是混合的）
· 不声称全门禁通过（263/0 是 test_*_static.py 通配）
```
