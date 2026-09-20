# 跨语言一致性门禁：把 C++ 写方与 Python 读方绑起来 — 2026-09-18

> 上一轮修好了 v3 缺口。本轮补的是**"为什么这个缺口能存在一整轮"**的结构性防线。

## 1. 缺口为什么能存活

批次 3 的门禁断言的**全部是 Python 侧符号存在性**：

```python
assert "PALETTE_OBJECT_FIRST_SIGHT_VERSION=3" in PARSER
assert "5:'FirstSight'" in PARSER
assert "firstSightStageUnderLegacyVersion" in PARSER
```

它**没有一条**把 C++ 写方和 Python 读方**关联**起来。于是：

```
C++ 写方 : 发 v3
Python 读方: 只注册 {1,2}
=> 两者都不违反任何断言 => 套件全绿 => 缺口存活
```

**根因不是"少写了一条断言"，而是"断言的是单侧符号，而不是两侧的关系"。**

## 2. 新增的四组跨语言检查

写入 `AutoTest/test_first_sight_observation_chain_static.py` §7：

| 组 | 断言 | 防的是什么 |
| --- | --- | --- |
| (a) | 写方表达式必须是 `firstSightUsed() ? 3 : 2`；读方必须**同时**定义 `PALETTE_OBJECT_FIRST_SIGHT_VERSION=3` **并**登记进 `PALETTE_OBJECT_BLOCK_FIELDS` | 写方发一个新版本而读方不认识 |
| (b) | 显式禁止"只定义常量、不登记形状" | **半截状态**（本次缺口的精确形态） |
| (c) | `counters` 校验必须**按版本**分支（`... if version>=PALETTE_OBJECT_FIRST_SIGHT_VERSION else COUNTER_FIELDS`） | 一视同仁导致 v2 被错收或 v3 被错拒 |
| (d) | 必须存在 `test_explicit_version_3_is_registered_and_accepted_by_both_readers` | 只测常量的门禁抓不到缺口 ⇒ 强制保留**端到端**用例 |

## 3. 门禁自身的一次切片错误（已修，如实记录）

第一版 (b) 写成"取 `PALETTE_OBJECT_BLOCK_FIELDS={` 之后到第一个 `}`"，结果截在**第一个 `frozenset` 内部**，
把正确代码判为缺失 ⇒ 立即误报。改为取 400 字符窗口，并在注释里写明该陷阱。

⇒ 与上一轮"匹配必须恰好一次"的设计同源：**静态检查的锚点本身也可能是错的，报错时先怀疑锚点。**

## 4. 验证

```
test_first_sight_observation_chain_static.py
  -> first-sight static: PASS (+ cross-language version-registration consistency)
全量静态脚本: 259 scripts, 0 failed
meson test  : 85 Ok / 0 Fail        （上一轮，未受本轮影响：只改 AutoTest）
```

## 5. 目标状态

| 项 | 状态 |
| --- | --- |
| ① 检查点（c0 + c1 + 归档 + 回退信息 + 环境区分） | ✅ |
| ② 三项缺陷（R1/R2/R3） | ✅ |
| ③ 有版本正常观察链 | ✅ 上一轮补齐；**本轮加跨语言防线** |
| ④ 严格契约确认 + 真实入口对照 | 前三子问题 ✅；**第四子问题缺运行期证据** |
| 套件 | 静态 259/0、meson 85/0、runnable 74/76；未跑 TDR/ABBA、前台门、**实机** |

## 6. 仍未闭合

- **无实机 v3 导出**：v3 现在两端一致且被读方接受，但**从未在实机产生过**；
- 第④问第四子问题（正常对象是否被误伤）：仍只有读码 + 纯谓词证据；
- 阻塞：`War3.exe` pid 42212 仍存活，本会话无权终止（`taskkill /F /PID 42212` 可解）；
- 现场 DLL：基线 `F275545B…5CF07FF3`，未触碰；未提交、未部署。