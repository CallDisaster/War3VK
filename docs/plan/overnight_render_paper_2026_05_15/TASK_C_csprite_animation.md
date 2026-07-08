# 子线程 C 任务卡 — CSprite / 动画推进 / 特效挂点

## 任务定位
渲染论文第 3 章。覆盖动画系统从"动画 tick"到"pose stack 准备"的完整路径。
**这是 Pose 章节（第 4 章）的前置——必须先把 CSprite 动画推进搞清楚才能讲 Pose**。

## 已知锚点（IDA 已命名）
| 地址 | 名字 |
|---|---|
| `0x6F182300` | `CSpriteUber_PreRenderAndUpdatePosePalette_Full` |
| `0x6F1820C0` | `CSpriteUber_PreRenderAndUpdatePosePalette_Mini` |
| `0x6F1825E0` | `CSpriteUber_PreRenderAndUpdatePosePalette_MiniLite` |
| `0x6F1826C0` | `CSpriteUber_PreRenderAndUpdatePosePalette_FullLite` |
| `0x6F12E900` | `CModel_EvalSingleGeosetAndRecurseChildren` |

## 已有研究（增补，不重写）
- `docs/research/war3_render_issues/18_csprite_animation_attach_reverse/README.md`
- AGENTS.md 第 78 条（Phase 7.47 dt gate 反证）

## 必须搞清楚的问题
1. **`CSpriteUber_PreRender*` 4 个变体的差异**：
   - Full vs Mini vs Lite vs MiniLite，分别在什么场景被调？
   - 它们的 dt 参数从哪里来？为什么 Phase 7.47 数据显示 98.79% 帧 dt > 0？
2. **dt gate 的意义**：`if (fabs(dt) >= FLT_EPSILON) CModel_EvalPoseStackAndChildren(...)`
   - dt = 0 时 producer 不跑，但视觉为什么没冻？（Phase 7.47 已证明跟 0.5s 卡顿无关，需要论文化）
3. **CSprite 的 vtable**：每个 sprite 类型（CSprite / CSpriteUber / CSpriteMini）的 vtable 结构
4. **CWidget + 0x28 → CSprite\***：这条 alias 的内存布局是怎么样的？
5. **动画环形队列**：`CSprite + ?` 偏移，多少帧的动画历史？
6. **CEffect / Attachment 挂点**：CSpriteUber 怎么挂 attachment effect？
7. **child sprites 递归**：`CModel_EvalSingleGeosetAndRecurseChildren` 怎么递归子节点？
8. **CAnimComplex**（如果存在）：复合动画混合的实现

## 输出格式
写到 `docs/plan/overnight_render_paper_2026_05_15/03_csprite_animation.md`：

```
# 第 3 章 — CSprite 动画系统

## 3.1 CSprite 类层次
## 3.2 CSpriteUber 4 个变体的差异（Full/Mini/Lite/MiniLite）
## 3.3 dt gate 与 pose stack 触发
## 3.4 动画环形队列与时间推进
## 3.5 child sprite 递归
## 3.6 attachment / linked sprites
## 3.7 与 CWidget / CUnit 的关系（+0x28 alias）
## 3.8 CEffect 各派生在动画系统中的作用
## 3.9 IDA rename / set_comments 建议
```

## 工具
- IDA MCP
- `AutoTest/_ida_call.py` 等 helper
- 项目代码：`src/d3d9/war3/model/war3_model_hook.{h,cpp}` —— **只读**

## 不能做的事
同前

## 完成条件
- 文档至少 800 行
- 4 个 PreRender 变体的 CFG 必须画出来
- dt gate 行为必须有 IDA 反编译证据
- 至少 25 处新命名建议
