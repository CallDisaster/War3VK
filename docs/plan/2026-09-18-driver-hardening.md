# 实机驱动加固：修复导致现场未恢复的两个缺口 — 2026-09-18

> 承接 `...-live-site-not-restored.md`。本文只做**驱动侧加固**，**不触碰现场 DLL**。

## 1. 事故暴露的两个缺口

| 缺口 | 后果（实测） |
| --- | --- |
| 恢复只有**一次** `shutil.copy2` 尝试 | 进程仍持有文件锁时抛 `WinError 32`，`finally` 走完而现场停在候选版本 |
| 停止后**未验证进程真的退出** | `stop_war3` 返回 `stopped:false, forced:true`，脚本却继续往下走，直到恢复失败才暴露 |

## 2. 加固内容（`AutoTest/live_contrast_palette_objects.py`）

### 2.1 `restore_with_unlock_retry(bak, site, want, tries=90, delay=10)`

- 语义：**解锁即恢复**的**有界重试**（默认 90 次 × 10s = 15 分钟）；
- 每次先检查是否已是基线（幂等）；成功则核对 SHA 并删除备份；
- 耗尽后**明确报错并保留备份**，而不是静默结束；
- docstring 里写明了它存在的理由（即本次事故），避免后人又把它简化回单次拷贝。

### 2.2 `wait_for_process_exit(pid, timeout_s=60)`

- 用 `tasklist /FI "PID eq <pid>"` 轮询，确认进程**真的**退出；
- 超时则打印明确警告：DLL 仍被锁、现场不会回基线、需人工 `taskkill /F /PID <pid>`；
- 位置：**停止请求之后、取证目录扫描之前** —— 让"进程没退"立刻可见，而不是等到 finally 才暴露。

### 2.3 `finally` 改为调用重试版并如实报告

```python
ok = restore_with_unlock_retry(bak, SITE_DLL, BASE_SHA)
log("RESTORE OK  ", ok)
if not ok:
    log("*** SITE DLL IS NOT AT BASELINE ***")
    log("*** backup kept at: %s" % bak)
```

⇒ 不再出现"`finally` 执行完了所以应该恢复了"这种**未经核对**的假设。

## 3. 下一次实机运行的正确顺序（写入结论）

```
0. 先用**极短运行**（例如 20 秒）验证"启动 → 停止 → 进程退出 → 恢复"整条链；
   只有这条链被证明闭合，才允许延长运行时间。
1. 延长运行 → 采集 → 停止 → 等进程退出 → 恢复 → 核对 SHA。
```

**关键教训**：本事故的根因不是"恢复代码写错了"，而是**把"机制看起来对"当成"机制已验证"** ——
我在没有验证"停止"环节的前提下，就进入了"部署 + 150 秒运行"的组合。

## 4. 现场状态（本轮未变，且未触碰）

```
现场 SHA : BBEF3BBC8F52088FD34D6B5F68FEFA111BAA31A7CB2331674AC3917DE0DE357A  (候选)
pid 42212: 存活
备份     : d3d9.dll.contrast_backup_20260918-151332 存在
守护     : AutoTest/restore_site_dll_watcher_long.py 运行中（15s × 最长 6h）
```

**仍需人工结束 pid 42212**（`taskkill /F /PID 42212`），之后守护会自动恢复。

## 5. 本轮边界

- 只改驱动与文档；**未**再部署、**未**再启动游戏、**未**触碰现场 DLL；
- 加固后的驱动**尚未在实机跑过**（因为在现场恢复前不会再有实机运行）⇒ 它当前的证据只有"语法/编译通过"，
  **不是**"已验证能防住该故障"。这一点必须如实说明。