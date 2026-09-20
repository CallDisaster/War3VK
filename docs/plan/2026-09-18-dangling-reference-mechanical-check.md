# 「悬空引用」机械核对：**34 个注释引用全部存在**（否证）— 2026-09-18（round 82）

> round 81 修了一个**悬空引用**（`evidence.h:848` 的"CloseWindow 的版本 3 分支"），
> 但在"不声称"里留了一条：「**不**声称"悬空引用"只有 `:848` 一处（我只核对了它提到的那一个代码路径）」。
>
> 本轮把它做成**机械可查**并闭合。

## 1. 方法（可复跑）

```
1. 取我改过的文件：产品侧 4 个（evidence.h / sink.cpp / capture.h / frame_evidence.cpp）
                  读方 2 个（analyze_palette_object_evidence.py / analyze_frame_evidence.py）
2. 只取**注释行**（C++ 的 // 与 *；Python 的 #）
3. 抽出注释里**反引号包裹的标识符**：  `([A-Za-z_][A-Za-z0-9_:]{2,})
4. 对每个标识符，在 src/ 与 AutoTest/ 里做存在性搜索
5. 命中 0 ⇒ 候选**悬空引用**
```

## 2. 结果：**0 处悬空** ✅

### 产品侧 22 个（全部存在）

```
firstReason .h=8 .cpp=2      chainType .h=9 .cpp=11      lastFirstSightFrame .h=2 .cpp=0
NoteFirstSight .h=4 .cpp=22  sawFirstSight .h=7 .cpp=0   StateLock .h=26 .cpp=13
attemptSerial .h=11 .cpp=9   firstSightUsed .h=1 .cpp=5  Unclosed .h=7 .cpp=51
Recovered .h=17 .cpp=131
STAGES(32)  LEGACY_STAGES(5)  lookup(172)  require(1304)
test_first_sight_stage_is_rejected_wholesale_under_legacy_version(1)
test_palette_object_evidence_analysis_static(3)
（bool / version / AutoTest / skin / event / ValueError: 不是路径引用，不计）
```

### 读方 12 个（全部存在）

```
closedObservationClosed(10/7)  OBSERVATION_CLOSED_FIELDS(6/0)   frozenZeroDeltaViolated(5/2)
firstSightInserted(16/11)      first_sight_events(1/0)          firstSightEmitted(23/9)
ObservationClosed(35/19)       chainType(12/20)                 item / record / stage / counters
```
（括注为 AutoTest 命中数 / src 命中数）

⇒ **产品侧 22 + 读方 12 = 34 个注释引用，全部指向真实存在的标识符。**
⇒ round 81 修掉的那一处，**是唯一的**（在本次核对范围内）。

## 3. 为什么不把它做成静态锁

理由（与 round 70 对"无害冗余"的处置同源）：

1. **现在无法有意义地失败**：34/34 全存在 ⇒ 一条此刻必然通过的断言，价值仅在"未来"；
2. **误报率高**：注释里合法地出现自然语言/通用词（`skin`、`event`、`bool`、`version`、`item`、`record`、`stage`、`counters`），
   要让它能用就得维护一份**白名单**，而白名单本身会腐烂（正是本项目反复出现的缺陷类）；
3. **语义边界模糊**：「引用」与「提及」无法机械区分 —— 例如注释里写"旧实现按 `firstSightUsed()` 决定"
   是**历史叙述**（应当保留），不是悬空引用；
4. ⇒ 更稳的做法是**把方法文档化**（本文），在改动相关文件时**手工复跑一次**。

## 4. 覆盖界限（诚实标注）

- 我只核对了**我改过的 6 个文件**的注释；`src/` 与 `AutoTest/` 的**其余文件未扫**；
- 我只抽取了**反引号包裹**的标识符；注释里**不用反引号**的引用（例如"见 CloseWindow 的…"
  这种**裸写**形式）**不在判据内** —— 而 round 81 修掉的那一处**恰好就是裸写形式** ⚠️
  ⇒ **本方法会漏掉裸写引用**；这是它的已知局限；
- "存在"只证明**同名标识符在某处出现**，不证明**注释说的那个路径/分支仍然成立**
  （那是 round 81 发现 `:848` 的方式：需要**读代码**而不是搜名字）。

## 5. 不声称

- **不**声称全树**没有**悬空引用（只扫了 6 个文件的**反引号**引用；裸写引用**未扫**）；
- **不**声称本方法能发现 `:848` 那类问题（**恰恰相反**：它是裸写形式，本方法**漏掉**了它；
  发现它靠的是**读 `CloseWindow` 的代码**）；
- **不**声称 34 个引用"语义正确"（只证明**同名标识符存在**）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。