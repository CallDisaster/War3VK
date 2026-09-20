# 🔴 我引入的真实回归：版本 2 块被加入未注册字段 — 2026-09-18

> **本文更正我此前两处不准确陈述**：(1)"meson roundtrip 失败疑为 env 注入差异"；(2)"直接运行时 116/116 PASS"。
> 两者都是**只读部分输出**造成的误判。真实原因是**我自己的改动破坏了版本 2 的线上形状**。

## 1. 真实失败（meson testlog，block 1023-1121）

```
ENCODER checks=73 failures=0 PASS
ROUNDTRIP checks=116 failures=0 PASS          <-- 我此前只看到这一行就收手
FAIL: A: the production block must be the registered version-2 shape
  (got ['activeSession','appendEntered','armed','counters','enqueueBlockReached',
        'productionInsertReached','productionNoteCalled', ...])
FAIL: A: the generic root reader must accept the real palette export
  (paletteObject extension version 2 has unknown fields: [同上 ...])
FAIL: A: the production parser must accept the real export  (同上)
FAIL: B: ...  FAIL: C: ...                              (三个场景全失败)
```

## 2. 根因（我的改动）

批次 2 的 **R3**（诊断计数器原子化 + 移入子门之后）**额外**把一批诊断字段写进了块头：

```
armed / activeSession / enqueueBlockReached / appendEntered /
productionInsertReached / productionNoteCalled / resetOrClearCount
```

但**版本 2 的注册形状不含这些字段**，因此：

| 断言 | 后果 |
| --- | --- |
| 生产块必须是**注册的版本 2 形状** | 失败（多出未注册字段） |
| 通用根读取器必须接受该导出 | 失败（`has unknown fields`） |
| 生产解析器必须接受该导出 | 失败（同上） |

⇒ 这属于外部复审反复强调的那类错误：**在未抬版本的协议块里加入新字段**。
R3 的要求是"诊断计数器原子化、位于子门判断之后、关闭后不再更新"——**并不要求把它们放进线上版本 2 块**。
我把"诊断"与"线上 wire"混在了一起。

## 3. 我为什么会误判（必须记住的教训）

| 我的陈述 | 实际 | 错误类型 |
| --- | --- | --- |
| "meson 失败疑为 env 注入差异" | meson **确实**声明了 `env:`（三变量）；失败与 env 无关 | **未读日志就推测** |
| "直接运行 116/116 PASS" | `ROUNDTRIP` 行只是包装器断言的**前一半**；其后三重断言全失败 | **只读部分输出** |
| "已确认不是既有问题" | **是本轮引入的回归** | 结论方向完全相反 |

⇒ 教训：**包装器驱动的测试，必须读到"最终退出码 + 全部断言"，不能只看中间某一行 PASS。**
这与本轮早先"`Select-Object -First` 截断管道导致 `EXIT` 失真"是同一类错误的两面。

## 4. 修复方向（**未实施**，需先定方案）

三条互斥路线，必须择一并在实施前说明依据：

| 方案 | 内容 | 评价 |
| --- | --- | --- |
| **X** | 把诊断字段从版本 2 块头**移出**（回到"仅内部计数器 + 子门 gating"），即严格按 R3 的原始要求，不扩展线上 wire | **最符合"不抬版本就不改形状"**；但需确认这些字段是否有既有消费者 |
| Y | 把它们纳入**版本 3**（版本 3 已由批次 3 的 FirstSight 引入），即"版本 3 = FirstSight + 诊断字段" | 需同步写方/解析器/测试三方 + 版本判定条件；影响面较大 |
| Z | 在解析器/根读取器的注册表里登记这些字段 | ⚠️ **这等于放宽解析器直到它通过** —— 除非同时抬版本，否则**不做** |

初步倾向 **X**：R3 的验收要求（原子、子门之后、关闭后不更新）**不依赖**这些字段出现在线上导出中；
把它们放进线上块属于**我自行扩展的范围**，是本次回归的直接原因。
但**在确认无既有消费者之前不实施**。

## 5. 现状（更正）

| 项 | 值 |
| --- | --- |
| 全量静态 | **258 / 0 failed** |
| meson | **84 / 1 FAIL** —— 该 1 项是**我引入的回归**（非 env、非既有） |
| 现场 DLL | `F275545B…` 未动（`519AFA69…` 备份在位） |
| 未提交 / 未部署 | 是 |
| 目标 | **不可标记完成** |

## 6. 硬约束更正声明

我此前称"全量静态 258/258 通过；meson 84/85，剩 1 个环境前置相关问题待判定"——
**前半正确，后半错误**。准确表述应为：

> **全量静态 258/258 通过；meson 84/85，剩余 1 项为本轮 R3 改动引入的 wire 形状回归，待修复。**