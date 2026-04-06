# War3渲染系统在DXVK/Vulkan环境下的见解与优化策略

> **基于完整的逆向分析报告** (Game.dll 1.27.x)
> **生成时间**: 2026-01-25
> **目的**: 在DXVK环境下充分发挥Vulkan性能优势，同时保持游戏正常运行

---

## 一、核心架构理解

### 1.1 渲染流程概览

War3的渲染系统采用**多阶段分批渲染**架构：

```
GameMain 
  └─ EventScheduler (每帧心跳)
      └─ CWorld::RenderScene (总指挥)
          ├─ Stage 1: 地形 (Terrain) → Flush
          ├─ Stage 9: 单位 (Units) → 收集
          ├─ Stage 2: 建筑 (Buildings) → 收集
          ├─ Stage 8: 特效 (Effects) → 收集
          └─ Flush Opaque (一次性刷新所有不透明对象)
              └─ Flush Transparent (透明队列独立刷新)
```

**关键特点**：
- **分层渲染**：22个不同的渲染阶段，每阶段有特定的渲染目标
- **批量提交**：通过RenderQueue收集批次，统一排序后分发给GPU
- **透明/不透明分离**：两个独立的队列，分别处理
- **状态优化**：通过memcmp比较LayerState，跳过重复的状态切换

### 1.2 核心数据结构

#### RenderBatchElement（20字节）
```cpp
struct RenderBatchElement {
    void* batchEntry;      // +0x00: RenderablePart指针
    uint32_t flags;       // +0x04: bit0=meshFlag, bit1=hasMoreLayers
    uint32_t layerIndex;   // +0x08: 层索引（排序关键）
    uint32_t layerCounter; // +0x0C: 可见层计数
    void* layerStatePtr;   // +0x10: 36字节状态块
};
```

**关键发现**：
- `layerStatePtr` 是状态优化的核心，仅比较前20字节
- `flags & 3 == 3` 标记为Special类型，走不同Dispatch路径
- `meshFlag` 触发提前退出，只处理首层

#### AUCTransparentEntry（24字节）
```cpp
struct AUCTransparentEntry {
    uint32_t type;     // +0x00: 0=粒子, 2=缎带, 3=特效, 4=附着物
    uint32_t sortKey;  // +0x04: 透明排序键（优先）
    float distSq;      // +0x08: 距离平方（次要）
    void* payload;    // +0x0C: 对象指针
    uint32_t arg1, arg2; // +0x10/0x14: 回调参数
};
```

**排序策略**：
- **Primary**: `transparentKey`（越大越后，渲染越上）
- **Secondary**: `distSq`（越小越后，近者优先）
- **Back-to-Front**: 透明对象从远到近渲染

---

## 二、DXVK/Vulkan环境下的挑战

### 2.1 D3D9状态管理的开销

原版引擎依赖D3D9的**懒加载状态管理**：

```cpp
// 原版逻辑
if (memcmp(lastState, newState, 20) == 0) {
    // 跳过状态切换
    stateChanged = 0;
} else {
    GxDevice_ApplyStateBlock(newState);
    stateChanged = 1;
}
```

**DXVK/Vulkan问题**：
1. **Pipeline State Object (PSO) 创建昂贵**：Vulkan的PSO不能像D3D9那样频繁切换
2. **描述符池限制**：频繁的SetTexture可能导致描述符耗尽
3. **命令缓冲开销**：每次状态改变都需要新的命令记录

### 2.2 Draw Call碎片化

原版引擎每帧产生**数千次Draw Call**：
- 每个Layer一次Dispatch
- 每次Dispatch调用SetVertexBuffer + DrawPrimitive
- 透明对象独立处理

**性能瓶颈**：
- CPU侧的命令缓冲录制开销
- GPU侧的PSO切换开销
- 缺乏实例化渲染

### 2.3 深度同步问题

原版依赖 `RenderSceneFlush()` 强制同步：
```cpp
if (meshFlag == 0) {
    GxDevice_RenderSceneFlush();  // 强制提交指令
}
```

**DXVK/Vulkan影响**：
- Vulkan的显式同步机制可能与D3D9的隐式同步冲突
- 过度刷新导致GPU管线利用率低

---

## 三、Vulkan优化策略

### 3.1 Pipeline State Object (PSO) 缓存

**目标**：减少PSO切换次数

**实现策略**：
```cpp
class PSOCache {
    struct PSOKey {
        uint32_t blendMode;      // +0x18 in LayerState
        uint32_t depthTest;      // 深度测试模式
        uint32_t depthWrite;      // 深度写入
        uint32_t alphaTest;       // Alpha测试
        uint32_t alphaFunc;       // Alpha函数
        // ... 其他状态
    };
    
    std::unordered_map<PSOKey, VkPipeline> cache_;
    
public:
    VkPipeline GetOrCreatePSO(const LayerState* state) {
        PSOKey key = ExtractKey(state);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second;
        }
        return CreateNewPSO(key);
    }
};
```

**优化效果**：
- 相同状态的批次共享PSO
- 预期减少PSO切换 60-80%

### 3.2 实例化渲染 (Instancing)

**目标**：合并相同模型的Draw Call

**识别相同模型**：
```cpp
// 获取模型ID路径
WorldObjectEntry → SceneNode → RenderablePart → MeshData → meshIndex (0x108)
```

**实现策略**：
```cpp
struct InstanceBatch {
    uint32_t meshIndex;       // 模型标识
    std::vector<float[16]> transforms;  // 实例变换矩阵
    std::vector<uint32_t> instanceIds; // 实例ID
};

class InstancingRenderer {
    std::unordered_map<uint32_t, InstanceBatch> batches_;
    
public:
    void AddInstance(const RenderBatchElement* batch, const float* matrix) {
        uint32_t meshIndex = batch->batchEntry->meshData->meshIndex;
        batches_[meshIndex].transforms.push_back(matrix);
        batches_[meshIndex].instanceIds.push_back(batch->layerIndex);
    }
    
    void RenderAll(VkCommandBuffer cmd) {
        for (auto& pair : batches_) {
            VkDrawMeshTasksIndirectCommand cmd = {};
            cmd.taskCount = pair.second.transforms.size();
            cmd.firstTask = 0;
            
            vkCmdDrawMeshTasksIndirect(cmd, &cmd);
        }
        batches_.clear();
    }
};
```

**优化效果**：
- 相同模型合并为单次Draw Call
- 预期减少Draw Call 70-90%
- 特别适用于单位群、树木等重复模型

### 3.3 批量描述符更新

**目标**：减少描述符集绑定次数

**实现策略**：
```cpp
class DescriptorBatcher {
    struct TextureKey {
        void* textures[8];  // 最多8个纹理阶段
        uint32_t hash;
    };
    
    std::unordered_map<TextureKey, VkDescriptorSet> cache_;
    std::vector<TextureKey> pending_;
    
public:
    void SetTexture(uint32_t stage, void* texture) {
        // 延迟绑定，累积所有纹理请求
        // 在Flush时批量更新描述符
    }
    
    void Flush(VkCommandBuffer cmd) {
        // 批量创建/更新描述符集
        for (auto& key : pending_) {
            UpdateDescriptorSet(key);
        }
        pending_.clear();
    }
};
```

**优化效果**：
- 减少描述符集切换 50-70%
- 提高描述符池利用率

### 3.4 多线程命令录制

**目标**：并行化命令缓冲录制

**实现策略**：
```cpp
class ParallelCmdRecorder {
    std::vector<VkCommandPool> pools_;
    std::vector<VkCommandBuffer> buffers_;
    std::thread threads_[4];
    
public:
    void BeginRecording() {
        // 为每个线程创建独立的命令池和缓冲
        for (int i = 0; i < 4; i++) {
            pools_[i] = CreateCmdPool();
            buffers_[i] = AllocCmdBuffer(pools_[i]);
        }
    }
    
    void RecordBatch(int threadId, const RenderBatchElement* batch) {
        // 并行录制命令到各自的缓冲
    }
    
    void Submit(VkQueue queue) {
        // 合并所有命令缓冲并提交
        VkSubmitInfo submit = {};
        submit.commandBufferCount = buffers_.size();
        submit.pCommandBuffers = buffers_.data();
        vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    }
};
```

**优化效果**：
- 利用多核CPU并行录制
- 预期提升CPU利用率 2-3倍

---

## 四、保持游戏正常性的关键点

### 4.1 StageUpdate(0) 的必要性

**问题**：原版引擎依赖StageUpdate刷新脏状态

**解决方案**：
```cpp
void RenderQueue_StageUpdate(int mode) {
    if (mode == 0) {
        // 检查D3D9状态是否脏
        if (IsStateDirty()) {
            FlushD3D9State();
        }
    } else {
        // 强制设置状态
        SetD3D9State();
    }
}
```

**关键**：在每次Dispatch后必须调用`RenderQueue_StageUpdate(0)`

### 4.2 跨阶段状态清理

**问题**：不透明阶段的Stage 1纹理（TeamColor）会泄露给透明阶段

**解决方案**：
```cpp
void RenderQueue_FlushAndReset(RenderCategory category, CWorld* world) {
    RenderQueue_FlushSortedItems(...);
    
    if (category == RenderCategory::Opaque) {
        // 清理Stage 1纹理
        ID3D9Device_SetTexture(1, nullptr);
    }
    
    // ... 其他清理逻辑
}
```

**关键**：在透明渲染开始前显式清理Stage 1

### 4.3 透明队列排序的正确性

**问题**：忽略距离导致深度冲突（基尔加丹、传送门）

**解决方案**：
```cpp
bool TransparentComparator(const AUCTransparentEntry* a, const AUCTransparentEntry* b) {
    // 优先比较sortKey
    if (a->sortKey != b->sortKey) {
        return a->sortKey < b->sortKey;
    }
    
    // 次要比较距离（远到近）
    return a->distSq > b->distSq;
}
```

**关键**：必须同时考虑`sortKey`和`distSq`

### 4.4 Type 3 (Special) 标志的正确传递

**问题**：不透明的Type 3对象被错误初始化

**解决方案**：
```cpp
void FlushSortedItems() {
    for (int i = 0; i < count; i++) {
        RenderBatchElement* batch = sortedPtrs[i];
        bool isSpecial = ((batch->flags & 3) == 3);
        
        // 正确传递isSpecial标志
        ExecBatchProcessor(batch, isSpecial);
    }
}
```

**关键**：传递`isSpecial`而非`!isSpecial`

---

## 五、性能优化路线图

### 阶段1：基础优化（即时实施）
- [x] 完整逆向渲染链
- [x] 实现PSO缓存系统
- [x] 修复StageUpdate调用时机
- [x] 修复跨阶段状态清理
- [ ] 修复透明队列排序
- [ ] 修复Type 3标志传递

**预期收益**：FPS提升 20-30%

### 阶段2：中级优化（1-2周）
- [ ] 实现实例化渲染
- [ ] 批量描述符更新
- [ ] 优化Draw Call合并策略
- [ ] 实现多线程命令录制

**预期收益**：FPS提升 40-60%

### 阶段3：高级优化（1-2月）
- [ ] 实现GPU驱动的剔除
- [ ] 实现计算着色器实例化
- [ ] 优化深度预渲染
- [ ] 实现延迟渲染（可选）

**预期收益**：FPS提升 80-120%

---

## 六、调试与验证

### 6.1 关键测试场景

1. **基尔加丹战斗**：验证复杂模型渲染
2. **传送门效果**：验证深度排序
3. **大量单位场景**：验证实例化效果
4. **粒子特效**：验证透明队列处理
5. **TeamColor切换**：验证Stage 1清理

### 6.2 性能指标监控

```cpp
struct PerformanceMetrics {
    uint32_t drawCalls;
    uint32_t psoSwitches;
    uint32_t descriptorUpdates;
    uint32_t flushCalls;
    float frameTime;
    float gpuTime;
};

PerformanceMetrics metrics;

void UpdateMetrics() {
    // 每帧记录关键指标
    metrics.drawCalls = GetDrawCallCount();
    metrics.psoSwitches = GetPSOSwitchCount();
    metrics.frameTime = GetFrameTime();
}
```

### 6.3 回退机制

```cpp
bool ValidateRendering() {
    // 检测渲染异常
    if (metrics.psoSwitches > threshold) {
        LOG_WARN("Too many PSO switches: %d", metrics.psoSwitches);
        return false;
    }
    
    if (metrics.frameTime > 33.0f) {
        LOG_WARN("Frame time too high: %.2f ms", metrics.frameTime);
        return false;
    }
    
    return true;
}

void SafeRender() {
    if (!ValidateRendering()) {
        // 回退到原版渲染路径
        FallbackToOriginalRenderer();
    } else {
        // 使用优化渲染路径
        OptimizedRender();
    }
}
```

---

## 七、总结与建议

### 7.1 核心见解

1. **RenderQueue是性能关键**：集中管理所有渲染批次，是优化的最佳切入点
2. **状态优化至关重要**：LayerState的20字节比较是原版优化的核心
3. **透明对象需要特殊处理**：独立的排序逻辑和距离计算
4. **Special类型影响渲染路径**：flags & 3 == 3 走不同Dispatch

### 7.2 Vulkan优势最大化

1. **PSO缓存**：减少昂贵的管线状态创建
2. **实例化渲染**：合并相同模型的Draw Call
3. **批量描述符**：减少描述符集绑定
4. **多线程录制**：利用多核CPU并行录制

### 7.3 游戏兼容性保障

1. **保持原有逻辑**：不改变渲染顺序和状态切换时机
2. **显式状态清理**：在阶段边界显式清理残留状态
3. **正确的排序**：透明对象必须考虑sortKey和distSq
4. **Special标志**：正确传递Type 3的isSpecial标志

### 7.4 实施建议

**渐进式优化**：
1. 先实现基础优化，确保游戏正常运行
2. 逐步添加高级优化，每步验证兼容性
3. 提供配置选项，允许用户启用/禁用优化
4. 保持回退机制，遇到问题时快速恢复

**性能目标**：
- 4K分辨率：60 FPS稳定
- 1080p分辨率：144 FPS稳定
- 低配设备：30 FPS稳定

---

## 八、附录

### 8.1 关键函数地址速查

| 函数名 | RVA | 说明 |
|--------|-----|------|
| `CWorld::RenderScene` | 0x3681C0 | 总渲染入口 |
| `RenderBatch_Submit` | 0x1375C0 | 批次提交 |
| `RenderQueue_FlushSortedItems` | 0x1380A0 | 排序刷新 |
| `Dispatch_Common` | 0x13A5E0 | 通用派发 |
| `Dispatch_Special` | 0x13A780 | 特殊派发 |

### 8.2 全局变量地址速查

| 变量名 | RVA | 类型 |
|--------|-----|------|
| `gx_device` | 0xBC5420 | GxDevice* |
| `g_RenderQueue_BatchArray` | 0xBC6BB0 | void* |
| `g_AUCTransparent_Array` | 0xBC6BC0 | void* |
| `g_RenderCamera_PosXY` | - | float[2] |
| `g_RenderCamera_PosZ` | - | float |

### 8.3 结构体大小速查

| 结构体 | 大小 | 关键偏移 |
|--------|------|----------|
| RenderBatchElement | 20 | +0x04=flags |
| AUCTransparentEntry | 24 | +0x04=sortKey |
| SceneNode | 252+ | +0x0C=renderableCount |
| MeshData | 296+ | +0x108=meshIndex |
| CWorld | 1796+ | +0x69C=currentStage |

---

**文档版本**: 1.0
**最后更新**: 2026-01-25
**作者**: AI Assistant (基于IDA Pro逆向分析)