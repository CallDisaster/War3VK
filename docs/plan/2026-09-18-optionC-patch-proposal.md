# 待裁定项②·（选项 C）**可直接应用的补丁提案** —— 两链各自记录 — 2026-09-18（round 84）

> 状态：**未应用**。本文是**提案**，供裁定后照抄。
>
> 背景（实测，见 `2026-09-18-stageC-dualchain-loses-recovery-measured.md`）：
>
> ```
> 对照（只有拒绝链）+ served→enqueued→drawn  ⇒ recovered=1                    ✅
> 处理（两链并存）  + 同样的完整序列         ⇒ recovered=0 windowExpired=1    ❌
> ```
> ⇒ 双链并存时，`FindOwner`（优先 Observation）把阶段事实**全部**给了观察链，
>   拒绝链因此**永远无法** `Recovered`。选项 B（改优先拒绝链）**已实测无效**（只是把假阴性搬到观察链）。

## 1. 为什么"两条链都需要这些事实"（不是偏好问题）

读 `CloseWindow` 的结算逻辑（`evidence.h:287-308`）：

```cpp
if (closedChain)                                     terminal = Recovered;          // 需 served+submit+draw
else if (e.hitCount == 0u && !e.sawServed)           terminal = WindowExpired;      // ← 没有任何事实 ⇒ 过期
else if (chainType == Observation)                   terminal = ObservationClosed;
```

⇒ **观察链也需要 served** 才能避开 `WindowExpired` 那一档、落到 `ObservationClosed`；
   **拒绝链也需要 served/submit/draw** 才能 `Recovered`。
⇒ 一条物理事实要同时满足两条链的结算条件 ⇒ **只有"各自记一份"能满足**。

## 2. 现状结构（三个入口同构）

```cpp
// NoteServed(:517) / NoteEnqueued(:563) / NoteDrawn(:608) 三者结构相同：
Entry* e = FindOwner(key);                       // ← 单一归属（这就是缺陷点）
if (e == nullptr) return;
… 去重(D6) …
if (!CanEmit(false)) { AccountDrop(false); return; }
… 更新 e 的 last*/来源/hitKey …
PaletteObjectEventRecord record = PrepareStage(e, Stage, frames);
… 发射 record …
```

## 3. 提案 C1（**推荐先测这个**）：逐条链各自判预算并各自发射

```cpp
// 新增：把一个阶段事实推进到**指定链**（存在才推进）。原函数体绝大部分搬进这里。
void AdvanceChain(Entry* e, PaletteObjectStage stage, PaletteObjectSource source,
                  uint64_t hitKey, const PaletteObjectFrames& frames,
                  bool selectionCleared, uint64_t* lastFrameField,
                  PaletteObjectSource* lastSourceField, uint64_t* lastHitKeyField,
                  bool* lastClearedField) {
  if (e == nullptr) return;
  if (*lastFrameField == frames.renderFrame) { …原 D6 去重… return; }
  if (!CanEmit(false)) { AccountDrop(false); return; }   // ← 逐条链判预算
  *lastFrameField = frames.renderFrame; …更新来源/hitKey/清空标志…
  PaletteObjectEventRecord record = PrepareStage(e, stage, frames);
  …原发射…
}

// NoteServed 变成：
  Entry* owner = FindOwner(key);
  Entry* other = FindChain(key, owner 是 Observation ? RejectionRecovery : Observation);
  AdvanceChain(owner, …);
  AdvanceChain(other, …);          // ← 另一条链**若存在**也记一份
```

### C1 的行为变化（**必须事先说清，不能事后解释**）

| 项 | 变化 |
| --- | --- |
| 事件条数 | 两链并存时，**一个物理阶段会产生 2 条记录**（各带自己的 `chainType`） |
| `counters.emitted` | 相应增加 |
| 两链的终态 | 拒绝链可 `Recovered`；观察链可 `ObservationClosed` —— **这正是目的** |
| 预算 | **逐条链**判 ⇒ 预算紧张时可能**只有一条链**记上 ⇒ 两链**不一致** ⚠️ |
| 读方 | **无需改动**：链分组键含 `chainType` ⇒ 两条链不会被并；每条链仍受既有的链型规则约束 ✅ |
| 编码 | v4 契约不变（`chainType` 已在 `data[4]`）✅ |

## 4. 提案 C2（若裁定要求"要么都记、要么都不记"）：单次预算、双写

```cpp
  // 先确认**要写几条链**，再一次性判预算，两条都通过才写：
  const uint32_t n = (other != nullptr ? 2u : 1u);
  if (!CanEmitMany(false, n)) { AccountDropMany(false, n); return; }
  AdvanceChain(owner, /*已判预算*/); AdvanceChain(other, /*已判预算*/);
```

- **优点**：两链**原子地**一致（不会一个记上一个没记）；
- **代价**：一次消耗 2 个预算槽；且需要**新接口** `CanEmitMany/AccountDropMany`（现有 `CanEmit` 是单槽语义）。

**C1 vs C2 的取舍属裁定范围**：C1 改动小但引入"两链可能不一致"；C2 一致但要动预算接口。

## 5. 无论选哪个，**必须同步的验证**（否则等于放宽判据）

1. **Case 35 的"特征化断言"必须被刻意更新** —— 它现在锁的是**旧行为**
   （`closedRecovered==0 && closedWindowExpired==1`），消息里已写明
   *"if you change the routing to record on both chains, update this assertion deliberately and cite the ruling"*。
   ⇒ 改路由必然让它失败，这正是它的用途 ✅；
2. **新增载荷测试**：对照/处理**再次成对实测**（`recovered==1` 在**单链与双链两种形状下都成立**）；
3. **Case 27/28 需复核**：它们断言"哪条链收到 enq/draw" ⇒ 双写后事件条数变化，期望**可能**要改
   （**改期望前必须先判断是"行为有意变化"还是"回归"**，不得为了让它们绿而改）；
4. **探针**：把 `AdvanceChain(other, …)` 撤掉 ⇒ 新断言必须**具名失败**（证明它载荷）；
5. **预算压力用例**：构造"只剩 1 个非终态槽"的情形，**观察**两链表现（C1 下应只有一条记上）——
   这是 C1 独有的新失败模式，**必须有具名用例**说明它，而不是留在文档里；
6. **wire/读方全量复跑**：`CHECKS=1160`、读方套件、宿主机 34 用例。

## 6. 我**没有**做的事

- **没有**写任何一行实现代码（本文只有提案；仓库未被改动）；
- **没有**新增 `CanEmitMany/AccountDropMany`（C2 需要，**未实现**）；
- **没有**改 Case 35 的断言（它必须由**应用补丁的人**刻意改，见 §5.1）；
- **没有**决定 C1 vs C2（**属裁定范围**）。

## 7. 不声称

- **不**声称 C 是"正确"的：我只证明**在当前读方与结算规则下**它是唯一能满足两条链结算条件的选项；
- **不**声称 C 的实现**没有其它副作用**（我只分析了事件条数、计数、预算、读方、编码五项；
  wire 体积、性能开销、以及 `hitCount` 语义的连带影响**未逐项量化** ⚠️）；
- **不**声称 C1/C2 的取舍我能自行决定；
- **不**声称本提案经任何验证（**未应用、未编译、未运行**）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。