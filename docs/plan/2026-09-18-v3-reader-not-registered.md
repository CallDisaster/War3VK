# 🔴 重大更正：批次 3 的版本 3 协议**只做了写方**，读方根本不接受 — 2026-09-18

> **本文更正我此前"③ 有版本正常观察链 实现完成"的报告。该报告是错误的。**

## 1. 实测证据（决定性）

用测试文件自带的 fixture 构造器造一份 `version=3` 的导出，分别喂给两个读方：

```
SEGMENTED_VERSION = 2
v3 palette-analyzer -> REJECTED  ValueError: unknown paletteObject extension version 3
                                    (registered versions: [1, 2])
v3 generic-reader   -> REJECTED  ValueError: unknown paletteObject extension version 3
                                    (registered versions: [1, 2])
```

## 2. 这意味着什么

```
写方（C++）: result["version"] = PaletteObjectRecorder().firstSightUsed() ? 3 : 2;   <-- 会发 v3
读方（Python）: PALETTE_OBJECT_BLOCK_FIELDS = {1: {...}, 2: {...}}                    <-- 只注册 1/2
```

⇒ **一旦实机真的走首见链，写方发出 v3，所有读方将拒绝整份导出**（`ValueError`），
而不是"多了一个未知字段"这种局部降级。
这恰好是外部复审反复警告的那类错误，而我却在批次 3 里造了出来。

## 3. 为什么我的门禁没有截住它（根因）

批次 3 的静态门禁（`test_first_sight_observation_chain_static.py`）断言的是**符号存在性**：

```python
assert "PALETTE_OBJECT_FIRST_SIGHT_VERSION=3" in PARSER      # 常量在
assert "5:'FirstSight'" in PARSER                          # 阶段表在
assert "firstSightStageUnderLegacyVersion" in PARSER         # 旧版本守卫在
```

它**从未断言"v3 被读方接受"**。Case24 只测**记录器（C++）**，不经过读方。
⇒ **符号存在 ≠ 端到端可用。** 这是本次漏检的根本原因，与我此前"只读部分输出"、
"把机制看起来对当成已验证"属于同一类错误：**用间接证据替代了端到端证据**。

## 4. 需要修的三处（已定位，**本轮未实施**）

| # | 文件 | 改动 |
| --- | --- | --- |
| 1 | `AutoTest/analyze_frame_evidence.py:29-35` | 新增 `PALETTE_OBJECT_FIRST_SIGHT_VERSION=3`，并把 3 加入 `PALETTE_OBJECT_BLOCK_FIELDS`（顶层字段集与 v2 相同：`version/watchCount/counters`） |
| 2 | `AutoTest/analyze_palette_object_evidence.py:212,659` | `COUNTER_FIELDS` 的校验目前是**精确集合相等**（`set(counters)==COUNTER_FIELDS`）。因写方**只在 v3** 输出 `firstSightInserted`/`firstSightEmitted`，校验必须**按版本**：v1/v2 用原集合，v3 用 `原集合 ∪ {firstSightInserted, firstSightEmitted}` |
| 3 | `AutoTest/test_palette_object_evidence_analysis_static.py:1299` | `for version in (0, 3, 7, 99, -1, "2", 2.0, True)` 把 **3 当成非法版本**；须从该列表移除 3，并新增"v3 被两个读方接受"的**正向**用例（否则门禁会继续把正确行为判为错误） |

### 4.1 为什么必须三处一起改

只改 #1 ⇒ 失败点从"未知版本"变成 #2 的 `counter fields mismatch`（现象改变、结局不变）；
只改 #1+#2 ⇒ 测试 #3 会因"v3 本应被拒绝"而失败。**三处必须同一轮完成并一起验证。**

这就是我**本轮不实施**修复的原因：在上下文将尽时做三处联动改动，极可能留下半截状态，
而"半截"在这里的表现恰是**读者行为与写者再次不一致**，比现在更糟。

## 5. 当前状态（不受本次更正影响的部分）

| 项 | 值 |
| --- | --- |
| 全量静态 | 259 scripts / 0 failed（**注意**：套件全绿并不代表 v3 可用，因为无用例覆盖 v3 端到端） |
| meson | 85 Ok / 0 Fail |
| Win32 runnable | 74 / 76 |
| 实机非零 palette 导出 | **仍无** |
| 现场 DLL | 基线 `F275545B…5CF07FF3`（未触碰） |
| 阻塞 | `War3.exe` pid 42212 仍存活，本会话无权终止 |
| 目标 | **不可标记完成** |

## 6. 对目标完成度的修正

| 项 | 我此前的报告 | **更正后** |
| --- | --- | --- |
| ③ 有版本正常观察链 | "实现完成（Case24 + 静态门禁），解析器侧集成测试待补" | **写方完成；读方未注册 v3 ⇒ 读方不可用；端到端不可用** |

## 7. 教训（第三条，与前两条同源）

1. 此前："只读部分输出" —— 把包装器断言的中间 PASS 当成整体通过；
2. 此前："把机制看起来对当成已验证" —— finally + 一次 copy2；
3. **本次："符号存在当成端到端可用"** —— 静态门禁断言常量存在，却没有一条用例真的把 v3 喂给读方。

⇒ 共同点：**用间接证据替代端到端证据。** 下一轮的修复必须包含一条"v3 往返"的端到端用例，
否则同类漏检还会发生。