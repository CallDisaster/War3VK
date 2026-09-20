# 阶段 C：`ObservationClosed` 首次尝试**被测试拦下并整体回退** — 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。本轮**没有留下代码改动**（已整体回退），
> 但产出了两条**必须遵守**的约束。**树回到 round 7 的全绿状态。**

## 1. 我做的改动

1. `PaletteObjectTerminal` 新增 `ObservationClosed = 7u`（0..6 不动）；
2. `CloseWindow` 的终态决策改为：
   - 加 `hasRejectFact = (firstReason != NotChecked)` 进 `closedChain`（修 K2）；
   - **`chainType == Observation` ⇒ `ObservationClosed`**（放在 `WindowExpired` **之前**）。

## 2. 被拦下的原因（**测试是对的，我的解释过宽**）

```
[FAIL] 24 first-sight observation chain vs reject-recovery chain (25 checks, 1 failures)
FAIL: a never-rejected object closes as WindowExpired, not Recovered
```

⇒ 既有用例明确断言：**从未被拒绝、且从未被接住的对象应当以 `WindowExpired` 结算**。
我把 `ObservationClosed` 的分支排在 `WindowExpired` **之前**，于是把它覆盖掉了。

**结论（Q3 的收敛解释）**：`ObservationClosed` 不是"观察链的一切终态"，而是
**替代 `Unclosed`** 的那一档 —— 即「观察链**曾经推进过**但没有走完」。
「从未被接住」仍是 `WindowExpired`，两种链型都一样。

## 3. 第二条发现：**计数器桶会静默贴错标签**

`CloseWindow` 的 F2 结算桶（`:287-292`）只有三档：

```cpp
if (terminal == Recovered)        closedRecovered++;
else if (terminal == WindowExpired) closedWindowExpired++;
else                                closedUnclosed++;   // <-- ObservationClosed 会落进这里
```

⇒ 一旦发出 `ObservationClosed`，它会被计入 **`closedUnclosed`** —— 一个**错误的标签**
（它恰恰**不是** unclosed）。修它必须：
1. 在计数器结构里新增 `closedObservationClosed`；
2. 头块 JSON 增加同名键；
3. **读方 v4 counters 集**同步接受；
4. 相关静态断言与夹具同步。

**这是一次级联**，本轮上下文不足以正确完成 ⇒ **整体回退**（不留半成品、不留下静默错标的计数器）。

## 4. 正确的下一步（下一轮按此实施，一次做齐）

```
① CloseWindow 终态分支顺序：
     hasRejectFact && closedChain            -> Recovered      （修 K2；必须真实拒绝事实）
     hitCount==0 && !sawServed               -> WindowExpired  （**先于** Observation 分支）
     chainType == Observation                -> ObservationClosed（替代 Unclosed）
     否则                                     -> Unclosed
② 新增 closedObservationClosed 计数器（C++ → 头块 → v4 counters 集 → 静态/夹具）
③ 负向探针：
     · Observation 链永不出现 Recovered（构造"走过 S/E/D 的观察链"）；
     · Recovered 必须带真实拒绝事实（构造 firstReason==NotChecked 的链）；
     · v1/v2/v3 读方遇终态 7 必须**整份拒绝**（已有 TERMINALS_BY_VERSION，需补用例）；
     · 桶标签正确（closedObservationClosed 与终态条数一致）。
```

## 5. 本轮状态（回到全绿）

```
ninja -C build32 -n : no work to do
AutoTest 全量静态    : 259 scripts, 0 failed
meson               : Ok: 85  Fail: 0
evidence test       : SUMMARY: 25 passed, 0 failed
wire roundtrip      : CHECKS=1153 FAILURES=0
候选 DLL            : 36,297,544 B
                      SHA-256 DABA6A83AC642BB942AD8AE849B49D0DEB768CC2D21866ACB5B49F2AAB9B636E
                      （因本轮编辑后回退触发了重新链接；源码已按逐字替换还原，
                        并核对不含 ObservationClosed / hasRejectFact 残余）
站点                : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git                 : 无写操作
```

## 6. 不声称

- **不**声称 `ObservationClosed` 已实现（**未上 wire**，C++ 枚举未留）；
- **不**声称 K2 已修（回退后 `closedChain` 仍**不要求拒绝事实**）；
- **不**声称阶段 C 主体完成；链型仍未参与查找、未上 wire；
- 本轮的产出是**两条约束**（终态分支优先级、计数器桶必须同步）与一次**及时的自我拦截**，
  不是代码进展 —— 如实记档，不得计为已完成项。
