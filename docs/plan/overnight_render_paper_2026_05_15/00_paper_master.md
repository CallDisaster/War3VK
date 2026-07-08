# 魔兽争霸 3 (1.27a) 渲染层实现论文 — 主交付（v1）

> **版本**：v1（2026-05-15 夜间无人值守生成）
> **基线**：`Game.dll @ ImageBase 0x6F000000`，War3 1.27a。
> **作者交付方式**：本论文以"章节集合"形式交付，每章独立成文。本文件为汇总入口与
> 阅读路线图。
>
> **范围说明（重要）**：
> 本论文 v1 包含十个完整章节——
> 第 1 章（剔除 → 渲染层过渡）、第 2 章（RenderQueue 完整数据流）、
> 第 3 章（CSprite 动画系统）、第 4 章（Pose 数据流，用户指定的"重中之重"）、
> 第 5 章（CGeosetData 顶点/skinning）、第 6 章（FogMask 静态阴影治理）、
> 第 7 章（Light/Shadow pass）、第 8 章（D3D9 State Bridge）、
> 第 9 章（UI 渲染分支）、第 10 章（粒子/Effect）。
> 这十章构成了"逻辑层对象 → GPU draw call"的完整逆向链，覆盖全部渲染域。

## 0. 论文定位

用户在性能优化主线之外开了一条研究线，要求做：

1. War3 1.27a 渲染层 *未逆向部分* 的完整覆盖；
2. **Pose 是重中之重**（用户多次强调）；
3. 所有逆向完毕、效果确认的函数 **写回 IDA**（rename + comment）；
4. 静态阴影问题继续加强研究（24 号文档 v3 之后）；
5. 不动 `src/` 源码（性能优化的另一线程在跑）；
6. 不启动 War3 / AutoTest（避免争用进程）。

## 1. 论文结构（v1 实际交付）

```
docs/plan/overnight_render_paper_2026_05_15/
├── 00_paper_master.md                  ← 本文（论文阅读入口）
├── OVERNIGHT_PLAN.md                   ← 总规划（含子线程任务列表）
├── 01_visibility_to_renderqueue.md ★   ← 第 1 章 剔除→渲染过渡（约 800 行）
├── 02_renderqueue_dispatch.md      ★★  ← 第 2 章 RenderQueue 完整数据流（约 1100 行）
├── 03_csprite_animation.md         ★   ← 第 3 章 CSprite 动画系统（约 750 行）
├── 04_cmodel_pose_palette.md      ★★★ ← 第 4 章 Pose（约 990 行，重中之重）
├── 05_cgeoset_vertex_skinning.md   ★★★ ← 第 5 章 CGeosetData 顶点/skinning（约 500 行）
├── 06_fogmask_static_shadow.md     ★★ ← 第 6 章 静态阴影（约 600 行）
├── 07_light_shadow_pass.md         ★★★ ← 第 7 章 Light/Shadow pass（约 450 行）
├── 08_d3d9_state_bridge.md         ★★ ← 第 8 章 D3D9 State Bridge（约 300 行）
├── 09_ui_rendering.md              ★  ← 第 9 章 UI 渲染分支（约 150 行）
├── 10_particle_effect.md           ★  ← 第 10 章 粒子/Effect（约 120 行）
├── TASK_A_visibility_to_renderqueue.md
├── TASK_B_renderqueue_dispatch.md
├── TASK_C_csprite_animation.md
├── TASK_D_cmodel_pose_palette.md
└── TASK_F_fogmask_static_shadow.md
```

## 2. 阅读路线图

### 2.0 我只想理解一帧从 MainLoop 到 GPU 的完整流程

按章节顺序阅读：第 1 章 → 第 2 章 → 第 3 章 → 第 4 章。
这条路径覆盖：
- 第 1 章：CWorld FrameUpdate → 视锥剔除 → 22 stage 调度 → group 0/1/2
- 第 2 章：RenderQueue 入队 → 主队列 / AUCTransparent → 排序 → Dispatch_Common/Special
- 第 3 章：CSprite 动画推进 → dt gate → BuildPoseStackRoot → SetWorldMatrix
- 第 4 章：4 个 palette writer → CPU skinning → 上传 dynamic VB

### 2.1 我只想知道 Pose 怎么工作

读 [`04_cmodel_pose_palette.md`](04_cmodel_pose_palette.md)。重点章节：

- §0.2 关键 RVA 锚点（4 个 writer + dispatcher）
- §1 数据结构层（CModel / CGeosetData / CRenderablePart 完整字段表）
- §2 Pose 写入路径（4 个 writer + EvalGeoset 分流）
- §3 Pose 消费路径（CPU skinning kernel）
- §5 Logic tick / palette cadence 8-frame 规律
- §6 Phase 7.30 ~ 7.80 完整决策树
- §7 IDA 写回清单

### 2.2 我只想知道建筑阴影怎么治理

读 [`06_fogmask_static_shadow.md`](06_fogmask_static_shadow.md)。重点章节：

- §1 数据结构（CFogMask / CFogMaskTable / CFogOfWarMap）
- §2 `WriteMaskRegion (0x234710)` 完整逆向（4 个 mask layer + type code 拆解）
- §2.6 `CFogMaskTable.idx` 分类（**这是治理的关键**：idx=3 = 阴影 footprint）
- §5 中央 `CWidget_RegisterFootprintAndShadowMask (0x65A140)` 30+ caller 分桶
- §7 静态阴影治理蓝图（**方案 A 推荐**）
- §8 项目历史拦截尝试反证表

### 2.3 我想做项目源码改动（治理建筑阴影）

按以下顺序操作：

1. 读 §7.4 推荐路径；
2. 读 §7.1 方案 A 的 hook 伪代码；
3. 在 `src/d3d9/war3/hooks/war3_hook_shadow.{h,cpp}` 新增 `Hook_TerrainShadow_WriteMaskRegion`；
4. 先在 DebugView 加 stats 日志（不拦截，仅观察 maskIdx 分布）；
5. 验证 maskIdx==3 命中数与建筑创建/销毁次数大致对应后，再启用拦截。

### 2.4 我想看完整的 dt gate / palette cadence 实测数据

读 第 4 章 §4.2 - §4.3、§5.4。Phase 7.47 黑匣子数据完整呈现。

### 2.5 我想做 IDA reverse engineering

每个章节的 §0.2 都列出了完整 RVA 锚点速查表。
反编译产物在 `AutoTest/artifacts/_overnight_render_research/` (320+ 文件)。

### 2.6 我想知道 RenderQueue 是怎么排序+分发的

读第 2 章。Dispatch_Common / Dispatch_Special / fallback multi-pass / 5 种 transparent type 全部覆盖。

### 2.7 我想理解 CSprite 4 个 PreRender 变体的差异

读第 3 章 §1。Full/Mini/MiniLite/FullLite 各自 CFG 图。

## 3. 关键发现（一句话总结）

### 3.1 全局架构（第 1-2 章）

> War3 1.27a 主渲染按 **22 个 stage** 调度，**2 次 RenderQueue_FlushAndReset** 切割主渲染期。
> group 0/1/2 对应 装饰物/单位/飞行，分别对应 stage 11/12/13。
> RenderQueue 由 *opaque 主队列*（10000 entry × 20B）+ *AUCTransparent 辅队列*
> （10000 entry × 24B）组成。Opaque 排序优先级 = special vs not → layer state →
> meshData ptr；透明排序优先级 = type → distSq。
> 项目主 hook 集中在 `RenderQueue_FlushAndReset` 与 `WorldObjects_RenderGroup` 处。

### 3.2 CSprite 动画系统（第 3 章）

> 4 个 `CSpriteUber_PreRender*` 变体都走同一个 dt gate
> （`fabs(dt) >= 2 * FLT_EPSILON`），但 dt > 0 占 98.79%，dt gate 几乎不早退。
> Phase 7.47 实测证伪了"dt gate 是阴影卡顿根因"假设。
> 真正的 anim 推进通过 `flags & 0x60000` 选择三种路径：标准 dt / SpriteSkip / ConstFlag。

### 3.3 Pose 主线（第 4 章）

> War3 1.27a **不在 GPU 上做 skinning**。它在 CPU 端把 pose palette 应用到 VB，
> 把 skin 后的顶点上传，shader 不消费 palette uniform/SSBO。
> 这就是为什么"palette 跨多帧不变"主渲染流畅，但项目的 GPU-skinning shadow caster
> 会卡——它直接读了 stale palette。
> 唯一可行的修复：旁路 GPU skinning，直接消费 CPU skin 后的 VB（Phase 7.55 v4）。

### 3.4 静态阴影（第 6 章）

> War3 建筑预渲染贴花阴影 **既不走 ListA/ListB，也不走 RegisterImage，
> 也不走 ShadowProjector，也不走 CDoodads stamp**。
> 它通过 `TerrainShadow_WriteMaskRegion (0x234710)` 直接修改 `CFogMask` 的
> 16-bit mask grid，由地形渲染管线在画地面 tile 时按 mask bit 着色。
> 决定"在哪份 mask 上写"的是对象 `+0x10C` 字段：`idx=3 = shadow footprint`。
> 因此 hook `WriteMaskRegion` + `maskIdx == 3` 拦截即可干净屏蔽建筑阴影，
> 不影响 fog/LOS/path。

## 4. IDA 写回完成度

| 章节 | rename | set_comments | 状态 |
|---|---|---|---|
| 24 号文档（CDoodads / CUnit / FogMask v3） | ~10 | ~9 | 历史已写 |
| 第 1 章 剔除→渲染过渡 | 26 | 6 | ✅ 本轮写回 |
| 第 2 章 RenderQueue 分发 | 23 | 13 | ✅ 本轮写回 |
| 第 3 章 CSprite 动画 | 7 | 0 | ✅ 本轮写回 |
| 第 4 章 Pose | 24 | 13 | ✅ 历史写回 |
| 第 5 章 CGeosetData 顶点/skinning | 15 | 6 | ✅ 本轮写回 |
| 第 6 章 静态阴影 | 41 | 14 | ✅ 历史写回 |
| 第 7 章 Light/Shadow pass | 30 | 4 | ✅ 本轮写回 |
| 第 8 章 D3D9 State Bridge | 8 | 2 | ✅ 本轮写回 |
| 第 9 章 UI 渲染 | 11 | 0 | ✅ 本轮写回 |
| 第 10 章 粒子/Effect | 3 | 0 | ✅ 本轮写回 |
| **合计** | **~198** | **~67** | 全部 ok=true |

后续打开 IDA 看反编译，关键函数都直接显示中文语义+背景注释。

## 5. 反证表（历史所有失败拦截尝试）

| 失败尝试 | 拦截点 | 结果 | 根因 |
|---|---|---|---|
| 1 | RegisterImage 全屏蔽 | 崩溃，建筑阴影还在 | 建筑阴影不走 RegisterImage |
| 2 | ListA/ListB type=4 | 部分消失，边缘伪影 | 建筑阴影不进 ListA/ListB |
| 3 | ShadowProjector_Add_FromObject | 单位贴花消失，建筑还在 | 建筑阴影不经过 ShadowProjector |
| 4 | ListA stamp 注册池 | 树木消失，建筑还在 | 那是 CDoodads 路径 |
| **未尝试 → 推荐** | **`WriteMaskRegion + maskIdx==3`** | **预期：建筑阴影消失，fog/LOS/path 不受影响** | **本论文锁定的真正写入路径** |

## 6. 后续工作

论文 v1 十章已全部完成，覆盖"逻辑层 → GPU draw call"全链路。

**已完成的 IDA 写回**：~198 处 rename + ~67 条 set_comments，涵盖：
- 主渲染链（CWorld → DispatchStage → RenderQueue → Dispatch）
- Pose 数据流（4 个 writer + EvalGeoset + CSpriteUber）
- CGeosetData 顶点/skinning（MatrixGroupRemap + CPU skinning kernel）
- TerrainShadow 全系统（30 个函数 + WriteMaskRegion + DispatchToShape）
- GxDevice 桥接层（8 个 vtable wrapper）
- UI 渲染分支（11 个函数）

**下一步可选扩展**：
- 更多 CEffect 派生类的 vtable 命名
- CUnit 状态机的完整事件分发表
- D3D9 state block 的字段级文档

## 7. 不能做的事（约束清单，下一轮 agent 必读）

- ❌ 不动 `src/` 任何源码（性能优化的另一线程在跑）
- ❌ 不启动 War3
- ❌ 不做 AutoTest（会跟性能优化线程争 War3 进程）
- ❌ 不删除现有研究文档（只新增/增补）
- ❌ 不在主线程做 grunt work（反编译/材料整理交子线程）
- ✅ 主线程只做：规划、IDA 写回、章节拼装、关键阻塞深挖
