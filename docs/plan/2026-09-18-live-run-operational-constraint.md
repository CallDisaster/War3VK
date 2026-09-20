# 实机运行的操作约束：每次运行留下一个我无法结束的孤儿进程 — 2026-09-18

## 1. 已验证成功的事实（20 秒极短验证运行）

```
SHA 守卫 / 备份 / 部署核对   -> 全部通过
启动                         -> ok:true, pid=18948, 隔离桌面 WarVK-P0
运行 20s                     -> 完成
stop_war3                    -> {"ok":false,"stopped":false,"forced":true}  进程未退出
wait_for_process_exit        -> 正确检测到并给出人工告警（新增的加固生效）
restore 用 rename-park       -> "restore: OK via rename-park on try 0"
RESTORE OK                   -> True   现场回到基线 F275545B…5CF07FF3
```

⇒ **整链闭合已被证明，且"进程存活也能恢复"这条关键性质得到实机验证。**
此前这条只有"手工做过一次"的证据，现在是**驱动自动完成**的证据。

## 2. 操作约束（本轮新认识）

每次实机运行都会留下一个 `War3.exe` 孤儿进程，而它**在本会话内无法终止**。
驱动的前置检查（正确地）拒绝在"已有 War3 在跑"的环境里启动，因此：
**一次运行需要一次人工清理。**

| 轮次 | 孤儿 pid | 是否有实机数据 |
| --- | --- | --- |
| 15:13（事故） | 42212 | 无（用户已结束） |
| 15:57（20s 验证） | 18948 | **无**（20 秒太短且未触发导出） |

采集运行因此被拦下：`ABORT: a game/editor process is already running ... War3.exe 18948`。

## 3. 已尝试的终止手段（本轮新增两项）

| 手段 | 结果 |
| --- | --- |
| `Stop-Process -Id 18948 -Force` | 拒绝访问 |
| `taskkill /F /T /PID 18948` | `Access is denied`（提示父进程 PID 29648） |
| （事故轮已试）WMI `Terminate()`、模块内 `_taskkill` | 均失败 |
| **在隔离桌面上启动终结进程** | `CreateProcessW 失败: 2`（该桌面内进程创建不可用） |

⇒ 需要一个**本会话权限之外**的主体来做（用户在交互式会话里结束它）。

## 4. 采集运行已就绪（只差一次干净启动）

驱动已接入取证控制面：

```python
control(pid, "status")   # 启动后 25s
control(pid, "arm")      # 外部 arm 需要 DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED=0（已在 env 中）
control(pid, "trigger")  # 运行 120s 后
control(pid, "export", session)
```

依据：`AutoTest/frame_evidence_control.py`（`--pid <pid> <action> [--session <token>]`，
内部 `_request(pid, "frame_evidence", {"action": ...})`）。

## 5. 需要的一次人工动作

```powershell
taskkill /F /PID 18948
```

（若拒绝访问，用任务管理器 → 详细信息结束 `War3.exe`。）

**这一次之后应当就能拿到证据**：采集运行会在运行中 arm + trigger + export，
而现场恢复**不再依赖进程退出**（rename-park 已经实机验证）。

## 6. 现场状态

```
E:\Work\Warcraft III\d3d9.dll = F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3  (基线，已核对)
残留 d3d9.dll.locked_20260918-155924 (36,297,472 B) 被 pid 18948 占用，进程结束后可删
```