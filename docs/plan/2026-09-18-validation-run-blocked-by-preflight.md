# 极短验证运行：被前置检查正确拦住（环境不干净）— 2026-09-18

> 本轮目标：执行约 20 秒的极短运行，验证"启动 → 停止 → 恢复"整链闭合。
> **结果：未执行。** 新增的前置检查在**任何改动之前**中止了本次运行，理由成立。

## 1. 新增并由代码强制的前置检查

```python
def require_no_war3_running():
    """前置：环境必须干净。
    教训：上一轮在遗留实例仍持有文件锁的情况下继续操作，使恢复失败、现场停在候选版本。
    因此"启动前必须没有任何 War3/WorldEditor 在跑"这条检查要由**代码**强制，而不是靠人记住。"""
```

它在 SHA 守卫**之后**、备份/部署**之前**调用；不满足即 `return 2`。

## 2. 实测：它确实拦住了

```
[15:28:42] pre  site sha F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3
[15:28:42] cand sha      BBEF3BBC8F52088FD34D6B5F68FEFA111BAA31A7CB2331674AC3917DE0DE357A
[15:28:43] ABORT: a game/editor process is already running; refusing to start a dirty run:
[15:28:43]     War3.exe  42212 Console  1  542,756 K
[15:28:43]   finish or kill it first (taskkill /F /PID <pid>), then retry.
```

**关键**：中止发生在**备份与部署之前** ⇒ 现场**未被改动**（本轮结束后复核仍为基线）。
这正是我想要的"宁可不动，也不要在脏环境里动"的行为。

## 3. 为什么环境不干净

上一轮遗留的 `War3.exe` **pid 42212** 仍然存活（始于 15:13 的实机运行）。
它在隔离桌面上运行，本会话**无权终止**（历次尝试：`Stop-Process -Force`、`taskkill /F /T`、
WMI `Terminate()`、模块内 `_taskkill` 全部"拒绝访问"）。

它还持有 `E:\Work\Warcraft III\d3d9.dll.locked_candidate`（上一轮改名让路留下的文件）。
这些**都不影响现场正确性** —— 站点路径已是基线。

## 4. 解除阻塞所需（人工，一条命令）

```powershell
taskkill /F /PID 42212
```

若该命令仍提示拒绝访问，可在**任务管理器 → 详细信息**中结束 `War3.exe`。
结束后：① `d3d9.dll.locked_candidate` 可删除；② 极短验证运行即可执行。

## 5. 本轮已完成/未完成

| 项 | 状态 |
| --- | --- |
| 前置检查写入驱动 | ✅ 并已实测拦停一次脏运行 |
| `RUN_SECONDS` 改为 20（验证模式） | ✅ |
| 极短验证运行 | ⬜ **被拦下**（环境不干净，符合预期） |
| 实机对照数据 | ⬜ 未取得 |

## 6. 现场状态（本轮未触碰）

```
E:\Work\Warcraft III\d3d9.dll
  SHA : F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3  (基线)
  len : 36,288,789 B
```

## 7. 目标状态

| 项 | 值 |
| --- | --- |
| 全量静态 / meson / runnable | 258-0 / 85-0 / 74-76 |
| 第④问第四子问题 | 仍只有读码 + 纯谓词证据 |
| 目标 | **不可标记完成** |