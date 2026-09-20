# 🔴 现场 DLL 尚未恢复：实机对照中断，必须处理 — 2026-09-18

> **本文是第一优先事项。** 现场 `E:\Work\Warcraft III\d3d9.dll` 当前是**候选 DLL**，不是基线。
> 备份完好，自动恢复守护正在运行；但**进程无法从本会话结束**，所以恢复尚未完成。

## 1. 当前确切的现场状态（实测）

```
E:\Work\Warcraft III\d3d9.dll
  SHA   : BBEF3BBC8F52088FD34D6B5F68FEFA111BAA31A7CB2331674AC3917DE0DE357A   <-- 候选（非基线）
  len   : 36,297,472 B
备份（完好，未删除）
  E:\Work\Warcraft III\d3d9.dll.contrast_backup_20260918-151332
  内容 = 基线 F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3
进程
  War3 pid 42212  —— 仍存活，持有该 DLL 的文件锁
```

## 2. 发生了什么（逐步）

| 步骤 | 结果 |
| --- | --- |
| SHA 守卫 | 通过（现场=基线、候选=期望） |
| 备份 | `d3d9.dll.contrast_backup_20260918-151332` 已建 |
| 部署 | 核对通过（部署后 SHA = 候选 SHA） |
| 启动 | **成功** —— `ok:true, pid:42212`，隔离桌面 `WarVK-P0`，`-loadfile Maps\(2)ConcealedHill.w3x` |
| 运行 150s | 完成 |
| `stop_war3` | `{"ok": false, "stopped": false, "forced": true}` —— **进程未退出** |
| **恢复（finally）** | **失败** `PermissionError [WinError 32]` 文件被占用 |

## 3. 为什么没能结束进程（已尝试全部手段）

| 手段 | 结果 |
| --- | --- |
| `Stop-Process -Id 42212 -Force` | 拒绝访问 |
| `taskkill /F /T /PID 42212` | `Access is denied` |
| `Win32_Process.Terminate()`（WMI） | 无效，进程仍在 |
| `war3.stop_war3(force=True)` | 返回 `stopped: true, pid: 0`（因新进程无跟踪状态）实则进程仍在 |
| 其父进程 48660 | **已不存在**（无法通过父进程回收） |

⇒ 该进程运行在**隔离桌面 `WarVK-P0`** 上，本会话对它**没有终止权限**（一致地拒绝访问）。
这与我此前"恢复放在 finally 里就一定安全"的假设不同：**文件锁可以在 finally 之外持续存在**。

## 4. 正在运行的自动恢复守护

`AutoTest/restore_site_dll_watcher.py`（后台作业）每 10 秒尝试一次，最多 200 次（约 33 分钟）：
一旦文件解锁立即恢复、核对 SHA、删除备份。**进程若退出，现场会自动回到基线。**

## 5. 如果需要立即处理（人工，一条命令）

```powershell
# 1) 结束测试进程（可在任务管理器结束 War3.exe pid 42212，或以管理员身份运行下面这条）
taskkill /F /PID 42212
# 2) 恢复现场
Copy-Item 'E:\Work\Warcraft III\d3d9.dll.contrast_backup_20260918-151332' 'E:\Work\Warcraft III\d3d9.dll' -Force
# 3) 核对
#  期望 F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3
#  (Get-FileHash 'E:\Work\Warcraft III\d3d9.dll' -Algorithm SHA256).Hash
```

## 6. 我在这件事上的判断失误

1. **把"恢复放在 finally"当成了充分条件** —— 实际上文件锁可以让恢复失败；必须有**解锁重试**这一层。
2. **没有先验证 `stop_war3` 能真正结束进程**就进入了"部署 + 长时间运行"的组合；
   更稳妥的顺序应是先用一次极短运行验证"启动→停止→恢复"全链，再延长运行时间。
3. 这与我此前几轮反复记录的教训同源：**把"机制看起来对"当成"机制已验证"**。

## 7. 本次实机运行的正面结果（不影响上述问题）

启动链本身被证明可用：隔离桌面创建、`direct` 启动器、`-loadfile` 指定地图、env 覆盖（修正为 JSON 字符串后）全部成功。
⇒ 一旦恢复完成，下一轮只需修掉"停止/恢复"这两环即可完成实机对照。

## 8. 状态

| 项 | 值 |
| --- | --- |
| **现场 DLL** | 🔴 **候选（未恢复）**，备份完好，守护运行中 |
| 全量静态 / meson / runnable | 258-0 / 85-0 / 74-76（与本次运行无关，未受影响） |
| 实机对照数据 | **未取得**（未触发取证导出） |
| 未提交 / 未部署（源码侧） | 是 |
| 目标 | **不可标记完成** |
---

## 9. 更新（2026-09-18 本轮）

### 9.1 状态未变

```
现场 SHA : BBEF3BBC8F52088FD34D6B5F68FEFA111BAA31A7CB2331674AC3917DE0DE357A  (候选)
len      : 36,297,472 B
baseline?: False
War3     : RUNNING pid=42212（StartTime 不可读 —— 典型的"无权限查询该进程"表现）
备份     : d3d9.dll.contrast_backup_20260918-151332 存在
```

### 9.2 又排查了一轮，仍无可用手段

| 方向 | 结果 |
| --- | --- |
| 控制面命令 | `war3_control_plane.cpp` 只实现 `shutdown_session`（已发过，游戏未退出）；无 quit/exit 命令 |
| `hotkey_quit_game`（`War3GameStruct+0x43C`） | 是游戏内热键，进程无响应时不可用 |
| 父进程 48660 | 已不存在，无法经父进程回收 |
| 常规终止（Stop-Process / taskkill /F /T / WMI Terminate） | 全部"拒绝访问" |

⇒ 该进程需要一个**在本会话权限之外**的主体来结束。

### 9.3 我**故意没有**采用的一种方案（并说明原因）

曾考虑用 `MoveFileEx(..., MOVEFILE_DELAY_UNTIL_REBOOT)` 安排"重启时把备份换回"。
**不采用**，因为它有危险的竞态：若守护**先**恢复成功并删除备份，重启时的挂起操作会先把站点 DLL 改名移走，
再尝试把**已不存在**的备份移入 ⇒ **站点 DLL 会直接缺失**。缺失比"停在候选版本"严重得多。

### 9.4 当前正在运行的保障

`AutoTest/restore_site_dll_watcher_long.py`（后台，**每 15s 重试，最长 6 小时**）：
一旦文件解锁立即恢复、核对 SHA、删除备份。**进程若在任何时刻退出，现场会自动回到基线。**

### 9.5 需要人工动作（唯一可靠路径）

在任务管理器中结束 **`War3.exe`（PID 42212）**，或在本机以管理员身份执行：

```powershell
taskkill /F /PID 42212
```

该进程结束后：**守护会自动完成恢复**（无需再执行任何命令）；
若要立即确认，可执行：

```powershell
(Get-FileHash 'E:\Work\Warcraft III\d3d9.dll' -Algorithm SHA256).Hash
# 期望 F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3
```

### 9.6 结论

这是一次**真实的操作事故**：我在未验证"停止"环节的前提下进入了"部署 + 长时间运行"的组合。
在进程结束之前，**不得**把现场视为已恢复，也**不得**继续任何涉及现场 DLL 的操作。