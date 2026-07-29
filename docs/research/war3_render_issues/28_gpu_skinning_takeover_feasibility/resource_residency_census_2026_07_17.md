# GPU / CPU 资源驻留普查（2026-07-17）

## 1. 范围与结论

本轮新增的是 **diagnostics-only 普查**，不是释放权限。运行时仅在显式设置
`DXVK_WAR3_RESOURCE_CENSUS=1` 时统计 D3D9 buffer/texture、War3 model cache、shadow runtime
store 与 GPU-skin pool；默认关闭，报告明确给出 `evictionAuthority=false` 和
`performanceComparable=false`。

首轮隔离光影测试图产物：

`AutoTest/artifacts/resource_residency_census_isolated_20260717_025503`

运行身份、`war3.exe/Game.dll/d3d9.dll` 模块、启动环境、报告 PID/creation time、原始进程
HANDLE 终止与最终残留均 exact。该轮只覆盖轻量光影图，不能解释 SunkenCity 约 600 MiB 的全部
缺口；统计也不包含 Game.dll 原始模型分配、driver reserve、所有瞬态 shadow build copy 或已退役
但仍被旧 published store 持有的对象。

## 2. 首轮数字：不可相加的四个层级

| 指标 | 字节 | 准确含义 |
|---|---:|---|
| 已知 live resource logical bytes | 220,534,112 | census 可见资源的逻辑大小 |
| device allocation slices | 222,884,096 | 对应的 GPU allocation slice |
| duplicate host backing | 44,735,294 | 同一逻辑内容同时有 CPU 后备与 GPU 副本 |
| managed texture payload | 44,576,936 | 587 张 `D3DPOOL_MANAGED` 纹理，是 duplicate 主体 |
| allocator used payload | 44,586,688 | 上述 payload 经 allocator 对齐后的使用量 |
| allocator section backing | 67,108,864 | 承载它们的一个 64 MiB pagefile-backed section |
| mapped process VA | 62,822,400 | 该 section 当前映射进 32 位 war3.exe 的视图 |

`44,735,294 B` duplicate 精确拆为：

- managed texture：`44,576,936 B`；
- GPU-skin static mirror：`158,102 B`；
- 一个静态 VB staging：`256 B`。

动态模型环形 VB/IB 的 `1,114,112 + 98,304 B` 是直接映射 GPU allocation，census 中
`duplicate=0`，不能误算成可释放 staging。

GPU-skin static atlas 当前预留 `134,217,728 B`，实际使用 `135,348 B`、被 lease 引用
`135,308 B`，ready resource 7 个。它提示 atlas 初始容量可改为 4--8 MiB 并按页增长，但这是
VRAM 预留，不是 War3 低 2 GiB CPU 地址空间。

## 3. 23,045,172 B 只是 Seal 候选，不是 free 权限

144 张 managed texture、共 `23,045,172 B` 在观察窗中同时满足：

- 每个 mip 曾出现 full-range write Lock；
- 每个 mip 后来都观察到 device upload；
- 300 帧没有再写；
- 没有 READONLY Lock、partial Lock 或 `AddDirtyRect`；
- 格式支持未来 GPU readback。

这只证明“若未来建立完整封存状态机，可能从 GPU 恢复 CPU 内容”。现在不能直接释放，原因是：

1. full-range Lock 只表示返回整张 subresource 地址，不证明调用者实际覆盖每个字节；
2. 当前 managed texture 明确忽略 `D3DLOCK_DISCARD`，generic full Lock 不能据此跳过 readback；
3. DXVK 兼容应用在 Unlock 后保留旧 `pBits`、随后以 `AddDirtyRect` 通知更新的现实行为；直接换址会
   破坏这一兼容合同；
4. 当前 `D3D9CommonTexture` 没有“device-authoritative / no m_data”状态，直接清空 `m_data` 会让下次
   Lock 返回无效地址；
5. 即便释放 23 MiB suballocation，只要同一 64 MiB chunk 仍有 survivor，现有 allocator 也不会
   `CloseHandle` 归还整块 section backing/commit。

## 4. 现有 allocator 的边界与已发现缺陷

`D3D9CommonTexture::UnmapData()` 会调用 `D3D9Memory::Unmap()`，减少该 allocation 的 mapping
引用并保留 section 内容；若共享 range 的 refcount 尚未归零，当前 view 甚至不会立刻解除。下次
Lock 可重新 `MapViewOfFile`，不需要 GPU readback。`D3D9Memory::Free()` 才把范围放回 chunk free list；只有
chunk 完全空时，现有 `FreeChunk` 才关闭 section handle。

但 `UnmapData()` **不保证 Lock 地址语义**：一旦 view 真正解除，后续 `MapViewOfFile` 的地址不保证
与旧地址相同，Unlock 后仍被应用保存的旧 `pBits` 因而可能失效。quiet、当前无 active Lock 也不能证明指针没有逃逸；
同一 1 MiB mapping range 还可能被多个 allocation 共用，因此 unmap 单个 candidate 未必释放任何
进程 VA。由此，VA unmap 目前只能作为显式风险 A/B，不能称为安全释放路线：

1. 若要称为 candidate-aware，必须先实现 quiet、无 active Lock 的 managed texture 精确筛选；
2. 使用既有 `UnmapData()` 时只保证 CPU-authoritative section 内容仍在，不保证旧/新 Lock 地址；
3. 同时记录 unmap/remap 次数、remap wall time、低位 mapped VA 与 process private/commit；
4. 单纯将 `d3d9.textureMemory` 从默认 100 MiB 改为 48 MiB、再 32 MiB 只会触发现有全局 LRU，
   它不筛 census candidate，必须明确标为更宽泛的风险实验；
5. 任意 remap 尖峰或指针生命周期异常即回退，不能把 VA 下降写成 private/commit 下降。

静态复核还发现 `D3D9MemoryChunk::AllocLocked` 的一个独立 allocator 缺陷：free range 分配后若
剩余不足 4 KiB，旧代码把这段尾碎片从 free list 删除，却仍只把调用者请求的逻辑长度保存在
`D3D9Memory` 中。释放时尾碎片不会归还，可能导致所有逻辑 allocation 都已释放而 chunk 仍无法
`IsEmpty()`，从而让 64 MiB section 长期驻留。

2026-07-17 已采用最保守修复：不再吞任何未分配尾段，只有 `range->length == 0` 时才删除 free
range；`D3D9Memory` 的逻辑 Size、Map 长度、UsedMemory 与 1 MiB shared-view 语义均未改变。
同时补齐两个失败边界：零字节请求不再创建空 64 MiB chunk；`MapViewOfFile` 失败时不再发布伪指针、
增加引用计数或计入 mapped bytes。`AutoTest/test_d3d9_memory_chunk_tail_offline.py` 的 7 个确定性用例
覆盖 1/63/64/4095 B 尾段、exact fit、单/多 chunk 乱序 free、零分配和映射失败，最终
used/allocated/chunk 均归零且无零长度 free range。

该修复已随 DLL `2DED992F29C78F6DB527CBAB2599A1B449CE125D9615FA40318D591891CE7F69`
完成 x86 `-j2` build 与隔离 P4 crash gate。它只保证“最终空的 chunk 可以真正闭合”，不能作为
当前约 23 MiB managed texture 候选已具备 free 权限或 steady-map 已减少 64 MiB 的证据。

真正 Seal 必须在 IDA 中证明精确 Game.dll loader callsite：每 mip memcpy 全覆盖、Lock 指针不逃逸、
无 conversion/autogen/shared/cube/volume/AddDirty 路径，并实现：

```text
CpuAuthoritative -> Sealing -> DeviceAuthoritative -> Materializing
```

generic 后续 Lock 必须先完整 materialize；只有另一条独立、精确的 native overwrite proof 才允许
省 readback。为了最终让 64 MiB chunk 变空，还需给 census 增加 `chunkId/livePayload/
candidatePayload/mappedRangeRefs`，再决定按静态/热更新分池、8--16 MiB 小 chunk、dedicated section
或 survivor evacuation，不能仅凭资源级候选字节拍脑袋释放。

## 5. 与 GPU skin 主线的交集

GPU-skin mirror 现已改为持有 `ShadowModelResourceCache` 发布的
`shared_ptr<const ShadowGeosetResourceRecord>` 不可变快照，不再复制
positions/normals/UV/indices/skinning vectors。观察时间戳独立存放，只有消费者内容真的变化才
copy-on-publish；queued miss 与最终 static resource 共享同一个 owning snapshot。本图收益很小，
但它是语义最干净、不会触碰 Game.dll Lock ABI 的第一笔去重。

普查计数已同时收紧：invalid static mirror 不计 duplicate；pending mirror 即使已经分配 atlas 目标
切片，也要等 exact upload 被 producer fence 接受后才晋级 duplicate。首轮轻图
`staticInvalidRecords=0`、`staticPendingRecords=0`，因此已报告的 `158,102 B` 数值不受此修正影响。

第二轮隔离普查产物：

`AutoTest/artifacts/resource_residency_census_isolated_20260717_084857`

同图下 GPU static mirror 的 host duplicate 从首轮 `158,102 B` 降为 `0`，queued/peak queued
host copy 也均为 `0`；总 duplicate 从 `44,735,294 B` 降到 `44,577,192 B`，差值恰为
`158,102 B`。managed texture 主体仍为 `44,576,936 B`，allocator reserve/used/mapped 为
`67,108,864 / 44,586,688 / 62,844,288 B`。本轮 lazy-readback 候选为 `23,094,324 B`；它与
首轮的轻微差异来自采样时资源集合，仍只有观察意义。

缓存自身仍有 `338,468 B` alias duplicate；这是下一笔可在不触碰 D3D9 Lock ABI 的范围内继续
合并的对象。更重地图下一轮应复用同一 isolated runner，再补 process memory、VirtualQuery region
与 allocator chunk closure。直到这些门闭合前，本轮 census 的任何 candidate 都保持
`evictionAuthority=false`。

该 census 使用 DLL
`2DED992F29C78F6DB527CBAB2599A1B449CE125D9615FA40318D591891CE7F69`。随后仅把本轮新增注释和
说明文档统一为中文，最终 DLL
`CF8922D617EA4698A5945D924E5A527795AE32A3ACFA9326ECB098509468FEC2` 已重新通过隔离 P4；没有
重跑 census，也不能把注释版本差异解释成新的内存或性能变化。
