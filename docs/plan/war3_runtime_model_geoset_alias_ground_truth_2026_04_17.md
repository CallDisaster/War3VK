# War3 Runtime Model / Geoset Alias Ground Truth

Date: 2026-04-17

## Scope

这一页只收敛本轮为了继续推进 `upper-layer shadow` 而重新在 IDA 里核过的
`CModelData / CModel / CGeoset` 事实，避免后面又回头把 `handle`、`runtime geoset`
和 `source geoset` 的关系搞混。

## RTTI / real class names

本轮涉及的真实类名仍然是：

- `CModelData`
- `CModel`
- `CGeoset`
- `CGeosetData`

这里没有再引入新的暴雪类名猜测。

## IDA-confirmed functions

### `0x6F04F1C0`

语义建议名：

`RetainSelfAndReturn`

IDA 伪代码：

```cpp
_DWORD* __thiscall sub_6F04F1C0(_DWORD* this) {
  if (this) {
    ++this[1];
    return this;
  }
  Storm_SetLastError(87);
  return 0;
}
```

结论：

1. 这不是 “unwrap handle -> object”。
2. 它只是对当前对象本体做 `refCount++` 后原样返回。

### `0x6F04F200`

语义建议名：

`RetainViaIdentityThunk`

IDA 伪代码表明：

1. 先调用 `0x6F04F1E0`。
2. `0x6F04F1E0` 只是 `return this`。
3. 然后再 `refCount++` 并返回同一个指针。

结论：

1. `0x6F04F200` 也不是 handle unwrap。
2. 当前这条链上的 `HMODELDATA / HGEOSET / HMODEL` 在我们关心的读字段语义上，
   仍然可以把“handle object 本体”当成返回值本体处理。

## Runtime model promotion chain

### `0x6F12A5C0`

语义建议名：

`CModelData_PromoteToRuntimeModel`

关键事实：

1. 如果 `CModelData + 0x94` 带复杂树标记，则走 `CModelComplex_` 分支。
2. 否则分配 `HMODEL`，构造 `CModel`，再调用 `0x6F130CD0`。
3. 最后对新建 runtime model 调 `0x6F04F1C0` 返回。

### `0x6F130CD0`

语义建议名：

`CModel_CopyResourceArraysAndHandlesFromSource`

关键事实：

1. `this + 0x98` retain `sourceModelData + 0x98`。
2. `this + 0x9C` retain `sourceModelData + 0x9C`。
3. `0x6F132230 / 0x6F1323A0 / 0x6F132420` 会把 source 侧 geoset/material/extra handle
   拷到 runtime-owned `CModelData`。

## Runtime geoset alias chain

### `0x6F132230`

语义建议名：

`CModelData_CloneRuntimeGeosetsFromSource`

关键事实：

1. 读取 `sourceModelData + 0x0C/+0x10` 的 geoset array。
2. 对每个 source geoset 调 `0x6F1325E0`。
3. 结果写进 runtime-owned `CModelData + 0x08/+0x10`。

### `0x6F1325E0`

语义建议名：

`CGeoset_CloneHandleAndAliasSourceGeosetData`

IDA 伪代码显示：

1. 分配新的 `HGEOSET`。
2. 新对象 vtable 是 `CGeoset::vftable`。
3. `newGeoset + 0x0C = sub_6F04F200(sourceGeoset + 0x0C)`。
4. `newGeoset + 0x10 = sourceGeoset + 0x10`。

结论：

1. runtime geoset pointer 与 source geoset pointer 不同。
2. 但 `runtimeGeoset + 0x0C` retain 的仍是 source geoset data，同一份
   `CGeosetData`。
3. 所以：
   - 不能要求 `runtimeGeosetPtr == sourceGeosetPtr`
   - 但可以安全依赖 `runtimeGeosetDataPtr == sourceGeosetDataPtr`

## 工程含义

### 1. `modelResourcePtr` 不是唯一可信主键

即使 `CModel + 0x9C` 是 retained owned `HMODELDATA`，上层阴影链也不能只靠
`modelResourcePtr + geosetIndex`。

原因：

1. visible manifest 可能先拿到 `meshIndex/geosetIndex`，但没及时拿到稳定的
   `modelResourcePtr`。
2. draw-time 最稳定的真实边界其实是：
   - `runtimeModel`
   - `runtime geoset array`
   - `runtimeGeoset + 0x0C = shared/source CGeosetData`

### 2. 上层 consumer 应优先接受两种命中方式

推荐优先级：

1. `runtimeGeosetPtr -> cache`
2. `runtimeGeosetDataPtr -> cache`
3. `runtimeModelPtr + geosetIndex -> runtime geoset alias`
4. `modelResourcePtr + geosetIndex -> model resource record`

### 3. 本轮代码落地结论

为了匹配上面这条事实链，当前工程已经补了：

1. `ShadowModelResourceCache::noteRuntimeModelBinding(...)`
2. `ShadowModelResourceCache::findRuntimeModelGeoset(...)`
3. `runtimeModel -> runtime geoset alias/model record` 的 sidecar 缓存
4. upper-layer resolve 里用 `semantic.runtimeModelPtr` 反补 geoset 的 fallback

## 对“完全脱离 VB/IB capture”的意义

这条逆向结论解决的不是最终 skinning 算法，而是更前面的一个硬前置：

我们终于可以不再把“拿不到稳定 `modelResourcePtr`”误判成“上层没有足够几何数据”。

只要：

1. `VisibleRenderableRegistry` 给出本轮 renderable 的 `meshIndex`
2. `runtimeModel` 能恢复
3. `PoseRegistry` 已有最终 palette

渲染层就可以从 runtime geoset alias 和共享 `CGeosetData` 直接查到静态几何，
不必再回到 `VB/IB snapshot`。
