# 写方修正① 的回归锁：多帧"只发一次首见"用例 — 2026-09-18

> 上一轮（修正①）**没有任何测试失败** ⇒ 说明此前**没有测试断言"每帧重发"行为**，
> 也就是说那条修复当时**没有被任何东西锁住**。本文补上这个锁。

## 1. 用例内容

加在 `war3_palette_object_evidence_test.cpp` 的 `Case8DuplicateRejectPerFrame()` **末尾**：

```cpp
const uint32_t storedBeforeFirstSight = g_stored;
rec.NoteFirstSight(key, MakeFrames(7u));
ok &= Require(g_stored - storedBeforeFirstSight == 1u,
              "the first NoteFirstSight of an entry must be emitted");
const uint64_t dupBeforeFirstSight = rec.counters().droppedDuplicatePerFrame;
rec.NoteFirstSight(key, MakeFrames(9u));          // **跨帧**
ok &= Require(g_stored - storedBeforeFirstSight == 1u,
              "a repeated NoteFirstSight on a LATER frame must NOT emit again "
              "(the chain head is per entry, not per frame)");
ok &= Require(rec.counters().droppedDuplicatePerFrame == dupBeforeFirstSight + 1u, ...);
ok &= Require(rec.counters().firstSightEmitted == 1u,
              "exactly one first-sight emission per entry");
```

**判别性设计**：`MakeFrames(7u)` → `MakeFrames(9u)` 是**两个不同的帧号**。
修复前（按 `(键, 帧)` 去重）第二次会**再发一条** ⇒ 断言失败；修复后只发一条 ⇒ 通过。
现有夹具全部跑单帧，**无法区分"发一次"与"每帧发"**，所以这条用例是必需的。

运行时见证：

```
firstSight once-per-entry witness: stored=1 firstSightEmitted=1 droppedDuplicatePerFrame=2
```

## 2. 我在放置上的一个错误（值得记下）

**第一版我把这块插在了 Case8 的中间**，结果：

```
FAIL: a later frame emits the reject again
FAIL: a next-frame reject must not grow either same-frame bucket
```

我自己新加的断言**全部通过**（witness 正确打印），但**把紧随其后的两处既有断言打挂了**。

原因：该块对**同一个 `key`** 调用了 `NoteFirstSight`，改变了条目状态（`sawFirstSight=true`、
`chainSequence` 推进）并推进了 `g_stored`，而 Case8 后半段的断言**共享同一个条目的状态**。

**教训**：在一个共享夹具的用例中间插入**会改变状态**的检查，会污染其后的断言。
要么放在用例末尾，要么用独立的 key/recorder。我把它移到了 Case8 末尾。

（移动过程中我还失败过一次：第二次编辑的 `old_string` 用了**我重构后**的注释文本，
与文件里的实际文本不符 ⇒ `old_string was not found`。**重构文本不能当作原文使用**。）

## 3. 验证

```
war3_palette_object_evidence_test : SUMMARY: 24 passed, 0 failed
  （含新用例；witness 正确打印）
meson test                        : 85 Ok / 0 Fail
全量静态                          : 259 scripts, 0 failed
```

## 4. 现在可以说与不可以说

| 可以说 | 不可以 说 |
| --- | --- |
| 修正① 有**多帧回归锁**了 | "75 条缺链首已解决"——**未实机重测** |
| 修复后全门禁通过 | "全门禁通过 ⇒ 发布就绪"——**候选≠发布** |

## 5. 仍未做

| 项 | 说明 |
| --- | --- |
| **实机重测** | 未跑采集验证 32 → 107，也未验证配额压力下降 |
| 修正②（预算预检提到 `Insert` 之前） | 未做 |
| 修正③（终态不得给从未发首见的条目兜底写 `Drawn`） | 未做；**会碰 v1/v2 旧合同** |
| P2 三项（夹具改真生产形状 / 新计数器加校验 / 场景 G 补齐检查） | 未做 |

## 6. 现场

```
候选 DLL 已含修正①；现场站点 d3d9.dll = 基线 F275545B…5CF07FF3（未部署）
无 War3 进程；未提交、未部署、未晋升稳定。
```