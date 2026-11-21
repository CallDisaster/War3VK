# P1 第一批：宿主纹理上传改用 DXVK staging slice，绑定命令列表与 GPU 退休

日期：2026-11-17。按 `2026-11-17-x64-texture-upload-and-exit-repair-design.md` §6-P1 实施。

> 状态：lead 改为由 Fable 设计和实施 P1。本文记录在改计划之前已经写好并测过的版本：开关默认关，代码仍在工作树中，没有回退。这只作为 Fable 审核或替换时的参考，不代表 P1 已经完成。

范围：只改 `tools/render_host/dxvk_core/d3d9_backend.{h,cpp}` 的 `uploadTextureRegion`、Owner 的上传额度，以及计数器。

开关 `WARVK_D3D9_UPLOAD_SLICES`，默认关（行为与现状相同）：
- `1`：常开；
- `alternate:N`：按宿主 Present 计数每 N 次切换，奇数块开启。

改默认值前由 Fable 审核。以下都没有改动：
- 线协议；
- Begin/Chunk/Commit 事务；
- managed CPU 副本（retainContent）；
- HRESULT 与故障语义。

## 1. 改法

**现状（关）**，每次上传：
1. `settle()` 等 GPU 空闲；
2. 把数据拷进唯一一块 coherent staging；
3. `begin` → `copyBufferToImage` → `submit(true)`，即同步等这一次上传完成。

一帧几十到上百次上传，每次都把流列表切断并等待 GPU。

**开（只在 stream 模式生效；非 stream 时自动退回现状路径）**，见 `d3d9_backend.cpp` `uploadTextureRegion`：
1. **额度**：`throttleUploads(对齐后字节)` 按完成时间线回收已退休的标记。仍超额时，依次：
   - 查询时间线；
   - 若有未提交的 slice，先 `flushStream`；
   - 按最老标记 `vkWaitSemaphores`，上限为原有 `GpuWaitNs`。

   失败会通知设备错误并抛出，于是 quarantine，即 fail-closed。
2. **记录**：`beginStream()`，从宿主自己的 `DxvkStagingBuffer`（4 MiB 块）分配 slice，memcpy 后在**当前流列表**里记录 `copyBufferToImage`。
   - 位置在已记录的 draw 之后、后续 draw 之前，由 DXVK 的屏障和资源追踪保证顺序。
   - 不再 settle，也不同步提交。
3. **提早提交**：本列表累计超过字节额度或块数额度的 1/3 时，`flushStream`（submit(false)）。其余时候随正常的流边界提交（Present、读回、Finish 等）。
4. **在途标记**：每次带 slice 的流列表提交后，记一个标记 {serial, 累计字节, 累计块数}，在时间线到达该 serial 时退休。
   - slice 的内存寿命由 DXVK 自己的资源追踪保证：块仍在使用时，`DxvkStagingBuffer` 会换新块。
   - 额度只负责给总量设上限。`DxvkStagingBuffer::alloc` 本身没有全局上限，所以把有界额度一并带上。
5. **发布**：`initializedLevels`/`uploaded` 在 slice 路径里于“记录成功”之后发布；关闭路径仍是“完成后”发布，两条路径写在同一处。异常时不发布。
6. **生命周期**：shutdown 时先释放 staging 和标记，再销毁设备。销毁、Reset、Finish、Query 和 CPU 读回都不把“已记录”当成“已完成”，它们仍走原有的 settle/等待。

**额度取值**（宿主自定，不是产品内存预算）：
- 在途最多 16 MiB、4096 块；
- 单个列表达到其中任一项的 1/3 就提前提交；
- 单次上传超过额度时独自放行。

对照：进程内 `ThrottleAllocation` 是 16×4 MiB、每次提交 1/3。

## 2. 验收矩阵（设计文档 §7）

**夹具**：`AutoTest/test_render_host_d3d9_upload_order.cpp`（新增，x86，父进程测试框架），在隔离桌面上运行 `scratchpad/p1_order_run.py`。

四个分支：
- 进程内参考；
- 代理 stream+ring，开关关；
- 代理 stream+ring，开关开；
- 代理 ring 非 stream，开关开，必须退回旧路径。

| 场景 | 覆盖内容 |
|---|---|
| full | 同一帧里 draw-old → 全量更新 → draw-new，连续三次，逐像素比较 |
| partial | 两块局部更新，未写区域保留原值 |
| mip | 非零 level，并配合 SetLOD 切换 |
| slices | 一帧 5,000 次 4×4 更新（超过 4096 块额度），中途 4 次 draw，跨多个列表保持顺序 |
| bytes | 24 次 4 MiB 全量更新（超过 16 MiB 字节额度），每 6 次画一次 |
| readonly | 更新后 LockRect(READONLY) 逐 texel 读回最新内容，含局部写过的 level |
| reset | 释放 default pool → Reset → 重建；managed 纹理内容保留并重新绘制 |
| release | 全部释放，宿主自然退出 |

**结果**（order-r2、order-r3）：
- 四个分支全部通过：6 张图、24,576 次像素检查、1,049,152 次 texel 检查、5,038 次更新。
- pixelHash 都是 `13c9a69ab106d325`，与进程内参考一致。
- 宿主均 exit 0，primary 31–47 ms。

开关开启的分支：
- 记录 5,038 个 slice、105 MB，提早提交 15 次；
- 在途峰值正好是 16 MiB 和 4096 块，等于上限，没有越过；
- 宿主上传总耗时 17–19 ms，每次 3.4–3.8 µs。关闭时 0.80–1.06 s，每次 158–210 µs。

非 stream 分支：`recorded=0`，确实退回了旧路径。

order-r3 同时打开了 `WARVK_D3D9_RING_WAKE_FIX=1` 和两个退出开关，结果相同。

**未覆盖**：
- 额度**等待**分支：夹具里 GPU 退休够快，超额时查询时间线即可回收，`creditWaits=0`。要覆盖它，需要能让 GPU 变慢的夹具，或可调小的额度。
- LRU 驱逐后的恢复。
- 故障事务（Begin OOM、缺 chunk、乱序 chunk 等）：本批没有改事务。
- 换图：游戏运行中会话会重建，但没有专门的换图用例。

## 3. 游戏内同一运行交替 A/B

宿主：`build64_render_host_p1_20261117_dev`，`a9342ae65becca82`；前端 `b8944a94af92483f`。退出开关打开，只影响 Finish 之后。

### 3.1 Lost Temple（`x64-p1-staging-slices-20261117-lt-ab-r1`，150 s，alternate:150，运行前 CPU 约 4%）

| 指标 | 关 | 开 |
|---|---|---|
| 纹理上传次数 | 79,993 | 77,383 |
| 宿主上传墙钟 | 32.65 s（408 µs/次） | 0.136 s（1.8 µs/次） |
| 在途峰值 | — | 700 KB / 33 块；提早提交 0；额度等待 0 |
| 前端稳态帧均值 ms | 36.25 | 22.36 |
| 前端稳态 p50 / p95 / p99 ms | 32.89 / 40.39 / 59.57 | 20.09 / 24.66 / 33.07 |
| 前端 >900 ms 帧（丢失唤醒） | 8 | 5 |
| 扣除停顿后均值 ms | 31.78 | 19.53 |

游戏内画面已核验（`ab2.bmp`）：基地、单位、地形、UI 和小地图都正常。

### 3.2 Echo Isles（`x64-p1-staging-slices-20261117-echo-ab-r1`，70 s，alternate:100）

| 指标 | 关 | 开 |
|---|---|---|
| 纹理上传次数 | 16,732 | 13,695 |
| 宿主上传墙钟 | 17.30 s（1,034 µs/次） | 0.096 s（7.0 µs/次） |
| 在途峰值 | — | 700 KB / 34 块；提早提交 0；额度等待 0 |
| 前端稳态帧均值 ms（样本 300 / 400 帧） | 75.88 | 47.03 |
| 前端稳态 p50 / p95 / p99 ms | 55.34 / 136.71 / 365.81 | 34.02 / 73.25 / 173.88 |
| 前端 >900 ms 帧 | 6 | 3 |
| 扣除停顿后均值 ms | 63.35 | 35.50 |

运行短（约 1,100 帧），样本少，只能看方向。游戏内画面已核验（`ab2.bmp`）。

### 3.3 生与死（`x64-p1-staging-slices-20261117-sys-ab-r1`，170 s，按键进入，alternate:50）

| 指标 | 关 | 开 |
|---|---|---|
| 纹理上传次数 | 24,570 | 10,456 |
| 宿主上传墙钟 | 17.04 s（694 µs/次） | 0.069 s（6.6 µs/次） |
| 在途峰值 | — | 1.40 MB / 32 块；提早提交 0；额度等待 0 |
| 前端稳态帧均值 ms | 32.65 | 25.22 |
| 前端稳态 p50 / p95 / p99 ms | 4.62 / 164.17 / 390.09 | 4.52 / 137.66 / 282.28 |
| 前端 >900 ms 帧 | 12 | 5 |
| 扣除停顿后均值 ms | 25.57 | 24.84 |

两相位的上传数量差很多（加载期的成批上传落在哪个相位不由开关决定），所以这里以单次上传成本为准。扣除丢失唤醒停顿后，帧均值只差约 3%。生与死的主成本在 §P4 的 CPU 路径（见 `2026-11-17-x64-shengyusi-host-cpu-paths.md`），不在纹理上传。

停顿在关闭相位明显更多（三张图分别为 8/5、6/3、12/5）。推断：关闭相位的同步上传更多也更慢，丢失唤醒窗口的机会随之增加。`WARVK_D3D9_RING_WAKE_FIX` 打开后这部分会消失（见 `2026-11-17-x64-lost-wakeup.md`）。

游戏内画面已核验（`ab2.bmp`：选英雄区、喷泉、聊天文字都正常）。回执另有 `actionsPosted=false`：最后的 `173:key=X` 发出时游戏已经自行退出，与本开关无关。

### 3.4 小结

| 地图 | 单次上传宿主耗时（关 → 开） | 扣除停顿后前端帧均值（关 → 开） |
|---|---|---|
| Lost Temple | 408 → 1.8 µs | 31.78 → 19.53 ms（−39%） |
| Echo Isles | 1,034 → 7.0 µs | 63.35 → 35.50 ms（−44%，样本少） |
| 生与死 | 694 → 6.6 µs | 25.57 → 24.84 ms（−3%） |

三张图的在途峰值都不超过 1.4 MB 和 34 块，远低于 16 MiB 和 4096 块的额度。没有触发额度等待，也没有触发提早提交。

## 4. 未做与风险

- 默认值没有改，需 Fable 审核。
- 额度等待分支和 LRU 恢复还缺夹具覆盖（见 §2）。
- 回执 `ok=false` 是因为 `frameNotFlat`：本系列运行只转储 1 帧（`--dump-every 100000`），今天所有 P0 式运行都这样。这不是本开关造成的；游戏内画面另由 ab1/ab2 截图核验。
