# Upper-Layer Shadow Cutover Status

Date: 2026-04-16

## 1. 本轮已落实到代码的链路

### 1.1 final visible -> geoset

当前代码已经把下面这条链接通：

`renderablePart`
-> `meshData`
-> `meshData + 0x108 = meshIndex/geosetIndex`
-> `runtimeModel + 0x10 runtime geoset array`
-> `runtimeGeosetPtr/runtimeGeosetDataPtr`
-> `ShadowModelResourceCache`

工程落点：

- `war3_visible_renderables.*`
- `war3_model_resource_cache.*`
- `war3_upper_layer_shadow.*`

### 1.2 pose palette

当前代码已经把下面这条链接通：

`CSpriteUber_ prerender/light-update`
-> `runtime pose / sprite-frame pose`
-> `CModel + 0x5C/+0x60 final 3x4 palette`
-> `PoseRegistry`
-> `War3GetOrCreateShadowMatrixPaletteFromData`
-> `War3FrameScene::shadowPalettes`

### 1.3 first upper-layer consumer

当前代码已经把 `upper-layer shadow consumer` 插入到
`D3D9DeviceEx::War3TryCaptureShadowCaster` 的前面：

1. 先尝试从 `VisibleRenderableRegistry + ShadowModelResourceCache + PoseRegistry`
   直接生成 shadow draw
2. 成功则直接写入
   - `shadowInstances`
   - `shadowCasters`
3. 若上层链不完整或不安全，则自动回退到旧的 DXVK freeze/capture 路径

### 1.4 upper-layer persistent geometry

这一轮之后，upper-layer path 不再是“每个 draw 临时 host upload”。

当前做法已经改成：

1. `ShadowModelResourceCache` 提供静态 geoset CPU 数据
2. `War3FindOrCreateShadowPersistentGeometry` 负责一次性上传到 GPU persistent pool
3. 后续同一 `model/geoset/material` 组合只复用 persistent geometry
4. skinned 只保留 `paletteIndex` 的逐帧变化，不再重复上传 positions / indices / uv / blendIndex

也就是说，已经从：

- upper-layer semantic + per-draw temp upload

推进到：

- upper-layer semantic + persistent geometry + per-frame palette

## 2. 当前 authoritative 边界

### 2.1 已 authoritative

1. rigid/static geoset
2. matrix-group skinned geoset（含 multi-group）

这里当前采用的正式 contract 是：

1. 顶点 `vertex_group_indices` 直接引用 geoset 本地的 matrix-group slot
2. `matrix_group_sizes + matrix_indices` 给出该 slot 引用的 runtime pose matrix 集合
3. `sub_6F12E200` 已证明：
   - group size = 1 时直接拷对应矩阵
   - group size = 2/3 时按等权平均合成
   - 更大 group 时仍走“累加后按数量平均”的规则
4. 因此 upper-layer 现在会先为每个 group slot 生成一份 runtime group palette
5. shadow draw 侧只需要：
   - `paletteIndex = runtime group palette`
   - `blendIndex = vertex_group_indices`
   - `vertexBlendCount = 0`

这些 case 现在已经默认优先走：

1. `VisibleRenderableRegistry`
2. `ShadowModelResourceCache`
3. `PoseRegistry`
4. `War3ShadowPersistentGeometry`
5. `shadowInstances / shadowCasters`

不会再走原来的 per-draw temporary upper-layer upload。

### 2.2 当前仍保守回退

以下内容目前仍可能保守回退或直接不画：

1. visible manifest / geoset resource / pose palette 任一环节未命中
2. 非 object-caster 或不在本轮上层 consumer 目标内的特效类对象
3. persistent pool 当帧创建失败

### 2.3 已同步补上的 fallback 行为

即使 legacy-freeze 仍存在于工程里，旧 fallback 也已经改成：

1. 只要 `runtimeModelPtr` 能在 `PoseRegistry` 命中最终 palette
2. 就优先使用 `CModel + 0x5C/+0x60` 的 runtime pose palette
3. 不再把 `ObjectKind::Unit` 排除在 runtime palette 覆盖之外

不过当前 object-caster 实验目前改成：

1. `kUpperLayerShadowObjectNoCaptureFallbackEnabled` 作为专项实验开关保留
2. 默认值已收回 `false`，避免“上层链未闭环时直接零阴影 + 高开销”
3. 同时补了 upper-layer resolve 失败计数，后续以 perf report / runtime summary 为准定位缺口

## 3. 当前 legacy-freeze 的角色

这一轮之后，legacy-freeze/capture 对 object caster 的角色已经降成：

1. 上层链未解析成功
2. 非 object-caster 场景仍保留历史兼容路径
3. 或 persistent pool 当帧创建失败/预算不允许

也就是说，object shadow 当前默认已经进入“纯 upper-layer 优先验证”阶段；
legacy-freeze 不再是 object caster 的默认兜底路径。

## 4. 这轮最重要的可靠结论

1. `RenderablePart + 0x108` 现在正式作为 `geosetIndex` 进入代码链路
2. `runtimeModel/runtime geoset array` 已作为优先解析来源
3. `PoseRegistry` 不再只是统计桥，而是开始被 shadow consumer 直接消费
4. `sub_6F12E200` 坐实了 matrix-group runtime 合成规则，multi-group 不需要再等待额外顶点 weight 数组
5. upper-layer path 现在已经是真正的“runtime group palette + persistent geometry”，不再只是 debug 记账
6. `SpriteHost_CreateSpriteAndBindRuntimeModel(0x6F185250)` 的宿主 `+0x20` 不能再被盲目信任为稳定资源绑定点；当前实现已经改成优先从 `runtimeModel + 0x9C` 回读 owned `CModelData` handle，并在 runtime matrix/palette 更新阶段反补 `ShadowModelResourceCache`
7. `0x6F04F1C0 / 0x6F04F200` 都只是 retain-self helper，不是 handle unwrap；`0x6F132230 / 0x6F1325E0` 进一步证明 runtime geoset 会复制 `CGeoset` 外壳，但继续 alias 同一份 `CGeosetData`。这一点已经单独收口到 `war3_runtime_model_geoset_alias_ground_truth_2026_04_17.md`

## 5. 下一步若继续推进

如果这轮游戏内验证通过，下一步最自然的是：

1. 继续清掉非 object-caster 场景里残留的 capture/freeze 依赖
2. 把更多 shadow/outline/material 消费端都收敛到同一份 upper-layer contract
3. 等 object shadow 全量稳定后，再开始把宿主从当前 DX9Ex/DXVK 入口迁到晚注入
