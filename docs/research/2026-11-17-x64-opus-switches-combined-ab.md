# “全部 Opus 开关”合并同一运行交替 A/B（staging 改动前的基线）

日期：2026-11-17。目的：在 P1 staging 改动开始前，给出本轮各开关同时开启时相对全关的干净基线。

## 1. 配置

**二进制**：
- 宿主：`build64_render_host_p1_20261117_dev`，`a9342ae65becca82`。它含 P1 代码，但 `WARVK_D3D9_UPLOAD_SLICES` 未设置，各次运行实测 `setting=0, recorded=0`。
- 前端：`scratchpad/fixture_build/proxy_wakefix.dll`，`e9599d5be6502ccd`。

**交替的开关**（同一个 N，奇数块全开、偶数块全关）：
- `WARVK_D3D9_RING_WAKE_FIX`：按前端帧计数；
- `WARVK_D3D9_PRESENT_WAIT`：按宿主 Present 计数；
- `WARVK_D3D9_DYNAMIC_CPU_COPY`：按宿主 Present 计数。

前端与宿主的计数在块边界可能差约 1 帧。

**整轮打开的开关**：`WARVK_D3D9_HOST_EXIT_BUFFERED=1` 与 `WARVK_D3D9_HOST_EXIT_GL_WATCHER=close`。退出只发生一次，无法交替；它们不影响帧时间。全关时的退出数据见 `2026-11-17-x64-host-exit-delay.md`。

**统计口径**：
- 前端稳态 = 去掉前 3 块和末块。
- 停顿帧 = 前端帧时间 >900 ms。
- 分析脚本：`scratchpad/combo_analyze.py`。

## 2. 结果

| 地图（目录 `x64-opus-switches-20261117-*`） | 全关 → 全开：前端稳态均值 ms | p50 | p95 | p99 | 扣除停顿后均值 ms | 停顿帧 |
|---|---|---|---|---|---|---|
| Lost Temple `lt-all-ab-r1`（150 s，N=150） | 36.88 → 23.25 | 30.12 → 23.15 | 44.28 → 32.33 | 102.67 → 50.30 | 31.80 → 23.25（−27%） | 11 → 0 |
| Echo Isles `echo-all-ab-r1`（150 s，N=100） | 35.27 → 25.52 | 29.49 → 22.97 | 48.87 → 35.87 | 86.61 → 75.49 | 30.84 → 24.63（−20%） | 10 → 1 |
| 生与死 `sys-all-ab-r1`（170 s，按键，N=50） | 17.43 → 8.68 | 3.08 → 1.19 | 110.61 → 47.51 | 173.92 → 118.77 | 16.00 → 8.49（−47%） | 7 → 1 |

各开关自身的计数（全关 → 全开）：

| 地图 | 丢失唤醒超时（基线已满足） | 宿主队列等待 µs/Present | DiscardCarry ms | CasterCopy ms |
|---|---|---|---|---|
| LT | 10（10.05 s）→ 0 | 1,515 → 443 | 6,965 → 121 | 5,850 → 622 |
| Echo | 10（10.10 s）→ 0 | 1,528 → 431 | 7,531 → 129 | 6,135 → 1,271 |
| 生与死 | 3（3.03 s）→ 0 | 1,549 → 234 | 19,828 → 298 | 19,969 → 1,156 |

全开相位里偶有 >900 ms 帧（Echo 1 个、生与死 1 个），但对应的丢失唤醒超时为 0。它们是别的长帧（推断：加载或场景切换），不是 RecheckMs 停顿。

镜像内存峰值为 1,114,112 B（三张图相同）。

## 3. 退出与游戏内核验

三轮的宿主退出：
- 都找到并关闭了 watcher（`windows=1 posted=1 waited=0`，0 ms）；
- 账本写出 5.0–5.2 ms；
- opengl32 detach 为 65–82 ms（单次 Sleep）；
- primary 退出 94–125 ms，进程 signalled 在 130–174 ms。

回执只失败 `frameNotFlat`：这一系列运行只转储 1 帧，今天所有同类运行都如此。`sessionsEndedClean`、`gameReady`、`captureVerified` 均为 true。

ab2 截图人工核验，画面都正常：
- LT：基地、农民、金矿、UI；
- Echo：同上；
- 生与死：选英雄区、喷泉。

## 4. 限制

- 每张图各 1 轮，机器上有其它负载（LT 运行前 CPU 约 38%）。
- 三个开关同相位切换，表中合计收益无法拆成各自的份额；各自单独的 A/B 见对应文档。
- 默认值都未改。唤醒协议和退出开关需审核；`DYNAMIC_CPU_COPY` 的像素等价由 lead 在冻结轮的阴影/效果门禁中加分支验证。
