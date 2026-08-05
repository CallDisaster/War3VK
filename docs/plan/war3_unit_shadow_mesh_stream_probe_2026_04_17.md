# War3 Unit Shadow Mesh Stream Probe

Date: 2026-04-17

## Scope

这页只收口本轮为了继续推进“单位阴影完全脱离 VB/IB snapshot/freeze”而新增确认的两层事实：

1. 为什么继续刷新 `runtime geoset` 仍然不能解决单位 `runtime-group miss`
2. 为什么下一步应当把调查重点转向 `meshData` 的 CPU-side stream contract

## Background

本轮之前的主假设是：

1. `VisibleRenderableRegistry` 已经能拿到 `runtimeGeosetPtr/runtimeGeosetDataPtr`
2. 如果对 runtime geoset 做 live recapture，就应该能读到 `0x131320` 写回后的
   remapped `vertex_group_indices`
3. 于是 `semantic shadow` 就能继续走：
   - `static geoset resource`
   - `per-frame runtime palette`
   - `vertex_group_indices -> runtime group palette`

本轮实际验证表明，这个假设不成立。

## Code changes verified in this round

### Runtime geoset recapture

`ShadowModelResourceCache` 已改成：

1. `noteRuntimeModelBinding(...)` 不再急着把 runtime geoset 当成最终静态资源固化
2. `noteRuntimeGeosetBinding(...)` 会按帧做 live recapture
3. runtime record 会打上 `prefersRuntimeContract`
4. `ShadowRuntimeContractCache/ShadowModelResourceStore` 会优先保留 runtime geoset 记录

相关代码：

- `src/d3d9/war3/model/war3_model_resource_cache.h`
- `src/d3d9/war3/model/war3_model_resource_cache.cpp`
- `src/d3d9/war3/shadow/war3_shadow_runtime_contract.h`
- `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`

### Background-only verification

本轮所有验证都使用隔离桌面后台启动，不占前台：

- `launch_war3_test(... use_isolated_desktop=True ...)`
- `wait_for_game_ready(...)`
- `capture_war3_screenshot(...)`
- `stop_war3(... avoid_foreground_switch=True)`

## IDA-confirmed blocker

### `0x6F132230`

IDA 伪代码已再次确认：

- 逐个遍历 source `CGeoset*`
- 对每个 geoset 调 `0x6F1325E0`

建议名：

`CModel_CopyRuntimeGeosetHandlesFromSource`

### `0x6F1325E0`

IDA 伪代码关键事实：

1. 分配新的 `HGEOSET`
2. `newGeoset + 0x0C = sub_6F04F200(sourceGeoset + 0x0C)`
3. `newGeoset + 0x10 = sourceGeoset + 0x10`

建议名：

`CGeoset_CloneHandleAndAliasSourceGeosetData`

### Consequence

这轮已经可以明确确认：

1. `runtimeGeosetPtr != sourceGeosetPtr`
2. 但 `runtimeGeoset + 0x0C` 仍然 alias 到同一份 source `CGeosetData`
3. 因而：
   - 继续 recapture `runtimeGeosetPtr`
   - 或继续 recapture `runtimeGeosetDataPtr`

都**不会**自动拿到 `0x131320` 里“目标 merged/runtime geoset data”的 remapped
`vertex_group_indices`

这就是本轮 recapture 已经上线、但单位 `runtime-group miss` 样子几乎不变的根因。

## Live probe results

### Background probe summary

后台隔离桌面 probe 的代表性结果：

- `avgFps = 72.328`
- `fallbackDrawCount = 629`
- `semanticCoreResolved = 52`
- `upperLayerResolveRuntimeGroupPaletteMiss = 460`
- `upperLayerResolveAuthoritativeSkinned = 55`

对应报告：

- `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_17_23_05_50.html`

### Representative miss logs

典型 miss 仍然长这样：

```text
model=1b232844 runtime=307f5f64 geoIdx=0
vertexGroups=778 uniqueSlots=21 maxSlot=20
groupCount=21 matrixIndices=23
poseCount=2 rootFinalCount=2
```

以及：

```text
model=1b2322f0 runtime=1d28af40 geoIdx=0
vertexGroups=615 uniqueSlots=79 maxSlot=78
groupCount=79 matrixIndices=168
poseCount=3 rootFinalCount=3
```

结论不变：

1. `rootOwnedModelResource == resource.modelResourcePtr`
2. `runtimeModel->final_pose_matrix_count` 的确只有 `1~3`
3. 但当前资源侧 `vertex_group_indices / matrix_group_sizes / matrix_indices`
   仍然看起来像 source geoset contract

所以当前失败点不是“找错 runtimeModel”，而是“当前喂给 semantic core 的 geoset
contract 不是当前帧真实 draw contract”。

## New meshData findings

本轮在 `SemanticCore: runtime-group miss` 日志里额外打印了：

- `meshData + 0x10`
- `meshData + 0x48`
- `meshData + 0x4C`
- `meshData + 0x58`
- `meshData + 0xF0`

### What is already clear

1. `meshData + 0xF0` 在这些单位上不是 runtimeModel 指针
   - 日志里稳定表现为小整数
   - 这更像 `binding / pose ctx / merged-slot-like id`
2. `meshData + 0x10` 指向一段可读的 float3 数据
   - 读出的前三个 float 看起来像真实顶点位置
3. `meshData + 0x4C` 指向一段可读的 packed byte 流
   - 代表性样本：
     - `0x10000101`
     - `0x07030502`
     - `0x24141313`
     - `0x02000202`
   - 这更像“当前帧动态几何的辅助 per-vertex stream”，而不是普通纹理坐标

### What is not yet trusted

当前 `MeshData` 结构里的这两个偏移还不能继续直接当真：

- `+0x48`
- `+0x58`

原因：

1. 按现有命名把它们当 `stride` 时，读出来的值明显不像标准 stride
2. 更像 `count / header / binding-side metadata`
3. 因此 `MeshData` 在世界对象 skinned 路径上的 stream layout 还需要再对一次

## Practical interpretation

到这一步，工程上更靠谱的结论已经不是：

> “继续从 runtime geoset data 里找 remapped vertex_group_indices”

而是：

> “当前帧真实的单位 skinned draw contract，更可能躺在 `meshData` 的 CPU-side
> stream/binding 里，而不是 source/runtime geoset handle 本体里。”

这条结论仍然满足项目最终目标：

1. 不再依赖 DXVK 末端 `VB/IB snapshot/freeze`
2. 继续从上层语义边界确认“本轮要画谁”
3. 只是单位 skinned path 的几何消费来源，可能需要从：
   - `static CGeosetData`
   切到：
   - `meshData` 当前帧 CPU-side stream contract

## Recommended next step

下一轮不要再把主要精力放在 `runtimeGeosetData` recapture 上。

应改成：

1. 继续逆向并验证 `meshData + primary/stream1 + binding ctx`
2. 确认：
   - 哪个字段是真实 vertex count
   - 哪个字段是 per-vertex aux stream 的真实 stride
   - 这条 aux stream 是否就是单位 skinned draw 的 blend/remap contract
3. 如果确认 `meshData` 已经持有当前帧 post-skin 或可直接消费的 per-vertex
   contract：
   - 新建 `MeshDynamicContractCache`
   - 让单位 shadow path 走：
     - 上层对象 manifest
     - `meshData` 当前帧动态几何
     - 自有 shadow consumer
   - 依然不回到 DX9 draw-time snapshot/freeze

## Bottom line

这轮最重要的新增 ground truth 是：

1. `runtime geoset live recapture` 不是单位阴影缺失的最终解法
2. `runtimeGeoset` 只 clone 了 `CGeoset` 外壳，`CGeosetData` 仍然 alias source
3. 单位真正缺的 contract，极大概率已经在 `meshData` 当前帧 stream/binding 里

这意味着项目下一步应正式从：

- `geoset-side remap hunting`

转向：

- `meshData dynamic geometry / auxiliary stream contract`

而不是继续在同一层 source geoset 假设上反复消耗时间。
