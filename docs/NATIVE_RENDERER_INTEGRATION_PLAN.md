# Native Renderer Hook 集成方案

## 现状分析

### 当前项目状态
1. **WarVK 项目已有完整的渲染系统**：
   - `War3Hook` 类在 `d3d9_war3_hook.cpp` 中
   - 使用 MinHook 进行函数Hook
   - 已有完整的对象追踪、渲染队列、批处理等系统

2. **我们的Native Renderer Hook系统**：
   - 基于完整逆向分析还原的渲染函数
   - 使用内联汇编Hook方式
   - 提供完全接管原版渲染流程的能力

### 编译错误原因

#### 1. `war3_native_renderer.cpp` 缺少函数实现
错误信息：
```
error: 'WorldObjectEntry_Render' was not declared in this scope
error: 'List_GetData' was not declared
error: 'List_GetCount' was not declared
```

**问题**：头文件中声明了这些函数，但`war3_native_renderer.cpp`中没有实现。

#### 2. `war3_native_renderer_core.cpp` 变量未声明
错误信息：
```
error: 'worldPos' was not declared in this scope
```

**问题**：在 `RenderBatch_Submit` 函数中使用了未声明的变量。

#### 3. `d3d9_war3_hook.cpp` 函数未声明
错误信息：
```
error: 'SetCallsitePatchEnabled' was not declared in this scope
error: 'SetStateCacheEnabled' was not declared
```

**问题**：现有项目代码调用了不存在的函数，可能是未完成的代码。

## 集成策略

### 方案A：渐进式集成（推荐）

#### 阶段1：准备阶段（当前）
1. **暂时禁用Native Renderer Hook接管**
   - 在 `war3_internal_test_config.h` 中设置：
     ```cpp
     inline constexpr bool kNativeRendererHookTakeoverEnabled = false;
     ```
   
2. **完善缺失的函数实现**
   - 在 `war3_native_renderer.cpp` 中添加缺失的函数：
     - `List_GetData` 和 `List_GetCount`
     - `WorldObjectEntry_Render` 实现
   - 添加必要的辅助函数

3. **修复现有项目编译错误**
   - 在 `d3d9_war3_hook.cpp` 中注释掉或修复未完成的函数调用
   - 确保现有项目能正常编译

4. **测试验证**
   - 编译整个项目
   - 运行游戏验证基本功能正常
   - 检查日志输出

#### 阶段2：测试阶段
1. **启用Native Renderer Probe（已实现）**
   - `kNativeRendererProbeEnabled = true`（已设置）
   - 验证Probe能正常收集渲染数据

2. **测试现有渲染流程**
   - 验证现有 `War3Hook` 系统工作正常
   - 确保没有破坏现有功能

3. **检查兼容性**
   - 确认两个系统可以共存
   - 验证不会重复Hook同一函数

#### 阶段3：渐进启用Hook接管
1. **分阶段启用Hook函数**
   - 先启用 `RenderQueue_AddBatch` Hook
   - 测试后启用 `RenderBatch_Submit` Hook
   - 逐步启用其他核心函数

2. **启用状态优化**
   - 启用 `kNativeQueueTakeoverEnabled`
   - 验证批处理优化正常工作

3. **最终完整接管**
   - 启用 `kNativeRendererHookTakeoverEnabled = true`
   - 完全替换原版渲染流程
   - 性能对比测试

### 方案B：并行独立运行（备选）

如果集成遇到严重问题，可以考虑：

1. **两个系统并行运行**
   - Native Renderer Hook 作为可选的增强功能
   - 现有 `War3Hook` 保持不变
   - 通过配置选项切换使用哪个系统

2. **逐步迁移**
   - 将Native Renderer的功能逐步迁移到 `War3Hook` 系统中
   - 最终完全替换

## 关键注意事项

### 1. Hook冲突处理
- 确保不会对同一函数进行多次Hook
- 添加Hook前的检查逻辑
- 提供卸载功能

### 2. 性能影响
- Native Renderer Hook 使用内联Hook，性能开销极小
- 但需要验证在DXVK环境下的实际效果
- 建议使用性能分析工具对比

### 3. 调试支持
- 保持详细的日志输出
- 使用配置开关控制日志级别
- 提供性能统计信息

### 4. 兼容性
- 确保与现有项目对象追踪系统兼容
- 保持数据结构一致性
- 支持热重载配置

## 立即行动项

### 高优先级（P0）
1. ✅ 添加缺失函数实现到 `war3_native_renderer.cpp`
2. ✅ 修复 `war3_native_renderer_core.cpp` 中的编译错误
3. ✅ 修复 `d3d9_war3_hook.cpp` 中的函数未声明错误
4. ✅ 确保配置文件中的 `kNativeRendererHookTakeoverEnabled = false`
5. ✅ 编译整个项目
6. ✅ 运行基础测试

### 中优先级（P1）
1. 测试 Native Renderer Probe 功能
2. 验证两个系统兼容性
3. 检查日志输出
4. 性能基准测试

### 低优先级（P2）
1. 实现Hook冲突检测机制
2. 添加性能统计
3. 完善文档和注释
4. 最终完整接管测试

## 成功标准

### 功能验证
- [ ] 原版游戏能正常启动和运行
- [ ] 现有渲染系统不受影响
- [ ] Probe功能正常收集数据
- [ ] 无崩溃或明显性能下降

### 性能验证
- [ ] FPS保持在可接受范围
- [ ] 内存使用无异常增长
- [ ] Draw Call 数量合理

### 代码质量
- [ ] 编译无警告
- [ ] 代码符合项目规范
- [ ] 有适当的错误处理
- [ ] 日志输出清晰有用

## 风险评估

### 技术风险
- **中等风险**：Hook系统可能与现有系统冲突
- **缓解措施**：渐进式启用，详细的错误处理和日志

### 稳定性风险
- **低风险**：即使有问题，可以通过配置禁用
- **缓解措施**：保持配置开关，快速回退能力

## 下一步

1. **立即行动**：修复P0优先级问题
2. **测试验证**：完成P0后进行全面测试
3. **监控反馈**：收集用户反馈和性能数据
4. **迭代改进**：根据反馈逐步优化

---

**文档版本**：v1.0  
**创建日期**：2026-01-25  
**作者**：Claude AI  
**状态**：待实施