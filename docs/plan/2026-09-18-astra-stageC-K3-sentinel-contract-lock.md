# 阶段 C · K3：**哨兵契约回归锁**（Case 30 由"只打印"变为断言）— 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只加断言，不改产品语义。**

## 1. 为什么锁"未知 ⇒ 不变"

round 39 落地 K3 时，我特意把实现做成**默认惰性**：
`attemptSerial` 为未知哨兵 `~0ull` 时走**既有终身单调判序**，
从而在裁定尚未指定"生产如何递增"之前**不改变任何生产行为**。

但那只是**实现里的一个分支**。若日后有人顺手把哨兵也当作"一个已知尝试号"，
生产行为会**静默改变** —— 而这种改变**不会**被任何现有断言拦住
（Case 31 用的是显式尝试号，走的是另一条分支）。

⇒ 本轮把 Case 30 从"只打印"变成**断言**，锁住这条兼容契约。

## 2. 断言（2 checks）

```cpp
ok &= Require(terminals == 1u,
              "the unknown-sentinel path must still settle exactly one chain");
ok &= Require(std::strcmp(terminal, "Unclosed") == 0 &&
              rec.counters().closedRecovered == 0u,
              "unknown attempt serial MUST preserve the pre-K3 ordering verbatim ...");
```

**⚠️ 重要限定（已写入注释）**：断言值 `Unclosed` 描述的是
**当前（未闭合的）实机行为**，**不是**期望的终态。
它被锁住的原因只有一个：**"未知 ⇒ 不变"本身是要保护的性质**。
期望的正确终态由 Case 31（显式尝试号）覆盖 —— 两者**分工不同**：

| 用例 | 锁什么 |
| --- | --- |
| Case 30 | **未知哨兵 ⇒ 与 K3 之前逐位相同**（兼容） |
| Case 31 | **已知尝试号 ⇒ 尝试内判序**（正确性），并保留同号内回退的违规判据 |

## 3. 状态

```
STATIC=259/0
=== meson ===
Ok:                85
Fail:              0
=== no-work ===
ninja: no work to do.
=== wire ===
wire=0
CHECKS=1160 FAILURES=0
DLL=B44F87F6E388284FDF7274EFE4A1F9C3F9C8DE4F1DAC962D8B14A6AB69D11130
evidence : 31 passed, 0 failed（Case 30 现 2 checks；Case 31 2 checks）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 4. 不声称

- **不**声称 K3 已完整落地 —— **生产调用方仍未给 attemptSerial**，实机判序仍是终身单调；
- **不**声称 `Unclosed` 是**期望**的实机终态（它是要被 K3 生产侧闭合掉的现象）；
- **不**声称本轮做了反向变异探针：**锁一条"保持不变"的契约**，
  其可失败性来自"改动未知分支的行为"，我没有去实测它（**未做，如实记档**）；
- **不**声称"一次尝试"的语义已定；读方未扩 v4 字段集；
- **不**声称窗口维度已进入查找键；**不**声称阶段 C 主体完成；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，
  更不代表阴影已恢复。