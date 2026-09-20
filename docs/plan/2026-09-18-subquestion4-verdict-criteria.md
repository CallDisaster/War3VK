# 第④问第四子问题：**判据规格**（实机导出到手后如何判定"正常对象是否被误伤"）— 2026-09-18

> 你最初问「**具体都需要什么数据**」。这份文档把答案写成**可执行判据** ——
> 实机导出拿到后，按本文逐条跑，即可给出"是/否/证据不足"的结论，而不是再靠叙述。

## 1. 为什么必须运行期证据

第④问的四条：①选中什么数据 ②为何允许 ③提交时是否被换掉 ④**正常对象是否被误伤**。
前三条的答案在**读码 + 纯谓词**层面可确定（已完成）。
但④在 **fail-closed** 下**不可由读码判定**：

```
"正常被误伤"  与  "不合规被正确拒绝"   在源码层面**同形** ——
两者都表现为"某个对象没走完链路"，区别只在**该对象本应符合条件**。
⇒ 必须有一次真实运行的导出，才能看到**哪些对象**、**在哪个阶段**停住。
```

## 2. 需要的数据（已由驱动自动采集）

| # | 数据 | 来源 | 为何需要 |
| --- | --- | --- | --- |
| D1 | 一份**非零** palette 对象导出（`DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1`） | 控制面 `export` | 唯一的对象级事实来源 |
| D2 | 导出块 `version` | 导出 JSON | 必须为 **3**（生产路径必发 FirstSight ⇒ 必为 v3）；若为 2 说明首见链**没生效** |
| D3 | 每个对象的**阶段链**（FirstSight → Enqueued → …） | 导出事件 | 区分"走了正常链"与"停在某阶段" |
| D4 | `recovered` 列表 | 解析器输出 | 正常链**不应**出现在这里（复审判据：正常完成不叫恢复） |
| D5 | `FirstSight` **早于** `Enqueued` 的顺序 | 导出事件 | 顺序倒置即"自动插表"嫌疑 ⇒ 复审明令禁止 |
| D6 | 计数恒等式：`emitted` / `accepted` / `evicted` / `reserved` | 块头 + 计数器 | 导出自身是否自洽 |

## 3. 判定规则（逐条可执行）

### R-A 「链路真的在跑」
```
assert export 非空（events > 0）且 watchCount > 0
若 events == 0  ==>  "证据不足"，不是"通过"
```

### R-B 「v3 在实机成立」
```
assert paletteObject.version == 3        # 生产路径必发 FirstSight，故必为 3
assert generic_reader.analyze(export) 成功且 extensions == {"paletteObject": 3}
assert palette_parser.analyze(export) 成功且 formatVersion == 3
若 version == 2 ==>  首见链未生效，须查 NoteFirstSight 是否被调用（不是"通过"）
```

### R-C 「正常链不被冒充」
```
对每个对象键：
    assert FirstSight 的 sequence < 首个 Enqueued 的 sequence      # 顺序（D5）
    assert 该键**不在** recovered 列表中                           # 正常 ≠ 恢复（D4）
若某键只有 Enqueued 而**没有** FirstSight ==>  复审禁止的"NoteEnqueued 自动插表"
```

### R-D 「无伪造终态」
```
assert 不存在 stage == Drawn 但 source == Unknown/0 的记录   # 不得假装"已画"
assert 未完成窗口的对象终态是**具名子门拒绝**（而非 Drawn）
```

### R-E 「正常对象是否被误伤」——**核心判据**

这一条**不能只看一个数**，必须做**两类对照**：

| 观察 | "被误伤"的特征 | "正确拒绝"的特征 |
| --- | --- | --- |
| 未走完链路的对象比例 | 在**正常**对象上出现 | 集中在**已知不合规**对象上 |
| 未走完时停在的阶段 | 停在**身份/数量**类检查（FirstSight 后立刻断） | 停在**语义**检查（有具名拒绝原因） |
| 同一对象跨帧 | 长期反复失败 | 失败后恢复正常或稳定拒绝 |
| 与 R1 相关的数量条件 | `producerGroupCount >= required` 却仍被拒 | `producerGroupCount < required`（合理拒绝） |

**因此需要两次运行做对照**：

```
运行 A（候选 DLL）  : 采集导出，统计上述四列
运行 B（基线 DLL）  : 同一地图、同一时长、同一路线，采集导出
对照：若"未走完比例"在 A/B 间**无显著差异**  ==> 未误伤
      若 A 明显更高且集中在正常对象 ==> 误伤（须给出具体对象键与阶段）
```

⚠️ **单次运行只能说明"有多少对象未走完"，不能说明原因** —— 这正是我一直强调"需要两次对照"的原因。

## 4. 明确不构成证据的东西

| 不算证据 | 原因 |
| --- | --- |
| 只有一次运行的导出 | 无基线可比，"未走完"无法归因 |
| 隔离桌面的帧率/耗时 | 复审与 AGENTS.md 均禁止当前台性能 |
| 解析器"跑通没报错" | 只证明可解析，不证明行为正确 |
| 我读码得出的结论 | 读码不能区分"误伤"与"正确拒绝"（见 §1） |
| `version == 2` 的导出 | 首见链未生效，无法回答本问 |

## 5. 采集运行的实际配方（驱动已就绪）

```
py AutoTest/live_contrast_palette_objects.py       # RUN_SECONDS=120
  前置：无 War3 在跑（干净环境 —— 并发实例已证明拿不到控制面管道）
  25s 后 : control(pid,"status") + control(pid,"arm")
  120s   : control(pid,"trigger") + control(pid,"export")
  之后   : rename-park 恢复现场（不依赖进程退出，已实机验证）
env: DXVK_WAR3_FRAME_EVIDENCE=1
     DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1
     DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED=0     # 允许外部 arm
```

## 6. 唯一阻塞

```
War3.exe pid 18948 仍存活（两个判据一致）=> 前置检查拒绝启动
解除：taskkill /F /PID 18948   （或任务管理器结束 War3.exe）
```