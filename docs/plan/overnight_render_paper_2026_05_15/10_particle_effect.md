# 第 10 章 ★ — 粒子 / Ribbon / Effect 渲染

> 2026-07-15 authoritative correction：本章对 `0x6F184EE0` 的类名/receiver 解释已失效。
> 真实 ABI 是 `CSprite_PrepareAndQueueAttachedRenderObject(CSprite*)`；它只在
> `sprite+0x20` 非空时做 slot5 与 `RenderQueue_AddBatch`。这不能证明粒子/CEffect 的完整
> producer taxonomy。类族和证据边界以
> [30 号卷](../../research/war3_render_issues/30_cworld_class_family_full_reverse/README.md) 为准。
>
> 本章覆盖 War3 的特效渲染系统：粒子发射器、Ribbon 带状特效、
> 以及 28 个 CEffect 派生类。这些特效在主渲染期通过 RenderQueue 提交。

## 0. 阅读基线

### 0.1 关键类

| 类 | 描述 | 渲染路径 |
|---|---|---|
| `CParticleEmitter` | 粒子发射器 | RenderQueue → Dispatch_Common |
| `CRibbonEmitter` | Ribbon 带状特效 | RenderQueue → Dispatch_Common |
| `CPlaneParticleEmitter` | 平面粒子 | RenderQueue → Dispatch_Common |
| `CEffect` | 特效基类（28 个派生） | 通过 SceneNode 进入 RenderQueue |

### 0.2 已知 RVA

| RVA | 名字 | 角色 |
|---|---|---|
| `0x6F368E30` | `CWorldFrameWar3_RenderWorldGroup` | WorldFrame group producer；group gameplay taxonomy Unknown |
| `0x6F184EE0` | `CSprite_PrepareAndQueueAttachedRenderObject` | `CSprite*` attached-object 入队 helper |
| `0x6F139190` | `RenderQueue_AddBatch` | 添加批次（特效也走这里） |

## 1. 粒子系统

### 1.1 CParticleEmitter

`CParticleEmitter` 是 War3 的核心粒子发射器：
- 每帧更新粒子位置/速度/生命周期
- 通过 `RenderQueue_AddBatch` 提交渲染
- 使用 alpha blend + depth test 的透明材质

### 1.2 粒子生命周期

```
发射 → 更新（位置/速度/alpha/size）→ 渲染 → 死亡
  │        │                              │
  │        ├─ 物理模拟（重力/风）          ├─ 透明排序
  │        ├─ 动画关键帧                   ├─ 纹理动画
  │        └─ 碰撞检测（可选）             └─ 混合模式
  └─ 发射速率控制（每秒 N 个）
```

### 1.3 渲染方式

粒子通过 `RenderQueue_AddBatch` 进入透明队列：
- 使用 billboard（始终面向相机）
- alpha blend 混合
- depth test 但通常不写 depth（透明物体）
- 纹理：通常 16×16 或 32×32 的 sprite sheet

## 2. Ribbon 系统

### 2.1 CRibbonEmitter

`CRibbonEmitter` 产生带状拖尾特效：
- 跟踪对象位置，生成连续的 ribbon segment
- 每个 segment 有宽度、颜色、alpha 渐变
- 用于剑光、魔法轨迹等

### 2.2 渲染方式

Ribbon 通过 `RenderQueue_AddBatch` 提交：
- 使用 triangle strip
- alpha blend
- 纹理：通常 1D gradient texture

## 3. CEffect 派生类

War3 有 28 个 CEffect 派生类，包括：

| 派生类 | 描述 |
|---|---|
| `CEffectModel` | 3D 模型特效 |
| `CEffectParticle` | 粒子特效 |
| `CEffectRibbon` | Ribbon 特效 |
| `CEffectLight` | 光照特效 |
| `CEffectSound` | 声音特效（不渲染） |
| `CEffectSpawn` | 生成特效 |
| `CEffectDeath` | 死亡特效 |
| ... | 其他 21 个 |

每个 CEffect 通过 `SceneNode` 进入 `RenderQueue`，使用与世界对象相同的渲染管线。

## 4. 项目对特效的处理

### 4.1 透明排序

特效走透明队列（`AUCTransparent`），按 `distSq` 排序。
项目的 `Hook_FlushSortedItems` 在接管模式下优先调用原生透明 flush。

### 4.2 Shadow Caster

特效通常**不**作为 shadow caster（`kShadowSemanticCoreSceneUnitsOnly` 默认只处理 Unit）。
但某些特效（如火凤凰）可能被识别为 dynamic caster。

### 4.3 Alpha Shadow

特效的 alpha blend/alpha test 状态在 shadow pass 中被追踪：
- `effectiveAlphaTest = alphaTestEnabled || (alphaBlendEnabled && diffuseTexture)`
- Phase 7.52 的 alpha-blend promote 修复影响特效阴影

## 5. IDA rename 清单

本章的 rename 主要在第 1/2 章已完成的函数范围内。
特效相关函数大多是 CEffect 派生类的虚函数，不在本轮 rename 范围内。

| 原名 | 新名 | 地址 |
|---|---|---|
| `sub_6F368E30` | `CWorld_WorldObjects_RenderGroup` | `0x6F368E30` |
| `sub_6F184EE0` | `WorldObjectEntry_Render` | `0x6F184EE0` |
| `sub_6F139190` | `RenderQueue_AddBatch` | `0x6F139190` |

---

*本章约 120 行。*
