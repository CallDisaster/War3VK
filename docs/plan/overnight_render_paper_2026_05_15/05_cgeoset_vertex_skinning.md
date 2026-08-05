# 第 5 章 ★★★ — CGeosetData 顶点格式与 CPU Skinning 流程

> 本章覆盖 War3 模型的"顶点数据 → CPU skinning → GPU 上传"完整链路。
> 它是第 4 章（Pose Palette）的下游：第 4 章讲矩阵怎么生成，本章讲矩阵怎么应用到顶点。
>
> 本章同时解释为什么项目的 draw-time VB capture（Phase 7.55 v4）能彻底解决
> shadow pose 卡顿——因为它直接消费了 CPU skinning 后的顶点，绕开了 palette 依赖。

## 0. 阅读与证据基线

### 0.1 写作前提

1. 版本基线：`Game.dll @ ImageBase 0x6F000000`（War3 1.27a）。
2. 反编译产物路径：`AutoTest/artifacts/_overnight_render_research/D_decomp_*.txt`。
3. 项目源码只读引用：`src/d3d9/d3d9_device.{h,cpp}`、`src/d3d9/war3/model/war3_model_hook.{h,cpp}`。
4. 历史数据来自 `AGENTS.md` 第 89~118 条（Phase 7.54 ~ 7.80）。

### 0.2 关键 RVA 锚点速查表

| RVA | 名字 | 角色 |
|---|---|---|
| `0x6F126250` | `CGeosetData_BuildFromRawArrays` | 从 raw positions/normals/uvs/indices 构造 CGeoset + CGeosetData |
| `0x6F1216A0` | `CGeosetData_Initialize` | 初始化 CGeosetData 内部字段 |
| `0x6F131150` | `CGeosetData_BuildPrefixSums` | 按 `matrix_group_sizes` 构建 prefix sum |
| `0x6F131210` | `CGeosetData_DedupGroupsToRuntime` | 去重 matrix groups 到 runtime 映射 |
| `0x6F1312F0` | `MatrixGroupRemap_AllocSlot` | 分配一个 matrix group remap slot |
| `0x6F132A10` | `MatrixGroupRemap_Lookup` | 查找 matrix group 的 runtime slot |
| `0x6F132700` | `MatrixGroupRemap_OverlapKey` | 判断两个 matrix group key 是否重叠 |
| `0x6F132790` | `MatrixGroupRemap_EqualKey` | 判断两个 matrix group key 是否相等 |
| `0x6F12E200` | `CMatrixGroup_BlendOutputMatrix` | 单 group 的矩阵混合核心（加权平均） |
| `0x6F12E600` | `CGeosetData_BuildGroupBlendedPalette` | 按 group 写 blended palette（第 4 章 Writer 2） |
| `0x6F12CFB0` | `CGeosetData_AppendVertexArray` | 追加顶点数组 |
| `0x6F12CF20` | `CGeosetData_AppendIndexArray` | 追加索引数组 |
| `0x6F12CEA0` | `CGeosetData_BindMaterialLayout` | 绑定材质布局 |
| `0x6F12C390` | `CGeosetData_AllocDefaultGroups` | 分配默认 matrix groups |

## 1. CGeosetData 数据结构

### 1.1 类层次

```
CGeoset (vtable @ 0x6FA59C50 附近)
  ├─ +0x00: vftable ptr
  ├─ +0x04: refCount
  ├─ +0x08: geosetIndex (-1 = unassigned)
  ├─ +0x0C ~ +0x14: 子对象指针
  └─ +0x18: CGeosetData* (核心数据)

CGeosetData (由 JassFrameAllocator 分配, tag="HGEOSETDATA")
  ├─ +0x00 ~ +0x3F: 基础字段（vftable, flags, counts）
  ├─ +0x40 (a1[16]): vertex count
  ├─ +0x44 (a1[17]): index count
  ├─ +0x48 (a1[18]): position array ptr (float3 per vertex)
  ├─ +0x4C (a1[19]): normal array ptr (float3 per vertex)
  ├─ +0x50 (a1[20]): UV array ptr (float2 per vertex)
  ├─ +0x54 (a1[21]): blend weight array ptr (float per vertex)
  ├─ +0x58 (a1[22]): blend index array ptr (uint32 per vertex)
  ├─ +0x5C (a1[23]): index array ptr (uint16 or uint32)
  ├─ ...
  ├─ +0x60 (a1[24]): vertex group indices ptr
  ├─ ...
  ├─ +0x9C (a1[39]): matrix group sizes array ptr
  ├─ +0xA0 (a1[40]): matrix indices array ptr
  ├─ ...
  ├─ +0xC8 (a1[50]): material layout count
  ├─ +0xCC (a1[51]): material layout array ptr
  ├─ ...
  ├─ +0xF0 (a1[60]): groupCount（★ 关键：决定 palette 写入粒度）
  ├─ +0xF4 (a1[61]): group sizes ptr（每 group 的 matrix 数量）
  ├─ ...
  ├─ +0x100 (a1[64]): group matrix indices ptr
  └─ +0x108 (a1[66]): geosetIndex（与 RenderablePart + 0x108 对应）
```

### 1.2 字段详解

| 偏移 | 类型 | 含义 |
|---|---|---|
| `+0x40` | u32 | 顶点数（positions/normals/UVs/blend 的元素数） |
| `+0x44` | u32 | 索引数（三角形 × 3） |
| `+0x48` | float* | position 数组（3 float/vertex = 12 bytes） |
| `+0x4C` | float* | normal 数组（3 float/vertex = 12 bytes） |
| `+0x50` | float* | UV 数组（2 float/vertex = 8 bytes） |
| `+0x54` | float* | blend weight 数组（1 float/vertex = 4 bytes） |
| `+0x58` | u32* | blend index 数组（1 u32/vertex = 4 bytes，指向 matrix index） |
| `+0x5C` | u16*/u32* | index 数组（三角形索引） |
| `+0x60` | u32* | vertex group indices（每个 vertex 属于哪个 group） |
| `+0x9C` | u32* | matrix group sizes（每 group 包含几个 matrix） |
| `+0xA0` | u32* | matrix indices（全局 matrix index 映射表） |
| `+0xC8` | u32 | material layout 数量 |
| `+0xCC` | void* | material layout 数组 |
| `+0xF0` | u32 | **groupCount**（★ palette 写入的矩阵数量） |
| `+0xF4` | u32* | 每 group 的 matrix 数量 |
| `+0x100` | u32* | group 的 matrix indices |
| `+0x108` | u32 | geosetIndex |

### 1.3 CGeoset 对象

`CGeoset` 是 `CGeosetData` 的外壳，由 `CGeosetData_BuildFromRawArrays (0x6F126250)` 创建：

```
CGeoset = JassFrameAllocator("HGEOSET")
  +0x00 = &CGeoset::vftable
  +0x04 = 0 (refCount)
  +0x08 = -1 (geosetIndex, unassigned)
  +0x18 = CGeosetData* (由 CGeosetData_Initialize 创建)
```

`CGeosetData` 本身也由 `JassFrameAllocator("HGEOSETDATA")` 分配，说明它和 CGeoset
共享同一个 frame allocator 的生命周期（地图加载时分配，卸载时释放）。

## 2. 顶点格式

### 2.1 War3 的顶点布局

War3 的模型顶点不是标准 D3D9 vertex declaration 里的固定格式。它在 CPU 端用
**分离数组（Structure of Arrays, SoA）** 存储：

```
positions[vertexCount * 3]   // float3: x, y, z
normals[vertexCount * 3]     // float3: nx, ny, nz
uvs[vertexCount * 2]         // float2: u, v
blendWeights[vertexCount]    // float: 权重（单 bone 权重，不是多 bone）
blendIndices[vertexCount]    // u32: 指向 matrix index 的索引
vertexGroupIndices[vertexCount] // u32: 每个 vertex 属于哪个 group
```

### 2.2 为什么是单权重？

War3 的模型每个顶点只有 **1 个 blend weight** 和 **1 个 blend index**。
这意味着每个顶点只受 **1 个骨骼** 影响（rigid skinning），不是现代引擎常见的
4-bone smooth skinning。

这是 War3 2002 年时代的优化：单权重 skinning 只需要一次矩阵变换，不需要加权平均。
但 War3 的 `CMatrixGroup_BlendOutputMatrix` 仍然做"混合"——它是在 **group 级别**
把多个 matrix 混合成一个 blended matrix，然后整个 group 的顶点都用这一个矩阵。

### 2.3 CPU skinning 后的顶点格式

CPU skinning 后，顶点被变换到世界空间。War3 的 CPU skinning kernel 对每个顶点做：

```c
// 伪代码（从 IDA 还原）
for (v = 0; v < vertexCount; v++) {
  int matrixIdx = blendIndices[v];
  Matrix3x4 boneMatrix = blendedPalette[groupForVertex[v]];
  float weight = blendWeights[v];

  // Rigid skinning: weight == 1.0, 所以直接变换
  outPositions[v*3+0] = dot(boneMatrix.row0, positions[v*3+0..2]) + boneMatrix.row0.w;
  outPositions[v*3+1] = dot(boneMatrix.row1, positions[v*3+0..2]) + boneMatrix.row1.w;
  outPositions[v*3+2] = dot(boneMatrix.row2, positions[v*3+0..2]) + boneMatrix.row2.w;
}
```

### 2.4 D3D9 上传格式

CPU skinning 后的顶点被写入 D3D9 Vertex Buffer（DynamicSysmemVBOs 或 regular VB）。
War3 以 **rigid 模式** 提交 D3D9 draw call：

```
D3DRS_VERTEXBLEND = D3DVBF_DISABLE  // 不用 D3D9 fixed-function vertex blending
```

**这就是 Phase 7.55 的决定性发现**：War3 不使用 D3D9 的 vertex blending 功能。
它在 CPU 端完成所有 skinning，GPU 只画已经变换好的刚体顶点。

## 3. Matrix Group Remap 系统

### 3.1 概念

War3 的模型可能有几十个骨骼（matrix），但每个 geoset 通常只用其中一部分。
`MatrixGroupRemap` 系统负责把"全局 matrix index"映射到"本 geoset 的局部 group index"。

### 3.2 数据流

```
CModelData::AddGeosetMaterialLayout (0x6F12A6A0)
  └─ CGeosetData_BuildPrefixSums (0x6F131150)
       ├─ 读 a2+240 (= groupCount)
       ├─ 读 a2+244 (= matrix_group_sizes[])
       ├─ 构建 prefixSum[i] = sum(sizes[0..i-1])
       └─ 调用 CGeosetData_DedupGroupsToRuntime (0x6F131210)

CGeosetData_DedupGroupsToRuntime (0x6F131210)
  ├─ 递归分治（类似归并排序）
  ├─ 对每个 group:
  │   ├─ MatrixGroupRemap_Lookup (0x6F132A10) → 查找已有 slot
  │   ├─ 若 miss: MatrixGroupRemap_AllocSlot (0x6F1312F0) → 分配新 slot
  │   └─ 记录 remap[group] = slot
  └─ 输出: runtime group → slot 映射表
```

### 3.3 MatrixGroupRemap 的内部结构

`MatrixGroupRemap_Lookup (0x6F132A10)` 使用 hash table：
- key = matrix group 的内容（一组 matrix indices）
- `OverlapKey (0x6F132700)`: 判断两个 key 是否有重叠 matrix
- `EqualKey (0x6F132790)`: 判断两个 key 是否完全相同
- 碰撞时用链表或开放寻址解决

`AllocSlot (0x6F1312F0)` 分配新 slot：
```c
int slot = this->nextSlot;
remapEntry->key = groupKey;
remapEntry->slot = slot;
remapEntry->count = matrixCount;
this->totalSlots += matrixCount;
return slot;
```

## 4. CPU Skinning 完整流程

### 4.1 每帧 timeline

```
Frame N (有 logic tick, dt > 0):
  1. CSpriteUber_PreRender_Full (0x6F182300)
     └─ dt gate: fabs(dt) >= 2*FLT_EPSILON → pass
     └─ CModel_EvalSingleGeosetAndRecurseChildren (0x6F12E900)
        ├─ [路径 A] groupCount == 0:
        │   └─ CModel_AllocAndFillSimpleFallbackPalette (0x12FF90)
        │       └─ 写 1 个 3x4 矩阵到 palette slot
        ├─ [路径 B] groupCount > 0:
        │   ├─ RenderQueue_ResizePaletteBuffer (0x12FE10)
        │   │   └─ 分配 groupCount * 48 bytes 在全局 palette arena
        │   ├─ CModel_AllocAndFillGroupPalette (0x12FED0)
        │   │   └─ CGeosetData_BuildGroupBlendedPalette (0x12E600)
        │   │       └─ 对每个 group: CMatrixGroup_BlendOutputMatrix
        │   │           └─ 加权平均该 group 的所有 bone 矩阵 → 1 个 3x4 矩阵
        │   └─ 写入 palette arena[slotIndex]
        └─ CModel_CopyPoseMatrixRangeFromStack (0x12FDC0)
            └─ 拷贝 pose stack 到 CModel + 0x60

  2. RenderQueue_Dispatch_Common (0x6F13A5E0)
     └─ RenderQueue_UpdateItemWorldMatrix (0x6F13A510)
         ├─ 读 RenderablePart + 0x08 (= palette slot index)
         ├─ paletteSlotAddress = Game.dll + 0xBC6BD0 + slotIndex * 48
         └─ CPU Skinning Kernel:
             for each vertex:
               matrix = palette[groupForVertex[v]]
               outPos = matrix * inPos + translation

  3. D3D9 DrawIndexedPrimitive
     └─ GPU 画已经 skin 好的顶点（rigid mode, no vertex blend）

Frame N+1 (no logic tick, dt == 0):
  1. CSpriteUber_PreRender_Full: dt gate → skip (所有 writer 不跑)
  2. GPU 用 Frame N 的 VB（顶点不变）
  3. 视觉连续（War3 logic tick ~30Hz，人眼不易察觉）
```

### 4.2 为什么主渲染流畅？

关键认知（Phase 7.54 决定性结论）：

1. War3 用 **CPU skinning**：骨骼变换在 logic tick 时完成，结果写入 VB
2. 两次 logic tick 之间，GPU 画的是同一份 VB（顶点不变）
3. palette 在两次 tick 之间不变（因为 `dt == 0` 时 writer 跳过）
4. 但主渲染**不受影响**，因为 VB 已经是 skin 后的结果
5. War3 的 logic tick 频率约 30Hz，人眼感知不到骨骼跳变

### 4.3 为什么 shadow caster 会卡？

项目早期（Phase 7.30 之前）的 shadow caster 使用 **GPU skinning**：
- vertex shader 从 palette SSBO 读矩阵做 blend
- palette 在 logic tick 之间不变 → shadow pose 冻结 7-8 帧
- 视觉表现：阴影"动 0.5s 停 0.5s"

### 4.4 draw-time VB capture 为什么解决？

Phase 7.55 v4 的 draw-time VB capture：
- 在 D3D9 `DrawIndexedPrimitive` 调用时，拷贝当前 VB 的 position stream
- 这份 VB 已经是 CPU skinning 后的结果（世界空间顶点）
- shadow caster 直接用这份 pre-skinned VB，`vertexBlendEnabled=false`
- 不依赖 palette，不受 palette cadence 影响
- 每帧都是 fresh 的（和主渲染同步）

## 5. RenderablePart 与 CGeosetData 的关系

### 5.1 RenderablePart 字段

| 偏移 | 类型 | 含义 |
|---|---|---|
| `+0x00` | vftable* | 虚函数表 |
| `+0x08` | u32 | **stagePresetSpanBaseIndex / palette slot index**（★ 关键） |
| `+0x10` | void* | meshData 指针 |
| `+0x108` | u32 | **geosetIndex**（对应 CGeosetData + 0x108） |
| `+0x10C` | u32 | payload word (用于 destructible state) |

### 5.2 从 RenderablePart 到 palette

```
RenderablePart + 0x08 = paletteSlotIndex
paletteSlotAddress = Game.dll + 0xBC6BD0 + paletteSlotIndex * 48
blendedMatrix = *(Matrix3x4*)paletteSlotAddress  // 48 bytes = 3x4 matrix
```

### 5.3 从 RenderablePart 到 CGeosetData

```
RenderablePart + 0x108 = geosetIndex
CModel.geosets[geosetIndex] → CGeoset → CGeosetData
CGeosetData + 0xF0 = groupCount
CGeosetData + 0xF4 = group sizes array
CGeosetData + 0x100 = group matrix indices
```

## 6. CMatrixGroup_BlendOutputMatrix 详解

### 6.1 函数签名

```c
void CMatrixGroup_BlendOutputMatrix(
    const Matrix3x4* poseStackBase,  // pose stack 上的矩阵数组
    const uint32_t* matrixIndices,    // 本 group 的 matrix indices
    uint32_t matrixCount,             // 本 group 的 matrix 数量
    Matrix3x4* outPalette             // 输出 blended matrix
);
```

### 6.2 算法

当 `matrixCount > 1` 时：
- 对每个 matrix index，从 `poseStackBase[index]` 读 3x4 矩阵
- 按权重加权平均（权重来自 pose stack 的附加数据）
- 输出 1 个 blended 3x4 矩阵

当 `matrixCount == 0` 时（简单 fallback）：
- 直接拷贝 `poseStackBase[0]`（identity 或 root bone）

### 6.3 Phase 7.47 实测数据

```
runtimeMatrixWrite(0x12E600): calls=13650 frames-with-hit=48 empty=0
runtimeGroupPaletteWrapper(0x12FED0): calls=5849 frames-with-hit=48 empty=0
runtimeSimpleGroupPalette(0x12FF90): calls=751 frames-with-hit=47 empty=1
```

每个 trace frame 都有 `mw+1 gpw+1` — producer 每帧都在写。

## 7. Alpha Test / Alpha Blend 处理

### 7.1 在 CGeosetData 中

alpha test 信息存储在 material layout（`+0xCC` / `+0xC8`）中。
每个 material layout 包含：
- texture stage 0 的 diffuse texture
- alpha test enable / alpha ref
- alpha blend enable
- blend mode (src/dst)

### 7.2 在 shadow caster 中

项目在 `War3TryCaptureShadowCaster` 的 v4 capture 路径读取 D3D9 state：
```cpp
entry.alphaTestEnabled = m_state.renderStates[D3DRS_ALPHATESTENABLE] != 0;
entry.alphaBlendEnabled = m_state.renderStates[D3DRS_ALPHABLENDENABLE] != 0;
entry.alphaRef = bit::cast<float>(m_state.renderStates[D3DRS_ALPHAREF]);
entry.diffuseTexture = m_state.textures[0];
```

Phase 7.52 的 alpha-blend promote 修复：
- 当 `alphaBlendEnabled && diffuseTexture && uvFormat valid` 时
- 自动 promote 为 alpha-test shadow（用 alphaRef=0.5 做 hard cutoff）
- 解决树叶/栅栏等半透明贴图的阴影"实心方块"问题

## 8. 项目 draw-time VB capture 实现（Phase 7.55 v4）

### 8.1 数据结构

```cpp
struct War3DrawTimeVBEntry {
  void* renderablePart;       // 用于 producer/consumer 匹配
  void* sceneNode;            // 用于 CSM cascade cull
  Rc<DxvkBuffer> positionBuffer;  // GPU copy 的 position stream
  DxvkResourceBufferInfo positionInfo;
  uint32_t positionStride;    // 原 VB 的 stride（可能 > 12 bytes）
  uint32_t positionOffset;    // position 在 stride 内的偏移
  uint32_t vertexCount;
  int32_t consumeVertexOffset; // IB rebase 修正
  Rc<DxvkBuffer> indexBuffer; // GPU copy 的 index buffer
  uint32_t indexCount;
  bool indexed;
  Matrix4 capturedWorldMatrix; // capture 时的 D3DTS_WORLD
  Rc<DxvkBuffer> uvBuffer;   // 用于 alpha-test
  bool alphaTestEnabled;
  bool alphaBlendEnabled;
  float alphaRef;
  Rc<DxvkImageView> diffuseTexture;
  uint64_t frameSerial;       // capture 帧号
  uint32_t rawcode;           // 对象 rawcode（用于 path blocker 过滤）
};
```

### 8.2 Capture 流程

```
War3TryCaptureShadowCaster (D3D9 DrawIndexedPrimitive hook)
  ├─ 检查 semantic.renderablePart != nullptr
  ├─ 检查 per-frame alloc budget (Phase 7.123, 默认 32/帧)
  ├─ 读 D3D9 state: VB[0], IB, stride, offset
  ├─ 计算 vertex range: [MinVertexIndex, MinVertexIndex+NumVertices)
  ├─ GPU copy: ctx->copyBuffer(positionRange → entry.positionBuffer)
  ├─ GPU copy: ctx->copyBuffer(indexRange → entry.indexBuffer)
  ├─ 读 alpha test/blend/texture state
  ├─ 保存 capturedWorldMatrix
  └─ 写入 m_war3DrawTimeVBCache[renderablePart]
```

### 8.3 Consume 流程

```
War3TryPopulateDrawTimeSemanticProducer (每帧 BeforeUi)
  ├─ 遍历 m_war3DrawTimeVBCache
  ├─ 对每个 fresh entry (frameSerial == current):
  │   ├─ 查 VisibleRenderableRegistry 确认可见
  │   ├─ 检查 path blocker (IsLosBlockerFourCc)
  │   ├─ 构建 War3ShadowCasterDraw:
  │   │   ├─ positionStorage = entry.positionBuffer
  │   │   ├─ positionStride = entry.positionStride
  │   │   ├─ indexStorage = entry.indexBuffer
  │   │   ├─ worldMatrix = entry.capturedWorldMatrix
  │   │   ├─ vertexBlendEnabled = false (★ 关键：不用 GPU skinning)
  │   │   ├─ alphaTest/texture 来自 entry
  │   │   └─ boundsRadius = 0 (no cull, v4 策略)
  │   └─ shadowCasters.emplace_back(draw)
  └─ 返回 submitted count
```

### 8.4 同帧去重（Phase 7.70）

同一帧内同一 `renderablePart` 可能被多次 draw（多 sub-draw）。
Phase 7.70 加了 fingerprint-based 去重：
- 第一次 capture: 完整 GPU copy + 记录 fingerprint
- 同帧后续: fingerprint 匹配 → 只刷新 alpha/texture 状态，不发 GPU copy
- 实测：~17% 的 capture 被去重（`drawTimeVBCacheSameFrameDedupHit ≈ 21/帧`）

## 9. IDA rename 清单（本章新增）

| 原名 | 新名 | 地址 |
|---|---|---|
| `sub_6F126250` | `CGeosetData_BuildFromRawArrays` | `0x6F126250` |
| `sub_6F1216A0` | `CGeosetData_Initialize` | `0x6F1216A0` |
| `sub_6F04F1C0` | `JassFrame_CloneToHeap` | `0x6F04F1C0` |
| `sub_6F131150` | `CGeosetData_BuildPrefixSums` | `0x6F131150` |
| `sub_6F131210` | `CGeosetData_DedupGroupsToRuntime` | `0x6F131210` |
| `sub_6F1312F0` | `MatrixGroupRemap_AllocSlot` | `0x6F1312F0` |
| `sub_6F132A10` | `MatrixGroupRemap_Lookup` | `0x6F132A10` |
| `sub_6F132700` | `MatrixGroupRemap_OverlapKey` | `0x6F132700` |
| `sub_6F132790` | `MatrixGroupRemap_EqualKey` | `0x6F132790` |
| `sub_6F12E200` | `CMatrixGroup_BlendOutputMatrix` | `0x6F12E200` |
| `sub_6F12CFB0` | `CGeosetData_AppendVertexArray` | `0x6F12CFB0` |
| `sub_6F12CF20` | `CGeosetData_AppendIndexArray` | `0x6F12CF20` |
| `sub_6F12CEA0` | `CGeosetData_BindMaterialLayout` | `0x6F12CEA0` |
| `sub_6F12C390` | `CGeosetData_AllocDefaultGroups` | `0x6F12C390` |
| `sub_6F12D710` | `CGeosetData_FinalizeVertexGroups` | `0x6F12D710` |

## 10. 关键发现总结

### 10.1 War3 的 CPU Skinning 是根本事实

War3 1.27a 不使用 D3D9 fixed-function vertex blending（`D3DRS_VERTEXBLEND == D3DVBF_DISABLE`）。
所有骨骼变换在 CPU 端完成，结果写入 VB，GPU 以 rigid 模式画。

### 10.2 Palette 只是 CPU Skinning 的中间数据

`CGeosetData_BuildGroupBlendedPalette` 产生的 group-blended palette 是 CPU skinning kernel
的输入。它不是 GPU shader 的 uniform/SSBO。

### 10.3 draw-time VB capture 是正确的 shadow pose 修复方向

Phase 7.55 v4 的 draw-time VB capture 直接消费 CPU skinning 后的 VB，
彻底绕开 palette 依赖。这是唯一不依赖 producer cadence 的方案。

### 10.4 单权重 skinning 简化了实现

每个顶点只受 1 个骨骼影响 → 不需要 multi-bone blend → CPU skinning kernel 极简。
这也是为什么 War3 2002 年的硬件能流畅运行。

---

*本章约 500 行。下一章：第 6 章 FogMask 静态阴影治理（已完成，见 `06_fogmask_static_shadow.md`）。*
