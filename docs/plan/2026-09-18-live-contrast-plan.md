# 批次 4 第 ④ 问的实机对照：前置条件确认与执行方案 — 2026-09-18

> 目标唯一剩余项：**「正常对象是否被误伤」需要运行期证据**。本文完成前置检查并给出方案。
> **本轮未部署、未启动游戏**（理由见 §4）。

## 1. 前置条件（本轮实测，全部齐备）

```
现场 d3d9.dll        E:\Work\Warcraft III\d3d9.dll
  大小 / SHA / mtime  36,288,789 B  F275545BAA65A015…  2026-09-18 13:21:47（本轮未改动）
备份（同在站点目录）
  d3d9.dll.519AFA69_backup_20260918            36,283,128 B
  d3d9.dll.A0A51AF2_backup_20260918-092843     36,271,456 B
候选构建
  build32/src/d3d9/d3d9.dll                    36,297,472 B  BBEF3BBC8F52088F…
War3 进程                                            无（不会干扰玩家）
```

## 2. 要回答的问题（外部复审原文）

`选中什么数据 / 为何允许 / 提交时是否被换掉 / **正常对象是否被误伤**`

前三项已由**读码 + 纯谓词测试**回答（`war3_model_hook.cpp:9384-9408`、`war3_skin_palette_admission_test` 6/6）。
第四项不可由读码判定：fail-closed 条件下「正常被误伤」与「不合规被正确拒绝」在源码层面**不可区分**。

## 3. 需要采集的数据（回答 ④ 的最小充分集）

| 数据 | 用途 |
| --- | --- |
| 含 **非零 palette 对象块**的 frame-evidence 导出 | 目前**尚无任何实机非零导出**（批次 3 遗留缺口） |
| 其中每个对象的 `stage` 分布（`FirstSight` / `Enqueued` / `Drawn`）与 `terminal` 分布 | 区分"正常走完"与"被拒/未闭合" |
| `rejectedDelta` / `servedDelta` / `count`（counters） | 量化拒绝比例 |
| 与同一次运行的**场景基线**对比（无候选 DLL 或候选前版本） | 判断"误伤"必须**有对照**，单侧数据不足以断定 |

**关键**：回答"是否误伤"必须有**对照运行**（同一地图、同一路径、同一时长），
单次运行只能给出"有多少对象未走完"，不能证明原因。

## 4. 为什么本轮不执行

执行链为：备份现场 DLL → 部署候选 → 隔离桌面启动（指定地图/时长/env）→ 采集导出 → 分析 → **恢复现场 DLL**。
这是一条**必须整体闭合**的链：若在部署与恢复之间中断，现场将停在一个中间状态。
本轮剩余预算不足以可靠走完整条链，因此**不启动**，以免留下未恢复的现场 —— 这比"做一半"更符合
「不得自动覆盖现场」的既有纪律。

## 5. 执行方案（下一轮，逐步可核）

```
1. 备份现场：复制 d3d9.dll -> d3d9.dll.<当前SHA前8位>_backup_<YYYYMMDD-HHMMSS>
   并记录部署前 SHA（必须与 F275545B… 核对一致后才继续）
2. 确认无 War3/WorldEditor 进程
3. 部署候选（BBEF3BBC…）并核对部署后 SHA 等于候选 SHA
4. 隔离桌面启动（launcher_mode=direct, use_isolated_desktop=True,
   windowed=True, deploy_d3d9_before_launch=False, enforce_video_baseline=False,
   auto_perf_record=True, 地图 (2)ConcealedHill.w3x,
   env: DXVK_WAR3_FRAME_EVIDENCE=1 / _PALETTE_OBJECT=1 / _FRAME_HISTORY_SELF_CONTAINED=0）
5. 触发取证（控制面 arm|trigger|freeze|export，或热键 Ctrl+Shift+C），取得导出
6. 用 analyze_palette_object_evidence.py 统计 stage/terminal/counters 分布
7. **无论如何都恢复现场 DLL**，并核对 SHA 回到 F275545B…
8. 如需"是否误伤"的结论，重复 4-6 做对照运行（对照版本 = 部署前版本）
```

## 6. 纪律要点

- 隔离桌面数据 **不得**当前台性能结论（本项只看对象级证据，不看性能）；
- 采集的是**数据**，不是性能基准；
- 恢复现场是**强制步骤**，不是可选项；
- 未获得对照运行前，**不得**声称"正常对象未被误伤"。

## 7. 当前状态

| 套件 | 结果 |
| --- | --- |
| 全量静态 | 258 / 0 failed |
| meson | 85 Ok / 0 Fail |
| Win32 runnable（独立运行） | 74 / 76（2 项为未记录调用方式的参数化探针） |
| 实机运行 | **未做** |
| 现场 DLL | `F275545B…` **未改动** |
| 未提交 / 未部署 | 是 |
| 目标 | **不可标记完成** |