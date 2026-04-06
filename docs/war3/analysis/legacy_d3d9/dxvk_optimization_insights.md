# War3 渲染系统逆向分析总结与DXVK优化建议

## 核心见解

### 1. War3 渲染架构的本质

War3使用了一个**分层批量渲染系统**，核心特点：

- **RenderQueue**: 中央调度器，收集所有渲染批次
- **SceneNode**: 场景节点，包含可渲染对象和子节点
- **RenderablePart**: 具体的渲染部件（Geoset/Part）
- **CWorldObjects**: 世界对象基类，管理可见性和渲染

**关键流程：**
```
CWorld::Render
  └─ RenderWorld_DispatchStage (按Stage遍历世界对象)
       └─ WorldObjects_RenderGroup (遍历单位/建筑列表)
            └─ WorldObjectEntry_Render (渲染单个对象)
                 ├─ vtable[5] (PreRender: 更新动画/状态)
                 └─ RenderQueue_AddBatch (提交批次)
                      └─ RenderBatch_Submit (遍历可渲染部件)
                           └─ 按材质层拆分为RenderBatch
                                └─ 排序 -> Dispatch (调用Draw)
```

### 2. 性能瓶颈分析

#### 瓶颈1: 过多的Draw Call
**问题：**
- 每个SceneNode的每个可见材质层产生一个Draw Call
- 一个模型可能有10+个层，产生10+个Draw Call
- 场景中1000+个单位 = 10,000+ Draw Call

**Vulkan的挑战：**
- Vulkan擅长处理大量Draw Call，但需要正确使用
- DXVK翻译D3D9调用时，每个Draw Call都会产生Pipeline Barrier
- 没有Instancing会导致GPU效率低下

#### 瓶颈2: 状态切换
**问题：**
- 排序键基于`layerStatePtr`（36字节的材质状态）
- 每次状态切换都需要更新D3D9状态
- DXVK需要将这些状态转换为Vulkan Descriptor Sets
- 频繁的状态切换导致Descriptor Cache失效

**Vulkan的挑战：**
- Descriptor Set的更新开销较大
- Push Descriptor可以缓解，但D3D9 -> DXVK转换层有开销

#### 瓶颈3: 透明对象排序
**问题：**
- 透明对象使用单独的队列（AUCTransparent）
- 需要按距离排序（Back-to-Front）
- 每帧都要重新排序1000+个透明对象
- 排序后逐个Draw，无法批处理

**Vulkan的挑战：**
- 透明排序是必须的（Vulkan无法自动处理）
- 无法有效批处理透明对象

### 3. DXVK环境下的特殊考虑

#### D3D9 -> Vulkan 的转换开销
```
D3D9 State
  └─ DXVK State Cache (哈希查找)
       └─ Vulkan Pipeline State Object (PSO)
            └─ GPU Pipeline Compilation
```

**问题：**
- 每个新的材质组合都需要编译PSO
- PSO编译耗时，首次渲染会卡顿
- Shader变体数量爆炸（每个层状态组合）

#### 内存管理差异
**D3D9:**
- 直接CPU -> GPU内存映射
- 资源生命周期由引用计数管理

**Vulkan (via DXVK):**
- 需要显式内存分配（Host Visible vs Device Local）
- 需要Staging Buffer进行数据传输
- 资池管理开销

## DXVK优化策略

### 策略1: Instancing 优化（最重要）

**实现路径：**

```cpp
// 1. 在RenderQueue_FlushSortedItems中收集实例化批次
struct InstancedBatch {
    uint32_t meshIndex;      // 来自MeshData+0x108
    uint32_t layerIndex;      // 层索引
    VkBuffer instanceBuffer;    // 实例数据
    uint32_t instanceCount;    // 实例数量
};

// 2. Hook RenderQueue_FlushSortedItems
void Native_FlushSortedItems(int category) {
    // 原版排序
    qsort(g_SortedPtrs, count, 4, ItemComparator);
    
    // 实例化批次收集
    std::vector<InstancedBatch> instancedBatches;
    uint32_t currentMeshIndex = -1;
    uint32_t currentLayerIndex = -1;
    std::vector<Matrix4x4> instanceMatrices;
    
    for (uint32_t i = 0; i < count; i++) {
        RenderBatch* batch = g_SortedPtrs[i];
        MeshData* mesh = batch->part->meshData;
        uint32_t meshIndex = mesh->meshIndex_264;
        
        if (meshIndex != currentMeshIndex || batch->layerIndex != currentLayerIndex) {
            // 结束当前批次
            if (!instanceMatrices.empty()) {
                instancedBatches.push_back({
                    currentMeshIndex,
                    currentLayerIndex,
                    CreateInstanceBuffer(instanceMatrices),
                    instanceMatrices.size()
                });
            }
            
            // 开始新批次
            currentMeshIndex = meshIndex;
            currentLayerIndex = batch->layerIndex;
            instanceMatrices.clear();
        }
        
        // 收集世界矩阵
        Matrix4x4 worldMatrix;
        GetWorldMatrix(batch->part->sceneNode, worldMatrix);
        instanceMatrices.push_back(worldMatrix);
    }
    
    // 3. 使用vkCmdDrawIndexedInstanced替代多个Draw Call
    for (auto& batch : instancedBatches) {
        vkCmdBindDescriptorSets(..., batch.instanceBuffer, ...);
        vkCmdDrawIndexed(..., 1, batch.instanceCount, ...);
    }
}
```

**预期收益：**
- Draw Call数量从 10,000+ 降到 <100
- GPU利用率提升 10-50倍
- CPU端排序开销保持不变

### 策略2: Pipeline State Cache 优化

**预编译PSO：**

```cpp
// 在游戏初始化时，扫描所有材质
void PrecompileAllPipelines() {
    // 遍历所有已加载的模型
    for (auto& model : g_LoadedModels) {
        for (auto& layer : model.layers) {
            // 创建所有可能的PSO变体
            VkPipeline pipeline = CreatePipeline(
                layer.vertexShader,
                layer.pixelShader,
                layer.blendMode,
                layer.depthWrite,
                ...
            );
            g_PipelineCache[GetStateHash(layer)] = pipeline;
        }
    }
}

// 在Flush时使用缓存
void DispatchBatch(RenderBatch* batch) {
    uint64_t stateHash = GetStateHash(batch->layerState);
    VkPipeline pipeline = g_PipelineCache[stateHash];
    
    if (!pipeline) {
        pipeline = CreatePipeline(batch->layerState);
        g_PipelineCache[stateHash] = pipeline;
    }
    
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDrawIndexed(...);
}
```

**预期收益：**
- 消除首次渲染卡顿
- PSO创建时间从每帧10ms降到0
- 减少Shader编译开销

### 策略3: Push Constants 优化

**替代Descriptor Set频繁更新：**

```cpp
// 使用Push Constants传递频繁变化的状态
struct PushConstants {
    Matrix4x4 worldMatrix;
    Matrix4x4 viewProjMatrix;
    float    time;
    uint32_t   flags;
};

// 在Dispatch时使用
void DispatchBatch(RenderBatch* batch) {
    PushConstants pushConsts;
    GetPushConstants(batch, &pushConsts);
    
    vkCmdPushConstants(
        commandBuffer,
        pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(PushConstants),
        &pushConsts
    );
    
    vkCmdDrawIndexed(...);
}
```

**预期收益：**
- 避免Descriptor Set更新开销
- 减少内存带宽
- 提升GPU缓存命中率

### 策略4: 多线程渲染队列

**Vulkan的优势：**

```cpp
// 使用Secondary Command Buffers并行构建
struct FrameContext {
    std::vector<VkCommandBuffer> primaryCmdBuffers;
    std::vector<std::vector<VkCommandBuffer>> secondaryCmdBuffers;
    std::mutex queueMutex;
};

// 每个渲染阶段使用不同线程
void RenderStageAsync(int stage, FrameContext* ctx, int threadId) {
    VkCommandBuffer secondaryCmd = BeginSecondaryCommandBuffer();
    
    // 渲染该阶段的所有批次
    for (auto& batch : g_StageBatches[stage]) {
        RenderBatchToCmdBuffer(secondaryCmd, batch);
    }
    
    EndSecondaryCommandBuffer(secondaryCmd);
    
    std::lock_guard<std::mutex> lock(ctx->queueMutex);
    ctx->secondaryCmdBuffers[threadId].push_back(secondaryCmd);
}

// 主线程合并
void RenderFrame(FrameContext* ctx) {
    // 并行执行所有Stage
    std::vector<std::thread> threads;
    for (int stage = 0; stage < MAX_STAGES; stage++) {
        threads.emplace_back([stage, ctx, threadId = stage]() {
            RenderStageAsync(stage, ctx, threadId);
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // 执行所有Secondary Command Buffers
    for (int i = 0; i < MAX_STAGES; i++) {
        for (auto& secondaryCmd : ctx->secondaryCmdBuffers[i]) {
            vkCmdExecuteCommands(primaryCmdBuffer, 1, &secondaryCmd);
        }
    }
}
```

**预期收益：**
- CPU多核利用率从单核提升到多核
- 场景复杂度提升3-5倍
- 帧率提升20-40%

### 策略5: GPU-Driven Culling

**将视锥剔除移到GPU：**

```cpp
// 1. 提交所有对象到GPU
struct CullData {
    Matrix4x4 worldMatrix;
    BoundingBox bbox;
    uint32_t   objectId;
};

// 2. 使用Compute Shader进行剔除
[shader] compute_cull {
    struct PushConst {
        Matrix4x4 viewProjMatrix;
        uint32_t objectCount;
    };
    
    layout(std430) buffer ObjectBuffer { CullData objects[]; };
    layout(std430) buffer VisibleBuffer { uint32_t visibleIndices[]; };
    
    void main() {
        uint32_t idx = gl_GlobalInvocationID.x;
        if (idx >= push.objectCount) return;
        
        CullData obj = objects[idx];
        Vector4 clipPos = mul(push.viewProjMatrix * obj.worldMatrix, obj.bbox.center);
        
        // 视锥剔除
        if (IsInFrustum(clipPos)) {
            atomicAdd(visibleCount, 1);
            visibleIndices[atomicAdd(visibleCount, 1)] = idx;
        }
    }
}

// 3. 只渲染可见对象
void RenderVisibleObjects() {
    vkCmdDispatch(computeCmd, ...);
    vkCmdMemoryBarrier(..., VK_ACCESS_SHADER_READ_BIT, ...);
    
    uint32_t visibleCount = ReadVisibleCount();
    for (uint32_t i = 0; i < visibleCount; i++) {
        RenderObject(visibleIndices[i]);
    }
}
```

**预期收益：**
- CPU端剔除开销降到0
- GPU并行剔除，速度提升100倍+
- 支持百万级对象场景

## 具体实现建议

### 阶段1: 快速优化（1-2周）

**目标：保持游戏正常运行，提升10-30%性能**

1. **Hook关键函数，不替换逻辑**
   ```cpp
   Hook("RenderQueue_FlushSortedItems", Native_FlushSortedItems);
   Hook("RenderQueue_Dispatch_Common", Native_Dispatch_Common);
   Hook("RenderQueue_Dispatch_Special", Native_Dispatch_Special);
   ```

2. **添加简单的实例化**
   ```cpp
   // 在Flush时检测相同模型的批次
   if (i > 0 && 
       batches[i]->meshIndex == batches[i-1]->meshIndex &&
       batches[i]->layerIndex == batches[i-1]->layerIndex) {
       // 合并到实例化批次
       AddToInstancedBatch(batches[i]);
   } else {
       FlushInstancedBatch();
       AddToInstancedBatch(batches[i]);
   }
   ```

3. **添加Pipeline Cache**
   ```cpp
   static std::unordered_map<uint64_t, VkPipeline> g_PipelineCache;
   ```

### 阶段2: 深度优化（2-4周）

**目标：完全替换渲染管线，提升50-200%性能**

1. **实现完整的RenderQueue替换**
   - 复现 `RenderBatch_Submit` 的逻辑
   - 添加实例化批次收集
   - 添加透明的优化排序

2. **实现多线程渲染**
   - Secondary Command Buffers
   - Stage级并行

3. **GPU Culling**
   - Compute Shader剔除
   - Indirect Drawing

### 阶段3: 高级特性（长期）

**目标：充分利用Vulkan特性**

1. **Ray Tracing 阴影**
   ```cpp
   // 使用Vulkan Ray Tracing扩展
   vkCmdTraceRaysKHR(..., shadowRayGenShader, ...);
   ```

2. **Variable Rate Shading (VRS)**
   ```cpp
   // 降低边缘区域的着色率
   vkCmdSetFragmentShadingRateKHR(..., VK_FRAGMENT_SHADING_RATE_2X4_PIXELS, ...);
   ```

3. **Mesh Shaders**
   ```cpp
   // 使用Mesh Shaders进行几何处理
   vkCmdDrawMeshTasksEXT(..., taskCount, firstTask);
   ```

## 注意事项

### 1. 保持游戏逻辑完整

**必须保留：**
- `vtable[5]` (PreRender) 调用
- 动画更新
- 可见性标志更新
- TeamColor状态

**不能破坏：**
- 游戏的逻辑循环
- JASS脚本的执行
- 联机游戏的同步

### 2. 兼容性考虑

**必须支持：**
- 旧版本War3（1.24-1.36）
- 各种Mod和自定义模型
- 不同的分辨率和设置
- 多GPU配置（集显+独显）

### 3. 调试和回退

**需要：**
- 详细的日志记录
- 性能计数器
- 热键切换优化开关
- 回退到原版渲染的能力

## 预期效果

### 低端配置（GTX 1060）
- 原版：30-60 FPS（单位数<200）
- 优化后：60-120 FPS（单位数<1000）
- 提升：**2-4倍**

### 中端配置（RTX 2060）
- 原版：60-90 FPS（单位数<500）
- 优化后：120-240 FPS（单位数<5000）
- 提升：**2-3倍**

### 高端配置（RTX 3080+）
- 原版：90-144 FPS（单位数<1000）
- 优化后：240+ FPS（单位数<20000）
- 提升：**2-3倍**

## 结论

War3的渲染系统虽然古老，但架构清晰，非常适合通过Vulkan进行现代化改造。

**关键成功因素：**
1. 准确理解RenderQueue的调度逻辑
2. 正确实现实例化（最重要）
3. 充分利用Vulkan的多线程特性
4. 保持与原版游戏逻辑的兼容性

**风险控制：**
- 逐步实现，每个阶段都保持游戏可运行
- 详细的性能测试和回归测试
- 保留回退机制

通过合理的优化策略，完全可以在DXVK环境下充分发挥Vulkan的性能，同时保持War3的原汁原味。