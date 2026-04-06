# War3 生命周期梳理

本文件描述 WarVK 在 1.27a 中的初始化阶段、运行阶段与退出清理的关键时机与代码入口。

## 阶段 0：D3D 设备创建（基础功能）
- 目的：只做与游戏逻辑无关的“基础功能”。
- 当前内容：
  - 崩溃捕捉、日志初始化
  - DXVK/D3D9 设备与渲染管线初始化
- 主要位置：
  - `src/d3d9/d3d9_device.cpp`
  - `src/d3d9/d3d9_interface.cpp`

## 阶段 1：JASS 入口触发（引导激活）
- 触发点：`executeJassFunction` 首次执行（JASS 执行入口）。
- 目的：只做“安全可用但不触发 JASS VM”的准备工作。
- 当前行为（只做预解析）：
  - `war3::Initialize`（模块级初始化）
  - `ShaderManager::reload`
  - `war3_preinit`（`base_game_init` + `jass_init`）
  - `game_table` 读取
  - 安装 NetEventHook
  - 安装完整 Game.dll Hook
- 主要位置：
  - `src/d3d9/d3d9_war3_hook.cpp`
  - `src/d3d9/jass/war3_game.cpp`

## 阶段 2：地图就绪（GameReady）
- 触发点：`NET_EVENT_GAME_READY`。
- 目的：真正执行 JASS 相关初始化。
- 当前行为：
  - `war3_init`（包含 `jass_native_init`）
  - `War3Events::fireOnJassReady`
  - `War3Events::fireOnGameStart`
- 主要位置：
  - `src/d3d9/war3/core/war3_net_event_hook.h`
  - `src/d3d9/jass/war3_game.cpp`

## 阶段 3：运行期（Tick / Render）
- 触发点：`NET_EVENT_GAME_TICK / IDLETICK`，以及各类渲染 Hook。
- 目的：更新状态、采集渲染对象、驱动效果。
- 主要位置：
  - `src/d3d9/d3d9_war3_hook.cpp`
  - `src/d3d9/war3/render/*`
  - `src/d3d9/war3/core/war3_events.h`

## 阶段 4：离开地图（GameLeave）
- 触发点：`NET_EVENT_GAME_LEAVE`。
- 目的：清理渲染缓存与状态，避免跨地图污染。
- 当前行为：
  - 清理渲染对象映射与缓存
  - 清理 RenderState/Outline/Stage/TLS
  - 重置 NetEventHook 与 runtime 激活标志
  - `War3Events::reset`
- 主要位置：
  - `src/d3d9/d3d9_war3_hook.cpp`
  - `src/d3d9/war3/core/war3_net_event_hook.h`

## 备注
- 再次进入地图后，`executeJassFunction` 会重新触发阶段 1。
- 如需新增“初始化”逻辑，优先放在阶段 2（GameReady）。

## 调试开关
- `DXVK_WAR3_RENDER_LOG=1`：开启渲染相关日志（默认关闭）。
- `DXVK_WAR3_NETEVENT_LOG=1`：开启 NetEvent 事件日志（默认关闭）。
- `DXVK_WAR3_TIME_LOG=1`：打印游戏内时间（GetFloatGameState）。
