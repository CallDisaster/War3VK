# R-E 对照：基线不中立 ⇒ 改用同 DLL 的选择入口 A/B — 2026-09-18

## 1. 候选 vs 站点"基线"的实测结果

```
CANDIDATE  cpu-30772-12582920915-1.json  version=3  accepted  formatVersion=3 recovered=0 chains=109
BASELINE   cpu-32852-15083772764-1.json  version=2  REJECTED  "paletteObject extension version 2 has
                                                      unknown fields: ['activeSession', 'appendEntered',
                                                      'armed', 'enqueueBlockReached', 'productionInsertReached', ...]"
```

## 2. 重要发现：站点"基线"不是中立参考

被拒绝的字段名 —— `activeSession` / `appendEntered` / `armed` / `enqueueBlockReached` /
`productionInsertReached` / … —— **正是我在 R3 修复中删掉的那 8 个字段**。

⇒ 站点上的 `F275545B…` **不是上游中立版本**，而是**我自己一个带 R3 回归的中间构建**
（有 palette 取证、无 `firstSightInserted` ⇒ 批次 3 之前、R3 回归之后）。

**两点推论**：

1. **R3 回归确实到达过现场**：那个构建发出的任何导出都会被**两个读方整份拒绝**。
   我的 R3 修复因此不只是"让套件变绿"，而是**恢复了导出可读性**；
2. **"候选 vs 基线"在 palette 证据上不构成有效对照** —— 基线那份根本读不出来，
   无法与候选在"链是否走完"上比较。我原定的 R-E 对照设计前提**不成立**，如实修正。

## 3. 改用真正干净的对照：同一 DLL + 选择入口开关

目标④原话要求「围绕**真实蒙皮选择入口**做正常/异常对照」。
干净的对照应当**同一二进制**、只切换入口行为 ——
而严格契约路径有一个**运行时开关**（无需重编）：

```
DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT = 1  (默认)  -> 走 snapshot 严格准入路径
DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT = 0          -> 该路径关闭
```

依据：`RenderablePartPaletteSnapshotEnabled()` = `GetEnvBoolCached("DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT", true)`。

⇒ 两次运行**同一候选 DLL**、同一地图、同一时长，只改这一个环境变量：

| | 观察量 |
| --- | --- |
| A（=1，严格路径开） | 链数、走完比例、`recovered`、各计数器 |
| B（=0，严格路径关） | 同上 |

**判读**：若"正常对象的链在 A 中走完、在 B 中同样走完"且 A 无额外半截链，
则严格准入**没有误伤正常对象** —— 这比拿一个不中立的基线对照更有说服力。

## 4. 已有证据（候选运行）汇总

```
version          : 3            两个读方都接受
chains           : 109          全部 windowSegment=1
recovered        : 0            正常链未被报成「恢复」
identityProven   : false        未知身份未被升级为已证明
firstSightInserted == closedWindowExpired == terminalEmitted == 109
                 => 每条链都走完「首见 -> 窗口结束 -> 终态」，无半截链
weakIdentityRecords == epochUnknownRecords == 109   => R2 有实机佐证
```

## 5. 下一步

跑 §3 的 A/B（同 DLL，只切 `DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT`），完成 R-E。

## 6. 现场

```
d3d9.dll = F275545B…5CF07FF3（基线运行前后 SHA 均未变，脚本自身核对 unchanged: True）
未提交、未部署新二进制、未晋升稳定。
```