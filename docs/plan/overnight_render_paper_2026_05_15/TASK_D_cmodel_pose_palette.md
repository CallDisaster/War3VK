# 子线程 D 任务卡 ★★★★★ — CModel Pose Palette / Matrix-Group / RuntimeModel
# 这是渲染论文最重要的一章（重中之重）

## 任务定位
渲染论文 **第 4 章**（用户明确指定 Pose 是重中之重）。
覆盖 War3 模型的"骨骼 → 矩阵 → palette → skinning → GPU"完整数据流。

## 已知锚点（IDA 已命名）
| 地址 | 名字 | 备注 |
|---|---|---|
| `0x6F12FED0` | `CModel_AllocAndFillGroupPalette` | 主要 palette writer |
| `0x6F12E600` | `CGeosetData_BuildGroupBlendedPalette` | 真正按 group 写 blended palette |
| `0x6F12FDC0` | `CModel_CopyPoseMatrixRangeFromStack` | 把 stack 上的 pose 拷到 +0x60 |
| `0x6F12FF90` | `CModel_AllocAndFillSimpleFallbackPalette` | 简单回退路径 |
| `0x6F12E900` | `CModel_EvalSingleGeosetAndRecurseChildren` | 单 geoset 评估 |

## 关键已知偏移
| 字段 | 含义 |
|---|---|
| `CModel + 0x5C` | 当前 pose matrix count |
| `CModel + 0x60` | 当前 pose matrix array (3x4) base |
| `CGeosetData + 0xF0` | groupCount |
| `RenderablePart + 0x08` | stagePresetSpanBaseIndex / palette slot index |
| `RenderablePart + 0x108` | geosetIndex |
| `Game.dll + 0xBC6BD0` | 全局 blended palette buffer |

## 已有研究（增补，不重写）
- `docs/research/war3_render_issues/22_cmodel_pose_palette_reverse/README.md`
- `docs/research/war3_render_issues/16_phase738_pose_stutter_root_cause/README.md`（如果存在）
- AGENTS.md 第 78 条 (Phase 7.47 dt gate 证伪)
- AGENTS.md 第 86 条（Phase 7.52 RenderablePart palette snapshot 修复）

## 必须搞清楚的问题（论文级别）

### 4.1 数据结构层
1. `CModel` 完整字段表（vtable, runtimeModel, instance, pose, child list, attachment, ...）
2. `CGeosetData` 完整字段表（matrixGroupSizes / matrixIndices / vertexGroupIndices / blendIndices / ...）
3. `CRenderablePart` 完整字段表（已知 `+0x08` slot, `+0x108` geosetIndex, 其它待补）
4. `runtimeModel` 与 `CModel` 的关系（可能是 alias，也可能是子对象）
5. `palette slot` 如何分配？为什么 War3 的 8 帧 cadence？

### 4.2 写入路径（4 个 writer + dispatcher）
1. `CSpriteUber_PreRender* → dt gate → CModel_EvalSingleGeosetAndRecurseChildren`
2. `EvalSingleGeoset` 内部分流：
   - 主路径：`AllocAndFillGroupPalette (0x12FED0) → BuildGroupBlendedPalette (0x12E600)`
   - 简单路径：`AllocAndFillSimpleFallbackPalette (0x12FF90)`
   - **同函数内**还会调 `CopyPoseMatrixRangeFromStack (0x12FDC0)` 维护 `+0x60`
3. 4 个 writer 的**精确 CFG**（哪个分支调用，每帧哪个先后顺序）
4. **写入频率统计**（已有 Phase 7.47 数据：mw=13650/frame, gpw=5849/frame, sgp=751/frame）—— 要解释为什么是这个比例

### 4.3 消费路径
1. RenderQueue / Dispatch_Common 怎么通过 `RenderablePart + 0x08` 的 slot 拿到 palette？
2. 全局 `0xBC6BD0 + slotIndex * 48` 是 SoA 还是 AoS？（48 字节 = 12 float = 一个 3x4 矩阵）
3. Vertex shader 拿 palette 的接口（D3D constant buffer 或 SSBO？）
4. CPU skinning vs GPU skinning：War3 实际用的是哪个？（Phase 7.55 v3 已证 War3 用 CPU skinning）
5. Attachment 怎么获取父节点的 bone matrix？（father runtimeModel？bone index？）

### 4.4 与 producer cadence 的关系（Phase 7 卡顿研究的最终澄清）
1. 为什么 producer 每帧都跑（runtimeMatrixWriteCount=370/frame stable）
2. 但 palette 内容跨多帧不变（Phase 7.48 CombinedHash 跨 8 帧 frozen）
3. 这是 CPU skinning 的本质特征：palette 只在 logic tick 更新；
   主渲染流畅是因为 GPU 上 VB 已经是 CPU skin 后的结果
4. shadow caster 的 GPU skinning 为什么会卡（项目早期方案的根因）
5. Phase 7.55 v4 的 draw-time VB capture 为什么解决（用 pre-skinned VB 而不是 palette）

### 4.5 Pose 写入 timeline（论文必须有图）
- Frame N logic tick：`PreRender → EvalGeoset → AllocAndFillGroupPalette → BuildBlendedPalette → CopyPoseFromStack`
- Frame N draw：使用 +0x60 的 final pose（CPU skinning 写 VB）
- Frame N+1 (no logic tick)：dt < epsilon，所有 writer 跳过；GPU 用 frame N 的 VB
- 用户视觉感知：在两个 logic tick 之间约 1-2 帧的 latency 内不易察觉

### 4.6 IDA rename / set_comments 建议
- 给 4 个 writer / EvalGeoset / EvalPoseStackAndChildren 等加详细中文注释
- 重命名所有相关 helper

## 输出格式
写到 `docs/plan/overnight_render_paper_2026_05_15/04_cmodel_pose_palette.md`：

```
# 第 4 章 ★★★★★ — CModel Pose Palette 数据流（重中之重）

## 4.1 数据结构（4 个核心类的完整字段表）
## 4.2 Pose 写入路径（4 个 writer + EvalGeoset 分流）
## 4.3 Pose 消费路径（RenderQueue → vertex shader）
## 4.4 CPU skinning vs GPU skinning 的本质区别
## 4.5 Logic tick / dt gate / palette cadence 的完整 timeline
## 4.6 项目历史踩坑（Phase 7.30~7.55 决策树）
## 4.7 IDA rename / set_comments 建议
```

## 工具
- IDA MCP
- 项目代码（**只读**）：
  - `src/d3d9/war3/model/war3_model_hook.{h,cpp}`
  - `src/d3d9/d3d9_device.cpp`（搜 `War3TryBuildLiveRuntimeGroupPalette` / `RuntimeMatrixWrite`）
  - `src/d3d9/war3/render/war3_current_draw_contract.{h,cpp}`

## 不能做的事
- 不动源码
- 不启动 War3
- IDA 写回交主线程

## 完成条件
- 文档至少 1500 行（这是论文最长一章）
- 4 个核心类字段表必须**完整**（不能只列已知）
- 4 个 writer 的 CFG 必须画出
- 历史踩坑（Phase 7.30 ~ 7.55）必须有完整 timeline
- 至少 50 处新命名建议
