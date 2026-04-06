# Codex 通宵优化路线图

## 📅 时间规划（预计 6-8 小时）

```
03:00 ─────────────────────────────────────────────────────── 11:00
  │                                                              │
  │  Phase 1      Phase 2        Phase 3         Phase 4         │
  │  [1.5h]       [2h]           [2h]            [2h]            │
  │  移除监控     简化 Hook       修复崩溃        Instancing 准备  │
  │                                                              │
  └──────────────────────────────────────────────────────────────┘
```

---

## 🧭 Phase 1: 移除性能监控开销（1.5h）
**目标 FPS: 180 → 280**

### 任务 1.1: 添加编译期开关 (30min)
**文件**: `war3_perf_monitor.h`

```cpp
// 添加到文件开头
#ifndef WAR3_PERF_ENABLED
  #if defined(NDEBUG) || defined(WAR3_RELEASE)
    #define WAR3_PERF_ENABLED 0
  #else
    #define WAR3_PERF_ENABLED 1
  #endif
#endif
```

**需要额外逆向**: ❌ 否

### 任务 1.2: 包装所有 cpuScope 调用 (45min)
**文件**: `d3d9_war3_hook.cpp`, `d3d9_device.cpp`, `d3d9_war3_pipeline.cpp`

```cpp
// 替换所有
auto perfScope = war3::War3PerfMonitor::instance().cpuScope("...");

// 为
#if WAR3_PERF_ENABLED
  auto perfScope = war3::War3PerfMonitor::instance().cpuScope("...");
#endif
```

**统计**: 约 40+ 处调用需要修改

**需要额外逆向**: ❌ 否

### 任务 1.3: 验证构建 (15min)
```bash
meson configure build32 -Db_ndebug=true
ninja -C build32
```

---

## 🧭 Phase 2: 简化高频 Hook（2h）
**目标 FPS: 280 → 350**

### 任务 2.1: 全局功能开关缓存 (30min)
**文件**: `war3_render_state.h/cpp`

```cpp
// 添加静态缓存，避免每次调用都检查多个条件
class War3RenderState {
public:
  // 帧开始时调用一次
  static void UpdateFrameFeatureFlags() {
    s_anyFeatureEnabled = HasOutlineHandles() || HasBloomHandles() || 
                          IsShadowEnabled() || ...;
  }
  
  static bool AnyFeatureEnabled() { return s_anyFeatureEnabled; }
  
private:
  static inline bool s_anyFeatureEnabled = false;
};
```

**需要额外逆向**: ❌ 否

### 任务 2.2: Hook_Dispatch_Common 极简化 (45min)
**文件**: `d3d9_war3_hook.cpp`

```cpp
int __fastcall Hook_Dispatch_Common(...) {
  // 快速路径：无功能启用时直接透传
  if (likely(!War3RenderState::AnyFeatureEnabled())) {
    return g_trampolineDispatchCommon(...);
  }
  
  // 慢速路径：原有逻辑
  // ...
}
```

**目标**: 将 286K 次/帧的 Hook 开销降到接近 0

**需要额外逆向**: ❌ 否

### 任务 2.3: Hook_Dispatch_Special 极简化 (45min)
同上逻辑

**需要额外逆向**: ❌ 否

---

## 🧭 Phase 3: 修复 FlushSortedItems 崩溃（2h）
**目标**: 稳定启用 kNativeQueueSortEnabled

### 任务 3.1: 分析崩溃原因 (1h)
**已知信息**:
- 战役模式崩溃
- 测试地图正常
- 崩溃发生在 Dispatch 调用

**可能原因**:
1. 战役模式的 RenderBatchElement 结构不同
2. SceneNode 指针无效
3. 函数指针地址在战役模式不同

**调试方法**:
```cpp
// 添加到 FlushSortedItems_StdSort 开头
WAR3_RENDER_LOG("FlushSortedItems: count=%u, first_batch=%p\n", 
                count, sortedPtrs[0]);
```

**需要额外逆向**: ⚠️ 可能需要
- 如果发现结构体差异，需要用 IDA 分析战役模式的内存布局

### 任务 3.2: 修复并验证 (1h)
根据 3.1 分析结果进行修复

---

## 🧭 Phase 4: Instancing 基础设施（2h）
**目标**: 为后续 Instancing 做准备

### 任务 4.1: 获取模型 ID (1h)
**已有信息**:
- `WorldObjectEntry` 结构位于 `this+91/92/93`
- `RenderablePart` 位于 `batchEntry`
- 需要找到：模型指针或模型 ID

**需要额外逆向**: ✅ 是

需要逆向的内容：
```
WorldObjectEntry (vtable)
  ├─ +0x00: vtable
  ├─ +0x04: 未知
  ├─ +0x08: 模型指针? (需要确认)
  └─ ...

或者从 RenderablePart:
  ├─ +0x0C: MeshData* (已知)
  └─ MeshData 是否包含模型 ID?
```

### 任务 4.2: 创建模型分组结构 (1h)
**文件**: `war3/reimpl/war3_instancing.h`

```cpp
struct InstanceGroup {
  uint32_t modelId;
  std::vector<Matrix4x4> transforms;
  std::vector<uint32_t> batchIndices;
};

class InstanceCollector {
public:
  void BeginFrame();
  void AddBatch(const RenderBatchElement* batch, const Matrix4x4& transform);
  const std::vector<InstanceGroup>& GetGroups() const;
};
```

**需要额外逆向**: ❌ 否（结构定义不需要，但 modelId 获取需要）

---

## 📊 逆向工程需求总结

| 任务 | 需要逆向 | 逆向内容 | 已有数据足够 |
|------|----------|----------|--------------|
| Phase 1 全部 | ❌ | - | ✅ |
| Phase 2 全部 | ❌ | - | ✅ |
| Phase 3.1 | ⚠️ 可能 | 战役模式差异 | 需要运行时调试 |
| Phase 4.1 | ✅ 是 | WorldObjectEntry vtable / 模型指针 | ❌ 不足 |

**结论**: **Phase 1-2 可以直接开工**，Phase 3-4 可能需要额外逆向或运行时调试。

---

## 🔧 Codex 执行建议

### 执行顺序
1. **先做 Phase 1-2**：这是纯代码修改，无需额外信息
2. **Phase 3**：需要带日志的 Debug 版本运行战役收集崩溃信息
3. **Phase 4**：需要 IDA 逆向获取模型 ID 偏移

### 验证点
- Phase 1 完成后：Release 模式 FPS ≥ 280
- Phase 2 完成后：Release 模式 FPS ≥ 350
- Phase 3 完成后：战役模式无崩溃 + kNativeQueueSortEnabled 可用
- Phase 4 完成后：能按模型 ID 分组（Instancing 的前置条件）

---

## 📁 需要创建/修改的文件清单

### 新建
- `war3/reimpl/war3_instancing.h`

### 修改
- `war3_perf_monitor.h` - 添加编译开关
- `d3d9_war3_hook.cpp` - 40+ 处 cpuScope 条件化 + Hook 极简化
- `d3d9_device.cpp` - cpuScope 条件化
- `d3d9_war3_pipeline.cpp` - cpuScope 条件化
- `war3_render_state.h/cpp` - 全局功能开关缓存
- `war3_render_queue.h` - 崩溃修复

---

## ✅ 检查清单

- [ ] Phase 1.1: 编译开关定义
- [ ] Phase 1.2: cpuScope 条件化（40+ 处）
- [ ] Phase 1.3: 构建验证
- [ ] Phase 2.1: 全局功能开关缓存
- [ ] Phase 2.2: Hook_Dispatch_Common 极简化
- [ ] Phase 2.3: Hook_Dispatch_Special 极简化
- [ ] Phase 3.1: 崩溃原因分析
- [ ] Phase 3.2: 崩溃修复
- [ ] Phase 4.1: 模型 ID 获取（需逆向）
- [ ] Phase 4.2: 模型分组结构
