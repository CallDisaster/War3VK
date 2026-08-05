# Automation Mission

Date: 2026-04-18

## Final Goal

把当前项目推进到下面这个状态：

1. 物体阴影不再依赖旧的 `VB/IB snapshot/freeze/capture`
2. 阴影语义完全来自 `Game.dll` 上层数据
3. 每帧都能明确知道“本轮到底要渲染哪些对象”
4. 渲染层能根据对象身份和 geoset/model key 命中自己的显存缓存
5. 对单位、建筑、可破坏物、doodad 的阴影都能正确绘制
6. 蒙皮单位不能再退回“只有静态块影子/错误方块阴影/撕裂阴影”
7. 先在 DXVK 宿主内验证整条路线，再把同一套渲染核心迁到晚注入 / native D3D9 路线
8. 新路径最终不依赖 DX9Ex

## Expected End State

最终希望形成的架构是：

`Game.dll upper-layer semantic data`
-> `ShadowFrameManifest`
-> `ShadowModelResourceStore`
-> `ShadowPoseStore`
-> `ShadowRendererCore`
-> `backend`
-> `shadow map rendering`

其中：

1. `ShadowRendererCore` 必须独立于 DXVK 业务逻辑
2. DXVK 只能作为验证宿主，不再作为语义来源
3. `late injection + native D3D9 backend` 必须能复用同一份 core

## Hard Acceptance Criteria

只有同时满足下面三条，才算阶段性完成：

1. `AutoTest` 可以稳定后台驱动项目，不依赖前台人工观察
2. object shadow 主路径完全脱离旧 `VB/IB snapshot/freeze`
3. 单位阴影视觉正确，不能再是撕裂、方块、错位、静止影子

## Non-Negotiable Constraints

1. 不允许依赖用户反复手工前台测试才能推进
2. 验证必须优先走隔离桌面 / 后台 AutoTest / control plane
3. 不能把“看见某种阴影”误判为完成，必须确认阴影形状和姿态都正确
4. 旧路径可以暂时保留在仓库，但不得继续作为默认正确性来源
5. 若某条新路线被后台验证证明无效，必须在状态页明确写明，不得反复重复试错

## Current Real Problem Statement

到 2026-04-18 这一轮，项目已经不再停留在“研究阶段”，而是进入了：

1. `control plane` 已完成
2. `shadow runtime contract` 已完成
3. `ShadowRendererCore` 已真实运行
4. `units-first semantic scene submission` 已真实提交到宿主 scene

但是当前还没有完成，因为：

1. 单位阴影虽然已经开始出现，但仍然是错误形态
2. 当前单位阴影表现为撕裂或方块状阴影
3. 主线程 CPU 仍然极高

## Most Likely Technical Direction

当前最可能的根因不是“看不到单位”，而是“单位当前帧动态几何 contract 还没被正确还原”。

更准确地说：

1. 现在已经能从 `meshData` 拿到当前帧 positions
2. 也已经能把单位语义阴影提交到 scene
3. 但当前单位阴影形状错误，说明仅靠 `source geoset + current positions` 还不够
4. 真正缺的更可能是 `meshData` 侧的完整 draw-time contract：
   - 正确的当前帧 index/topology
   - 正确的 auxiliary stream / binding
   - 正确的 skinned dynamic geometry 语义

因此后续自动化提示不能只要求“继续优化性能”，而必须先明确：

1. 修正单位 shadow mesh contract
2. 再在正确形态基础上继续压性能

## Immediate Priorities

1. 确认单位阴影为什么呈现为撕裂/方块
2. 继续逆向并落实 `meshData dynamic geometry / auxiliary stream / index contract`
3. 把单位阴影从“可见但错误”推进到“视觉正确”
4. 在此基础上继续削掉残余 fallback 与高 CPU 路径

## Test Discipline

每一轮自动化验证至少要记录：

1. 是否使用隔离桌面
2. 最新 `get_shadow_runtime_summary`
3. 最新 `get_frame_manifest_summary`
4. 若进行了截图，截图路径
5. 最新 perf report 路径
6. 当前结论是：
   - 功能前进
   - 性能前进
   - 路线无效
   - 仍需继续
