# Footman Model Signature (2026-04-19)

## Purpose

把 `C:\Users\Administrator\Desktop\Footman.txt` 里对当前 semantic-shadow
主线最有价值的 classic skinning 签名固化下来，作为后续 runtime 对照尺。

## Model-level facts

1. `Model "Footman"`
2. `NumGeosets = 5`
3. `NumBones = 25`
4. `NumHelpers = 15`

## Geoset signatures

### Geoset 0

1. `vertexCount = 447`
2. `groupCount = 35`
3. `matrixIndices = 54`
4. `matrixGroupSizes` 对应 35 组，包含单骨、双骨、三骨 group
5. `VertexGroup[0..15] = [11, 12, 11, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34]`
6. `VertexGroup[0..3]` 按 little-endian 打包：
   - bytes = `[0x0B, 0x0C, 0x0B, 0x22]`
   - dword = `0x220B0C0B`

这与旧 runtime 样本里：

- `geoIdx=0`
- `vertexGroups=447`
- `groupCount=35`
- `matrixIndices=54`
- `sample1=0x220B0C0B`

形成了直接一一对应。

### Geoset 1

1. `vertexCount = 52`
2. `groupCount = 9`

### Geoset 2

1. `vertexCount = 26`
2. `groupCount = 4`
3. `Matrices = {9}, {10}, {11}, {12}`

### Geoset 3

1. `vertexCount = 6`
2. `groupCount = 1`
3. `Matrices = {20}`

### Geoset 4

1. `vertexCount = 51`
2. `groupCount = 1`
3. `Matrices = {0}`

## Why this matters

这份样本已经证明：

1. 至少对 `Footman` 主 body geoset 而言，`meshStream1` 可以直接承载
   `VertexGroup` 的 packed slot 数据；
2. 当前主 blocker 不再是“有没有顶点 group slot”，而是：
   - 当前 draw 的 group slot 到底该吃哪份 runtime pose/group palette；
3. 如果 live unit path 仍出现：
   - `maxSlot=34`
   - 但 `poseCount=1~2`
   这更像是 body geoset 被错误绑定到了 child runtime / part runtime
   的小姿态记录，而不是 `VertexGroup` 本身还没解开。

## Immediate engineering implication

后续与 `Footman` 相关的 live probe，优先检查：

1. `renderable.runtimeModelPtr`
2. `sceneNode` 绑定到的 pose matrix count
3. `meshPoseCtx` 对应的 runtime model
4. 最终被 `ShadowRendererCore` 选中的 pose 是否仍然只是 child runtime
   的小 palette
