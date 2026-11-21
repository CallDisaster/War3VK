# 生与死宿主 CPU 热路径：CasterCopy 与 DiscardCarry 读的是写合并内存

日期：2026-11-17。范围：`tools/render_host/dxvk_core/d3d9_backend.cpp` 的动态缓冲（DYNAMIC VB）上传与阴影投射体拷贝。

开关 `WARVK_D3D9_DYNAMIC_CPU_COPY`，默认关（行为与现状相同）：`1` 常开；`alternate:N` 按宿主 Present 计数每 N 次切换，奇数块开启。

阴影生命周期没有改动：何时捕获投射体、拷贝哪些范围、何时保留或释放，都与现状相同。开关只改变这两处拷贝的**源地址**，从映射内存改为宿主堆里的同内容镜像。

## 1. 结论

1. **两条路径都在读写合并（WC）的系统内存。**
   - 动态缓冲以 `HOST_VISIBLE|HOST_COHERENT` 申请（`coherentBuffer`）。
   - 本机 DXVK 日志列出的内存类型（`logs/host_warvk-d3d9-host.log`，RTX 4060 Ti，驱动 621.1.0）：
     - Type 2：`HOST_VISIBLE|HOST_COHERENT`，在 Heap 1；
     - Type 3：在 Type 2 基础上加 `HOST_CACHED`；
     - Type 4：`DEVICE_LOCAL|HOST_VISIBLE`，214 MiB BAR。
   - DXVK 分配器（`determineMemoryTypesWithPropertyFlags`/`allocateMemory`）对这组标志只考虑系统内存类型，并从低位开始取，所以选中 Type 2。DXVK 自己把这类内存命名为 “Write-combined system memory”。
   - 实测读带宽与 WC 读一致：DiscardCarry 关闭相位约 149 MB/s，从缓存镜像读约 11.2 GB/s。
2. **从宿主堆镜像读，两条路径的墙钟分别降到原来的 1/75 和 1/19**；前端稳态帧均值下降 35%。
3. **宿主内存代价是 1.06 MiB**：动态 VB 字节数，x64 宿主私有堆，不占 x86 地址空间。动态 IB 原本就有 CPU 副本（`indexBytes`）。

## 2. 同一运行交替 A/B（生与死，按键进入对局）

**证据**：`AutoTest/artifacts/x64-dynamic-read-20261117-sys-ab-r1/`，170 s，`alternate:50`。
- 宿主：`build64_render_host_p0_20261117_dev`（`9ecf4a47b2c8e6c1`）。
- 前端：`b8944a94af92483f`。

| 指标 | 关（读映射内存） | 开（读宿主镜像） |
|---|---|---|
| DiscardCarry 次数 / 字节 | 8,060 / 2.632 GB | 8,059 / 2.633 GB |
| DiscardCarry 墙钟 | 17.64 s（2.19 ms/次，149 MB/s） | 0.235 s（29 µs/次，11.2 GB/s） |
| 投射体拷贝次数 | 327,044 | 327,140 |
| CasterCopy 墙钟 | 17.85 s（54.6 µs/个） | 0.957 s（2.9 µs/个） |
| 每前端帧 DiscardCarry + CasterCopy | 3.36 + 3.40 ms | 0.045 + 0.18 ms |
| 前端帧数 | 5,250 | 5,203 |
| 前端稳态帧均值 ms | 18.38 | 12.01 |
| 前端稳态 p50 / p95 / p99 ms | 3.03 / 113.6 / 126.9 | 2.99 / 54.8 / 127.2 |
| 前端 >900 ms 帧（丢失唤醒） | 5 | 4 |
| 前端帧时间合计 | 106.3 s | 62.2 s |

两相位每帧差约 6.5 ms，与前端均值差 6.4 ms 吻合。p50 低，是因为大量轻帧（加载、界面）；收益集中在重帧，见 p95。p99 由其它成本决定，没有变化。

这里按“每个前端帧”平均。此前 P0 报告的 36.5 / 30.2 ms/帧取自生与死 r2，统计口径不同（推断：按对局重帧计）。

在 `alternate` 下镜像在两个相位都会维护（每次写映射后同步写镜像），所以关闭相位也付了这笔写镜像的小开销。A/B 对开启一侧偏保守。

## 3. 两种做法的内存代价

| 做法 | 额外宿主内存 | 风险 |
|---|---|---|
| 宿主堆镜像（已实现，开关） | 动态 VB 字节数：本轮峰值 1,114,112 B（1.06 MiB）；动态缓冲合计峰值 1,212,416 B（3 个） | 每次写映射多一次缓存内 memcpy；镜像初值为零，从未写过的区域在映射里是未定义（D3D9 中该区域内容本来未定义） |
| 改用 `HOST_CACHED` 类型（Type 3） | 0 B | GPU 每帧读取需要窥探缓存一致性（PCIe snoop）；CPU 写走缓存再被窥探。这改变了 GPU 资源的放置，GPU 侧代价需要另行实测。本轮未实现 |

## 4. 计数器说明与限制

- `dynamicRead.flagsSeen=6` 和 `createdCached=0` 记录的是**申请**的标志（`DxvkBuffer::memFlags()` 返回申请集合），不是实际分配到的类型。
  - 实际类型由上面的日志和分配器顺序推得，并由读带宽佐证。
  - 源码已改为记录实际类型（`storage()->getMemoryProperties()`），下次构建生效，本轮二进制不含这一改动。
- 像素等价没有单独过门：阴影、效果门禁由 lead 安排，本轮不跑。建议下一轮阴影门禁增加 `WARVK_D3D9_DYNAMIC_CPU_COPY=1` 分支，逐像素对比。
- 单轮；机器有其它负载。默认值未改。阴影相关由 Fable 审核。
