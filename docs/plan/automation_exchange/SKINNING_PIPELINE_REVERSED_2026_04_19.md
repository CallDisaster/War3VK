# War3 Skinning Pipeline Reversed (2026-04-19)

## 摘要 (Executive Summary)

巨大的突破！我们彻底查明了 War3 的**蒙皮矩阵连线图（Remap/Span/Weights）**是如何运作的。
此前最大的 Blocker 是：“在拿到 pose 和 geoset 之后，如何知道顶点吃哪几个骨骼槽位，以及它们的蒙皮权重是多少？”

我们的长期盲区在于试图寻找一张“绝对的静态映射表”（比如 `l1cHop` 内联结构）。
**事实真相是：War3 在真正的 Draw Call 分发之前，在 CPU 端动态构造了一张“已经混合好的专用 Matrix Palette”！**
并且，**War3 根本没有自定义的浮点数蒙皮权重（Blend Weights）**！所有多骨骼影响都是绝对的**平均分配**（2根骨头就是 1/2，3根骨头就是 1/3）。

## 核心证据链 (Call Chain)

我们在 `d3d9.dll` 拦截到的所谓单矩阵或调色板，实际上是以下流水线在 CPU 端临时运算的产物：

### 1. 结构与格式对应 (MDL to CGeosetData)
参照 `Footman.mdl`：
```text
Groups 35 54 {
    Matrices { 1 },
    Matrices { 2, 3 },
    Matrices { 2, 3, 13 },
...
```
在运行时，这完美对应着 `CGeosetData` 的偏移量：
- `+0xF0` : `MatrixGroupCount` (= 35)
- `+0xF4` : `MatrixGroupSizes` (uint32 数组, [1, 2, 3...])
- `+0x100`: `MatrixIndices` (uint32 数组, [1, 2, 3, 2, 3, 13...])

### 2. 完整的自顶向下追踪链 (The Complete Top-Down Call Chain)

经过进一步的汇编追踪，我们拿到了自最上层单位绘制（`CSpriteUber`）直达调色板生成的**无懈可击的具体证据链**：

1. **`CSprite` 取出运行时模型**:  
   - 在上层绘制准备函数 `CSpriteUber__PreRenderAndUpdatePosePalette` (`0x6F182530`) 中。
   - `CSpriteUber + 0x20` 存放了指向其内部 `CModel*`（运行时模型实例）的指针，这被直接压入 `ecx`。
   - 然后调用 `CModel_SetWorldMatrixAndDispatchPose` (`0x6F12F0A0`) 进入模型调度入口。

2. **分配全局骨骼变换与准备分发**:  
   - `CModel_SetWorldMatrixAndDispatchPose` 若判断有复杂节点，会走进 `CModel_PrepareRenderPalettesAndDispatch` (`0x6F12E900`)。
   - 这时该实例所有的绝对动画骨骼运算已完成，结果存在一个**全局连续缓冲池 (Global Palette Pool)** 里面。
   - `PalettePool_GetSlotAddress` (`0x6F12E870`) 函数执行简单的 `PoolBase + 48 * poseIdx`，获取当前模型在全局缓存池里的基地址。

3. **遍历渲染组，分配专用槽位**:  
   - 上述步骤得到的全局骨骼池基地址（放在 `edx` 寄存器里），以及当前 `CModel*`（在 `ecx` 中）被一同传给 `CModel_AllocAndFillGroupPalette` (`0x6F12FED0`)。
   - 这个函数遍历该 CModel 下已生成可渲染的每一个渲染批次元素（Batch Item）。
   - **铁证**：它从 `batch_item + 0x0C` 拿到了 `CGeosetData*`（即引擎中的网格元数据）。
   - 接着向 `RenderQueue` 申请一段显存友好的调色板卡槽（PaletteSlot），写入到 `batch_item + 0x08`。

4. **调配执行 (The Bridge)**:  
   - 在拿到了 `GeosetData*`、专属分配的输出卡槽地址 (`eax`)、以及上游全局骨骼池基地址 (`edx`) 后。
   - 调用了最终的核心混合函数：**`CGeosetData_BuildGroupBlendedPalette` (`0x6F12E600`)**。

### 3. CPU 矩阵混合核心 (The CPU Matrix Blender)

`CGeosetData_BuildGroupBlendedPalette` 汇编级别的行为完全符合 MDL 定义：
它遍历 `MatrixGroupCount`，内部每次读取 `MatrixGroupSizes[i]` 根骨骼的索引，然后调用：
**`CMatrixGroup_BlendOutputMatrix` (0x6F12E200)**。

在 0x6F12E200 中，暴雪硬编码了如下分支：
- **情况 1 (单骨骼)**：直接拷贝 48 字节 (3x4 Matrix)。
- **情况 2 (双骨骼)**：执行 SSE 指令 `_mm_add_ps`，然后乘以 `0.5`（`xmmword_6F961850`）。
- **情况 3 (三骨骼)**：执行 SSE 指令连加三次，然后乘以 `0.33333331`（`xmmword_6F961840`）。
- **默认 (N骨骼)**：循环累加，最后乘以 `1.0 / N`。

产生的新混合矩阵，被**挨个线性写入**到当前对象的专用调色板中。

### 4. 渲染提交 (Dispatch)

到了真正画图的时候：`RenderQueue_Dispatch_Common` (0x6F13A5E0)。
`Stream1 (VertexGroups)` 的值，实际上是**这批动态调色板的绝对索引（[0, 1, 2... 34]）**！
游戏引擎调用虚函数 `gx_device->vf[22]` (也就是 `SetTransform` 的多矩阵变种)，直接把混合好的 `0~34` 矩阵数组整块传给 DX9。DX9 端只要执行索引提取（`INDEXEDVERTEXBLEND=ENABLE`）即可。

## 下一步行动指南 (Action Items for Codex)

我们彻底走出了误区，无需再死磕 `l1cHop` 内联表里莫须有的 Byte mapping 了！

1. **废弃旧合同猜想**：
   无需再尝试为 `VertexGroup` 反推 `remap-span` 或解包所谓隐含的权重。
2. **拥抱 CGeosetData 直接源**：
   我们**早已拥有** `MatrixGroupSizes` (+0xF4) 和 `MatrixIndices` (+0x100)！
   我们手里也有全局骨架的 Pose！
   我们在 Semantic Renderer 里完全可以**自己跑一遍 0x6F12E600 逻辑**：
   将 `Stream1` 中的 index 作为骨骼索引，利用 `Sizes/Indices` 读取 Pose 数组做平均相加，直接生成供 GPU 使用的调色板。
3. **甚至直接交给 GPU (Ultimate GPU Skinning)**：
   由于知道了规律只是 `Sizes + Indices + 均权`，我们甚至可以把 Sizes 和 Indices 当作 SSBO 或纹理推给 Shader，在 Vertex Shader 里做最终相加！或者，继续在 CPU 端快速算好（开销其实不大），作为 `packet payload` 给 GPU 更新。

**终极谜团已解，Semantic Shadow Skinned Path 畅通无阻！**
