# Project Iris - 模块化着色器引擎

## 目标与愿景
- 把硬编码的渲染 Hook 重构为数据驱动的管线系统
- 解耦游戏逻辑与渲染逻辑，形成稳定的可扩展架构
- 向脚本/模组作者开放渲染数据与运行时开关

## 核心原则
- 解耦：游戏逻辑（Jass/Net）与渲染逻辑彻底分离
- 数据驱动：Pass/Shader/Material 由 JSON 描述
- 可拓展：Shader 可访问中间数据（GBuffer/Depth/Normals）
- 可控：所有特性具备运行时开关

## 初始化与 Hook 策略（当前约束）
- 仅在首次执行 JASS 函数后激活 Game.dll Hook 与初始化
- NetEventHook 仅负责触发 OnGameStart（不再做 JASS 初始化）
- 这样可以避免过早触发导致的空指针崩溃

## 阶段路线
### Phase 1：核心解耦
- 1.1 引入 War3Renderer，迁移每帧与渲染调用逻辑
- 1.2 完成 RenderObjectRegistry 迁移，Hook 只做桥接
- 1.3 统一全局状态，避免散落的 TLS/全局变量

### Phase 2：JSON 管线
- 定义 War3PipelineSpec
- 实现 ShaderPackLoader 与 Pipeline 解析

### Phase 3：开放 Shader API
- 全局 UniformBlock（时间/矩阵/环境/交互数据）
- 纹理资源池（中间 RT 具名访问）
- 材质参数与自定义 Shader 加载

### Phase 4：ImGui 深度集成
- UI 作为游戏内功能而非调试工具
- Scripting 绑定（Jass/Lua/C++）

### Phase 5：动态特性开关
- RenderConfig + 脚本 API
- 地图触发器可动态切换渲染特性

### Phase 6：热重载与容错
- 监控 shaderpacks 目录
- Shader 编译失败回退并提示

### Phase 7：文档与示例
- 生成 API 参考
- 提供 ExampleShaderPack

## 当前进展
- 已完成：War3Renderer 初步落地，Hook 迁移 TLS 与场景注册
- 已完成：War3RenderDispatcher 引入，分发阶段状态开始抽离
- 已完成：JASS 首次执行后统一激活初始化与 Hook
- 进行中：清理 hook.cpp 内残余渲染逻辑

## 开发流程（Ninja Loop）
1. 小步改动
2. `ninja -C build32`
3. 运行验证（日志检查）
4. 继续迭代

## 相关入口
- 进度总览：`STATUS.md`
- War3 模块说明：`src/d3d9/war3/README.md`
- 生命周期说明：`docs/WAR3_LIFECYCLE.md`
