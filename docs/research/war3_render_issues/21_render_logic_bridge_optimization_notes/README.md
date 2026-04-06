# 渲染层到逻辑层桥接降本笔记

## 目标

在渲染层尽可能低成本地知道“当前正在渲染的是游戏里的哪个对象”，避免每次都走：

1. `renderablePart -> sceneNode`
2. `sceneNode -> RenderObjectRegistry`
3. 必要时再回退到 `unitPtr -> handle` 或 `handle -> rawcode`

这份笔记只记录当前代码树和逆向结果已经确认的事实，不在这里直接给最终改造结论。

## 当前桥接主链

### 1. 收集阶段

`Hook_WorldObjects_RenderGroup` 在调用原版 `WorldObjects_RenderGroup` 前，会先执行：

1. `War3Renderer::OnWorldObjectsGroup`
2. `SceneCollector::CollectWorldObjects`
3. `RenderObjectRegistry::registerWorldObjectsBatch`

这里已经能拿到：

1. `WorldObjectListEntry + 0x00 = WorldObjectEntry*`
2. `WorldObjectListEntry + 0x14 = handle / unit 通道`
3. `WorldObjectEntry + 0x20 = sceneNode`

所以当前 registry 发布的核心快照是：

1. `byEntry[worldObjectEntry] = RenderObjectInfo`
2. `sceneToInfo[sceneNode] = RenderObjectInfo*`
3. `handleToInfo[jHandle] = RenderObjectInfo*`

### 2. dispatch 热路径

`ExecBatchProcessor::Begin` 当前的对象识别优先级是：

1. `War3RenderState::GetTlsDispatchHandle()`
2. `RenderQueueTracker::GetCachedObjectInfo(element, sceneNode)`
3. `ExecBatchProcessor::GetCurrentBatchEntry() -> registry.findByEntry`
4. `registry.findBySceneNode(sceneNode)`
5. `g_unitToHandleId.find(unitPtr)` 兜底

也就是说，项目设计上已经留了三条“非扫表直桥”：

1. `TLS dispatch handle`
2. `TLS current batch entry`
3. `worldObjectEntry -> registry.byEntry`

## 当前最贵的路径

### 1. `HandleResolver::findHandleByUnitPtr`

这是目前确认最贵的真扫表路径。

特征：

1. 线性扫 handle table
2. 最多扫 `50000` 项
3. 每项都要做可读性检查和 `agent->unitPtr` 比对

这条路径现在只该保留为极低频 fallback，不能当热路径主方案。

### 2. `ExecBatchProcessor::UpdateUnitCache`

这条路径会整帧扫描 handle array，并构建：

1. `unitPtr -> handleId`
2. `unitPtr -> agentPtr`

当前 `SceneCollector` 里已经明确把这条“每帧大扫表”调用注释掉了，理由就是它会带来明显 CPU 压力。

### 3. `SceneCollector` 的 tracked-handle 反查

当开启 tracked-handle 过滤时，`SceneCollector` 还会做一次：

1. `handleId -> resolveHandle`
2. `handle -> agent/unit ptr`
3. 指针集合 `sort/unique`

这条链比 `findHandleByUnitPtr` 轻，但仍然不是“免费”。

## 已经埋好的低损耗入口

### 1. `War3RenderState::SetTlsDispatchHandle`

`ExecBatchProcessor::Begin` 会优先读取它，但当前代码树里只有清零点，没有真正的非零写入点。

现状：

1. 入口存在
2. 消费者存在
3. 生产者缺失

### 2. `ExecBatchProcessor::SetCurrentBatchEntry`

`Begin()` 已经支持：

1. 取 `TLS current batch entry`
2. 直接 `registry.findByEntry(worldObjectEntry)`
3. 跳过 `sceneNode -> registry` 的倒查

但当前代码树里同样没有找到实际 setter 调用点。

### 3. `War3Renderer::SetCurrentWorldObjectContext`

这条 TLS 也已经有定义：

1. `worldObjectEntry`
2. `sceneNode`

并且 `Hook_SceneSubmitBatch` 已经会检测“当前是否位于某个 world object 的上下文里”。  
但目前同样没真正接线。

### 4. native 侧天然插点

从原生渲染链视角，最自然的直桥位置其实就是：

1. `WorldObjectEntry_Render`
2. `RenderQueue_AddBatch(sceneNode)`

因为这一跳已经同时拥有：

1. `worldObjectEntry`
2. `sceneNode`
3. 对象仍处在当前调用栈里

但当前仓库里的 `war3_native_hooks.cpp` 只真正安装了：

1. `CWorldFrameWar3::RenderScene`

而下面这些 hook 虽然有实现和地址，但当前没有被正式启用：

1. `WorldObjectEntry_Render`
2. `RenderQueue_AddBatch`
3. `RenderBatch_Submit`

## 从当前逆向结果看，最值得优先追的方向

### 方向 A：把 `worldObjectEntry` 直接带到 dispatch

最有希望替代 `sceneNode -> registry` 反查的方案，是让 `dispatch` 直接拿到：

1. `worldObjectEntry`
2. 或 `RenderObjectInfo*`

这样可以直接命中：

1. `registry.findByEntry(worldObjectEntry)`
2. 或完全不查 registry，直接吃缓存对象

### 方向 B：避免重新做 `unitPtr -> handle` 反推

当前更值得坚持的是：

1. `handle -> rawcode` 缓存
2. `worldObjectEntry/sceneNode -> RenderObjectInfo`

而不是回到：

1. `unitPtr -> handleId` 线性扫描
2. 每帧重建全局 `unitPtr -> handle` 大表

### 方向 C：如果要做 intrusive bridge，优先挂在“已确认稳定的 key”上

当前已确认最稳定的对象 key 是：

1. `worldObjectEntry`
2. `sceneNode`
3. `jHandle`

而不是直接改写当前仍未完全确认的批次内部未知槽位。

## 当前阶段的保守结论

1. 项目其实已经设计过“低损耗直桥”，但目前还处在“消费者已经写好、生产者没真正接上”的状态。
2. 真正应该优先消灭的，是所有会掉进 `findHandleByUnitPtr` 或全表缓存重建的路径。
3. 从原生渲染链看，`WorldObjectEntry_Render -> RenderQueue_AddBatch(sceneNode)` 是最自然的低损耗桥接点。
4. 如果后续要接管渲染层，这部分最好和 `RenderQueueTracker` / `ExecBatchProcessor` 一起改，不要只修一侧。
