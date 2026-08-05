# 04. VisibleRenderable / ShadowManifest / Core Set / PartLease 生命周期

这一篇讲 **消费端**。也就是 Phase 7.20–7.30 这么多轮重构一直在改的那一层。读完你会明白：
- `ShadowManifestObjectKey` vs `ShadowManifestPartKey` 有什么区别
- core set / committed core / phantom shrink 在修什么
- `DirectPartPacketLease` 的 TTL 为什么 3 帧 / 6 帧来回调
- 为什么这整层“都修对了”也不能解决 palette 数据错的问题

## 4.1 从数据流角度定位

```mermaid
flowchart LR
  A["PublishCurrentDrawContract<br/>(per draw call)"] -->|"record"| B["VisibleRenderableRegistry<br/>(per frame 合并)"]
  B -->|"manifestObjectKey<br/>+ manifestPartKey"| C["SemanticShadowManifest<br/>(对象→parts 的索引)"]
  C --> D["ManifestObjectCoreSet<br/>(本对象必需的 part 集合)"]
  D --> E["DirectPartPacketLease<br/>(per part 的 TTL 租约)"]
  E --> F["ShadowPacket submit"]

  style A fill:#eef
  style F fill:#efe
```

## 4.2 关键概念表

| 名字 | 一句话定义 | 作用 |
|---|---|---|
| `manifestObjectKey` | 对象稳定 ID | 同一个英雄跨帧稳定，即使 renderablePart 指针被回收重分配 |
| `manifestPartKey` | 对象内一个 part 的稳定 ID | 英雄的盾、披风、头盔 分别有各自的 partKey |
| ManifestObjectCoreSet.committedPartKeys | 这个对象**必须都在场**的 part 集合 | 缺件就不能 submit，防止出现“有身体没头”的阴影 |
| DirectPartPacketLease | 针对一个 partKey 的租约 | 即使这一帧没收到 live packet，也能用上次提交的 packet 顶一会 |
| coreExtendedFrames | core part 的 lease TTL | 当前默认 = `manifestGeometryCacheFrames * 2u` = 6 帧 |
| phantom shrink | 当 core epoch 触发时从 committed 剔除**本帧不在 live、不在 lease** 的部件 | 防止“幻部件”卡住 core gate 永远不完整 |
| stale pose restore | 允许 core part 在 pose 不新鲜时仍复用 lease | 用户看到“阴影停一下再追上” |

## 4.3 ShadowManifestObjectKey 的识别路径

看 `war3_visible_renderables.cpp` 的 `computeShadowManifestObjectKey`：

```
尝试顺序：
1. jHandle    (jass handle)        → 最稳定
2. unitPtr    (CUnit*)             → 单位稳定
3. worldObjectEntry                → 容器稳定
4. sceneNode                       → 最弱，指针可能回收
```

任何一个能取到的就用它。现在 **unit / destructible / doodad 都走同一条 key 路径**——也就是说 destructible（大门）和 unit（英雄）在 key 生成层面没有本质区分。

## 4.4 ShadowManifestPartKey 的字段

当前版本：
```cpp
hash = 0x72110100
hash = mix(objectKey)
hash = mix(record.layerIndex)
hash = mix(record.payloadWord108)
```

注意 **没有 `payloadWord11C`**。这是 Iter C 撤回的结果：
- Iter C 把 `payloadWord11C` 加进 key 之后，大门 variant 切换（closed/opening/opened）时 partKey 会变，manifest 认为“旧 part 消失、新 part 出现”，闪烁会被“重建”而不是“lease 垫住”
- 但 benchmark 场景（20K+ skinned）FPS 从 100 崩到 3.7，原因是 `ShadowManifestPartKey` 的 hash 不同导致 part 数量翻 N 倍，下游有个 `noteShadowManifestPartGoodPacket` 走 O(N²) 路径

所以 Iter H 回退了这个决定。代价：**destructible 大门现在用和单位一样的 partKey，11C 变化时 lease 仍然活着，导致整 part 身份漂移 → 闪得特别厉害**。

## 4.5 Core Set 和 Phantom Shrink

### 什么是 core set

同一对象不同帧里看到的 part 不完全一样。core set 是 **一段时间内持续观察到的“关键 parts”**。只有 core set 满足时，才认为这个对象可以 submit。

### 为什么要 phantom shrink

假设英雄有 8 个 part：头、身、盾、剑、披风、光环、特效 A、特效 B。

场景：
- 前 10 帧都看到 8 个 part
- 第 11 帧特效 B 没出现（技能冷却）
- 第 12 帧特效 B 还是没出现

如果我们永远要 committed core = 8 part，那第 11 帧之后每帧都是 “incomplete”，整个对象被跳过 submit → 阴影整体消失。

Phantom shrink：**当 core epoch 触发时，扫描 committed，把“本帧不在 live 也不在 lease 表”的部件从 committed 中移除**。这样下次核对 core 时，committed 已经缩到 7 了，不再要求特效 B。

这是 Phase 7.30 第一刀的核心。

## 4.6 coreExtendedFrames = manifestGeometryCacheFrames * 2

看 [d3d9_device.cpp 11352–11387](../../../src/d3d9/d3d9_device.cpp)：

```cpp
const uint64_t coreExtendedFrames = manifestGeometryCacheFrames * 2u;
// ...
const uint64_t effectiveExpiryFrames =
    isCommittedCorePartKey(it->second.selectionKey, it->first)
        ? coreExtendedFrames           // core part → 6 帧
        : manifestGeometryCacheFrames; // non-core → 3 帧
```

意思：**被 committed 引用的 part lease 生存期是 non-core 的两倍**。目的是防止 core gate 正在要求这个 part 时 lease 又刚好过期。

Claude 在 Phase 7.31 Iter B/H 反复调这个值：
- Iter B 把 stale restore 关了（等同于不再用 lease 顶 live 缺帧）
- 结果英雄/凤凰单位阴影整体消失，比 stutter-catchup 更糟
- Iter H 恢复 stale on + TTL 6 帧（当前状态）

结论：**这一层的 TTL / stale / phantom 已经被调到接近最优**，AutoTest `submittedObjectJaccardMean=999.12` 就是证据——每一帧的 submit 对象集合 99.9% 稳定。但**视觉上仍然不对**，说明问题不在这一层。

## 4.7 为什么 Jaccard=999 和视觉"无阴影"可以同时成立

这是最核心的认知突破。

- Jaccard 只衡量“我们提交了哪些对象/parts 到阴影 pass”
- 视觉只关心“这些提交的 silhouette 是不是画在了正确位置”

如果 palette 里的矩阵把顶点算到视野外 / 压成一个点 / 超出 cascade 范围：
- submit count 照常 +1
- Jaccard 照常稳定
- shadow map 上**没有写入任何像素**
- receiver 采样结果永远是 "no shadow"
- 屏幕上 → 阴影消失

这就解释了用户截图“只有建筑、树、花草装饰有阴影，英雄/火凤凰/紫色单位几乎看不到阴影”。建筑和树大部分是 **rigid**（骨骼数量为 1 或走 simple fallback），palette 哪怕走 raw arena 也基本对得上。skinned 单位（英雄、凤凰）才是 palette 坏的重灾区。

## 4.8 三轮修复都没解决的根本原因

| 轮次 | 修的层 | 修对了没 | 为什么不解决问题 |
|---|---|---|---|
| Phase 7.20–7.25 | Manifest / Object key | 对 | 提交对象身份稳 99%，但 palette 错 |
| Phase 7.26–7.29 | DirectPartPacketLease TTL | 对 | 保住对象不消失，但 palette 还是错 |
| Phase 7.30 | Phantom shrink + core TTL wide | 对 | Jaccard 从 90 抬到 999，**仍然不解决 palette 错** |
| Phase 7.31 Iter A–H | 各种 TTL/stale restore A/B | 大多撤回 | 已经在错误数据上修表面，调哪个都是表面 |

**所有这些 Phase 都没动 palette 捕获这一层**。直到 Phase 7.31 P0 （batch capture）才第一次真正改了 palette 的生产捕获。

但 P0 只做了一半：
- 写入端（hook）已经按 groupCount 批量写 trusted cache
- 消费端（publish）的 trusted/raw 分支还没加 provenance
- 另外可能有其他 writer（0x12FF90 simple path）没 hook

下一篇把三轮重构为什么集体走偏的原因按时间线梳理清楚。
