# Current Status

Date: 2026-05-03 (04:30)

## 1. Executive Summary

**动态阴影已实现连贯显示**：阴影现在可以随模型动画变化，不再是初始姿态。但仍存在刷新率低（0.5HZ）和只渲染地面单位的问题。

## 2. 当前状态

### 2.1 已实现功能

- ✅ 动态阴影姿态：阴影现在可以随模型动画变化
- ✅ 地面单位阴影：步兵、骑士等地面单位的阴影正确显示
- ✅ 阴影位置跟随：阴影位置跟着 caster 移动

### 2.2 待解决问题

- ❌ 刷新率低：阴影刷新率约 0.5HZ（每2秒刷新一次）
- ❌ 飞行单位阴影：飞行单位没有阴影
- ❌ 建筑阴影：建筑没有阴影
- ❌ 可破坏物阴影：可破坏物没有阴影
- ❌ 装饰物阴影：装饰物没有阴影

## 3. 技术实现

### 3.1 根本原因分析

**问题**：`CModel + 0x60` 只有 2-3 个根骨骼矩阵，而非完整骨架的 35+ 个矩阵。

**引擎实际流程**：
1. `CSpriteUber__PreRenderAndUpdatePosePalette` 调用 `CModel_SetWorldMatrixAndEvaluateRootPose`
2. `CModel_SetWorldMatrixAndEvaluateRootPose` 调用 `CModel_AllocAndFillGroupPalette`
3. `CModel_AllocAndFillGroupPalette` 为每个 RenderablePart 分配调色板槽位
4. `CGeosetData_BuildGroupBlendedPalette` 从全局矩阵池构建完整调色板
5. 完整调色板存储在 `dword_6FBC6BD0 + 48 * slotIndex`

### 3.2 解决方案

1. **直接使用引擎的调色板缓冲区**：从 `dword_6FBC6BD0 + 48 * slotIndex` 读取调色板
2. **添加调色板槽位索引缓存**：解决 `RenderablePart + 0x08` 在某些帧没有被更新的问题
3. **使用 `DecodeRuntimePoseMatrix48` 解析矩阵**：正确处理 3x4 矩阵格式

### 3.3 关键代码改动

**文件**：`src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`

1. 添加全局调色板缓冲区基址常量：
```cpp
constexpr uintptr_t kGlobalPaletteBufferRva = 0xBC6BD0;
```

2. 添加调色板槽位索引缓存：
```cpp
struct PaletteSlotCacheEntry {
  void* renderablePart = nullptr;
  uint32_t paletteSlotIndex = 0xFFFFFFFF;
  uint64_t lastUpdateFrame = 0;
};
static constexpr size_t kMaxPaletteSlotCacheEntries = 4096;
static thread_local PaletteSlotCacheEntry s_paletteSlotCache[kMaxPaletteSlotCacheEntries];
```

3. 修改 `tryEngineDirectPosePalette` 函数：
```cpp
auto tryEngineDirectPosePalette = [&]() -> bool {
    // 读取调色板槽位索引
    uint32_t paletteSlotIndex = 0xFFFFFFFF;
    if (renderable.renderablePart != nullptr) {
      dxvk::war3::SafeReadU32Fast(
          renderable.renderablePart,
          dxvk::war3::RenderablePartFieldOffsets::PaletteSlotIndex,
          paletteSlotIndex);
    }
    // 使用缓存机制
    paletteSlotIndex = FindOrUpdatePaletteSlotCache(
        renderable.renderablePart, paletteSlotIndex);
    // ... 读取调色板数据
};
```

## 4. IDA 逆向验证

### 4.1 关键函数验证

| 函数 | RVA | 验证状态 |
|---|---|---|
| `CGeosetData_BuildGroupBlendedPalette` | 0x6F12E600 | ✅ 正确 |
| `CMatrixGroup_BlendOutputMatrix` | 0x6F12E200 | ✅ 正确 |
| `CModel_AllocAndFillGroupPalette` | 0x6F12FED0 | ✅ 正确 |
| `CModel_CopyResolvedPoseMatricesToOutputPalette` | 0x6F12FDC0 | ✅ 正确 |
| `sub_6F138FF0` (分配调色板槽位) | 0x6F138FF0 | ✅ 正确 |
| `sub_6F139060` (获取调色板地址) | 0x6F139060 | ✅ 正确 |

### 4.2 关键数据结构验证

| 结构 | 偏移 | 验证状态 |
|---|---|---|
| `RenderablePart + 0x08` | PaletteSlotIndex | ✅ 正确 |
| `RenderablePart + 0x0C` | MeshData | ✅ 正确 |
| `RenderablePart + 0x10` | SkipFlag | ✅ 正确 |
| `MeshData + 0xF0` | TransformOrPoseCtx | ✅ 正确 |
| `CGeosetData + 0xF0` | MatrixGroupCount | ✅ 正确 |
| `CGeosetData + 0xF4` | MatrixGroupSizes | ✅ 正确 |
| `CGeosetData + 0x100` | MatrixIndices | ✅ 正确 |

### 4.3 关键全局变量验证

| 变量 | 地址 | 用途 |
|---|---|---|
| `dword_6FBC6BD0` | Game.dll + 0xBC6BD0 | 全局调色板缓冲区基址 |
| `dword_6FBDA4C8` | Game.dll + 0xBDA4C8 | 调色板槽位偏移 |
| `dword_6FBDA4CC` | Game.dll + 0xBDA4CC | 帧标识 |
| `dword_6FBC6BE4` | Game.dll + 0xBC6BE4 | 另一个偏移 |

## 5. 下一步计划

### 5.1 短期（1-2天）

1. **提高刷新率**：调查为什么阴影刷新率只有 0.5HZ
2. **添加飞行单位阴影**：调查飞行单位为什么没有阴影
3. **添加建筑阴影**：调查建筑为什么没有阴影

### 5.2 中期（3-5天）

1. **添加可破坏物阴影**：实现可破坏物的阴影渲染
2. **添加装饰物阴影**：实现装饰物的阴影渲染
3. **性能优化**：优化阴影渲染性能

### 5.3 长期（1-2周）

1. **GPU蒙皮**：将CPU蒙皮迁移到GPU
2. **完整渲染管线接管**：逐步接管魔兽争霸3的底层渲染

## 6. 更新日志

- 2026-05-03 04:30: 动态阴影已实现连贯显示，记录当前状态和下一步计划
