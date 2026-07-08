# 更新日志

## WarVK 静态阴影解决版 - 2026-07-08

### 重点更新

- 解决 Warcraft III 原生建筑静态阴影残留问题：默认在 `CUnitUIManager_RecordSetStructureShadow` 写入 `buildingShadow(+0x50)` 时阻断阴影文件名进入 UnitUI 类型记录。
- 保留并默认移除 `TerrainShadow_RenderListB` 旧版单位黑色圆影，让画面不再叠加原版 blob 阴影。
- 退役 `WriteMaskRegion / StaticStampPath / RegisterImage / DoodadStamp` 等历史静态阴影实验默认路径，保留为证伪资料和专项诊断入口。
- 整理静态阴影研究文档，明确 `CUnit+0x50` 不是阴影字符串，真正生产点是 UnitUI type record `+0x50 = buildingShadow`。

### 验证结果

- 实机日志确认 `DXVK War3Hook: CUnitUI buildingShadow BLOCK calls=768 blocked=768 mode=0 ... name=ShadowTreeofLife`。
- 用户实机确认建筑阴影完全不可见。
- `ninja -C build32` 通过。

## v1.1.0 - 2026-04-05

### 重点更新

- 修复了路径阻断器被错误渲染进魔兽争霸3场景的问题，避免非目标几何污染主渲染与阴影链路。
- 内置整合 `StormBreaker`，为项目提供更稳定的内存管理与运行时基础能力。
- 更新 `GPU Arena`，继续推进阴影捕获、几何预算与运行时资源管理。
- 接入第一阶段缓存机制，目前只对静态模型启用；动态单位、飞行单位、蒙皮多边形仍保持非缓存路径，避免阴影静止或姿态错误。

### 当前状态

- 静态模型缓存可用。
- 动态姿态接管仍在推进中，目标是后续改为“静态模型资源 + 每帧 Pose/Palette 更新”。
- 当前版本重点仍是稳定性与链路铺设，而不是完全接管动态模型顶点计算。
