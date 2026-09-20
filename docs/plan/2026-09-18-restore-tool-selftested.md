# 恢复工具化：`AutoTest/restore_site_dll.py`（已自测）— 2026-09-18

> 把事故中手工做对的那一步，变成**可复用、可自测**的工具。不依赖实机，不接触现场。

## 1. 工具

`AutoTest/restore_site_dll.py`

```
py AutoTest/restore_site_dll.py              # 自动发现基线备份并恢复现场（幂等）
py AutoTest/restore_site_dll.py --selftest   # 在临时目录自测两种路径，不接触现场
```

核心仍是**改名让路**：把被锁的 site 改名为 `<site>.locked_<stamp>`，原路径即刻空闲，
再把备份复制进去；被锁时同样有效，且不影响正在使用旧映射的进程。

## 2. 自测结果（本轮实测，全部在临时目录）

```
[15:29:25] restored via rename-park (try 0); parked=d3d9.dll.locked_20260918-152925
[15:29:25] selftest 1 (restore to target): PASS
[15:29:25] already at target (try 0)
[15:29:25] selftest 2 (idempotent when already target): PASS
[15:29:25] selftest 3 (rename-park produced a parked file): PASS
EXIT=0
```

覆盖了三条性质：①能把"非目标"改回目标；②已是目标时幂等、不做多余操作；③确实走的是改名路径（产生了 parked 文件）。

## 3. 一次**正确拒绝**的现场实测

```
py AutoTest/restore_site_dll.py
[15:29:25] no usable backup found next to E:\Work\Warcraft III\d3d9.dll
```

这是**正确的行为**，不是缺陷：工具只在找到 **SHA == 基线** 的备份时才动手；
现场现有的两个备份（`519AFA69…`、`A0A51AF2…`）是**更早的版本**，不是基线，因此被拒绝。
⇒ 它**不会**把任意旧版本当成基线装进现场。

## 4. 与驱动内恢复的关系

| 位置 | 角色 |
| --- | --- |
| `live_contrast_palette_objects.py` 的 `restore_site_dll()` | 实机运行**自身**的 finally 恢复（同算法，内联） |
| `restore_site_dll.py` | **独立的补救入口** —— 当驱动进程已经退出、现场却仍停在候选时使用 |

两者算法一致，因此不会出现"驱动修好了但补救工具还是老做法"的分裂。

## 5. 验证程度（如实）

| 证据 | 强度 |
| --- | --- |
| 自测 3/3 PASS（临时目录） | 逻辑正确性 |
| 现场"无基线备份即拒绝" | 安全性（不会误装旧版） |
| **改名让路本身** | **有实机证据** —— 2026-09-18 事故中正是它把现场恢复回基线 |

仍未验证的是：**在真实"被锁"状态下由该脚本自动完成恢复**（本轮现场已解锁，刻意不再制造一次锁定）。

## 6. 现场与目标状态

```
E:\Work\Warcraft III\d3d9.dll
  SHA : F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3  (基线，本轮未触碰)
阻塞 : War3.exe pid 42212 仍存活（隔离桌面，本会话无权终止）
```

| 项 | 值 |
| --- | --- |
| 全量静态 / meson / runnable | 258-0 / 85-0 / 74-76 |
| 实机对照数据 | 未取得（被 pid 42212 阻塞） |
| 目标 | **不可标记完成** |