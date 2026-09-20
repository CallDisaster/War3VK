# 2026-09-17 P0 对象级 palette 证据：离线成本测量（④ 实机前置）

本文件是 **D6 去重载荷口径收尾** 时的**离线成本测量**记录（上级要求：容量 / 最坏探测成本 /
分配 / 关闭开销 / 开启开销）。全部数字来自**宿主机 CPU** 上的生产记录器实例
（PaletteObjectRecorder()，src/d3d9/war3/tools/war3_palette_object_evidence_sink.cpp），
不经过游戏、不创建 Vulkan/D3D9 设备。

> **措辞上限（必须随数字一起引用）**：下面所有纳秒/微秒都是 **CPU 侧代码路径** 的实测值。
> 它们**不是** GPU 开销，**不是**像素证据，也**不构成**"实机性能已达标"或"已稳定"的结论。
> 本次工作没有部署 DLL、没有启动游戏、没有 git 写、没有新增 env。

测量载体（新增，纯宿主机可执行：
src/d3d9/war3/render/tests/war3_palette_object_evidence_cost_test.cpp，
meson 目标 war3_palette_object_evidence_cost，
即 build32/src/d3d9/war3_palette_object_evidence_cost_test.exe）。

三份可核对原始日志（都在 B 树根目录）：
cost_measurement_raw.log（运行 1）、cost_measurement_under_load.log（运行 2，**与全量静态/
meson 门禁并发**，用于说明并发负载会放大抖动）、cost_measurement_clean.log（运行 3，
**空闲机器**，即本文 §9 的原始输出）。

## 1. 测量环境

| 项 | 值 |
| --- | --- |
| CPU | AMD Ryzen 5 5600X 6-Core Processor（6 核 / 12 逻辑处理器，标称 3701 MHz） |
| OS | Windows 11 专业版 10.0.26200 |
| 编译器 | i686-w64-mingw32-g++.exe（MinGW-w64 GCC **15.2.0**，32 位 PE） |
| 优化等级 | **-O3**（-std=c++17 -msse -msse2 -msse3 -mfpmath=sse） |
| 其它相关开关 | -D_GLIBCXX_ASSERTIONS=1、-D_WIN32_WINNT=0xa00、-DNOMINMAX、-mpreferred-stack-boundary=2；**未定义** _DEBUG / NDEBUG（MinGW 下 _DEBUG 恒未定义） |
| 计时源 | QueryPerformanceCounter / QueryPerformanceFrequency = 10,000,000（100 ns 刻度） |
| 本进程子门 | PaletteObjectEvidenceEnabled() = **off**（未设 DXVK_WAR3_FRAME_EVIDENCE*，这是生产默认） |
| 线程 | 单线程；运行 1/3 空闲机器，运行 2 与门禁并发（见 §2） |
| 结构体尺寸 | sizeof(PaletteObjectEvidence) = 237,776 B（定长 1024 槽表 + 计数 + 递归锁）；sizeof(PaletteObjectEvidenceRecord) = 176 B；sizeof(Event) = 392 B |
| 常量 | kWatchCapacity=1024、kPerFrameBudget=64、kNormalBudget=3584、kTerminalReserve=512、kTotalBudget=4096 |

## 2. 样本量与抖动

| 测量 | 样本量 | 计时方式 |
| --- | --- | --- |
| 子门判定 | 2,000,000 次调用 | 单对 QPC 包住整个循环 |
| R/S/E/D 每事件 | 2,000 事件 x 4 阶段 x 2 种发射器 | 单对 QPC 包住整个循环 |
| 满表查找（隔离 Find） | 200,000 次 x 2 键族 x 2 位置（最坏/最好） | 单对 QPC 包住整个循环 |
| 逐次插入（含 QPC 开销） | 1024 次 x 2 键族 | **每事件一对 QPC**（含 QPC 固定开销） |
| CloseWindow | 21 次 x 3 情形 | 每事件一对 QPC |
| 表满后 NoteReject | 1 次（首次）+ 1,000 次 | 首次单独计时；其余为整循环均值 |
| QPC 固定开销 | 200,000 对 | 单对 QPC 包住整个循环 ⇒ **25–26 ns / 对** |

**抖动（同一二进制、同机、三次独立运行的逐行对比）**：

| 指标 | 运行 1（空闲） | 运行 2（与门禁并发） | 运行 3（空闲，§9） | 极差 |
| --- | --- | --- | --- | --- |
| 子门判定 ns/调用 | 1.000 | 1.000 | 1.000 | 0 % |
| R（none）ns/事件 | 32.80 | 32.05 | 31.15 | 约 5 % |
| S（none）ns/事件 | 29.45 | 29.85 | 28.50 | 约 5 % |
| E（none）ns/事件 | 29.20 | 28.75 | 28.80 | 约 2 % |
| D（none）ns/事件 | 29.30 | 29.10 | 28.45 | 约 3 % |
| R（wire-encode）ns/事件 | 104.90 | 105.60 | **148.80** | **约 42 %** |
| S（wire-encode）ns/事件 | 103.65 | 105.95 | 124.45 | 约 20 % |
| E（wire-encode）ns/事件 | 103.95 | 104.30 | 104.00 | 小于 1 % |
| D（wire-encode）ns/事件 | 103.20 | 108.90 | 109.35 | 约 6 % |
| 键族 A 最坏查找 ns | 808 | 835 | 810 | 约 3 % |
| 键族 A 最好查找 ns | 8 | 9 | 8 | 约 12 % |
| 键族 A 平均插入 ns | 383.9 | 380.5 | 381.1 | 小于 1 % |
| 键族 B 平均插入 ns | 174.3 | 168.4 | 168.2 | 约 4 % |
| 表满后续 NoteReject ns | 852 | 866 | 837 | 约 3 % |
| 表满**首次**未知键 ns（单样本） | 14700 | 13800 | 7700 | **约 1.9 倍** |
| CloseWindow 中位 us（512 终态） | 26 | 26 | 26 | 0 % |
| CloseWindow 最大 us（512 终态） | 31 | 27 | 26 | 约 19 % |
| CloseWindow 最大 us（64 终态） | 31 | **49** | 26 | **约 1.9 倍** |

结论仅限：**主体数字在中位意义上重复性约 5 %，但存在 20–90 % 的尾部/单样本抖动
（wire-encode 前两个阶段、表满首次单样本、CloseWindow 最大值；运行 2 明确显示与门禁并发时
抖动放大）**。没有做多次进程/冷启动统计，也没有做 CPU 频率与调度干扰控制。
**不得**据此宣称任何稳定性。

## 3. 容量 / 饱和与最坏探测成本

探测长度由本测试用**生产 PaletteObjectEvidence::HashKey** 与**与 Find/Insert 完全一致的
探测顺序**在影子表上复算（起始槽 = HashKey % 1024；Insert 取从起始槽起第一个非 used 槽，
墓碑可复用；Find 从起始槽线性探测到命中或**真正的空槽**为止）。本场景没有删除 ⇒ 无墓碑。
记录器本身**不暴露**逐键探测次数，因此这个数字是"用生产哈希 + 生产探测顺序的复算"，
并用运行时量做交叉验证（复算与运行时一致：最坏键明显慢于最好键）。

| 键族 | 填入 | 不同起始槽 | 最坏探测次数 | 平均探测次数 | 插入 ns 最大 | 插入 ns 均值 | 最坏键查找 ns（三次） | 最好键查找 ns | 最坏/最好 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A：part = 0x900000 + i*0x40（既有记录器宿主机用例的键族） | 1024 | **6** / 1024 | **1022** | 224.5 | 1600 | 381–384 | **808 / 835 / 810** | 8–9 | 约 93–104 倍 |
| B：part = 0x1000000 + i*8（指针样、8 字节对齐） | 1024 | 47 / 1024 | **987** | 76.5 | 1500–1600 | 168–174 | **784 / 814 / 785** | 8–10 | 约 81–98 倍 |

探测次数分布（访问过的槽数，含起始槽；三次运行逐位相同）：

| 桶 | 1 | 2 | 3–4 | 5–8 | 9–16 | 17–32 | 33–64 | 65–128 | 129–256 | 257–512 | 513–1024 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 键族 A | 6 | 6 | 12 | 24 | 48 | 96 | 171 | 227 | 85 | 202 | 147 |
| 键族 B | 47 | 47 | 94 | 171 | 161 | 118 | 137 | 81 | 77 | 51 | 40 |

- **查找（隔离 Find）**：用"同帧同载荷的第二次 NoteServed"恰好隔离
  Find -> 载荷比较 -> droppedDuplicatePerFrame++ 这条路径（每次 200,000 次，
  dedupWorst=dedupBest=200000 证明确实走到去重分支）。最坏键约 **0.78–0.84 us/次**，
  最好键约 **8–10 ns/次** ⇒ 该表在这种键分布下，单次查找成本可相差约两个数量级。
- **表满后的 NoteReject**：表满（1024 条、watchCount=1024）后，**每个从未出现过的键**
  必然让 Find 走满 1024 槽（droppedProbeLimit++），随后 Insert **一次**立即被拒
  （m_watchCount >= kWatchCapacity 直接返回 nullptr）并累加 droppedTableFull。
  实测 1,001 个未知键 ⇒ droppedProbeLimit +1001、droppedTableFull +1001、closedTableFull +1
  （TableFull 终态每窗口只公告一次）；首次单样本 7.7–14.7 us（抖动约 1.9 倍，且含一次
  TableFull 终态），后续均值约 **0.84–0.87 us/次**。
- **容量上界**：1024 槽就是硬上界；填满过程中 droppedProbeLimit 保持 0（填表不会走满探测
  范围），只有"表满 + 未知键"才会走满 —— 这也是最坏探测长度 = **1024** 的运行时证据。

## 4. 淘汰 / 拒绝触发点（精确条件 + 实测计数）

每个触发点都是**独立窗口**（先 Reset），实测值如下（emittedDelta = 该窗口内从条件构造到
读数之间增加的已发事件数；这些数字在三次运行中**逐位相同**，即确定性）：

| 计数 | 精确触发条件（生产代码路径） | 实测值 |
| --- | --- | --- |
| droppedPerFrame | 同一 render frame 内**已被接受**的非终态事件达到 kPerFrameBudget = 64，第 65 条起 CanEmit(false) 拒绝 ⇒ AccountDrop(false) 记 droppedPerFrame | 一帧内 100 次合法 Reject ⇒ emittedDelta=64、droppedPerFrame=36、droppedPerSession=0 |
| droppedPerSession | 非终态事件达到 kNormalBudget = 3584 = 4096 − 512（终态预留已从总额扣出），此后再来的普通事件记 droppedPerSession | emitted 恰为 3584 后再来 3 条 ⇒ emittedDelta=0、droppedPerSession=3、droppedPerFrame=0 |
| droppedTerminalReserve | CloseWindow 结算 1024 条在途条目时，终态可用预留只剩 kTerminalReserve = 512，第 513 条终态起 AccountDrop(true) 记 droppedTerminalReserve | terminalEmitted=512、droppedTerminalReserve=512、emittedDelta=512 |
| droppedTableFull | m_watchCount 达到 kWatchCapacity = 1024 且 Insert 返回 nullptr（对象没有条目可跟踪）；closedTableFull 只在 TableFull 终态**真的发出**时 +1 | 2 个未知键 ⇒ droppedTableFull=2、closedTableFull=1、terminalEmitted=1、droppedProbeLimit=2 |
| droppedDuplicatePerFrame | 同键 / 同帧 / 同阶段，且载荷**可证等价**：Rejected 比 lastRejectReason；ServedCandidate 比 lastSource + lastHitKey；Enqueued 比 submitSource + submitHitKey + submitSelectionCleared；Drawn 比 drawSource + drawHitKey + drawSelectionCleared | 四阶段各 1 次等价重复 ⇒ droppedDuplicatePerFrame=4、emittedDelta=4（每阶段仍恰好 1 条事件） |
| droppedPayloadConflict | 同键 / 同帧 / 同阶段，但载荷**冲突**（上述任一字段不同）⇒ 不得压缩成"已证明等价的重复"，计冲突并阻止认证 | 四阶段各 1 次冲突 ⇒ droppedPayloadConflict=4、droppedPerFrame=0、droppedPerSession=0、emittedDelta=4 |

六个计数均为 0 或按上表精确增长；没有出现跨桶串位（同一场景里其余 dropped* 全为 0）。

## 5. 分配：记录器路径零堆分配

**机制**：本 TU 替换了**进程级**全局 operator new / new[] / delete / delete[]，把调用次数计入
原子计数器（与宿主机记录器用例 Case11 同一手段，且覆盖同一进程内所有 C++ 堆分配）。

**结果**：Reset + 填满 1024 槽 + CloseWindow，再做 **32 轮**
「Reset + 重新填满 1024 + CloseWindow」（共 33 轮 = 33,792 次 NoteReject + 33 次满表结算）之后：

- newDelta = 0、newArrayDelta = 0、deleteDelta = 0、deleteArrayDelta = 0（三次运行一致）。

_CrtMemDifference **不可用**：本树是 MinGW 编译、_DEBUG 未定义（release CRT），
_CrtMemCheckpoint/_CrtMemDifference 在这种配置下是空实现，因此不能作为判据；
主判据就是上面的进程级 operator new/delete 计数。可执行文件在非 _DEBUG 时也把这一限制
打印出来（ALLOC _CrtMemDifference=unavailable (release CRT: _DEBUG not defined)），
不做"看起来做了 CRT 差分"的暗示。

## 6. 关闭开销：满表 CloseWindow（QPC，us）

满表 = 1024 条在途条目；三种情形通过**先消耗一部分终态预留**构造
（消耗方式：建 1 条条目 = 1 条普通事件 + NoteObjectGone = 1 条终态；消耗 N 条后表会被清回，
再重新填满 1024 条，所以三种情形的**条目数都是 1024**）。每情形重复 21 次（运行 3 = §9）：

| 情形 | reserveConsumed | 实际发出的终态 | droppedTerminalReserve | min us | median us | max us | 关闭前 emitted |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 预留完整 | 0 | **512** | 512 | 26 | 26 | 26 | 1024 |
| 预留只剩 64 | 448 | **64** | 960 | 24 | 24 | 26 | 1696 |
| 预留已耗尽 | 512 | **0** | 1024 | 23 | 23 | 24 | 1792 |

三次运行的中位数范围：512 终态 26/26/26 us，64 终态 24/24/24 us，0 终态 24/24/23 us；
最大值在空闲机器上 24–26 us，而与门禁并发的那次出现 **49 us** 的单样本（见 §2）。

同一数量级说明：满表结算的主要成本是 **1024 条条目的遍历/判定/记账**（每次结算还要构造
PaletteObjectEventRecord），而"实际发出 0 / 64 / 512 条终态"对总耗时的影响只在数微秒量级。

## 7. 开关开销

### 7.1 子门关闭时采集点判定

| 项 | 值 |
| --- | --- |
| 被测函数 | dxvk::war3::tools::evidence::PaletteObjectEvidenceEnabled()（war3_frame_evidence.cpp 的**非 inline** 定义，内部是函数局部 static 配置 + 一次字段读取） |
| 调用次数 | 2,000,000 |
| 实测 | **1.000 ns / 次**（nsPerCallX1000=1000，三次运行逐位相同） |
| 环境 | 未设任何 DXVK_WAR3_FRAME_EVIDENCE* ⇒ 子门 **off**（生产默认） |

含义：采集点在子门关闭时"第一条语句短路"的**判定本身**在纳秒量级；它**不包括**任何测量
（矩阵扫描、键构造、查表、锁、日志）—— 那些在子门关闭时根本不会执行（静态门禁与采集点
自查已钉死，本测量不重复证明）。

### 7.2 开启时每事件成本（CPU 侧）

2,000 事件/组，发射器两种形态（表内为**运行 1 / 运行 2 / 运行 3** 三次值）：

| 阶段 | 发射器 = none（纯记录器状态机）ns/事件 | 发射器 = wire-encode-no-ring（状态机 + 生产转换点 EncodePaletteObjectEvent）ns/事件 |
| --- | --- | --- |
| R NoteReject | 32.80 / 32.05 / 31.15 | 104.90 / 105.60 / **148.80** |
| S NoteServed | 29.45 / 29.85 / 28.50 | 103.65 / 105.95 / 124.45 |
| E NoteEnqueued | 29.20 / 28.75 / 28.80 | 103.95 / 104.30 / 104.00 |
| D NoteDrawn | 29.30 / 29.10 / 28.45 | 103.20 / 108.90 / 109.35 |

- 记录器状态机本体（none）：**约 28.5–32.8 ns/事件**，四个阶段彼此接近（差小于 15 %）。
- 加上生产转换点（按 palette-object/v1 逐位写 12 x u64 + 48 x u32，并调用一次
  ::GetCurrentThreadId()）：**约 104–149 ns/事件**。其中 wire-encode 的 R/S 两次出现
  124–149 ns 的离群（三次运行中只有该组离群），E/D 稳定在 104–109 ns；这说明该组的
  离群更像运行期噪声/缓存态，而不是阶段差异，本文按实测区间如实列出。
- **未包含**：生产发射器还会做 PaletteObjectEvidenceEnabled() 再判定、ActiveSession()、
  以及 Record(session, event)（证据环 Ring::append）。这三项**不在**本测量内（见 §8）。

## 8. 未覆盖 / 不确定（如实列出）

1. **证据环 append（Record(session,event) / Ring::append）成本未测**；wire 导出与 JSON
   序列化成本未测。它们由生产往返测试覆盖**正确性**，但本次没有测成本。
2. **GPU 提交、GPU 执行、像素**：完全没有测量，也不在本文件范围内。本文所有数字都是 CPU 侧。
3. **其余 CPU/编译器/架构**：只在上面那一台 Ryzen 5 5600X + MinGW GCC 15.2.0 -O3 上测过；
   没有 MSVC、没有 64 位、没有其它 CPU 的对照，也没有跨进程/冷启动重复统计。
4. **生产键分布未知**：§3 的最坏探测长度是**两个人造键族**（既有宿主机用例族与指针样族）的
   复算结果：这两个族都表现出**起始槽高度聚集**（1024 个键只落在 6 / 47 个不同起始槽），
   因而"最坏探测约 1e3 槽、最坏查找约 0.8 us"是**这两族**的结论。真实 renderablePart /
   runtimeModelPtr 指针分布、以及生产采集点当前恒为 lifecycleIdentity=0 + identityWeak=true
   的键形态**没有**被采样，故"实机最坏探测成本"**未测**，不得由本文外推。
5. **满表 + 长时间运行**：只测了单窗口与 33 轮 Reset 循环；没有测跨地图/跨设备的长期行为
   （那属于实机门），也没有测多线程并发下的这些路径（并发所有者协议由往返测试场景 E 覆盖
   **正确性**，本次没有测并发成本）。
6. **CloseWindow 的分配行为**：本测量证明记录器路径零堆分配，但没有逐一证明**上层**
   发射消费端（生产 EmitPaletteObjectEvent -> 环 -> 导出）零分配。
7. 子门**开启**时的采集点判定成本未单独测（被测量函数在开/关下函数体相同，都是同一字段读取；
   但本次只在关闭态取数）。
8. 本文没有做多次运行的中位数统计（只有 21 次 CloseWindow 采样与三次整体复跑），
   也没有把 QPC 固定开销从"逐次插入"数字里扣除（QPC_OVERHEAD 约 25–26 ns/对，插入均值
   168–384 ns 里含约 25 ns 的计时开销）。
9. "表满后第一个未知键"是**单样本**（其余 1,000 次才是均值），三次运行相差 1.9 倍，
   因此该值只能当作量级参考。

## 9. 原始输出

完整原始输出（原样粘贴自 cost_measurement_clean.log = 运行 3 / 空闲机器；由
build32/src/d3d9/war3_palette_object_evidence_cost_test.exe 打印，进程退出码 0）：

~~~text
war3_palette_object_evidence_cost_test: offline CPU-side cost measurement (host-only, no game, no GPU, no pixels)
COST_ENV qpcFrequency=10000000 sizeofRecorder=237776 sizeofRecord=176 sizeofEvent=392 kWatchCapacity=1024 kPerFrameBudget=64 kNormalBudget=3584 kTerminalReserve=512 kTotalBudget=4096
QPC_OVERHEAD pairs=200000 nsPerPairX1000=25000 qpcSinkLow=79
PROBE_SHADOW family=A_existing_test_keys(part=0x900000+i*0x40) inserted=1024 watchCount=1024 distinctStartSlots=6 maxProbe=1022 meanProbeX1000=224500 insertNsMax=1600 insertNsMeanX1000=381054 droppedProbeLimit=0 droppedTableFull=0
PROBE_HISTOGRAM family=A_existing_test_keys(part=0x900000+i*0x40) buckets(1,2,3-4,5-8,9-16,17-32,33-64,65-128,129-256,257-512,513-1024)=6,6,12,24,48,96,171,227,85,202,147
PROBE_WORST_KEY family=A_existing_test_keys(part=0x900000+i*0x40) index=1021 probe=1022 ; PROBE_BEST_KEY family=A_existing_test_keys(part=0x900000+i*0x40) index=0 probe=1 (shadow replay of the production Find/Insert probe order + production HashKey)
LOOKUP_RUNTIME family=A_existing_test_keys(part=0x900000+i*0x40) samples=200000 frame=500000 worstProbe=1022 worstNs=810 bestProbe=1 bestNs=8 ratioX1000=101250 dedupWorst=200000 dedupBest=200000 (isolated Find + payload compare + dedup counter; measured on the production recorder instance)
PROBE_SHADOW family=B_pointer_like_keys(part=0x1000000+i*8) inserted=1024 watchCount=1024 distinctStartSlots=47 maxProbe=987 meanProbeX1000=76500 insertNsMax=1500 insertNsMeanX1000=168164 droppedProbeLimit=0 droppedTableFull=0
PROBE_HISTOGRAM family=B_pointer_like_keys(part=0x1000000+i*8) buckets(1,2,3-4,5-8,9-16,17-32,33-64,65-128,129-256,257-512,513-1024)=47,47,94,171,161,118,137,81,77,51,40
PROBE_WORST_KEY family=B_pointer_like_keys(part=0x1000000+i*8) index=1018 probe=987 ; PROBE_BEST_KEY family=B_pointer_like_keys(part=0x1000000+i*8) index=0 probe=1 (shadow replay of the production Find/Insert probe order + production HashKey)
LOOKUP_RUNTIME family=B_pointer_like_keys(part=0x1000000+i*8) samples=200000 frame=500000 worstProbe=987 worstNs=785 bestProbe=1 bestNs=8 ratioX1000=98125 dedupWorst=200000 dedupBest=200000 (isolated Find + payload compare + dedup counter; measured on the production recorder instance)
PROBE_SUMMARY familyA_maxProbe=1022 familyB_maxProbe=987 familyA_distinctStarts=6 familyB_distinctStarts=47 familyA_worstNs=810 familyB_worstNs=785
TABLEFULL firstUnknownNs=7700 subsequentCalls=1000 subsequentNs=837 droppedProbeLimitDelta=1001 droppedTableFullDelta=1001 closedTableFullDelta=1 watchCount=1024 (each unknown key costs exactly one full 1024-slot Find walk plus one immediate Insert refusal)
TRIGGER droppedPerFrame condition="more than kPerFrameBudget(64) accepted events in ONE render frame" emittedDelta=64 droppedPerFrame=36 droppedPerSession=0 droppedTableFull=0 droppedProbeLimit=0 droppedDuplicatePerFrame=0 droppedPayloadConflict=0 droppedTerminalReserve=0 terminalEmitted=0 closedTableFull=0
TRIGGER droppedPerSession condition="normal events reach kNormalBudget(3584) = kTotalBudget(4096) - kTerminalReserve(512)" emittedDelta=0 droppedPerFrame=0 droppedPerSession=3 droppedTableFull=0 droppedProbeLimit=0 droppedDuplicatePerFrame=0 droppedPayloadConflict=0 droppedTerminalReserve=0 terminalEmitted=0 closedTableFull=0
TRIGGER droppedTerminalReserve condition="CloseWindow settles kWatchCapacity(1024) entries while kTerminalReserve(512) terminals are available" emittedDelta=512 droppedPerFrame=0 droppedPerSession=0 droppedTableFull=0 droppedProbeLimit=0 droppedDuplicatePerFrame=0 droppedPayloadConflict=0 droppedTerminalReserve=512 terminalEmitted=512 closedTableFull=0
TRIGGER droppedTableFull condition="m_watchCount reaches kWatchCapacity(1024) and Insert returns nullptr" emittedDelta=1 droppedPerFrame=0 droppedPerSession=0 droppedTableFull=2 droppedProbeLimit=2 droppedDuplicatePerFrame=0 droppedPayloadConflict=0 droppedTerminalReserve=0 terminalEmitted=1 closedTableFull=1
TRIGGER droppedDuplicatePerFrame+droppedPayloadConflict condition="same key / same render frame / same stage at most once, decided ONLY by payload equivalence (4 stages, one equivalent + one conflict each)" emittedDelta=4 droppedPerFrame=0 droppedPerSession=0 droppedTableFull=0 droppedProbeLimit=0 droppedDuplicatePerFrame=4 droppedPayloadConflict=4 droppedTerminalReserve=0 terminalEmitted=0 closedTableFull=0
ALLOC warmup sizeofRecorder=237776 sizeofRecord=176 sizeofEvent=392
ALLOC cycles=33 fillPerCycle=1024 newDelta=0 newArrayDelta=0 deleteDelta=0 deleteArrayDelta=0
ALLOC _CrtMemDifference=unavailable (release CRT: _DEBUG not defined); the process-wide operator new/delete counters above are the mechanism used
CLOSE reserveConsumed=0 entries=1024 repeats=21 emittedTerminals=512 droppedTerminalReserve=512 minUs=26 medianUs=26 maxUs=26 emittedBeforeClose=1024
CLOSE reserveConsumed=448 entries=1024 repeats=21 emittedTerminals=64 droppedTerminalReserve=960 minUs=24 medianUs=24 maxUs=26 emittedBeforeClose=1696
CLOSE reserveConsumed=512 entries=1024 repeats=21 emittedTerminals=0 droppedTerminalReserve=1024 minUs=23 medianUs=23 maxUs=24 emittedBeforeClose=1792
GATE predicate=PaletteObjectEvidenceEnabled calls=2000000 nsPerCallX1000=1000 (sub-gate default OFF in this process: off)
EVENT stage=R(NoteReject) emitter=none events=2000 totalNs=62300 nsPerEventX1000=31150
EVENT stage=S(NoteServed) emitter=none events=2000 totalNs=57000 nsPerEventX1000=28500
EVENT stage=E(NoteEnqueued) emitter=none events=2000 totalNs=57600 nsPerEventX1000=28800
EVENT stage=D(NoteDrawn) emitter=none events=2000 totalNs=56900 nsPerEventX1000=28450
EVENT stage=R(NoteReject) emitter=wire-encode-no-ring events=2000 totalNs=297600 nsPerEventX1000=148800
EVENT stage=S(NoteServed) emitter=wire-encode-no-ring events=2000 totalNs=248900 nsPerEventX1000=124450
EVENT stage=E(NoteEnqueued) emitter=wire-encode-no-ring events=2000 totalNs=208000 nsPerEventX1000=104000
EVENT stage=D(NoteDrawn) emitter=wire-encode-no-ring events=2000 totalNs=218700 nsPerEventX1000=109350
EVENT_NOTE all numbers are CPU-side code-path costs; they are NOT GPU work, NOT pixel evidence, and the production emitter's ring append (Record) plus ActiveSession()/sub-gate re-check are excluded here.
UNCOVERED ring append (Record(session,event)) cost, wire export/JSON serialization, GPU submission, GPU execution, pixels, other CPUs/compilers, and any production frame budget under a real game workload are NOT measured here.
COST_VERDICT=PASS checks=38 failures=0
SUMMARY: all checks passed
~~~

（单位约定：字段名里的 X1000 表示"以千分之一为单位打印"，即 meanProbeX1000=224500 ⇒ 平均探测
224.5 次、nsPerEventX1000=31150 ⇒ 31.15 ns/事件、nsPerCallX1000=1000 ⇒ 1.000 ns/调用；
insertNsMax 与 TABLEFULL 的 Ns 字段直接就是纳秒；CLOSE 的 Us 字段直接就是微秒。）

## 10. 复现

~~~powershell
# 构建（本树纪律：Below Normal + 最多 -j2）
cmd /c "start /belownormal /b /wait cmd /c ninja -C build32 -j2 > ninja_j2.log 2>&1"
# 直接运行（子门保持默认关闭；不需要任何 env）
.\build32\src\d3d9\war3_palette_object_evidence_cost_test.exe
# 或经 meson
meson test -C build32 war3_palette_object_evidence_cost
~~~

## 11. 结论（措辞上限）

- 容量上界是硬性的 1024 槽：填满不需要走满探测范围；**表满后每个未知键**都要付出一次完整的
  1024 槽扫描（后续均值约 0.84–0.87 us；首次单样本 7.7–14.7 us，因为还带一次 TableFull 终态）。
- 六个 dropped* 触发点都有精确、可复现的构造，并且计数与构造一一对应（含 D6 的
  droppedDuplicatePerFrame / droppedPayloadConflict 两向）；这些计数在三次运行中逐位相同。
- 记录器路径在"填满 + 关闭 + 32 轮 Reset 循环"下**零堆分配**（进程级 operator new/delete 计数）。
- 满表 CloseWindow 在实测机器上是**数十微秒**量级（21 次采样中位 23–26 us，空闲机器最大值
  26 us；与门禁并发时出现过 49 us 单样本），与实际发出 0 / 64 / 512 条终态关系不大。
- 子门关闭时的采集点判定约 1 ns/次；开启时记录器状态机约 **28.5–32.8 ns/事件**，
  加上生产 wire 转换点约 **104–149 ns/事件**（不含环 append；R/S 组有一次运行出现离群）。
- 以上都是**单机 CPU 侧**数字。它们**没有**回答实机帧预算、并发成本、真实键分布的最坏探测，
  也**没有**、也不能回答任何 GPU/像素问题；不构成稳定或达标结论。
