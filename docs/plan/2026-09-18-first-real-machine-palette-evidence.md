# 🎯 首次实机 palette 对象取证：整链打通 + v3 验证 + 109 条真实链路 — 2026-09-18

> 用户将命令行默认权限提升为管理员后，我**自己**结束了孤儿进程，从而解除了长期阻塞。
> 本文记录首次成功的实机取证运行与判据核对结果。

## 1. 权限提升解决了根因

```
user    : DESKTOP-C5HVFLT\Administrator
isAdmin : True
Stop-Process -Id 18948 -Force  ->  OK，进程消失
```

⇒ 历轮所有"拒绝访问"确实是**令牌权限**问题，不是 API 用法问题。
⇒ 每轮孤儿进程需人工清理这一操作约束**也随之解除**。

## 2. 整链首次完整打通（逐次试错得出协议顺序）

驱动在运行中必须按 **`arm → trigger → freeze → export`** 顺序发控制面命令。
我逐次撞出两个错，**都是顺序/字段问题，不是功能问题**：

| 现象 | 根因 | 修正 |
| --- | --- | --- |
| `WaitNamedPipeW failed: 2`（trigger/export 全失败），但 `arm` 成功 | 我把 trigger/export 放在 **`stop_war3` 之后** —— 那时控制面已被关闭 | 移到**运行窗口内**、停游戏之前 |
| `session mismatch` | `session` 在响应的 **`result` 内层**（`war3_frame_evidence.cpp:104`），我只在顶层找 ⇒ 取到 None | 递归查找 `session` |
| `freeze before export` | 漏了 `freeze`：环必须先冻结才能导出 | 加 `freeze` |

最终：

```
freeze : ok:true
export : ok:true   result={"accepted":"6113373", ...}
新导出 : cpu-30772-12582920915-1.json            3,379,545 B
         cpu-30772-12582920915-1.json.inputs.json 47,345,020 B
stop   : ok:true； restore: OK via rename-park； RESTORE OK True
```

## 3. 实机导出核对判据 —— **R-B / R-C / R-D 全部通过**

```
paletteObject version = 3                                  ← R-B ✅（实机成立）
generic reader : ACCEPTED {"paletteObject": 3}             ← 两个读方都接受
palette parser : ACCEPTED formatVersion=3
                 recovered = []                            ← 正常链未被报成「恢复」
                 chains    = 109
```

**⇒ 读方注册缺口的修复是必要且正确的**：修复前读方只注册 {1,2}，
这份实机导出会被**整份拒绝**。我在 round 209 从源码推出的「版本必为 3」现在有了实机证据。

### 计数器（自洽且互相印证）

```
firstSightInserted  : 109      ← 首见链建条目
firstSightEmitted   : 169
terminalEmitted     : 109
closedWindowExpired : 109      ← 窗口正常结束
weakIdentityRecords : 109      ← 与生产采集点传 identityWeak 一致（R2 佐证）
epochUnknownRecords : 109      ← 与 nativeKnown=false 一致（R2 实机佐证）
emitted             : 3693
droppedPerFrame     : 13547
droppedPerSession   : 2253427
```

`firstSightInserted == closedWindowExpired == terminalEmitted == 109`：
**每条链都走完了首见→窗口结束→终态**，没有半截链。

### 每条链的性质（前 5 条样例一致）

```
windowSegment      : 1                                  ← R-C：窗口结束已记录
identityBasis      : wireCarriedInstanceLifecycleProof
identityProven     : false                              ← 未知身份**未被升级**为已证明
identityProofKinds : [0]
```

⇒ 复审约束「**不把未知身份升级为已证明**」与「**正常完成不能叫恢复**」**在实机上成立**。

## 4. 第④问四子问题的当前状态

| 子问题 | 状态 |
| --- | --- |
| ① 选中什么数据 | ✅ 读码 + 本导出（source=Unknown、hitKey=0、弱身份、epoch 未知） |
| ② 为何允许 | ✅ 读码（严格契约路径 + `ProducerGroupCountCovers` 共享谓词） |
| ③ 提交时是否被换掉 | ✅ 读码（TOCTOU 重读 slot/frameTag/epoch 后 `clear()+false`） |
| ④ **正常对象是否被误伤** | **部分**：本跑 109 条链**全部走完**（无半截），是有利证据；但按 R-E 判据**仍需基线对照**才能归因 |

## 5. R-E 仍需一次基线对照运行

按 `2026-09-18-subquestion4-verdict-criteria.md` 的 R-E：
单次运行只能说明"有多少对象未走完"，**不能说明原因**。
本跑的 109/109 全部走完，**没有出现"未走完"的对象**，这本身是很强的有利证据；
但要排除"候选 DLL 是否让某些对象**根本没能进入**取证链"（静默丢失），仍需一次基线 DLL 的同条件对照。

## 6. 现场状态

```
d3d9.dll = 基线 F275545B…5CF07FF3   （已由 rename-park 恢复并核对）
War3 进程 = 无（本轮运行已自行退出，权限提升后无需人工清理）
```

未提交、未部署、未晋升稳定。