# 无人值守渲染层论文 + 静态阴影深化 — 总规划（2026-05-15）

## 0. 用户的硬要求
1. **本线程仅做逆向**，不允许动 War3 项目源码（性能优化的另一线程在跑，不能干扰）。
2. 把魔兽争霸 3 从**逻辑层剔除（visibility/scene cull）→ 渲染层过渡**开始一路逆向，直到把
   渲染层"未逆向过的内容"都覆盖一轮。
3. 第二天交一份**详细的渲染层实现论文**，**Pose 是重中之重**。
4. 所有"逆向完毕、确认效果"的函数 **必须写回 IDA**（rename + comment）。
5. 在逆向同时**继续加强静态阴影问题研究**（24 文档 v3 找到的 `WriteMaskRegion` 路径）。
6. 主线程不要亲自做工作量大、不重要的事——**子线程并行**，避免上下文打满。
7. 没事不要停下来，要给自己创造任务，今晚必须高密度推进。

## 1. 主线程负责（保留上下文）
- 整体规划、子线程任务分配 + 结果汇总
- IDA 写回（rename / set_comments）—— 这是最终交付物的一部分
- 论文最终拼装（需要把握全文结构）
- 关键阻塞点的深挖（如静态阴影 type code bit 的精确锁定）

## 2. 子线程并行计划（按"独立可推进"原则切分）

> 每个子线程都是 **`general-task-execution`**，自带工具，输入"独立的逆向任务包"，
> 输出 markdown 研究稿 + 建议的 IDA rename/comment 列表（主线程统一回写）。

### 子线程 A：剔除层 → 渲染层过渡
- 范围：`CWorld::FrameUpdate` → 视锥剔除 / quadtree 查询 → 可见对象列表 → `RenderQueue` 入口
- 关键函数已知锚点：`worldFrameUpdateAndPreparePasses(0x368480)`、`CWorld_RenderScene(0x3681C0)`、
  `CWorld_DispatchStage(0x363020)`、`CWorld_WorldObjects_RenderGroup(0x368E30)`、
  `WorldObjectEntry_Render(0x184EE0)`、`worldObjectListEntryWrite(0x0CB110)`
- 输出：剔除策略 + 可见集分发 + 进入 RenderQueue 的边界

### 子线程 B：RenderQueue 入队 / 排序 / 分发深挖
- 范围：`RenderQueue_AddBatch` → opaque/transparent 分流 → `FlushSortedItems` → `Dispatch_Common/Special` → `RenderBatch_Submit`
- 关键已知：`renderQueueAddBatch(0x139190)`、`renderBatchSubmit(0x1375C0)`、
  `aucTransparentAddEntry(0x137AF0)`、`flushSortedItems(0x1380A0)`、
  `dispatchCommon(0x13A5E0)`、`dispatchSpecial(0x13A780)`、`applyDrawStateAndDraw(0x138F70)`、
  `rqStageUpdate(0x13A9B0)`
- 既有研究：`20_renderqueue_dispatch_layer_reverse`，但还需补 `MeshLayerDispatchRecord +0x40 之后的字段`、
  `Dispatch_Special` 的 fallback multipass 完整链。
- 输出：完整 dispatch 文档 + 字段更新

### 子线程 C：CSprite / CSpriteUber / 动画推进
- 范围：`CSpriteUber_PreRenderAndUpdatePosePalette_Full / Mini / Lite / MiniLite`、
  `CSprite::Update`、`CAnimComplex`、动画环形队列、attachment / linked sprites
- 关键已知（来自 18 文档）：`CSpriteUber_PreRender*` 4 个变体、`sub_6F12E900 EvalSingleGeoset`
- 重点：**dt gate 行为、pose stack 维护、ChildSprite recursion**
- 输出：CSprite 完整生命周期 + 动画推进时序

### 子线程 D：CModel pose palette / matrix-group / runtimeModel
- 范围：`CModel + 0x60` final pose array、`CGeosetData` matrix-group remap、
  `0x12FED0 AllocAndFillGroupPalette`、`0x12E600 BuildGroupBlendedPalette`、
  `0x12FDC0 CopyPoseMatrixRangeFromStack`、`0x12FF90 SimpleFallbackPalette`
- 既有研究：`22_cmodel_pose_palette_reverse`，但 22 文档主要是数据结构，
  **本轮要补 4 个 writer 的 CFG 和 dt gate 关系，并把 attachment 路径打通**
- 输出：Pose 数据流 + 4 writer 调用频率/触发条件 + RenderQueue 消费侧字段

### 子线程 E：CGeosetData 顶点 / index buffer / skinning
- 范围：`CGeosetData + matrix_group_sizes + matrix_indices + vertex_group_indices`、
  CPU skinning vs GPU skinning 切换、UV / normal / blend weight 的 stream layout、
  alpha test / alpha blend 处理
- 这是 v2/v3 之外尚未深挖的领域。Phase 7.55 v4 的 draw-time VB capture 之所以
  能 work，正因为 War3 用 CPU skinning，但具体 stride / vertex format 还需要文档化。
- 输出：vertex format + skinning 流程

### 子线程 F：TerrainShadow + FogMask grid type code 完整逆向
- 范围：`TerrainShadow_WriteMaskRegion` 的 `a3 typeCode` 16 bit 完整含义、
  `CFogMaskTable / CFogOfWarMap / CFogMask` 的 cell 结构、
  4 个并行 mask layer (`this+11/+12/+14/+15`) 的语义
- 已知（24 文档 v3）：`0x234710 / 0x234620 / 0x3DB260 / 0x233E90 / 0x232060 / 0x230210`
- 输出：**type code bit 含义表 + 建筑 footprint 对应 bit 的精确锁定方案**

### 子线程 G：Light pass / Shadow pass 着色器与 RT 绑定（GPU 侧）
- 范围：`d3d9_war3_shadow.cpp` 的接管点、shadow map render serial、CSM cascade、
  point light shadow、receiver pass、TAA history ping-pong
- 这部分项目自身代码已经接近完整，但**War3 原生的 receiver pass / shadow projection**
  还没有完整文档，要从 `0x76F060 TerrainShadow_Dispatch` 的 16 个 stage 倒推。
- 输出：War3 原生 shadow pipeline 全景 + 项目接管点对应表

### 子线程 H：D3D9 state block 与 sampler / texture / blend 桥接
- 范围：`gxApplyStateBlock(0x0E34B0)`、`gxCleanup74(0x0E3640)`、`gxCleanup78(0x0E3670)`、
  `GxDevice_BindPrimaryResource`、`GxStateBom`、`CGxMat`
- 这是渲染层最薄但最关键的一层（项目所有 hook 接管的地方都在这），需要完整文档化。
- 输出：D3D9 state 接管点 + Gx* 函数语义

### 子线程 I：UI / Frame / Sprite 渲染分支
- 范围：`uiRenderableRender(0x184F00)`、`uiDispatch(0x0CAA90)`、UI 相关 CSpriteFrame /
  CModelFrame / CBackdropFrame
- 输出：UI 渲染数据流（与世界渲染的边界）

### 子线程 J：粒子 / Ribbon / Effect 渲染
- 范围：`CParticleEmitter / CRibbonEmitter / CParticle / CPlaneParticleEmitter`、
  CEffect 各派生（28 个 Effect class）
- 输出：粒子 / 特效路径

## 3. 子线程任务卡片格式
每个子线程要给一个独立 markdown 文件作为输入，包括：
- 任务范围（哪些函数 / 类 / 数据结构）
- 已知锚点（地址 + 已知名字）
- 期望输出（论文章节 + IDA rename/comment 建议）
- 不要做的事（不动项目源码 / 不做 AutoTest）

## 4. 主线程在子线程跑期间做什么
1. 静态阴影 type code 锁定的**手工实验**：
   - 反编译 `WriteMaskRegion` 的 16 个分支，对照 4 个并行 mask layer 的写入位置；
   - 对照 `CFogOfWarMap_BuildVisibilityMask / UpdateVisibilityMask` 看哪些 bit 是 fog；
   - 对照 LOSBlocker / PathBlocker 已有研究看哪些 bit 是 path；
   - 剩下的就是建筑 footprint。
2. 收到子线程结果后批量回写 IDA。
3. 论文骨架先搭好。

## 5. 论文交付结构（按子线程聚合）
```
docs/plan/overnight_render_paper_2026_05_15/
  ├── OVERNIGHT_PLAN.md              ← 本文
  ├── 00_paper_master.md             ← 主线程拼装的论文主文件
  ├── 01_visibility_to_renderqueue.md  ← 子 A
  ├── 02_renderqueue_dispatch.md       ← 子 B
  ├── 03_csprite_animation.md          ← 子 C
  ├── 04_cmodel_pose_palette.md   ★★★ ← 子 D（重中之重）
  ├── 05_cgeoset_vertex_skinning.md    ← 子 E
  ├── 06_fogmask_static_shadow.md      ← 子 F（静态阴影主线）
  ├── 07_light_shadow_pass.md          ← 子 G
  ├── 08_d3d9_state_bridge.md          ← 子 H
  ├── 09_ui_rendering.md               ← 子 I
  ├── 10_particle_effect.md            ← 子 J
  └── ida_rename_batch.md              ← 主线程汇总的 IDA 命名清单
```

## 6. 时间线（粗略）
- 阶段 1（现在 ~ +30min）：写 10 个子线程任务卡片，启动子线程 A/B/C 并行
- 阶段 2（+30 ~ +90min）：A/B/C 收尾时，启动 D/E/F/G
- 阶段 3（+90 ~ +180min）：D/E/F/G 收尾时，启动 H/I/J，主线程开始静态阴影 type code 锁定
- 阶段 4（+180 ~ +300min）：所有子线程结果汇总，主线程批量 IDA 回写
- 阶段 5（+300 ~ +480min）：拼装论文 `00_paper_master.md`，最终 review

## 7. 不能做的事
- 不动 `src/` 下任何源码（项目正在跑性能优化）
- 不做 AutoTest（会跟性能优化线程争 War3 进程）
- 不主动启动 War3
- 不删除现有研究文档（只新增 / 增补）


---

## 11. 实际推进进度（2026-05-15 夜间无人值守）

### 11.1 已交付章节

| 章节 | 文件 | 行数 | 状态 |
|---|---|---|---|
| 第 4 章（重中之重）Pose | `04_cmodel_pose_palette.md` | ~990（含 §7 IDA 写回清单 + §8 总结） | ✅ 完整稿 |
| 第 6 章 静态阴影主线 | `06_fogmask_static_shadow.md` | ~600 | ✅ 完整稿 |

### 11.2 IDA 回写情况

| 批次 | 脚本 | 内容 | 结果 |
|---|---|---|---|
| 24 文档 v3 | `_ida_rename_comment.py` | CDoodads 5 个调度器 + ListA stamp 入口 | ok（已写回） |
| 第 4 章 Pose | `_ida_rename_comment_chapter4.py` | 24 处 rename + 13 条 set_comments（CSpriteUber dispatch / anim advance 三变体 / pose stack helpers / RenderQueue palette / sprite-runtime helpers） | **全部 ok=true** |
| 第 6 章 FogMask | `_ida_rename_comment_chapter6.py` | 41 处 rename + 14 条 set_comments（FogMask helpers / WriteMaskRegion fastpath / CWidget 30+ caller / CFogMask 字段语义） | **全部 ok=true** |

### 11.3 其它已落盘成果

- 320+ 个反编译产物落盘 `AutoTest/artifacts/_overnight_render_research/D_*.txt / F_*.txt`，
  下一轮章节继续展开时直接复用，不需重复 IDA 调用。
- 5 个章节任务卡（A/B/C/D/F）已完整落盘，子线程可直接接手。
- 24 号文档（CDoodads/CUnit/FogMask 静态阴影 v3）已是论文的事实型补充。

### 11.4 下一步推荐路径（给后续 agent / 用户）

按用户原始优先级排序：

1. **第 4 章 Pose 已交付，且 §7 IDA 回写已完成**——这是用户明确指定的"重中之重"。
2. **第 6 章 静态阴影治理蓝图已交付**——给主线程一份"可直接落地"的方案 A
   （hook `WriteMaskRegion` + `maskIdx == 3` 拦截）。建议主线程下一次有空闲窗口
   时按蓝图小步落地，先在 DebugView 加日志验证 idx 的实际值，再启用拦截。
3. **章节 A / B / C / E / G / H / I / J 仍是 task card stub**：
   - 任务卡已写好，反编译产物已落盘；
   - 下次有 token 时主线程可批量启动子线程 (`general-task-execution`) 一次性完成。
4. 第 4 章和第 6 章足够交付一份"以 Pose 与静态阴影为核心"的论文；如果今晚必须
   收尾，就以这两章为最终交付即可。


---

## 12. 第二批进度更新（2026-05-15 夜间无人值守 v2）

### 12.1 新增交付章节

| 章节 | 文件 | 行数 | 状态 |
|---|---|---|---|
| 第 1 章 剔除→渲染过渡 | `01_visibility_to_renderqueue.md` | ~750 | ✅ 完整稿 |
| 第 2 章 RenderQueue 完整数据流 | `02_renderqueue_dispatch.md` | ~1100 | ✅ 完整稿 |
| 第 3 章 CSprite 动画 | `03_csprite_animation.md` | ~750 | ✅ 完整稿 |

### 12.2 IDA 写回新增

| 批次 | 脚本 | 内容 | 结果 |
|---|---|---|---|
| 第 1/2/3 章合并 | `_ida_rename_comment_chapter_1_2_3.py` | 56 处 rename + 21 条 set_comments | **全部 ok=true** |

### 12.3 累计 IDA 写回

| 章节 | rename | set_comments | 状态 |
|---|---|---|---|
| 24 号文档（CDoodads / CUnit / FogMask v3） | ~10 | ~9 | 历史 |
| 第 1 章 剔除→渲染过渡 | 26 | 6 | ✅ |
| 第 2 章 RenderQueue 分发 | 23 | 13 | ✅ |
| 第 3 章 CSprite 动画 | 7 | 0 | ✅ |
| 第 4 章 Pose | 24 | 13 | ✅ |
| 第 6 章 静态阴影 | 41 | 14 | ✅ |
| **合计** | **~131** | **~55** | 全部 ok=true |

### 12.4 论文 v1 完成度

**已交付**：第 1 / 2 / 3 / 4 / 6 章（5 章），完整覆盖"逻辑层 → GPU draw call"
主链 + 静态阴影治理。论文 v1 可独立交付。

**剩余**：第 5 / 7 / 8 / 9 / 10 章（CGeoset 顶点 / Light pass / D3D9 state bridge /
UI 渲染 / 粒子 effect）— 反编译产物已落盘，下次有空闲窗口可批量启动子线程完成。

### 12.5 阅读建议

按 1 → 2 → 3 → 4 顺序读完整流程；单独读第 6 章看静态阴影治理。
所有章节都是**互相补充**而非"重叠"，连起来读最完整。
