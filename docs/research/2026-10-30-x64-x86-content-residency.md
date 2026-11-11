# x64 迁移的 x86 内存：渲染内容副本按预算驻留

2026-10-30，Claude 实施。作者明确要求：迁移的初衷是减轻 32 位进程的内存压力，内存优先于继续做性能。
本批只改开发代理与 x64 宿主，默认关闭；不接游戏、不部署玩家 DLL、不提交/推送/发布。Game.dll guard 保持。

## 为什么先做这个

开发代理此前为每个顶点缓冲、索引缓冲和纹理在 x86 里各留一份完整 `std::vector` 副本，从创建一直保留到销毁。
被替换的进程内 DXVK 有 `d3d9.textureMemory`（默认 100 MiB），只同时映射一部分 managed 纹理，
其余放在驱动内存里，不占 x86 地址空间。如果直接把游戏切到代理，纹理会以完整副本留在 x86 里，
可能比现在还占地址空间，抵掉驱动和 DXVK 移出带来的收益。

## 设计

默认关闭。设置 `WARVK_D3D9_X86_BACKING_KIB=<十进制 KiB>` 打开（0 合法，最大 4194304）；
格式不对时 CreateDevice 返回 `D3DERR_NOTAVAILABLE`。

1. **宿主成为内容权威。** Initialize 的 `args[6]` 新增特性字，只定义 bit0 `RetainContent`；其它位、`args[7..15]`
   仍必须为 0。协商后，宿主在 x64 内存里为每个顶点缓冲、索引缓冲和纹理保留打包副本（纹理按布局自身的块行，
   与 x86 tight pitch 一致）。每次已接受的 `UploadBuffer`、`UploadTexture`、`CommitTextureUpload` 之后更新。
   索引缓冲另留一份会话副本，与后端的 `indexBytes` 分开，`ReadContent` 不进入后端。
2. **新 op `ReadContent`。** 只读宿主保留的 CPU 副本：不做 GPU 读回、不 settle，也不改变命令顺序。
   `args[0]` 是纹理 level（缓冲必须为 0），`args[1]` 偏移，`args[2]` 字节数（1–65,408）；回复负载恰好为该长度。
   拒绝条件：未协商、未初始化的 level、区间越界、缓冲读到已初始化前缀之外、多余参数或负载、
   该纹理正处于上传事务中、错误代际。同步调用下这些拒绝都可恢复。
3. **x86 有界缓存。** 每个缓冲、每个纹理 level 是一个驻留项（`tools/render_host/d3d9_residency.h`）。
   Lock 时钉住，不会被淘汰；Unlock 成功后成为最新项，随后按“最久未释放”淘汰，直到未钉住副本的总量
   不超过预算。被淘汰的项释放 x86 内存，`CpuAccount` 同步更新，对象计数不变。
   Lock 一个已淘汰项时：
   - 已初始化的纹理 level、索引缓冲，和非 DISCARD 的顶点缓冲：整块从宿主读回；
   - 从未上传过的 level：和以前一样从零开始；
   - 顶点缓冲 DISCARD：不读回（D3D9 规定旧内容未定义，本 profile 的缓冲都是 WRITEONLY，调用方也不能读）。
   读回被拒或传输失败说明两侧状态不一致，直接 fail-closed（`D3DERR_DEVICELOST`）。
4. **预算模式下延迟分配。** 创建资源时不分配 x86 副本，第一次 Lock 才分配。默认模式仍在创建时分配，
   CPU 账户与以前逐字节相同。
5. **索引缓冲在 draw 时取回。** 代理在批次准入前逐个检查索引范围（延迟拒绝是终止性的），需要本地内容。
   `DrawIndexedPrimitive` 先做原有的廉价检查，再取得索引副本：已淘汰就同步读回（任何同步调用都会先冲刷
   已暂存的批次，远端顺序不变），检查期间钉住，结束后释放并成为最新项。检查逻辑和拒绝结果与以前完全相同。
   索引缓冲 DISCARD 也读回，保证本地检查看到的就是宿主要画的字节。

不变的部分：wire 版本 D9C2、已冻结的 v1 输出行、默认路径、所有 draw、批次和流协议。
新增冷输出：`warvk-d3d9-backing-proxy/v1`（代理设备析构时）和 `warvk-d3d9-backing-host/v1`
（宿主正常结束时），两者始终打印。

## 改动文件

- `tools/render_host/d3d9_wire.h`：`Op::ReadContent`、`InitializeRetainContent`、`Initialization::retainContent`。
- `tools/render_host/d3d9_texture_wire.h`：`PatchTextureLayout`（打包区域写入整布局副本）、`MaxContentRead`。
- `tools/render_host/d3d9_residency.h`（新）：`ContentOwner`、`ResidentContent`、`Residency`
  （侵入式 LRU，`dxvk::mutex` 保护，允许在其它线程上做最终 Release）。
- `tools/render_host/d3d9_proxy.cpp`：
  - 缓冲改用 `extent` 表示大小，`restore()` 负责读回；纹理改为每个 level 一个 vector，账户为各 level 之和；
  - `acquireContent()`/`releaseContent()`：`DrawIndexedPrimitive` 检查索引前后取得、释放索引副本；
  - `BackingBudget()` 读取环境变量，Initialize 时协商，设备析构时打印冷输出行。
- `tools/render_host/d3d9_proxy_stateblock.h`：stream offset 检查改用 `extent`。
- `tools/render_host/dxvk_core/d3d9_host_main.cpp`：`Entry::content`，保留/释放计数，`ReadContent` 处理，冷输出行。
- 测试：
  - `AutoTest/test_render_host_d3d9_wire.cpp`：Initialize 特性字、`ReadContent` op 边界；
  - `AutoTest/test_render_host_texture.h`：`PatchTextureLayout` 对照独立格式表；
  - `AutoTest/test_render_host_residency.h`（新）：淘汰顺序、钉住、超预算单项、零预算、重复 unpin/discard；
  - `AutoTest/test_render_host_fixed_rejection.cpp`：`WARVK_TEST_RAW_RETAIN=1` 时 4 个正向读（纹理整级、纹理区间、
    顶点缓冲、索引缓冲）和 12 个拒绝，否则 1 个拒绝；
  - `AutoTest/test_render_host_d3d9_backing.cpp`（新）：内存工作量，见下文。
- `AutoTest/run_d3d9_factory_clear_gate.py`：
  - `--backing` 增加两个预算臂，以及内存工作量的四种模式和保留模式下的 raw 故障用例；
  - 每个远端用例都检查双方冷输出行（未开时必须 0 淘汰、0 读回、宿主不保留）。

这些文件在 git 中均未跟踪；T0 快照仍待作者确认。

## 内存工作量

`test_render_host_d3d9_backing.cpp` 通过公开 COM 创建 12 张 1024×1024 A8R8G8B8 满 mip 链 managed 纹理
（每张 5,592,404 B，合计 67,108,848 B，约 64 MiB），逐 level 填充上传，逐张绘制核对像素。然后：

- **纹理部分写：** 整 level 加锁、只写左上 16×16，未写的纹素必须保留；整 level 只读回锁逐纹素核对（1,048,576 个）。
- **缓冲部分写：** 整缓冲加锁、只写右半 quad，左半必须保留。

18 张图逐像素精确比较，并在 device / uploaded / drawn / relocked / released / closed 各采一次本进程内存
（`GetProcessMemoryInfo` 加完整 `VirtualQuery` 扫描）。
同一 fixture 依次跑原进程内 x86 渲染器和代理三种模式。

## 构建与门

- `x64-backing-build-20261030-r1`：**失败，保留**。构建期间我改了源码（把驻留类抽到头文件），
  `sourcesPreserved=false`，按规则不追认。
- `x64-backing-build-20261030-r2`：fresh BelowNormal/-j2，sourcesPreserved、protectedUnchanged 通过。
  宿主 `A36E5BAF96CDC26378456C0361E03FC594490BB6A30FFA7DD90E0FFE0F8EC6CA`，5,894,144 B。
- CPU 门 `x64-backing-cpu-20261030-r1`：x86/x64 各 210,734 项，wire 各 1,454,681 项，全部通过。
- GPU 门 `x64-backing-gpu-20261030-r1`（`--stream --fixed-batch --backing`，默认管线，未开 VVL）：
  - 主 fixture 九端（本地前后、同步、分组、流、两个固定批、0 KiB、流 64 KiB）297 张图哈希同为 `0627ed73c4fa582e`，
    与上一批相同；
  - 内存工作量四种模式哈希同为 `544ef0dcb1262325`；
  - 四个 raw 故障用例全部通过，保留模式下 3 个读回与宿主计数一致，12 个拒绝全部可恢复。
  - 回执 4,162,931 B / `EAE70D19BBAB7BBD4862252BC3B2D1278CE59B5114D6C03A3E5EB2B4925AEE8E`。
- 资源寿命门 `x64-backing-lifetime-20261030-r1`（r2 宿主，restricted-Medium）：通过。
- M0 内存门 `x64-backing-memory-20261030-r1..r3`：**失败，保留**。不是本批回归：该门自 10-22 r3 后没跑过，
  fixture 和读方硬编码了逐调用的序号与计数，此后有三处协议变化没有同步：
  - 托管纹理上传改成 Begin/Chunk/Commit 事务，每轮多 2 次调用（r1：Uploaded 检查点宿主序号 15，期望 13）；
  - 首个 draw 发布一次未改变的采样器状态，只多 1 次（r2：PrivateBound 为 20，按每轮推导应为 19）；
  - 宿主 v1 行新增 `queryWaits` 字段，exchanges/accepted/backendCalls 随之变化（r3：fixture 已全部通过，读方拒绝）。

  按调用结构推导新值，而不是照抄观测值：每轮 20 次（6 次创建、2 次缓冲上传、3 次纹理事务、clear/draw/readback、
  6 次延迟销毁），首个 draw 加 1 次采样器；序号 `3,15,20,27,328,969,1910,2437`，exchanges/accepted 2445
  （最终检查点后 8 次收尾调用，与旧的 2244→2252 相同），backendCalls 2243（Begin/Chunk 不计、Commit 计 1 次，
  采样器加 1），轮询模式下 `queryWaits=0`。用 r3 的实际记录离线回放读方，`check_case` 与 1,488 个读方变异全部通过。
  改动只在 `AutoTest/test_render_host_d3d9_memory.cpp`、`run_render_host_memory_gate.py`、`test_render_host_memory_gate.py`（39 项通过）。
- M0 内存门 `x64-backing-memory-20261030-r4`（r2 宿主）：通过；8 个检查点、1,488/1,488 变异；默认模式 CPU 账户
  逐 kind 与期望完全一致（驻留改动没有改变默认路径）。

### 索引缓冲加入之后（r3 宿主）

- `x64-backing-build-20261030-r3`：fresh BelowNormal/-j2，sourcesPreserved、protectedUnchanged 通过。
  宿主 `53E76905B794F665F9D38A7E06887397AEC4BF471170D068CDF216384F0C5FCA`，5,894,144 B。
- CPU 门 `x64-backing-cpu-20261030-r2`：x86/x64 各 210,734 项，wire 各 1,454,681 项，通过。
- GPU 门 `x64-backing-gpu-vvl-20261030-r1`（`--validation --stream --fixed-batch --backing`）：通过。
  - VVL 1.4.365、同步校验开：14 个远端用例（7 个主 fixture 远端、3 个内存工作量远端、4 个 raw 故障）
    均为 0 错误、0 个非状态警告；
  - 主 fixture 哈希仍为 `0627ed73c4fa582e`，内存工作量四种模式仍为 `544ef0dcb1262325`；
  - 保留模式 raw 用例：4 个正向读（含索引缓冲 12 B）共 4,276 B，12 个拒绝全部可恢复，与宿主计数一致；
  - 回执 4,392,026 B / `8B6DC7F56C6F8FD229BCFD394B0DF74F876D8659CDDCB0F8E193C813E0D24472`。
- 资源寿命门 `x64-backing-lifetime-20261030-r2`、M0 内存门 `x64-backing-memory-20261030-r5`：通过。

## 结果（合成工作量，不是游戏）

x86 fixture 进程，单位 MiB。“+上传”是 uploaded 采样减 device 采样。

| 模式 | device 私有 | device 已用 VA | uploaded 私有 | uploaded 已用 VA | +上传 VA | 峰值私有 |
|---|---:|---:|---:|---:|---:|---:|
| 原进程内 x86 DXVK | 270.7 | 675.5 | 270.9 | **739.9** | +64.4 | 278.8 |
| 代理，默认（完整副本） | 2.3 | 58.5 | 66.6 | 126.5 | +68.0 | 66.6 |
| 代理，预算 0 | 2.3 | 58.5 | 2.8 | **59.5** | +1.0 | 6.9 |
| 代理，流模式，预算 8 MiB | 2.3 | 58.5 | 9.0 | 65.5 | +7.1 | 13.1 |

- 同一工作量、同样的像素下，x86 已用 VA 从进程内 DXVK 的 739.9 MiB 降到代理预算 0 的 59.5 MiB。
  其中约 617 MiB 是“进程里有 Vulkan/DXVK/驱动”本身的基线（device 采样时就已占用），约 64 MiB 是纹理映射。
- 进程内 DXVK 的纹理成本体现在映射 VA（mapped 32.4 → 96.8 MiB），不在 Private Bytes 里；
  代理默认模式的完整副本则体现在私有内存（+64.3 MiB）。预算模式把这 64 MiB 移到了 x64 宿主
  （宿主保留峰值 67,109,184 B），x86 只剩被钉住的那一个 level。
- 预算 0：141 次分配、141 次淘汰、8 次读回（两次 4 MiB level 和 6 次 336 B 顶点缓冲，共 8,390,624 B），
  峰值驻留 4 MiB（正在加锁的那一级）。预算 8 MiB：122 次淘汰、1 次读回（4 MiB），
  解锁后最大驻留 6.99 MiB，不超过预算。
- 主 fixture（r2 宿主，索引缓冲未加入）：默认峰值驻留 176,472 B；预算 0 时为 132,612 B（就是最大的一级 257×129），
  277 次淘汰、119 次读回、745,533 B。流 64 KiB 臂只读回 1 次。
- 加入索引缓冲后（r3 宿主，VVL 轮）：默认峰值驻留 176,484 B（多出的 12 B 是唯一的索引缓冲，默认模式也计入
  驻留统计）；预算 0 时 280 次淘汰、121 次读回、745,557 B。多出的 2 次 × 12 B 都是这个索引缓冲：
  一次是 `parent` fixture 的两字节部分 Lock（未写的 10 B 必须保留），一次是深度 draw 前的索引检查，像素不变。
  流 64 KiB 臂 5 次淘汰、仍只读回 1 次。
- 内存工作量不含索引缓冲，r3 宿主下各采样点与上表相差不超过 1.1 MiB（预算 0 的 uploaded 已用 VA 为 58.5 MiB），
  表中数字仍取 r2 那一轮。

## 限制

- **合成工作量，不是游戏。** 进程内基线 617 MiB 的构成（驱动、DXVK 堆、管线）没有拆分归属；
  游戏里 WarVK 加载的是同一套驱动栈，但游戏的数字仍要靠接线后同图 A/B 来测。
- **只是机制。** 开发 profile 有 16 个纹理槽、8 个缓冲槽、1024 边长、无 Present。
  游戏能跑在代理上之前还缺完整 D3D9 覆盖和 Present。
- **读回是同步往返。** 预算远小于每帧会重新加锁的工作集时，每次 Lock 都要读回。
  本工作量的计时没有比较（机器上有其它重负载）。
- **x64 物理内存增加。** 宿主保留副本使 x64 私有内存增加约等于纹理和顶点总量；系统总 RAM 不会因此下降，
  减少的是 x86 地址空间。
- **宿主保留内容不计入冻结的 `D9M1` 宿主内存样本。** 它只出现在新的冷输出行里。
- **索引缓冲被淘汰后，下一次索引 draw 要同步读回。** 读回会先冲刷已暂存的批次。
  热的索引缓冲因为每次 draw 都刷新 LRU 位置而留在 x86；预算 0 是压力臂，不是推荐配置。
- **shader token 仍是完整 x86 副本**（每个至少 16 KiB 容量）；系统内存表面和读回表面不变。

## 下一步建议

1. 强制优化管线下复跑同一门（本批已跑 VVL，见上）。
2. 动态缓冲的 DISCARD 重命名和有界上传窗口（原路线 ③），避免高频 Lock 时整块读回。
3. 游戏接线前定一个默认预算（例如 64 MiB），并把这个 fixture 的四模式比较扩到更接近游戏的纹理和缓冲分布。
4. 若真实负载里索引缓冲读回频繁，再考虑每块 min/max 摘要，或把逐索引检查交给宿主（需作者决定：
   批次内的拒绝会从可恢复变成终止性）。

玩家 DLL 保持 `1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`；
未启动游戏、部署、提交、推送、发布或修改系统。
