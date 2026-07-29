# 第 7 章 ★★★ — Light Pass / Shadow Pass 着色器与 RT 绑定（GPU 侧）

> 2026-07-15 authoritative correction：`0x6F76F060` 不是 TerrainShadow owner 的 thiscall；
> 其 ECX 是 0..16 selector，canonical ABI 为
> `RenderGlobalPass_DispatchBySelector(int selector)`。selector13 明确尾调 TextTag pass，故本章
> 后续把全部 case 统称 TerrainShadow 的段落只保留历史。逐 case 证据见
> [30 号卷](../../research/war3_render_issues/30_cworld_class_family_full_reverse/callgraphs.md)。
>
> 本章覆盖 War3 的阴影渲染管线：从 War3 原生 TerrainShadow 系统到项目的
> D3D9 CSM shadow caster pipeline 的完整接管。
> 它回答：War3 原生怎么画阴影、我们怎么接管、shadow map / receiver / TAA 怎么工作。

## 0. 阅读与证据基线

### 0.1 写作前提

1. 版本基线：`Game.dll @ ImageBase 0x6F000000`（War3 1.27a）。
2. 项目源码：`src/d3d9/d3d9_war3_shadow*.cpp`、`src/d3d9/d3d9_war3_shadow.h`。
3. Shader 源码：`subprojects/war3fx/shaders/war3_shadow_*.frag/vert`。
4. 历史数据：`AGENTS.md` 第 59~132 条。

### 0.2 关键 RVA 锚点速查表

| RVA | 名字 | 角色 |
|---|---|---|
| `0x6F76F060` | `RenderGlobalPass_DispatchBySelector` | global selector 0..16；并非所有 case 都属于 TerrainShadow |
| `0x6F76EF80` | `TerrainShadow_FlushPass` | 原生阴影 flush |
| `0x6F737500` | `TerrainShadow_RenderListA` | 渲染 ListA（混合层：雾/边界/烘焙阴影/贴花） |
| `0x6F737400` | `TerrainShadow_RenderListB` | 渲染 ListB |
| `0x6F737620` | `TerrainShadow_RenderLayer` | 渲染单层 |
| `0x6F736D50` | `TerrainShadow_ListA_RenderEntryComplex` | ListA 复杂条目渲染 |
| `0x6F736C20` | `TerrainShadow_ListA_RenderSubBatch` | ListA 子批次渲染 |
| `0x6F713250` | `TerrainShadow_RegisterImageEntry` | 注册阴影图像条目 |
| `0x6F7290B0` | `TerrainShadow_RegisterImageEntryWithParams` | 带参数注册 |
| `0x6F74DB30` | `TerrainShadow_ToggleStaticStampFromObject` | 按对象开关静态 stamp |
| `0x6F74DE40` | `TerrainShadow_ToggleEmitterStamp` | 按对象开关 emitter stamp |
| `0x6F234710` | `TerrainShadow_WriteMaskRegion` | 写 mask grid（建筑阴影真正入口） |
| `0x6F233E90` | `TerrainShadow_RebuildMaskFromObjectLists` | 整体重建 mask |
| `0x6F6F4CE0` | `TerrainShadow_Node7E4_Render` | 节点 7E4 渲染 |

## 1. War3 原生阴影系统概览

### 1.1 三条独立路径

War3 的阴影系统由三条完全独立的路径组成，最终落到不同的渲染数据结构：

```
路径 X: CDoodads stamp 注册（树木/装饰物/腐地）
  └─ CDoodads::CreateDoodadAndActivate (0x74D500)
      ├─ ShadowPath_StaticStamp_Toggle (0x74E420)     → ListA mask 直写
      ├─ TerrainShadow_ToggleStaticStampFromObject     → RegisterImageEntry(type=0)
      └─ TerrainShadow_ToggleEmitterStamp              → RegisterImageEntry(type=4)

路径 Y: CUnit ShadowProjector 注册（单位脚下方块/特效 emitter）
  └─ CUnit::RefreshAllShadowEmitters (0x5457B0)
      └─ ShadowPath_ObjectProjector_Runtime (0x38D7A0)
          └─ ShadowProjector_Add_FromObject (0x76D800)
              └─ RegisterImageEntryWithParams → RegisterImageEntry(type=4)

路径 Z: FogMask 直写（建筑预渲染贴花阴影 + 路径阻挡 + 迷雾 + 视野）
  └─ TerrainShadow_WriteMaskRegion (0x234710)
      └─ CFogMaskTable mask grid 16-bit 按 type bit set/clear
      └─ 由地形渲染管线在画地面 tile 时按 mask bit 着色
```

### 1.2 为什么项目的历史拦截全部失效

| 历史尝试 | 拦截点 | 结果 | 根因 |
|---|---|---|---|
| 1 | RegisterImage 全屏蔽 | 崩溃 + 建筑阴影还在 | 建筑阴影不走 RegisterImage |
| 2 | ListA/ListB type=4 | 部分消失 | 建筑阴影不进 ListA/ListB |
| 3 | ShadowProjector_Add_FromObject | 单位贴花消失，建筑还在 | 建筑阴影不经过 ShadowProjector |
| 4 | ListA stamp 注册池 | 树木消失，建筑还在 | 那是 CDoodads 路径 |
| **正确方案** | **WriteMaskRegion + maskIdx==3** | **预期：建筑阴影消失，fog/LOS/path 不受影响** | **路径 Z 才是建筑阴影真正入口** |

### 1.3 TerrainShadow_Dispatch 的 16 个 stage

`CWorld_TerrainShadow_Dispatch (0x6F76F060)` 按 stage code 0..16 分发：

| Stage | 语义 | 渲染内容 |
|---|---|---|
| 0 | 主阴影渲染 | ListA + ListB + Layer + Node7E4 |
| 1 | 阴影层 1 | RenderLayer(1, 0, 0) |
| 2 | 阴影层 2 | RenderLayer(0, 1, 1) |
| 3 | 阴影层 3 | RenderLayer(0, 1, 2) |
| 4 | 子分发器 | vtable[9] 调用 |
| 5..16 | 其他 | 各种辅助渲染 |

## 2. 项目的 D3D9 CSM Shadow Caster Pipeline

### 2.1 架构概览

项目的阴影系统完全替代了 War3 原生的阴影渲染，使用现代 CSM（Cascaded Shadow Maps）：

```
BeforeUi 事件
  └─ War3TryPopulateSemanticShadowScene
      ├─ War3TryPopulateDirectCurrentDrawGrouped (current-draw 路径)
      │   ├─ 遍历 currentDraw contracts
      │   ├─ 构建 War3ShadowCasterDraw
      │   ├─ 路径阻断器过滤
      │   └─ shadowCasters.emplace_back
      ├─ War3TryPopulateDrawTimeSemanticProducer (draw-time 补充路径)
      │   ├─ 遍历 m_war3DrawTimeVBCache fresh entries
      │   ├─ 查 VisibleRenderableRegistry
      │   ├─ 构建 War3ShadowCasterDraw (pre-skinned VB)
      │   └─ shadowCasters.emplace_back
      └─ 合并返回

Shadow Map Pass
  └─ renderShadowMap()
      ├─ BuildShadowReplayDraws (构建 replay draw 列表)
      ├─ 4 个 CSM cascade:
      │   ├─ 计算 cascade 矩阵（orthographic projection）
      │   ├─ 过滤 + 排序 draw 列表
      │   ├─ 对每个 caster:
      │   │   ├─ 绑定 pipeline (vertex shader + fragment shader)
      │   │   ├─ 绑定 vertex/index buffer
      │   │   ├─ 设置 uniform (worldMatrix, alphaTest, etc.)
      │   │   └─ DrawIndexedPrimitive
      │   └─ 生成 shadow depth texture
      └─ 输出: 4 张 cascade shadow map

Receiver Pass
  └─ renderShadowReceiver()
      ├─ 绑定 shadow map textures
      ├─ 绑定 TAA history (ping-pong)
      ├─ 对每个 terrain tile:
      │   ├─ 计算 shadow visibility (PCF sampling)
      │   ├─ TAA 历史混合
      │   └─ 输出 shadow factor
      └─ 输出: shadow visibility texture
```

### 2.2 Shadow Caster 数据结构

```cpp
struct War3ShadowCasterDraw {
  // 顶点数据
  Rc<DxvkBuffer> positionStorage;
  DxvkResourceBufferInfo positionInfo;
  uint32_t positionStride;
  uint32_t positionOffset;
  VkFormat positionFormat;

  // 索引数据
  Rc<DxvkBuffer> indexStorage;
  DxvkResourceBufferInfo indexInfo;
  VkIndexType indexType;
  uint32_t indexCount;
  uint32_t firstIndex;
  uint32_t vertexOffset;
  uint32_t vertexCount;

  // 变换
  Matrix4 worldMatrix;
  Vector4 boundsCenter;
  float boundsRadius;

  // 材质
  bool alphaTestEnabled;
  float alphaRef;
  bool alphaBlendEnabled;
  bool depthWriteEnabled;
  Rc<DxvkImageView> diffuseTexture;
  Rc<DxvkSampler> diffuseSampler;

  // 蒙皮
  bool vertexBlendEnabled;
  uint32_t paletteIndex;

  // 分类
  uint8_t objectKind;
  uint32_t batchHandle;
  War3BatchTag batchTag;
  War3RenderState::StageCategory category;
};
```

### 2.3 CSM Cascade 系统

4 级 cascade，每级覆盖不同距离：

| Cascade | 覆盖距离 | 分辨率 |
|---|---|---|
| 0 | 近距离（~1000 单位） | 2048×2048 |
| 1 | 中距离（~2000 单位） | 2048×2048 |
| 2 | 远距离（~4000 单位） | 2048×2048 |
| 3 | 最远距离（~8000 单位） | 2048×2048 |

Phase 7.68 调整：`maxDistance` 从 8000 改为 4000（War3 RTS 相机俯角很少看 8000 远）。

### 2.4 Shadow Map 自适应分辨率

`kShadowAdaptiveResolutionEnabled`（Phase 7.68 默认关闭）：
- 当 caster 数量多且视角稳定时，可以降低分辨率换取性能
- 但 Phase 7.68 发现降级到 2048 对 FPS 提升很小，不如保持 4096

## 3. Shadow TAA（Temporal Anti-Aliasing）

### 3.1 概念

Shadow TAA 通过混合当前帧和历史帧的 shadow visibility 来降低阴影边缘的锯齿。

### 3.2 实现

```
Ping-Pong History Buffer:
  ├─ shadowCurrent[0]: 当前帧 visibility
  ├─ shadowCurrent[1]: 历史帧 visibility
  └─ 每帧交换读写

Motion-Adaptive Blending:
  newWeight = 0.12 + motionVector * 8.0
  output = lerp(history, current, newWeight)
```

### 3.3 Phase 7.58 调整

- `shadowTaaEnabled` 默认改为 `true`
- 移除"semantic dynamic caster 一刀切禁用 TAA"的旧逻辑
- 新增 `DXVK_WAR3_SHADOW_DISABLE_TAA_FOR_SEMANTIC_DYNAMIC` env 开关

## 4. 路径阻断器过滤

### 4.1 黑名单

8 个 fourcc（第二字符大小写归一化）：
```
YTab, YTac, YTpb, YTpc, YTfb, YTfc, YTlb, YTlc
```

### 4.2 拦截点（11 个分桶）

| 分桶 | 函数 | 触发条件 |
|---|---|---|
| EntryGate | `War3TryCaptureShadowCaster` 入口 | 所有 capture |
| EarlyBypass | `War3TryCaptureShadowCaster` 早期分支 | `earlyNeedsSemanticContext=true` |
| Producer | `War3TryPopulateDrawTimeSemanticProducer` | draw-time VB cache 遍历 |
| FastAppend | `tryAppendDrawTimeFastEligible` | fast-append 路径 |
| DirectGrouped | `War3TryPopulateDirectCurrentDrawGrouped` | current-draw grouped 路径 |
| StaticSupplement | 静态补充路径 | 静态对象补充 |
| AppendVbBlend | `War3TryAppendSemanticShadowPacket` | VB blend append |
| AppendEntry | `War3TryAppendSemanticShadowPacket` | 通用 append |
| LegacyCapture/Main | `War3TryCaptureShadowCaster` legacy 主路径 | legacy capture |
| LegacyCapture/TerrainDoodadFallback | legacy terrain doodad 路径 | terrain doodad |
| EligibilityGate | `War3ShouldSubmitSemanticPacket` | eligibility 检查 |

### 4.3 已知限制

D3D9 CSM pipeline 的拦截**已经完整工作**（Phase 7.136 log 证明 27 次/30s 命中）。
但 path blocker 的 **TerrainShadow 原生系统**（路径 Z: `WriteMaskRegion`）不受 D3D9 hook 控制。
需要 hook `TerrainShadow_WriteMaskRegion` + `maskIdx==3` 才能彻底屏蔽。

## 5. Shadow Receiver

### 5.1 Receiver Pass

Receiver pass 在 shadow map 生成后执行，负责：
1. 采样 shadow map（PCF filtering）
2. 混合 TAA history
3. 输出 shadow visibility factor

### 5.2 Shader 实现

`war3_shadow_receiver.frag`:
- 读 shadow map depth texture
- 计算 PCF（Percentage-Closer Filtering）
- TAA history blending（motion-adaptive weight）
- 输出 shadow factor

`war3_shadow_visibility.frag`:
- TAA current-visibility prepass
- 稳定 4 tap 采样

### 5.3 Phase 7.67 调整

- receiver history 采样使用连续 UV
- motion-adaptive 新帧权重从 `0.18/mv*12` 收敛到 `0.12/mv*8`
- 树影 history 更愿意积累（减少抖动）

## 6. Alpha Shadow 处理

### 6.1 Alpha-Blend Promote（Phase 7.52）

当物体只有 `alphaBlendEnabled`（没有 `alphaTestEnabled`）但有 diffuseTexture + UV 时，
自动 promote 为 alpha-test shadow：

```cpp
effectiveAlphaTest = alphaTestEnabled ||
    (alphaBlendEnabled && diffuseTexture && uvFormat valid && uvStride > 0);
```

alphaRef: cutout 用原值，alpha-blend promote 时用 0.5。

### 6.2 Hashed Alpha Shadow（Phase 7.68 默认关闭）

`alphaShadowHashed=false`：不使用 dither 模板，走确定性 hard cutoff。
原因：hashed fractional coverage + mip sampler 在 CSM/TAA 后表现为树影噪声和糊边。

### 6.3 Texture-Anchored Dither（Phase 7.67）

Caster fragment shader 的 alpha-test hash 改为绑定 alpha 贴图 texel：
- 树叶遮罩的随机模板跟着纹理图案走
- 避免 CSM/camera 微动时模板在叶片上滑动
- 缩窄 hash 过渡带，减少树叶边缘的随机覆盖面积

## 7. 项目接管点对应表

| War3 原生 | 项目接管 | 说明 |
|---|---|---|
| `TerrainShadow_Dispatch` stage 0 | `renderShadowMap()` | 完全替代原生阴影渲染 |
| `TerrainShadow_RenderListA/B` | 不使用 | 原生 ListA/ListB 被绕过 |
| `RegisterImageEntry` | `Hook_TerrainShadow_RegisterImageEntry` | 仅用于诊断/统计 |
| `WriteMaskRegion` | `Hook_TerrainShadow_WriteMaskRegion` | 诊断-only（pass-through） |
| `ShadowProjector_Add_FromObject` | `Hook_ShadowProjector_Add_FromObject` | FourCC 过滤 |
| D3D9 `DrawIndexedPrimitive` | `War3TryCaptureShadowCaster` | draw-time VB capture |
| `RenderQueue_FlushSortedItems` | `Hook_FlushSortedItems` | semantic scene 构建 |

## 8. 关键 env 开关

| 环境变量 | 默认值 | 含义 |
|---|---|---|
| `DXVK_WAR3_SHADOW_TAA` | 1 | 启用 Shadow TAA |
| `DXVK_WAR3_SHADOW_DISABLE_TAA_FOR_SEMANTIC_DYNAMIC` | 0 | 禁用动态 caster 的 TAA |
| `DXVK_WAR3_SHADOW_ALPHA_HASH` | 0 | 启用 hashed alpha shadow |
| `DXVK_WAR3_SHADOW_ALPHA_MIP` | 0 | 启用 alpha mip sampling |
| `DXVK_WAR3_SHADOW_ALPHA_MIP_BIAS` | 0.0 | alpha mip LOD bias |
| `DXVK_WAR3_SEMANTIC_DRAW_TIME_DIRECT_PRODUCER` | 1 | 启用 draw-time producer |
| `DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND` | 1 | 启用 fast append |
| `DXVK_WAR3_SEMANTIC_DRAW_TIME_PREBUILD_BYPASS` | 1 | 启用 prebuild bypass |

## 9. IDA rename 清单（本章相关）

| 原名 | 新名 | 地址 |
|---|---|---|
| `sub_6F76F060` | `CWorld_TerrainShadow_Dispatch` | `0x6F76F060` |
| `sub_6F76EF80` | `TerrainShadow_FlushPass` | `0x6F76EF80` |
| `sub_6F737500` | `TerrainShadow_RenderListA` | `0x6F737500` |
| `sub_6F737400` | `TerrainShadow_RenderListB` | `0x6F737400` |
| `sub_6F737620` | `TerrainShadow_RenderLayer` | `0x6F737620` |
| `sub_6F736D50` | `TerrainShadow_ListA_RenderEntryComplex` | `0x6F736D50` |
| `sub_6F736C20` | `TerrainShadow_ListA_RenderSubBatch` | `0x6F736C20` |
| `sub_6F713250` | `TerrainShadow_RegisterImageEntry` | `0x6F713250` |
| `sub_6F7290B0` | `TerrainShadow_RegisterImageEntryWithParams` | `0x6F7290B0` |
| `sub_6F74DB30` | `TerrainShadow_ToggleStaticStampFromObject` | `0x6F74DB30` |
| `sub_6F74DE40` | `TerrainShadow_ToggleEmitterStamp` | `0x6F74DE40` |
| `sub_6F234710` | `TerrainShadow_WriteMaskRegion` | `0x6F234710` |
| `sub_6F233E90` | `TerrainShadow_RebuildMaskFromObjectLists` | `0x6F233E90` |
| `sub_6F6F4CE0` | `TerrainShadow_Node7E4_Render` | `0x6F6F4CE0` |
| `sub_6F234420` | `TerrainShadow_DispatchToShape` | `0x6F234420` |
| `sub_6F234620` | `TerrainShadow_WriteMaskRegion_ForObject` | `0x6F234620` |
| `sub_6F3DB260` | `TerrainShadow_WriteMaskRegion_FromActorRuntime` | `0x6F3DB260` |
| `sub_6F714D10` | `TerrainShadow_TickAndRunUpdateList` | `0x6F714D10` |
| `sub_6F7469E0` | `TerrainShadow_List78C_RenderOne` | `0x6F7469E0` |
| `sub_6F73DE70` | `TerrainShadow_ListA_PrepareSortableGroups` | `0x6F73DE70` |
| `sub_6F737310` | `TerrainShadow_RenderListBEntry` | `0x6F737310` |
| `sub_6F7370A0` | `TerrainShadow_ListA_RenderPreparedGroups` | `0x6F7370A0` |
| `sub_6F75E340` | `TerrainShadow_RefreshStampScaleResource` | `0x6F75E340` |
| `sub_6F76D5F0` | `TerrainShadow_RegisterImageEntryFromPoint` | `0x6F76D5F0` |
| `sub_6F76D6D0` | `TerrainShadow_RegisterImageEntryFromTwoPoints` | `0x6F76D6D0` |
| `sub_6F76F5F0` | `TerrainShadow_RefreshEntryResourceByPos` | `0x6F76F5F0` |
| `sub_6F1F4DD0` | `TerrainShadow_PolyFastpath` | `0x6F1F4DD0` |
| `sub_6F1F5180` | `TerrainShadow_BoxFastpath` | `0x6F1F5180` |
| `sub_6F233570` | `TerrainShadow_ScanlineFastpath` | `0x6F233570` |

---

*本章约 450 行。下一章：第 8 章 D3D9 State Bridge / GxDevice。*
