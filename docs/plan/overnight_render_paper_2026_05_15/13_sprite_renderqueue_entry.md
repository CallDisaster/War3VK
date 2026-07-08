# 第 13 章 — CSpriteUber 4 变体 + WorldObjectEntry_Render + RenderQueue_AddBatch

> 本章补全三个核心函数的完整算法：
> 1. CSpriteUber_PreRender 4 个变体的精确差异
> 2. WorldObjectEntry_Render 完整流程
> 3. RenderQueue_AddBatch 完整流程

## 1. CSpriteUber_PreRender 4 变体对比

### 1.1 共同点

所有 4 个变体共享：
- **早退条件**：`this+32 == 0`（无 model）或 `this+40 & 0x10000`（skip-render 标志）→ return 0
- **dt gate**：末尾 `fabs(dt) >= 2 * FLT_EPSILON (0x34000000)` 时才调 `CModel_EvalPoseStackAndChildren`
- **dt < epsilon 时**：跳过整条 eval 链（0x12FED0/0x12E600/0x12FDC0 一次都不跑）

### 1.2 差异对比表

| 特性 | Full (0x182300) | Mini (0x1820C0) | MiniLite (0x1825E0) | FullLite (0x1826C0) |
|---|---|---|---|---|
| **参数数量** | 5 (this, dt, a3, a4, a5) | 5 (this, dt, a3, a4, a5) | 2 (this, dt) | 2 (this, dt) |
| **flags 0x20000 分支** | ✅ 有 | ✅ 有 | ✅ 有 | ✅ 有 |
| **flags 0x40000 分支** | ✅ 有 | ✅ 有 | ✅ 有 | ✅ 有 |
| **v9/a5 双向 pose 写回** | ✅ 有 | ❌ 无 | ❌ 无 | ❌ 无 |
| **pose stack 合成方式** | v20[3] + v21[3] + v22[3] | v15[3] 合成 | v7[9] 简化 | v8[36] 简化 |
| **子对象递归** | ✅ 有 | ❌ 无 | ❌ 无 | ❌ 无 |
| **使用场景** | 复杂模型（多子对象） | 简单模型（单 geoset） | 最简模型（无动画状态） | 简单模型+动画推进 |

### 1.3 Full 变体详细流程

```
CSpriteUber_PreRender_Full(this, dt, a3, a4, a5):
  1. if this+44 == 0xFFFE: sub_6F183A30()  // 初始化检查
  2. sub_6F18F030(dt)                       // 时间推进
  3. if this+32 == 0 || this+40 & 0x10000: return 0  // 早退
  4. sub_6F137170(this+100)                 // 状态更新
  5. 读 this+136/+144 构建 v18 (pose 基础矩阵)
  6. v7 = sub_6F139AE0(0)                   // 获取当前帧索引
  7. if this+40 & 0x20000:                  // SpriteSkip 模式
       sub_6F12EE90(this+32)                // AdvanceAnimSpriteSkip
     elif this+40 & 0x40000:                // ConstFlag 模式
       sub_6F12FAA0(this+32, dword_6FBE3D70) // AdvanceAnimByConstFlag
     else:                                  // 标准 dt 模式
       sub_6F12EF70(this+32, dt*1000)       // AdvanceAnimWithDeltaMs
  8. 构建 pose stack (v20/v21/v22)
  9. dt gate: if fabs(dt) >= 2*FLT_EPSILON:
       CModel_EvalPoseStackAndChildren(this+32, pose_stack)
```

### 1.4 MiniLite 变体（最简）

```
CSpriteUber_PreRender_MiniLite(this, dt):
  1. if this+44 == 0xFFFE: sub_6F183A30()
  2. sub_6F18F030(dt)
  3. if this+32 == 0 || this+40 & 0x10000: return 0
  4. if this+40 & 0x20000: sub_6F12EE90(this+32)
     elif this+40 & 0x40000: sub_6F12FAA0(this+32, ...)
     else: sub_6F12EF70(this+32, dt*1000)
  5. sub_6F137170(v7, this+100)
  6. 读 this+136/+144 构建 v8
  7. dt gate: if fabs(dt) >= 2*FLT_EPSILON:
       CModel_EvalPoseStackAndChildren(this+32, v7)
```

**关键差异**：MiniLite 没有 flags 0x20000/0x40000 的 SpriteSkip/ConstFlag 分支，
直接按 dt 推进动画。它用于最简单的模型（无复杂动画状态机）。

### 1.5 dt gate 的精确语义

```c
float dt_abs = fabs(dt - 0.0f);
if (dt_abs >= 0.00000023841858f) {  // 2 * FLT_EPSILON
    CModel_EvalPoseStackAndChildren(modelPtr, poseStack);
}
```

- `dt == 0`：完全跳过（logic tick 没跑）
- `|dt| < 2*FLT_EPSILON`：视为浮点噪声，跳过
- `|dt| >= 2*FLT_EPSILON`：正常执行 pose eval

Phase 7.47 实测：dt>0 占 98.79%，dt==0 仅 1.21% 且集中在进图前两帧。

## 2. WorldObjectEntry_Render 完整流程

### 2.1 `WorldObjectEntry_Render (0x6F184EE0)`

```c
int WorldObjectEntry_Render(int world, int entry) {
    CSpriteUber* sprite = GetSpriteFromEntry(entry);

    if (sprite->modelPtr) {  // sprite+8 != 0
        // 调用 sprite 的 vtable[5]（PreRender）
        sprite->vtable[5](sprite);

        // 如果 sprite 有 sceneNode，进入 RenderQueue
        if (sprite->sceneNode) {
            RenderQueue_AddBatch(world, entry);
        }
    }
    return result;
}
```

**关键洞察**：
1. 先调 `vtable[5]` 做 PreRender（即 CSpriteUber_PreRender 的某个变体）
2. 只有当 sprite 有 `sceneNode` 时才进入 `RenderQueue_AddBatch`
3. `sceneNode` 是 sprite 进入渲染管线的"门票"

## 3. RenderQueue_AddBatch 完整流程

### 3.1 `RenderQueue_AddBatch (0x6F139190)`

```c
void RenderQueue_AddBatch(CRenderQueue* rq, int sceneNode) {
    int renderableCount = sceneNode->renderableCount;  // +156

    // 步骤 1：提交当前批次
    RenderBatch_Submit(rq);

    // 步骤 2：如果 sceneNode 有透明标志
    if (sceneNode->flags & 0x10) {
        // 添加到 4 个透明列表
        SceneNode_AddTransparentList0(sceneNode, renderableCount);
        SceneNode_AddTransparentList2(sceneNode, renderableCount);
        SceneNode_AddTransparentList3(sceneNode, renderableCount);
        SceneNode_AddTransparentList4(sceneNode);

        // 步骤 3：递归子节点
        DWORD childCount = sceneNode->childCount;  // +196
        if (childCount) {
            int* childPtr = sceneNode->childArray + 8;  // +200+8

            for (DWORD i = 0; i < childCount; i++) {
                // 检查子节点可见性缓存
                if (!sceneNode->visibilityCache ||
                    CheckVisibility(sceneNode->visibilityCache, i)) {

                    int childNode = *childPtr;
                    if (childNode > 0) {
                        // 递归调用 AddBatch
                        do {
                            RenderQueue_AddBatch(rq, childNode);
                            childNode = *(DWORD*)(childNode + 4);
                        } while (childNode > 0);
                    }
                }
                childPtr += 3;
            }
        }
    }
}
```

**关键洞察**：
1. 先 `RenderBatch_Submit` 提交当前批次
2. 如果有透明标志（`flags & 0x10`），添加到 4 个透明列表
3. 递归处理子节点，每个子节点可能有多个 sibling（链表结构）
4. 子节点可见性通过缓存表检查（`visibilityCache + childIndex`）

### 3.2 SceneNode 的 4 个透明列表

| 列表 | 函数 | 用途 |
|---|---|---|
| List0 | `SceneNode_AddTransparentList0` | 基础透明 |
| List2 | `SceneNode_AddTransparentList2` | 附加透明 |
| List3 | `SceneNode_AddTransparentList3` | 特殊透明 |
| List4 | `SceneNode_AddTransparentList4` | 全局透明 |

## 4. IDA rename 清单（本章新增）

| 原名 | 新名 | 地址 |
|---|---|---|
| `sub_6F182300` | `CSpriteUber_PreRenderAndUpdatePosePalette_Full` | `0x6F182300` |
| `sub_6F1820C0` | `CSpriteUber_PreRenderAndUpdatePosePalette_Mini` | `0x6F1820C0` |
| `sub_6F1825E0` | `CSpriteUber_PreRenderAndUpdatePosePalette_MiniLite` | `0x6F1825E0` |
| `sub_6F1826C0` | `CSpriteUber_PreRenderAndUpdatePosePalette_FullLite` | `0x6F1826C0` |
| `sub_6F184EE0` | `WorldObjectEntry_Render` | `0x6F184EE0` |
| `sub_6F139190` | `RenderQueue_AddBatch` | `0x6F139190` |
| `sub_6F1375C0` | `RenderBatch_Submit` | `0x6F1375C0` |
| `sub_6F1380A0` | `RenderQueue_FlushSortedItems` | `0x6F1380A0` |
| `sub_6F13A5E0` | `RenderQueue_Dispatch_Common` | `0x6F13A5E0` |
| `sub_6F13A780` | `RenderQueue_Dispatch_Special` | `0x6F13A780` |
| `sub_6F13A510` | `RenderQueue_UpdateItemWorldMatrix` | `0x6F13A510` |
| `sub_6F138F70` | `RenderQueue_ApplyDrawStateAndDraw` | `0x6F138F70` |
| `sub_6F138EE0` | `RenderQueue_ApplyDrawStateAndSamplerPair` | `0x6F138EE0` |
| `sub_6F13ABA0` | `RenderQueue_ApplyLayerColorAlphaRegisters` | `0x6F13ABA0` |
| `sub_6F137BD0` | `RenderQueue_ComposeLayerTintAndAlpha` | `0x6F137BD0` |
| `sub_6F13AC00` | `RenderQueue_ApplyTextureStageMode` | `0x6F13AC00` |
| `sub_6F13AC70` | `RenderQueue_IsSpecialBatchStateConsistent` | `0x6F13AC70` |

---

*本章约 400 行。*
