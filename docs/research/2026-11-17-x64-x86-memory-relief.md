# x86 内存减负 M1–M4 回执（2026-11-17）

依据是 [地址空间余量](2026-11-17-x64-x86-va-headroom.md) 的 §2–§5（下称“余量报告”）。每条结论标 **已核对**（代码、数据或实测直接看到）
或 **推断**。没有提交、没有部署；玩家目录 `E:/Work/Warcraft III` 没碰，注册表与显示模式没碰。

## 先说结论

| 项 | 改了什么 | 低 2 GiB 的变化（逐块对账，已核对） |
|---|---|---|
| M1 | 探针 `va=regions` 扫满 4 GiB；旧字段含义不变，高半区单列 | 高半区两种模式都 ≈2045 MiB 空闲、最大洞 2030–2043 MiB，只有 7 块共 3.3 MiB |
| M2 | TLSF：现有池放不下的 >8 MB 请求单独 `VirtualAlloc`（“直接块”），释放即整块归还 | 生与死 TLSF 扩展部分 222.4 → 133.6 MiB（−88.8），两种模式 |
| M3 | `RenderQueueTracker` 表 2^18 → 2^16 项 | 24 → 6 MiB（−18），两种模式；门里每帧峰值 97（LT）/ 139（EI）/ 397（生与死）项，0 次探测失败 |
| M4 | 产品 DLL 的四个大静态数组改为按需分配 | 映像 77.7 → 21.9 MiB（−55.8）；代理模式四样都不分配，净省 55.8；进程内调色板表与绘制缓冲搬到堆上，净省时间线 5.2 |

A/B 门（三张图 × 两种模式 × 两轮，12 局全部通过探针检查）与 r1 比，低 2 GiB 空闲：代理模式 LT/EI **+71**、生与死 **+163 MiB**；
进程内 LT/EI **+20**、生与死 **+123 MiB**；最大洞代理模式 +175 / +175 / **+304 MiB**。与上面逐项相加的预期相符（§5）。
中途发现并修掉一个自己引入的泄漏（进程内调色板表三份，§4），修之前那次门作废并保留。

## 0. 构建与对照

- 产品 DLL：dev 构建 `build32_device_owner_mem_20261117_dev`（llvm-mingw clang 22，BelowNormal、-j2）。跑门用冻结副本
  `scratchpad/mem_dll/v3/d3d9.dll`（md5 `3b9c004971d3440a189fc20925deae87`，`SizeOfImage` 22,941,696）。
  前端 `scratchpad/fixture_build/proxy_phase52.dll`、宿主 `build64_render_host_phase52_20261117_a`，与 r1 同一套。
- 对照：`AutoTest/artifacts/x64-phase52-ab-20261117-r1`（下称 **r1**，产品 DLL 为 phase52 冻结构建）。
- **dev 构建用的是当前工作树**，里面同时有别的 agent 在改的东西（覆盖层 UI、门脚本等）。门里的总数差因此不全是本回执的；
  各项的账按“哪块分配变了”逐块对（`scratchpad/mem_va.py`、`mem_diff.py`：TLSF 主池/扩展池/直接块按块头识别，
  产品映像按 `SizeOfImage` 精确匹配，其余按大小桶对比）。

## 1. M1 探针扫满 4 GiB

改动（已核对）：

- `AutoTest/run_x64_game_proxy_probe.py`：`VA_REGIONS_END = 1 << 32`，`va_regions()` 从 0x10000 扫到 4 GiB；新函数 `va_holes()`
  算一段范围的空闲形状。
  - 旧字段**含义不变**：`freeBytes` 仍是原始 MEM_FREE 字节（与 `native_game_va_sample` 同口径）；
    `allocations` / `privateCommittedBytes` / `largestPrivate` 仍只算低 2 GiB。
  - 新字段：`scanBegin/scanEnd`、`low` / `high`（各自 `freeBytes`、`usableFreeBytes`＝洞起点按 64 KiB 上取整后能分到的、
    最大洞、前 5 大洞、按类型的保留/提交）、`highPrivate`。逐块的 `va-regions-*.json` 也含高半区（`base` ≥ 0x80000000）。
- `AutoTest/run_x64_phase52_ab_gate.py`：`LOW_END`；`privateCommittedBytes` / `imageBytes` 只算低半区（与 r1 同口径），
  另加 `highPrivateCommittedBytes`、`highAllocatedBytes`、`scanLowFreeBytes`、`scanLowUsableFreeBytes`、`highFreeBytes`、
  `highUsableFreeBytes`、`highLargestHoleBytes`；`freeBytes` / `largestHoleBytes` 仍取 `vaSample`（低 2 GiB、关窗时），和 r1 比的是同一个数。
  静态 TLS 模板为 0 时不再按 0 字节块去数“份数”。
- 生与死的按键：r1 只在 72 s 按一次空格。机器忙时（别的会话在跑 ML）这张图进程内要载入约 110 s，72 s 的键落在载入画面上，
  进不了游戏（`x64-mem-relief-spot-20261117-sy-inprocess-v1` 就是这样失败的；P0 的一次进程内运行同样）。门里改成 72/86/100/114 s
  各按一次；进了游戏以后空格只是把镜头跳到最近的警报处。**这是相对 r1 的唯一流程差别**。本次门里机器没那么忙，
  生与死四局都在 72 s 那一下进了游戏（就绪 75.0–75.5 s，r1 74.6–75.8 s），后三下落在游戏里；对比图里两种模式镜头位置相同（看过
  `shengyusi-r1-ab2.png`）。

证据（已核对）：高半区在每一轮、两种模式里都是 ≈2044.9 MiB 空闲，7 块分配共 3.3 MiB（其中私有提交 8 KiB），最大洞
2030–2043 MiB。低半区 `scanLowFreeBytes` 与 `vaSample.freeBytes` 相差只在两次采样之间的几秒变化（同一时刻两者相等，
例：生与死代理 spot 911.3 = 911.3）；`usableFreeBytes` 比原始字节少 7.7–12.6 MiB（洞头对齐损失）。

静态测试：`AutoTest/test_x86_memory_relief_static.py::ProbeAddressSpaceScan`（扫到 4 GiB；`freeBytes` 是原始字节、
`usableFreeBytes` 是对齐后字节；门的低半区口径）。

## 2. M2 TLSF 超大请求不再滞留

问题（余量报告 §2.2，已核对）：现有池放不下的大请求，旧做法开一个“请求 + 余量”大小的扩展池；池尾余量进了 TLSF 公共空闲链，
被别的分配占住后，大块释放了池也退不掉。生与死上 4 个池共 82.5 MiB 空闲块退不回系统，扩展池合计 11 个 222.4 MiB。

改动（`src/d3d9/war3/memory/war3_tlsf_pool.cpp/.h`，已核对）：

- `AllocWithExtendLocked`：现有池分配失败、且未超总上限后，若 `size > 8 MB`（`kExtendGranularity / 2`）且对齐 ≤ 64 KiB，
  走 `AllocDirectBlockLocked`：按 4 KiB 取整，用同一个 `AllocateLowRange`（低 2 GiB）拿一段，登记进 64 KiB 页目录（所有权查询照旧 O(1)），
  按基址排序存进 `g_directBlocks`。块里不放 TLSF 控制结构，别的分配落不进去。
- 释放：`TlsfPool_Free` 认出直接块 → 注销页目录 → `VirtualFree(MEM_RELEASE)`。
- `Realloc`：缩小或不变就地返回；变大则新分配、拷贝、释放旧段。`ReallocInPlace`：只在新大小 ≤ 块大小时成功（不做部分退还）。
- `IsExactAllocatedBlockLocked` 认直接块基址（Storm 钩子的整块校验）、拒内部指针；`BlockSize` 返回页取整后的大小；
  `Shutdown` 释放所有直接块；`TlsfPoolStats` 加 `directBlocks / directBytes / directAllocCount`。
- 不变的：小于 8 MB、或池里放得下的请求，路径与以前完全一样；`StormTlsfHeader`（'SBT1'）照样写在返回地址开头
  （直接块基址 64 KiB 对齐，满足 16 字节对齐）。受保护的 M2-x 迁移块没碰。

证据：

- CPU 测试 `AutoTest/test_tlsf_pool_direct_blocks.cpp`（生产代码在 32 位测试进程里跑，构建方法在文件头）：61 项检查 0 失败（已核对）。
  覆盖：填满主池后 19 MB 请求成为直接块（64 KiB 对齐、<2 GiB、可查所有权、整块校验通过、内部指针被拒、块大小、统计）；
  小请求不落进直接块；就地改大小的规则；变大搬家后旧段 `VirtualQuery` 为 MEM_FREE；释放后段归还、二次释放被拒；
  带邻居的 realloc 链逐段归还；7 MB 请求不走直接块。
- 实测（已核对）：生与死扩展部分从 r1 的 11 个池 222.4 MiB（16.0×3、16.69、17.94、19.19、19.75、22.06、22.44、25.69、30.62）
  变成 4 个 16 MiB 常规池 + 3 个直接块（14.07、25.55、30.0 MiB），共 133.6 MiB，−88.8 MiB；两种模式相同（数字见 §5）。
  LT / EI 上两边都没有扩展池，不受影响。

风险（推断）：

- 每个直接块按 64 KiB 分配粒度最多浪费 <64 KiB 地址空间。
- 游戏若反复申请/释放同一个 >8 MB 块，现在每次都是一次 `VirtualAlloc` + `VirtualFree`（以前可复用池内空闲块）。
  `directAllocCount` 只走 `war3dbg::Print`（OutputDebugString），游戏里没采到次数；帧率在门里没有看出差别，但门的帧率本身噪声大。
- 直接块仍在低 2 GiB；地址空间碎片的形状变了（块释放后是一个整洞）。

## 3. M3 RenderQueueTracker 表缩到 2^16

- 归属（已核对）：`RenderQueueTracker()` 构造时 `m_entries.resize(capacity)`，每项 96 B，2^18 项 = 24 MiB 整，进程堆块提交 0x1801000。
  容量来自 `kNativeRenderQueueFastTrackerCapacity = 1u << 18`（`src/d3d9/war3/core/war3_internal_test_config.h:447`，53 个文件包含它）。
- 先量后缩：给 `MarkTagStage` 加了每帧写入数与探测失败计数（每次调用一个 relaxed `fetch_add`），`Reset()`（每帧）折算峰值，
  峰值涨 25% 以上或出现新的探测失败时写产品日志 `WarVK RenderQueueTracker: capacity=… peakEntriesPerFrame=… peakProbeFailuresPerFrame=…
  totalProbeFailures=… frames=… maxProbes=16`。
- 实测（已核对，2^18 时）：生与死代理 7584 帧，每帧峰值 397 项、探测失败 0。
- 改动：`war3_render_queue_tracker.cpp` 里 `kRenderQueueTrackerCapacity = 1u << 16`，取它与配置值的较小者（没改配置头，免得 53 个文件重编）。
  2^16 是峰值的 165 倍；表按帧号回收，老条目可复用。
- 2^16 下的门内日志（已核对，12 局全部 `capacity=65536`、`totalProbeFailures=0`）：LT 每帧峰值 97、EI 139、生与死 396–397；
  进程内 `Reset` 的次数比呈现帧多（生与死一局约 19.5 万次），峰值仍是 396。
- 风险（推断）：探测 16 次都撞满时这个元素不被标记，查询返回“未标记”，进程内按未标记的绘制处理（不是崩溃）；日志会报出来。

## 4. M4 产品 DLL 的静态数组改为按需分配

| 对象 | 大小 | 读者（已核对） | 改法 |
|---|---|---|---|
| `s_renderablePartPaletteBindings` | 33.19 MiB | 只在进程内渲染：`d3d9_device.cpp`、`war3_live_palette_selection.cpp`、`war3_shadow_renderer_core.cpp`、current-draw 发布 → `g_paletteSnapshotCache`；阴影位姿诊断。代理模式的施影者（`war3_proxy_casters.cpp`）不读调色板，`ShadowCaster` 记录里也没有调色板字段 | `ProducerPaletteTable`：代理模式（`proxy::Requested()`）不分配；进程内静态初始化时 `new (nothrow)`，析构时释放 |
| `s_slotBlendedPaletteCache` | 5.50 MiB | 同上 | 同上 |
| `war3shader::g_drawCallBuffers` | 12.00 MiB | 只由 `UpdateRenderContext` 写，管线只在有渲染监听者时调用（代理模式没有设备，永不调用） | `unique_ptr`，第一次 `UpdateRenderContext` 时在 `g_contextMutex` 下分配；`GetDrawCalls` 在未分配时按空帧回答 |
| `timeline::store()` | 5.22 MiB | 只在 `DXVK_WAR3_FRAME_TIMELINE=1` 时用 | 首次使用时在堆上构造（函数内静态 `unique_ptr`）；`NativeFrameSyncInstalled` 的两个标志移到文件级原子量，不再触发构造 |

- 调色板表：所有下标访问前都判空，表不存在时的行为与“查不到”相同（同样的计数器）；`ResetMapSession` 的范围 for 在空表上是空循环。
  `proxy::Requested()` 是函数内静态、只读环境变量，静态初始化阶段调用是安全的（已核对）。
- 映像（已核对）：`SizeOfImage` 77.65 → 21.88 MiB，`.data` 56.65 → 0.74 MiB。
- **中途发现并修掉的错**（已核对）：第一版（v2）表没有析构函数。进程内跑 LT 时看到每张表**三份**（33.19 MiB×3、5.50 MiB×3），
  映像只有一份——即 DLL 在游戏进程里被加载、释放过不止一次（推断：游戏启动时探测 D3D9），每次静态初始化都新分配，旧的漏掉；
  以前数组在映像里，随映像卸掉。进程内低半区私有提交因此 +110 MiB、空闲 −57 MiB。加了析构（释放并置空）后再跑，每张表一份（§5）。
  时间线的 store 同理改成 `unique_ptr`（只在诊断开关打开时存在）。半途停下的那次门保留在
  `AutoTest/artifacts/x64-mem-relief-ab-20261117-r1`（只有 LT 四轮，进程内两轮带泄漏，代理两轮有效）。
- 进程内的账（已核对）：两张表 38.7 MiB 与绘制缓冲 12 MiB 从映像搬到堆上（进程内有渲染监听者，绘制缓冲会被分配），
  时间线默认不分配；加上 M3 的 −18，进程内低半区净省约 23 MiB。代理模式四样都不分配，映像 −55.8 MiB 全部是净省，
  其中调色板表的 38.7 MiB 以前在加载时就被写满（`Matrix4` 默认构造写单位矩阵），是实打实的私有字节。
- 风险：进程内若静态初始化时 `new` 失败（38.7 MiB），表按“不存在”处理——蒙皮调色板查询全部落空，画面退化但不崩溃（推断；加载时地址空间充裕）。

静态测试：`test_x86_memory_relief_static.py::ProducerPaletteTables`（代理模式不分配、有析构、每个下标访问都在判空之后）、
`LazyStores`（绘制缓冲由写者分配；时间线首次使用才构造）。

## 5. A/B 门

命令（持 `game` 锁跑完整 20 分钟，BelowNormal）：`run_x64_phase52_ab_gate.py --proxy fixture_build/proxy_phase52.dll --host
build64_render_host_phase52_20261117_a/.../warvk-d3d9-host-dev.exe --product-dll scratchpad/mem_dll/v3/d3d9.dll`，
输出 **`AutoTest/artifacts/x64-mem-relief-ab-20261117-r2`**。12 局 `probeOk` 全部为真；门退出码 1 只因三张图的
`paceWithinLimit`（r1 同样三条不过，帧率比上限是提议值）。内存的 9 条检查全过。

两轮中位数，MiB；`空闲`/`最大洞` 是 `vaSample`（低 2 GiB、原始 MEM_FREE、关窗时），与 r1 同一个数（已核对）：

| 图 | 模式 | 空闲 r1 → 本次 | 最大洞 r1 → 本次 | 峰值私有字节 r1 → 本次 | 逐项预期的空闲增量 |
|---|---|---|---|---|---|
| LT | 进程内 | 769.2 → 789.8（**+20.6**） | 262.0 → 305.8（+43.8） | 630.3 → 604.6（−25.7） | +23（M3 18 + 时间线 5.2） |
| LT | 代理 | 1337.8 → 1409.2（**+71.4**） | 814.2 → 988.7（+174.5） | 374.3 → 302.2（−72.1） | +74（映像 55.8 + M3 18） |
| EI | 进程内 | 782.4 → 801.8（**+19.4**） | 276.1 → 303.0（+26.9） | 616.6 → 602.6（−14.0） | +23 |
| EI | 代理 | 1348.9 → 1420.4（**+71.5**） | 825.2 → 1000.4（+175.2） | 365.9 → 293.9（−72.0） | +74 |
| 生与死 | 进程内 | 146.7 → 269.3（**+122.6**） | 19.5 → 100.2（+80.7） | 1199.4 → 1072.2（−127.2） | +112（再加 M2 88.8） |
| 生与死 | 代理 | 766.8 → 929.8（**+163.0**） | 186.9 → 491.0（+304.1） | 873.0 → 711.5（−161.5） | +163 |

逐块核对（已核对，`mem_va.py` / `mem_diff.py` 对 r1 同名运行）：

- 产品映像每局都是 21.9 MiB 一份（r1 77.7）；低半区映像保留两种模式都 −55.8。
- 代理模式：没有 33.19 / 5.50 / 12.0 MiB 的表块；跟踪表 6 MiB 一块（r1 24）。生与死代理低半区私有保留 −107.1 = M2 −88.8 + M3 −18。
- 进程内：调色板表各一份、绘制缓冲一份（在堆上），跟踪表 6 MiB；低半区私有提交 +30（LT）——表 38.7 + 绘制缓冲 12 − 跟踪表 18，
  映像 −56，净省约 23。
- 生与死两种模式：TLSF 扩展 4×16 MiB + 直接块 14.07 / 25.55 / 30.0 MiB，四局一样。
- 高半区：每局 2044.9 MiB 空闲。静态 TLS 份数与 r1 相同（进程内 77 份、代理 45 / 38 份），不是本回执的东西。
- 峰值私有字节代理模式省得比地址空间多（LT −72）：调色板表以前加载时就被写满（映像里的写时复制页，算私有字节），
  现在不分配；加上跟踪表 18（推断：剩下 ~15 MiB 没逐页对）。

不作结论的：帧率。本次进程内 LT 186 fps（r1 136）、EI 115（134）、生与死 40（51），代理 29 / 26 / 8.2（29 / 30 / 9.8）——
同一台机器上别的会话在跑 ML，帧率噪声比这些改动可能带来的差别大。对比图两种模式的平均差：LT/EI 1.6–1.9（r1 1.6–1.8），
生与死 6.5–7.0（r1 4.5–5.0）；看过生与死那张，镜头与画面内容一致，差别在选英雄区的单位动作上（推断，未逐像素查）。

## 6. 按工具链

- 上面全部是 **llvm-mingw dev 构建**的实测。这个构建有 3.22 MiB/线程的静态 TLS 模板（进程内约 246 MiB、代理约 122–147 MiB），
  r1 与本次相同，不在本回执范围内。
- **GCC 构建的 1.22.03 发行版没有这个模板**（emutls），所以它的绝对空闲空间比表里多出上述那么多（推断，未实测）。
- 本回执四项都是源码层面的改动，发行版里同样成立（推断，未在 GCC 构建上量）：
  - M4 映像：四个对象都是 `.bss` 零初始化静态对象，类型与 GCC 无关，发行版映像同样少约 55.8 MiB；代理模式同样不分配。
  - M3 跟踪表 −18 MiB、M2 生与死 −88.8 MiB：堆与 `VirtualAlloc`，与编译器无关。
  - 因为发行版的总余量更大，同样的 MiB 占比更小；但生与死进程内的最大洞（r1 只有 19.5 MiB）与 TLS 无关的那部分改善是一样的。

## 7. 失败与保留的产物（都在 `AutoTest/artifacts/`）

| 产物 | DLL | 结果 | 说明 |
|---|---|---|---|
| `x64-mem-relief-spot-20261117-sy-inprocess-v1` | v1 | 失败：`gameReady`、`captureVerified` | 机器忙，载入约 110 s，72 s 的键落在载入画面；只用了其中的 TLSF 块数 |
| `x64-mem-relief-spot-20261117-sy-proxy-v1` | v1 | 只有预期的 `frameNotFlat` | 2^18 下量到每帧峰值 397、0 次探测失败（M3 的依据） |
| `x64-mem-relief-ab-20261117-r1` | v2 | **作废**：只有 LT 四局 | v2 的调色板表没有析构，进程内三份（+110 MiB 私有）。发现后我停掉了自己起的门进程（`mem_gate.sh`、`gate_run.py`、门脚本），没动探针，第四局正常跑完后才放 `game` 锁。其中 LT r1 代理一局 `sessionsEndedClean` 不过，r2 门的 6 局代理没再出现 |
| `x64-mem-relief-ab-20261117-r2` | v3 | 12 局全过；门只因 `paceWithinLimit` 不过 | 本回执的数字 |

冻结的 DLL：`scratchpad/mem_dll/v1`（md5 eaae5847…）、`v2`（5c87e9d9…）、`v3`（3b9c0049…）。

## 8. 剩下的

1. **产品自己的大块放到高半区**（推断，最大的一项）：高半区 2045 MiB 几乎全空，Storm 的有符号比较只约束交给游戏的指针。
   进程内的调色板表 38.7、绘制缓冲 12、跟踪表 6 MiB 都只被产品代码读写，可以 `VirtualAlloc(MEM_TOP_DOWN)` 放到 2 GiB 以上，
   进程内低半区再省约 57 MiB。要先查有没有把这些指针交给游戏或做有符号比较的地方。
2. 进程内 DLL 会被加载、释放多次（三份表是证据，原因推断为游戏启动时的 D3D9 探测）。我只修了自己引入的；别的持有堆内存或
   `VirtualAlloc` 的静态对象（如 TLSF 池是否在早期加载里初始化）没查。
3. M2：直接块的分配次数只走 OutputDebugString，游戏里没采到；`ReallocInPlace` 缩小不退还页；TLSF 主池按需提交（余量报告第 7 项）没做。
4. M3：配置头 `kNativeRenderQueueFastTrackerCapacity` 仍写 2^18，实际上限由跟踪表里的常量压到 2^16；若以后要放大，两处都要改。
5. 代理模式数据层仍装钩子、仍有 6 MiB 跟踪表；它在代理模式下还要不要（只标注施影者），没评估。
6. 静态 TLS（另一位在查）、游戏自己的 128 / 32 MiB 保留与 64 KiB 小保留，不在本回执范围。

## 改动文件

- `src/d3d9/war3/memory/war3_tlsf_pool.cpp`、`war3_tlsf_pool.h`（M2）
- `src/d3d9/war3/render/war3_render_queue_tracker.cpp`、`.h`（M3）
- `src/d3d9/war3/model/war3_model_hook.cpp`、`src/d3d9/war3_shader_api.cpp`、`src/d3d9/war3/tools/war3_frame_timeline.cpp`（M4）
- `AutoTest/run_x64_game_proxy_probe.py`、`AutoTest/run_x64_phase52_ab_gate.py`（M1，持 `probe` 锁编辑）
- 新增 `AutoTest/test_tlsf_pool_direct_blocks.cpp`（CPU 测试，61 项）、`AutoTest/test_x86_memory_relief_static.py`（11 项）
- 相关静态测试已跑：`test_skin_palette_contract_static`、`test_palette_slot_cache_producer_confirmation_static`、`test_frame_timeline_static`、
  `test_shadow_cross_map_lifecycle_static`、`test_current_draw_*`、`test_issue6_render_identity_cache_reset_static`、
  `test_proxy_mode_lifecycle_static` 全过。此前全量 293 个静态脚本里唯一不过的是 `test_shadow_receiver_ab_env_static.py`
  （它读的 `d3d9_war3_pipeline.cpp` 我没改，缺的是别人的 A/B 环境开关）。
