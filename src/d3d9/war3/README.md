# War3 模块说明

## 目录结构

```
war3/
├── core/                        核心工具
│   ├── war3_memory.h/cpp        内存安全读取工具
│   └── war3_game_structs.h      游戏结构定义和偏移常量
├── handle/                      Handle 解析
│   └── war3_handle_resolver.h/cpp
├── render/                      渲染对象追踪
│   ├── war3_render_objects.h/cpp
│   ├── war3_scene_collector.h/cpp
│   ├── war3_renderer.h/cpp      渲染入口与 Hook 桥接
│   ├── war3_render_dispatcher.h/cpp 渲染分发阶段状态桥接
│   ├── war3_render_queue_tracker.h/cpp RenderQueue 标签/阶段跟踪
│   ├── war3_render_exec_batch.h/cpp ExecBatch 解析与句柄追踪
│   └── war3_render_state.h/cpp  渲染阶段/批次状态管理
├── debug/                       调试工具
│   └── war3_debug.h/cpp
└── war3.h/cpp                   统一入口
```

## 模块详述

### core/war3_memory.h
提供安全的内存读取工具：
- `IsReadableRange()` - 检查内存区域是否可读
- `IsExecutableRange()` - 检查内存区域是否可执行  
- `SafeRead<T>()` - 安全读取指定偏移的值

### core/war3_game_structs.h
包含 War3 游戏内部结构定义：
- **结构体**：`CUnit`, `CAgentBaseAbs`, `JassHandleTable` 等
- **偏移常量**：`CUnitOffsets`, `CAgentOffsets` 等命名空间
- **Flag 常量**：`UnitFlags5C`, `UnitFlags60`
- **FourCC 常量**：`AgentTypeFourCC` (包含 `_LE` 后缀版本用于内存比较)
- **工具函数**：`MakeFourCC()`, `IsLikelyFourCC()`, `MakeJHandle()` 等

### handle/war3_handle_resolver.h
Handle 与游戏对象的双向解析：
- `findHandleByUnitPtr()` - 从 CUnit* 查找对应的 HandleId
- `resolveHandle()` - 从 jHandle 解析 Agent 对象

### render/war3_render_objects.h
渲染对象追踪系统：
- `RenderObjectRegistry` - 单例注册表
- `RenderObjectInfo` - 渲染对象完整信息
- `ObjectKind` - 对象类型枚举

### render/war3_scene_collector.h
场景对象收集器：
- `CollectWorldObjects()` - 遍历游戏对象列表，注册到 Registry
- 负责 List Structure 的解析和数据获取

### render/war3_renderer.h
渲染入口与 Hook 桥接：
- 统一每帧生命周期（BeginFrame/EndFrame）
- 维护渲染上下文 TLS（WorldObjectEntry/SceneNode）
- 封装 Hook 与 RenderObjectRegistry 的交互

### render/war3_render_dispatcher.h
渲染分发阶段状态桥接：
- 统一 RenderDispatcher/SceneSubmitBatch 的阶段标记
- 统一 UI 分发入口的层级标记

### render/war3_render_queue_tracker.h
RenderQueue 标签/阶段追踪：
- 记录 RenderQueue 元素的 tag/stage，避免 Flush 后丢失上下文
- 在 ExecBatch 执行时恢复 stage/tag，供后续分类与效果使用

### render/war3_render_exec_batch.h
ExecBatch 解析与句柄追踪：
- 统一 ExecBatch Type0/Type3 的上下文处理
- 解析 SceneNode/Handle/Agent 信息，为描边与调试输出提供数据
- 集中管理 ExecBatch 缓存与清理

### render/war3_render_state.h
渲染阶段/批次状态管理：
- 提供 Stage/Layer/BatchTag 的统一查询与更新接口
- 为管线插入点、阴影与调试渲染提供稳定状态

### state/war3_render_state.h
渲染状态管理器：
- `beginFrame`/`endFrame` - 帧生命周期管理
- `setWorldPointer` - 管理全局 World 对象指针
- 替代分散的全局变量，提供线程安全的单例访问

## 使用方式

```cpp
#include "war3/war3.h"

// 初始化（可选，传入 Game.dll 基址）
dxvk::war3::Initialize();

// 在 Hook 中注册对象
auto& registry = dxvk::war3::render::RenderObjectRegistry::instance();
registry.registerWorldObject(entryPtr, unitPtr, groupIdx);

// 查询对象信息
RenderObjectInfo info;
if (registry.queryBySceneNode(sceneNode, info)) {
    if (info.kind == ObjectKind::Unit) {
        // 处理单位
    }
}
```

## 与其他模块的关系

- `jass/` 目录 - 底层 Jass 脚本接口和完整游戏结构
- `d3d9_war3_hook.*` - Hook 逻辑和 War3RenderState 状态管理
- `d3d9_war3_shadow.*` - 阴影和描边渲染

## 规划文档

- `docs/PROJECT_IRIS.md` - Project Iris 重构路线图
- `docs/WAR3_LIFECYCLE.md` - 初始化与生命周期说明
