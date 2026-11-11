# W1-A1：Footman 原生数据到共享核心的映射方案

状态：2026-09-30 **方案，未实现采集，未进行新的游戏取证**。本文件不改变渲染路径、产品默认值或发布状态。总回执见 [W1 准备](../SHARED_RENDER_CORE_W1_PREP_2026-09-30.md)，序列化合同见 [A2](2026-09-30-shared-core-native-frame-export-contract.md)，宿主边界见 [A3](2026-09-30-shared-core-warvk-host-adapter.md)。

## 1. 样本与本轮出口

首个资产是 `Units\\Human\\Footman\\Footman.mdx`，不是 Portrait、地图导入同名模型或仅以 `hfoo` 推断的资产。FORGE 既有 W1 回执记录的模型 SHA-256 为：

`7EE255776A6E757BD89766354DF763201CD0519C488585B7C30151D5E313FC13`

依据：[FORGE 三帧回执](E:/Mycode/Source/Repos/War3MapEditor64/artifacts/astra-rounds/step3/w1-footman-20260929-020716/report.json)。本次只读解析确认：两实例分别 Stand（sequenceIndex=0，500 ms）和 Walk（sequenceIndex=6，250→283 ms）；第三张重复 283 ms；回执明确为 `PreviewApproximate`，含通用旋转/相机节点诊断。该摘要是 **FORGE 样本声明**，不是本轮重新读取游戏挂载源的证明，也不是原生姿态黄金真值。

W1 要建立三项独立关系：

1. **资产对应**：两个宿主确实使用相同的 MDX 字节及同一原 geoset 定义。
2. **数据对应**：当前实例、部件、组号及实际矩阵范围属于同一次原生消费。
3. **数值对应**：在已证明的顺序、空间与语义下比较矩阵、alpha/RGB、位置和法线。

不能用“模型看着像”“顶点数一样”“最后一次 palette 非空”替代任何一项。本轮交付的是建立这些关系的方案，不是关系已经闭合的结论。

## 2. 当前能取得什么

以下是当前源码事实；“可取得”不代表所有运行时样本都完整。

| 来源 | 当前字段/入口 | 可以证明的事实 | 不能直接证明的事实 |
| --- | --- | --- | --- |
| [ModelInstanceRecord](../../src/d3d9/war3/model/war3_model_registry.h)，34–55 | worldObjectEntry、sceneNode、unitPtr、spritePtr、runtimeModelPtr、modelResourcePtr、jHandle、rawcode、modelKey、first/lastSeenFrame、lastSceneCollectorBatchFrame | 插件观察到的实例关联与各观察帧 | 指针永久唯一、MDX 内容身份、原生对象完整生命周期；lastSeenFrame 不是消费同帧见证 |
| 同文件 `ModelResourceRecord` | modelPath、modelResourcePtr、modelKey | 路径及运行时资源关联候选 | 游戏覆盖层解析结果与源 MDX SHA-256；modelKey 不能取代内容摘要 |
| [ShadowGeosetResourceRecord](../../src/d3d9/war3/model/war3_model_resource_cache.h)，39 起 | positions、normals、indices、vertexGroupIndices、matrixGroupSizes、matrixIndices、各 UV 层、primitiveRecords；geosetIndex、layout/material 槽；mapEpoch、immutableModelGeneration、immutableCaptureStatus | 已拥有的不可变几何载荷、缓存签发的内容代际与地图域 | geosetIndex 等于 MDX GEOS 原序号、runtime matrixIndices 等于 MDX objectId、材质槽的未证明语义 |
| [PoseRecord](../../src/d3d9/war3/model/war3_model_registry.h)，162 起 | worldTransform、spriteFrameTransform、matrixPalette、matrixHash、独立的 root/sprite/palette 帧 | 观察到的变换/矩阵及其各自时点 | 所有字段来自同一个 draw；sequenceId/sequenceTime 是准确动作序号/当前播放时钟 |
| [Hook_RuntimeMatrixWrite](../../src/d3d9/war3/model/war3_model_hook.cpp)，7921 起；原生 RVA `0x12E600` | 原函数返回后的连续 `count×48B` 组矩阵，输出范围、native frameTag；缓存写者维护 writeSerial | 某次原生组混合写出的数值；writeSerial 是插件缓存写入序列 | 参数名 nodePtr 实为 CGeosetData，不能单独归属实例；同帧标签不能证明槽位归属 |
| 同文件 `Hook_RuntimeMatrixRangeCopy`，8289 起；RVA `0x12FDC0` | runtimeModel、调用参数中的精确源段，CModel+0x5C 数量；源偏移 `(context+0x54)-(context+0x6C)`，48B 步长 | 某次 resolved pose 范围复制的来源与数值 | 该范围就是某个 geoset 的最终混合组数组；延后读 CModel+0x60 未被其他 flush 覆盖 |
| [skin::Selection](../../src/d3d9/war3/render/war3_skin_palette_selection.h) | source、space、domain、runtimeModel/part/meshPayload、ownerEpoch、publicationTicket、captureSerial、hash、slot、actualGroupCount、frameTag | 当前 CPU 捕获/发布合同中实际声明的来源、域与空间 | publicationTicket 是原生 allocator lease；slotAllocationGeneration=0 不是所有权证明 |
| [War3DrawTimeVBEntry](../../src/d3d9/d3d9_device.h)，2659 起 | exact owner/submit frame、part/layer、实例关联、实际 VB/IB 范围与 lease、capturedWorldMatrix、alphaTest/Blend/Ref、diffuseTexture | 原生 draw-time 几何及 D3D9 状态的最终捕获候选 | alphaRef 是 GEOA alpha；diffuseTexture 是 RGB；buffer 引用自动保活重定位前物理存储 |

两个重要的现状限制：

- [模型 Hook](../../src/d3d9/war3/model/war3_model_hook.cpp)：5983–6018 读取的是动画时间 **override**（未开启则 0），7412/7429 的 sprite-frame 更新传入 sequenceId=0。因此不能把 PoseRecord 直接作为可靠动作时钟。
- 当前 writer 捕获有 20000 次/帧工作上限，连续矩阵读取也有数量上限。上限命中、截断、零/未知 frameTag 必须体现在样本缺口中；不得把缓存命中升级成“此次原生写入完整”。

## 3. MDX geoset 与不可变 runtime geoset

### 3.1 当前序号的含义

[资源缓存](../../src/d3d9/war3/model/war3_model_resource_cache.cpp) 的 1451、1580 将 **资源/运行时数组遍历序号 i** 写入 geosetIndex。这能定位数组槽，不能证明它就是原文件 GEOS 第 i 项。`mergedGeosetSlotOrBindingIndex` 也不是已证实的 MDX 原序号。

建立一个资产级映射表，键为：

`(MDX SHA-256, sourceLayer, mapEpoch, immutableModelGeneration)`

每条映射同时保留 `mdxGeosetOrdinal`、`runtimeGeosetOrdinal`、`partIdentity`、`layerOrdinal` 及映射证据。层不是 geoset：同一网格多个材质层不得变成多个几何资产。MDX 原序号即使在某帧不可见也必须保留，不能按当前可见列表重新编号。

### 3.2 建立映射的顺序

1. 用 FORGE/共享上游的唯一 MDX 解析器读取作者本机资产；WarVK 不新增第二套 MDX 解析实现，不复制 FORGE 代码。核源字节摘要与挂载来源，未知来源只作候选。
2. 导出原 geoset 的结构清单：顶点/索引/组数量、primitive 域、各 UV 层数量、材质索引；为绑定姿态位置、法线、索引、UV、顶点组和有序组成员分别计算定义明确的 SHA-256。
3. 与 WarVK 持有的不可变 snapshot 比较 **完整对应流**，不是只比数量或现有 FNV contentHash。原生浮点布局和解析器规范化步骤分别登记；摘要不等时输出首个不同流，不自动删字段“对齐”。
4. 若原生只重排组号，证明 §4 的重映射后，比较每个顶点实际引用的有序组成员。不得把组编号改写隐藏在几何摘要里。
5. 对 Footman 的首批样本只接受明确的一对一关系。多个候选匹配、合并网格、裁剪/重排顶点或不支持的 primitive，标 `Ambiguous/Unsupported`；不靠最近对象、rawcode 或画面形状补全。

**一对一不意味着所有游戏模型都一对一。** 原生支持组查重/merged geoset；以后支持 N:M 必须另有显式范围表。此处失败只阻止新共享样本获得认证，不能改动当前游戏绘制。

映射是加载/资产变化时的工作；不允许每个 draw 重新 hash 整模型。缓存复用必须再次核 active map、资源代际和实例绑定，旧地图的相同指针不能复用映射许可。

## 4. 顶点组号与 palette 范围

### 4.1 三个不同的编号域

```text
MDX matrixIndices：节点 objectId 及有序组成员
  ↓ 原生/共享加载映射（必须具名记录）
runtime/merged group slot：顶点 uint8 组号的目标
  ↓ 当前实际 palette 范围及该次消费绑定
palette[startSlot + runtimeGroupSlot]：48B 的最终混合矩阵
```

保存的逆向证据：[CModel/remap 专题](../research/war3_render_issues/22_cmodel_pose_palette_reverse/README.md)，117–187。`0x6F131150` 构建组成员前缀；`0x6F131210/0x6F132A10` 对**有序**成员序列查重；`0x6F1312F0` 分配组槽，并写 byte remap。因此原文件 objectId、原 geoset 组号和 runtime palette 槽不能直接等同。重复组可以合并，映射可能多对一；不得强求组映射也一对一。

### 4.2 可用组见证必须同时满足

- 绑定同一实例、runtimeModel、part、geoset immutable generation 和当前目标消费；part 不是唯一实例证明。
- 原 geoset 组成员顺序及 remap 表已知；禁止给成员排序后比较（浮点平均顺序可能改变）。
- 每个 vertexGroup 在本 geoset 组范围内；转换后的 runtimeGroup 在该次实际 palette 范围内。
- `startSlot + requiredCount` 用溢出安全检查；原始数量、实际复制数量、所需数量分列，截断不得认证。
- 保留 writer invocation/captureSerial、发布 ticket、native frameTag、采集 frame 与消费 frame；只用 frameTag 相等不足以证明对象归属。
- 矩阵字节属于当时绑定的已拥有副本，不在后台凭 slot 回读共享 arena。

原生算法依据：[保存的函数导出](../research/ida_export/war3_render_tree_C_20260504_034552.txt)，`0x6F12E600`（193 起）逐组调用 `0x6F12E200`（4318 起），每组输出 48B，按组成员进行矩阵算术平均。零组另有直接复制一条矩阵分支，须标 `RigidCopy`，不能把它伪造为正常 GroupAverage。

首选对照源是该次 `0x12E600` 输出的最终组矩阵；`0x12FDC0` 的 resolved pose 源段是辅助见证。只有确认其索引域和成员映射后，才能由后者重建组矩阵；不能根据函数注释的“final palette”就直接喂给共享 geoset groups。

## 5. 世界空间与模型局部空间

### 5.1 先固定数学意义，再决定是否求逆

[原生 ASM 合同](../research/war3_render_issues/28_gpu_skinning_takeover_feasibility/native_asm_contract.md)，250–272，记录 kernel RVA `0x0EDDC0`：

```text
x' = M[0]*x + M[3]*y + M[6]*z + M[9]
y' = M[1]*x + M[4]*y + M[7]*z + M[10]
z' = M[2]*x + M[5]*y + M[8]*z + M[11]
```

本方案采用上述公式对应的列向量数学记法 P。数组顺序为四列、每列三项；这不表示 DXVK 内部 Matrix4 的所有接口都是列向量乘法。必须通过明确的 pack/unpack 公式转换，不能仅按“行主序/列主序”标签猜一次 transpose。

如果已证明 **同一 part、同次消费** 满足 `P_world = W_instance × G_modelLocal`，才有：

`G_modelLocal = inverse(W_instance) × P_world`

候选 W 优先取该实例 CModel+0x64：当前 [TryReadRuntimeWorldTransform](../../src/d3d9/war3/model/war3_model_hook.cpp)，6181–6192。CSpriteUber+0x108 变换和 PoseRecord 只作对账，不在未知 part/附件关系时替换它。必须验证父子层级、缩放与 billboard/camera 影响是否已进入 P，不能凭字段名字认定分解成立。

特别注意 [draw-time 捕获注释](../../src/d3d9/d3d9_device.h)，2781–2786：动态 CPU-skinned 顶点通常已经在世界空间，D3DTS_WORLD 通常 identity；因此 **不能用这个 identity 当作从 native world palette 还原模型局部的实例 W**。它是最终 draw 的另一个空间见证。

### 5.2 待安排的最小证明实验

作者在场后另行安排，不在本轮执行：

1. 同资产双实例，不同平移/朝向；捕获同一 geoset 在两次原生消费中的绑定、组矩阵及各自 W，排除临时组范围被后一实例覆盖。
2. 先固定相机，再改变实例平移、yaw、统一缩放；验证按上述公式复算出的 world 顶点与原生 kernel 输出一致。用实际 vertexGroup，而非任取 palette 第 0 项。
3. 再改变相机；区分模型局部姿态中的 billboard 与实例 W，不能把相机依赖误消掉。
4. 记录实际矩阵中的非有限值、奇异/近奇异 W、部分捕获、重入/跨帧。求逆失败或关系未证即不输出认证的 local 结果。

保留 **原始 P_world 与原始 W 的 f32 位模式**，派生的 local 结果存另一个块，注明转换方法与误差。求逆/再乘引入舍入，不能据此要求与原生 world 输入逐位相等，也不能在差异出现后临时扩大容差。

第一阶段优先利用上游新增的显式 `World` 外部已求值输入，**不求逆也能先验证原始数据交接**；只有消费者明确支持该空间并且不会再乘实例 W 时才能使用。现有 `ExternalGroups` 不支持这种合同。

## 6. alpha/RGB 与法线必须独立对齐

FORGE 新工单要求“已求值组矩阵 + alpha + RGB、不要求动作时钟”的输入。WarVK 当前 [CanonicalMaterialContract](../../src/d3d9/war3/render/war3_canonical_draw.h)，159–173，含 alpha mode/ref、层/队列信息，**不含完整当前 GEOA alpha/RGB**；draw-time 的 alphaRef 也只是测试阈值。故这两条已知来源不足以完成共享材质输入。

下一次采集前需定位当前 geoset 的实际 GEOA 求值输出，分别记录：

- `geoaEvaluatedAlpha/Rgb`：与共享核心按原 geoset 顺序求值的同阶段数值。
- `layerAlpha`、`instanceColor/Alpha`、`finalDrawModulation`：存在则另列，不与前者混成一个“alpha/RGB”。
- RGB 顺序、绝对值或乘数、颜色编码/量化阶段及默认值的来源。未知不能填 1 或再次乘静态 GEOA RGB。

若只有最终 draw 颜色而无法分解为 GEOA，样本仍可保存，但 `GeoaChannelsUnproven`，不得对共享接口作完整输入认证。原生动作时钟未知不妨碍保存已求值数值，却妨碍与 FORGE 任意 Stand/Walk 时间点声称“同一姿态”。

法线已有明确语义差异：原生 `0x0EDDC0` 对法线只做同矩阵 3×3 变换、不归一化；FORGE [SkinModel.hlsl](E:/Mycode/Source/Repos/War3MapEditor64/src/Render/Shaders/SkinModel.hlsl)，22–25，及 SkinPreviewGeoset 会归一化。W1 必须保存 `nativeRawNormal` 与 `forgePreviewNormalizedNormal` 为不同语义，检查方向和长度分别报告；本轮不改任何一个算法，也不把方向接近写成法线逐位一致。

## 7. 证据表与准入结果

| 问题 | 本轮已核实 | 下一步缺的证明 | 未补齐时的结果 |
| --- | --- | --- | --- |
| 同 Footman 字节 | FORGE 回执的路径/SHA 与时间点可读 | 游戏真实挂载层及源字节 SHA | AssetSourceUnproven |
| 原 geoset 顺序 | 共享类型有原序 layout；WarVK 有 runtime 数组序号 | 全流比较或具名重映射的一对一表 | GeosetMappingUnproven |
| 组数/混合规则 | RVA 与保存的源码/ASM；现有缓存持有组流 | 该实例当前消费的 group remap/range | GroupBindingUnproven |
| world→local | 12 项布局与候选 W 入口已知 | 同一次 part 消费的组合关系、稳定 W | SpaceUnproven；不强制求逆 |
| 当前 alpha/RGB | 现有状态不能替代 GEOA | 实际求值来源与语义点 | MaterialChannelsUnproven |
| 当前实例生命周期 | 现有观察身份能作关联候选 | 指针复用/销毁/换图的有效实例见证 | InstanceIdentityWeak |
| 法线 | 已知原生/预览规则不同 | 同输入量化差异夹具 | 单列差异；不声称等价 |

零样本、全拒绝或只有一个匹配 geoset 不算 W1 数据对齐完成。每项应输出 `Proven / Candidate / Uncovered / Rejected`，说明证据级别；不把构建、输入认证、命令记录和像素恢复混为一个 ok。

## 8. 与上游的交接清单

需要 FORGE/War3RenderCore 提供：

1. 同一 Footman 的源摘要、**全原 geoset 顺序**、有序组成员、顶点组映射与各流摘要；不能只给截图或矩阵 FNV。
2. Stand 500、Walk 250/283、重复 283 的每 geoset 矩阵/alpha/RGB 实际数组，以及生成器、源码/编译器身份和 `PreviewApproximate` 诊断。
3. 显式 ModelLocal/World、不依赖动作时钟、不重算颜色的外部已求值输入；未知/数量不符拒绝，不改旧 Computed/ExternalGroups。
4. 只读绘制资产视图规格；公开对应原 geoset/groups 的数据，不要求 WarVK 越过 CompiledModel 私有 ModelData。
5. 有序组 remap、零组刚体、空间/法线差异的夹具。没有原版运行样本时注明 synthetic/preview，不称 native oracle。

本文件没有复制 FORGE 的实现、MDX 原始字节或逆向反编译代码；地址依据属于既有证据。新增到共享核心的代码来源以后逐项申报，本轮不向上游写入文件。
